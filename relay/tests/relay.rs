//! Relay acceptance tests at the protocol level: accounts and tokens (AT-COL-3),
//! roles, invites and removal (AT-COL-4, relay side), durability across a relay
//! restart (AT-COL-9, relay side), and join refusals. Clients are the real
//! weft-sync transport with an Automerge document each.

use automerge::sync::SyncDoc;
use automerge::transaction::Transactable;
use automerge::{AutoCommit, ObjType, ReadDoc, ROOT};
use serde_json::{json, Value};
use std::time::{Duration, Instant};
use weft_proto::{notice, reject, Message};
use weft_relay::{Config, RunningRelay};
use weft_sync::http::{request, Body};
use weft_sync::{Connection, Event, Tokens};

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
        Duration::from_secs(10),
    );
    (r.status, r.json.unwrap_or(Value::Null))
}

struct Account {
    id: String,
    tokens: Tokens,
}

fn account_from(j: &Value) -> Account {
    Account {
        id: j["userId"].as_str().unwrap().to_string(),
        tokens: Tokens {
            access_token: j["accessToken"].as_str().unwrap().to_string(),
            access_expires_ms: j["accessExpiresAt"].as_i64().unwrap(),
            refresh_token: j["refreshToken"].as_str().unwrap().to_string(),
        },
    }
}

fn sign_up(relay: &str, email: &str, name: &str) -> Account {
    let (status, j) = call(
        relay,
        "POST",
        "/v1/auth/signup",
        None,
        json!({ "email": email, "password": "password123", "displayName": name }),
    );
    assert_eq!(status, 201, "{j}");
    account_from(&j)
}

/// A client: the weft-sync connection plus an Automerge replica, driven by `pump`.
struct Client {
    conn: Connection,
    doc: AutoCommit,
    state: automerge::sync::State,
    role: Option<String>,
    acked: Vec<[u8; 32]>,
    server_heads: Vec<u8>,
    notices: Vec<weft_proto::Notice>,
    rejected: Option<String>,
    disconnects: usize,
    refreshed: bool,
}

impl Client {
    fn connect(relay: &str, project: &str, account: &Account, doc: AutoCommit) -> Client {
        Self::connect_with(relay, project, account.tokens.clone(), doc, 1)
    }

    fn connect_with(
        relay: &str,
        project: &str,
        tokens: Tokens,
        doc: AutoCommit,
        schema: u32,
    ) -> Client {
        let conn = Connection::start(weft_sync::Config {
            relay_url: relay.to_string(),
            project_id: project.to_string(),
            client_id: weft_relay::auth::random_id("c_"),
            client_version: "test".into(),
            schema_version: schema,
            tokens,
            one_way_delay: Duration::ZERO,
            start_online: true,
        });
        Client {
            conn,
            doc,
            state: automerge::sync::State::new(),
            role: None,
            acked: Vec::new(),
            server_heads: Vec::new(),
            notices: Vec::new(),
            rejected: None,
            disconnects: 0,
            refreshed: false,
        }
    }

    fn send_sync(&mut self) {
        if let Some(m) = self.doc.sync().generate_sync_message(&mut self.state) {
            self.conn.send(&Message::Sync(m.encode()));
        }
    }

    fn pump(&mut self) {
        while let Some(e) = self.conn.poll() {
            match e {
                Event::Message(Message::Welcome(w)) => {
                    self.role = Some(w.role);
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
                    self.doc.load_incremental(&b).unwrap();
                }
                Event::Message(Message::Ack(a)) => {
                    for h in a.hashes.chunks_exact(32) {
                        self.acked.push(h.try_into().unwrap());
                    }
                    self.server_heads = a.heads;
                }
                Event::Message(Message::Notice(n)) => self.notices.push(n),
                Event::Message(Message::Reject(r)) => self.rejected = Some(r.code),
                Event::Disconnected { .. } => self.disconnects += 1,
                Event::TokensRefreshed(_) => self.refreshed = true,
                _ => {}
            }
        }
    }

    /// Commits a local edit and sends it as a CHANGE.
    fn edit(&mut self, f: impl FnOnce(&mut AutoCommit)) -> [u8; 32] {
        f(&mut self.doc);
        self.doc.commit();
        let c = self.doc.get_last_local_change().unwrap().clone();
        self.conn.send(&Message::Change(c.raw_bytes().to_vec()));
        c.hash().0
    }

