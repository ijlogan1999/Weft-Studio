# 05 · Collaboration Specification

Real-time multi-user collaboration is designed into Weft from the first milestone. A solo, never-shared project uses exactly the same document model and code paths as a shared one; sharing only adds a network transport.

---

## 1. Goals and non-goals

**Goals (MVP)**
1. Several people edit one project at the same time, and every concurrent edit merges without a conflict dialog.
2. Edits made offline merge automatically on reconnect.
3. Everyone sees who is online, where their playhead and pointer are, what they have selected, and which track they're working on.
4. Audio media is transferred once per file per machine, identified by content hash.
5. Timeline comments, markers, chat, and version history with restore.
6. Optional per-track locks; merging is the default.
7. Each user hears the project through their own audio interface; there is no networked audio.

**Non-goals (MVP)**
- Real-time networked audio jamming (research item, §12).
- Voice/video chat (Later).
- Character-level collaborative text editing (names and comments are whole-value edits, D-DM-15).
- Peer-to-peer transport without a relay (Later; the protocol does not preclude it).
- Server-side mixing or rendering.

---

## 2. Architecture overview

```
 ┌───────────────┐   WebSocket (TLS)    ┌──────────────────────┐   WebSocket   ┌───────────────┐
 │ Weft client A │◄────────────────────►│     Relay server     │◄─────────────►│ Weft client B │
 │               │   doc sync msgs      │  • auth / rooms      │               │               │
 │ CRDT replica  │   presence msgs      │  • doc store (log)   │               │ CRDT replica  │
 │ local media   │                      │  • presence fan-out  │               │ local media   │
 │ cache         │   HTTPS blob PUT/GET │  • blob store (S3-   │  HTTPS blobs  │ cache         │
 └───────────────┘◄────────────────────►│    compatible/disk)  │◄─────────────►└───────────────┘
                                         └──────────────────────┘
```

- **Client replica.** Every client holds a full copy of the CRDT document and applies local edits immediately (optimistic, zero-latency editing). It persists changes locally (W-10).
- **Relay.** Stores the document's change log, relays changes and presence between clients in a project "room", authenticates users, enforces roles, and stores media blobs. The relay never needs to interpret musical content; it does minimal document inspection (schema version, member list in a separate store).
- **Two channels.** (1) A WebSocket for document sync and presence. (2) HTTPS for blob upload/download (large, resumable, cacheable).

**Decision D-COL-1:** a relay (star topology) rather than peer-to-peer. It keeps history and blobs available when no other peer is online, simplifies NAT traversal, and gives one place to enforce access control.

---

## 3. Relay server

### 3.1 Responsibilities
1. Accounts and sessions: sign up / sign in (email + password with Argon2id hashing; OAuth providers v2), issuing short-lived access tokens (JWT, 15 min) and refresh tokens (30 days).
2. Projects and membership: `projectId → {ownerId, members: [{userId, role}]}` stored in a relational store (SQLite for self-hosted, Postgres for hosted).
3. Document log: append-only store of CRDT changes per project, plus periodic compacted snapshots.
4. Rooms: one room per open project; fan-out of changes and presence to connected members.
5. Blob store: content-addressed media (§8).
6. Invites: signed invite links with role and expiry (default 7 days).

### 3.2 Roles
| Role | Read doc | Edit doc | Comment/chat | Invite | Change roles / delete project | Break locks |
|---|---|---|---|---|---|---|
| Owner | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Editor | ✓ | ✓ | ✓ | ✓ (as Editor or Viewer) | – | – |
| Viewer | ✓ | – | ✓ | – | – | – |

The relay rejects document changes from Viewers, **except** changes that only touch `collab.comments` and `collab.chat` (the relay inspects the touched paths). **Decision D-COL-2:** Viewers can comment and chat, which is how reviewers (mix clients, producers) typically participate.

### 3.3 Sync protocol (WebSocket)

