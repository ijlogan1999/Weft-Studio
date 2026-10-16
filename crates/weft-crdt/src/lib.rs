//! C ABI over Automerge for the Weft Studio client.
//!
//! The client and the relay share one Automerge implementation: the relay links
//! the `automerge` crate directly and the C++ client links this crate as a
//! static library. `include/weft_crdt.h` is the matching C header.
//!
//! The API is path based and schema agnostic: values and paths cross the
//! boundary in the binary encoding described in `codec.rs`. The C++ side
//! (`core/src/doc`) layers the spec 03 schema, commands and undo on top.
//!
//! Conventions:
//! - Every function returns a `WeftStatus` (0 = ok) unless it is a constructor
//!   (null = failure), a destructor or a query documented otherwise.
//! - Byte buffers handed to the caller are owned by the caller and released
//!   with `weft_bytes_free`.
//! - No panic crosses the FFI boundary.
//!
//! # Safety
//!
//! Every pointer argument must be null or valid for the stated length, and a
//! `WeftDoc*` must come from this library and not be used after
//! `weft_doc_free`. Functions check for null; they cannot check anything else.
#![allow(clippy::missing_safety_doc)] // covered once by the crate-level Safety section

pub mod codec;
pub mod ops;

use automerge::{transaction::CommitOptions, ActorId, AutoCommit, ChangeHash};
use ops::OpError;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::{ptr, slice};

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WeftStatus {
    Ok = 0,
    InvalidArgument = 1,
    NotFound = 2,
    WrongType = 3,
    CorruptData = 4,
    Internal = 5,
}

impl From<OpError> for WeftStatus {
    fn from(e: OpError) -> Self {
        match e {
            OpError::InvalidArgument => WeftStatus::InvalidArgument,
            OpError::NotFound => WeftStatus::NotFound,
            OpError::WrongType => WeftStatus::WrongType,
            OpError::Internal => WeftStatus::Internal,
        }
    }
}

/// An owned byte buffer returned to C. Release with `weft_bytes_free`.
#[repr(C)]
pub struct WeftBytes {
    pub data: *mut u8,
    pub len: usize,
}

impl WeftBytes {
    pub fn empty() -> Self {
        WeftBytes {
            data: ptr::null_mut(),
            len: 0,
        }
    }

    pub fn from_vec(v: Vec<u8>) -> Self {
        let mut boxed = v.into_boxed_slice();
        let out = WeftBytes {
            data: boxed.as_mut_ptr(),
            len: boxed.len(),
        };
        std::mem::forget(boxed);
        out
    }
}

/// Opaque document handle.
pub struct WeftDoc {
    inner: AutoCommit,
}

fn guard<F: FnOnce() -> WeftStatus>(f: F) -> WeftStatus {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(WeftStatus::Internal)
}

unsafe fn bytes_arg<'a>(p: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        Some(&[])
    } else if p.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(p, len))
    }
}

fn result_to_out(r: Result<Vec<u8>, OpError>, out: &mut WeftBytes) -> WeftStatus {
    match r {
        Ok(v) => {
            *out = WeftBytes::from_vec(v);
            WeftStatus::Ok
        }
        Err(e) => e.into(),
    }
}

/// Decodes the path argument and runs `f` with it.
unsafe fn with_path(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
    f: impl FnOnce(&mut AutoCommit, &[codec::Segment]) -> WeftStatus,
) -> WeftStatus {
    let Some(doc) = doc.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    let Some(raw) = bytes_arg(path, path_len) else {
        return WeftStatus::InvalidArgument;
    };
    let Ok(path) = codec::decode_path(raw) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| f(&mut doc.inner, &path))
}

// ---------------------------------------------------------------------------
// Lifetime, save and load

/// Creates an empty document with a random actor id.
#[no_mangle]
pub extern "C" fn weft_doc_new() -> *mut WeftDoc {
    catch_unwind(|| {
        Box::into_raw(Box::new(WeftDoc {
            inner: AutoCommit::new(),
        }))
    })
    .unwrap_or(ptr::null_mut())
}

