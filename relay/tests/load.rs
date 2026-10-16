//! Relay capacity (spec 05 §3.5): 50 rooms with 8 clients each, every client editing about
//! once a second, on a relay with two worker threads (the 2-vCPU target). Every change must
//! reach the other 7 clients in its room, with a p95 fan-out time within 150 ms.
//!
//! It needs a release build to say anything about capacity, so it is ignored by default:
//! `cargo test --release -p weft-relay --test load -- --ignored --nocapture`
//! (CI runs it that way).

use automerge::sync::SyncDoc;
use automerge::transaction::Transactable;
use automerge::{AutoCommit, ObjType, ReadDoc, ROOT};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::time::{Duration, Instant};
use weft_proto::Message;
use weft_relay::{Config, RunningRelay};
use weft_sync::http::{request, Body};
use weft_sync::{Connection, Event, Tokens};

const ROOMS: usize = 50;
const CLIENTS: usize = 8;
const SECONDS: u64 = 20;
const EDIT_EVERY: Duration = Duration::from_millis(1000);
const P95_LIMIT: Duration = Duration::from_millis(150);

fn call(relay: &str, method: &str, path: &str, token: Option<&str>, body: Value) -> (u16, Value) {
    let b = if body.is_null() {
        Body::None
    } else {
        Body::Json(body)
    };
    let r = request(
        method,
        &format!("{relay}{path}"),
        token,
        b,
        Duration::from_secs(30),
    );
    (r.status, r.json.unwrap_or(Value::Null))
}

fn sign_up(relay: &str, n: usize) -> Tokens {
    let (status, j) = call(
        relay,
        "POST",
        "/v1/auth/signup",
        None,
        json!({ "email": format!("user{n}@example.org"), "password": "password123", "displayName": format!("User {n}") }),
    );
    assert_eq!(status, 201, "{j}");
    Tokens {
        access_token: j["accessToken"].as_str().unwrap().to_string(),
        access_expires_ms: j["accessExpiresAt"].as_i64().unwrap(),
        refresh_token: j["refreshToken"].as_str().unwrap().to_string(),
    }
}

struct Client {
    conn: Connection,
    doc: AutoCommit,
    state: automerge::sync::State,
    joined: bool,
    next_edit: Instant,
    edits: usize,
    failures: Vec<String>,
}

impl Client {
    fn connect(
        relay: &str,
        project: &str,
        tokens: &Tokens,
        doc: AutoCommit,
        first_edit: Instant,
    ) -> Client {
        Client {
            conn: Connection::start(weft_sync::Config {
                relay_url: relay.to_string(),
                project_id: project.to_string(),
                client_id: weft_relay::auth::random_id("c_"),
                client_version: "load".into(),
                schema_version: 1,
                tokens: tokens.clone(),
                one_way_delay: Duration::ZERO,
                start_online: true,
            }),
            doc,
            state: automerge::sync::State::new(),
            joined: false,
            next_edit: first_edit,
            edits: 0,
            failures: Vec::new(),
        }
    }

    fn send_sync(&mut self) {
        if let Some(m) = self.doc.sync().generate_sync_message(&mut self.state) {
            self.conn.send(&Message::Sync(m.encode()));
        }
    }

    /// Handles what arrived; returns the hashes of the changes that came in as CHANGE frames.
    fn pump(&mut self, arrived: &mut Vec<[u8; 32]>) {
        while let Some(e) = self.conn.poll() {
            match e {
                Event::Message(Message::Welcome(_)) => {
                    self.joined = true;
                    self.state = automerge::sync::State::new();
                    self.send_sync();
                }
                Event::Message(Message::Sync(b)) => {
                    let m = automerge::sync::Message::decode(&b).unwrap();
                    self.doc
                        .sync()
                        .receive_sync_message(&mut self.state, m)
                        .unwrap();
                    self.send_sync();
                }
                Event::Message(Message::Change(b)) => {
                    let change = automerge::Change::from_bytes(b.clone()).unwrap();
                    arrived.push(change.hash().0);
                    self.doc.load_incremental(&b).unwrap();
                }
                Event::Message(Message::Reject(r)) => {
                    self.failures.push(format!("rejected: {}", r.code))
                }
                Event::Disconnected { .. } => self.failures.push("disconnected".into()),
                _ => {}
            }
        }
    }

    fn edit(&mut self, key: &str) -> [u8; 32] {
        let regions = self.doc.get(ROOT, "regions").unwrap().unwrap().1;
        let r = self.doc.put_object(&regions, key, ObjType::Map).unwrap();
        self.doc.put(&r, "start", self.edits as i64 * 960).unwrap();
        self.doc.put(&r, "gainDb", -3.0).unwrap();
        self.doc.commit();
        self.edits += 1;
        let c = self.doc.get_last_local_change().unwrap().clone();
        self.conn.send(&Message::Change(c.raw_bytes().to_vec()));
        c.hash().0
    }
}

fn project_doc() -> AutoCommit {
    let mut d = AutoCommit::new();
    d.put(ROOT, "schemaVersion", 1).unwrap();
    d.put_object(ROOT, "regions", ObjType::Map).unwrap();
    d.commit();
    d
}

