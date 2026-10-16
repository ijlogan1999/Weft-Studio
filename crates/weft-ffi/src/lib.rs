//! Bundles the C ABIs of weft-crdt and weft-codec into one static library. Two Rust
//! static libraries in one binary would each carry the Rust runtime and clash at link
//! time, so the client links only this one.

pub use weft_codec;
pub use weft_crdt;