/// Creates an empty document with the given actor id bytes (1..=32 bytes).
#[no_mangle]
pub unsafe extern "C" fn weft_doc_new_with_actor(actor: *const u8, len: usize) -> *mut WeftDoc {
    if actor.is_null() || len == 0 || len > 32 {
        return ptr::null_mut();
    }
    let bytes = slice::from_raw_parts(actor, len).to_vec();
    catch_unwind(move || {
        let doc = AutoCommit::new().with_actor(ActorId::from(bytes));
        Box::into_raw(Box::new(WeftDoc { inner: doc }))
    })
    .unwrap_or(ptr::null_mut())
}

/// Loads a document saved with `weft_doc_save`. Returns null on corrupt data.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_load(data: *const u8, len: usize) -> *mut WeftDoc {
    let Some(bytes) = bytes_arg(data, len) else {
        return ptr::null_mut();
    };
    // Change hashes are not re-verified on load: the saved file's chunk checksum already
    // guards against corruption, and verification costs about a third of the load time.
    let options =
        automerge::LoadOptions::new().verification_mode(automerge::VerificationMode::DontCheck);
    catch_unwind(AssertUnwindSafe(|| {
        match AutoCommit::load_with_options(bytes, options) {
            Ok(inner) => Box::into_raw(Box::new(WeftDoc { inner })),
            Err(_) => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Replaces the actor id of an existing document (e.g. after loading it from disk).
#[no_mangle]
pub unsafe extern "C" fn weft_doc_set_actor(
    doc: *mut WeftDoc,
    actor: *const u8,
    len: usize,
) -> WeftStatus {
    let Some(doc) = doc.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    if actor.is_null() || len == 0 || len > 32 {
        return WeftStatus::InvalidArgument;
    }
    let bytes = slice::from_raw_parts(actor, len).to_vec();
    guard(|| {
        doc.inner.set_actor(ActorId::from(bytes));
        WeftStatus::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn weft_doc_free(doc: *mut WeftDoc) {
    if !doc.is_null() {
        drop(Box::from_raw(doc));
    }
}

/// Serializes the whole document (compressed Automerge format). Commits pending operations first.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_save(doc: *mut WeftDoc, out: *mut WeftBytes) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| {
        *out = WeftBytes::from_vec(doc.inner.save());
        WeftStatus::Ok
    })
}

// ---------------------------------------------------------------------------
// Reads and writes

/// Sets the value at `path` (value in the codec encoding). Pending until commit.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_put(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
    value: *const u8,
    value_len: usize,
) -> WeftStatus {
    let Some(raw) = bytes_arg(value, value_len) else {
        return WeftStatus::InvalidArgument;
    };
    let Ok(v) = codec::decode_value(raw) else {
        return WeftStatus::InvalidArgument;
    };
    with_path(doc, path, path_len, |d, p| match ops::put(d, p, &v) {
        Ok(()) => WeftStatus::Ok,
        Err(e) => e.into(),
    })
}

/// Inserts a value into a list; the last path segment is the index (length appends).
#[no_mangle]
pub unsafe extern "C" fn weft_doc_insert(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
    value: *const u8,
    value_len: usize,
) -> WeftStatus {
    let Some(raw) = bytes_arg(value, value_len) else {
        return WeftStatus::InvalidArgument;
    };
    let Ok(v) = codec::decode_value(raw) else {
        return WeftStatus::InvalidArgument;
    };
    with_path(doc, path, path_len, |d, p| match ops::insert(d, p, &v) {
        Ok(()) => WeftStatus::Ok,
        Err(e) => e.into(),
    })
}

/// Deletes a map key or list element.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_delete(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
) -> WeftStatus {
    with_path(doc, path, path_len, |d, p| match ops::delete(d, p) {
        Ok(()) => WeftStatus::Ok,
        Err(e) => e.into(),
    })
}

/// Exports the value at `path` (empty path = whole document), including pending operations.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_get(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let Some(out) = out.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    with_path(doc, path, path_len, |d, p| {
        result_to_out(ops::get(d, p), out)
    })
}