    fn synced(&mut self) -> bool {
        self.role.is_some()
            && weft_crdt::ops::heads_bytes(&self.doc.get_heads()) == self.server_heads
    }
}

fn wait_until(
    clients: &mut [&mut Client],
    what: &str,
    mut cond: impl FnMut(&mut [&mut Client]) -> bool,
) {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        for c in clients.iter_mut() {
            c.pump();
        }
        if cond(clients) {
            return;
        }
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(5));
    }
}

/// A project document like the client's: a regions map and collab.comments / chat.
fn project_doc() -> AutoCommit {
    let mut d = AutoCommit::new();
    d.put(ROOT, "schemaVersion", 1).unwrap();
    d.put_object(ROOT, "regions", ObjType::Map).unwrap();
    let collab = d.put_object(ROOT, "collab", ObjType::Map).unwrap();
    d.put_object(&collab, "comments", ObjType::Map).unwrap();
    d.put_object(&collab, "chat", ObjType::List).unwrap();
    d.commit();
    d
}

fn obj(d: &AutoCommit, path: &[&str]) -> automerge::ObjId {
    let mut o = ROOT;
    for p in path {
        o = d.get(&o, *p).unwrap().unwrap().1;
    }
    o
}

/// Does the map at `path` have `key` yet?
fn has(d: &AutoCommit, path: &[&str], key: &str) -> bool {
    let mut o = ROOT;
    for p in path {
        match d.get(&o, *p) {
            Ok(Some((_, id))) => o = id,
            _ => return false,
        }
    }
    matches!(d.get(&o, key), Ok(Some(_)))
}

fn share(relay: &str, owner: &Account, id: &str) {
    let (status, j) = call(
        relay,
        "POST",
        "/v1/projects",
        Some(&owner.tokens.access_token),
        json!({ "projectId": id, "name": "Song" }),
    );
    assert_eq!(status, 201, "{j}");
}

fn invite(relay: &str, from: &Account, project: &str, role: &str, to: &Account) {
    let (status, j) = call(
        relay,
        "POST",
        &format!("/v1/projects/{project}/invites"),
        Some(&from.tokens.access_token),
        json!({ "role": role }),
    );
    assert_eq!(status, 201, "{j}");
    let url = j["url"].as_str().unwrap().to_string();
    assert!(url.starts_with(relay), "{url}");
    let token = j["token"].as_str().unwrap();
    let (status, j) = call(
        relay,
        "POST",
        &format!("/v1/invites/{token}/accept"),
        Some(&to.tokens.access_token),
        Value::Null,
    );
    assert_eq!(status, 200, "{j}");
    assert_eq!(j["role"], role);
}

#[test]
fn healthz_reports_ok() {
    let relay = RunningRelay::start(Config::default());
    let (status, j) = call(&relay.url(), "GET", "/healthz", None, Value::Null);
    assert_eq!(status, 200);
    assert_eq!(j["status"], "ok");
}

