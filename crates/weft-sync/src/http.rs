//! Blocking HTTP calls to the relay's REST API (accounts, projects, invites).

use std::time::Duration;
use weft_crdt::codec::Value;

/// A JSON or binary request body.
pub enum Body {
    None,
    Json(serde_json::Value),
    Bytes(Vec<u8>),
}

pub struct Response {
    /// HTTP status, or 0 when the relay couldn't be reached.
    pub status: u16,
    pub json: Option<serde_json::Value>,
    pub bytes: Option<Vec<u8>>,
    /// Why the request failed: the relay's `error` text, or a network error.
    pub error: Option<String>,
    /// Response headers, names in lower case.
    pub headers: Vec<(String, String)>,
}

impl Response {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.as_str())
    }
}

impl Response {
    fn failed(msg: String) -> Response {
        Response {
            status: 0,
            json: None,
            bytes: None,
            error: Some(msg),
            headers: Vec::new(),
        }
    }
}

/// Plain http is only for a relay on this computer (05 §11).
pub fn check_scheme(url: &str) -> Result<(), String> {
    if url.starts_with("https://") || url.starts_with("wss://") {
        return Ok(());
    }
    let rest = url
        .strip_prefix("http://")
        .or_else(|| url.strip_prefix("ws://"))
        .ok_or_else(|| "The relay address must start with https:// or http://.".to_string())?;
    let authority = rest.split('/').next().unwrap_or("");
    let host = if let Some(v6) = authority.strip_prefix('[') {
        v6.split(']').next().unwrap_or("")
    } else {
        authority.rsplit_once(':').map_or(authority, |(h, _)| h)
    };
    let loopback = host == "localhost"
        || host
            .parse::<std::net::IpAddr>()
            .is_ok_and(|ip| ip.is_loopback());
    if loopback {
        Ok(())
    } else {
        Err("Use https:// for a relay that isn't on this computer.".into())
    }
}

fn agent(timeout: Duration) -> ureq::Agent {
    ureq::Agent::new_with_config(
        ureq::Agent::config_builder()
            .http_status_as_error(false)
            .timeout_global(Some(timeout))
            .build(),
    )
}

pub fn request(
    method: &str,
    url: &str,
    token: Option<&str>,
    body: Body,
    timeout: Duration,
) -> Response {
    request_with_headers(method, url, token, body, timeout, &[])
}

/// Like `request`, with extra request headers (such as `range`).
pub fn request_with_headers(
    method: &str,
    url: &str,
    token: Option<&str>,
    body: Body,
    timeout: Duration,
    headers: &[(String, String)],
) -> Response {
    if let Err(e) = check_scheme(url) {
        return Response::failed(e);
    }
    let mut builder = ureq::http::Request::builder().method(method).uri(url);
    if let Some(t) = token {
        builder = builder.header("authorization", format!("Bearer {t}"));
    }
    for (k, v) in headers {
        builder = builder.header(k.as_str(), v.as_str());
    }
    let payload = match body {
        Body::None => Vec::new(),
        Body::Json(v) => {
            builder = builder.header("content-type", "application/json");
            serde_json::to_vec(&v).unwrap_or_default()
        }
        Body::Bytes(b) => {
            builder = builder.header("content-type", "application/octet-stream");
            b
        }
    };
    let req = match builder.body(payload) {
        Ok(r) => r,
        Err(e) => return Response::failed(e.to_string()),
    };
    let resp = match agent(timeout).run(req) {
        Ok(r) => r,
        Err(e) => return Response::failed(format!("Can't reach the relay: {e}")),
    };
    let status = resp.status().as_u16();
    let response_headers: Vec<(String, String)> = resp
        .headers()
        .iter()
        .filter_map(|(k, v)| {
            Some((
                k.as_str().to_ascii_lowercase(),
                v.to_str().ok()?.to_string(),
            ))
        })
        .collect();
    let is_json = resp
        .headers()
        .get("content-type")
        .and_then(|v| v.to_str().ok())
        .is_some_and(|v| v.starts_with("application/json"));
    let data = match resp
        .into_body()
        .into_with_config()
        .limit(1 << 32)
        .read_to_vec()
    {
        Ok(d) => d,
        Err(e) => return Response::failed(format!("The relay's reply was cut off: {e}")),
    };
    let json = if is_json {
        serde_json::from_slice::<serde_json::Value>(&data).ok()
    } else {
        None
    };
    let error = if status >= 400 {
        Some(
            json.as_ref()
                .and_then(|j| j.get("error"))
                .and_then(|e| e.as_str())
                .map(str::to_string)
                .unwrap_or_else(|| format!("The relay answered {status}.")),
        )
    } else {
        None
    };
    Response {
        status,
        bytes: (!is_json).then_some(data),
        json,
        error,
        headers: response_headers,
    }
}

// JSON <-> codec values ------------------------------------------------------------

pub fn json_to_value(j: &serde_json::Value) -> Value {
    match j {
        serde_json::Value::Null => Value::Null,
        serde_json::Value::Bool(b) => Value::Bool(*b),
        serde_json::Value::Number(n) => match n.as_i64() {
            Some(i) => Value::Int(i),
            None => Value::F64(n.as_f64().unwrap_or(0.0)),
        },
        serde_json::Value::String(s) => Value::Str(s.clone()),
        serde_json::Value::Array(a) => Value::List(a.iter().map(json_to_value).collect()),
        serde_json::Value::Object(o) => {
            let mut items: Vec<(String, Value)> = o
                .iter()
                .map(|(k, v)| (k.clone(), json_to_value(v)))
                .collect();
            items.sort_by(|a, b| a.0.cmp(&b.0));
            Value::Map(items)
        }
    }
}

pub fn value_to_json(v: &Value) -> serde_json::Value {
    match v {
        Value::Null => serde_json::Value::Null,
        Value::Bool(b) => (*b).into(),
        Value::Int(i) => (*i).into(),
        Value::F64(f) => serde_json::Number::from_f64(*f)
            .map(serde_json::Value::Number)
            .unwrap_or(serde_json::Value::Null),
        Value::Str(s) => s.clone().into(),
        Value::Bytes(b) => b
            .iter()
            .map(|x| format!("{x:02x}"))
            .collect::<String>()
            .into(),
        Value::List(l) => serde_json::Value::Array(l.iter().map(value_to_json).collect()),
        Value::Map(m) => serde_json::Value::Object(
            m.iter()
                .map(|(k, v)| (k.clone(), value_to_json(v)))
                .collect(),
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_http_is_only_for_this_computer() {
        assert!(check_scheme("http://localhost:7878").is_ok());
        assert!(check_scheme("http://127.0.0.1:7878/v1").is_ok());
        assert!(check_scheme("ws://[::1]:7878/x").is_ok());
        assert!(check_scheme("https://relay.example.org").is_ok());
        assert!(check_scheme("http://relay.example.org").is_err());
        assert!(check_scheme("http://192.168.1.4:7878").is_err());
        assert!(check_scheme("ftp://x").is_err());
    }

    #[test]
    fn json_and_values_round_trip() {
        let j = serde_json::json!({"b": [1, 2.5, "x", null, true], "a": {"c": -3}});
        assert_eq!(value_to_json(&json_to_value(&j)), j);
    }
}