/// The id (`counter@actorhex`) of the operation that wrote the current value at `path`.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_op_id(
    doc: *mut WeftDoc,
    path: *const u8,
    path_len: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let Some(out) = out.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    with_path(doc, path, path_len, |d, p| {
        result_to_out(ops::op_id(d, p).map(String::into_bytes), out)
    })
}

// ---------------------------------------------------------------------------
// Changes and history

/// Commits pending operations as one change. Returns 1 if a change was created,
/// 0 if nothing was pending, -1 on error.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_commit(
    doc: *mut WeftDoc,
    message: *const u8,
    message_len: usize,
    time_ms: i64,
) -> i32 {
    let Some(doc) = doc.as_mut() else { return -1 };
    let Some(raw) = bytes_arg(message, message_len) else {
        return -1;
    };
    let Ok(msg) = std::str::from_utf8(raw) else {
        return -1;
    };
    catch_unwind(AssertUnwindSafe(|| {
        let opts = CommitOptions::default()
            .with_message(msg)
            .with_time(time_ms);
        i32::from(doc.inner.commit_with(opts).is_some())
    }))
    .unwrap_or(-1)
}

/// Discards pending (uncommitted) operations. Returns how many were discarded.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_rollback(doc: *mut WeftDoc) -> usize {
    match doc.as_mut() {
        Some(doc) => catch_unwind(AssertUnwindSafe(|| doc.inner.rollback())).unwrap_or(0),
        None => 0,
    }
}

/// The current heads, as concatenated 32-byte change hashes.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_heads(doc: *mut WeftDoc, out: *mut WeftBytes) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| {
        *out = WeftBytes::from_vec(ops::heads_bytes(&doc.inner.get_heads()));
        WeftStatus::Ok
    })
}

/// The raw bytes of the last change made through this handle.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_last_local_change(
    doc: *mut WeftDoc,
    out: *mut WeftBytes,
) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| match doc.inner.get_last_local_change() {
        Some(change) => {
            *out = WeftBytes::from_vec(change.raw_bytes().to_vec());
            WeftStatus::Ok
        }
        None => WeftStatus::NotFound,
    })
}

/// All changes in causal order, as concatenated raw change chunks.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_all_changes(
    doc: *mut WeftDoc,
    out: *mut WeftBytes,
    count: *mut usize,
) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| {
        let changes = doc.inner.get_changes(&[]);
        if let Some(count) = count.as_mut() {
            *count = changes.len();
        }
        let bytes: Vec<u8> = changes
            .iter()
            .flat_map(|c| c.raw_bytes().to_vec())
            .collect();
        *out = WeftBytes::from_vec(bytes);
        WeftStatus::Ok
    })
}

/// Applies concatenated change chunks or saved-document bytes. Changes already
/// present are ignored.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_apply(
    doc: *mut WeftDoc,
    data: *const u8,
    len: usize,
) -> WeftStatus {
    let Some(doc) = doc.as_mut() else {
        return WeftStatus::InvalidArgument;
    };
    let Some(bytes) = bytes_arg(data, len) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| match doc.inner.load_incremental(bytes) {
        Ok(_) => WeftStatus::Ok,
        Err(_) => WeftStatus::CorruptData,
    })
}

/// Returns 1 if the document contains the change with this 32-byte hash, 0 if not, -1 on error.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_has_change(doc: *mut WeftDoc, hash: *const u8) -> i32 {
    let Some(doc) = doc.as_mut() else { return -1 };
    if hash.is_null() {
        return -1;
    }
    let Ok(h) = ChangeHash::try_from(slice::from_raw_parts(hash, 32)) else {
        return -1;
    };
    catch_unwind(AssertUnwindSafe(|| {
        i32::from(doc.inner.get_change_by_hash(&h).is_some())
    }))
    .unwrap_or(-1)
}

