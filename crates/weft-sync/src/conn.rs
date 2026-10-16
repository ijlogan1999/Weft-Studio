//! The sync connection (spec 05 §3.3): one thread per open shared project that
//! keeps a WebSocket to the relay, reconnecting with backoff (1, 2, 4 … 30 s,
//! §13), refreshing the access token when it's about to expire, and sending a
//! PING every 15 s. The document itself stays with the caller: this thread
//! only moves frames, and hands everything it receives to `poll`.

use std::collections::VecDeque;
use std::io::ErrorKind;
use std::net::TcpStream;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use tungstenite::protocol::WebSocket;
use tungstenite::stream::MaybeTlsStream;
use weft_proto::{reject, Hello, Message};

use crate::http;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tokens {
    pub access_token: String,
    /// When the access token expires, in Unix ms (by the relay's clock).
    pub access_expires_ms: i64,
    pub refresh_token: String,
}

#[derive(Clone, Debug)]
pub struct Config {
    /// `http(s)://host[:port]`, no trailing slash.
    pub relay_url: String,
    pub project_id: String,
    pub client_id: String,
    pub client_version: String,
    pub schema_version: u32,
    pub tokens: Tokens,
    /// Test hook: delay added to each direction (half the round trip).
    pub one_way_delay: Duration,
    pub start_online: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    Connecting,
    Message(Message),
    /// The socket closed; `retry_in` is None when the client won't retry on its own.
    Disconnected {
        reason: String,
        retry_in: Option<Duration>,
    },
    /// The access token was refreshed; the caller should save it.
    TokensRefreshed(Tokens),
}

struct Inner {
    outgoing: VecDeque<(Instant, Vec<u8>)>,
    events: VecDeque<(Instant, Event)>,
    online: bool,
    stop: bool,
    heads: Vec<u8>,
    tokens: Tokens,
    /// Bumped when the caller supplies new tokens, so a rejected client retries.
    token_generation: u64,
}

struct Shared {
    inner: Mutex<Inner>,
    wake: Condvar,
}

pub struct Connection {
    shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
    delay: Duration,
}

/// Refresh the access token when less than this is left.
const REFRESH_MARGIN_MS: i64 = 60_000;
const READ_POLL: Duration = Duration::from_millis(5);
const MAX_BACKOFF: Duration = Duration::from_secs(30);

pub fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

