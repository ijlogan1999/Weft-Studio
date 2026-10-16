//! Accounts and sessions (spec 05 §3.1, §11): Argon2id password hashes, HS256
//! access tokens that last 15 minutes, opaque refresh tokens that last 30 days,
//! and a sign-in rate limit of 5 failures per minute per IP and per account.

use argon2::password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::Argon2;
use base64::engine::general_purpose::URL_SAFE_NO_PAD as B64;
use base64::Engine;
use hmac::{Hmac, Mac};
use rand::rngs::OsRng;
use rand::RngCore;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Mutex;

pub const ACCESS_TOKEN_MS: i64 = 15 * 60 * 1000;
pub const REFRESH_TOKEN_MS: i64 = 30 * 24 * 60 * 60 * 1000;
pub const SIGN_IN_FAILURES_PER_MINUTE: usize = 5;
pub const MIN_PASSWORD_LENGTH: usize = 8;

/// Wall clock with a test offset (`--test-clock` lets tests move time forward).
#[derive(Default)]
pub struct Clock {
    offset_ms: AtomicI64,
}

impl Clock {
    pub fn now_ms(&self) -> i64 {
        let wall = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        wall + self.offset_ms.load(Ordering::Relaxed)
    }

    pub fn advance(&self, ms: i64) {
        self.offset_ms.fetch_add(ms, Ordering::Relaxed);
    }
}

pub fn hash_password(password: &str) -> Option<String> {
    let salt = SaltString::generate(&mut OsRng);
    Argon2::default()
        .hash_password(password.as_bytes(), &salt)
        .ok()
        .map(|h| h.to_string())
}

pub fn verify_password(password: &str, hash: &str) -> bool {
    PasswordHash::new(hash)
        .map(|parsed| {
            Argon2::default()
                .verify_password(password.as_bytes(), &parsed)
                .is_ok()
        })
        .unwrap_or(false)
}

/// A random URL-safe token of `bytes` random bytes.
pub fn random_token(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    OsRng.fill_bytes(&mut buf);
    B64.encode(buf)
}

pub fn random_id(prefix: &str) -> String {
    let mut buf = [0u8; 8];
    OsRng.fill_bytes(&mut buf);
    let hex: String = buf.iter().map(|b| format!("{b:02x}")).collect();
    format!("{prefix}{hex}")
}

/// Refresh tokens are stored hashed, so a leaked database can't be replayed.
pub fn token_hash(token: &str) -> String {
    Sha256::digest(token.as_bytes())
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

type HmacSha256 = Hmac<Sha256>;

/// Issues and checks HS256 JWT access tokens.
pub struct Tokens {
    secret: Vec<u8>,
}

#[derive(serde::Serialize, serde::Deserialize)]
struct Claims {
    sub: String,
    /// Expiry in milliseconds since the epoch (the relay's clock, so tests can move it).
    exp_ms: i64,
    typ: String,
}

impl Tokens {
    pub fn new(secret: Vec<u8>) -> Tokens {
        Tokens { secret }
    }

    fn sign(&self, data: &[u8]) -> Vec<u8> {
        let mut mac = HmacSha256::new_from_slice(&self.secret).expect("HMAC takes any key length");
        mac.update(data);
        mac.finalize().into_bytes().to_vec()
    }

    pub fn issue(&self, user_id: &str, now_ms: i64) -> (String, i64) {
        let exp_ms = now_ms + ACCESS_TOKEN_MS;
        let header = B64.encode(br#"{"alg":"HS256","typ":"JWT"}"#);
        let claims = serde_json::to_vec(&Claims {
            sub: user_id.to_string(),
            exp_ms,
            typ: "access".into(),
        })
        .expect("claims serialize");
        let body = format!("{header}.{}", B64.encode(claims));
        let sig = B64.encode(self.sign(body.as_bytes()));
        (format!("{body}.{sig}"), exp_ms)
    }

    /// The user id of a valid, unexpired access token.
    pub fn verify(&self, token: &str, now_ms: i64) -> Option<String> {
        let (body, sig) = token.rsplit_once('.')?;
        let sig = B64.decode(sig).ok()?;
        let mut mac = HmacSha256::new_from_slice(&self.secret).ok()?;
        mac.update(body.as_bytes());
        mac.verify_slice(&sig).ok()?;
        let (_, claims) = body.split_once('.')?;
        let claims: Claims = serde_json::from_slice(&B64.decode(claims).ok()?).ok()?;
        (claims.typ == "access" && claims.exp_ms > now_ms).then_some(claims.sub)
    }
}

/// Counts failed sign-ins per key (an IP or an account) over the last minute.
#[derive(Default)]
pub struct RateLimiter {
    failures: Mutex<HashMap<String, VecDeque<i64>>>,
}

impl RateLimiter {
    fn prune(q: &mut VecDeque<i64>, now_ms: i64) {
        while q.front().is_some_and(|&t| now_ms - t >= 60_000) {
            q.pop_front();
        }
    }

    /// True while any key has had 5 failures in the last minute.
    pub fn limited(&self, keys: &[&str], now_ms: i64) -> bool {
        let mut map = self.failures.lock().unwrap();
        keys.iter().any(|k| {
            map.get_mut(*k).is_some_and(|q| {
                Self::prune(q, now_ms);
                q.len() >= SIGN_IN_FAILURES_PER_MINUTE
            })
        })
    }

    pub fn record_failure(&self, keys: &[&str], now_ms: i64) {
        let mut map = self.failures.lock().unwrap();
        for k in keys {
            let q = map.entry(k.to_string()).or_default();
            Self::prune(q, now_ms);
            q.push_back(now_ms);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn passwords_hash_with_argon2id_and_verify() {
        let h = hash_password("correct horse").unwrap();
        assert!(h.starts_with("$argon2id$"), "{h}");
        assert!(verify_password("correct horse", &h));
        assert!(!verify_password("wrong", &h));
        assert!(!verify_password("x", "not a hash"));
    }

    #[test]
    fn access_tokens_expire_after_15_minutes_and_reject_tampering() {
        let t = Tokens::new(b"secret".to_vec());
        let (tok, exp) = t.issue("u_1", 1_000);
        assert_eq!(exp, 1_000 + ACCESS_TOKEN_MS);
        assert_eq!(t.verify(&tok, 1_000).as_deref(), Some("u_1"));
        assert_eq!(t.verify(&tok, exp - 1).as_deref(), Some("u_1"));
        assert_eq!(t.verify(&tok, exp), None);
        assert_eq!(Tokens::new(b"other".to_vec()).verify(&tok, 1_000), None);
        let mut forged = tok.clone();
        forged.insert(10, 'x');
        assert_eq!(t.verify(&forged, 1_000), None);
    }

    #[test]
    fn five_failures_in_a_minute_limit_the_sixth() {
        let r = RateLimiter::default();
        for i in 0..5 {
            assert!(!r.limited(&["ip", "a@b"], i));
            r.record_failure(&["ip", "a@b"], i);
        }
        assert!(r.limited(&["ip"], 10));
        assert!(r.limited(&["other-ip", "a@b"], 10));
        assert!(!r.limited(&["other-ip", "c@d"], 10));
        assert!(!r.limited(&["ip", "a@b"], 60_005));
    }
}
