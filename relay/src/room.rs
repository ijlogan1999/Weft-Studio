//! Rooms (spec 05 §3.1 item 4): one per open project. A room holds the project's
//! Automerge document, loaded from the change log while anyone is connected, and
//! fans changes and presence out to the other clients.

use automerge::{AutoCommit, Change, ChangeHash, ReadDoc};
use std::collections::HashMap;
use tokio::sync::mpsc::UnboundedSender;
use weft_crdt::codec::{decode_value, Value};
use weft_proto::{Message, PresenceEntry};

use crate::store::Store;

/// A compacted snapshot every 1,000 changes or 10 minutes of activity (05 §3.4).
pub const SNAPSHOT_EVERY_CHANGES: usize = 1000;
pub const SNAPSHOT_EVERY_MS: i64 = 10 * 60 * 1000;

pub enum Outbound {
    Frame(Vec<u8>),
    /// Close the socket after the frames before it.
    Close,
}

pub struct Client {
    pub user_id: String,
    pub client_id: String,
    pub role: String,
    pub tx: UnboundedSender<Outbound>,
    pub presence: Option<Vec<u8>>,
}

impl Client {
    pub fn send(&self, m: &Message) {
        let _ = self.tx.send(Outbound::Frame(m.encode()));
    }
}

pub struct Room {
    pub project_id: String,
    pub doc: AutoCommit,
    pub clients: HashMap<u64, Client>,
    changes_since_snapshot: usize,
    first_unsnapshotted_ms: Option<i64>,
    last_seq: i64,
}

/// Paths a Viewer may change (D-COL-2).
fn viewer_may_touch(path: &[Value]) -> bool {
    matches!(path, [Value::Str(a), Value::Str(b), ..] if a == "collab" && (b == "comments" || b == "chat"))
}

/// The outcome of offering changes to a room.
pub struct Applied {
    /// Changes the room didn't have before, in causal order, now stored durably.
    pub new_changes: Vec<Change>,
}

impl Room {
    pub fn load(store: &Store, project_id: &str) -> Result<Room, String> {
        let (snapshot, changes) = store.load_log(project_id).map_err(|e| e.to_string())?;
        let mut doc = match &snapshot {
            Some((_, bytes)) => AutoCommit::load(bytes).map_err(|e| e.to_string())?,
            None => AutoCommit::new(),
        };
        for c in &changes {
            doc.load_incremental(c).map_err(|e| e.to_string())?;
        }
        let last_seq = store.change_count(project_id).map_err(|e| e.to_string())?;
        Ok(Room {
            project_id: project_id.to_string(),
            doc,
            clients: HashMap::new(),
            changes_since_snapshot: changes.len(),
            first_unsnapshotted_ms: None,
            last_seq,
        })
    }

    pub fn heads(&mut self) -> Vec<ChangeHash> {
        self.doc.get_heads()
    }

    pub fn heads_bytes(&mut self) -> Vec<u8> {
        weft_crdt::ops::heads_bytes(&self.doc.get_heads())
    }

    /// The document's `schemaVersion` (0 while the room is still empty).
    pub fn schema_version(&self) -> i64 {
        match self.doc.get(automerge::ROOT, "schemaVersion") {
            Ok(Some((automerge::Value::Scalar(s), _))) => match s.as_ref() {
                automerge::ScalarValue::Int(i) => *i,
                automerge::ScalarValue::Uint(u) => *u as i64,
                _ => 0,
            },
            _ => 0,
        }
    }

    /// Would applying `apply` to a copy of the document only touch what a Viewer may?
    pub fn viewer_allowed(
        &mut self,
        apply: impl FnOnce(&mut AutoCommit) -> Result<(), String>,
    ) -> Result<bool, String> {
        let before = self.doc.get_heads();
        let mut fork = self.doc.fork();
        apply(&mut fork)?;
        let encoded = weft_crdt::ops::changed_paths(&mut fork, &before, 2);
        let Ok(Value::List(paths)) = decode_value(&encoded) else {
            return Err("can't read the changed paths".into());
        };
        Ok(paths.iter().all(|p| match p {
            Value::List(segs) => viewer_may_touch(segs),
            _ => false,
        }))
    }

    /// Stores whatever `apply` added to the document. On a storage error the room's
    /// document is reloaded by the caller, so memory never runs ahead of the log.
    pub fn apply(
        &mut self,
        store: &mut Store,
        now_ms: i64,
        apply: impl FnOnce(&mut AutoCommit) -> Result<(), String>,
    ) -> Result<Applied, String> {
        let before = self.doc.get_heads();
        apply(&mut self.doc)?;
        let new_changes: Vec<Change> = self.doc.get_changes(&before);
        if new_changes.is_empty() {
            return Ok(Applied { new_changes });
        }
        let rows: Vec<([u8; 32], Vec<u8>)> = new_changes
            .iter()
            .map(|c| (c.hash().0, c.raw_bytes().to_vec()))
            .collect();
        self.last_seq = store
            .append_changes(&self.project_id, &rows)
            .map_err(|e| e.to_string())?;
        self.changes_since_snapshot += new_changes.len();
        self.first_unsnapshotted_ms.get_or_insert(now_ms);
        self.maybe_snapshot(store, now_ms);
        Ok(Applied { new_changes })
    }

