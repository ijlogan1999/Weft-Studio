// CRDT load benchmark (ARCHITECTURE.md M1): a project with 200 tracks, 5,000
// regions and 200,000 notes loads (document + projection) in at most 3 s.
//
// Registered with CTest only in Release builds. The 3 s limit is enforced when
// WEFT_BENCH_ENFORCE=1 (the Linux CI job sets it); otherwise the time is reported.

#include "weft/commands/Session.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace weft;
using doc::Value;

namespace
{
doc::Bytes buildLargeProject()
{
    auto session = commands::Session::createNew ({}, "bench", nullptr, [] { return std::int64_t { 1'700'000'000'000 }; }, 11);
    REQUIRE (session);
    auto& d = session->document();
    IdGenerator ids (5);

    Value tracks = d.get (doc::path ({ "tracks" })).value();
    Value::List order;
    Value regions = Value::map(), sequences = Value::map(), media = Value::map();
    const std::string hash = "b3:" + std::string (64, 'a');
    media.set (hash, Value::map().set ("originalName", "loop.wav").set ("lengthSamples", 48000 * 600).set ("channels", 2));

    for (int t = 0; t < 200; ++t)
    {
        const bool midi = t % 2 == 1;
        const auto trackId = ids.next();
        tracks.set (trackId, Value::map()
                                 .set ("id", trackId)
                                 .set ("kind", midi ? "instrument" : "audio")
                                 .set ("name", (midi ? "Inst " : "Audio ") + std::to_string (t))
                                 .set ("color", t % 24)
                                 .set ("channels", 2)
                                 .set ("output", Value::map().set ("kind", "master"))
                                 .set ("strip", Value::map().set ("volumeDb", -6.0).set ("pan", 0.0).set ("inserts", Value::list()).set ("sends", Value::list())));
        order.emplace_back (trackId);

        for (int r = 0; r < 25; ++r) // 200 x 25 = 5,000 regions
        {
            const auto regionId = ids.next();
            auto region = Value::map()
                              .set ("id", regionId)
                              .set ("trackId", trackId)
                              .set ("kind", midi ? "midi" : "audio")
                              .set ("start", r * 7680)
                              .set ("length", midi ? 7680 : 96000)
                              .set ("z", 1)
                              .set ("muted", false)
                              .set ("createdBy", "bench")
                              .set ("modifiedBy", "bench")
                              .set ("modifiedAt", 0);
            if (midi)
            {
                const auto seqId = ids.next();
                Value notes = Value::map();
                for (int n = 0; n < 80; ++n) // 2,500 MIDI regions x 80 = 200,000 notes
                    notes.set (ids.next(), Value::map()
                                               .set ("pos", n * 96)
                                               .set ("length", 90)
                                               .set ("pitch", 36 + (n % 48))
                                               .set ("velocity", 100)
                                               .set ("releaseVelocity", 64)
                                               .set ("channel", 1)
                                               .set ("muted", false));
                sequences.set (seqId, Value::map().set ("id", seqId).set ("notes", std::move (notes)).set ("controllers", Value::map()));
                region.set ("midi", Value::map().set ("sequenceId", seqId).set ("sequenceOffset", 0).set ("quantize", Value()));
            }
            else
            {
                region.set ("audio", Value::map()
                                         .set ("media", hash)
                                         .set ("sourceOffset", r * 1000)
                                         .set ("gainDb", 0.0)
                                         .set ("fadeIn", Value::map().set ("length", 0).set ("curve", "equalPower"))
                                         .set ("fadeOut", Value::map().set ("length", 0).set ("curve", "equalPower")));
            }
            regions.set (regionId, std::move (region));
        }
    }
    for (const auto& [key, value] : { std::pair<const char*, Value*> { "tracks", &tracks }, { "regions", &regions }, { "midiSequences", &sequences }, { "media", &media } })
        REQUIRE (d.put (doc::path ({ key }), *value) == doc::Status::ok);
    REQUIRE (d.put (doc::path ({ "trackOrder" }), Value (std::move (order))) == doc::Status::ok);
    REQUIRE (d.commit ("weft1|bench|Build", 0));
    return d.save();
}
} // namespace

TEST_CASE ("CRDT load benchmark: 200 tracks, 5,000 regions, 200,000 notes", "[bench][M1]")
{
    using clock = std::chrono::steady_clock;
    const auto bytes = buildLargeProject();

    const auto start = clock::now();
    auto loaded = doc::Document::load (bytes);
    REQUIRE (loaded);
    const auto afterLoad = clock::now();
    const auto model = model::Projector::project (*loaded);
    const auto end = clock::now();

    CHECK (model.tracks.size() == 202); // + the new project's Audio 1 and master
    CHECK (model.regions.size() == 5000);
    std::size_t notes = 0;
    for (const auto& [id, seq] : model.midiSequences)
        notes += seq->notes.size();
    CHECK (notes == 200000);

    const auto ms = [] (auto d) { return std::chrono::duration<double, std::milli> (d).count(); };
    const double total = ms (afterLoad - start) + ms (end - afterLoad);
    std::cout << "load benchmark: file " << bytes.size() / 1024 << " KiB, document load " << ms (afterLoad - start) << " ms, projection "
              << ms (end - afterLoad) << " ms, total " << total << " ms (limit 3000 ms)\n";
    const char* enforce = std::getenv ("WEFT_BENCH_ENFORCE");
    if (enforce != nullptr && std::string (enforce) == "1")
        CHECK (total <= 3000.0);
}
