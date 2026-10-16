//! The relay's WebSocket protocol (spec 05 §3.3).
//!
//! Every message is one binary frame: a type byte, then the payload. `SYNC` and
//! `CHANGE` carry raw Automerge bytes (a sync message, or concatenated change
//! chunks); every other payload is a CBOR map.

use serde::{Deserialize, Serialize};

pub const PROTOCOL_VERSION: u32 = 1;

/// Keepalive interval and the silence after which a peer counts as gone (05 §3.3).
pub const PING_INTERVAL_MS: u64 = 15_000;
pub const IDLE_TIMEOUT_MS: u64 = 45_000;

pub mod kind {
    pub const HELLO: u8 = 1;
    pub const WELCOME: u8 = 2;
    pub const REJECT: u8 = 3;
    pub const SYNC: u8 = 4;
    pub const CHANGE: u8 = 5;
    pub const ACK: u8 = 6;
    pub const PRESENCE: u8 = 7;
    pub const PING: u8 = 8;
    pub const PONG: u8 = 9;
    pub const BLOB_AVAILABLE: u8 = 10;
    pub const NOTICE: u8 = 11;
}

/// `REJECT` codes.
pub mod reject {
    pub const AUTH: &str = "auth";
    pub const FORBIDDEN: &str = "forbidden";
    pub const UPDATE_REQUIRED: &str = "update_required";
    pub const NOT_FOUND: &str = "not_found";
}