#[test]
fn at_col_3_sign_up_sign_in_rate_limit_and_token_refresh() {
    let relay = RunningRelay::start(Config {
        data_dir: None,
        test_clock: true,
    });
    let url = relay.url();
    let a = sign_up(&url, "Ana@Example.org", "Ana");
    // The same email again, in another case, is taken.
    let (status, _) = call(
        &url,
        "POST",
        "/v1/auth/signup",
        None,
        json!({ "email": "ana@example.org", "password": "password123", "displayName": "X" }),
    );
    assert_eq!(status, 409);
    let (status, _) = call(
        &url,
        "POST",
        "/v1/auth/signup",
        None,
        json!({ "email": "b@example.org", "password": "short", "displayName": "B" }),
    );
    assert_eq!(status, 400);

    let (status, j) = call(
        &url,
        "POST",
        "/v1/auth/signin",
        None,
        json!({ "email": "ana@example.org", "password": "password123" }),
    );
    assert_eq!(status, 200);
    assert_eq!(j["userId"], a.id.as_str());
    let (status, j) = call(
        &url,
        "GET",
        "/v1/me",
        Some(j["accessToken"].as_str().unwrap()),
        Value::Null,
    );
    assert_eq!((status, j["displayName"].as_str()), (200, Some("Ana")));

    // Wrong password: rejected; the 6th failed attempt within a minute is rate-limited,
    // even with the right password, until the minute has passed.
    for _ in 0..5 {
        let (status, _) = call(
            &url,
            "POST",
            "/v1/auth/signin",
            None,
            json!({ "email": "ana@example.org", "password": "wrong-password" }),
        );
        assert_eq!(status, 401);
    }
    let (status, j) = call(
        &url,
        "POST",
        "/v1/auth/signin",
        None,
        json!({ "email": "ana@example.org", "password": "password123" }),
    );
    assert_eq!(status, 429, "{j}");
    call(
        &url,
        "POST",
        "/v1/test/clock",
        None,
        json!({ "advanceMs": 61_000 }),
    );
    let (status, _) = call(
        &url,
        "POST",
        "/v1/auth/signin",
        None,
        json!({ "email": "ana@example.org", "password": "password123" }),
    );
    assert_eq!(status, 200);

    // The access token lasts 15 minutes.
    let (_, j) = call(
        &url,
        "POST",
        "/v1/auth/signin",
        None,
        json!({ "email": "ana@example.org", "password": "password123" }),
    );
    let fresh = account_from(&j);
    call(
        &url,
        "POST",
        "/v1/test/clock",
        None,
        json!({ "advanceMs": 14 * 60_000 }),
    );
    assert_eq!(
        call(
            &url,
            "GET",
            "/v1/me",
            Some(&fresh.tokens.access_token),
            Value::Null
        )
        .0,
        200
    );
    call(
        &url,
        "POST",
        "/v1/test/clock",
        None,
        json!({ "advanceMs": 61_000 }),
    );
    assert_eq!(
        call(
            &url,
            "GET",
            "/v1/me",
            Some(&fresh.tokens.access_token),
            Value::Null
        )
        .0,
        401
    );

    // Refreshed transparently: the sync client's expired token is rejected at HELLO, the
    // client trades its refresh token for a new one and joins without the user noticing.
    share(&url, &fresh_token(&url, &fresh), "p1");
    let mut c = Client::connect(&url, "p1", &fresh, project_doc());
    wait_until(&mut [&mut c], "a welcome after the refresh", |c| {
        c[0].role.is_some()
    });
    assert!(c.refreshed);
    assert_eq!(c.role.as_deref(), Some("owner"));

    // A bad refresh token gets 401.
    let (status, _) = call(
        &url,
        "POST",
        "/v1/auth/refresh",
        None,
        json!({ "refreshToken": "nope" }),
    );
    assert_eq!(status, 401);
}

/// Signs in again by refresh token (the first one has expired in the test above).
fn fresh_token(url: &str, a: &Account) -> Account {
    let (status, j) = call(
        url,
        "POST",
        "/v1/auth/refresh",
        None,
        json!({ "refreshToken": a.tokens.refresh_token }),
    );
    assert_eq!(status, 200, "{j}");
    account_from(&j)
}

