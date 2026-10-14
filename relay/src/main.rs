use std::net::SocketAddr;

#[tokio::main]
async fn main() {
    // Localhost by default; hosting is an open decision (ARCHITECTURE.md §8).
    let addr: SocketAddr = std::env::var("WEFT_RELAY_ADDR")
        .unwrap_or_else(|_| "127.0.0.1:7878".to_string())
        .parse()
        .expect("WEFT_RELAY_ADDR must be host:port");
    let listener = tokio::net::TcpListener::bind(addr)
        .await
        .expect("bind relay address");
    println!("weft-relay listening on http://{addr}");
    axum::serve(listener, weft_relay::app())
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await
        .expect("relay server error");
}
