#include "weft/core/Version.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("Product name and version are set", "[U]")
{
    CHECK (weft::productName == "Weft Studio");
    CHECK_FALSE (weft::versionString().empty());
}
