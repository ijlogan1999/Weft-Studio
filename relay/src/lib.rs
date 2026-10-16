//! Weft Studio collaboration relay (spec 05 §3): accounts, projects and roles,
//! invites, the per-project change log, and rooms that sync documents and fan
//! out changes and presence over WebSockets.
//!
//! The relay speaks plain HTTP. On anything but a loopback address it expects a
//! TLS-terminating proxy in front of it (05 §11, D-ARCH-38).

pub mod auth;
pub mod room;
pub mod store;

use automerge::sync::SyncDoc;
use axum::extract::ws::{Message as WsMessage, WebSocket, WebSocketUpgrade};
use axum::extract::{ConnectInfo, Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{delete, get, post};
use axum::{Json, Router};
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use serde_json::{json, Value as Json_};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::mpsc::unbounded_channel;

use auth::{Clock, RateLimiter, Tokens};
use room::{Client, Outbound, Room};
use store::Store;
use weft_proto::{notice, reject, Ack, Member, Message, Notice, Reject, Welcome};

pub const PROTOCOL_VERSION: u32 = weft_proto::PROTOCOL_VERSION;
pub const INVITE_DEFAULT_DAYS: i64 = 7;
const DAY_MS: i64 = 24 * 60 * 60 * 1000;

#[derive(Default, Clone)]
pub struct Config {
    /// Where the database and signing key live; None keeps everything in memory.
    pub data_dir: Option<PathBuf>,
    /// Enables `POST /v1/test/clock`, so tests can expire tokens and invites.
    pub test_clock: bool,
}

pub struct AppState {
    store: Mutex<Store>,
    tokens: Tokens,
    clock: Clock,
    limiter: RateLimiter,
    rooms: Mutex<HashMap<String, Arc<Mutex<Room>>>>,
    next_conn: AtomicU64,
    test_clock: bool,
}

/// Builds the relay's state and routes.
pub fn build(config: &Config) -> Result<Router, String> {
    let (store, secret) = match &config.data_dir {
        Some(dir) => {
            std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            let key_file = dir.join("signing.key");
            let secret = match std::fs::read(&key_file) {
                Ok(k) if k.len() >= 32 => k,
                _ => {
                    let k = auth::random_token(48).into_bytes();
                    std::fs::write(&key_file, &k)
                        .map_err(|e| format!("{}: {e}", key_file.display()))?;
                    k
                }
            };
            let store = Store::open(&dir.join("relay.sqlite")).map_err(|e| e.to_string())?;
            (store, secret)
        }
        None => (
            Store::in_memory().map_err(|e| e.to_string())?,
            auth::random_token(48).into_bytes(),
        ),
    };
    let state = Arc::new(AppState {
        store: Mutex::new(store),
        tokens: Tokens::new(secret),
        clock: Clock::default(),
        limiter: RateLimiter::default(),
        rooms: Mutex::new(HashMap::new()),
        next_conn: AtomicU64::new(1),
        test_clock: config.test_clock,
    });
    Ok(Router::new()
        .route("/healthz", get(healthz))
        .route("/v1/auth/signup", post(sign_up))
        .route("/v1/auth/signin", post(sign_in))
        .route("/v1/auth/refresh", post(refresh))
        .route("/v1/me", get(me))
        .route("/v1/projects", get(list_projects).post(create_project))
        .route("/v1/projects/{id}", get(get_project).delete(delete_project))
        .route("/v1/projects/{id}/document", get(get_document))
        .route("/v1/projects/{id}/members", get(list_members))
        .route(
            "/v1/projects/{id}/members/{user}",
            delete(remove_member).patch(change_role),
        )
        .route("/v1/projects/{id}/invites", post(create_invite))
        .route("/v1/invites/{token}", get(get_invite))
        .route("/v1/invites/{token}/accept", post(accept_invite))
        .route("/v1/projects/{id}/ws", get(ws_upgrade))
        .route("/v1/test/clock", post(advance_clock))
        .with_state(state))
}

/// Serves until `shutdown` resolves.
pub async fn serve(
    listener: tokio::net::TcpListener,
    router: Router,
    shutdown: impl std::future::Future<Output = ()> + Send + 'static,
) -> std::io::Result<()> {
    axum::serve(
        listener,
        router.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown)
    .await
}

/// A relay on its own runtime thread, for tests. Dropping it stops the server.
pub struct RunningRelay {
    pub addr: SocketAddr,
    stop: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl RunningRelay {
    pub fn start(config: Config) -> RunningRelay {
        Self::start_at(config, "127.0.0.1:0".parse().unwrap())
    }

    pub fn start_at(config: Config, addr: SocketAddr) -> RunningRelay {
        let (addr_tx, addr_rx) = std::sync::mpsc::channel();
        let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
        let thread = std::thread::spawn(move || {
            let rt = tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .unwrap();
            rt.block_on(async move {
                let router = build(&config).expect("relay config");
                let listener = tokio::net::TcpListener::bind(addr).await.expect("bind");
                addr_tx.send(listener.local_addr().unwrap()).unwrap();
                let _ = serve(listener, router, async {
                    let _ = stop_rx.await;
                })
                .await;
            });
            // Open WebSocket tasks end with the runtime.
            rt.shutdown_timeout(Duration::from_millis(500));
        });
        RunningRelay {
            addr: addr_rx.recv().expect("relay started"),
            stop: Some(stop_tx),
            thread: Some(thread),
        }
    }

    pub fn url(&self) -> String {
        format!("http://{}", self.addr)
    }
}

impl Drop for RunningRelay {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

// Errors ---------------------------------------------------------------------

pub struct ApiError(StatusCode, String);

impl ApiError {
    fn new(code: StatusCode, msg: impl Into<String>) -> ApiError {
        ApiError(code, msg.into())
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.0, Json(json!({ "error": self.1 }))).into_response()
    }
}

impl From<rusqlite::Error> for ApiError {
    fn from(e: rusqlite::Error) -> Self {
        ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
    }
}

type ApiResult<T> = Result<T, ApiError>;

fn unauthorized() -> ApiError {
    ApiError::new(StatusCode::UNAUTHORIZED, "Sign in again.")
}

fn forbidden(msg: &str) -> ApiError {
    ApiError::new(StatusCode::FORBIDDEN, msg)
}

fn not_found() -> ApiError {
    ApiError::new(StatusCode::NOT_FOUND, "Project not found.")
}

// Helpers ----------------------------------------------------------------------

impl AppState {
    fn now(&self) -> i64 {
        self.clock.now_ms()
    }

    fn user_from_headers(&self, headers: &HeaderMap) -> ApiResult<String> {
        let token = headers
            .get("authorization")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.strip_prefix("Bearer "))
            .ok_or_else(unauthorized)?;
        self.tokens
            .verify(token, self.now())
            .ok_or_else(unauthorized)
    }

    /// The caller's role in the project; 404 for a project that doesn't exist or that
    /// they aren't a member of (05 §11: membership is checked on every request).
    fn role(&self, project_id: &str, user_id: &str) -> ApiResult<String> {
        let store = self.store.lock().unwrap();
        store.role_of(project_id, user_id)?.ok_or_else(not_found)
    }

    fn session_json(&self, user: &store::User, refresh_token: Option<String>) -> ApiResult<Json_> {
        let now = self.now();
        let (access, exp) = self.tokens.issue(&user.id, now);
        let refresh = match refresh_token {
            Some(r) => r,
            None => {
                let r = auth::random_token(32);
                self.store.lock().unwrap().add_refresh_token(
                    &auth::token_hash(&r),
                    &user.id,
                    now + auth::REFRESH_TOKEN_MS,
                )?;
                r
            }
        };
        Ok(json!({
            "userId": user.id,
            "email": user.email,
            "displayName": user.display_name,
            "accessToken": access,
            "accessExpiresAt": exp,
            "refreshToken": refresh,
        }))
    }

    /// The room for a project, loading it from the log if nobody has it open.
    fn room(&self, project_id: &str) -> Result<Arc<Mutex<Room>>, String> {
        let mut rooms = self.rooms.lock().unwrap();
        if let Some(r) = rooms.get(project_id) {
            return Ok(r.clone());
        }
        let room = Room::load(&self.store.lock().unwrap(), project_id)?;
        let arc = Arc::new(Mutex::new(room));
        rooms.insert(project_id.to_string(), arc.clone());
        Ok(arc)
    }

    fn open_room(&self, project_id: &str) -> Option<Arc<Mutex<Room>>> {
        self.rooms.lock().unwrap().get(project_id).cloned()
    }

    fn member_proto(row: &store::MemberRow) -> Member {
        Member {
            user_id: row.user_id.clone(),
            display_name: row.display_name.clone(),
            role: row.role.clone(),
            color: row.color,
        }
    }
}

fn member_json(m: &store::MemberRow) -> Json_ {
    json!({
        "userId": m.user_id,
        "displayName": m.display_name,
        "email": m.email,
        "role": m.role,
        "color": m.color,
    })
}

fn normalize_email(email: &str) -> String {
    email.trim().to_lowercase()
}

// Routes -----------------------------------------------------------------------

async fn healthz() -> Json<Json_> {
    Json(json!({
        "status": "ok",
        "protocol": PROTOCOL_VERSION,
        "version": env!("CARGO_PKG_VERSION"),
    }))
}

#[derive(Deserialize)]
struct SignUp {
    email: String,
    password: String,
    #[serde(rename = "displayName")]
    display_name: String,
}

async fn sign_up(State(st): State<Arc<AppState>>, Json(b): Json<SignUp>) -> ApiResult<Response> {
    let email = normalize_email(&b.email);
    let name = b.display_name.trim();
    if !email.contains('@') || email.len() > 254 {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "Enter a valid email address.",
        ));
    }
    if name.is_empty() || name.chars().count() > 64 {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "Enter a display name of up to 64 characters.",
        ));
    }
    if b.password.chars().count() < auth::MIN_PASSWORD_LENGTH {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            format!(
                "Use a password of at least {} characters.",
                auth::MIN_PASSWORD_LENGTH
            ),
        ));
    }
    let hash = auth::hash_password(&b.password).ok_or_else(|| {
        ApiError::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "Couldn't hash the password.",
        )
    })?;
    let user = store::User {
        id: auth::random_id("u_"),
        email,
        display_name: name.to_string(),
        password_hash: hash,
    };
    if !st.store.lock().unwrap().create_user(&user, st.now())? {
        return Err(ApiError::new(
            StatusCode::CONFLICT,
            "An account with this email already exists.",
        ));
    }
    Ok((StatusCode::CREATED, Json(st.session_json(&user, None)?)).into_response())
}

