#pragma once

#include <QString>
#include <QUrl>

#include <cstdint>

class QSettings;

/// Reporting problems: a new GitHub issue already holding what helps (Kiebitz's version, macOS
/// and the processor; nothing personal), and noticing when Kiebitz didn't quit normally.
namespace kiebitz::app::problems {

/// The kind of report, which sets the issue's first lines.
enum class Kind : std::uint8_t {
    Problem, ///< Help → Report a Problem…
    Crash,   ///< Kiebitz quit unexpectedly last time
};

/// A new issue on Kiebitz's GitHub page, filled in with a template and this Mac's details.
[[nodiscard]] QUrl reportUrl(Kind kind);
/// The details the report carries: "Kiebitz 0.3.0 · macOS 26.6 · arm64".
[[nodiscard]] QString systemLine();

/// Marks Kiebitz as running and says whether the run before ended without quitting (a crash,
/// or a forced quit). main() calls it at the start, `finishRun` after a normal quit.
[[nodiscard]] bool startRun(QSettings& settings);
void finishRun(QSettings& settings);

/// macOS's report of Kiebitz's most recent crash (~/Library/Logs/DiagnosticReports), if any.
[[nodiscard]] QString latestCrashReport();

} // namespace kiebitz::app::problems