Messages are binary frames: 1-byte type + payload (CBOR for envelopes, raw bytes for CRDT data).

| Type | Direction | Payload | Meaning |
|---|---|---|---|
| `HELLO` | C→S | token, projectId, clientId, clientVersion, schemaVersion, heads | Join a room. `heads` = client's current document heads. |
| `WELCOME` | S→C | role, members, presence snapshot, server heads | Joined. |
| `REJECT` | S→C | code (`auth`, `forbidden`, `update_required`, `not_found`) | Join refused. |
| `SYNC` | both | CRDT sync message | Library sync protocol (Automerge sync / Yjs state-vector exchange). Repeated until both sides converge. |
| `CHANGE` | both | one or more CRDT changes | Live incremental change(s). Server appends to log and fans out to other clients. |
| `ACK` | S→C | change hashes | Server durably stored these changes. |
| `PRESENCE` | both | presence state (§5) | Ephemeral; never stored. |
| `PING`/`PONG` | both | timestamp | Keepalive every 15 s; 45 s without traffic = disconnected. |
| `BLOB_AVAILABLE` | S→C | hash, size | A blob has finished uploading (lets waiting clients start downloading). |
| `NOTICE` | S→C | kind, data | e.g. member added/removed, role changed, lock broken, project deleted. |

**Join flow**
1. Client opens `wss://<relay>/v1/projects/<id>/ws` and sends `HELLO`.
2. Relay authenticates, checks membership and schema version. If the client's max supported schema < the document's `schemaVersion`, send `REJECT update_required`.
3. Relay sends `WELCOME` and both sides run the sync exchange until heads match.
4. Thereafter, local changes are sent as `CHANGE` immediately (batched at most every 50 ms), and remote changes arrive as `CHANGE`.

**Ordering and delivery.** The relay preserves per-client order and fans out in the order it appends. CRDT semantics make delivery order irrelevant for convergence; the relay may deliver duplicates after reconnects and clients ignore already-applied changes.

**Durability.** A client marks a local change "synced" only after `ACK`. Unacknowledged changes stay in the local pending queue (and in `Recovery/changes.log`) and are re-sent after reconnect.

### 3.4 Storage and compaction
- Changes are appended to a per-project log. Every 1,000 changes or 10 minutes of activity, the relay (or a client on its behalf, for libraries without a server runtime) stores a compacted snapshot; new joiners load snapshot + subsequent changes.
- History is never discarded in MVP (needed for version restore). Storage limits per project are a hosting concern, not a client behavior.

### 3.5 Performance targets
- Edit-to-remote-display latency ≤ 500 ms p95 on a 50 ms RTT connection (F-COL-4); target ≤ 150 ms + RTT.
- Relay handles ≥ 50 concurrent rooms × 8 clients on a 2-vCPU instance.
- Document load for a 200-track, 5,000-region, 200,000-note project ≤ 3 s on a mid-range laptop.

