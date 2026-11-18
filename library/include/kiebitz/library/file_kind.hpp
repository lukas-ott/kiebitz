#pragma once

#include "kiebitz/pgn/pgn.hpp"

#include <optional>
#include <span>
#include <string_view>

/// What kind of file a PGN is, as En Croissant has it: games, a repertoire, a tournament,
/// puzzles or anything else. Stored in the file itself, as a header in its games
/// ([FileType "Repertoire"]), so it travels with the file.
namespace kiebitz::library {

enum class FileKind : std::uint8_t { Game, Repertoire, Tournament, Puzzle, Other };

/// The header that holds it.
inline constexpr std::string_view kFileTypeHeader = "FileType";

/// "Game", "Repertoire", ... (the header's value).
[[nodiscard]] std::string_view nameOf(FileKind kind) noexcept;
/// The kind a header value names (any case), if it names one.
[[nodiscard]] std::optional<FileKind> kindFromName(std::string_view name) noexcept;

/// A file's kind from its games' headers: the first game with [FileType] decides; files
/// marked by an earlier Kiebitz ([Repertoire "..."]) are repertoires; everything else is games.
[[nodiscard]] FileKind kindOf(std::span<const pgn::GameEntry> games);
/// The same, for a file at `path` in the Library (relative: "Repertoires/Najdorf.pgn"): one
/// without a kind in its headers takes its top folder's (see folderKind), else is games.
[[nodiscard]] FileKind kindOf(std::span<const pgn::GameEntry> games, std::string_view path);

/// The kind the Library's top folder of `path` (relative) stands for, in the suggested layout:
/// "Repertoires", "Puzzles", "Tournaments" and "Games" (any case, also in German and
/// Norwegian). None for a file at the top or in any other folder.
[[nodiscard]] std::optional<FileKind> folderKind(std::string_view path);
/// The side a folder of `path` (relative) names: "White" or "Black" as a word in it ("White
/// Repertoire (Nc3)", also Weiß/Schwarz and Hvit/Svart), the outermost folder that says one.
/// None if none does.
[[nodiscard]] std::optional<core::Color> folderSide(std::string_view path);

} // namespace kiebitz::library