#[derive(Deserialize)]
struct SignIn {
    email: String,
    password: String,
}

async fn sign_in(
    State(st): State<Arc<AppState>>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Json(b): Json<SignIn>,
) -> ApiResult<Json<Json_>> {
    let email = normalize_email(&b.email);
    let ip = format!("ip:{}", addr.ip());
    let account = format!("account:{email}");
    let keys = [ip.as_str(), account.as_str()];
    let now = st.now();
    if st.limiter.limited(&keys, now) {
        return Err(ApiError::new(
            StatusCode::TOO_MANY_REQUESTS,
            "Too many attempts. Try again in a minute.",
        ));
    }
    let user = st.store.lock().unwrap().user_by_email(&email)?;
    match user {
        Some(u) if auth::verify_password(&b.password, &u.password_hash) => {
            Ok(Json(st.session_json(&u, None)?))
        }
        _ => {
            st.limiter.record_failure(&keys, now);
            Err(ApiError::new(
                StatusCode::UNAUTHORIZED,
                "The email or password is wrong.",
            ))
        }
    }
}

#[derive(Deserialize)]
struct Refresh {
    #[serde(rename = "refreshToken")]
    refresh_token: String,
}

async fn refresh(
    State(st): State<Arc<AppState>>,
    Json(b): Json<Refresh>,
) -> ApiResult<Json<Json_>> {
    let user = {
        let store = st.store.lock().unwrap();
        let id = store
            .refresh_token_user(&auth::token_hash(&b.refresh_token), st.now())?
            .ok_or_else(unauthorized)?;
        store.user_by_id(&id)?.ok_or_else(unauthorized)?
    };
    Ok(Json(st.session_json(&user, Some(b.refresh_token))?))
}