impl Connection {
    pub fn start(config: Config) -> Connection {
        // tungstenite's rustls connector uses the process-wide crypto provider.
        let _ = rustls::crypto::ring::default_provider().install_default();
        let shared = Arc::new(Shared {
            inner: Mutex::new(Inner {
                outgoing: VecDeque::new(),
                events: VecDeque::new(),
                online: config.start_online,
                stop: false,
                heads: Vec::new(),
                tokens: config.tokens.clone(),
                token_generation: 0,
            }),
            wake: Condvar::new(),
        });
        let delay = config.one_way_delay;
        let s = shared.clone();
        let thread = std::thread::Builder::new()
            .name("weft-sync".into())
            .spawn(move || Worker { shared: s, config }.run())
            .expect("spawn sync thread");
        Connection {
            shared,
            thread: Some(thread),
            delay,
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.shared.inner.lock().unwrap()
    }

    /// Queues a frame; frames queued while offline are dropped (the caller re-syncs on reconnect).
    pub fn send(&self, m: &Message) {
        let mut g = self.lock();
        if g.online {
            g.outgoing
                .push_back((Instant::now() + self.delay, m.encode()));
        }
    }

    pub fn set_online(&self, online: bool) {
        let mut g = self.lock();
        if g.online != online {
            g.online = online;
            g.outgoing.clear();
        }
        self.shared.wake.notify_all();
    }

    pub fn set_heads(&self, heads: Vec<u8>) {
        self.lock().heads = heads;
    }

    pub fn set_tokens(&self, tokens: Tokens) {
        let mut g = self.lock();
        g.tokens = tokens;
        g.token_generation += 1;
        self.shared.wake.notify_all();
    }

    /// The next event that is due, if any.
    pub fn poll(&self) -> Option<Event> {
        let mut g = self.lock();
        match g.events.front() {
            Some((due, _)) if *due <= Instant::now() => g.events.pop_front().map(|(_, e)| e),
            _ => None,
        }
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        self.lock().stop = true;
        self.shared.wake.notify_all();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

enum Ended {
    Stop,
    Offline,
    /// The relay refused us; retry only after new tokens (auth) or never (others).
    Rejected(String),
    Closed(String),
}

struct Worker {
    shared: Arc<Shared>,
    config: Config,
}

type Socket = WebSocket<MaybeTlsStream<TcpStream>>;

impl Worker {
    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.shared.inner.lock().unwrap()
    }

    fn emit(&self, e: Event, delayed: bool) {
        let due = Instant::now()
            + if delayed {
                self.config.one_way_delay
            } else {
                Duration::ZERO
            };
        self.lock().events.push_back((due, e));
    }

    /// Waits up to `d` or until woken; returns false if stopping.
    fn wait(&self, d: Duration, until: impl Fn(&Inner) -> bool) -> bool {
        let deadline = Instant::now() + d;
        let mut g = self.lock();
        loop {
            if g.stop {
                return false;
            }
            if until(&g) {
                return true;
            }
            let now = Instant::now();
            if now >= deadline {
                return true;
            }
            g = self.shared.wake.wait_timeout(g, deadline - now).unwrap().0;
        }
    }

    fn run(self) {
        let mut backoff = Duration::from_secs(1);
        loop {
            let (stop, online) = {
                let g = self.lock();
                (g.stop, g.online)
            };
            if stop {
                return;
            }
            if !online {
                if !self.wait(Duration::from_secs(3600), |g| g.online) {
                    return;
                }
                continue;
            }
            if let Err(reason) = self.ensure_fresh_token() {
                self.emit(
                    Event::Message(Message::Reject(weft_proto::Reject {
                        code: reject::AUTH.into(),
                        message: reason,
                    })),
                    false,
                );
                if !self.wait_for_new_tokens() {
                    return;
                }
                continue;
            }
            // Frames from a previous session belong to its sync exchange; the caller
            // starts a fresh one after WELCOME.
            self.lock().outgoing.clear();
            self.emit(Event::Connecting, false);
            let (ended, welcomed) = match self.connect() {
                Ok(ws) => self.session(ws),
                Err(e) => (Ended::Closed(e), false),
            };
            if welcomed {
                backoff = Duration::from_secs(1);
            }
            match ended {
                Ended::Stop => return,
                Ended::Offline => {
                    self.emit(
                        Event::Disconnected {
                            reason: "offline".into(),
                            retry_in: None,
                        },
                        false,
                    );
                }
                Ended::Rejected(code) => {
                    self.emit(
                        Event::Disconnected {
                            reason: code.clone(),
                            retry_in: None,
                        },
                        true,
                    );
                    if code == reject::AUTH {
                        // The access token may have expired between our check and HELLO.
                        let refreshed = self.refresh_tokens().is_ok();
                        if !refreshed && !self.wait_for_new_tokens() {
                            return;
                        }
                    } else if !self.wait(Duration::from_secs(24 * 3600), |g| !g.online) {
                        // Not found, forbidden, update required: wait until the caller
                        // goes offline (and back), or stops us.
                        return;
                    }
                }
                Ended::Closed(reason) => {
                    self.emit(
                        Event::Disconnected {
                            reason,
                            retry_in: Some(backoff),
                        },
                        true,
                    );
                    if !self.wait(backoff, |g| !g.online) {
                        return;
                    }
                    backoff = (backoff * 2).min(MAX_BACKOFF);
                }
            }
        }
    }

    fn wait_for_new_tokens(&self) -> bool {
        let generation = self.lock().token_generation;
        self.wait(Duration::from_secs(24 * 3600), move |g| {
            g.token_generation != generation || !g.online
        })
    }

    fn ensure_fresh_token(&self) -> Result<(), String> {
        let expires = self.lock().tokens.access_expires_ms;
        if expires == 0 || expires - now_ms() > REFRESH_MARGIN_MS {
            return Ok(());
        }
        self.refresh_tokens()
    }

    /// Trades the refresh token for a new access token (05 §3.1), transparently.
    fn refresh_tokens(&self) -> Result<(), String> {
        let refresh = self.lock().tokens.refresh_token.clone();
        if refresh.is_empty() {
            return Err("Sign in again.".into());
        }
        let resp = http::request(
            "POST",
            &format!("{}/v1/auth/refresh", self.config.relay_url),
            None,
            http::Body::Json(serde_json::json!({ "refreshToken": refresh })),
            Duration::from_secs(15),
        );
        let json = match (resp.status, resp.json) {
            (200, Some(j)) => j,
            (_, _) => return Err(resp.error.unwrap_or_else(|| "Sign in again.".into())),
        };
        let tokens = Tokens {
            access_token: json["accessToken"].as_str().unwrap_or_default().to_string(),
            access_expires_ms: json["accessExpiresAt"].as_i64().unwrap_or(0),
            refresh_token: json["refreshToken"]
                .as_str()
                .unwrap_or(&refresh)
                .to_string(),
        };
        self.lock().tokens = tokens.clone();
        self.emit(Event::TokensRefreshed(tokens), false);
        Ok(())
    }

    fn connect(&self) -> Result<Socket, String> {
        let base = &self.config.relay_url;
        http::check_scheme(base)?;
        let ws_url = if let Some(rest) = base.strip_prefix("https://") {
            format!("wss://{rest}")
        } else if let Some(rest) = base.strip_prefix("http://") {
            format!("ws://{rest}")
        } else {
            base.clone()
        };
        let url = format!("{ws_url}/v1/projects/{}/ws", self.config.project_id);
        let authority = ws_url
            .split("://")
            .nth(1)
            .and_then(|r| r.split('/').next())
            .ok_or("bad relay address")?
            .to_string();
        let default_port = if ws_url.starts_with("wss://") {
            443
        } else {
            80
        };
        let host_port = if authority
            .rsplit_once(':')
            .is_some_and(|(_, p)| p.parse::<u16>().is_ok())
            && !authority.ends_with(']')
        {
            authority.clone()
        } else {
            format!("{authority}:{default_port}")
        };
        use std::net::ToSocketAddrs;
        let addr = host_port
            .to_socket_addrs()
            .map_err(|e| format!("Can't find the relay: {e}"))?
            .next()
            .ok_or("Can't find the relay.")?;
        let tcp = TcpStream::connect_timeout(&addr, Duration::from_secs(10))
            .map_err(|e| format!("Can't reach the relay: {e}"))?;
        let _ = tcp.set_nodelay(true);
        tcp.set_read_timeout(Some(Duration::from_secs(10)))
            .map_err(|e| e.to_string())?;
        let config = tungstenite::protocol::WebSocketConfig::default()
            .max_message_size(Some(256 << 20))
            .max_frame_size(Some(256 << 20));
        let (ws, _) = tungstenite::client_tls_with_config(url.as_str(), tcp, Some(config), None)
            .map_err(|e| format!("Can't open the sync connection: {e}"))?;
        let tcp_ref = match ws.get_ref() {
            MaybeTlsStream::Plain(s) => s,
            MaybeTlsStream::Rustls(s) => s.get_ref(),
            _ => return Err("unsupported stream".into()),
        };
        tcp_ref
            .set_read_timeout(Some(READ_POLL))
            .map_err(|e| e.to_string())?;
        Ok(ws)
    }

    fn write(ws: &mut Socket, frame: Vec<u8>) -> Result<(), String> {
        ws.send(tungstenite::Message::Binary(frame.into()))
            .map_err(|e| format!("The connection dropped: {e}"))
    }

    /// Runs one connected session; returns why it ended and whether it was welcomed.
    fn session(&self, mut ws: Socket) -> (Ended, bool) {
        let hello = {
            let g = self.lock();
            Message::Hello(Hello {
                token: g.tokens.access_token.clone(),
                project_id: self.config.project_id.clone(),
                client_id: self.config.client_id.clone(),
                client_version: self.config.client_version.clone(),
                schema_version: self.config.schema_version,
                heads: g.heads.clone(),
            })
        };
        if let Err(e) = Self::write(&mut ws, hello.encode()) {
            return (Ended::Closed(e), false);
        }
        let mut welcomed = false;
        let mut last_inbound = Instant::now();
        let mut last_ping = Instant::now();
        let ping_every = Duration::from_millis(weft_proto::PING_INTERVAL_MS);
        let idle = Duration::from_millis(weft_proto::IDLE_TIMEOUT_MS);
        loop {
            // Outgoing frames that are due, and the stop and offline switches.
            let (stop, online, due) = {
                let mut g = self.lock();
                let now = Instant::now();
                let mut due = Vec::new();
                while g.outgoing.front().is_some_and(|(t, _)| *t <= now) {
                    due.push(g.outgoing.pop_front().unwrap().1);
                }
                (g.stop, g.online, due)
            };
            if stop || !online {
                let _ = ws.close(None);
                let _ = ws.flush();
                return (if stop { Ended::Stop } else { Ended::Offline }, welcomed);
            }
            if welcomed {
                for f in due {
                    if let Err(e) = Self::write(&mut ws, f) {
                        return (Ended::Closed(e), welcomed);
                    }
                }
                if last_ping.elapsed() >= ping_every {
                    last_ping = Instant::now();
                    if let Err(e) = Self::write(&mut ws, Message::Ping(now_ms()).encode()) {
                        return (Ended::Closed(e), welcomed);
                    }
                }
            } else if !due.is_empty() {
                // Frames queued before WELCOME go out after it, in order.
                let mut g = self.lock();
                for f in due.into_iter().rev() {
                    g.outgoing.push_front((Instant::now(), f));
                }
            }
            if last_inbound.elapsed() >= idle {
                return (
                    Ended::Closed("The relay stopped answering.".into()),
                    welcomed,
                );
            }
            match ws.read() {
                Ok(tungstenite::Message::Binary(b)) => {
                    last_inbound = Instant::now();
                    let Ok(m) = Message::decode(&b) else { continue };
                    match &m {
                        Message::Welcome(_) => welcomed = true,
                        Message::Reject(r) => {
                            let code = r.code.clone();
                            self.emit(Event::Message(m), true);
                            return (Ended::Rejected(code), welcomed);
                        }
                        Message::Pong(_) => continue,
                        _ => {}
                    }
                    self.emit(Event::Message(m), true);
                }
                Ok(tungstenite::Message::Close(_)) => {
                    return (
                        Ended::Closed("The relay closed the connection.".into()),
                        welcomed,
                    )
                }
                Ok(_) => last_inbound = Instant::now(),
                Err(tungstenite::Error::Io(e))
                    if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                Err(tungstenite::Error::ConnectionClosed)
                | Err(tungstenite::Error::AlreadyClosed) => {
                    return (
                        Ended::Closed("The relay closed the connection.".into()),
                        welcomed,
                    )
                }
                Err(e) => {
                    return (
                        Ended::Closed(format!("The connection dropped: {e}")),
                        welcomed,
                    )
                }
            }
        }
    }
}
