//! Media blobs (spec 05 §8): content-addressed audio files. Uploads are resumable in
//! 8 MB chunks and verified against their BLAKE3 hash before anyone can download them;
//! downloads support HTTP Range so clients can fetch (and play) a file front to back.
//!
//! Every request names the project it is for (`?project=<id>`). Someone who isn't a
//! member gets 403, whether or not the blob exists; a member asking for a blob the
//! project doesn't have gets 404. A blob stored for one project is reused by another
//! project of the same Owner without a second upload (D-COL-9).

use axum::body::{Body, Bytes};
use axum::extract::{Path, Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde::Deserialize;
use serde_json::json;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path as FsPath, PathBuf};
use std::sync::Arc;
use weft_proto::{BlobAvailable, Message};

use crate::{auth, forbidden, store, ApiError, ApiResult, AppState};

/// Upload chunk size (05 §8.2).
pub const CHUNK_BYTES: i64 = 8 << 20;
/// The largest blob (a 4 GB WAV, 05 §8.4).
pub const MAX_BLOB_BYTES: i64 = 4 << 30;
/// Range requests are answered with at most this much.
const MAX_RANGE_BYTES: u64 = 16 << 20;

/// Where blob files and unfinished uploads live: `<root>/blobs/<hex>` and
/// `<root>/uploads/<id>.part`. A relay without a data folder uses a temporary
/// folder that goes away with it.
pub struct BlobDir {
    root: PathBuf,
    temporary: bool,
}

impl BlobDir {
    pub fn new(root: PathBuf, temporary: bool) -> Result<BlobDir, String> {
        for sub in ["blobs", "uploads"] {
            std::fs::create_dir_all(root.join(sub))
                .map_err(|e| format!("{}: {e}", root.join(sub).display()))?;
        }
        Ok(BlobDir { root, temporary })
    }

    pub fn temporary() -> Result<BlobDir, String> {
        let dir = std::env::temp_dir().join(auth::random_id("weft-relay-blobs-"));
        BlobDir::new(dir, true)
    }

    fn blob(&self, hex: &str) -> PathBuf {
        self.root.join("blobs").join(hex)
    }

    fn part(&self, upload_id: &str) -> PathBuf {
        self.root.join("uploads").join(format!("{upload_id}.part"))
    }
}

impl Drop for BlobDir {
    fn drop(&mut self) {
        if self.temporary {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }
}

/// "b3:<64 hex>" -> the hex digits.
pub fn parse_hash(hash: &str) -> Option<&str> {
    let hex = hash.strip_prefix("b3:")?;
    (hex.len() == 64
        && hex
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)))
    .then_some(hex)
}

fn bad_hash() -> ApiError {
    ApiError::new(StatusCode::BAD_REQUEST, "Not a blob hash.")
}

fn not_here() -> ApiError {
    ApiError::new(StatusCode::NOT_FOUND, "This project has no such file yet.")
}

fn hash_file(path: &FsPath) -> std::io::Result<String> {
    let mut f = std::fs::File::open(path)?;
    let mut hasher = blake3::Hasher::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(format!("b3:{}", hasher.finalize().to_hex()))
}

impl AppState {
    /// The caller's role in the project; 403 for anyone who isn't a member (AT-COL-16).
    fn blob_role(
        &self,
        headers: &HeaderMap,
        q: &HashMap<String, String>,
    ) -> ApiResult<(String, String, String)> {
        let user = self.user_from_headers(headers)?;
        let project = q.get("project").cloned().unwrap_or_default();
        let role = self
            .store
            .lock()
            .unwrap()
            .role_of(&project, &user)?
            .ok_or_else(|| forbidden("You aren't a member of this project."))?;
        Ok((user, project, role))
    }

