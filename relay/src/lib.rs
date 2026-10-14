//! Weft Studio collaboration relay. Milestone 0 only serves a health check;
//! the sync protocol from spec 05 §3 arrives in Milestone 10.

use axum::{routing::get, Json, Router};
use serde::Serialize;

pub const PROTOCOL_VERSION: u32 = 1;

pub fn app() -> Router {
    Router::new().route("/healthz", get(healthz))
}

#[derive(Serialize)]
struct Health {
    status: &'static str,
    protocol: u32,
    version: &'static str,
}

async fn healthz() -> Json<Health> {
    Json(Health {
        status: "ok",
        protocol: PROTOCOL_VERSION,
        version: env!("CARGO_PKG_VERSION"),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::Body;
    use axum::http::{Request, StatusCode};
    use http_body_util::BodyExt;
    use tower::ServiceExt;

    #[tokio::test]
    async fn healthz_reports_ok() {
        let res = app()
            .oneshot(Request::get("/healthz").body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(res.status(), StatusCode::OK);
        let body = res.into_body().collect().await.unwrap().to_bytes();
        let text = std::str::from_utf8(&body).unwrap();
        assert!(text.contains(r#""status":"ok""#), "{text}");
    }
}
