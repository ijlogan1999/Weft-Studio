//! MP3 decoding over Symphonia, with gapless trimming from the LAME/Info header. Weft
//! uses it to import MP3s, and to check its own bounces against an independent decoder.

use crate::{set_error, text};
use std::ffi::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::{MetadataOptions, StandardTagKey};
use symphonia::core::probe::Hint;

/// Decoded audio, interleaved. Free with `weft_mp3_free`.
#[repr(C)]
pub struct WeftMp3Decoded {
    pub samples: *mut f32,
    pub frames: usize,
    pub channels: c_int,
    pub sample_rate: c_int,
    pub title: [c_char; 256],
    pub artist: [c_char; 256],
}

struct Decoded {
    samples: Vec<f32>,
    channels: usize,
    rate: u32,
    title: String,
    artist: String,
}

fn decode(path: &str) -> Result<Decoded, String> {
    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let stream = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    hint.with_extension("mp3");
    let format_options = FormatOptions {
        enable_gapless: true,
        ..Default::default()
    };
    let mut probed = symphonia::default::get_probe()
        .format(&hint, stream, &format_options, &MetadataOptions::default())
        .map_err(|e| e.to_string())?;
    let mut title = String::new();
    let mut artist = String::new();
    let mut read_tags = |rev: &symphonia::core::meta::MetadataRevision| {
        for tag in rev.tags() {
            match tag.std_key {
                Some(StandardTagKey::TrackTitle) => title = tag.value.to_string(),
                Some(StandardTagKey::Artist) => artist = tag.value.to_string(),
                _ => {}
            }
        }
    };
    if let Some(meta) = probed.metadata.get() {
        if let Some(rev) = meta.current() {
            read_tags(rev);
        }
    }
    let mut format = probed.format;
    if let Some(rev) = format.metadata().current() {
        read_tags(rev);
    }
    let track = format.default_track().ok_or("No audio track")?;
    let track_id = track.id;
    let params = track.codec_params.clone();
    let mut decoder = symphonia::default::get_codecs()
        .make(&params, &DecoderOptions::default())
        .map_err(|e| e.to_string())?;
    let mut samples = Vec::new();
    let mut channels = params.channels.map(|c| c.count()).unwrap_or(0);
    let mut rate = params.sample_rate.unwrap_or(0);
    loop {
        let packet = match format.next_packet() {
            Ok(p) => p,
            Err(Error::IoError(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e.to_string()),
        };
        if packet.track_id() != track_id {
            continue;
        }
        let audio = match decoder.decode(&packet) {
            Ok(a) => a,
            Err(Error::DecodeError(_)) => continue,
            Err(e) => return Err(e.to_string()),
        };
        let spec = *audio.spec();
        channels = spec.channels.count();
        rate = spec.rate;
        let mut buf = SampleBuffer::<f32>::new(audio.capacity() as u64, spec);
        buf.copy_interleaved_ref(audio);
        samples.extend_from_slice(buf.samples());
    }
    Ok(Decoded {
        samples,
        channels,
        rate,
        title,
        artist,
    })
}

fn copy_text(dest: &mut [c_char; 256], s: &str) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(dest.len() - 1);
    for (d, b) in dest.iter_mut().zip(&bytes[..n]) {
        *d = *b as c_char;
    }
    dest[n] = 0;
}

/// Decodes the MP3 at `path` (UTF-8) into `out`. 0 on success.
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_decode(
    path: *const c_char,
    out: *mut WeftMp3Decoded,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if path.is_null() || out.is_null() {
        return -1;
    }
    let path = text(path);
    match catch_unwind(AssertUnwindSafe(|| decode(&path))) {
        Ok(Ok(d)) => {
            let out = &mut *out;
            out.frames = d.samples.len().checked_div(d.channels).unwrap_or(0);
            out.channels = d.channels as c_int;
            out.sample_rate = d.rate as c_int;
            copy_text(&mut out.title, &d.title);
            copy_text(&mut out.artist, &d.artist);
            let boxed = d.samples.into_boxed_slice();
            out.samples = Box::into_raw(boxed) as *mut f32;
            0
        }
        Ok(Err(e)) => {
            set_error(err, err_len, &e);
            -1
        }
        Err(_) => {
            set_error(err, err_len, "MP3 decoder panicked");
            -1
        }
    }
}

/// Frees the samples of a successful `weft_mp3_decode`.
#[no_mangle]
pub unsafe extern "C" fn weft_mp3_free(out: *mut WeftMp3Decoded) {
    if out.is_null() || (*out).samples.is_null() {
        return;
    }
    let out = &mut *out;
    let len = out.frames * out.channels.max(0) as usize;
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
        out.samples,
        len,
    )));
    out.samples = std::ptr::null_mut();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::encode::*;
    use std::ffi::CString;

    #[test]
    fn round_trips_a_sine_gaplessly_with_tags() {
        let dir = std::env::temp_dir().join(format!("weft-codec-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = CString::new(dir.join("sine.mp3").to_str().unwrap()).unwrap();
        let title = CString::new("Sine").unwrap();
        let artist = CString::new("Weft").unwrap();
        let settings = WeftMp3Settings {
            sample_rate: 44100,
            channels: 2,
            vbr: 0,
            bitrate_kbps: 320,
            vbr_quality: 2,
            title: title.as_ptr(),
            artist: artist.as_ptr(),
            album: std::ptr::null(),
            year: std::ptr::null(),
        };
        let frames = 44100usize;
        let sine: Vec<f32> = (0..frames)
            .map(|i| 0.5 * (2.0 * std::f32::consts::PI * 1000.0 * i as f32 / 44100.0).sin())
            .collect();
        unsafe {
            let mut err = [0 as c_char; 256];
            let enc = weft_mp3_open(path.as_ptr(), &settings, err.as_mut_ptr(), err.len());
            assert!(!enc.is_null());
            assert_eq!(
                weft_mp3_write(
                    enc,
                    sine.as_ptr(),
                    sine.as_ptr(),
                    frames,
                    err.as_mut_ptr(),
                    err.len()
                ),
                0
            );
            assert_eq!(weft_mp3_finish(enc, err.as_mut_ptr(), err.len()), 0);
            let mut out: WeftMp3Decoded = std::mem::zeroed();
            assert_eq!(
                weft_mp3_decode(path.as_ptr(), &mut out, err.as_mut_ptr(), err.len()),
                0
            );
            assert_eq!(out.channels, 2);
            assert_eq!(out.sample_rate, 44100);
            assert_eq!(out.frames, frames);
            assert_eq!(CStr::from_ptr(out.title.as_ptr()).to_str().unwrap(), "Sine");
            assert_eq!(
                CStr::from_ptr(out.artist.as_ptr()).to_str().unwrap(),
                "Weft"
            );
            weft_mp3_free(&mut out);
        }
        std::fs::remove_dir_all(&dir).ok();
    }

    use std::ffi::CStr;
}