### 3.6 Decisions made while building the relay (M10)
- **Decision D-COL-14:** invites are links. The Share dialog creates a link for a role (Editor or Viewer) to copy and send; the relay sends no email in MVP, so the "invite by email" field of 04 §9.2 is the link field. Links are random 256-bit tokens stored on the relay with their role and expiry (7 days), which makes them unguessable, revocable and checkable without a signing key on clients.
- **Decision D-COL-15:** a drag is one change. A gesture (fader, region drag) is one CRDT transaction committed on release (D-ARCH-6), so collaborators see the final value rather than the drag in progress, and a 2 s drag is one change (AT-COL-6). Remote changes that arrive during your own drag are applied after release (D-COL-3).
- **Decision D-COL-16:** M10 shares the document only. Media upload and download, and the "N MB of audio" in the share question, arrive with blob sync in M11; until then the Share dialog says audio stays on this computer.
- **Decision D-COL-17:** un-ACKed changes are remembered by hash in `<project>/Cache/sync-pending.txt`. The changes themselves are in the document (or the recovery log after a crash), and the sync exchange on reconnect sends whatever the relay lacks; the pending list drives "Offline – N changes pending". The status is "Synced" once the relay has acknowledged every local change and the client holds every head the relay last reported.
- **Decision D-COL-18:** a reconnect that brings in more than one collaborator change shows "Merged N changes from collaborators" (§13).
- **Decision D-COL-19:** the signed-in account (relay, email, display name and tokens) is kept in the per-user settings folder. New edits are attributed to the account's user id once signed in (03 §1.2); signing out keeps the project working locally.
- **Decision D-COL-20:** Share dialog calls to the relay block the window for at most their timeout in M10; they move to the background transfer thread with M11.
- **Decision D-COL-21:** clients send their own changes through the `SYNC` exchange (batched at most every 50 ms) rather than as bare `CHANGE` frames; the relay still fans changes out to other clients as `CHANGE`. A bare change can reach the relay before one it depends on (made before a reconnect, say), and then waits there unapplied; the sync exchange always sends dependencies first. A client that receives a `CHANGE` it can't apply yet resumes the sync exchange to fetch what it's missing.

---

## 4. Document and merge semantics

The document is the CRDT described in `03-data-model.md` §10. Summary of user-visible merge outcomes:

| Concurrent edits by A and B | Result |
|---|---|
| A moves region R; B moves region S | Both moves apply. |
| A moves R in time; B changes R's gain | Both apply (different fields). |
| A moves R to bar 5; B moves R to bar 9 | One position wins deterministically (LWW by CRDT order); both clients converge to the same position. The loser sees R jump and a toast "Region moved by B". |
| A moves R to another track; B moves R in time | Both apply (`trackId` and `start` are separate fields): R ends up on A's track at B's time. |
| A deletes R; B edits R | R is deleted (D-DM-16). B gets a toast. |
| A adds notes to region R; B adds different notes | All notes kept. |
| A changes note N's pitch; B changes N's velocity | Both apply. |
| A and B both drag the same fader | LWW; converges to one value. During the drag, each sees remote values arrive; the local value wins while the local user is touching the control (UI only, §4.1). |
| A and B both rename a track | LWW. |
| A inserts plugin at slot 2; B inserts a different plugin at slot 2 | Both plugins kept, in a deterministic order. |
| A sends track 1 → Bus 1; B routes Bus 1 → track 1's bus chain creating a cycle | Both changes stored; the newer edge is disabled at read time with a warning (D-DM-6). |
| A edits plugin P's internal state (window open); B edits P too | LWW on the opaque state blob; individual automatable parameter values merge per parameter (D-DM-5). |
| A adds tempo change at bar 9 = 100 BPM; B adds one at bar 9 = 140 BPM | Both points stored; greater Id wins at read time; a conflict badge appears on the tempo lane (D-DM-9). |
| A deletes track T; B records onto T | B's recording lands on a recovered track (D-W1-6, D-DM-17). |

