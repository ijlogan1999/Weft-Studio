//! Path-based document operations on top of Automerge. The FFI layer in
//! `lib.rs` is a thin wrapper around these.

use crate::codec::{Segment, Value, Writer, TAG_LIST, TAG_MAP, TAG_NULL};
use automerge::{
    transaction::Transactable, AutoCommit, ChangeHash, ObjId, ObjType, PatchAction, Prop, ReadDoc,
    ScalarValue, ScalarValueRef, ValueRef, ROOT,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpError {
    InvalidArgument,
    NotFound,
    WrongType,
    Internal,
}

impl From<automerge::AutomergeError> for OpError {
    fn from(_: automerge::AutomergeError) -> Self {
        OpError::Internal
    }
}

pub type OpResult<T> = Result<T, OpError>;

/// Finds the index of the list element equal to `id`, or whose map has `"id": id`.
fn find_by_id(doc: &AutoCommit, list: &ObjId, id: &str) -> OpResult<Option<usize>> {
    for item in doc.list_range(list, ..) {
        match &item.value {
            ValueRef::Scalar(ScalarValueRef::Str(s)) if s.as_ref() == id => {
                return Ok(Some(item.index))
            }
            ValueRef::Object(ObjType::Map) => {
                let obj = item.id();
                if let Some((automerge::Value::Scalar(s), _)) = doc.get(&obj, "id")? {
                    if let ScalarValue::Str(text) = s.as_ref() {
                        if text.as_str() == id {
                            return Ok(Some(item.index));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    Ok(None)
}

/// Turns a segment into a prop on `obj`, resolving by-id lookups.
fn prop_for(doc: &AutoCommit, obj: &ObjId, seg: &Segment) -> OpResult<Prop> {
    let ty = doc.object_type(obj).map_err(|_| OpError::NotFound)?;
    match (seg, ty) {
        (Segment::Key(k), ObjType::Map | ObjType::Table) => Ok(Prop::Map(k.clone())),
        (Segment::Index(i), ObjType::List) => Ok(Prop::Seq(*i)),
        (Segment::ById(id), ObjType::List) => find_by_id(doc, obj, id)?
            .map(Prop::Seq)
            .ok_or(OpError::NotFound),
        _ => Err(OpError::WrongType),
    }
}

/// Resolves a path that names an object (map or list).
pub fn resolve_obj(doc: &AutoCommit, path: &[Segment]) -> OpResult<ObjId> {
    let mut cur = ROOT;
    for seg in path {
        let prop = prop_for(doc, &cur, seg)?;
        match doc.get(&cur, prop)? {
            Some((automerge::Value::Object(_), id)) => cur = id,
            Some(_) => return Err(OpError::WrongType),
            None => return Err(OpError::NotFound),
        }
    }
    Ok(cur)
}

fn split_last(path: &[Segment]) -> OpResult<(&[Segment], &Segment)> {
    match path.split_last() {
        Some((last, parent)) => Ok((parent, last)),
        None => Err(OpError::InvalidArgument),
    }
}

fn scalar(v: &Value) -> Option<ScalarValue> {
    Some(match v {
        Value::Null => ScalarValue::Null,
        Value::Bool(b) => ScalarValue::Boolean(*b),
        Value::Int(i) => ScalarValue::Int(*i),
        Value::F64(f) => ScalarValue::F64(*f),
        Value::Str(s) => ScalarValue::Str(s.as_str().into()),
        Value::Bytes(b) => ScalarValue::Bytes(b.clone()),
        Value::Map(_) | Value::List(_) => return None,
    })
}

fn fill(doc: &mut AutoCommit, obj: &ObjId, v: &Value) -> OpResult<()> {
    match v {
        Value::Map(entries) => {
            for (k, child) in entries {
                put_prop(doc, obj, Prop::Map(k.clone()), child)?;
            }
        }
        Value::List(items) => {
            for (i, child) in items.iter().enumerate() {
                insert_at(doc, obj, i, child)?;
            }
        }
        _ => {}
    }
    Ok(())
}

fn put_prop(doc: &mut AutoCommit, obj: &ObjId, prop: Prop, v: &Value) -> OpResult<()> {
    match v {
        Value::Map(_) => {
            let child = doc.put_object(obj, prop, ObjType::Map)?;
            fill(doc, &child, v)
        }
        Value::List(_) => {
            let child = doc.put_object(obj, prop, ObjType::List)?;
            fill(doc, &child, v)
        }
        _ => Ok(doc.put(obj, prop, scalar(v).unwrap())?),
    }
}

fn insert_at(doc: &mut AutoCommit, obj: &ObjId, index: usize, v: &Value) -> OpResult<()> {
    match v {
        Value::Map(_) => {
            let child = doc.insert_object(obj, index, ObjType::Map)?;
            fill(doc, &child, v)
        }
        Value::List(_) => {
            let child = doc.insert_object(obj, index, ObjType::List)?;
            fill(doc, &child, v)
        }
        _ => Ok(doc.insert(obj, index, scalar(v).unwrap())?),
    }
}

/// Sets the value at `path`. Maps and lists in `v` create new objects. The
/// parent object must exist. A list index must be in range.
pub fn put(doc: &mut AutoCommit, path: &[Segment], v: &Value) -> OpResult<()> {
    let (parent, last) = split_last(path)?;
    let obj = resolve_obj(doc, parent)?;
    let prop = prop_for(doc, &obj, last)?;
    if let Prop::Seq(i) = prop {
        if i >= doc.length(&obj) {
            return Err(OpError::NotFound);
        }
    }
    put_prop(doc, &obj, prop, v)
}

/// Inserts `v` into the list at `path`'s parent, at the index given by the last
/// segment (equal to the length appends).
pub fn insert(doc: &mut AutoCommit, path: &[Segment], v: &Value) -> OpResult<()> {
    let (parent, last) = split_last(path)?;
    let obj = resolve_obj(doc, parent)?;
    if doc.object_type(&obj).map_err(|_| OpError::NotFound)? != ObjType::List {
        return Err(OpError::WrongType);
    }
    let Segment::Index(i) = last else {
        return Err(OpError::InvalidArgument);
    };
    if *i > doc.length(&obj) {
        return Err(OpError::NotFound);
    }
    insert_at(doc, &obj, *i, v)
}

/// Deletes a map key or list element. Missing keys are NotFound.
pub fn delete(doc: &mut AutoCommit, path: &[Segment]) -> OpResult<()> {
    let (parent, last) = split_last(path)?;
    let obj = resolve_obj(doc, parent)?;
    let prop = prop_for(doc, &obj, last)?;
    if doc.get(&obj, prop.clone())?.is_none() {
        return Err(OpError::NotFound);
    }
    Ok(doc.delete(&obj, prop)?)
}

fn write_scalar(w: &mut Writer, s: &ScalarValue) {
    match s {
        ScalarValue::Bytes(b) => w.bytes(b),
        ScalarValue::Str(t) => w.str(t),
        ScalarValue::Int(i) | ScalarValue::Timestamp(i) => w.int(*i),
        ScalarValue::Uint(u) => w.int(*u as i64),
        ScalarValue::F64(f) => w.f64(*f),
        ScalarValue::Counter(c) => w.int(i64::from(c)),
        ScalarValue::Boolean(b) => w.value(&Value::Bool(*b)),
        ScalarValue::Null | ScalarValue::Unknown { .. } => w.tag(TAG_NULL),
    }
}

fn write_scalar_ref(w: &mut Writer, s: &ScalarValueRef) {
    match s {
        ScalarValueRef::Bytes(b) => w.bytes(b),
        ScalarValueRef::Str(t) => w.str(t),
        ScalarValueRef::Int(i) | ScalarValueRef::Timestamp(i) | ScalarValueRef::Counter(i) => {
            w.int(*i)
        }
        ScalarValueRef::Uint(u) => w.int(*u as i64),
        ScalarValueRef::F64(f) => w.f64(*f),
        ScalarValueRef::Boolean(b) => w.value(&Value::Bool(*b)),
        ScalarValueRef::Null | ScalarValueRef::Unknown { .. } => w.tag(TAG_NULL),
    }
}

/// Writes the object `obj` and everything below it.
fn export_obj(doc: &AutoCommit, obj: &ObjId, ty: ObjType, w: &mut Writer) {
    match ty {
        ObjType::Map | ObjType::Table => {
            w.tag(TAG_MAP);
            let count_at = w.buf.len();
            w.u32(0);
            let mut n = 0u32;
            for item in doc.map_range(obj, ..) {
                w.raw_str(&item.key);
                match &item.value {
                    ValueRef::Object(child_ty) => export_obj(doc, &item.id(), *child_ty, w),
                    ValueRef::Scalar(s) => write_scalar_ref(w, s),
                }
                n += 1;
            }
            w.buf[count_at..count_at + 4].copy_from_slice(&n.to_le_bytes());
        }
        ObjType::List => {
            w.tag(TAG_LIST);
            let count_at = w.buf.len();
            w.u32(0);
            let mut n = 0u32;
            for item in doc.list_range(obj, ..) {
                match &item.value {
                    ValueRef::Object(child_ty) => export_obj(doc, &item.id(), *child_ty, w),
                    ValueRef::Scalar(s) => write_scalar_ref(w, s),
                }
                n += 1;
            }
            w.buf[count_at..count_at + 4].copy_from_slice(&n.to_le_bytes());
        }
        ObjType::Text => match doc.text(obj) {
            Ok(t) => w.str(&t),
            Err(_) => w.tag(TAG_NULL),
        },
    }
}

/// Encodes the value at `path` (the whole document for an empty path).
pub fn get(doc: &AutoCommit, path: &[Segment]) -> OpResult<Vec<u8>> {
    let mut w = Writer::default();
    if path.is_empty() {
        export_obj(doc, &ROOT, ObjType::Map, &mut w);
        return Ok(w.buf);
    }
    let (parent, last) = split_last(path)?;
    let obj = resolve_obj(doc, parent)?;
    let prop = prop_for(doc, &obj, last)?;
    match doc.get(&obj, prop)? {
        Some((automerge::Value::Object(ty), id)) => export_obj(doc, &id, ty, &mut w),
        Some((automerge::Value::Scalar(s), _)) => write_scalar(&mut w, &s),
        None => return Err(OpError::NotFound),
    }
    Ok(w.buf)
}

/// The id of the operation that wrote the current (winning) value at `path`, as
/// `counter@actorhex`. For an object this is the id of the operation that created
/// it, so it identifies the object.
pub fn op_id(doc: &AutoCommit, path: &[Segment]) -> OpResult<String> {
    let (parent, last) = split_last(path)?;
    let obj = resolve_obj(doc, parent)?;
    let prop = prop_for(doc, &obj, last)?;
    match doc.get(&obj, prop)? {
        Some((_, id)) => Ok(id.to_string()),
        None => Err(OpError::NotFound),
    }
}

/// Paths (truncated to `depth` segments) touched between `before` and the current heads.
pub fn changed_paths(doc: &mut AutoCommit, before: &[ChangeHash], depth: usize) -> Vec<u8> {
    let after = doc.get_heads();
    let patches = doc.diff(before, &after);
    let mut seen: Vec<Vec<Prop>> = Vec::new();
    for p in patches {
        let mut props: Vec<Prop> = p.path.into_iter().map(|(_, prop)| prop).collect();
        match p.action {
            PatchAction::PutMap { key, .. } | PatchAction::DeleteMap { key } => {
                props.push(Prop::Map(key))
            }
            PatchAction::PutSeq { index, .. }
            | PatchAction::Insert { index, .. }
            | PatchAction::DeleteSeq { index, .. }
            | PatchAction::SpliceText { index, .. } => props.push(Prop::Seq(index)),
            PatchAction::Increment { prop, .. } | PatchAction::Conflict { prop } => {
                props.push(prop)
            }
            PatchAction::Mark { .. } => {}
        }
        props.truncate(depth);
        if !seen.contains(&props) {
            seen.push(props);
        }
    }
    let mut w = Writer::default();
    w.tag(TAG_LIST);
    w.u32(seen.len() as u32);
    for props in seen {
        w.tag(TAG_LIST);
        w.u32(props.len() as u32);
        for prop in props {
            match prop {
                Prop::Map(k) => w.str(&k),
                Prop::Seq(i) => w.int(i as i64),
            }
        }
    }
    w.buf
}

pub fn heads_bytes(heads: &[ChangeHash]) -> Vec<u8> {
    heads.iter().flat_map(|h| h.0).collect()
}

pub fn parse_heads(data: &[u8]) -> OpResult<Vec<ChangeHash>> {
    if !data.len().is_multiple_of(32) {
        return Err(OpError::InvalidArgument);
    }
    data.chunks(32)
        .map(|c| ChangeHash::try_from(c).map_err(|_| OpError::InvalidArgument))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::decode_value;

    fn k(s: &str) -> Segment {
        Segment::Key(s.into())
    }

    #[test]
    fn put_get_nested_and_by_id() {
        let mut doc = AutoCommit::new();
        let sends = Value::List(vec![
            Value::Map(vec![
                ("id".into(), Value::Str("S1".into())),
                ("level".into(), Value::F64(0.0)),
            ]),
            Value::Map(vec![
                ("id".into(), Value::Str("S2".into())),
                ("level".into(), Value::F64(-6.0)),
            ]),
        ]);
        put(&mut doc, &[k("sends")], &sends).unwrap();
        put(
            &mut doc,
            &[k("sends"), Segment::ById("S2".into()), k("level")],
            &Value::F64(-12.0),
        )
        .unwrap();
        let got = decode_value(&get(&doc, &[k("sends"), Segment::Index(1), k("level")]).unwrap())
            .unwrap();
        assert_eq!(got, Value::F64(-12.0));
        assert_eq!(
            get(&doc, &[k("sends"), Segment::ById("S9".into())]),
            Err(OpError::NotFound)
        );
    }

    #[test]
    fn insert_and_delete_in_lists() {
        let mut doc = AutoCommit::new();
        put(&mut doc, &[k("order")], &Value::List(vec![])).unwrap();
        insert(
            &mut doc,
            &[k("order"), Segment::Index(0)],
            &Value::Str("A".into()),
        )
        .unwrap();
        insert(
            &mut doc,
            &[k("order"), Segment::Index(1)],
            &Value::Str("C".into()),
        )
        .unwrap();
        insert(
            &mut doc,
            &[k("order"), Segment::Index(1)],
            &Value::Str("B".into()),
        )
        .unwrap();
        assert_eq!(
            insert(&mut doc, &[k("order"), Segment::Index(9)], &Value::Null),
            Err(OpError::NotFound)
        );
        delete(&mut doc, &[k("order"), Segment::ById("A".into())]).unwrap();
        let got = decode_value(&get(&doc, &[k("order")]).unwrap()).unwrap();
        assert_eq!(
            got,
            Value::List(vec![Value::Str("B".into()), Value::Str("C".into())])
        );
    }

    #[test]
    fn changed_paths_reports_touched_entities() {
        let mut doc = AutoCommit::new();
        put(&mut doc, &[k("regions")], &Value::Map(vec![])).unwrap();
        doc.commit();
        let before = doc.get_heads();
        put(
            &mut doc,
            &[k("regions"), k("R1")],
            &Value::Map(vec![("start".into(), Value::Int(5))]),
        )
        .unwrap();
        doc.commit();
        let got = decode_value(&changed_paths(&mut doc, &before, 2)).unwrap();
        assert_eq!(
            got,
            Value::List(vec![Value::List(vec![
                Value::Str("regions".into()),
                Value::Str("R1".into())
            ])])
        );
    }
}
