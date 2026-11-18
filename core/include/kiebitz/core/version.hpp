#pragma once

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kiebitz::core {

/// The application version, taken from the CMake project version.
[[nodiscard]] std::string_view version() noexcept;

/// A version number: 1.0.0, or a release tag such as "v0.3.0".
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const Version&) const = default;
};
/// "1.2.3" or "v1.2.3" (anything after the numbers, as in "1.2.3-rc1", is ignored); nullopt if
/// it doesn't start with three numbers.
[[nodiscard]] std::optional<Version> parseVersion(std::string_view text);

/// A release as GitHub lists it.
struct Release {
    std::string tag;
    bool prerelease = false; ///< or a draft
};
/// Which of `releases` to offer as an update to `current`: the newest one that is newer. While
/// `current` is below 1.0 pre-releases count too (there are no others yet); after that only
/// proper releases. nullopt: up to date.
[[nodiscard]] std::optional<std::size_t> pickUpdate(const Version& current,
                                                    const std::vector<Release>& releases);

} // namespace kiebitz::core