#[test]
fn at_col_4_roles_invites_viewer_comments_and_removal() {
    let relay = RunningRelay::start(Config::default());
    let url = relay.url();
    let owner = sign_up(&url, "owner@example.org", "Owner");
    let b = sign_up(&url, "b@example.org", "Bea");
    let c = sign_up(&url, "c@example.org", "Cy");
    share(&url, &owner, "song");

    // The Owner uploads the local project.
    let mut o = Client::connect(&url, "song", &owner, project_doc());
    wait_until(&mut [&mut o], "the owner's upload", |c| {
        c[0].synced() && !c[0].server_heads.is_empty()
    });

    // An Editor invite lets B join and edit.
    invite(&url, &owner, "song", "editor", &b);
    let (status, _) = call(
        &url,
        "GET",
        "/v1/projects/song/document",
        Some(&c.tokens.access_token),
        Value::Null,
    );
    assert_eq!(status, 404, "a non-member can't read the document");
    let doc = request(
        "GET",
        &format!("{url}/v1/projects/song/document"),
        Some(&b.tokens.access_token),
        Body::None,
        Duration::from_secs(5),
    );
    assert_eq!(doc.status, 200);
    let mut bc = Client::connect(
        &url,
        "song",
        &b,
        AutoCommit::load(&doc.bytes.unwrap()).unwrap(),
    );
    wait_until(&mut [&mut o, &mut bc], "B to join", |c| c[1].synced());
    assert_eq!(bc.role.as_deref(), Some("editor"));
    let regions = obj(&bc.doc, &["regions"]);
    let h = bc.edit(|d| {
        d.put(&regions, "r1", 480).unwrap();
    });
    wait_until(&mut [&mut o, &mut bc], "B's region on the owner", |c| {
        has(&c[0].doc, &["regions"], "r1")
    });
    assert!(bc.acked.contains(&h));

    // Viewer C can read, comment and chat; a region change is refused with `forbidden`.
    invite(&url, &bc_account(&b), "song", "viewer", &c);
    let (status, _) = call(
        &url,
        "POST",
        "/v1/projects/song/invites",
        Some(&c.tokens.access_token),
        json!({ "role": "viewer" }),
    );
    assert_eq!(status, 403, "viewers can't invite");
    let mut cc = Client::connect(&url, "song", &c, AutoCommit::new());
    wait_until(
        &mut [&mut o, &mut bc, &mut cc],
        "C to read the project",
        |c| c[2].synced() && has(&c[2].doc, &["regions"], "r1"),
    );
    assert_eq!(cc.role.as_deref(), Some("viewer"));
    let comments = obj(&cc.doc, &["collab", "comments"]);
    let chat = obj(&cc.doc, &["collab", "chat"]);
    let ok = cc.edit(|d| {
        d.put(&comments, "k1", "Nice take").unwrap();
        d.insert(&chat, 0, "hello").unwrap();
    });
    wait_until(
        &mut [&mut o, &mut bc, &mut cc],
        "C's comment on the owner",
        |c| c[2].acked.contains(&ok),
    );
    wait_until(
        &mut [&mut o, &mut bc, &mut cc],
        "the comment to fan out",
        |c| has(&c[0].doc, &["collab", "comments"], "k1"),
    );
    let regions_c = obj(&cc.doc, &["regions"]);
    let bad = cc.edit(|d| {
        d.put(&regions_c, "r1", 960).unwrap();
    });
    wait_until(
        &mut [&mut o, &mut bc, &mut cc],
        "the forbidden notice",
        |c| c[2].notices.iter().any(|n| n.kind == notice::FORBIDDEN),
    );
    let n = cc
        .notices
        .iter()
        .find(|n| n.kind == notice::FORBIDDEN)
        .unwrap();
    assert_eq!(n.hashes, bad.to_vec());
    assert!(!cc.acked.contains(&bad));
    std::thread::sleep(Duration::from_millis(100));
    o.pump();
    let r1 = o
        .doc
        .get(obj(&o.doc, &["regions"]), "r1")
        .unwrap()
        .unwrap()
        .0;
    assert_eq!(
        r1.as_i64(),
        Some(480),
        "the owner never sees the viewer's region change"
    );

    // The Owner removes B: B's socket closes with a `removed` notice.
    let (status, _) = call(
        &url,
        "DELETE",
        &format!("/v1/projects/song/members/{}", b.id),
        Some(&owner.tokens.access_token),
        Value::Null,
    );
    assert_eq!(status, 204);
    wait_until(&mut [&mut o, &mut bc, &mut cc], "B's removal", |c| {
        c[1].notices.iter().any(|n| n.kind == notice::REMOVED) && c[1].disconnects > 0
    });
    assert!(o.notices.iter().any(|n| n.kind == notice::MEMBER_REMOVED));
    // B can't come back.
    let mut again = Client::connect(&url, "song", &b, AutoCommit::new());
    wait_until(&mut [&mut again], "B's rejection", |c| {
        c[0].rejected.is_some()
    });
    assert_eq!(again.rejected.as_deref(), Some(reject::FORBIDDEN));
    let (status, j) = call(
        &url,
        "GET",
        "/v1/projects/song/members",
        Some(&owner.tokens.access_token),
        Value::Null,
    );
    assert_eq!(status, 200);
    let names: Vec<&str> = j
        .as_array()
        .unwrap()
        .iter()
        .map(|m| m["displayName"].as_str().unwrap())
        .collect();
    assert_eq!(names, ["Owner", "Cy"]);
}

fn bc_account(a: &Account) -> Account {
    Account {
        id: a.id.clone(),
        tokens: a.tokens.clone(),
    }
}

