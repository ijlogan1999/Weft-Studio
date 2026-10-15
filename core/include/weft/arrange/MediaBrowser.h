#pragma once

// The project media browser (F-LIB-2): every audio file the project knows or refers to,
// with how many regions use it.

#include "weft/model/ProjectModel.h"

#include <string>
#include <vector>

namespace weft::arrange
{
struct MediaUsage
{
    std::string hash;
    std::string name;     // original file name, or "(missing)" when only regions refer to it
    double seconds = 0.0; // duration at its sample rate
    int regions = 0;      // regions that play it
};

/** Sorted by name, then hash. */
std::vector<MediaUsage> mediaUsage (const model::ProjectModel& m);
} // namespace weft::arrange
