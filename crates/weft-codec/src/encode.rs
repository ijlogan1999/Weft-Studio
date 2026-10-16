//! Streaming MP3 encoder over LAME.

use crate::{id3, set_error, text};
use mp3lame_sys as lame;
use std::ffi::{c_char, c_int};
use std::fs::File;
use std::io::{Seek, SeekFrom, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Encoder settings. `vbr` = 0 is CBR at `bitrate_kbps`; otherwise VBR at
/// `vbr_quality` (0 best to 9 smallest). Tags may be null or empty.
#[repr(C)]
pub struct WeftMp3Settings {
    pub sample_rate: c_int,
    pub channels: c_int,
    pub vbr: c_int,
    pub bitrate_kbps: c_int,
    pub vbr_quality: c_int,
    pub title: *const c_char,
    pub artist: *const c_char,
    pub album: *const c_char,
    pub year: *const c_char,
}

pub struct WeftMp3Encoder {
    gfp: *mut lame::lame_global_flags,
    file: File,
    channels: usize,
    tag_end: u64,
    buffer: Vec<u8>,
}

impl Drop for WeftMp3Encoder {
    fn drop(&mut self) {
        unsafe { lame::lame_close(self.gfp) };
    }
}

impl WeftMp3Encoder {
    fn open(path: &str, s: &WeftMp3Settings) -> Result<Self, String> {
        if !(1..=2).contains(&s.channels) {
            return Err("MP3 supports mono or stereo".into());
        }
        let gfp = unsafe { lame::lame_init() };
        if gfp.is_null() {
            return Err("Couldn't start the MP3 encoder".into());
        }
        // From here `enc` owns gfp and closes it on any error.
        let file = match File::create(path) {
            Ok(f) => f,
            Err(e) => {
                unsafe { lame::lame_close(gfp) };
                return Err(format!("Couldn't create the file: {e}"));
            }
        };
        let mut enc = WeftMp3Encoder {
            gfp,
            file,
            channels: s.channels as usize,
            tag_end: 0,
            buffer: Vec::new(),
        };
        unsafe {
            lame::lame_set_in_samplerate(gfp, s.sample_rate);
            lame::lame_set_out_samplerate(gfp, s.sample_rate);
            lame::lame_set_num_channels(gfp, s.channels);
            lame::lame_set_mode(
                gfp,
                if s.channels == 1 {
                    lame::MPEG_mode::MONO
                } else {
                    lame::MPEG_mode::JOINT_STEREO
                },
            );
            lame::lame_set_quality(gfp, 2);
            lame::lame_set_bWriteVbrTag(gfp, 1);
            if s.vbr != 0 {
                lame::lame_set_VBR(gfp, lame::vbr_mode::vbr_default);
                lame::lame_set_VBR_quality(gfp, s.vbr_quality.clamp(0, 9) as f32);
            } else {
                lame::lame_set_VBR(gfp, lame::vbr_mode::vbr_off);
                lame::lame_set_brate(gfp, s.bitrate_kbps);
            }
            if lame::lame_init_params(gfp) < 0 {
                return Err("The MP3 encoder doesn't support these settings".into());
            }
        }
        let tag = unsafe {
            id3::tag(&[
                (b"TIT2", &text(s.title)),
                (b"TPE1", &text(s.artist)),
                (b"TALB", &text(s.album)),
                (b"TDRC", &text(s.year)),
            ])
        };
        enc.file.write_all(&tag).map_err(|e| e.to_string())?;
        enc.tag_end = tag.len() as u64;
        Ok(enc)
    }

    fn write(&mut self, left: &[f32], right: &[f32]) -> Result<(), String> {
        const CHUNK: usize = 8192;
        for start in (0..left.len()).step_by(CHUNK) {
            let end = (start + CHUNK).min(left.len());
            let n = end - start;
            // LAME's worst case: 1.25 x samples + 7200 bytes.
            self.buffer.resize(n * 5 / 4 + 7200, 0);
            let r = if self.channels == 2 {
                &right[start..end]
            } else {
                &left[start..end]
            };
            let written = unsafe {
                lame::lame_encode_buffer_ieee_float(
                    self.gfp,
                    left[start..end].as_ptr(),
                    r.as_ptr(),
                    n as c_int,
                    self.buffer.as_mut_ptr(),
                    self.buffer.len() as c_int,
                )
            };
            if written < 0 {
                return Err(format!("MP3 encoding failed ({written})"));
            }
            self.file
                .write_all(&self.buffer[..written as usize])
                .map_err(|e| e.to_string())?;
        }
        Ok(())
    }

    fn finish(&mut self) -> Result<(), String> {
        self.buffer.resize(7200, 0);
        let written = unsafe {
            lame::lame_encode_flush(
                self.gfp,
                self.buffer.as_mut_ptr(),
                self.buffer.len() as c_int,
            )
        };
        if written < 0 {
            return Err(format!("MP3 encoding failed ({written})"));
        }
        self.file
            .write_all(&self.buffer[..written as usize])
            .map_err(|e| e.to_string())?;
        // The first frame was a placeholder for the LAME/Info header, which carries the
        // frame count and the encoder delay and padding that gapless decoders trim.
        let mut frame = vec![0u8; 2880];
        let n = unsafe { lame::lame_get_lametag_frame(self.gfp, frame.as_mut_ptr(), frame.len()) };
        if n > 0 && n <= frame.len() {
            self.file
                .seek(SeekFrom::Start(self.tag_end))
                .and_then(|_| self.file.write_all(&frame[..n]))
                .map_err(|e| e.to_string())?;
        }
        self.file.flush().map_err(|e| e.to_string())
    }
}

/// Creates `path` (UTF-8) and starts encoding. Null on failure, with the reason in `err`.
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_open(
    path: *const c_char,
    settings: *const WeftMp3Settings,
    err: *mut c_char,
    err_len: usize,
) -> *mut WeftMp3Encoder {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if path.is_null() || settings.is_null() {
            return Err("No path or settings".to_string());
        }
        WeftMp3Encoder::open(&text(path), &*settings)
    }));
    match result {
        Ok(Ok(enc)) => Box::into_raw(Box::new(enc)),
        Ok(Err(e)) => {
            set_error(err, err_len, &e);
            std::ptr::null_mut()
        }
        Err(_) => {
            set_error(err, err_len, "MP3 encoder panicked");
            std::ptr::null_mut()
        }
    }
}