    /// Links an existing blob of the project's Owner to the project; true when it now has it.
    fn link_owned_blob(&self, hash: &str, project: &str) -> ApiResult<Option<i64>> {
        let store = self.store.lock().unwrap();
        if let Some(size) = store.blob_in_project(hash, project)? {
            return Ok(Some(size));
        }
        let Some(owner) = store.project(project)?.map(|p| p.owner_id) else {
            return Ok(None);
        };
        let Some(size) = store.blob_of_owner(hash, &owner)? else {
            return Ok(None);
        };
        store.link_blob(hash, project)?;
        drop(store);
        self.announce_blob(project, hash, size);
        Ok(Some(size))
    }

    /// Tells the project's open room a blob can be downloaded (`BLOB_AVAILABLE`).
    fn announce_blob(&self, project: &str, hash: &str, size: i64) {
        if let Some(room) = self.open_room(project) {
            room.lock().unwrap().broadcast_except(
                None,
                &Message::BlobAvailable(BlobAvailable {
                    hash: hash.to_string(),
                    size: size.max(0) as u64,
                }),
            );
        }
    }
}

/// `HEAD /v1/blobs/<hash>?project=<id>`: 200 when the project has the blob (an Editor
/// asking links a blob the Owner already stored for another project), else 404.
pub async fn head_blob(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(hash): Path<String>,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<Response> {
    parse_hash(&hash).ok_or_else(bad_hash)?;
    let (_, project, role) = st.blob_role(&headers, &q)?;
    let size = if role == "viewer" {
        st.store.lock().unwrap().blob_in_project(&hash, &project)?
    } else {
        st.link_owned_blob(&hash, &project)?
    };
    let size = size.ok_or_else(not_here)?;
    Ok((StatusCode::OK, [(header::CONTENT_LENGTH, size.to_string())]).into_response())
}

/// `bytes=a-b`, `bytes=a-` or `bytes=-n` -> (first, last) within `size`.
fn parse_range(v: &str, size: u64) -> Option<(u64, u64)> {
    let spec = v.trim().strip_prefix("bytes=")?;
    if spec.contains(',') || size == 0 {
        return None;
    }
    let (a, b) = spec.split_once('-')?;
    let (first, last) = if a.is_empty() {
        let n: u64 = b.parse().ok()?;
        (size.saturating_sub(n), size - 1)
    } else {
        let first: u64 = a.parse().ok()?;
        let last = if b.is_empty() {
            size - 1
        } else {
            b.parse::<u64>().ok()?.min(size - 1)
        };
        (first, last)
    };
    (first <= last && first < size).then_some((first, last))
}

fn read_span(path: &FsPath, first: u64, len: u64) -> std::io::Result<Vec<u8>> {
    let mut f = std::fs::File::open(path)?;
    f.seek(SeekFrom::Start(first))?;
    let mut buf = vec![0u8; len as usize];
    f.read_exact(&mut buf)?;
    Ok(buf)
}

/// `GET /v1/blobs/<hash>?project=<id>`, with optional `Range` (05 §8.3).
pub async fn get_blob(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(hash): Path<String>,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<Response> {
    let hex = parse_hash(&hash).ok_or_else(bad_hash)?.to_string();
    let (_, project, _) = st.blob_role(&headers, &q)?;
    let size = st
        .store
        .lock()
        .unwrap()
        .blob_in_project(&hash, &project)?
        .ok_or_else(not_here)? as u64;
    let path = st.blob_dir.blob(&hex);
    let io_error =
        |e: std::io::Error| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string());
    if let Some(range) = headers.get(header::RANGE).and_then(|v| v.to_str().ok()) {
        let Some((first, last)) = parse_range(range, size) else {
            return Ok((
                StatusCode::RANGE_NOT_SATISFIABLE,
                [(header::CONTENT_RANGE, format!("bytes */{size}"))],
            )
                .into_response());
        };
        let last = last.min(first + MAX_RANGE_BYTES - 1);
        let bytes = tokio::task::spawn_blocking(move || read_span(&path, first, last - first + 1))
            .await
            .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?
            .map_err(io_error)?;
        return Ok((
            StatusCode::PARTIAL_CONTENT,
            [
                (header::CONTENT_TYPE, "application/octet-stream".to_string()),
                (
                    header::CONTENT_RANGE,
                    format!("bytes {first}-{last}/{size}"),
                ),
                (header::ACCEPT_RANGES, "bytes".to_string()),
            ],
            bytes,
        )
            .into_response());
    }
    // The whole file, streamed a megabyte at a time.
    let file = tokio::fs::File::open(&path).await.map_err(io_error)?;
    let stream = futures_util::stream::unfold(file, |mut f| async move {
        use tokio::io::AsyncReadExt;
        let mut buf = vec![0u8; 1 << 20];
        match f.read(&mut buf).await {
            Ok(0) => None,
            Ok(n) => {
                buf.truncate(n);
                Some((Ok::<_, std::io::Error>(Bytes::from(buf)), f))
            }
            Err(e) => Some((Err(e), f)),
        }
    });
    Ok((
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, "application/octet-stream".to_string()),
            (header::CONTENT_LENGTH, size.to_string()),
            (header::ACCEPT_RANGES, "bytes".to_string()),
        ],
        Body::from_stream(stream),
    )
        .into_response())
}

