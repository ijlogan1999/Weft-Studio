#include "weft/collab/Relay.h"

#include <weft_sync.h>

#include <algorithm>
#include <cctype>

namespace weft::collab
{
HttpResult httpRequest (std::string_view method, const std::string& url, const std::string& token, const Value& json, int timeoutMs)
{
    auto req = Value::map().set ("method", method).set ("url", url).set ("timeoutMs", timeoutMs);
    if (! token.empty())
        req.set ("token", token);
    if (! json.isNull())
        req.set ("json", json);
    const auto bytes = doc::encode (req);
    WeftBytes out {};
    HttpResult r;
    if (weft_http (bytes.data(), bytes.size(), &out) != WEFT_OK)
    {
        r.error = "The relay request couldn't be made.";
        return r;
    }
    const auto v = doc::decode ({ out.data, out.len });
    weft_bytes_free (out);
    if (! v)
    {
        r.error = "The relay's reply couldn't be read.";
        return r;
    }
    r.status = static_cast<int> ((*v)["status"].asInt());
    r.json = (*v)["json"];
    r.bytes = (*v)["bytes"].asBytes();
    r.error = (*v)["error"].asString();
    return r;
}

Value Account::toValue() const
{
    return Value::map()
        .set ("relayUrl", relayUrl)
        .set ("userId", userId)
        .set ("email", email)
        .set ("displayName", displayName)
        .set ("accessToken", accessToken)
        .set ("refreshToken", refreshToken)
        .set ("accessExpiresAt", accessExpiresAt);
}

Account Account::fromValue (const Value& v)
{
    Account a;
    a.relayUrl = v["relayUrl"].asString();
    a.userId = v["userId"].asString();
    a.email = v["email"].asString();
    a.displayName = v["displayName"].asString();
    a.accessToken = v["accessToken"].asString();
    a.refreshToken = v["refreshToken"].asString();
    a.accessExpiresAt = v["accessExpiresAt"].asInt();
    return a;
}

namespace
{
/** Fills the tokens and identity from a sign-in, sign-up or refresh reply. */
void readSession (Account& a, const Value& j)
{
    a.userId = j["userId"].asString (a.userId);
    a.email = j["email"].asString (a.email);
    a.displayName = j["displayName"].asString (a.displayName);
    a.accessToken = j["accessToken"].asString();
    a.accessExpiresAt = j["accessExpiresAt"].asInt();
    a.refreshToken = j["refreshToken"].asString (a.refreshToken);
}

std::optional<Account> session (const std::string& relayUrl, const std::string& path, const Value& body, std::string* error)
{
    const auto base = normalizeRelayUrl (relayUrl);
    const auto r = httpRequest ("POST", base + path, {}, body);
    if (! r.ok())
    {
        if (error != nullptr)
            *error = r.error.empty() ? "The relay refused the request." : r.error;
        return std::nullopt;
    }
    Account a;
    a.relayUrl = base;
    readSession (a, r.json);
    return a;
}

ProjectInfo projectFrom (const Value& j)
{
    return { j["projectId"].asString(), j["name"].asString(), j["role"].asString() };
}
} // namespace

std::string normalizeRelayUrl (std::string_view url)
{
    std::string s (url);
    s.erase (0, s.find_first_not_of (" \t\r\n"));
    s.erase (s.find_last_not_of (" \t\r\n") + 1);
    if (s.find ("://") == std::string::npos)
    {
        const bool local = s.starts_with ("localhost") || s.starts_with ("127.") || s.starts_with ("[::1]");
        s = (local ? "http://" : "https://") + s;
    }
    const auto authorityStart = s.find ("://") + 3;
    const auto pathStart = s.find ('/', authorityStart);
    if (pathStart != std::string::npos)
        s.erase (pathStart);
    return s;
}

std::optional<std::pair<std::string, std::string>> parseInviteLink (std::string_view link)
{
    std::string s (link);
    s.erase (0, s.find_first_not_of (" \t\r\n"));
    s.erase (s.find_last_not_of (" \t\r\n") + 1);
    const auto marker = s.find ("/v1/invites/");
    if (marker == std::string::npos || s.find ("://") == std::string::npos || s.find ("://") > marker)
        return std::nullopt;
    auto token = s.substr (marker + 12);
    if (const auto end = token.find_first_of ("/?#"); end != std::string::npos)
        token.erase (end);
    if (token.empty())
        return std::nullopt;
    return std::make_pair (s.substr (0, marker), token);
}

std::string roleLabel (std::string_view role)
{
    std::string s (role);
    if (! s.empty())
        s[0] = static_cast<char> (std::toupper (static_cast<unsigned char> (s[0])));
    return s;
}

std::optional<Account> RelayApi::signUp (const std::string& relayUrl, const std::string& email, const std::string& password,
                                         const std::string& displayName, std::string* error)
{
    return session (relayUrl, "/v1/auth/signup",
                    Value::map().set ("email", email).set ("password", password).set ("displayName", displayName), error);
}

std::optional<Account> RelayApi::signIn (const std::string& relayUrl, const std::string& email, const std::string& password, std::string* error)
{
    return session (relayUrl, "/v1/auth/signin", Value::map().set ("email", email).set ("password", password), error);
}

std::optional<InviteInfo> RelayApi::inviteInfo (const std::string& link, std::string* error)
{
    const auto parsed = parseInviteLink (link);
    if (! parsed)
    {
        if (error != nullptr)
            *error = "That isn't a Weft invite link.";
        return std::nullopt;
    }
    const auto r = httpRequest ("GET", parsed->first + "/v1/invites/" + parsed->second);
    if (! r.ok())
    {
        if (error != nullptr)
            *error = r.error;
        return std::nullopt;
    }
    InviteInfo i;
    i.url = parsed->first + "/v1/invites/" + parsed->second;
    i.token = parsed->second;
    i.role = r.json["role"].asString();
    i.projectId = r.json["projectId"].asString();
    i.name = r.json["name"].asString();
    i.expiresAt = r.json["expiresAt"].asInt();
    return i;
}

bool RelayApi::refresh()
{
    const auto r = httpRequest ("POST", acct.relayUrl + "/v1/auth/refresh", {}, Value::map().set ("refreshToken", acct.refreshToken));
    if (! r.ok())
    {
        lastError = r.error.empty() ? "Sign in again." : r.error;
        return false;
    }
    readSession (acct, r.json);
    return true;
}

HttpResult RelayApi::call (std::string_view method, const std::string& path, const Value& json)
{
    lastError.clear();
    auto r = httpRequest (method, acct.relayUrl + path, acct.accessToken, json);
    if (r.status == 401 && refresh())
        r = httpRequest (method, acct.relayUrl + path, acct.accessToken, json);
    if (! r.ok())
        lastError = r.error.empty() ? "The relay refused the request." : r.error;
    return r;
}

std::optional<ProjectInfo> RelayApi::shareProject (const std::string& projectId, const std::string& name)
{
    const auto r = call ("POST", "/v1/projects", Value::map().set ("projectId", projectId).set ("name", name));
    if (! r.ok())
        return std::nullopt;
    return projectFrom (r.json);
}

std::optional<ProjectInfo> RelayApi::project (const std::string& projectId)
{
    const auto r = call ("GET", "/v1/projects/" + projectId);
    if (! r.ok())
        return std::nullopt;
    return projectFrom (r.json);
}

std::vector<ProjectInfo> RelayApi::projects()
{
    std::vector<ProjectInfo> out;
    const auto r = call ("GET", "/v1/projects");
    for (const auto& p : r.json.asList())
        out.push_back (projectFrom (p));
    return out;
}

std::vector<MemberInfo> RelayApi::members (const std::string& projectId)
{
    std::vector<MemberInfo> out;
    const auto r = call ("GET", "/v1/projects/" + projectId + "/members");
    for (const auto& m : r.json.asList())
        out.push_back ({ m["userId"].asString(), m["displayName"].asString(), m["email"].asString(), m["role"].asString(),
                         static_cast<int> (m["color"].asInt()) });
    return out;
}

std::optional<InviteInfo> RelayApi::createInvite (const std::string& projectId, const std::string& role)
{
    const auto r = call ("POST", "/v1/projects/" + projectId + "/invites", Value::map().set ("role", role));
    if (! r.ok())
        return std::nullopt;
    InviteInfo i;
    i.url = r.json["url"].asString();
    i.token = r.json["token"].asString();
    i.role = r.json["role"].asString();
    i.expiresAt = r.json["expiresAt"].asInt();
    i.projectId = projectId;
    return i;
}

std::optional<ProjectInfo> RelayApi::acceptInvite (const std::string& token)
{
    const auto r = call ("POST", "/v1/invites/" + token + "/accept");
    if (! r.ok())
        return std::nullopt;
    return projectFrom (r.json);
}

bool RelayApi::setRole (const std::string& projectId, const std::string& userId, const std::string& role)
{
    return call ("PATCH", "/v1/projects/" + projectId + "/members/" + userId, Value::map().set ("role", role)).ok();
}

bool RelayApi::removeMember (const std::string& projectId, const std::string& userId)
{
    return call ("DELETE", "/v1/projects/" + projectId + "/members/" + userId).ok();
}

std::optional<Bytes> RelayApi::fetchDocument (const std::string& projectId)
{
    const auto r = call ("GET", "/v1/projects/" + projectId + "/document");
    if (! r.ok())
        return std::nullopt;
    return r.bytes;
}
} // namespace weft::collab