/// `NOTICE` kinds.
pub mod notice {
    pub const MEMBER_ADDED: &str = "member_added";
    pub const MEMBER_REMOVED: &str = "member_removed";
    /// Sent to a member who was removed, just before their socket closes.
    pub const REMOVED: &str = "removed";
    pub const ROLE_CHANGED: &str = "role_changed";
    /// A change was refused (a Viewer touched more than comments and chat, D-COL-2).
    pub const FORBIDDEN: &str = "forbidden";
    pub const PROJECT_DELETED: &str = "project_deleted";
    pub const CLIENT_JOINED: &str = "client_joined";
    pub const CLIENT_LEFT: &str = "client_left";
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Hello {
    pub token: String,
    #[serde(rename = "projectId")]
    pub project_id: String,
    #[serde(rename = "clientId")]
    pub client_id: String,
    #[serde(rename = "clientVersion")]
    pub client_version: String,
    /// The newest document schema this client understands.
    #[serde(rename = "schemaVersion")]
    pub schema_version: u32,
    /// The client's document heads (32 bytes each).
    #[serde(with = "serde_bytes")]
    pub heads: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Member {
    #[serde(rename = "userId")]
    pub user_id: String,
    #[serde(rename = "displayName")]
    pub display_name: String,
    pub role: String,
    /// Collaborator color index in Weft's palette, stable per user and project (05 §5).
    pub color: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PresenceEntry {
    #[serde(rename = "clientId")]
    pub client_id: String,
    #[serde(rename = "userId")]
    pub user_id: String,
    /// The client's presence state, opaque to the relay.
    #[serde(with = "serde_bytes")]
    pub state: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Welcome {
    pub role: String,
    #[serde(rename = "userId")]
    pub user_id: String,
    pub members: Vec<Member>,
    pub presence: Vec<PresenceEntry>,
    #[serde(with = "serde_bytes")]
    pub heads: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Reject {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Ack {
    /// Changes the relay has durably stored (32 bytes each).
    #[serde(with = "serde_bytes")]
    pub hashes: Vec<u8>,
    /// The relay's document heads after storing them.
    #[serde(with = "serde_bytes")]
    pub heads: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BlobAvailable {
    pub hash: String,
    pub size: u64,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Notice {
    pub kind: String,
    #[serde(rename = "userId", default, skip_serializing_if = "Option::is_none")]
    pub user_id: Option<String>,
    #[serde(rename = "clientId", default, skip_serializing_if = "Option::is_none")]
    pub client_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub role: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub member: Option<Member>,
    /// Change hashes the notice is about (32 bytes each), e.g. refused changes.
    #[serde(default, with = "serde_bytes", skip_serializing_if = "Vec::is_empty")]
    pub hashes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Message {
    Hello(Hello),
    Welcome(Welcome),
    Reject(Reject),
    Sync(Vec<u8>),
    Change(Vec<u8>),
    Ack(Ack),
    /// Client to relay: just the state. Relay to clients: stamped with who sent it.
    Presence(PresenceEntry),
    Ping(i64),
    Pong(i64),
    BlobAvailable(BlobAvailable),
    Notice(Notice),
}

#[derive(Debug)]
pub struct DecodeError(pub String);

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "bad protocol message: {}", self.0)
    }
}

impl std::error::Error for DecodeError {}

fn cbor<T: Serialize>(kind: u8, v: &T) -> Vec<u8> {
    let mut out = vec![kind];
    ciborium::into_writer(v, &mut out).expect("CBOR encoding into a Vec can't fail");
    out
}

fn from_cbor<T: for<'de> Deserialize<'de>>(payload: &[u8]) -> Result<T, DecodeError> {
    ciborium::from_reader(payload).map_err(|e| DecodeError(e.to_string()))
}

#[derive(Serialize, Deserialize)]
struct Stamp {
    t: i64,
}

impl Message {
    pub fn kind(&self) -> u8 {
        match self {
            Message::Hello(_) => kind::HELLO,
            Message::Welcome(_) => kind::WELCOME,
            Message::Reject(_) => kind::REJECT,
            Message::Sync(_) => kind::SYNC,
            Message::Change(_) => kind::CHANGE,
            Message::Ack(_) => kind::ACK,
            Message::Presence(_) => kind::PRESENCE,
            Message::Ping(_) => kind::PING,
            Message::Pong(_) => kind::PONG,
            Message::BlobAvailable(_) => kind::BLOB_AVAILABLE,
            Message::Notice(_) => kind::NOTICE,
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let k = self.kind();
        match self {
            Message::Hello(m) => cbor(k, m),
            Message::Welcome(m) => cbor(k, m),
            Message::Reject(m) => cbor(k, m),
            Message::Sync(b) | Message::Change(b) => {
                let mut out = Vec::with_capacity(b.len() + 1);
                out.push(k);
                out.extend_from_slice(b);
                out
            }
            Message::Ack(m) => cbor(k, m),
            Message::Presence(m) => cbor(k, m),
            Message::Ping(t) | Message::Pong(t) => cbor(k, &Stamp { t: *t }),
            Message::BlobAvailable(m) => cbor(k, m),
            Message::Notice(m) => cbor(k, m),
        }
    }

    pub fn decode(frame: &[u8]) -> Result<Message, DecodeError> {
        let (&k, payload) = frame
            .split_first()
            .ok_or_else(|| DecodeError("empty frame".into()))?;
        Ok(match k {
            kind::HELLO => Message::Hello(from_cbor(payload)?),
            kind::WELCOME => Message::Welcome(from_cbor(payload)?),
            kind::REJECT => Message::Reject(from_cbor(payload)?),
            kind::SYNC => Message::Sync(payload.to_vec()),
            kind::CHANGE => Message::Change(payload.to_vec()),
            kind::ACK => Message::Ack(from_cbor(payload)?),
            kind::PRESENCE => Message::Presence(from_cbor(payload)?),
            kind::PING => Message::Ping(from_cbor::<Stamp>(payload)?.t),
            kind::PONG => Message::Pong(from_cbor::<Stamp>(payload)?.t),
            kind::BLOB_AVAILABLE => Message::BlobAvailable(from_cbor(payload)?),
            kind::NOTICE => Message::Notice(from_cbor(payload)?),
            other => return Err(DecodeError(format!("unknown message type {other}"))),
        })
    }
}

/// Splits concatenated 32-byte hashes.
pub fn split_hashes(bytes: &[u8]) -> impl Iterator<Item = &[u8]> {
    bytes.chunks_exact(32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_message_round_trips() {
        let member = Member {
            user_id: "u1".into(),
            display_name: "Ana".into(),
            role: "owner".into(),
            color: 3,
        };
        let messages = vec![
            Message::Hello(Hello {
                token: "t".into(),
                project_id: "p".into(),
                client_id: "c".into(),
                client_version: "0.1.0".into(),
                schema_version: 1,
                heads: vec![7; 64],
            }),
            Message::Welcome(Welcome {
                role: "editor".into(),
                user_id: "u2".into(),
                members: vec![member.clone()],
                presence: vec![PresenceEntry {
                    client_id: "c".into(),
                    user_id: "u1".into(),
                    state: vec![1, 2],
                }],
                heads: vec![],
            }),
            Message::Reject(Reject {
                code: reject::AUTH.into(),
                message: "Sign in again.".into(),
            }),
            Message::Sync(vec![1, 2, 3]),
            Message::Change(vec![]),
            Message::Ack(Ack {
                hashes: vec![9; 32],
                heads: vec![8; 32],
            }),
            Message::Ping(42),
            Message::Pong(-1),
            Message::BlobAvailable(BlobAvailable {
                hash: "b3:00".into(),
                size: 10,
            }),
            Message::Notice(Notice {
                kind: notice::ROLE_CHANGED.into(),
                role: Some("viewer".into()),
                member: Some(member),
                ..Default::default()
            }),
        ];
        for m in messages {
            let bytes = m.encode();
            assert_eq!(bytes[0], m.kind());
            assert_eq!(Message::decode(&bytes).unwrap(), m);
        }
        assert!(Message::decode(&[]).is_err());
        assert!(Message::decode(&[99]).is_err());
        assert!(Message::decode(&[kind::HELLO, 0xff]).is_err());
    }
}
