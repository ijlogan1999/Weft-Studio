//! Client transport for Weft Studio's collaboration relay: blocking HTTP calls
//! for accounts, projects and invites, and the sync WebSocket (spec 05 §3).
//!
//! The C API (`include/weft_sync.h`) passes structured data as codec values
//! (see weft-crdt's `codec.rs`), which the C++ side reads as `doc::Value`.
//! The document stays on the C++ side; this crate only moves bytes.
//!
//! # Safety
//!
//! Pointer arguments must be null or valid for the stated length, and a
//! `WeftConn*` must come from `weft_conn_start` and not be used after
//! `weft_conn_free`.
#![allow(clippy::missing_safety_doc)]

pub mod conn;
pub mod http;

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::time::Duration;
use weft_crdt::codec::{decode_value, Value, Writer};
use weft_crdt::{WeftBytes, WeftStatus};
use weft_proto::{kind, Message, PresenceEntry};

pub use conn::{Config, Connection, Event, Tokens};

fn map(mut items: Vec<(&str, Value)>) -> Value {
    items.sort_by(|a, b| a.0.cmp(b.0));
    Value::Map(items.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
}

fn s(v: &str) -> Value {
    Value::Str(v.to_string())
}

fn get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    match v {
        Value::Map(items) => items.iter().find(|(k, _)| k == key).map(|(_, v)| v),
        _ => None,
    }
}

fn get_str(v: &Value, key: &str) -> String {
    match get(v, key) {
        Some(Value::Str(s)) => s.clone(),
        _ => String::new(),
    }
}

fn get_int(v: &Value, key: &str) -> i64 {
    match get(v, key) {
        Some(Value::Int(i)) => *i,
        Some(Value::F64(f)) => *f as i64,
        _ => 0,
    }
}

fn get_bool(v: &Value, key: &str, fallback: bool) -> bool {
    match get(v, key) {
        Some(Value::Bool(b)) => *b,
        _ => fallback,
    }
}

fn encode(v: &Value) -> Vec<u8> {
    let mut w = Writer::default();
    w.value(v);
    w.buf
}

fn member_value(m: &weft_proto::Member) -> Value {
    map(vec![
        ("userId", s(&m.user_id)),
        ("displayName", s(&m.display_name)),
        ("role", s(&m.role)),
        ("color", Value::Int(i64::from(m.color))),
    ])
}

fn presence_value(p: &PresenceEntry) -> Value {
    map(vec![
        ("clientId", s(&p.client_id)),
        ("userId", s(&p.user_id)),
        ("state", Value::Bytes(p.state.clone())),
    ])
}

fn tokens_value(t: &Tokens) -> Value {
    map(vec![
        ("accessToken", s(&t.access_token)),
        ("accessExpiresAt", Value::Int(t.access_expires_ms)),
        ("refreshToken", s(&t.refresh_token)),
    ])
}

fn tokens_from(v: &Value) -> Tokens {
    Tokens {
        access_token: get_str(v, "accessToken"),
        access_expires_ms: get_int(v, "accessExpiresAt"),
        refresh_token: get_str(v, "refreshToken"),
    }
}