### 4.1 Interaction rules for live controls
- While a user is dragging a control or region, remote updates to *that same property* are buffered for display and applied when the drag ends (then the local final value is written, so the local user's drag wins unless the remote write happened later in CRDT order, in which case it is visible right after release). **Decision D-COL-3:** this avoids controls jittering under the user's mouse.
- Continuous control drags emit at most 20 changes per second over the network (intermediate values coalesced); the final value is always sent.

### 4.2 Audio engine updates
The UI thread applies remote changes to the document and then publishes an immutable engine snapshot (see Phase 2 architecture) to the audio thread. Remote edits never block or lock the audio thread. A remote edit during playback takes effect within one audio buffer of the snapshot swap.

### 4.3 Transport is not shared
Each user's transport (play/stop, playhead, cycle on/off *state while playing*) is local. The cycle range itself is part of the shared document. **Decision D-COL-4:** cycle range and punch range are shared (they are part of the arrangement intent), but whether *your* transport is playing is not. "Play for everyone" is v2 (F-COL-24).

---

## 5. Presence

Presence is ephemeral state broadcast through the relay and never written to the document or history.

```ts
Presence {
  userId: UserId
  clientId: string             // one user may connect from two machines
  displayName: string
  color: Color                 // assigned per project by the relay, stable per user
  status: 'active' | 'idle' | 'recording' | 'offline-editing'
  view: 'arrange' | 'pianoRoll' | 'audioEditor' | 'mixer'
  focus: { trackId?: Id, regionId?: Id }       // where they're working
  playhead: { pos: Ticks, playing: bool, at: timestamp }
  pointer: { area: 'arrange' | 'pianoRoll', pos: Ticks, y: TrackRef | Pitch } | null
  selection: { regions: Id[], notes: Id[], automationPoints: Id[] }   // capped at 500 ids; beyond → { count: n }
  writingAutomationOn?: Id     // trackId
}
```

**Update rates**
- Pointer: at most 20 Hz, only when it moved; sent as musical coordinates (ticks + track id or pitch), so it lands correctly regardless of each viewer's zoom/scroll. **Decision D-COL-5.**
- Playhead while playing: send `{pos, playing:true, at}` once on start and every 2 s; viewers extrapolate the position from the sender's tempo map and `at`, correcting on each update. When stopped, sent on change.
- Selection, focus, view, status: on change, debounced 100 ms.
- Idle: no input for 2 minutes → `idle`.

**Display rules** (see 04 §6, §9)
- Pointers: shown only when the viewer's visible area includes the pointer's position; fade out after 5 s without movement.
- Playheads: shown only while that collaborator is playing.
- Selections: outlined in the collaborator's color. If two collaborators select the same region, both outlines stack (offset by 1 px).
- Track badges: avatar on the track header when that user's `focus.trackId` is the track.

**Offline:** when the relay is unreachable, collaborators' presence is cleared after 45 s and the local client shows "Offline".

---

## 6. Undo/redo in collaboration

- Undo is **per user**: Ctrl+Z reverts the most recent change *made by this user on this client* that hasn't been undone, by generating a new inverse change (it does not rewind the shared history).
- The inverse change restores each field the original change wrote to the value it had *before* that change, **only if the field still holds the value the original change wrote**. If a collaborator has since overwritten the field, that field is left alone. **Decision D-COL-6:** "undo only what is still mine", so your undo never clobbers someone else's later work.
- If an object the change touched was deleted by someone else, that part is skipped and the status bar says "Couldn't fully undo: <object> was changed by <name>".
- Undoing a deletion re-creates the objects with their original IDs and field values.
- Redo applies the original change's values again, under the same rule.
- The undo stack is per session and cleared on reload (D-W3-4).

---

## 7. Track locking (optional)

- Locking is off by default; merging is the default (D-INV-5).
- Any Editor can lock a track they're working on. A lock is a `LockInfo` on the track in the document (03 §9.3), so it syncs like other state and survives disconnects.
- While a track is locked by someone else, a compliant client prevents local edits to: the track's fields (including mute), its channel strip, its regions (including moving regions onto or off it), its automation lanes, and its take folders. Solo stays available.
- **Decision D-COL-7:** solo is per-user local state (like record-enable), so one person soloing a track never changes what anyone else hears, and it stays usable on locked tracks. Mute is shared, because muting is a mix decision, and is therefore disabled on tracks locked by others.
- Concurrent edits made offline before seeing a lock still merge (a lock can't be enforced retroactively). A client that receives a lock for a track it edited offline shows a notice to the lock holder: "<name>'s offline edits to <track> were merged".
- The lock holder unlocks explicitly; locks also expire 24 h after the holder's last presence in the project. The Owner may break any lock (NOTICE sent to the holder).
- Locks are advisory at the relay level in MVP (the relay doesn't inspect which track a change touches). **Decision D-COL-8.**

---

## 8. Media (blob) sync

### 8.1 Identity
- Every media file (after import normalization, D-DM-4) is identified by `b3:<BLAKE3-256 hex>` of its full byte content.
- The same audio imported by two users produces the same hash and is stored once.

### 8.2 Upload
1. When a region referencing a new hash is committed (recording stop, import), the client queues the blob for upload if the project is shared.
2. `HEAD /v1/blobs/<hash>`: if present, done (dedupe across projects is allowed only within the same owner's account in MVP; **Decision D-COL-9**, to avoid leaking the existence of files across accounts).
3. Otherwise upload with a resumable protocol: `POST /v1/blobs/<hash>/uploads` → upload id; `PUT` chunks of 8 MB with offsets; `POST …/complete`. The relay verifies the hash before making the blob available, then broadcasts `BLOB_AVAILABLE`.
4. Uploads run in a background I/O thread with at most 2 concurrent uploads; they pause during recording if bandwidth/disk contention is detected (simple rule: uploads pause while recording is active). **Decision D-COL-10.**
5. The document change referencing the blob is sent immediately, *not* after upload completes, so collaborators see the region at once (in "downloading" state).

### 8.3 Download
1. When a client's document references a hash not in its local `Media/` cache, it requests `GET /v1/blobs/<hash>` (supports HTTP Range).
2. Priority: blobs referenced by regions near any collaborator's playhead or the local playhead first, then by timeline order.
3. "Streamed to peers": during download, the client can start playback of the already-downloaded prefix of a file (WAV is linear PCM, so a prefix is playable). Regions whose required portion isn't downloaded yet are skipped silently during playback and drawn with the hatched/progress pattern. **Decision D-COL-11.**
4. Downloaded bytes are verified against the hash when complete; a mismatch discards the file and retries once, then reports an error.
5. Peak (waveform overview) files are generated locally after download; they are not synced.

### 8.4 Large-file and quota behavior
- Max blob size: 4 GB (WAV limit; BWF/RF64 > 4 GB is v2).
- Status bar shows transfer progress (04 §10).

### 8.5 Plugin state blobs
Plugin state larger than 64 KB is stored as a blob with the same mechanism (03 §5.2).

---

## 9. Comments, markers, chat, and version history

### 9.1 Comments and markers
Stored in the document (03 §8.1, §9.1), so they sync, work offline, and appear in history. Editing a comment is allowed only by its author (enforced in clients; relay enforces "Viewers may only touch comments/chat"). Resolving can be done by anyone with comment rights.

Mentions (`@name`) produce an in-app notification for that user when they next connect (computed client-side from the document: a mention newer than the user's last-seen marker, stored in local view state).

### 9.2 Chat
- Append-only list in the document (D-DM-14). Messages can be deleted (tombstone) by their author.
- Max 4,000 characters, plain text with time links (D-UI-3).

### 9.3 Version history
- **Automatic versions:** the client creates an auto `VersionTag` when a user's session in the project ends (close or 30 min idle) if they made changes, and the relay creates one per day of activity. Auto versions are collapsed by day in the UI.
- **Named versions:** "Save Version…" creates a tag with the user's name for it.
- **Preview:** selecting a version materializes the document at those heads (CRDT "view at heads") read-only, and the client renders it (04 §9.5).
- **Restore:** creates a *new change* on top of current state that sets the document's content to equal the old version's content (computed as a diff from current to old and applied as ordinary edits). History is not rewritten, so restoring is itself undoable and visible to all. **Decision D-COL-12.** After restore, a version tag "Restored '<name>'" is added automatically.
- Restore requires Editor or Owner role.
- Media: blobs referenced by any version are retained by the relay as long as the version exists (no garbage collection in MVP).

---

## 10. Machine-specific settings in a shared project

Collaborators have different audio interfaces and plugins.

- **Inputs:** a track's `input.firstChannel` is shared, but each client maps it through its own device: if the channel doesn't exist on that machine, the track shows "Input not available" and can't be armed until the user picks one (which writes the shared value). **Decision D-COL-13:** keep the input shared (the common case is one recordist), accept that another user re-picking it changes it for all.
- **Missing plugins:** placeholders keep state intact (F-PLG-15, F-COL-20). A collaborator without the plugin hears the track without that insert (passthrough); their edits never overwrite the plugin's state. The Missing Plugins dialog lists what's missing and who added it.
- **Plugin version differences:** state is passed to the installed version as-is; if it fails to load, the instance becomes a placeholder.
- **Sample rate:** project sample rate is shared; clients whose device runs at another rate resample (D-W0-1).

---

## 11. Security and privacy

- All traffic over TLS 1.2+ (relay rejects plain `ws://` except on `localhost` for development).
- Access tokens are bound to the user; every `HELLO` and blob request is authorized against project membership. Blob URLs are not guessable-and-public: a hash alone does not grant access; the relay checks that the requesting user is a member of a project that references the hash.
- Removing a member immediately closes their sockets and revokes access to the document and blobs. Data they already have locally stays on their machine (unavoidable; documented in the Share dialog).
- Passwords: Argon2id. Rate-limit sign-in attempts (5/min/IP + account).
- The relay stores only what's needed: account email, display name, membership, document log, blobs. No telemetry in MVP.
- End-to-end encryption is Later (would prevent server-side snapshots/compaction).

---

## 12. Research item (v2+): real-time networked jamming

**Not in MVP.** This section records the latency analysis that justifies deferring it.

### 12.1 Latency budget
For two musicians to play together "in time", the one-way mouth-to-ear (or pick-to-ear) delay should stay under roughly **25–30 ms**; research on ensemble performance places the comfortable threshold around 20–30 ms one-way, with tempo drift and degradation beyond that. Sound in air travels about 1 m per 3 ms, so 25 ms equals standing ~8 m apart on a stage.

One-way end-to-end delay components:

| Stage | Typical | Optimistic |
|---|---|---|
| Audio input buffer + ADC (ASIO, 64–128 samples @ 48 kHz) | 2–4 ms | 1.5 ms |
| Encoding (Opus low-delay, 2.5–10 ms frames, or uncompressed PCM) | 5–10 ms | 0 ms (PCM) |
| Network propagation (fiber ~5 µs/km; real routes ~1.5× great-circle) | see below | |
| Network jitter buffer | 5–20 ms | 2–5 ms |
| Decoding | 0–2 ms | 0 ms |
| Output buffer + DAC | 2–4 ms | 1.5 ms |
| **Overhead excluding propagation** | **14–40 ms** | **~5 ms** |

Propagation (one-way, real routes):
- Same city (≤ 50 km): 1–3 ms
- 500 km (e.g. regional): ~4–8 ms
- 1,500 km: ~12–20 ms
- Transcontinental (~4,000 km): ~30–45 ms
- Transatlantic: ~40–60 ms

Conclusion: with optimistic local overhead (~5 ms, uncompressed PCM, tiny jitter buffers on a wired connection), jamming within roughly **500–1,000 km** can fit under 25 ms. With typical consumer conditions (Wi-Fi, compressed audio, larger jitter buffers), even same-city sessions sit at 20–40 ms. Long-distance real-time jamming is physically limited by the speed of light in fiber, whatever the software does.

### 12.2 Options to evaluate in research
1. **Direct peer-to-peer UDP audio** (bypassing the relay; NAT traversal via STUN/TURN; TURN relays add latency).
2. **Uncompressed or lightly compressed audio** (PCM 48 kHz/16-bit stereo ≈ 1.5 Mbit/s each way; acceptable on most broadband).
3. **Adaptive jitter buffering** with packet loss concealment.
4. **Latency-accepting modes** used by existing open approaches: delaying everyone by exactly one bar/phrase so each player plays along with the others' previous measure ("interval" style). Musically restrictive but tolerant of any latency.
5. **Global clock alignment** so each user's transport and metronome are synchronized via NTP-style offset estimation, enabling "everyone records against the same click" with post-aligned results (this is a realistic v2 step: synchronized remote recording without live monitoring of each other).

### 12.3 Recommended path
- v2: synchronized remote playback/recording (F-COL-24, option 5), each user still monitoring locally. Recordings are aligned by transport position, not by network arrival.
- Later: experimental P2P low-latency audio between two peers in the same region, with a live latency readout and an explicit "works best within ~500 km on wired connections" warning.

---

## 13. Failure modes and expected behavior

| Situation | Behavior |
|---|---|
| Network drops mid-edit | Edits continue locally; status bar "Offline – N changes pending"; reconnect with exponential backoff (1, 2, 4 … max 30 s); sync on reconnect. |
| Long offline period (days), many edits on both sides | Merge on reconnect; deterministic result; a summary toast "Merged N changes from collaborators". |
| Relay restarts | Clients reconnect and resync from heads. No data loss for ACKed changes; un-ACKed changes are re-sent. |
| Client crashes before ACK | Changes are in `Recovery/changes.log` and are re-sent after restart. |
| Two clients of the same user | Treated as separate clients (separate presence, separate undo stacks). |
| Member removed while online | Socket closed with `NOTICE removed`; project becomes read-only locally with a banner. |
| Relay storage full / blob upload fails | Retry with backoff; status bar shows the error; document sync is unaffected. |
| Document from a newer schema | `REJECT update_required`; client works on its local replica offline and prompts to update. |
| Corrupt change received | Library rejects it; client logs it, requests full resync; relay quarantines the change if repeatable. |

---

## 14. Consistency notes for other files

- `Track.solo` is per-user local state (D-COL-7); `03-data-model.md` §3 marks it LOCAL ONLY.
- Media hashing and storage by hash (D-W10-1) is the same mechanism locally and remotely.

---

## Decisions in this file

| ID | Decision |
|---|---|
| D-COL-1 | Relay star topology rather than P2P. |
| D-COL-2 | Viewers may comment and chat. |
| D-COL-3 | Remote updates to a property you're dragging are deferred until release. |
| D-COL-4 | Cycle/punch ranges are shared; transport state is local. |
| D-COL-5 | Pointers sent in musical coordinates. |
| D-COL-6 | Undo reverts only fields still holding your value. |
| D-COL-7 | Mute shared; solo local; mute disabled on others' locked tracks. |
| D-COL-8 | Locks enforced by clients, not relay, in MVP. |
| D-COL-9 | Blob dedupe only within an owner's account. |
| D-COL-10 | Uploads pause while recording. |
| D-COL-11 | Partially downloaded media plays its available prefix; missing parts are silent. |
| D-COL-12 | Restore is a forward change; history is never rewritten. |
| D-COL-13 | Track input assignments are shared. |
| D-COL-14 | Invites are unguessable stored links with a role and expiry; no email delivery in MVP. |
| D-COL-15 | A drag is one change, sent on release. |
| D-COL-16 | M10 shares the document only; media sync is M11. |
| D-COL-17 | Pending changes are tracked by hash in Cache/sync-pending.txt; "Synced" means ACKed and caught up. |
| D-COL-18 | "Merged N changes from collaborators" after a reconnect with more than one remote change. |
| D-COL-19 | The account lives in the settings folder; edits are attributed to it once signed in. |
| D-COL-20 | Share dialog relay calls block briefly in M10. |
| D-COL-21 | Clients send their changes through the sync exchange; the relay fans out CHANGE frames. |
