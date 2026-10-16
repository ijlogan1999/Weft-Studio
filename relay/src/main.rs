use std::net::SocketAddr;
use std::path::PathBuf;

const USAGE: &str = "usage: weft-relay [--addr host:port] [--data <dir>] [--public] [--port-file <file>] [--test-clock]

  --addr        Address to listen on (default 127.0.0.1:7878, or WEFT_RELAY_ADDR).
  --data        Folder for the database and signing key (default ./weft-relay-data).
  --public      Allow a non-loopback address. Put a TLS proxy in front: clients only
                talk plain HTTP to localhost.
  --port-file   Write the bound port to this file once listening (for tests).
  --test-clock  Let tests move the relay's clock (POST /v1/test/clock).";

fn main() {
    let mut addr: SocketAddr = std::env::var("WEFT_RELAY_ADDR")
        .unwrap_or_else(|_| "127.0.0.1:7878".to_string())
        .parse()
        .expect("WEFT_RELAY_ADDR must be host:port");
    let mut config = weft_relay::Config {
        data_dir: Some(PathBuf::from("weft-relay-data")),
        test_clock: false,
    };
    let mut public = false;
    let mut port_file: Option<PathBuf> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--addr" => {
                addr = args
                    .next()
                    .and_then(|v| v.parse().ok())
                    .unwrap_or_else(|| usage())
            }
            "--data" => {
                config.data_dir = Some(args.next().map(PathBuf::from).unwrap_or_else(|| usage()))
            }
            "--port-file" => {
                port_file = Some(args.next().map(PathBuf::from).unwrap_or_else(|| usage()))
            }
            "--public" => public = true,
            "--test-clock" => config.test_clock = true,
            _ => usage(),
        }
    }
    if !addr.ip().is_loopback() && !public {
        eprintln!("weft-relay: {addr} isn't a loopback address. Pass --public and put a TLS proxy in front.");
        std::process::exit(2);
    }

    let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
    rt.block_on(async move {
        let router = weft_relay::build(&config).unwrap_or_else(|e| {
            eprintln!("weft-relay: {e}");
            std::process::exit(1);
        });
        let listener = tokio::net::TcpListener::bind(addr)
            .await
            .unwrap_or_else(|e| {
                eprintln!("weft-relay: can't listen on {addr}: {e}");
                std::process::exit(1);
            });
        let bound = listener.local_addr().expect("bound address");
        if let Some(f) = port_file {
            let tmp = f.with_extension("tmp");
            std::fs::write(&tmp, bound.port().to_string()).expect("write port file");
            std::fs::rename(&tmp, &f).expect("write port file");
        }
        println!("weft-relay listening on http://{bound}");
        weft_relay::serve(listener, router, async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await
        .expect("relay server error");
    });
}

fn usage() -> ! {
    eprintln!("{USAGE}");
    std::process::exit(2);
}
