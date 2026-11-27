// Groups and VCA faders (D-W6-15, D-W6-16): spec 06 AT-MIX-20 and AT-MIX-21.

#include "Fixtures.h"

#include <catch2/catch_approx.hpp>

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-MIX-20: a group's members follow each other's linked controls, as one undo step", "[U][mixer][groups][AT-MIX-20]")
{
    auto s = newSession();
    const auto a = firstTrackOfKind (s, TrackKind::audio);
    const auto b = s.createTrack (TrackKind::audio, "B").value();
    const auto c = s.createTrack (TrackKind::audio, "C").value();
    const auto outside = s.createTrack (TrackKind::audio, "D").value();
    REQUIRE (s.setTrackVolume (b, -10.0));
    const auto g = s.createGroup ({ a, b, c }).value();
    CHECK (s.model().groups.at (g)->name == "Group 1");
    CHECK (s.model().groupMembers (g) == std::vector<Id> { a, b, c });
    CHECK (s.undoName() == "Create Group");

    SECTION ("volume moves together and keeps the members' relative levels")
    {
        REQUIRE (s.setTrackVolume (a, -6.0));
        CHECK (s.model().track (b)->strip.volumeDb == Catch::Approx (-16.0));
        CHECK (s.model().track (c)->strip.volumeDb == Catch::Approx (-6.0));
        CHECK (s.model().track (outside)->strip.volumeDb == 0.0);
        REQUIRE (s.undo().done);
        CHECK (s.model().track (a)->strip.volumeDb == 0.0);
        CHECK (s.model().track (b)->strip.volumeDb == Catch::Approx (-10.0));
        // A member at the top stops there.
        REQUIRE (s.setTrackVolume (b, 0.0));
        CHECK (s.model().track (a)->strip.volumeDb == Catch::Approx (6.0));
    }
    SECTION ("mute links by default; pan, colour, automation mode and hide only when ticked")
    {
        REQUIRE (s.setTrackMute (c, true));
        CHECK (s.model().track (a)->mute);
        CHECK (s.model().track (b)->mute);
        CHECK_FALSE (s.model().track (outside)->mute);
        REQUIRE (s.setTrackPan (a, -0.5));
        CHECK (s.model().track (b)->strip.pan == 0.0);

        auto settings = *s.model().groups.at (g);
        settings.linkPan = settings.linkColor = settings.linkAutomationMode = settings.linkHide = true;
        settings.linkMute = false;
        REQUIRE (s.setGroup (settings));
        REQUIRE (s.setTrackPan (b, 0.25));
        CHECK (s.model().track (a)->strip.pan == Catch::Approx (-0.25));
        CHECK (s.model().track (c)->strip.pan == Catch::Approx (0.25));
        model::Color red;
        red.palette = 3;
        REQUIRE (s.setTrackColor (a, red));
        CHECK (s.model().track (c)->color.palette == 3);
        REQUIRE (s.setTrackAutomationMode (a, "latch"));
        CHECK (s.model().track (b)->automationMode == "latch");
        REQUIRE (s.setTrackMute (a, false));
        CHECK (s.model().track (b)->mute); // mute no longer linked
        REQUIRE (s.setTrackHidden (a, true));
        CHECK (s.model().track (c)->hidden);
    }
    SECTION ("linked send slots keep their relative levels")
    {
        const auto bus = s.createTrack (TrackKind::bus, "Verb").value();
        REQUIRE (s.addSend (a, bus, -10.0));
        REQUIRE (s.addSend (b, bus, -20.0));
        auto settings = *s.model().groups.at (g);
        settings.linkSends = 1; // slot 1
        REQUIRE (s.setGroup (settings));
        REQUIRE (s.setSendLevel (a, s.model().track (a)->strip.sends[0].id, -4.0));
        CHECK (s.model().track (b)->strip.sends[0].levelDb == Catch::Approx (-14.0));
    }
    SECTION ("the override, a group turned off and groups turned off each stop the links")
    {
        s.setGroupOverride (true);
        REQUIRE (s.setTrackMute (a, true));
        CHECK_FALSE (s.model().track (b)->mute);
        s.setGroupOverride (false);
        REQUIRE (s.setTrackMute (a, false));

        auto settings = *s.model().groups.at (g);
        settings.enabled = false;
        REQUIRE (s.setGroup (settings));
        REQUIRE (s.setTrackMute (a, true));
        CHECK_FALSE (s.model().track (b)->mute);
        settings.enabled = true;
        REQUIRE (s.setGroup (settings));
        REQUIRE (s.setTrackMute (a, false));

        REQUIRE (s.setGroupsActive (false));
        REQUIRE (s.setTrackMute (a, true));
        CHECK_FALSE (s.model().track (b)->mute);
        CHECK (s.undoName() == "Mute Track");
    }
    SECTION ("H hides the members' rows; deleting the group frees its tracks")
    {
        auto settings = *s.model().groups.at (g);
        settings.hidden = true;
        REQUIRE (s.setGroup (settings));
        CHECK (s.model().rows.size() == 1); // only D
        REQUIRE (s.setGroupsActive (false));
        CHECK (s.model().rows.size() == 4);
        REQUIRE (s.setGroupsActive (true));
        REQUIRE (s.deleteGroup (g));
        CHECK (s.model().groups.empty());
        CHECK (s.model().track (a)->groupId.empty());
        CHECK (s.model().rows.size() == 4);
        REQUIRE (s.undo().done);
        CHECK (s.model().track (a)->groupId == g);
    }
    SECTION ("new groups take the lowest free number; tracks move between groups")
    {
        const auto g2 = s.createGroup ({ a }).value();
        CHECK (s.model().groups.at (g2)->number == 2);
        CHECK (s.model().track (a)->groupId == g2);
        REQUIRE (s.deleteGroup (g));
        const auto g3 = s.createGroup().value();
        CHECK (s.model().groups.at (g3)->number == 1);
        REQUIRE (s.setTrackGroup (outside, g3));
        CHECK (s.model().groupMembers (g3) == std::vector<Id> { outside });
        CHECK_FALSE (s.setTrackGroup (outside, g3));
        CHECK_FALSE (s.setTrackGroup (s.model().info.masterTrackId, g3));
    }
}

TEST_CASE ("AT-MIX-21: VCA faders are made, renamed, assigned and deleted", "[U][mixer][vca][AT-MIX-21]")
{
    auto s = newSession();
    const auto a = firstTrackOfKind (s, TrackKind::audio);
    const auto v = s.createVca().value();
    CHECK (s.model().vcas.at (v)->name == "VCA 1");
    CHECK (s.model().vcas.at (v)->volumeDb == 0.0);
    REQUIRE (s.setTrackVca (a, v));
    CHECK (s.model().track (a)->vcaId == v);
    REQUIRE (s.setVcaVolume (v, -6.0));
    CHECK_FALSE (s.setVcaVolume (v, 7.0));
    REQUIRE (s.setVcaMute (v, true));
    REQUIRE (s.renameVca (v, "Drums"));
    CHECK (s.model().vcasInOrder().front()->name == "Drums");
    CHECK_FALSE (s.setTrackVca (s.model().info.masterTrackId, v));
    REQUIRE (s.deleteVca (v));
    CHECK (s.model().track (a)->vcaId.empty());
    REQUIRE (s.undo().done);
    CHECK (s.model().track (a)->vcaId == v);
    CHECK (s.model().vcas.at (v)->volumeDb == -6.0);
}
