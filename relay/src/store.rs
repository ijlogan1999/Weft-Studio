//! The relay's durable state in SQLite (spec 05 §3.1): accounts, refresh tokens,
//! projects and members, invites, and each project's change log with its latest
//! compacted snapshot (§3.4).

use rusqlite::{params, Connection, OptionalExtension};
use std::path::Path;

pub type DbResult<T> = rusqlite::Result<T>;

pub struct Store {
    db: Connection,
}

#[derive(Debug, Clone)]
pub struct User {
    pub id: String,
    pub email: String,
    pub display_name: String,
    pub password_hash: String,
}

#[derive(Debug, Clone)]
pub struct Project {
    pub id: String,
    pub name: String,
    pub owner_id: String,
}

#[derive(Debug, Clone)]
pub struct MemberRow {
    pub user_id: String,
    pub display_name: String,
    pub email: String,
    pub role: String,
    pub color: u32,
}

#[derive(Debug, Clone)]
pub struct Upload {
    pub id: String,
    pub hash: String,
    pub project_id: String,
    pub size: i64,
    pub received: i64,
}

#[derive(Debug, Clone)]
pub struct Invite {
    pub project_id: String,
    pub role: String,
    pub expires_ms: i64,
}

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS users(
    id TEXT PRIMARY KEY, email TEXT UNIQUE NOT NULL, display_name TEXT NOT NULL,
    password_hash TEXT NOT NULL, created_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS refresh_tokens(
    token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL, expires_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS projects(
    id TEXT PRIMARY KEY, name TEXT NOT NULL, owner_id TEXT NOT NULL, created_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS members(
    project_id TEXT NOT NULL, user_id TEXT NOT NULL, role TEXT NOT NULL, color INTEGER NOT NULL,
    PRIMARY KEY(project_id, user_id));
CREATE TABLE IF NOT EXISTS invites(
    token TEXT PRIMARY KEY, project_id TEXT NOT NULL, role TEXT NOT NULL,
    created_by TEXT NOT NULL, expires_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS changes(
    project_id TEXT NOT NULL, seq INTEGER NOT NULL, hash BLOB NOT NULL, data BLOB NOT NULL,
    PRIMARY KEY(project_id, seq));
CREATE UNIQUE INDEX IF NOT EXISTS changes_by_hash ON changes(project_id, hash);
CREATE TABLE IF NOT EXISTS snapshots(
    project_id TEXT PRIMARY KEY, seq INTEGER NOT NULL, data BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS blobs(
    hash TEXT PRIMARY KEY, size INTEGER NOT NULL, created_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS blob_owners(
    hash TEXT NOT NULL, owner_id TEXT NOT NULL, PRIMARY KEY(hash, owner_id));
CREATE TABLE IF NOT EXISTS blob_projects(
    hash TEXT NOT NULL, project_id TEXT NOT NULL, PRIMARY KEY(hash, project_id));
CREATE TABLE IF NOT EXISTS uploads(
    id TEXT PRIMARY KEY, hash TEXT NOT NULL, project_id TEXT NOT NULL, size INTEGER NOT NULL,
    received INTEGER NOT NULL, created_ms INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS uploads_by_hash ON uploads(hash, project_id);
";

/// Collaborator colors cycle through this many palette entries (05 §5).
pub const PALETTE_SIZE: u32 = 12;

impl Store {
    pub fn open(path: &Path) -> DbResult<Store> {
        let db = Connection::open(path)?;
        // FULL synchronous: an ACKed change survives a crash or power cut (05 §3.3 Durability).
        db.execute_batch(
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;",
        )?;
        db.execute_batch(SCHEMA)?;
        Ok(Store { db })
    }

    pub fn in_memory() -> DbResult<Store> {
        let db = Connection::open_in_memory()?;
        db.execute_batch(SCHEMA)?;
        Ok(Store { db })
    }

    // Accounts -------------------------------------------------------------

    pub fn create_user(&self, user: &User, now_ms: i64) -> DbResult<bool> {
        let n = self.db.execute(
            "INSERT OR IGNORE INTO users(id, email, display_name, password_hash, created_ms) VALUES(?1,?2,?3,?4,?5)",
            params![user.id, user.email, user.display_name, user.password_hash, now_ms],
        )?;
        Ok(n == 1)
    }

    fn user_from_row(r: &rusqlite::Row<'_>) -> rusqlite::Result<User> {
        Ok(User {
            id: r.get(0)?,
            email: r.get(1)?,
            display_name: r.get(2)?,
            password_hash: r.get(3)?,
        })
    }

    pub fn user_by_email(&self, email: &str) -> DbResult<Option<User>> {
        self.db
            .query_row(
                "SELECT id, email, display_name, password_hash FROM users WHERE email = ?1",
                [email],
                Self::user_from_row,
            )
            .optional()
    }

    pub fn user_by_id(&self, id: &str) -> DbResult<Option<User>> {
        self.db
            .query_row(
                "SELECT id, email, display_name, password_hash FROM users WHERE id = ?1",
                [id],
                Self::user_from_row,
            )
            .optional()
    }

    pub fn add_refresh_token(
        &self,
        token_hash: &str,
        user_id: &str,
        expires_ms: i64,
    ) -> DbResult<()> {
        self.db.execute(
            "INSERT INTO refresh_tokens(token_hash, user_id, expires_ms) VALUES(?1,?2,?3)",
            params![token_hash, user_id, expires_ms],
        )?;
        Ok(())
    }

    /// The user a refresh token belongs to, if it exists and hasn't expired.
    pub fn refresh_token_user(&self, token_hash: &str, now_ms: i64) -> DbResult<Option<String>> {
        self.db
            .query_row(
                "SELECT user_id FROM refresh_tokens WHERE token_hash = ?1 AND expires_ms > ?2",
                params![token_hash, now_ms],
                |r| r.get(0),
            )
            .optional()
    }

    // Projects and members ---------------------------------------------------

    pub fn create_project(&self, project: &Project, now_ms: i64) -> DbResult<bool> {
        let n = self.db.execute(
            "INSERT OR IGNORE INTO projects(id, name, owner_id, created_ms) VALUES(?1,?2,?3,?4)",
            params![project.id, project.name, project.owner_id, now_ms],
        )?;
        if n == 1 {
            self.add_member(&project.id, &project.owner_id, "owner")?;
        }
        Ok(n == 1)
    }

    pub fn project(&self, id: &str) -> DbResult<Option<Project>> {
        self.db
            .query_row(
                "SELECT id, name, owner_id FROM projects WHERE id = ?1",
                [id],
                |r| {
                    Ok(Project {
                        id: r.get(0)?,
                        name: r.get(1)?,
                        owner_id: r.get(2)?,
                    })
                },
            )
            .optional()
    }

    /// Projects the user belongs to, with their role: (project, role).
    pub fn projects_of(&self, user_id: &str) -> DbResult<Vec<(Project, String)>> {
        let mut st = self.db.prepare(
            "SELECT p.id, p.name, p.owner_id, m.role FROM projects p JOIN members m ON m.project_id = p.id
             WHERE m.user_id = ?1 ORDER BY p.name",
        )?;
        let rows = st.query_map([user_id], |r| {
            Ok((
                Project {
                    id: r.get(0)?,
                    name: r.get(1)?,
                    owner_id: r.get(2)?,
                },
                r.get(3)?,
            ))
        })?;
        rows.collect()
    }

    pub fn delete_project(&self, id: &str) -> DbResult<()> {
        for table in [
            "members",
            "invites",
            "changes",
            "snapshots",
            "blob_projects",
            "uploads",
        ] {
            self.db
                .execute(&format!("DELETE FROM {table} WHERE project_id = ?1"), [id])?;
        }
        self.db
            .execute("DELETE FROM projects WHERE id = ?1", [id])?;
        Ok(())
    }

    /// Adds a member with the next free collaborator color; an existing member keeps their role.
    pub fn add_member(&self, project_id: &str, user_id: &str, role: &str) -> DbResult<bool> {
        if self.role_of(project_id, user_id)?.is_some() {
            return Ok(false);
        }
        let used: Vec<u32> = {
            let mut st = self
                .db
                .prepare("SELECT color FROM members WHERE project_id = ?1")?;
            let rows = st.query_map([project_id], |r| r.get(0))?;
            rows.collect::<DbResult<_>>()?
        };
        let color = (0..PALETTE_SIZE)
            .find(|c| !used.contains(c))
            .unwrap_or(used.len() as u32 % PALETTE_SIZE);
        self.db.execute(
            "INSERT INTO members(project_id, user_id, role, color) VALUES(?1,?2,?3,?4)",
            params![project_id, user_id, role, color],
        )?;
        Ok(true)
    }

    pub fn role_of(&self, project_id: &str, user_id: &str) -> DbResult<Option<String>> {
        self.db
            .query_row(
                "SELECT role FROM members WHERE project_id = ?1 AND user_id = ?2",
                params![project_id, user_id],
                |r| r.get(0),
            )
            .optional()
    }

    pub fn set_role(&self, project_id: &str, user_id: &str, role: &str) -> DbResult<bool> {
        let n = self.db.execute(
            "UPDATE members SET role = ?3 WHERE project_id = ?1 AND user_id = ?2",
            params![project_id, user_id, role],
        )?;
        Ok(n == 1)
    }

    pub fn remove_member(&self, project_id: &str, user_id: &str) -> DbResult<bool> {
        let n = self.db.execute(
            "DELETE FROM members WHERE project_id = ?1 AND user_id = ?2",
            params![project_id, user_id],
        )?;
        Ok(n == 1)
    }

    pub fn members(&self, project_id: &str) -> DbResult<Vec<MemberRow>> {
        let mut st = self.db.prepare(
            "SELECT m.user_id, u.display_name, u.email, m.role, m.color FROM members m JOIN users u ON u.id = m.user_id
             WHERE m.project_id = ?1 ORDER BY CASE m.role WHEN 'owner' THEN 0 WHEN 'editor' THEN 1 ELSE 2 END, u.display_name",
        )?;
        let rows = st.query_map([project_id], |r| {
            Ok(MemberRow {
                user_id: r.get(0)?,
                display_name: r.get(1)?,
                email: r.get(2)?,
                role: r.get(3)?,
                color: r.get(4)?,
            })
        })?;
        rows.collect()
    }

    // Invites ------------------------------------------------------------------

    pub fn add_invite(&self, token: &str, invite: &Invite, created_by: &str) -> DbResult<()> {
        self.db.execute(
            "INSERT INTO invites(token, project_id, role, created_by, expires_ms) VALUES(?1,?2,?3,?4,?5)",
            params![token, invite.project_id, invite.role, created_by, invite.expires_ms],
        )?;
        Ok(())
    }

    pub fn invite(&self, token: &str) -> DbResult<Option<Invite>> {
        self.db
            .query_row(
                "SELECT project_id, role, expires_ms FROM invites WHERE token = ?1",
                [token],
                |r| {
                    Ok(Invite {
                        project_id: r.get(0)?,
                        role: r.get(1)?,
                        expires_ms: r.get(2)?,
                    })
                },
            )
            .optional()
    }

    // Change log -----------------------------------------------------------------

    /// Appends changes (hash, bytes) in order; ones already stored are skipped. Returns the last seq.
    pub fn append_changes(
        &mut self,
        project_id: &str,
        changes: &[([u8; 32], Vec<u8>)],
    ) -> DbResult<i64> {
        let tx = self.db.transaction()?;
        let mut seq: i64 = tx.query_row(
            "SELECT COALESCE(MAX(seq), 0) FROM changes WHERE project_id = ?1",
            [project_id],
            |r| r.get(0),
        )?;
        {
            let mut st = tx.prepare(
                "INSERT OR IGNORE INTO changes(project_id, seq, hash, data) VALUES(?1,?2,?3,?4)",
            )?;
            for (hash, data) in changes {
                if st.execute(params![project_id, seq + 1, &hash[..], data])? == 1 {
                    seq += 1;
                }
            }
        }
        tx.commit()?;
        Ok(seq)
    }

    /// The latest snapshot (seq, bytes) and every change after it, in order.
    #[allow(clippy::type_complexity)]
    pub fn load_log(&self, project_id: &str) -> DbResult<(Option<(i64, Vec<u8>)>, Vec<Vec<u8>>)> {
        let snapshot: Option<(i64, Vec<u8>)> = self
            .db
            .query_row(
                "SELECT seq, data FROM snapshots WHERE project_id = ?1",
                [project_id],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .optional()?;
        let after = snapshot.as_ref().map_or(0, |s| s.0);
        let mut st = self
            .db
            .prepare("SELECT data FROM changes WHERE project_id = ?1 AND seq > ?2 ORDER BY seq")?;
        let rows = st.query_map(params![project_id, after], |r| r.get(0))?;
        Ok((snapshot, rows.collect::<DbResult<_>>()?))
    }

    pub fn change_count(&self, project_id: &str) -> DbResult<i64> {
        self.db.query_row(
            "SELECT COUNT(*) FROM changes WHERE project_id = ?1",
            [project_id],
            |r| r.get(0),
        )
    }

    pub fn save_snapshot(&self, project_id: &str, seq: i64, data: &[u8]) -> DbResult<()> {
        self.db.execute(
            "INSERT INTO snapshots(project_id, seq, data) VALUES(?1,?2,?3)
             ON CONFLICT(project_id) DO UPDATE SET seq = excluded.seq, data = excluded.data",
            params![project_id, seq, data],
        )?;
        Ok(())
    }

    // Media blobs (05 §8) ---------------------------------------------------------

    /// The blob's size if it's stored and linked to the project.
    pub fn blob_in_project(&self, hash: &str, project_id: &str) -> DbResult<Option<i64>> {
        self.db
            .query_row(
                "SELECT b.size FROM blobs b JOIN blob_projects p ON p.hash = b.hash
                 WHERE b.hash = ?1 AND p.project_id = ?2",
                params![hash, project_id],
                |r| r.get(0),
            )
            .optional()
    }

    /// The blob's size if it's stored for this owner's account (D-COL-9).
    pub fn blob_of_owner(&self, hash: &str, owner_id: &str) -> DbResult<Option<i64>> {
        self.db
            .query_row(
                "SELECT b.size FROM blobs b JOIN blob_owners o ON o.hash = b.hash
                 WHERE b.hash = ?1 AND o.owner_id = ?2",
                params![hash, owner_id],
                |r| r.get(0),
            )
            .optional()
    }

    /// Records a verified blob for an owner's account and links it to a project.
    pub fn add_blob(
        &self,
        hash: &str,
        size: i64,
        owner_id: &str,
        project_id: &str,
        now_ms: i64,
    ) -> DbResult<()> {
        self.db.execute(
            "INSERT OR IGNORE INTO blobs(hash, size, created_ms) VALUES(?1,?2,?3)",
            params![hash, size, now_ms],
        )?;
        self.db.execute(
            "INSERT OR IGNORE INTO blob_owners(hash, owner_id) VALUES(?1,?2)",
            params![hash, owner_id],
        )?;
        self.link_blob(hash, project_id)
    }

    pub fn link_blob(&self, hash: &str, project_id: &str) -> DbResult<()> {
        self.db.execute(
            "INSERT OR IGNORE INTO blob_projects(hash, project_id) VALUES(?1,?2)",
            params![hash, project_id],
        )?;
        Ok(())
    }

    fn upload_from_row(r: &rusqlite::Row<'_>) -> rusqlite::Result<Upload> {
        Ok(Upload {
            id: r.get(0)?,
            hash: r.get(1)?,
            project_id: r.get(2)?,
            size: r.get(3)?,
            received: r.get(4)?,
        })
    }

    /// An unfinished upload of this blob for this project, to resume.
    pub fn open_upload(&self, hash: &str, project_id: &str) -> DbResult<Option<Upload>> {
        self.db
            .query_row(
                "SELECT id, hash, project_id, size, received FROM uploads
                 WHERE hash = ?1 AND project_id = ?2 ORDER BY created_ms LIMIT 1",
                params![hash, project_id],
                Self::upload_from_row,
            )
            .optional()
    }

    pub fn upload(&self, id: &str) -> DbResult<Option<Upload>> {
        self.db
            .query_row(
                "SELECT id, hash, project_id, size, received FROM uploads WHERE id = ?1",
                [id],
                Self::upload_from_row,
            )
            .optional()
    }

    pub fn create_upload(&self, upload: &Upload, now_ms: i64) -> DbResult<()> {
        self.db.execute(
            "INSERT INTO uploads(id, hash, project_id, size, received, created_ms) VALUES(?1,?2,?3,?4,?5,?6)",
            params![
                upload.id,
                upload.hash,
                upload.project_id,
                upload.size,
                upload.received,
                now_ms
            ],
        )?;
        Ok(())
    }

    pub fn set_upload_received(&self, id: &str, received: i64) -> DbResult<()> {
        self.db.execute(
            "UPDATE uploads SET received = ?2 WHERE id = ?1",
            params![id, received],
        )?;
        Ok(())
    }

    pub fn delete_upload(&self, id: &str) -> DbResult<()> {
        self.db.execute("DELETE FROM uploads WHERE id = ?1", [id])?;
        Ok(())
    }
}