#[test]
fn at_col_9_acked_changes_survive_a_relay_restart() {
    let dir = std::env::temp_dir().join(weft_relay::auth::random_id("weft-relay-test-"));
    let config = Config {
        data_dir: Some(dir.clone()),
        test_clock: false,
    };
    let relay = RunningRelay::start(config.clone());
    let addr = relay.addr;
    let url = relay.url();
    let owner = sign_up(&url, "o@example.org", "Owner");
    share(&url, &owner, "p");
    let mut o = Client::connect(&url, "p", &owner, project_doc());
    wait_until(&mut [&mut o], "the upload", |c| c[0].synced());
    let regions = obj(&o.doc, &["regions"]);
    let mut hashes = Vec::new();
    for i in 0..20 {
        hashes.push(o.edit(|d| {
            d.put(&regions, format!("r{i}"), i).unwrap();
        }));
    }
    wait_until(&mut [&mut o], "every ACK", |c| {
        hashes.iter().all(|h| c[0].acked.contains(h))
    });
    let expected = o.doc.get_heads();
    drop(relay);

    // The client notices, keeps editing offline and reconnects when the relay is back.
    wait_until(&mut [&mut o], "the disconnect", |c| c[0].disconnects > 0);
    let late = o.edit(|d| {
        d.put(&regions, "late", 1).unwrap();
    });
    let relay = RunningRelay::start_at(config, addr);
    let fresh = request(
        "GET",
        &format!("{url}/v1/projects/p/document"),
        Some(&owner.tokens.access_token),
        Body::None,
        Duration::from_secs(5),
    );
    let restored = AutoCommit::load(&fresh.bytes.unwrap()).unwrap();
    for h in &expected {
        assert!(
            restored.get_change_by_hash(h).is_some(),
            "an ACKed change was lost"
        );
    }
    let deadline = Instant::now() + Duration::from_secs(40);
    loop {
        o.pump();
        if o.synced() && o.server_heads == weft_crdt::ops::heads_bytes(&o.doc.get_heads()) {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "the client didn't resync after the restart"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
    let doc = request(
        "GET",
        &format!("{}/v1/projects/p/document", relay.url()),
        Some(&owner.tokens.access_token),
        Body::None,
        Duration::from_secs(5),
    );
    let after = AutoCommit::load(&doc.bytes.unwrap()).unwrap();
    assert!(
        after
            .get_change_by_hash(&automerge::ChangeHash(late))
            .is_some(),
        "the un-ACKed change was re-sent"
    );
    drop(relay);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn joins_are_refused_for_newer_schemas_strangers_and_missing_projects() {
    let relay = RunningRelay::start(Config::default());
    let url = relay.url();
    let owner = sign_up(&url, "o@example.org", "Owner");
    let stranger = sign_up(&url, "s@example.org", "Stranger");
    share(&url, &owner, "p");
    let mut doc = project_doc();
    doc.put(ROOT, "schemaVersion", 2).unwrap();
    doc.commit();
    let mut o = Client::connect_with(&url, "p", owner.tokens.clone(), doc, 2);
    wait_until(&mut [&mut o], "the upload", |c| c[0].synced());

    let mut old = Client::connect_with(&url, "p", owner.tokens.clone(), AutoCommit::new(), 1);
    wait_until(&mut [&mut old], "update_required", |c| {
        c[0].rejected.is_some()
    });
    assert_eq!(old.rejected.as_deref(), Some(reject::UPDATE_REQUIRED));

    let mut s = Client::connect(&url, "p", &stranger, AutoCommit::new());
    wait_until(&mut [&mut s], "forbidden", |c| c[0].rejected.is_some());
    assert_eq!(s.rejected.as_deref(), Some(reject::FORBIDDEN));

    let mut missing = Client::connect(&url, "nope", &owner, AutoCommit::new());
    wait_until(&mut [&mut missing], "not_found", |c| {
        c[0].rejected.is_some()
    });
    assert_eq!(missing.rejected.as_deref(), Some(reject::NOT_FOUND));

    let mut bad = Client::connect_with(
        &url,
        "p",
        Tokens {
            access_token: "x".into(),
            ..Default::default()
        },
        AutoCommit::new(),
        1,
    );
    wait_until(&mut [&mut bad], "auth", |c| c[0].rejected.is_some());
    assert_eq!(bad.rejected.as_deref(), Some(reject::AUTH));
}
