//! Binary encoding shared with the C++ side (`core/src/doc/Codec.cpp`).
//!
//! All integers are little-endian.
//!
//! Value:
//! ```text
//!   0 null | 1 false | 2 true | 3 int (i64) | 4 f64 | 5 str (u32 len, utf-8)
//!   6 bytes (u32 len, data) | 7 map (u32 count, then count x (str key, value))
//!   8 list (u32 count, then count x value)
//! ```
//! Map keys are written in ascending byte order. Automerge `uint`, `counter` and
//! `timestamp` scalars are exported as int.
//!
//! Path: a sequence of segments, each `0 key (str)`, `1 index (u32)` or
//! `2 by-id (str)`. A by-id segment selects the list element that equals the
//! string or whose map has an `"id"` field equal to it.

pub const TAG_NULL: u8 = 0;
pub const TAG_FALSE: u8 = 1;
pub const TAG_TRUE: u8 = 2;
pub const TAG_INT: u8 = 3;
pub const TAG_F64: u8 = 4;
pub const TAG_STR: u8 = 5;
pub const TAG_BYTES: u8 = 6;
pub const TAG_MAP: u8 = 7;
pub const TAG_LIST: u8 = 8;

const SEG_KEY: u8 = 0;
const SEG_INDEX: u8 = 1;
const SEG_BY_ID: u8 = 2;

/// Upper bound on nesting, so hostile input can't overflow the stack.
const MAX_DEPTH: usize = 64;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Int(i64),
    F64(f64),
    Str(String),
    Bytes(Vec<u8>),
    Map(Vec<(String, Value)>),
    List(Vec<Value>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Segment {
    Key(String),
    Index(usize),
    ById(String),
}

#[derive(Debug)]
pub struct DecodeError;

struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], DecodeError> {
        let end = self.pos.checked_add(n).ok_or(DecodeError)?;
        let out = self.data.get(self.pos..end).ok_or(DecodeError)?;
        self.pos = end;
        Ok(out)
    }

    fn u8(&mut self) -> Result<u8, DecodeError> {
        Ok(self.take(1)?[0])
    }

    fn u32(&mut self) -> Result<u32, DecodeError> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn u64(&mut self) -> Result<u64, DecodeError> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }

    fn string(&mut self) -> Result<String, DecodeError> {
        let len = self.u32()? as usize;
        String::from_utf8(self.take(len)?.to_vec()).map_err(|_| DecodeError)
    }

    fn done(&self) -> bool {
        self.pos == self.data.len()
    }
}

pub fn decode_value(data: &[u8]) -> Result<Value, DecodeError> {
    let mut r = Reader { data, pos: 0 };
    let v = read_value(&mut r, 0)?;
    if !r.done() {
        return Err(DecodeError);
    }
    Ok(v)
}

fn read_value(r: &mut Reader, depth: usize) -> Result<Value, DecodeError> {
    if depth > MAX_DEPTH {
        return Err(DecodeError);
    }
    Ok(match r.u8()? {
        TAG_NULL => Value::Null,
        TAG_FALSE => Value::Bool(false),
        TAG_TRUE => Value::Bool(true),
        TAG_INT => Value::Int(r.u64()? as i64),
        TAG_F64 => Value::F64(f64::from_bits(r.u64()?)),
        TAG_STR => Value::Str(r.string()?),
        TAG_BYTES => {
            let len = r.u32()? as usize;
            Value::Bytes(r.take(len)?.to_vec())
        }
        TAG_MAP => {
            let n = r.u32()? as usize;
            let mut entries = Vec::with_capacity(n.min(4096));
            for _ in 0..n {
                let k = r.string()?;
                entries.push((k, read_value(r, depth + 1)?));
            }
            Value::Map(entries)
        }
        TAG_LIST => {
            let n = r.u32()? as usize;
            let mut items = Vec::with_capacity(n.min(4096));
            for _ in 0..n {
                items.push(read_value(r, depth + 1)?);
            }
            Value::List(items)
        }
        _ => return Err(DecodeError),
    })
}

pub fn decode_path(data: &[u8]) -> Result<Vec<Segment>, DecodeError> {
    let mut r = Reader { data, pos: 0 };
    let mut out = Vec::new();
    while !r.done() {
        out.push(match r.u8()? {
            SEG_KEY => Segment::Key(r.string()?),
            SEG_INDEX => Segment::Index(r.u32()? as usize),
            SEG_BY_ID => Segment::ById(r.string()?),
            _ => return Err(DecodeError),
        });
    }
    Ok(out)
}

/// Streaming writer used by the exporter so large documents are encoded without
/// building an intermediate tree.
#[derive(Default)]
pub struct Writer {
    pub buf: Vec<u8>,
}

impl Writer {
    pub fn tag(&mut self, t: u8) {
        self.buf.push(t);
    }

    pub fn u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn int(&mut self, v: i64) {
        self.tag(TAG_INT);
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn f64(&mut self, v: f64) {
        self.tag(TAG_F64);
        self.buf.extend_from_slice(&v.to_bits().to_le_bytes());
    }

    pub fn raw_str(&mut self, s: &str) {
        self.u32(s.len() as u32);
        self.buf.extend_from_slice(s.as_bytes());
    }

    pub fn str(&mut self, s: &str) {
        self.tag(TAG_STR);
        self.raw_str(s);
    }

    pub fn bytes(&mut self, b: &[u8]) {
        self.tag(TAG_BYTES);
        self.u32(b.len() as u32);
        self.buf.extend_from_slice(b);
    }

    pub fn value(&mut self, v: &Value) {
        match v {
            Value::Null => self.tag(TAG_NULL),
            Value::Bool(b) => self.tag(if *b { TAG_TRUE } else { TAG_FALSE }),
            Value::Int(i) => self.int(*i),
            Value::F64(f) => self.f64(*f),
            Value::Str(s) => self.str(s),
            Value::Bytes(b) => self.bytes(b),
            Value::Map(entries) => {
                self.tag(TAG_MAP);
                self.u32(entries.len() as u32);
                for (k, v) in entries {
                    self.raw_str(k);
                    self.value(v);
                }
            }
            Value::List(items) => {
                self.tag(TAG_LIST);
                self.u32(items.len() as u32);
                for v in items {
                    self.value(v);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn value_round_trip() {
        let v = Value::Map(vec![
            ("a".into(), Value::Int(-5)),
            (
                "b".into(),
                Value::List(vec![Value::F64(1.5), Value::Str("x".into()), Value::Null]),
            ),
            ("c".into(), Value::Bytes(vec![1, 2, 3])),
            ("d".into(), Value::Bool(true)),
        ]);
        let mut w = Writer::default();
        w.value(&v);
        assert_eq!(decode_value(&w.buf).unwrap(), v);
    }

    #[test]
    fn truncated_input_is_rejected() {
        let mut w = Writer::default();
        w.str("hello");
        assert!(decode_value(&w.buf[..w.buf.len() - 1]).is_err());
        assert!(decode_value(&[99]).is_err());
    }

    #[test]
    fn deep_nesting_is_rejected() {
        let mut data = vec![TAG_LIST, 1, 0, 0, 0].repeat(MAX_DEPTH + 2);
        data.push(TAG_NULL);
        assert!(decode_value(&data).is_err());
    }
}