async fn me(State(st): State<Arc<AppState>>, headers: HeaderMap) -> ApiResult<Json<Json_>> {
    let id = st.user_from_headers(&headers)?;
    let u = st
        .store
        .lock()
        .unwrap()
        .user_by_id(&id)?
        .ok_or_else(unauthorized)?;
    Ok(Json(
        json!({ "userId": u.id, "email": u.email, "displayName": u.display_name }),
    ))
}

async fn list_projects(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
) -> ApiResult<Json<Json_>> {
    let user = st.user_from_headers(&headers)?;
    let rows = st.store.lock().unwrap().projects_of(&user)?;
    Ok(Json(Json_::Array(
        rows.iter()
            .map(|(p, role)| json!({ "projectId": p.id, "name": p.name, "role": role, "ownerId": p.owner_id }))
            .collect(),
    )))
}

#[derive(Deserialize)]
struct CreateProject {
    #[serde(rename = "projectId")]
    project_id: Option<String>,
    name: String,
}

/// Shares a project: the caller becomes its Owner. Sharing the same id again is a no-op
/// for its Owner (so an interrupted share can be retried) and a conflict for anyone else.
async fn create_project(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Json(b): Json<CreateProject>,
) -> ApiResult<Response> {
    let user = st.user_from_headers(&headers)?;
    let id = b
        .project_id
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| auth::random_id("p_"));
    if id.len() > 128
        || !id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
    {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "Project ids use letters, digits, - and _.",
        ));
    }
    let store = st.store.lock().unwrap();
    let project = store::Project {
        id: id.clone(),
        name: b.name.trim().to_string(),
        owner_id: user.clone(),
    };
    let body = json!({ "projectId": id, "name": project.name, "role": "owner" });
    if store.create_project(&project, st.now())? {
        return Ok((StatusCode::CREATED, Json(body)).into_response());
    }
    match store.project(&id)? {
        Some(p) if p.owner_id == user => Ok(Json(body).into_response()),
        _ => Err(ApiError::new(
            StatusCode::CONFLICT,
            "A project with this id already exists.",
        )),
    }
}

