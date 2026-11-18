#include "kiebitz/library/file_kind.hpp"

#include <array>
#include <cctype>

namespace kiebitz::library {
namespace {

constexpr std::array<std::string_view, 5> kNames{"Game", "Repertoire", "Tournament", "Puzzle",
                                                 "Other"};

bool sameText(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

constexpr std::array<std::pair<std::string_view, FileKind>, 12> kFolders{{
    {"Repertoires", FileKind::Repertoire},
    {"Repertoire", FileKind::Repertoire},
    {"Repertoarer", FileKind::Repertoire}, // Norwegian
    {"Puzzles", FileKind::Puzzle},
    {"Aufgaben", FileKind::Puzzle},
    {"Oppgaver", FileKind::Puzzle},
    {"Tournaments", FileKind::Tournament},
    {"Turniere", FileKind::Tournament},
    {"Turneringer", FileKind::Tournament},
    {"Games", FileKind::Game},
    {"Partien", FileKind::Game},
    {"Partier", FileKind::Game},
}};
constexpr std::array<std::string_view, 4> kWhiteWords{"White", "Weiß", "Weiss", "Hvit"};
constexpr std::array<std::string_view, 3> kBlackWords{"Black", "Schwarz", "Svart"};

bool isLetter(char c) noexcept {
    // Bytes of UTF-8 letters (ß, ø) count as letters, so "Weißer" is not "Weiß".
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           static_cast<unsigned char>(c) >= 0x80;
}

/// Whether `text` has one of `words` as a whole word (any case).
template <std::size_t N>
bool hasWord(std::string_view text, const std::array<std::string_view, N>& words) {
    for (const std::string_view word : words) {
        for (std::size_t at = 0; at + word.size() <= text.size(); ++at) {
            if (sameText(text.substr(at, word.size()), word) &&
                (at == 0 || !isLetter(text[at - 1])) &&
                (at + word.size() == text.size() || !isLetter(text[at + word.size()]))) {
                return true;
            }
        }
    }
    return false;
}

/// The kind the headers say, if they say one.
std::optional<FileKind> declaredKind(std::span<const pgn::GameEntry> games) {
    for (const auto& game : games) {
        if (auto kind = kindFromName(game.header(kFileTypeHeader))) {
            return kind;
        }
    }
    for (const auto& game : games) {
        if (!game.header("Repertoire").empty()) {
            return FileKind::Repertoire;
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view nameOf(FileKind kind) noexcept {
    return kNames[static_cast<std::size_t>(kind)];
}

std::optional<FileKind> kindFromName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (sameText(name, kNames[i])) {
            return static_cast<FileKind>(i);
        }
    }
    return std::nullopt;
}

FileKind kindOf(std::span<const pgn::GameEntry> games) {
    return declaredKind(games).value_or(FileKind::Game);
}

FileKind kindOf(std::span<const pgn::GameEntry> games, std::string_view path) {
    if (const auto kind = declaredKind(games)) {
        return *kind;
    }
    return folderKind(path).value_or(FileKind::Game);
}

std::optional<FileKind> folderKind(std::string_view path) {
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt; // at the top
    }
    const std::string_view top = path.substr(0, slash);
    for (const auto& [name, kind] : kFolders) {
        if (sameText(top, name)) {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<core::Color> folderSide(std::string_view path) {
    // The folders, the outermost first: "White/Against Black's Dutch" is White's.
    std::size_t start = 0;
    for (std::size_t slash = path.find('/'); slash != std::string_view::npos;
         slash = path.find('/', start)) {
        const std::string_view folder = path.substr(start, slash - start);
        const bool white = hasWord(folder, kWhiteWords);
        const bool black = hasWord(folder, kBlackWords);
        if (white != black) {
            return white ? core::Color::White : core::Color::Black;
        }
        start = slash + 1;
    }
    return std::nullopt;
}

} // namespace kiebitz::library