#[derive(Deserialize)]
pub struct StartUpload {
    size: i64,
}

fn may_upload(role: &str) -> ApiResult<()> {
    if role == "viewer" {
        return Err(forbidden("Viewers can't add audio to the project."));
    }
    Ok(())
}

/// `POST /v1/blobs/<hash>/uploads?project=<id>` `{size}` -> `{uploadId, offset}` to
/// continue from, or `{complete: true}` when the project already has the blob.
/// An unfinished upload of the same blob for the same project is resumed.
pub async fn start_upload(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(hash): Path<String>,
    Query(q): Query<HashMap<String, String>>,
    Json(b): Json<StartUpload>,
) -> ApiResult<Response> {
    parse_hash(&hash).ok_or_else(bad_hash)?;
    let (_, project, role) = st.blob_role(&headers, &q)?;
    may_upload(&role)?;
    if b.size < 0 || b.size > MAX_BLOB_BYTES {
        return Err(ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "Audio files can be at most 4 GB.",
        ));
    }
    if st.link_owned_blob(&hash, &project)?.is_some() {
        return Ok(Json(json!({ "complete": true })).into_response());
    }
    let store = st.store.lock().unwrap();
    if let Some(u) = store.open_upload(&hash, &project)? {
        if u.size == b.size {
            // What's on disk past the last recorded chunk didn't finish arriving.
            let part = st.blob_dir.part(&u.id);
            if let Ok(f) = std::fs::OpenOptions::new().write(true).open(&part) {
                let _ = f.set_len(u.received as u64);
            }
            return Ok(Json(
                json!({ "uploadId": u.id, "offset": u.received, "chunkSize": CHUNK_BYTES }),
            )
            .into_response());
        }
        store.delete_upload(&u.id)?;
        let _ = std::fs::remove_file(st.blob_dir.part(&u.id));
    }
    let upload = store::Upload {
        id: auth::random_id("up_"),
        hash: hash.clone(),
        project_id: project,
        size: b.size,
        received: 0,
    };
    std::fs::File::create(st.blob_dir.part(&upload.id))
        .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    store.create_upload(&upload, st.now())?;
    Ok((
        StatusCode::CREATED,
        Json(json!({ "uploadId": upload.id, "offset": 0, "chunkSize": CHUNK_BYTES })),
    )
        .into_response())
}

fn upload_for(st: &AppState, id: &str, hash: &str, project: &str) -> ApiResult<store::Upload> {
    st.store
        .lock()
        .unwrap()
        .upload(id)?
        .filter(|u| u.hash == hash && u.project_id == project)
        .ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "No such upload."))
}

