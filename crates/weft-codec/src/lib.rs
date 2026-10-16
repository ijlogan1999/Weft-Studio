//! C ABI for Weft Studio's MP3 bounce (F-EXP-2): an encoder over LAME that writes an
//! ID3v2.4 tag and the LAME/Info header (for gapless decoding), and a decoder over
//! Symphonia that tests use as an independent reference (AT-EXP-3, AT-EXP-4).
//!
//! Every function catches panics, so nothing unwinds into C++.

#![allow(clippy::missing_safety_doc)]

mod decode;
mod encode;
mod id3;

use std::ffi::{c_char, CStr};

pub use decode::*;
pub use encode::*;

/// Copies `msg` into a caller buffer of `len` bytes, NUL-terminated and truncated to fit.
pub(crate) unsafe fn set_error(err: *mut c_char, len: usize, msg: &str) {
    if err.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(len - 1);
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, n);
    *err.add(n) = 0;
}

/// A borrowed C string as UTF-8; null or invalid gives an empty string.
pub(crate) unsafe fn text(s: *const c_char) -> String {
    if s.is_null() {
        return String::new();
    }
    CStr::from_ptr(s).to_string_lossy().into_owned()
}
