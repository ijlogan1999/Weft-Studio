#pragma once

// Helpers for the [S] sync tests: a real weft-relay process on localhost, accounts,
// and peers (a Session plus a SyncClient each) driven from the test thread.

#include "weft/collab/Relay.h"
#include "weft/collab/SyncClient.h"
#include "weft/commands/Session.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <csignal>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
extern char** environ;
#endif

namespace weft::test
{
namespace fs = std::filesystem;
using collab::Account;
using collab::RelayApi;
using collab::SyncClient;
using collab::SyncState;
using commands::Session;

inline constexpr Ticks barTicks = 3840;

struct TempDir
{
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("weft-sync-" + std::to_string (rd()) + std::to_string (rd()));
        fs::create_directories (path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all (path, ec);
    }
    TempDir (const TempDir&) = delete;
    TempDir& operator= (const TempDir&) = delete;
};

/** The relay binary running in a child process; kill() is a hard kill (no clean shutdown). */
class RelayProcess
{
public:
    explicit RelayProcess (bool testClock = false) : testClock (testClock) { start(); }
    ~RelayProcess()
    {
        kill();
        if (const char* keep = std::getenv ("WEFT_RELAY_LOG_DIR"))
        {
            std::error_code ec;
            fs::copy_file (data.path / "relay.log", fs::path (keep) / ("relay-" + std::to_string (port) + ".log"), fs::copy_options::overwrite_existing, ec);
        }
    }
    RelayProcess (const RelayProcess&) = delete;
    RelayProcess& operator= (const RelayProcess&) = delete;