/// `PUT /v1/blobs/<hash>/uploads/<id>?project=<id>&offset=<n>` with up to 8 MB.
/// A chunk at the wrong offset gets 409 and the offset to continue from.
pub async fn put_chunk(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path((hash, id)): Path<(String, String)>,
    Query(q): Query<HashMap<String, String>>,
    body: Bytes,
) -> ApiResult<Response> {
    parse_hash(&hash).ok_or_else(bad_hash)?;
    let (_, project, role) = st.blob_role(&headers, &q)?;
    may_upload(&role)?;
    let upload = upload_for(&st, &id, &hash, &project)?;
    let offset: i64 = q.get("offset").and_then(|o| o.parse().ok()).unwrap_or(-1);
    if offset != upload.received {
        return Ok((
            StatusCode::CONFLICT,
            Json(json!({ "error": "Wrong offset.", "offset": upload.received })),
        )
            .into_response());
    }
    let len = body.len() as i64;
    if len > CHUNK_BYTES || upload.received + len > upload.size {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "The chunk doesn't fit the upload.",
        ));
    }
    let part = st.blob_dir.part(&id);
    let write = move || -> std::io::Result<()> {
        let mut f = std::fs::OpenOptions::new().append(true).open(&part)?;
        f.write_all(&body)?;
        f.sync_data()
    };
    tokio::task::spawn_blocking(write)
        .await
        .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?
        .map_err(|e| ApiError::new(StatusCode::INSUFFICIENT_STORAGE, e.to_string()))?;
    let received = upload.received + len;
    st.store
        .lock()
        .unwrap()
        .set_upload_received(&id, received)?;
    Ok(Json(json!({ "offset": received })).into_response())
}

/// `POST /v1/blobs/<hash>/uploads/<id>/complete?project=<id>`: verifies the hash, makes
/// the blob available and announces it (`BLOB_AVAILABLE`). A mismatch discards the upload.
pub async fn complete_upload(
    State(st): State<Arc<AppState>>,
    headers: HeaderMap,
    Path((hash, id)): Path<(String, String)>,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<Response> {
    let hex = parse_hash(&hash).ok_or_else(bad_hash)?.to_string();
    let (_, project, role) = st.blob_role(&headers, &q)?;
    may_upload(&role)?;
    let upload = upload_for(&st, &id, &hash, &project)?;
    if upload.received != upload.size {
        return Err(ApiError::new(
            StatusCode::CONFLICT,
            format!(
                "The upload has {} of {} bytes.",
                upload.received, upload.size
            ),
        ));
    }
    let part = st.blob_dir.part(&id);
    let check = part.clone();
    let actual = tokio::task::spawn_blocking(move || hash_file(&check))
        .await
        .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?
        .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    if actual != hash {
        st.store.lock().unwrap().delete_upload(&id)?;
        let _ = std::fs::remove_file(&part);
        return Err(ApiError::new(
            StatusCode::UNPROCESSABLE_ENTITY,
            "The uploaded bytes don't match their hash.",
        ));
    }
    let dest = st.blob_dir.blob(&hex);
    if dest.exists() {
        let _ = std::fs::remove_file(&part);
    } else {
        std::fs::rename(&part, &dest)
            .map_err(|e| ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    }
    {
        let store = st.store.lock().unwrap();
        let owner = store
            .project(&project)?
            .map(|p| p.owner_id)
            .unwrap_or_default();
        store.add_blob(&hash, upload.size, &owner, &project, st.now())?;
        store.delete_upload(&id)?;
    }
    st.announce_blob(&project, &hash, upload.size);
    Ok(Json(json!({ "hash": hash, "size": upload.size })).into_response())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranges_and_hashes_parse() {
        assert_eq!(parse_range("bytes=0-99", 1000), Some((0, 99)));
        assert_eq!(parse_range("bytes=900-", 1000), Some((900, 999)));
        assert_eq!(parse_range("bytes=-10", 1000), Some((990, 999)));
        assert_eq!(parse_range("bytes=950-2000", 1000), Some((950, 999)));
        assert_eq!(parse_range("bytes=1000-", 1000), None);
        assert_eq!(parse_range("bytes=5-1", 1000), None);
        assert_eq!(parse_range("items=0-1", 1000), None);
        let h = format!("b3:{}", "a".repeat(64));
        assert!(parse_hash(&h).is_some());
        assert!(parse_hash("b3:xyz").is_none());
        assert!(parse_hash(&format!("b3:{}", "A".repeat(64))).is_none());
    }
}
