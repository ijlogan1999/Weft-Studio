#pragma once

#include <string_view>

namespace weft
{
/** Product name shown in the UI and About box. */
inline constexpr std::string_view productName = "Weft Studio";

/** Semantic version of this build, e.g. "0.1.0". */
std::string_view versionString() noexcept;
} // namespace weft
