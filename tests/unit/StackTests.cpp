// Track stacks (D-W6-17): spec 06 AT-TRK-25.

#include "Fixtures.h"

using namespace weft;
using namespace weft::test;

TEST_CASE ("AT-TRK-25: folder and summing stacks: create, open and close, flatten", "[U][tracks][stacks][AT-TRK-25]")
{
    auto s = newSession();
    const auto a = firstTrackOfKind (s, TrackKind::audio);
    const auto b = s.createTrack (TrackKind::audio, "B").value();
    const auto c = s.createTrack (TrackKind::audio, "C").value();
    const auto bus = s.createTrack (TrackKind::bus, "Verb").value();
    REQUIRE (s.setTrackOutput (a, bus));

    SECTION ("a summing stack: the main above its members, which play into it; it plays where the first did")
    {
        const auto main = s.createStack ({ c, a }, true).value();
        CHECK (s.undoName() == "Create Summing Stack");
        const auto* m = s.model().track (main);
        CHECK (m->name == "Stack 1");
        CHECK (m->stack == "summing");
        CHECK (m->output.busId == bus);
        CHECK (s.model().trackOrder == std::vector<Id> { main, a, c, b, bus });
        CHECK (s.model().stackMembers (main) == std::vector<Id> { a, c });
        CHECK (s.model().track (a)->output.busId == main);
        CHECK (s.model().track (c)->output.busId == main);
        CHECK (s.model().stackMain (a)->id == main);
        // Tracks in a stack don't join another.
        CHECK_FALSE (s.createStack ({ a }, false));

        REQUIRE (s.setStackOpen (main, false));
        CHECK (s.model().rows == std::vector<Id> { main, b, bus });
        REQUIRE (s.setStackOpen (main, true));
        CHECK (s.model().rows.size() == 5);

        REQUIRE (s.flattenStack (main));
        CHECK (s.model().track (main) == nullptr);
        CHECK (s.model().track (a)->output.busId == bus);
        CHECK (s.model().track (c)->output.busId == bus);
        CHECK (s.model().stackMain (a) == nullptr);
        REQUIRE (s.undo().done);
        CHECK (s.model().track (a)->output.busId == main);
        REQUIRE (s.undo().done); // the open / close steps, then the stack itself
        REQUIRE (s.undo().done);
        REQUIRE (s.undo().done);
        CHECK (s.model().track (main) == nullptr);
        CHECK (s.model().track (a)->output.busId == bus);
        CHECK (s.model().trackOrder == std::vector<Id> { a, b, c, bus });
    }
    SECTION ("a folder stack changes no routing and takes no outputs or sends")
    {
        const auto main = s.createStack ({ b, c }, false).value();
        CHECK (s.undoName() == "Create Folder Stack");
        CHECK (s.model().track (main)->isFolderStack());
        CHECK (s.model().track (b)->output.toMaster());
        CHECK (s.model().trackOrder == std::vector<Id> { a, main, b, c, bus });
        CHECK_FALSE (s.setTrackOutput (a, main));
        CHECK_FALSE (s.addSend (a, main));
        REQUIRE (s.flattenStack (main));
        CHECK (s.model().trackOrder == std::vector<Id> { a, b, c, bus });
    }
}
