#pragma once

// Shared helpers for model, command and undo tests.

#include "weft/commands/Session.h"

#include <catch2/catch_test_macros.hpp>

namespace weft::test
{
using commands::Session;
using model::TrackKind;

inline std::int64_t fixedClock() { return 1'700'000'000'000; }

inline Session newSession (std::uint64_t seed = 1, const std::string& user = "userA")
{
    auto s = Session::createNew ({}, user, nullptr, fixedClock, seed);
    REQUIRE (s.has_value());
    return std::move (*s);
}

/** A copy of another session's document, as a second collaborator would have it. */
inline Session joinSession (Session& other, std::uint64_t seed, const std::string& user)
{
    auto d = doc::Document::load (other.document().save());
    REQUIRE (d.has_value());
    return Session (std::move (*d), user, fixedClock, seed);
}

/** Exchanges all changes both ways. */
inline void sync (Session& a, Session& b)
{
    REQUIRE (b.applyChanges (a.document().allChanges()) == doc::Status::ok);
    REQUIRE (a.applyChanges (b.document().allChanges()) == doc::Status::ok);
}

inline Id firstTrackOfKind (const Session& s, TrackKind kind)
{
    for (const auto& id : s.model().trackOrder)
        if (s.model().track (id)->kind == kind)
            return id;
    return {};
}

/** A small project with one of everything the edit tests need. Ids are recorded for the tests. */
struct Scene
{
    Session session = newSession();
    Id audioTrack, instTrack, bus, mediaHash = "b3:0000000000000000000000000000000000000000000000000000000000000001";
    Id audioRegion, midiRegion, note, plugin, send, lane;

    Scene()
    {
        audioTrack = firstTrackOfKind (session, TrackKind::audio);
        instTrack = session.createTrack (TrackKind::instrument, "Keys").value();
        bus = session.createTrack (TrackKind::bus, "Verb").value();
        model::MediaRef m;
        m.hash = mediaHash;
        m.originalName = "take.wav";
        m.lengthSamples = 48000 * 20;
        REQUIRE (session.addMedia (m));
        audioRegion = session.createAudioRegion (audioTrack, 3840, mediaHash, 0, 48000 * 4).value();
        midiRegion = session.createMidiRegion (instTrack, 0, 7680).value();
        for (int i = 0; i < 8; ++i)
        {
            model::Note n;
            n.pos = i * 960 + 13;
            n.pitch = 60 + i;
            const auto id = session.addNote (midiRegion, n).value();
            if (i == 0)
                note = id;
        }
        model::PluginInstance p;
        p.format = "builtin";
        p.pluginUid = "weft.eq";
        p.name = "EQ";
        plugin = session.addInsert (instTrack, p).value();
        send = session.addSend (instTrack, bus, -10.0).value();
        session.addAutomationPoint (instTrack, {}, 0, 0.5);
        lane = session.model().automationLanes.begin()->first;
    }
};
} // namespace weft::test