async fn get_project(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Json<Json_>> {
    let user = st.user_from_headers(&headers)?;
    let role = st.role(&id, &user)?;
    let p = st
        .store
        .lock()
        .unwrap()
        .project(&id)?
        .ok_or_else(not_found)?;
    Ok(Json(
        json!({ "projectId": p.id, "name": p.name, "role": role, "ownerId": p.owner_id }),
    ))
}

async fn delete_project(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<StatusCode> {
    let user = st.user_from_headers(&headers)?;
    if st.role(&id, &user)? != "owner" {
        return Err(forbidden("Only the Owner can delete the project."));
    }
    st.store.lock().unwrap().delete_project(&id)?;
    if let Some(room) = st.rooms.lock().unwrap().remove(&id) {
        let room = room.lock().unwrap();
        let m = Message::Notice(Notice {
            kind: notice::PROJECT_DELETED.into(),
            ..Default::default()
        });
        for c in room.clients.values() {
            c.send(&m);
            let _ = c.tx.send(Outbound::Close);
        }
    }
    Ok(StatusCode::NO_CONTENT)
}

/// The whole document, for a member opening a shared project for the first time.
async fn get_document(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    let user = st.user_from_headers(&headers)?;
    st.role(&id, &user)?;
    let bytes = match st.open_room(&id) {
        Some(room) => room.lock().unwrap().doc.save(),
        None => {
            let mut room = Room::load(&st.store.lock().unwrap(), &id)
                .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e))?;
            room.doc.save()
        }
    };
    Ok(([("content-type", "application/octet-stream")], bytes).into_response())
}

async fn list_members(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Json<Json_>> {
    let user = st.user_from_headers(&headers)?;
    st.role(&id, &user)?;
    let rows = st.store.lock().unwrap().members(&id)?;
    Ok(Json(Json_::Array(rows.iter().map(member_json).collect())))
}

#[derive(Deserialize)]
struct RoleBody {
    role: String,
}

async fn change_role(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path((id, member)): Path<(String, String)>,
    Json(b): Json<RoleBody>,
) -> ApiResult<Json<Json_>> {
    let user = st.user_from_headers(&headers)?;
    if st.role(&id, &user)? != "owner" {
        return Err(forbidden("Only the Owner can change roles."));
    }
    if !matches!(b.role.as_str(), "editor" | "viewer") {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "The role must be editor or viewer.",
        ));
    }
    let row = {
        let store = st.store.lock().unwrap();
        if store.role_of(&id, &member)?.as_deref() == Some("owner") {
            return Err(ApiError::new(
                StatusCode::BAD_REQUEST,
                "The Owner's role can't change.",
            ));
        }
        if !store.set_role(&id, &member, &b.role)? {
            return Err(ApiError::new(
                StatusCode::NOT_FOUND,
                "Not a member of this project.",
            ));
        }
        store
            .members(&id)?
            .into_iter()
            .find(|m| m.user_id == member)
    };
    if let (Some(room), Some(row)) = (st.open_room(&id), &row) {
        let mut room = room.lock().unwrap();
        for c in room.clients.values_mut().filter(|c| c.user_id == member) {
            c.role = b.role.clone();
        }
        room.broadcast_except(
            None,
            &Message::Notice(Notice {
                kind: notice::ROLE_CHANGED.into(),
                user_id: Some(member.clone()),
                role: Some(b.role.clone()),
                member: Some(AppState::member_proto(row)),
                ..Default::default()
            }),
        );
    }
    Ok(Json(row.as_ref().map(member_json).unwrap_or(Json_::Null)))
}