/// The codec value for an event (see `weft_sync.h` for the shapes).
pub fn event_value(e: &Event) -> Value {
    match e {
        Event::Connecting => map(vec![("kind", s("connecting"))]),
        Event::Disconnected { reason, retry_in } => map(vec![
            ("kind", s("disconnected")),
            ("reason", s(reason)),
            (
                "retryInMs",
                retry_in.map_or(Value::Null, |d| Value::Int(d.as_millis() as i64)),
            ),
        ]),
        Event::TokensRefreshed(t) => {
            let mut v = tokens_value(t);
            if let Value::Map(items) = &mut v {
                items.push(("kind".into(), s("tokens")));
                items.sort_by(|a, b| a.0.cmp(&b.0));
            }
            v
        }
        Event::Message(m) => match m {
            Message::Welcome(w) => map(vec![
                ("kind", s("welcome")),
                ("role", s(&w.role)),
                ("userId", s(&w.user_id)),
                ("heads", Value::Bytes(w.heads.clone())),
                (
                    "members",
                    Value::List(w.members.iter().map(member_value).collect()),
                ),
                (
                    "presence",
                    Value::List(w.presence.iter().map(presence_value).collect()),
                ),
            ]),
            Message::Reject(r) => map(vec![
                ("kind", s("reject")),
                ("code", s(&r.code)),
                ("message", s(&r.message)),
            ]),
            Message::Sync(b) => map(vec![("kind", s("sync")), ("data", Value::Bytes(b.clone()))]),
            Message::Change(b) => map(vec![
                ("kind", s("change")),
                ("data", Value::Bytes(b.clone())),
            ]),
            Message::Ack(a) => map(vec![
                ("kind", s("ack")),
                ("hashes", Value::Bytes(a.hashes.clone())),
                ("heads", Value::Bytes(a.heads.clone())),
            ]),
            Message::Presence(p) => {
                let mut v = presence_value(p);
                if let Value::Map(items) = &mut v {
                    items.push(("kind".into(), s("presence")));
                    items.sort_by(|a, b| a.0.cmp(&b.0));
                }
                v
            }
            Message::Notice(n) => map(vec![
                ("kind", s("notice")),
                ("notice", s(&n.kind)),
                ("userId", n.user_id.as_deref().map_or(Value::Null, s)),
                ("clientId", n.client_id.as_deref().map_or(Value::Null, s)),
                ("role", n.role.as_deref().map_or(Value::Null, s)),
                ("message", n.message.as_deref().map_or(Value::Null, s)),
                (
                    "member",
                    n.member.as_ref().map_or(Value::Null, member_value),
                ),
                ("hashes", Value::Bytes(n.hashes.clone())),
            ]),
            Message::BlobAvailable(b) => map(vec![
                ("kind", s("blob")),
                ("hash", s(&b.hash)),
                ("size", Value::Int(b.size as i64)),
            ]),
            Message::Hello(_) | Message::Ping(_) | Message::Pong(_) => {
                map(vec![("kind", s("other"))])
            }
        },
    }
}

// C API -----------------------------------------------------------------------------

unsafe fn bytes_arg<'a>(p: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        Some(&[])
    } else if p.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(p, len))
    }
}

unsafe fn value_arg(p: *const u8, len: usize) -> Option<Value> {
    decode_value(bytes_arg(p, len)?).ok()
}

/// Makes one HTTP request. `request` is a codec map
/// `{method, url, token?, json?, bytes?, timeoutMs?}`; the response is
/// `{status, json?, bytes?, error?}` (status 0: the relay couldn't be reached).
#[no_mangle]
pub unsafe extern "C" fn weft_http(
    request: *const u8,
    len: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let Some(out) = out.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    let Some(req) = value_arg(request, len) else {
        return WeftStatus::InvalidArgument;
    };
    catch_unwind(AssertUnwindSafe(|| {
        let body = match (get(&req, "json"), get(&req, "bytes")) {
            (Some(j), _) if !matches!(j, Value::Null) => http::Body::Json(http::value_to_json(j)),
            (_, Some(Value::Bytes(b))) => http::Body::Bytes(b.clone()),
            _ => http::Body::None,
        };
        let token = get_str(&req, "token");
        let timeout = match get_int(&req, "timeoutMs") {
            t if t > 0 => Duration::from_millis(t as u64),
            _ => Duration::from_secs(30),
        };
        let r = http::request(
            &get_str(&req, "method"),
            &get_str(&req, "url"),
            (!token.is_empty()).then_some(token.as_str()),
            body,
            timeout,
        );
        let mut items = vec![("status", Value::Int(i64::from(r.status)))];
        if let Some(j) = &r.json {
            items.push(("json", http::json_to_value(j)));
        }
        if let Some(b) = r.bytes {
            items.push(("bytes", Value::Bytes(b)));
        }
        if let Some(e) = &r.error {
            items.push(("error", s(e)));
        }
        *out = WeftBytes::from_vec(encode(&map(items)));
        WeftStatus::Ok
    }))
    .unwrap_or(WeftStatus::Internal)
}

