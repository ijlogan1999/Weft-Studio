/* C ABI for the Weft Studio relay transport. Matches crates/weft-sync/src/lib.rs.
   Structured arguments and results are codec values (see weft_crdt.h); buffers are
   released with weft_bytes_free. */
#ifndef WEFT_SYNC_H
#define WEFT_SYNC_H

#include "weft_crdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One blocking HTTP request. request: {method, url, token?, json?, bytes?, timeoutMs?, headers?}.
   response: {status, headers, json?, bytes?, error?}; status 0 means the relay couldn't be
   reached. Header names are lower case. */
WeftStatus weft_http(const uint8_t* request, size_t len, WeftBytes* response);

typedef struct WeftConn WeftConn;

/* config: {relayUrl, projectId, clientId, clientVersion, schemaVersion, accessToken,
   accessExpiresAt, refreshToken, oneWayDelayMs?, online?}. Null for a bad config. */
WeftConn* weft_conn_start(const uint8_t* config, size_t len);
/* kind 4 = SYNC, 5 = CHANGE (raw Automerge bytes), 7 = PRESENCE (presence state). */
WeftStatus weft_conn_send(WeftConn* conn, uint8_t kind, const uint8_t* data, size_t len);
void weft_conn_set_online(WeftConn* conn, int online);
void weft_conn_set_heads(WeftConn* conn, const uint8_t* heads, size_t len);
WeftStatus weft_conn_set_tokens(WeftConn* conn, const uint8_t* tokens, size_t len);
/* 1 and an event {kind: connecting | welcome | reject | sync | change | ack | presence |
   notice | blob | disconnected | tokens, ...}, or 0 when nothing is due. */
int weft_conn_poll(WeftConn* conn, WeftBytes* event);
void weft_conn_free(WeftConn* conn);

#ifdef __cplusplus
}
#endif

#endif