#[test]
#[ignore = "capacity test: run in release with --ignored"]
fn relay_carries_50_rooms_of_8_clients_on_2_workers() {
    let relay = RunningRelay::start(Config::default()); // 2 worker threads
    let url = relay.url();
    let users: Vec<Tokens> = (0..CLIENTS).map(|n| sign_up(&url, n)).collect();

    // The first user owns every project and invites the others as Editors.
    for room in 0..ROOMS {
        let project = format!("load{room}");
        let (status, j) = call(
            &url,
            "POST",
            "/v1/projects",
            Some(&users[0].access_token),
            json!({ "projectId": project, "name": "Load" }),
        );
        assert_eq!(status, 201, "{j}");
        for user in &users[1..] {
            let (status, j) = call(
                &url,
                "POST",
                &format!("/v1/projects/{project}/invites"),
                Some(&users[0].access_token),
                json!({ "role": "editor" }),
            );
            assert_eq!(status, 201, "{j}");
            let (status, j) = call(
                &url,
                "POST",
                &format!("/v1/invites/{}/accept", j["token"].as_str().unwrap()),
                Some(&user.access_token),
                Value::Null,
            );
            assert_eq!(status, 200, "{j}");
        }
    }

    // Everyone joins; edits start staggered over the first second.
    let start = Instant::now() + Duration::from_secs(600); // set once everyone has synced
    let mut clients: Vec<Vec<Client>> = (0..ROOMS)
        .map(|room| {
            (0..CLIENTS)
                .map(|n| {
                    let doc = if n == 0 {
                        project_doc()
                    } else {
                        AutoCommit::new()
                    };
                    Client::connect(&url, &format!("load{room}"), &users[n], doc, start)
                })
                .collect()
        })
        .collect();
    let mut arrived = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let mut all = true;
        for room in clients.iter_mut() {
            for c in room.iter_mut() {
                c.pump(&mut arrived);
                all &= c.joined && c.doc.get(ROOT, "regions").ok().flatten().is_some();
            }
        }
        if all {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "not every client joined and synced in 60 s"
        );
        std::thread::sleep(Duration::from_millis(2));
    }
    let joined_after = deadline - Duration::from_secs(60);
    println!(
        "{} clients joined in {:.1} s",
        ROOMS * CLIENTS,
        joined_after.elapsed().as_secs_f64()
    );

    let begin = Instant::now();
    for (room, cs) in clients.iter_mut().enumerate() {
        for (n, c) in cs.iter_mut().enumerate() {
            c.next_edit =
                begin + EDIT_EVERY * (room * CLIENTS + n) as u32 / (ROOMS * CLIENTS) as u32;
        }
    }

    // Each change: when it was sent, and how many of the room's other clients have it.
    struct Sent {
        at: Instant,
        room: usize,
        received: usize,
        last: Duration,
    }
    let mut sent: HashMap<[u8; 32], Sent> = HashMap::new();
    let mut fanout: Vec<Duration> = Vec::new();
    let end = begin + Duration::from_secs(SECONDS);
    let mut drained_by = end + Duration::from_secs(10);
    loop {
        let now = Instant::now();
        for (room, cs) in clients.iter_mut().enumerate() {
            for (n, c) in cs.iter_mut().enumerate() {
                if now < end && now >= c.next_edit {
                    let hash = c.edit(&format!("r{n}-{}", c.edits));
                    sent.insert(
                        hash,
                        Sent {
                            at: Instant::now(),
                            room,
                            received: 0,
                            last: Duration::ZERO,
                        },
                    );
                    c.next_edit += EDIT_EVERY;
                }
                arrived.clear();
                c.pump(&mut arrived);
                for h in &arrived {
                    let s = sent.get_mut(h).expect("a change nobody sent");
                    assert_eq!(s.room, room, "a change reached another room");
                    s.received += 1;
                    s.last = s.at.elapsed();
                    if s.received == CLIENTS - 1 {
                        fanout.push(s.last);
                    }
                }
            }
        }
        let now = Instant::now();
        if now >= end && fanout.len() == sent.len() {
            break;
        }
        if now >= drained_by {
            break;
        }
        if now < end {
            drained_by = end + Duration::from_secs(10);
        }
        std::thread::sleep(Duration::from_millis(1));
    }

    let failures: Vec<String> = clients
        .iter()
        .flatten()
        .flat_map(|c| c.failures.iter().cloned())
        .collect();
    assert!(failures.is_empty(), "{failures:?}");
    let missing = sent.values().filter(|s| s.received < CLIENTS - 1).count();
    assert_eq!(
        missing,
        0,
        "{missing} of {} changes didn't reach the whole room",
        sent.len()
    );

    fanout.sort();
    let p = |q: f64| fanout[((fanout.len() as f64 * q) as usize).min(fanout.len() - 1)];
    println!(
        "{} changes in {} s, each to 7 clients: p50 {:?}, p95 {:?}, p99 {:?}, max {:?}",
        sent.len(),
        SECONDS,
        p(0.5),
        p(0.95),
        p(0.99),
        fanout.last().unwrap()
    );
    assert!(sent.len() >= ROOMS * CLIENTS * (SECONDS as usize - 1));
    assert!(p(0.95) <= P95_LIMIT, "p95 {:?} over {P95_LIMIT:?}", p(0.95));

    // Every client ends with every change of its room.
    for cs in &mut clients {
        let heads = cs[0].doc.get_heads();
        for c in cs.iter_mut() {
            assert_eq!(c.doc.get_heads(), heads);
        }
    }
}