/// Encodes `frames` planar float frames (`right` is ignored for mono). 0 on success.
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_write(
    enc: *mut WeftMp3Encoder,
    left: *const f32,
    right: *const f32,
    frames: usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if enc.is_null() || left.is_null() || frames == 0 {
        return if enc.is_null() { -1 } else { 0 };
    }
    let enc = &mut *enc;
    let l = std::slice::from_raw_parts(left, frames);
    let r = if right.is_null() {
        l
    } else {
        std::slice::from_raw_parts(right, frames)
    };
    match catch_unwind(AssertUnwindSafe(|| enc.write(l, r))) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            set_error(err, err_len, &e);
            -1
        }
        Err(_) => {
            set_error(err, err_len, "MP3 encoder panicked");
            -1
        }
    }
}

/// Flushes, writes the LAME/Info header, closes the file and frees the encoder. 0 on success.
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_finish(
    enc: *mut WeftMp3Encoder,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if enc.is_null() {
        return -1;
    }
    let mut enc = Box::from_raw(enc);
    match catch_unwind(AssertUnwindSafe(|| enc.finish())) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            set_error(err, err_len, &e);
            -1
        }
        Err(_) => {
            set_error(err, err_len, "MP3 encoder panicked");
            -1
        }
    }
}

/// Closes the file and frees the encoder without finishing it (the caller deletes the file).
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_abort(enc: *mut WeftMp3Encoder) {
    if !enc.is_null() {
        drop(Box::from_raw(enc));
    }
}