/// Lists the paths touched between `heads` and the current state, each truncated
/// to `depth` segments, as a codec list of lists of strings/ints.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_changed_paths(
    doc: *mut WeftDoc,
    heads: *const u8,
    heads_len: usize,
    depth: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    let Some(raw) = bytes_arg(heads, heads_len) else {
        return WeftStatus::InvalidArgument;
    };
    let Ok(before) = ops::parse_heads(raw) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| {
        *out = WeftBytes::from_vec(ops::changed_paths(&mut doc.inner, &before, depth));
        WeftStatus::Ok
    })
}

/// A new document holding the state at `heads` (concatenated 32-byte hashes),
/// for read-only previews of a version (spec 05 §9.3). Null if a head is unknown.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_fork_at(
    doc: *mut WeftDoc,
    heads: *const u8,
    heads_len: usize,
) -> *mut WeftDoc {
    let Some(doc) = doc.as_mut() else {
        return ptr::null_mut();
    };
    let Some(raw) = bytes_arg(heads, heads_len) else {
        return ptr::null_mut();
    };
    let Ok(at) = ops::parse_heads(raw) else {
        return ptr::null_mut();
    };
    catch_unwind(AssertUnwindSafe(|| match doc.inner.fork_at(&at) {
        Ok(inner) => Box::into_raw(Box::new(WeftDoc { inner })),
        Err(_) => ptr::null_mut(),
    }))
    .unwrap_or(ptr::null_mut())
}

/// Merges all changes from `other` into `doc`. `other`'s pending operations are committed first.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_merge(doc: *mut WeftDoc, other: *mut WeftDoc) -> WeftStatus {
    let (Some(doc), Some(other)) = (doc.as_mut(), other.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| match doc.inner.merge(&mut other.inner) {
        Ok(_) => WeftStatus::Ok,
        Err(_) => WeftStatus::CorruptData,
    })
}

