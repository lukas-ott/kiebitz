#include "kiebitz/gamedb/import.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <zstd.h>

namespace kiebitz::gamedb {
namespace {

/// Games to read, as one block of text and where each game is in it.
struct Batch {
    std::uint64_t sequence = 0;
    std::string text;
    std::vector<std::pair<std::size_t, std::size_t>> games; ///< offset, length
    std::uint64_t bytes = 0; ///< input read up to the end of this batch
};

struct Scanned {
    std::vector<ScannedGame> games;
    std::uint64_t bytes = 0;
};

constexpr std::size_t kBatchGames = 2000;
constexpr std::size_t kBatchBytes = 4U << 20U;

} // namespace

ImportResult importPgn(DatabaseWriter& writer,
                       const std::function<std::size_t(char* buffer, std::size_t size)>& read,
                       const ImportOptions& options) {
    const unsigned threads =
        options.threads > 0 ? options.threads : std::max(1U, std::thread::hardware_concurrency());
    const auto cancelled = [&] {
        return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
    };

    std::mutex mutex;
    std::condition_variable work;          // a batch to read, or the end
    std::condition_variable room;          // the queues have room again
    std::condition_variable finished;      // a read batch for the writer
    std::deque<Batch> pending;             // batches to read
    std::map<std::uint64_t, Scanned> done; // read batches, by sequence
    bool inputEnded = false;
    bool stop = false;
    const std::size_t maxQueued = static_cast<std::size_t>(threads) * 3;

    // Readers: turn text into games.
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            while (true) {
                Batch batch;
                {
                    std::unique_lock lock(mutex);
                    work.wait(lock, [&] { return !pending.empty() || inputEnded || stop; });
                    if (stop || pending.empty()) {
                        return;
                    }
                    batch = std::move(pending.front());
                    pending.pop_front();
                    room.notify_all();
                }
                Scanned scanned;
                scanned.bytes = batch.bytes;
                scanned.games.reserve(batch.games.size());
                for (const auto& [offset, length] : batch.games) {
                    scanned.games.push_back(
                        scanGame(std::string_view(batch.text).substr(offset, length)));
                }
                const std::scoped_lock lock(mutex);
                done.emplace(batch.sequence, std::move(scanned));
                finished.notify_all();
            }
        });
    }

    // The writer: stores the games in the order of the input.
    ImportResult result;
    std::uint64_t sequences = 0; // batches handed out (known once the input ended)
    bool allHandedOut = false;
    std::thread storer([&] {
        std::uint64_t next = 0;
        while (true) {
            Scanned scanned;
            {
                std::unique_lock lock(mutex);
                finished.wait(lock, [&] {
                    return stop || done.contains(next) || (allHandedOut && next == sequences);
                });
                if (stop || !done.contains(next)) {
                    return;
                }
                scanned = std::move(done.at(next));
                done.erase(next);
                room.notify_all();
            }
            for (const ScannedGame& game : scanned.games) {
                if (game.skipped) {
                    ++result.counts.skipped;
                    continue;
                }
                writer.add(game);
                ++result.counts.games;
                if (game.damaged) {
                    ++result.counts.damaged;
                }
            }
            result.counts.bytes = scanned.bytes;
            if (options.progress) {
                options.progress(result.counts);
            }
            ++next;
        }
    });

    // This thread: read the input and cut it into batches of games.
    GameSplitter splitter;
    Batch batch;
    std::uint64_t bytes = 0;
    const auto handOut = [&] {
        if (batch.games.empty()) {
            return;
        }
        batch.sequence = sequences++;
        batch.bytes = bytes;
        std::unique_lock lock(mutex);
        room.wait(lock, [&] { return stop || (pending.size() + done.size()) < maxQueued; });
        pending.push_back(std::move(batch));
        batch = Batch();
        work.notify_one();
    };
    const GameSplitter::Sink sink = [&](std::string_view game) {
        batch.games.emplace_back(batch.text.size(), game.size());
        batch.text.append(game);
        if (batch.games.size() >= kBatchGames || batch.text.size() >= kBatchBytes) {
            handOut();
        }
    };
    std::vector<char> buffer(1U << 20U);
    while (!cancelled()) {
        const std::size_t got = read(buffer.data(), buffer.size());
        if (got == 0) {
            break;
        }
        bytes += got;
        splitter.feed(std::string_view(buffer.data(), got), sink);
    }
    if (cancelled()) {
        result.cancelled = true;
        const std::scoped_lock lock(mutex);
        stop = true;
    } else {
        splitter.finish(sink);
        handOut();
    }
    {
        const std::scoped_lock lock(mutex);
        inputEnded = true;
        allHandedOut = true;
        work.notify_all();
        finished.notify_all();
        room.notify_all();
    }
    for (auto& worker : workers) {
        worker.join();
    }
    {
        const std::scoped_lock lock(mutex);
        finished.notify_all();
    }
    storer.join();
    return result;
}

} // namespace kiebitz::gamedb