pub struct WeftConn(Connection);

/// Starts a sync connection. `config` is a codec map `{relayUrl, projectId, clientId,
/// clientVersion, schemaVersion, accessToken, accessExpiresAt, refreshToken,
/// oneWayDelayMs?, online?}`. Returns null for a bad config.
#[no_mangle]
pub unsafe extern "C" fn weft_conn_start(config: *const u8, len: usize) -> *mut WeftConn {
    let Some(c) = value_arg(config, len) else {
        return std::ptr::null_mut();
    };
    catch_unwind(AssertUnwindSafe(|| {
        let cfg = Config {
            relay_url: get_str(&c, "relayUrl").trim_end_matches('/').to_string(),
            project_id: get_str(&c, "projectId"),
            client_id: get_str(&c, "clientId"),
            client_version: get_str(&c, "clientVersion"),
            schema_version: get_int(&c, "schemaVersion").max(0) as u32,
            tokens: tokens_from(&c),
            one_way_delay: Duration::from_millis(get_int(&c, "oneWayDelayMs").max(0) as u64),
            start_online: get_bool(&c, "online", true),
        };
        if cfg.relay_url.is_empty() || cfg.project_id.is_empty() {
            return std::ptr::null_mut();
        }
        Box::into_raw(Box::new(WeftConn(Connection::start(cfg))))
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// Queues a frame: `kind` 4 (SYNC) or 5 (CHANGE) with raw Automerge bytes, or 7
/// (PRESENCE) with the presence state.
#[no_mangle]
pub unsafe extern "C" fn weft_conn_send(
    conn: *mut WeftConn,
    kind_: u8,
    data: *const u8,
    len: usize,
) -> WeftStatus {
    let (Some(conn), Some(bytes)) = (conn.as_ref(), bytes_arg(data, len)) else {
        return WeftStatus::InvalidArgument;
    };
    let m = match kind_ {
        kind::SYNC => Message::Sync(bytes.to_vec()),
        kind::CHANGE => Message::Change(bytes.to_vec()),
        kind::PRESENCE => Message::Presence(PresenceEntry {
            client_id: String::new(),
            user_id: String::new(),
            state: bytes.to_vec(),
        }),
        _ => return WeftStatus::InvalidArgument,
    };
    conn.0.send(&m);
    WeftStatus::Ok
}

#[no_mangle]
pub unsafe extern "C" fn weft_conn_set_online(conn: *mut WeftConn, online: i32) {
    if let Some(conn) = conn.as_ref() {
        conn.0.set_online(online != 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn weft_conn_set_heads(conn: *mut WeftConn, heads: *const u8, len: usize) {
    if let (Some(conn), Some(h)) = (conn.as_ref(), bytes_arg(heads, len)) {
        conn.0.set_heads(h.to_vec());
    }
}

/// New tokens after the user signs in again: codec map `{accessToken, accessExpiresAt, refreshToken}`.
#[no_mangle]
pub unsafe extern "C" fn weft_conn_set_tokens(
    conn: *mut WeftConn,
    tokens: *const u8,
    len: usize,
) -> WeftStatus {
    let (Some(conn), Some(v)) = (conn.as_ref(), value_arg(tokens, len)) else {
        return WeftStatus::InvalidArgument;
    };
    conn.0.set_tokens(tokens_from(&v));
    WeftStatus::Ok
}

/// Writes the next due event and returns 1, or returns 0 when there is none.
#[no_mangle]
pub unsafe extern "C" fn weft_conn_poll(conn: *mut WeftConn, out: *mut WeftBytes) -> i32 {
    let (Some(conn), Some(out)) = (conn.as_ref(), out.as_mut()) else {
        return 0;
    };
    *out = WeftBytes::empty();
    match conn.0.poll() {
        Some(e) => {
            *out = WeftBytes::from_vec(encode(&event_value(&e)));
            1
        }
        None => 0,
    }
}

/// Closes the connection and joins its thread.
#[no_mangle]
pub unsafe extern "C" fn weft_conn_free(conn: *mut WeftConn) {
    if !conn.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(conn))));
    }
}