/// Removes a member (the Owner removing anyone else, or a member leaving). Their sockets
/// close at once with a `removed` notice (05 §11, §13).
async fn remove_member(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path((id, member)): Path<(String, String)>,
) -> ApiResult<StatusCode> {
    let user = st.user_from_headers(&headers)?;
    let role = st.role(&id, &user)?;
    if role != "owner" && user != member {
        return Err(forbidden("Only the Owner can remove members."));
    }
    {
        let store = st.store.lock().unwrap();
        if store.role_of(&id, &member)?.as_deref() == Some("owner") {
            return Err(ApiError::new(
                StatusCode::BAD_REQUEST,
                "The Owner can't be removed.",
            ));
        }
        if !store.remove_member(&id, &member)? {
            return Err(ApiError::new(
                StatusCode::NOT_FOUND,
                "Not a member of this project.",
            ));
        }
    }
    if let Some(room) = st.open_room(&id) {
        let mut room = room.lock().unwrap();
        let removed: Vec<u64> = room
            .clients
            .iter()
            .filter(|(_, c)| c.user_id == member)
            .map(|(k, _)| *k)
            .collect();
        for k in removed {
            if let Some(c) = room.clients.remove(&k) {
                c.send(&Message::Notice(Notice {
                    kind: notice::REMOVED.into(),
                    user_id: Some(member.clone()),
                    message: Some("You were removed from this project.".into()),
                    ..Default::default()
                }));
                let _ = c.tx.send(Outbound::Close);
            }
        }
        room.broadcast_except(
            None,
            &Message::Notice(Notice {
                kind: notice::MEMBER_REMOVED.into(),
                user_id: Some(member),
                ..Default::default()
            }),
        );
    }
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Deserialize)]
struct CreateInvite {
    role: String,
    #[serde(rename = "expiresInDays")]
    expires_in_days: Option<i64>,
}

fn base_url(headers: &HeaderMap) -> String {
    let host = headers
        .get("host")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("localhost");
    let proto = headers
        .get("x-forwarded-proto")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("http");
    format!("{proto}://{host}")
}

/// An invite link with a role and an expiry (default 7 days). Editors may invite
/// Editors and Viewers; Viewers can't invite (05 §3.2).
async fn create_invite(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(b): Json<CreateInvite>,
) -> ApiResult<Response> {
    let user = st.user_from_headers(&headers)?;
    let role = st.role(&id, &user)?;
    if !matches!(b.role.as_str(), "editor" | "viewer") {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "Invite as an Editor or a Viewer.",
        ));
    }
    if role == "viewer" {
        return Err(forbidden("Viewers can't invite people."));
    }
    let days = b
        .expires_in_days
        .unwrap_or(INVITE_DEFAULT_DAYS)
        .clamp(1, 90);
    let token = auth::random_token(24);
    let invite = store::Invite {
        project_id: id,
        role: b.role.clone(),
        expires_ms: st.now() + days * DAY_MS,
    };
    st.store
        .lock()
        .unwrap()
        .add_invite(&token, &invite, &user)?;
    let url = format!("{}/v1/invites/{token}", base_url(&headers));
    Ok((
        StatusCode::CREATED,
        Json(json!({ "token": token, "url": url, "role": b.role, "expiresAt": invite.expires_ms })),
    )
        .into_response())
}

fn live_invite(st: &AppState, token: &str) -> ApiResult<(store::Invite, store::Project)> {
    let store = st.store.lock().unwrap();
    let invite = store
        .invite(token)?
        .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "This invite link isn't valid."))?;
    if invite.expires_ms <= st.now() {
        return Err(ApiError::new(
            StatusCode::GONE,
            "This invite link has expired.",
        ));
    }
    let project = store.project(&invite.project_id)?.ok_or_else(not_found)?;
    Ok((invite, project))
}

async fn get_invite(
    State(st): State<Arc<AppState>>,
    Path(token): Path<String>,
) -> ApiResult<Json<Json_>> {
    let (invite, project) = live_invite(&st, &token)?;
    Ok(Json(json!({
        "projectId": project.id, "name": project.name, "role": invite.role, "expiresAt": invite.expires_ms,
    })))
}

