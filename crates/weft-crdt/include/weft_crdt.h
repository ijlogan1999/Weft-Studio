/* C ABI over Automerge for the Weft Studio client. Matches crates/weft-crdt/src/lib.rs. */
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

WeftDoc* weft_doc_new(void);
WeftDoc* weft_doc_new_with_actor(const uint8_t* actor, size_t len);
WeftDoc* weft_doc_load(const uint8_t* data, size_t len);
void weft_doc_free(WeftDoc* doc);

WeftStatus weft_doc_save(WeftDoc* doc, WeftBytes* out);
WeftStatus weft_doc_put_str(WeftDoc* doc, const char* key, size_t key_len, const char* value, size_t value_len);
WeftStatus weft_doc_get_str(WeftDoc* doc, const char* key, size_t key_len, WeftBytes* out);
WeftStatus weft_doc_merge(WeftDoc* doc, WeftDoc* other);

void weft_bytes_free(WeftBytes bytes);

#ifdef __cplusplus
}
#endif

#endif