/// Releases a buffer returned by this library. Safe to call on an empty buffer.
#[no_mangle]
pub unsafe extern "C" fn weft_bytes_free(bytes: WeftBytes) {
    if !bytes.data.is_null() {
        drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
            bytes.data, bytes.len,
        )));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use codec::{Segment, Value, Writer};

    fn path(segs: &[&str]) -> Vec<u8> {
        let mut out = Vec::new();
        for s in segs {
            out.push(0);
            out.extend_from_slice(&(s.len() as u32).to_le_bytes());
            out.extend_from_slice(s.as_bytes());
        }
        out
    }

    fn enc(v: &Value) -> Vec<u8> {
        let mut w = Writer::default();
        w.value(v);
        w.buf
    }

    unsafe fn get(doc: *mut WeftDoc, segs: &[&str]) -> Option<Value> {
        let p = path(segs);
        let mut out = WeftBytes::empty();
        if weft_doc_get(doc, p.as_ptr(), p.len(), &mut out) != WeftStatus::Ok {
            return None;
        }
        let v = codec::decode_value(slice::from_raw_parts(out.data, out.len)).unwrap();
        weft_bytes_free(out);
        Some(v)
    }

    #[test]
    fn save_load_round_trip() {
        unsafe {
            let a = weft_doc_new();
            let p = path(&["name"]);
            let v = enc(&Value::Str("Song".into()));
            assert_eq!(
                weft_doc_put(a, p.as_ptr(), p.len(), v.as_ptr(), v.len()),
                WeftStatus::Ok
            );
            assert_eq!(weft_doc_commit(a, ptr::null(), 0, 0), 1);
            let mut bytes = WeftBytes::empty();
            assert_eq!(weft_doc_save(a, &mut bytes), WeftStatus::Ok);
            let b = weft_doc_load(bytes.data, bytes.len);
            assert!(!b.is_null());
            assert_eq!(get(b, &["name"]), Some(Value::Str("Song".into())));
            weft_bytes_free(bytes);
            weft_doc_free(a);
            weft_doc_free(b);
        }
    }

    #[test]
    fn corrupt_load_returns_null() {
        unsafe {
            let junk = [1u8, 2, 3, 4, 5];
            assert!(weft_doc_load(junk.as_ptr(), junk.len()).is_null());
        }
    }

    #[test]
    fn commit_rollback_and_replay() {
        unsafe {
            let a = weft_doc_new();
            let p = path(&["x"]);
            let v = enc(&Value::Int(1));
            weft_doc_put(a, p.as_ptr(), p.len(), v.as_ptr(), v.len());
            assert_eq!(weft_doc_rollback(a), 1);
            assert_eq!(get(a, &["x"]), None);
            assert_eq!(weft_doc_commit(a, ptr::null(), 0, 0), 0);

            weft_doc_put(a, p.as_ptr(), p.len(), v.as_ptr(), v.len());
            assert_eq!(weft_doc_commit(a, b"edit".as_ptr(), 4, 1000), 1);

            let mut all = WeftBytes::empty();
            let mut count = 0usize;
            assert_eq!(
                weft_doc_all_changes(a, &mut all, &mut count),
                WeftStatus::Ok
            );
            assert_eq!(count, 1);
            let b = weft_doc_new();
            assert_eq!(weft_doc_apply(b, all.data, all.len), WeftStatus::Ok);
            assert_eq!(get(b, &["x"]), Some(Value::Int(1)));

            let mut heads = WeftBytes::empty();
            weft_doc_heads(a, &mut heads);
            assert_eq!(heads.len, 32);
            assert_eq!(weft_doc_has_change(b, heads.data), 1);
            weft_bytes_free(heads);
            weft_bytes_free(all);
            weft_doc_free(a);
            weft_doc_free(b);
        }
    }

    #[test]
    fn segment_type_is_exposed() {
        // Keeps the by-id segment variant exercised from the FFI side too.
        assert_ne!(Segment::ById("a".into()), Segment::Index(0));
    }
}

// ---------------------------------------------------------------------------
// Sync (spec 05 §3.3): Automerge's sync protocol, one state per connection.

/// Opaque per-peer sync state; free with `weft_sync_state_free`.
pub struct WeftSyncState(automerge::sync::State);

#[no_mangle]
pub extern "C" fn weft_sync_state_new() -> *mut WeftSyncState {
    Box::into_raw(Box::new(WeftSyncState(automerge::sync::State::new())))
}

#[no_mangle]
pub unsafe extern "C" fn weft_sync_state_free(state: *mut WeftSyncState) {
    if !state.is_null() {
        drop(Box::from_raw(state));
    }
}

/// Writes the next sync message for the peer, or returns `NotFound` when there is nothing to send.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_sync_generate(
    doc: *mut WeftDoc,
    state: *mut WeftSyncState,
    out: *mut WeftBytes,
) -> WeftStatus {
    let (Some(doc), Some(state), Some(out)) = (doc.as_mut(), state.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| {
        use automerge::sync::SyncDoc;
        match doc.inner.sync().generate_sync_message(&mut state.0) {
            Some(m) => {
                *out = WeftBytes::from_vec(m.encode());
                WeftStatus::Ok
            }
            None => WeftStatus::NotFound,
        }
    })
}