    /** Starts the relay, on the same port as last time after a restart. */
    void start()
    {
        const auto portFile = data.path / "port";
        std::error_code ec;
        fs::remove (portFile, ec);
        std::vector<std::string> args { WEFT_RELAY_EXE, "--data", (data.path / "db").string(), "--port-file", portFile.string(),
                                        "--addr", "127.0.0.1:" + std::to_string (port) };
        if (testClock)
            args.push_back ("--test-clock");
        spawn (args);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
        while (! fs::exists (portFile))
        {
            REQUIRE (std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        std::ifstream f (portFile);
        f >> port;
        REQUIRE (port > 0);
    }

    void kill()
    {
#ifdef _WIN32
        if (process != nullptr)
        {
            TerminateProcess (process, 1);
            WaitForSingleObject (process, 5000);
            CloseHandle (process);
            process = nullptr;
        }
#else
        if (pid > 0)
        {
            ::kill (pid, SIGKILL);
            int status = 0;
            waitpid (pid, &status, 0);
            pid = 0;
        }
#endif
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string (port); }

    /** Moves the relay's clock forward (needs testClock). */
    void advanceClock (std::int64_t ms) const
    {
        REQUIRE (collab::httpRequest ("POST", url() + "/v1/test/clock", {}, doc::Value::map().set ("advanceMs", ms)).ok());
    }

private:
    void spawn (const std::vector<std::string>& args)
    {
#ifdef _WIN32
        std::string cmd;
        for (const auto& a : args)
            cmd += "\"" + a + "\" ";
        STARTUPINFOA si {};
        si.cb = sizeof (si);
        PROCESS_INFORMATION pi {};
        REQUIRE (CreateProcessA (nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        CloseHandle (pi.hThread);
        process = pi.hProcess;
#else
        std::vector<char*> argv;
        for (const auto& a : args)
            argv.push_back (const_cast<char*> (a.c_str()));
        argv.push_back (nullptr);
        // The relay's output goes to a log file, so a relay outliving a killed test
        // run doesn't hold the runner's pipes open.
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init (&actions);
        const auto log = (data.path / "relay.log").string();
        posix_spawn_file_actions_addopen (&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2 (&actions, 1, 2);
        const auto rc = posix_spawn (&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy (&actions);
        REQUIRE (rc == 0);
#endif
    }

    TempDir data;
    bool testClock = false;
    int port = 0;
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t pid = 0;
#endif
};

inline Account signUp (const RelayProcess& relay, const std::string& name)
{
    std::string error;
    static int n = 0;
    auto a = RelayApi::signUp (relay.url(), name + std::to_string (++n) + "@example.org", "password123", name, &error);
    INFO (error);
    REQUIRE (a.has_value());
    return *a;
}

inline std::string randomClientId()
{
    std::random_device rd;
    return "c" + std::to_string (rd()) + std::to_string (rd());
}

/** One collaborator: a Session and its SyncClient. */
struct Peer
{
    std::string name;
    Account account;
    std::unique_ptr<Session> session;
    std::unique_ptr<SyncClient> client;
    std::vector<std::string> messages; // status bar messages
    int remoteChanges = 0;

    Peer (std::string n, Account a, Session s, const std::string& projectId, int oneWayDelayMs = 0, fs::path pendingFile = {})
        : name (std::move (n)), account (std::move (a)), session (std::make_unique<Session> (std::move (s)))
    {
        collab::SyncOptions o;
        o.account = account;
        o.projectId = projectId;
        o.clientId = randomClientId();
        o.oneWayDelayMs = oneWayDelayMs;
        o.pendingFile = std::move (pendingFile);
        client = std::make_unique<SyncClient> (*session, std::move (o));
        client->onMessage = [this] (const std::string& m) { messages.push_back (m); };
        client->onRemoteChange = [this] { ++remoteChanges; };
    }

    Session& s() { return *session; }
    const model::ProjectModel& m() const { return session->model(); }
    bool synced() const { return client->state() == SyncState::synced; }
};

using Peers = std::vector<Peer*>;

inline std::string describe (const Peers& peers)
{
    std::string out;
    for (auto* p : peers)
        out += p->name + ": " + p->client->statusText() + ", pending " + std::to_string (p->client->pendingCount()) + ", heads "
               + std::to_string (p->s().document().heads().size() / 32)
               + (p->s().document().heads() == peers.front()->s().document().heads() ? " (same as first)" : " (differ)") + ", changes "
               + std::to_string (p->s().document().changesSince ({}).size()) + "\n";
    return out;
}

inline void pumpUntil (const Peers& peers, const std::string& what, const std::function<bool()>& done, int seconds = 30)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
    for (;;)
    {
        for (auto* p : peers)
            p->client->poll();
        if (done())
            return;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            INFO ("waiting for " << what << "\n" << describe (peers));
            FAIL();
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
}

/** Everyone joined, has the same heads, and nothing is pending. */
inline void waitConverged (const Peers& peers, int seconds = 60)
{
    pumpUntil (
        peers, "convergence",
        [&] {
            for (auto* p : peers)
                if (! p->synced() || p->s().document().heads() != peers.front()->s().document().heads())
                    return false;
            return true;
        },
        seconds);
}

/** A shared project: the owner's new local project uploaded to the relay. */
struct SharedProject
{
    RelayProcess& relay;
    Account ownerAccount;
    std::string projectId;
    std::unique_ptr<Peer> owner;

    explicit SharedProject (RelayProcess& r, int oneWayDelayMs = 0) : relay (r), ownerAccount (signUp (r, "Owner"))
    {
        auto s = Session::createNew ({}, ownerAccount.userId);
        REQUIRE (s.has_value());
        projectId = s->model().info.id;
        RelayApi api (ownerAccount);
        REQUIRE (api.shareProject (projectId, "Song"));
        owner = std::make_unique<Peer> ("Owner", ownerAccount, std::move (*s), projectId, oneWayDelayMs);
        pumpUntil ({ owner.get() }, "the upload", [&] { return owner->synced(); });
    }

    /** A new member invited with `role`, who opens the project from the relay. */
    std::unique_ptr<Peer> join (const std::string& name, const std::string& role = "editor", int oneWayDelayMs = 0, fs::path pendingFile = {})
    {
        auto account = signUp (relay, name);
        RelayApi ownerApi (ownerAccount);
        const auto invite = ownerApi.createInvite (projectId, role);
        REQUIRE (invite);
        RelayApi api (account);
        const auto parsed = collab::parseInviteLink (invite->url);
        REQUIRE (parsed);
        REQUIRE (api.acceptInvite (parsed->second));
        const auto bytes = api.fetchDocument (projectId);
        REQUIRE (bytes);
        auto document = doc::Document::load (*bytes);
        REQUIRE (document);
        auto peer = std::make_unique<Peer> (name, account, Session (std::move (*document), account.userId), projectId, oneWayDelayMs,
                                            std::move (pendingFile));
        waitConverged ({ owner.get(), peer.get() });
        return peer;
    }
};

/** Takes both peers offline, runs each one's edit, and brings them back (concurrent edits). */
inline void concurrently (Peer& a, Peer& b, const std::function<void (Peer&)>& editA, const std::function<void (Peer&)>& editB)
{
    a.client->setOnline (false);
    b.client->setOnline (false);
    editA (a);
    editB (b);
    a.client->setOnline (true);
    b.client->setOnline (true);
    waitConverged ({ &a, &b });
}
} // namespace weft::test