async fn accept_invite(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(token): Path<String>,
) -> ApiResult<Json<Json_>> {
    let user = st.user_from_headers(&headers)?;
    let (invite, project) = live_invite(&st, &token)?;
    let (added, row) = {
        let store = st.store.lock().unwrap();
        let added = store.add_member(&project.id, &user, &invite.role)?;
        let row = store
            .members(&project.id)?
            .into_iter()
            .find(|m| m.user_id == user);
        (added, row)
    };
    let role = row.as_ref().map(|r| r.role.clone()).unwrap_or(invite.role);
    if let (true, Some(room), Some(row)) = (added, st.open_room(&project.id), &row) {
        room.lock().unwrap().broadcast_except(
            None,
            &Message::Notice(Notice {
                kind: notice::MEMBER_ADDED.into(),
                user_id: Some(user.clone()),
                member: Some(AppState::member_proto(row)),
                ..Default::default()
            }),
        );
    }
    Ok(Json(
        json!({ "projectId": project.id, "name": project.name, "role": role }),
    ))
}

#[derive(Deserialize)]
struct AdvanceClock {
    #[serde(rename = "advanceMs")]
    advance_ms: i64,
}

async fn advance_clock(
    State(st): State<Arc<AppState>>,
    Json(b): Json<AdvanceClock>,
) -> ApiResult<Json<Json_>> {
    if !st.test_clock {
        return Err(ApiError::new(StatusCode::NOT_FOUND, "Not found."));
    }
    st.clock.advance(b.advance_ms);
    Ok(Json(json!({ "now": st.now() })))
}

// WebSocket sync ------------------------------------------------------------------

async fn ws_upgrade(
    State(st): State<Arc<AppState>>,
    Path(id): Path<String>,
    ws: WebSocketUpgrade,
) -> Response {
    ws.max_message_size(256 << 20)
        .on_upgrade(move |socket| connection(st, id, socket))
}

fn reject_frame(code: &str, message: &str) -> WsMessage {
    WsMessage::Binary(
        Message::Reject(Reject {
            code: code.into(),
            message: message.into(),
        })
        .encode()
        .into(),
    )
}

