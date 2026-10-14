//! C ABI over Automerge for the Weft Studio client.
//!
//! The client and the relay share one Automerge implementation: the relay links
//! the `automerge` crate directly and the C++ client links this crate as a
//! static library. The API is deliberately small and grows milestone by
//! milestone; `include/weft_crdt.h` is the matching C header.
//!
//! Conventions:
//! - Every function returns a `WeftStatus` (0 = ok) unless it is a constructor
//!   (null = failure) or a destructor.
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

use automerge::{
    transaction::Transactable, ActorId, AutoCommit, ReadDoc, ScalarValue, Value, ROOT,
};
use std::os::raw::c_char;
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

/// An owned byte buffer returned to C. Release with `weft_bytes_free`.
#[repr(C)]
pub struct WeftBytes {
    pub data: *mut u8,
    pub len: usize,
}

impl WeftBytes {
    fn empty() -> Self {
        WeftBytes {
            data: ptr::null_mut(),
            len: 0,
        }
    }

    fn from_vec(v: Vec<u8>) -> Self {
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

unsafe fn str_arg<'a>(p: *const c_char, len: usize) -> Option<&'a str> {
    if p.is_null() && len != 0 {
        return None;
    }
    let bytes = if len == 0 {
        &[][..]
    } else {
        slice::from_raw_parts(p as *const u8, len)
    };
    std::str::from_utf8(bytes).ok()
}

unsafe fn doc_mut<'a>(doc: *mut WeftDoc) -> Option<&'a mut WeftDoc> {
    doc.as_mut()
}

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
    if data.is_null() && len != 0 {
        return ptr::null_mut();
    }
    let bytes = if len == 0 {
        &[][..]
    } else {
        slice::from_raw_parts(data, len)
    };
    catch_unwind(AssertUnwindSafe(|| match AutoCommit::load(bytes) {
        Ok(inner) => Box::into_raw(Box::new(WeftDoc { inner })),
        Err(_) => ptr::null_mut(),
    }))
    .unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn weft_doc_free(doc: *mut WeftDoc) {
    if !doc.is_null() {
        drop(Box::from_raw(doc));
    }
}

/// Serializes the whole document (compressed Automerge format).
#[no_mangle]
pub unsafe extern "C" fn weft_doc_save(doc: *mut WeftDoc, out: *mut WeftBytes) -> WeftStatus {
    let (Some(doc), false) = (doc_mut(doc), out.is_null()) else {
        return WeftStatus::InvalidArgument;
    };
    *out = WeftBytes::empty();
    guard(|| {
        *out = WeftBytes::from_vec(doc.inner.save());
        WeftStatus::Ok
    })
}

/// Sets a string value on the root map.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_put_str(
    doc: *mut WeftDoc,
    key: *const c_char,
    key_len: usize,
    value: *const c_char,
    value_len: usize,
) -> WeftStatus {
    let Some(doc) = doc_mut(doc) else {
        return WeftStatus::InvalidArgument;
    };
    let (Some(key), Some(value)) = (str_arg(key, key_len), str_arg(value, value_len)) else {
        return WeftStatus::InvalidArgument;
    };
    guard(|| match doc.inner.put(ROOT, key, value) {
        Ok(()) => WeftStatus::Ok,
        Err(_) => WeftStatus::Internal,
    })
}

/// Reads a string value from the root map into a caller-owned buffer.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_get_str(
    doc: *mut WeftDoc,
    key: *const c_char,
    key_len: usize,
    out: *mut WeftBytes,
) -> WeftStatus {
    let Some(doc) = doc_mut(doc) else {
        return WeftStatus::InvalidArgument;
    };
    let Some(key) = str_arg(key, key_len) else {
        return WeftStatus::InvalidArgument;
    };
    if out.is_null() {
        return WeftStatus::InvalidArgument;
    }
    *out = WeftBytes::empty();
    guard(|| match doc.inner.get(ROOT, key) {
        Ok(Some((Value::Scalar(s), _))) => match s.as_ref() {
            ScalarValue::Str(text) => {
                *out = WeftBytes::from_vec(text.as_bytes().to_vec());
                WeftStatus::Ok
            }
            _ => WeftStatus::WrongType,
        },
        Ok(Some(_)) => WeftStatus::WrongType,
        Ok(None) => WeftStatus::NotFound,
        Err(_) => WeftStatus::Internal,
    })
}

/// Merges all changes from `other` into `doc`. `other` is not modified.
#[no_mangle]
pub unsafe extern "C" fn weft_doc_merge(doc: *mut WeftDoc, other: *mut WeftDoc) -> WeftStatus {
    let (Some(doc), Some(other)) = (doc_mut(doc), doc_mut(other)) else {
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

    unsafe fn put(doc: *mut WeftDoc, k: &str, v: &str) {
        assert_eq!(
            weft_doc_put_str(doc, k.as_ptr().cast(), k.len(), v.as_ptr().cast(), v.len()),
            WeftStatus::Ok
        );
    }

    unsafe fn get(doc: *mut WeftDoc, k: &str) -> Option<String> {
        let mut out = WeftBytes::empty();
        let st = weft_doc_get_str(doc, k.as_ptr().cast(), k.len(), &mut out);
        if st != WeftStatus::Ok {
            return None;
        }
        let s = String::from_utf8(slice::from_raw_parts(out.data, out.len).to_vec()).unwrap();
        weft_bytes_free(out);
        Some(s)
    }

    #[test]
    fn save_load_round_trip() {
        unsafe {
            let a = weft_doc_new();
            put(a, "name", "Song");
            let mut bytes = WeftBytes::empty();
            assert_eq!(weft_doc_save(a, &mut bytes), WeftStatus::Ok);
            let b = weft_doc_load(bytes.data, bytes.len);
            assert!(!b.is_null());
            assert_eq!(get(b, "name").as_deref(), Some("Song"));
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
}