/// Applies a sync message from the peer (changes in it are verified and applied).
#[no_mangle]
pub unsafe extern "C" fn weft_doc_sync_receive(
    doc: *mut WeftDoc,
    state: *mut WeftSyncState,
    data: *const u8,
    len: usize,
) -> WeftStatus {
    let (Some(doc), Some(state)) = (doc.as_mut(), state.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    let Some(bytes) = bytes_arg(data, len) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| {
        use automerge::sync::SyncDoc;
        let Ok(message) = automerge::sync::Message::decode(bytes) else {
            return WeftStatus::CorruptData;
        };
        match doc.inner.sync().receive_sync_message(&mut state.0, message) {
            Ok(()) => WeftStatus::Ok,
            Err(_) => WeftStatus::CorruptData,
        }
    })
}

/// Describes the changes made since `heads` (all changes for empty heads) as a codec list of
/// maps `{hash: bytes, actor: str (hex), message: str, time: int}`, in causal order.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_changes_info(
    doc: *mut WeftDoc,
    heads: *const u8,
    heads_len: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let (Some(doc), Some(out)) = (doc.as_mut(), out.as_mut()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    let Some(raw) = bytes_arg(heads, heads_len) else {
        return WeftStatus::InvalidArgument;
    };
    let Ok(before) = ops::parse_heads(raw) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| {
        let list = doc
            .inner
            .get_changes(&before)
            .iter()
            .map(|c| {
                codec::Value::Map(vec![
                    (
                        "actor".into(),
                        codec::Value::Str(c.actor_id().to_hex_string()),
                    ),
                    ("hash".into(), codec::Value::Bytes(c.hash().0.to_vec())),
                    (
                        "message".into(),
                        codec::Value::Str(c.message().unwrap_or_default().to_string()),
                    ),
                    ("time".into(), codec::Value::Int(c.timestamp())),
                ])
            })
            .collect();
        let mut w = codec::Writer::default();
        w.value(&codec::Value::List(list));
        *out = WeftBytes::from_vec(w.buf);
        WeftStatus::Ok
    })
}

/// Writes the 32-byte hash of one encoded change chunk to `out32`.
#[no_mangle]
pub unsafe extern "C" fn weft_change_hash(
    data: *const u8,
    len: usize,
    out32: *mut u8,
) -> WeftStatus {
    let Some(bytes) = bytes_arg(data, len) else {
        return WeftStatus::InvalidArgument;
    };
    if out32.is_null() {
        return WeftStatus::InvalidArgument;
    }
    guard(|| match automerge::Change::from_bytes(bytes.to_vec()) {
        Ok(c) => {
            ptr::copy_nonoverlapping(c.hash().0.as_ptr(), out32, 32);
            WeftStatus::Ok
        }
        Err(_) => WeftStatus::CorruptData,
    })
}

// ---------------------------------------------------------------------------
// BLAKE3 content hashing for media blobs (spec 03 §4.5, 05 §8). Lives here so
// the client links one Rust library.

/// Opaque incremental hasher; free with `weft_hasher_free`.
pub struct WeftHasher(blake3::Hasher);

#[no_mangle]
pub extern "C" fn weft_hasher_new() -> *mut WeftHasher {
    Box::into_raw(Box::new(WeftHasher(blake3::Hasher::new())))
}

#[no_mangle]
pub unsafe extern "C" fn weft_hasher_update(
    hasher: *mut WeftHasher,
    data: *const u8,
    len: usize,
) -> WeftStatus {
    if hasher.is_null() || (data.is_null() && len > 0) {
        return WeftStatus::InvalidArgument;
    }
    if len > 0 {
        (*hasher).0.update(slice::from_raw_parts(data, len));
    }
    WeftStatus::Ok
}

/// Writes the 64 lowercase hex digits of the hash (no terminator) to `out_hex`.
#[no_mangle]
pub unsafe extern "C" fn weft_hasher_finish_hex(
    hasher: *const WeftHasher,
    out_hex: *mut u8,
) -> WeftStatus {
    if hasher.is_null() || out_hex.is_null() {
        return WeftStatus::InvalidArgument;
    }
    let hex = (*hasher).0.finalize().to_hex();
    ptr::copy_nonoverlapping(hex.as_bytes().as_ptr(), out_hex, 64);
    WeftStatus::Ok
}