async fn connection(st: Arc<AppState>, project_id: String, socket: WebSocket) {
    let (mut sink, mut stream) = socket.split();
    let hello = match tokio::time::timeout(Duration::from_secs(10), stream.next()).await {
        Ok(Some(Ok(WsMessage::Binary(b)))) => match Message::decode(&b) {
            Ok(Message::Hello(h)) => h,
            _ => return,
        },
        _ => return,
    };
    let refuse = |code: &'static str, msg: &'static str| (code, msg);
    let outcome: Result<(String, String), (&str, &str)> = (|| {
        let user = st
            .tokens
            .verify(&hello.token, st.now())
            .ok_or(refuse(reject::AUTH, "Sign in again."))?;
        if hello.project_id != project_id {
            return Err(refuse(reject::NOT_FOUND, "Project not found."));
        }
        let store = st.store.lock().unwrap();
        if store.project(&project_id).ok().flatten().is_none() {
            return Err(refuse(reject::NOT_FOUND, "Project not found."));
        }
        let role = store
            .role_of(&project_id, &user)
            .ok()
            .flatten()
            .ok_or(refuse(
                reject::FORBIDDEN,
                "You aren't a member of this project.",
            ))?;
        Ok((user, role))
    })();
    let (user_id, role) = match outcome {
        Ok(v) => v,
        Err((code, msg)) => {
            let _ = sink.send(reject_frame(code, msg)).await;
            let _ = sink.close().await;
            return;
        }
    };
    let room_arc = match st.room(&project_id) {
        Ok(r) => r,
        Err(_) => {
            let _ = sink
                .send(reject_frame(
                    reject::NOT_FOUND,
                    "The project's history can't be read.",
                ))
                .await;
            return;
        }
    };
    let members: Vec<Member> = st
        .store
        .lock()
        .unwrap()
        .members(&project_id)
        .unwrap_or_default()
        .iter()
        .map(AppState::member_proto)
        .collect();

    let conn_id = st.next_conn.fetch_add(1, Ordering::Relaxed);
    let (tx, mut rx) = unbounded_channel::<Outbound>();
    let mut sync_state = automerge::sync::State::new();
    let joined = {
        let mut room = room_arc.lock().unwrap();
        if i64::from(hello.schema_version) < room.schema_version() {
            false
        } else {
            let welcome = Message::Welcome(Welcome {
                role: role.clone(),
                user_id: user_id.clone(),
                members,
                presence: room.presence_snapshot(conn_id),
                heads: room.heads_bytes(),
            });
            let _ = tx.send(Outbound::Frame(welcome.encode()));
            room.broadcast_except(
                None,
                &Message::Notice(Notice {
                    kind: notice::CLIENT_JOINED.into(),
                    user_id: Some(user_id.clone()),
                    client_id: Some(hello.client_id.clone()),
                    ..Default::default()
                }),
            );
            room.clients.insert(
                conn_id,
                Client {
                    user_id: user_id.clone(),
                    client_id: hello.client_id.clone(),
                    role,
                    tx: tx.clone(),
                    presence: None,
                },
            );
            let first = room.doc.sync().generate_sync_message(&mut sync_state);
            if let Some(m) = first {
                let _ = tx.send(Outbound::Frame(Message::Sync(m.encode()).encode()));
            }
            true
        }
    };
    if !joined {
        let _ = sink
            .send(reject_frame(
                reject::UPDATE_REQUIRED,
                "This project needs a newer version of Weft Studio.",
            ))
            .await;
        let _ = sink.close().await;
        drop(room_arc);
        st.release_room_if_idle(&project_id);
        return;
    }

    let writer = tokio::spawn(async move {
        while let Some(o) = rx.recv().await {
            match o {
                Outbound::Frame(f) => {
                    if sink.send(WsMessage::Binary(f.into())).await.is_err() {
                        break;
                    }
                }
                Outbound::Close => {
                    let _ = sink.close().await;
                    break;
                }
            }
        }
    });

    let idle = Duration::from_millis(weft_proto::IDLE_TIMEOUT_MS);
    loop {
        match tokio::time::timeout(idle, stream.next()).await {
            Ok(Some(Ok(WsMessage::Binary(b)))) => {
                if !st.handle_frame(&room_arc, conn_id, &mut sync_state, &b) {
                    break;
                }
            }
            Ok(Some(Ok(WsMessage::Close(_)))) | Ok(Some(Err(_))) | Ok(None) | Err(_) => break,
            Ok(Some(Ok(_))) => {}
        }
    }

    {
        let mut room = room_arc.lock().unwrap();
        if room.clients.remove(&conn_id).is_some() {
            room.broadcast_except(
                None,
                &Message::Notice(Notice {
                    kind: notice::CLIENT_LEFT.into(),
                    user_id: Some(user_id),
                    client_id: Some(hello.client_id),
                    ..Default::default()
                }),
            );
        }
    }
    drop(tx);
    let _ = writer.await;
    drop(room_arc);
    st.release_room_if_idle(&project_id);
}

impl AppState {
    /// Unloads a room nobody is connected to.
    /// Unloads a room nobody is in. A connection holding the room (one that is joining,
    /// or a request working on it) keeps it loaded: unloading it then would leave that
    /// connection in a room the next joiner can't see, and their changes would stop
    /// reaching each other. Callers drop their own reference first.
    fn release_room_if_idle(&self, project_id: &str) {
        let mut rooms = self.rooms.lock().unwrap();
        let idle = rooms
            .get(project_id)
            .is_some_and(|r| Arc::strong_count(r) == 1 && r.lock().unwrap().clients.is_empty());
        if idle {
            rooms.remove(project_id);
        }
    }