namespace kiebitz::gamedb {

PgnFilesReader::PgnFilesReader(std::vector<std::string> paths,
                               std::atomic<std::uint64_t>* bytesRead)
    : m_paths(std::move(paths)), m_input(1U << 20U), m_bytesRead(bytesRead) {}

PgnFilesReader::~PgnFilesReader() {
    closeFile();
}

void PgnFilesReader::closeFile() {
    if (m_file != nullptr) {
        std::fclose(m_file);
        m_file = nullptr;
    }
    if (m_zstd != nullptr) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(m_zstd));
        m_zstd = nullptr;
    }
    m_inputPos = 0;
    m_inputSize = 0;
}

bool PgnFilesReader::openNext() {
    closeFile();
    while (m_next < m_paths.size()) {
        const std::string& path = m_paths[m_next++];
        m_file = std::fopen(path.c_str(), "rb");
        if (m_file == nullptr) {
            m_error = "Couldn't read " + path + ".";
            return false;
        }
        m_inputSize = std::fread(m_input.data(), 1, m_input.size(), m_file);
        m_inputPos = 0;
        if (m_bytesRead != nullptr) {
            m_bytesRead->fetch_add(m_inputSize);
        }
        // A zstd frame (28 B5 2F FD), or a skippable one (5? 2A 4D 18).
        const auto byte = [&](std::size_t i) -> unsigned {
            return static_cast<unsigned char>(m_input[i]);
        };
        const bool frame = m_inputSize >= 4 && byte(0) == 0x28 && byte(1) == 0xB5 &&
                           byte(2) == 0x2F && byte(3) == 0xFD;
        const bool skippable = m_inputSize >= 4 && (byte(0) & 0xF0U) == 0x50 && byte(1) == 0x2A &&
                               byte(2) == 0x4D && byte(3) == 0x18;
        if (frame || skippable) {
            m_zstd = ZSTD_createDCtx();
            m_frameEnded = false;
        }
        m_separator = m_next > 1; // a game never runs from one file into the next
        return true;
    }
    return false;
}

std::size_t PgnFilesReader::read(char* buffer, std::size_t size) {
    if (size == 0 || !m_error.empty()) {
        return 0;
    }
    while (true) {
        if (m_file == nullptr && !openNext()) {
            return 0;
        }
        if (m_separator) {
            m_separator = false;
            buffer[0] = '\n';
            return 1;
        }
        if (m_inputPos == m_inputSize && m_zstd != nullptr && !m_frameEnded) {
            // All input taken, but some output may still be held back: let it out first.
            ZSTD_inBuffer in{m_input.data(), 0, 0};
            ZSTD_outBuffer out{buffer, size, 0};
            const std::size_t status =
                ZSTD_decompressStream(static_cast<ZSTD_DCtx*>(m_zstd), &out, &in);
            if (ZSTD_isError(status) != 0) {
                m_error = "A download is damaged: " + m_paths[m_next - 1] + ".";
                closeFile();
                return 0;
            }
            m_frameEnded = status == 0;
            if (out.pos > 0) {
                return out.pos;
            }
        }
        if (m_inputPos == m_inputSize) {
            m_inputSize = std::fread(m_input.data(), 1, m_input.size(), m_file);
            m_inputPos = 0;
            if (m_bytesRead != nullptr) {
                m_bytesRead->fetch_add(m_inputSize);
            }
            if (m_inputSize == 0) {
                if (m_zstd != nullptr && !m_frameEnded) {
                    m_error = "A download is incomplete: " + m_paths[m_next - 1] + ".";
                    closeFile();
                    return 0;
                }
                closeFile();
                continue; // the next file
            }
        }
        if (m_zstd == nullptr) {
            const std::size_t n = std::min(size, m_inputSize - m_inputPos);
            std::memcpy(buffer, m_input.data() + m_inputPos, n);
            m_inputPos += n;
            return n;
        }
        ZSTD_inBuffer in{m_input.data(), m_inputSize, m_inputPos};
        ZSTD_outBuffer out{buffer, size, 0};
        while (out.pos == 0 && in.pos < in.size) {
            const std::size_t status =
                ZSTD_decompressStream(static_cast<ZSTD_DCtx*>(m_zstd), &out, &in);
            if (ZSTD_isError(status) != 0) {
                m_error = "A download is damaged: " + m_paths[m_next - 1] + ".";
                closeFile();
                return 0;
            }
            m_frameEnded = status == 0;
        }
        m_inputPos = in.pos;
        if (out.pos > 0) {
            return out.pos;
        }
    }
}

} // namespace kiebitz::gamedb
