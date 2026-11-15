#pragma once

#include "kiebitz/gamedb/database.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace kiebitz::gamedb {

struct ImportProgress {
    std::uint64_t bytes = 0;   ///< read from the input so far
    std::uint64_t games = 0;   ///< stored so far
    std::uint64_t damaged = 0; ///< games kept only up to a broken move
    std::uint64_t skipped = 0; ///< variants and unreadable positions, not stored
};

struct ImportOptions {
    unsigned threads = 0; ///< games are read on this many threads; 0 = one per core
    /// Called now and then on a worker thread.
    std::function<void(const ImportProgress&)> progress;
    /// Set it to stop the import (it then returns `cancelled`).
    const std::atomic<bool>* cancel = nullptr;
};

struct ImportResult {
    ImportProgress counts;
    bool cancelled = false;
    std::string error; ///< empty unless the input couldn't be read
};

/// Reads PGN from `read` (which fills a buffer and returns how much it filled, 0 at the end)
/// and adds every game to `writer`, in the order of the input. Any size: the input is read
/// piece by piece and never held whole. The writer is not finished.
ImportResult importPgn(DatabaseWriter& writer,
                       const std::function<std::size_t(char* buffer, std::size_t size)>& read,
                       const ImportOptions& options = {});

/// Reads PGN files one after another, each plain or zstd-compressed (".zst", as Lichess's
/// downloads are; told by its first bytes, not its name), as the input of importPgn().
/// `bytesRead` (optional) counts the bytes read from the files so far (compressed bytes for
/// a compressed one), for progress.
class PgnFilesReader {
public:
    explicit PgnFilesReader(std::vector<std::string> paths,
                            std::atomic<std::uint64_t>* bytesRead = nullptr);
    ~PgnFilesReader();
    PgnFilesReader(const PgnFilesReader&) = delete;
    PgnFilesReader& operator=(const PgnFilesReader&) = delete;
    PgnFilesReader(PgnFilesReader&&) = delete;
    PgnFilesReader& operator=(PgnFilesReader&&) = delete;

    /// Fills `buffer` with up to `size` bytes of PGN; 0 at the end of the last file.
    std::size_t read(char* buffer, std::size_t size);
    /// Empty, or why a file couldn't be read (missing, damaged, cut off).
    [[nodiscard]] const std::string& error() const { return m_error; }

private:
    bool openNext();
    void closeFile();

    std::vector<std::string> m_paths;
    std::size_t m_next = 0;
    std::FILE* m_file = nullptr;
    void* m_zstd = nullptr; ///< ZSTD_DCtx, when the file is compressed
    bool m_frameEnded = true;
    std::vector<char> m_input;
    std::size_t m_inputPos = 0;
    std::size_t m_inputSize = 0;
    bool m_separator = false; ///< a newline still owed between two files
    std::atomic<std::uint64_t>* m_bytesRead;
    std::string m_error;
};

} // namespace kiebitz::gamedb