#[no_mangle]
pub unsafe extern "C" fn weft_hasher_free(hasher: *mut WeftHasher) {
    if !hasher.is_null() {
        drop(Box::from_raw(hasher));
    }
}

#[cfg(test)]
mod sync_tests {
    use super::*;

    fn put_str(doc: *mut WeftDoc, key: &str, v: &str) {
        let mut p = codec::Writer::default();
        p.tag(0);
        p.raw_str(key);
        let mut val = codec::Writer::default();
        val.value(&codec::Value::Str(v.into()));
        unsafe {
            assert_eq!(
                weft_doc_put(
                    doc,
                    p.buf.as_ptr(),
                    p.buf.len(),
                    val.buf.as_ptr(),
                    val.buf.len()
                ),
                WeftStatus::Ok
            );
            assert!(weft_doc_commit(doc, b"m".as_ptr(), 1, 0) > 0);
        }
    }

    fn heads(doc: *mut WeftDoc) -> Vec<u8> {
        let mut out = WeftBytes::empty();
        unsafe {
            weft_doc_heads(doc, &mut out);
            let v = slice::from_raw_parts(out.data, out.len).to_vec();
            weft_bytes_free(out);
            v
        }
    }

    #[test]
    fn two_documents_converge_through_sync_messages() {
        let a = weft_doc_new();
        let b = weft_doc_new();
        put_str(a, "x", "from a");
        put_str(b, "y", "from b");
        let (sa, sb) = (weft_sync_state_new(), weft_sync_state_new());
        unsafe {
            for _ in 0..10 {
                let mut progressed = false;
                for (from, state_from, to, state_to) in [(a, sa, b, sb), (b, sb, a, sa)] {
                    let mut m = WeftBytes::empty();
                    if weft_doc_sync_generate(from, state_from, &mut m) == WeftStatus::Ok {
                        progressed = true;
                        assert_eq!(
                            weft_doc_sync_receive(to, state_to, m.data, m.len),
                            WeftStatus::Ok
                        );
                        weft_bytes_free(m);
                    }
                }
                if !progressed {
                    break;
                }
            }
            assert_eq!(heads(a), heads(b));
            let mut info = WeftBytes::empty();
            assert_eq!(
                weft_doc_changes_info(a, ptr::null(), 0, &mut info),
                WeftStatus::Ok
            );
            let v = codec::decode_value(slice::from_raw_parts(info.data, info.len)).unwrap();
            weft_bytes_free(info);
            let codec::Value::List(items) = v else {
                panic!()
            };
            assert_eq!(items.len(), 2);

            let mut last = WeftBytes::empty();
            assert_eq!(weft_doc_last_local_change(a, &mut last), WeftStatus::Ok);
            let mut hash = [0u8; 32];
            assert_eq!(
                weft_change_hash(last.data, last.len, hash.as_mut_ptr()),
                WeftStatus::Ok
            );
            weft_bytes_free(last);
            assert_eq!(weft_doc_has_change(b, hash.as_ptr()), 1);
            weft_sync_state_free(sa);
            weft_sync_state_free(sb);
            weft_doc_free(a);
            weft_doc_free(b);
        }
    }
}

#[cfg(test)]
mod hash_tests {
    use super::*;

    #[test]
    fn hasher_matches_known_vector() {
        // BLAKE3 of the empty input.
        let h = weft_hasher_new();
        let mut out = [0u8; 64];
        unsafe {
            assert_eq!(weft_hasher_update(h, ptr::null(), 0), WeftStatus::Ok);
            assert_eq!(weft_hasher_finish_hex(h, out.as_mut_ptr()), WeftStatus::Ok);
            weft_hasher_free(h);
        }
        assert_eq!(
            std::str::from_utf8(&out).unwrap(),
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
        );
    }
}
