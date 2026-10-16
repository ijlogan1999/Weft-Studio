#pragma once

// Relay accounts, projects, members and invites (spec 05 §3.1, §3.2) over the
// relay's REST API. Calls block, so the app makes them off the message thread.

#include "weft/doc/Document.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weft::collab
{
using doc::Bytes;
using doc::Value;

struct HttpResult
{
    int status = 0; // 0: the relay couldn't be reached
    Value json;
    Bytes bytes;
    std::string error;

    bool ok() const noexcept { return status >= 200 && status < 300; }
};

/** One request to the relay; json is sent as the body when not null. */
HttpResult httpRequest (std::string_view method, const std::string& url, const std::string& token = {},
                        const Value& json = {}, int timeoutMs = 30000);

/** A signed-in relay account and its tokens (access 15 min, refresh 30 days). */
struct Account
{
    std::string relayUrl; // http(s)://host[:port], no trailing slash
    std::string userId, email, displayName;
    std::string accessToken, refreshToken;
    std::int64_t accessExpiresAt = 0; // Unix ms by the relay's clock

    bool signedIn() const noexcept { return ! userId.empty() && ! refreshToken.empty(); }
    Value toValue() const;
    static Account fromValue (const Value& v);
};

struct ProjectInfo
{
    std::string projectId, name, role;
};

struct MemberInfo
{
    std::string userId, displayName, email, role;
    int color = 0;
};

struct InviteInfo
{
    std::string url, token, role, projectId, name;
    std::int64_t expiresAt = 0;
};

/** "http(s)://host[:port]/v1/invites/<token>" -> (relay URL, token). */
std::optional<std::pair<std::string, std::string>> parseInviteLink (std::string_view link);

/** Trims a relay address to "scheme://host[:port]" and adds http:// for localhost. */
std::string normalizeRelayUrl (std::string_view url);

/** "owner" -> "Owner" for display. */
std::string roleLabel (std::string_view role);

class RelayApi
{
public:
    /** Calls the relay as `account`, refreshing its access token when the relay asks. */
    explicit RelayApi (Account& account) : acct (account) {}

    static std::optional<Account> signUp (const std::string& relayUrl, const std::string& email, const std::string& password,
                                          const std::string& displayName, std::string* error = nullptr);
    static std::optional<Account> signIn (const std::string& relayUrl, const std::string& email, const std::string& password,
                                          std::string* error = nullptr);
    /** Anyone holding the link may look at it (no account needed). */
    static std::optional<InviteInfo> inviteInfo (const std::string& link, std::string* error = nullptr);

    /** New access token from the refresh token. */
    bool refresh();
    /** Registers the project on the relay with the caller as Owner (idempotent for the Owner). */
    std::optional<ProjectInfo> shareProject (const std::string& projectId, const std::string& name);
    std::optional<ProjectInfo> project (const std::string& projectId);
    std::vector<ProjectInfo> projects();
    std::vector<MemberInfo> members (const std::string& projectId);
    std::optional<InviteInfo> createInvite (const std::string& projectId, const std::string& role);
    std::optional<ProjectInfo> acceptInvite (const std::string& token);
    bool setRole (const std::string& projectId, const std::string& userId, const std::string& role);
    bool removeMember (const std::string& projectId, const std::string& userId);
    /** The whole document as saved bytes (members only). */
    std::optional<Bytes> fetchDocument (const std::string& projectId);

    const std::string& error() const noexcept { return lastError; }
    const Account& account() const noexcept { return acct; }

private:
    HttpResult call (std::string_view method, const std::string& path, const Value& json = {});
    Account& acct;
    std::string lastError;
};
} // namespace weft::collab