    /// Handles one frame from a client; false closes the connection.
    fn handle_frame(
        &self,
        room_arc: &Arc<Mutex<Room>>,
        conn_id: u64,
        sync_state: &mut automerge::sync::State,
        frame: &[u8],
    ) -> bool {
        let Ok(msg) = Message::decode(frame) else {
            return true;
        };
        let now = self.now();
        let mut room = room_arc.lock().unwrap();
        let Some(client) = room.clients.get(&conn_id) else {
            // Removed from the project while this frame was in flight.
            return false;
        };
        let is_viewer = client.role == "viewer";
        let sender_tx = client.tx.clone();
        let send = |m: Message| {
            let _ = sender_tx.send(Outbound::Frame(m.encode()));
        };
        let forbid = |hashes: Vec<u8>| {
            send(Message::Notice(Notice {
                kind: notice::FORBIDDEN.into(),
                message: Some("Viewers can only add comments and chat.".into()),
                hashes,
                ..Default::default()
            }));
        };

        match msg {
            Message::Sync(bytes) => {
                let Ok(m) = automerge::sync::Message::decode(&bytes) else {
                    return true;
                };
                if is_viewer {
                    let mut probe_state = sync_state.clone();
                    let probe = m.clone();
                    let allowed = room.viewer_allowed(|d| {
                        d.sync()
                            .receive_sync_message(&mut probe_state, probe)
                            .map_err(|e| e.to_string())
                    });
                    if !matches!(allowed, Ok(true)) {
                        forbid(Vec::new());
                        return true;
                    }
                }
                let applied = {
                    let mut store = self.store.lock().unwrap();
                    room.apply(&mut store, now, |d| {
                        d.sync()
                            .receive_sync_message(sync_state, m)
                            .map_err(|e| e.to_string())
                    })
                };
                match applied {
                    Ok(a) => {
                        let hashes: Vec<u8> =
                            a.new_changes.iter().flat_map(|c| c.hash().0).collect();
                        send(Message::Ack(Ack {
                            hashes,
                            heads: room.heads_bytes(),
                        }));
                        fan_out(&room, conn_id, &a.new_changes);
                    }
                    Err(e) => {
                        eprintln!("weft-relay: sync from {conn_id} refused: {e}");
                        return reload_room(self, &mut room);
                    }
                }
                let reply = room.doc.sync().generate_sync_message(sync_state);
                if let Some(reply) = reply {
                    send(Message::Sync(reply.encode()));
                }
            }
            Message::Change(bytes) => {
                let single = automerge::Change::from_bytes(bytes.clone()).ok();
                let offered: Vec<u8> = single
                    .as_ref()
                    .map(|c| c.hash().0.to_vec())
                    .unwrap_or_default();
                let load = |d: &mut automerge::AutoCommit| {
                    d.load_incremental(&bytes)
                        .map(|_| ())
                        .map_err(|e| e.to_string())
                };
                if is_viewer && !matches!(room.viewer_allowed(load), Ok(true)) {
                    forbid(offered);
                    return true;
                }
                let applied = {
                    let mut store = self.store.lock().unwrap();
                    room.apply(&mut store, now, load)
                };
                match applied {
                    Ok(a) => {
                        let mut hashes: Vec<u8> =
                            a.new_changes.iter().flat_map(|c| c.hash().0).collect();
                        if let Some(c) = &single {
                            let already_acked = a.new_changes.iter().any(|n| n.hash() == c.hash());
                            if !already_acked && room.doc.get_change_by_hash(&c.hash()).is_some() {
                                hashes.extend_from_slice(&c.hash().0);
                            }
                        }
                        if let Some(c) = &single {
                            if room.doc.get_change_by_hash(&c.hash()).is_none() {
                                let missing: Vec<_> = c
                                    .deps()
                                    .iter()
                                    .filter(|d| room.doc.get_change_by_hash(d).is_none())
                                    .collect();
                                eprintln!(
                                    "weft-relay: change {} from {conn_id} not applied; missing deps {:?}",
                                    c.hash(),
                                    missing
                                );
                            }
                        }
                        send(Message::Ack(Ack {
                            hashes,
                            heads: room.heads_bytes(),
                        }));
                        fan_out(&room, conn_id, &a.new_changes);
                    }
                    Err(e) => {
                        eprintln!("weft-relay: change from {conn_id} refused: {e}");
                        return reload_room(self, &mut room);
                    }
                }
            }
            Message::Presence(mut entry) => {
                if let Some(c) = room.clients.get_mut(&conn_id) {
                    c.presence = Some(entry.state.clone());
                    entry.client_id = c.client_id.clone();
                    entry.user_id = c.user_id.clone();
                }
                room.broadcast_except(Some(conn_id), &Message::Presence(entry));
            }
            Message::Ping(t) => send(Message::Pong(t)),
            _ => {}
        }
        true
    }
}

/// Sends newly stored changes to every other client, in the order they were appended.
fn fan_out(room: &Room, from: u64, changes: &[automerge::Change]) {
    if changes.is_empty() {
        return;
    }
    let mut bytes = Vec::new();
    for c in changes {
        bytes.extend_from_slice(c.raw_bytes());
    }
    room.broadcast_except(Some(from), &Message::Change(bytes));
}

/// After a failed write the in-memory document may be ahead of the log: reload it from
/// the log and drop the connection so the client resyncs (05 §13, corrupt change).
fn reload_room(st: &AppState, room: &mut Room) -> bool {
    if let Ok(fresh) = Room::load(&st.store.lock().unwrap(), &room.project_id) {
        room.doc = fresh.doc;
    }
    false
}
