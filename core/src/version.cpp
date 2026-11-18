#include "kiebitz/core/version.hpp"

#include <array>
#include <charconv>

namespace kiebitz::core {

std::string_view version() noexcept {
    return KIEBITZ_VERSION;
}

std::optional<Version> parseVersion(std::string_view text) {
    if (text.starts_with('v') || text.starts_with('V')) {
        text.remove_prefix(1);
    }
    std::array<int, 3> parts{};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (!text.starts_with('.')) {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                                  parts[static_cast<std::size_t>(i)]);
        if (error != std::errc() || parts[static_cast<std::size_t>(i)] < 0) {
            return std::nullopt;
        }
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    }
    return Version{parts[0], parts[1], parts[2]};
}

std::optional<std::size_t> pickUpdate(const Version& current,
                                      const std::vector<Release>& releases) {
    const bool takePre = current.major == 0;
    std::optional<std::size_t> best;
    Version bestVersion = current;
    for (std::size_t i = 0; i < releases.size(); ++i) {
        if (releases[i].prerelease && !takePre) {
            continue;
        }
        const auto version = parseVersion(releases[i].tag);
        if (version && *version > bestVersion) {
            best = i;
            bestVersion = *version;
        }
    }
    return best;
}

} // namespace kiebitz::core
