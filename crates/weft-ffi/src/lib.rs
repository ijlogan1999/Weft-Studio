//! Bundles the C ABIs of weft-crdt, weft-codec and weft-sync into one static library.
//! Two Rust static libraries in one binary would each carry the Rust runtime and clash
//! at link time, so the client links only this one.

pub use weft_codec;
pub use weft_crdt;
pub use weft_sync;
