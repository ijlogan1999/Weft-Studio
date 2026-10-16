/* C ABI over Automerge for the Weft Studio client. Matches crates/weft-crdt/src/lib.rs.
   Values and paths use the binary encoding described in crates/weft-crdt/src/codec.rs. */
#ifndef WEFT_CRDT_H
#define WEFT_CRDT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum WeftStatus {
    WEFT_OK = 0,
    WEFT_INVALID_ARGUMENT = 1,
    WEFT_NOT_FOUND = 2,
    WEFT_WRONG_TYPE = 3,
    WEFT_CORRUPT_DATA = 4,
    WEFT_INTERNAL = 5
} WeftStatus;

/* Owned by the caller; release with weft_bytes_free. */
typedef struct WeftBytes {
    uint8_t* data;
    size_t len;
} WeftBytes;

typedef struct WeftDoc WeftDoc;

/* Lifetime, save and load */
WeftDoc* weft_doc_new(void);
WeftDoc* weft_doc_new_with_actor(const uint8_t* actor, size_t len);
WeftDoc* weft_doc_load(const uint8_t* data, size_t len);
WeftStatus weft_doc_set_actor(WeftDoc* doc, const uint8_t* actor, size_t len);
void weft_doc_free(WeftDoc* doc);
WeftStatus weft_doc_save(WeftDoc* doc, WeftBytes* out);

/* Reads and writes (writes are pending until weft_doc_commit) */
WeftStatus weft_doc_put(WeftDoc* doc, const uint8_t* path, size_t path_len, const uint8_t* value, size_t value_len);
WeftStatus weft_doc_insert(WeftDoc* doc, const uint8_t* path, size_t path_len, const uint8_t* value, size_t value_len);
WeftStatus weft_doc_delete(WeftDoc* doc, const uint8_t* path, size_t path_len);
WeftStatus weft_doc_get(WeftDoc* doc, const uint8_t* path, size_t path_len, WeftBytes* out);
WeftStatus weft_doc_op_id(WeftDoc* doc, const uint8_t* path, size_t path_len, WeftBytes* out);

/* Changes and history */
int32_t weft_doc_commit(WeftDoc* doc, const uint8_t* message, size_t message_len, int64_t time_ms);
size_t weft_doc_rollback(WeftDoc* doc);
WeftStatus weft_doc_heads(WeftDoc* doc, WeftBytes* out);
WeftStatus weft_doc_last_local_change(WeftDoc* doc, WeftBytes* out);
WeftStatus weft_doc_all_changes(WeftDoc* doc, WeftBytes* out, size_t* count);
WeftStatus weft_doc_apply(WeftDoc* doc, const uint8_t* data, size_t len);
int32_t weft_doc_has_change(WeftDoc* doc, const uint8_t* hash32);
WeftStatus weft_doc_changed_paths(WeftDoc* doc, const uint8_t* heads, size_t heads_len, size_t depth, WeftBytes* out);
WeftStatus weft_doc_merge(WeftDoc* doc, WeftDoc* other);

void weft_bytes_free(WeftBytes bytes);

/* Sync (spec 05 §3.3): Automerge's sync protocol, one state per connection */
typedef struct WeftSyncState WeftSyncState;
WeftSyncState* weft_sync_state_new(void);
void weft_sync_state_free(WeftSyncState* state);
/* WEFT_NOT_FOUND when there is nothing to send. */
WeftStatus weft_doc_sync_generate(WeftDoc* doc, WeftSyncState* state, WeftBytes* out);
WeftStatus weft_doc_sync_receive(WeftDoc* doc, WeftSyncState* state, const uint8_t* data, size_t len);
/* Codec list of {hash, actor, message, time} for the changes since heads (all for empty heads). */
WeftStatus weft_doc_changes_info(WeftDoc* doc, const uint8_t* heads, size_t heads_len, WeftBytes* out);
WeftStatus weft_change_hash(const uint8_t* data, size_t len, uint8_t* out32);

/* BLAKE3 content hashing (media blobs) */
typedef struct WeftHasher WeftHasher;
WeftHasher* weft_hasher_new(void);
WeftStatus weft_hasher_update(WeftHasher* hasher, const uint8_t* data, size_t len);
/* Writes 64 lowercase hex digits, no terminator. */
WeftStatus weft_hasher_finish_hex(const WeftHasher* hasher, uint8_t* out_hex);
void weft_hasher_free(WeftHasher* hasher);

#ifdef __cplusplus
}
#endif

#endif
