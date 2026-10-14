// AT-TMP-1, AT-TMP-4 and the quantize math (spec 02 W-5).

#include "weft/time/Quantize.h"
#include "weft/time/SignatureMap.h"
#include "weft/time/TempoMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace weft;

TEST_CASE ("AT-TMP-1: step tempo map converts ticks to samples and back", "[U][tempo][AT-TMP-1]")
{
    const TempoMap map ({ { "initial", 0, 120.0 }, { "slow", 3840, 60.0 } }, 48000.0);
    CHECK (map.tickToSample (3840) == 96000.0);
    CHECK (map.tickToSample (4800) == 144000.0);
    CHECK (map.sampleToTick (96000.0) == 3840.0);
    CHECK (map.sampleToTick (144000.0) == 4800.0);
    CHECK (map.bpmAt (3839) == 120.0);
    CHECK (map.bpmAt (3840) == 60.0);
    for (double t : { 0.0, 1.0, 959.5, 3840.0, 5000.25, 123456.0 })
        CHECK (map.sampleToTick (map.tickToSample (t)) == Catch::Approx (t).margin (1e-9));
}

TEST_CASE ("Tempo map adds a missing initial point and clamps BPM", "[U][tempo]")
{
    const TempoMap map ({ { "late", 960, 5000.0 } }, 48000.0);
    REQUIRE (map.points().size() == 2);
    CHECK (map.points()[0].bpm == 120.0);
    CHECK (map.points()[1].bpm == TempoMap::maxBpm);
}

TEST_CASE ("AT-TMP-4: bar starts across signature changes", "[U][tempo][AT-TMP-4]")
{
    const SignatureMap map ({ { "initial", 0, 4, 4 }, { "a", 2, 3, 4 }, { "b", 4, 6, 8 } });
    const Ticks expected[] = { 0, 3840, 7680, 10560, 13440, 16320 };
    for (int bar = 0; bar < 6; ++bar)
        CHECK (map.barStart (bar) == expected[bar]);
    CHECK (map.barLength (4) == 2880);
    CHECK (map.barAt (10560).bar == 3);
    CHECK (map.barAt (10559).bar == 2);
    CHECK (map.barAt (13440 + 2880 * 3 + 5).bar == 7);
    CHECK (map.barAt (13440 + 2880 * 3 + 5).offset == 5);
}

TEST_CASE ("Signature validation", "[U][tempo]")
{
    CHECK (SignatureMap::isValid (7, 8));
    CHECK_FALSE (SignatureMap::isValid (0, 4));
    CHECK_FALSE (SignatureMap::isValid (4, 3));
    CHECK_FALSE (SignatureMap::isValid (33, 4));
}

TEST_CASE ("Quantize: nearest grid, strength and swing", "[U][quantize]")
{
    CHECK (quantizeTarget (130, 240, 0.0) == 240);
    CHECK (quantizeTarget (110, 240, 0.0) == 0);
    CHECK (quantizeTarget (120, 240, 0.0) == 0); // ties go earlier
    CHECK (quantizeTick (100, 240, 0.5, 0.0) == 50);
    CHECK (quantizeTick (100, 240, 0.0, 0.0) == 100);
    // Full swing moves odd grid points a third of a grid later (triplet feel).
    CHECK (quantizeTarget (240, 240, 100.0) == 320);
    CHECK (quantizeTarget (480, 240, 100.0) == 480);
}