    fn maybe_snapshot(&mut self, store: &Store, now_ms: i64) {
        let due_by_count = self.changes_since_snapshot >= SNAPSHOT_EVERY_CHANGES;
        let due_by_time = self
            .first_unsnapshotted_ms
            .is_some_and(|t| now_ms - t >= SNAPSHOT_EVERY_MS);
        if !(due_by_count || due_by_time) {
            return;
        }
        let bytes = self.doc.save();
        if store
            .save_snapshot(&self.project_id, self.last_seq, &bytes)
            .is_ok()
        {
            self.changes_since_snapshot = 0;
            self.first_unsnapshotted_ms = None;
        }
    }

    pub fn broadcast_except(&self, except: Option<u64>, m: &Message) {
        let frame = m.encode();
        for (id, c) in &self.clients {
            if Some(*id) != except {
                let _ = c.tx.send(Outbound::Frame(frame.clone()));
            }
        }
    }

    pub fn presence_snapshot(&self, except: u64) -> Vec<PresenceEntry> {
        self.clients
            .iter()
            .filter(|(id, _)| **id != except)
            .filter_map(|(_, c)| {
                c.presence.as_ref().map(|state| PresenceEntry {
                    client_id: c.client_id.clone(),
                    user_id: c.user_id.clone(),
                    state: state.clone(),
                })
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use automerge::transaction::Transactable;

    #[test]
    fn viewers_may_only_touch_comments_and_chat() {
        let mut doc = AutoCommit::new();
        let collab = doc
            .put_object(automerge::ROOT, "collab", automerge::ObjType::Map)
            .unwrap();
        doc.put_object(&collab, "comments", automerge::ObjType::Map)
            .unwrap();
        doc.put_object(automerge::ROOT, "regions", automerge::ObjType::Map)
            .unwrap();
        doc.commit();
        let mut store = Store::in_memory().unwrap();
        let mut room = Room::load(&store, "p").unwrap();
        let bytes = doc.save();
        room.apply(&mut store, 0, |d| {
            d.load_incremental(&bytes)
                .map(|_| ())
                .map_err(|e| e.to_string())
        })
        .unwrap();

        let comment = {
            let mut d = room.doc.fork();
            let collab = d.get(automerge::ROOT, "collab").unwrap().unwrap().1;
            let comments = d.get(&collab, "comments").unwrap().unwrap().1;
            d.put(&comments, "c1", "hello").unwrap();
            d.commit();
            d.save_after(&room.heads())
        };
        assert!(room
            .viewer_allowed(|d| d
                .load_incremental(&comment)
                .map(|_| ())
                .map_err(|e| e.to_string()))
            .unwrap());

        let region = {
            let mut d = room.doc.fork();
            let regions = d.get(automerge::ROOT, "regions").unwrap().unwrap().1;
            d.put(&regions, "r1", 1).unwrap();
            d.commit();
            d.save_after(&room.heads())
        };
        assert!(!room
            .viewer_allowed(|d| d
                .load_incremental(&region)
                .map(|_| ())
                .map_err(|e| e.to_string()))
            .unwrap());
        // The check works on a copy: the room's document is unchanged.
        assert!(room.doc.get(automerge::ROOT, "regions").unwrap().is_some());
        assert_eq!(room.doc.get_changes(&[]).len(), 1);
    }

    #[test]
    fn the_log_reloads_from_snapshot_plus_later_changes() {
        let mut store = Store::in_memory().unwrap();
        let mut room = Room::load(&store, "p").unwrap();
        let mut src = AutoCommit::new();
        for i in 0..(SNAPSHOT_EVERY_CHANGES + 5) {
            let before = src.get_heads();
            src.put(automerge::ROOT, "n", i as i64).unwrap();
            src.commit();
            let bytes = src.save_after(&before);
            room.apply(&mut store, 0, |d| {
                d.load_incremental(&bytes)
                    .map(|_| ())
                    .map_err(|e| e.to_string())
            })
            .unwrap();
        }
        let (snapshot, rest) = store.load_log("p").unwrap();
        assert_eq!(snapshot.unwrap().0, SNAPSHOT_EVERY_CHANGES as i64);
        assert_eq!(rest.len(), 5);
        let mut reloaded = Room::load(&store, "p").unwrap();
        assert_eq!(reloaded.heads(), src.get_heads());
    }
}
