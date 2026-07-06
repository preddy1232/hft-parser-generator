// =============================================================================
// itch_file_reader.hpp — Memory-mapped ITCH binary file reader
// =============================================================================
// Zero-copy, memory-mapped reader for NASDAQ's raw ITCH binary format.
// Each message is prefixed by a 2-byte big-endian length field (same as
// MoldUDP64 inner framing).
//
// Platform support:
//   - Linux/macOS: mmap(2)
//   - Windows:     CreateFileMapping / MapViewOfFile
//   - Fallback:    std::ifstream read into allocated buffer
//
// Usage:
//   itch50::ItchFileReader reader;
//   if (!reader.open("data.itch")) { /* handle error */ }
//
//   // Bulk processing:
//   MyHandler handler;
//   auto stats = reader.process_file(handler);
//
//   // Or iterate manually:
//   reader.reset();
//   while (auto msg = reader.next_message()) {
//       dispatch_message(handler, msg->type, msg->data);
//   }
// =============================================================================
#pragma once

#include "endian_utils.hpp"
#include "generated_itch_parser.hpp"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <chrono>

// Platform-specific includes for memory mapping
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#else
#  include <fstream>
#  include <vector>
#endif

namespace itch50 {

// ─────────────────────────────────────────────────────────────────────────────
// File reading statistics
// ─────────────────────────────────────────────────────────────────────────────
struct FileReaderStats {
    uint64_t messages_parsed  = 0;
    uint64_t bytes_processed  = 0;
    uint64_t total_file_bytes = 0;
    uint64_t unknown_messages = 0;
    double   elapsed_seconds  = 0.0;

    // Derived throughput metrics
    [[nodiscard]] double messages_per_second() const noexcept {
        return (elapsed_seconds > 0.0)
            ? static_cast<double>(messages_parsed) / elapsed_seconds
            : 0.0;
    }

    [[nodiscard]] double gigabytes_per_second() const noexcept {
        return (elapsed_seconds > 0.0)
            ? (static_cast<double>(bytes_processed) / (1024.0 * 1024.0 * 1024.0)) / elapsed_seconds
            : 0.0;
    }
};

namespace detail {

// ─────────────────────────────────────────────────────────────────────────────
// Platform-abstracted memory-mapped file
// ─────────────────────────────────────────────────────────────────────────────
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }

    // Non-copyable, movable
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept { swap(other); }
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) { close(); swap(other); }
        return *this;
    }

    [[nodiscard]] bool open(const char* path) noexcept {
        close();
#ifdef _WIN32
        return open_win32(path);
#elif defined(__linux__) || defined(__APPLE__)
        return open_posix(path);
#else
        return open_fallback(path);
#endif
    }

    void close() noexcept {
#ifdef _WIN32
        close_win32();
#elif defined(__linux__) || defined(__APPLE__)
        close_posix();
#else
        close_fallback();
#endif
    }

    [[nodiscard]] const char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

private:
    const char* data_ = nullptr;
    std::size_t size_ = 0;

    void swap(MappedFile& other) noexcept {
        auto tmp_data = data_; data_ = other.data_; other.data_ = tmp_data;
        auto tmp_size = size_; size_ = other.size_; other.size_ = tmp_size;
#ifdef _WIN32
        auto tmp_file = file_handle_; file_handle_ = other.file_handle_; other.file_handle_ = tmp_file;
        auto tmp_map = map_handle_; map_handle_ = other.map_handle_; other.map_handle_ = tmp_map;
#elif defined(__linux__) || defined(__APPLE__)
        auto tmp_fd = fd_; fd_ = other.fd_; other.fd_ = tmp_fd;
#else
        buffer_.swap(other.buffer_);
#endif
    }

    // ── Windows implementation ──────────────────────────────────────────
#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE map_handle_  = nullptr;

    bool open_win32(const char* path) noexcept {
        file_handle_ = CreateFileA(
            path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );
        if (file_handle_ == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(file_handle_, &file_size)) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        size_ = static_cast<std::size_t>(file_size.QuadPart);

        if (size_ == 0) {
            // Empty file — valid but nothing to map
            data_ = nullptr;
            return true;
        }

        map_handle_ = CreateFileMappingA(
            file_handle_, nullptr, PAGE_READONLY, 0, 0, nullptr
        );
        if (!map_handle_) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        data_ = static_cast<const char*>(
            MapViewOfFile(map_handle_, FILE_MAP_READ, 0, 0, 0)
        );
        if (!data_) {
            CloseHandle(map_handle_);
            CloseHandle(file_handle_);
            map_handle_ = nullptr;
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }

    void close_win32() noexcept {
        if (data_) { UnmapViewOfFile(data_); data_ = nullptr; }
        if (map_handle_) { CloseHandle(map_handle_); map_handle_ = nullptr; }
        if (file_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
        }
        size_ = 0;
    }

    // ── POSIX (Linux/macOS) implementation ──────────────────────────────
#elif defined(__linux__) || defined(__APPLE__)
    int fd_ = -1;

    bool open_posix(const char* path) noexcept {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;

        struct stat st{};
        if (fstat(fd_, &st) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {
            data_ = nullptr;
            return true;
        }

        void* ptr = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Hint the kernel for sequential access
        madvise(ptr, size_, MADV_SEQUENTIAL);

        data_ = static_cast<const char*>(ptr);
        return true;
    }

    void close_posix() noexcept {
        if (data_ && size_ > 0) { munmap(const_cast<char*>(data_), size_); data_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        size_ = 0;
    }

    // ── Portable fallback (std::ifstream) ───────────────────────────────
#else
    std::vector<char> buffer_;

    bool open_fallback(const char* path) noexcept {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) return false;

        auto file_size = ifs.tellg();
        if (file_size <= 0) {
            size_ = 0;
            data_ = nullptr;
            return true;
        }

        size_ = static_cast<std::size_t>(file_size);
        buffer_.resize(size_);
        ifs.seekg(0);
        ifs.read(buffer_.data(), static_cast<std::streamsize>(size_));
        if (!ifs) {
            buffer_.clear();
            size_ = 0;
            return false;
        }
        data_ = buffer_.data();
        return true;
    }

    void close_fallback() noexcept {
        buffer_.clear();
        buffer_.shrink_to_fit();
        data_ = nullptr;
        size_ = 0;
    }
#endif
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Raw message view (zero-copy pointer into mapped memory)
// ─────────────────────────────────────────────────────────────────────────────
struct RawMessage {
    char              type;     // ITCH message type character
    uint16_t          length;   // Message body length (excluding the 2-byte prefix)
    const char*       data;     // Pointer to the start of the message body
};

// ─────────────────────────────────────────────────────────────────────────────
// ItchFileReader — Memory-mapped ITCH binary file reader
// ─────────────────────────────────────────────────────────────────────────────
class ItchFileReader {
public:
    ItchFileReader() = default;

    /// Open an ITCH binary file for reading. Returns false on failure.
    [[nodiscard]] bool open(const char* path) noexcept {
        if (!file_.open(path)) return false;
        offset_ = 0;
        return true;
    }

    /// Close the file and release resources.
    void close() noexcept {
        file_.close();
        offset_ = 0;
    }

    /// Reset the read cursor to the beginning of the file.
    void reset() noexcept { offset_ = 0; }

    /// Check if the file is open and valid.
    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    /// Return total file size in bytes.
    [[nodiscard]] std::size_t file_size() const noexcept { return file_.size(); }

    /// Return current read offset.
    [[nodiscard]] std::size_t current_offset() const noexcept { return offset_; }

    /// Check if there are more messages to read.
    [[nodiscard]] bool has_more() const noexcept {
        return offset_ + 2 <= file_.size();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Iterator-style API: retrieve the next raw message.
    // Returns true and populates `out` on success, false at end/error.
    // ─────────────────────────────────────────────────────────────────────
    [[nodiscard]] bool next_message(RawMessage& out) noexcept {
        const char* buf = file_.data();
        const std::size_t len = file_.size();

        if (offset_ + 2 > len) return false;

        uint16_t msg_len = detail::read_be16(buf + offset_);
        if (msg_len == 0 || offset_ + 2 + msg_len > len) return false;

        out.length = msg_len;
        out.type   = buf[offset_ + 2];
        out.data   = buf + offset_ + 2;

        offset_ += 2 + msg_len;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Bulk processing: dispatch all messages through a Handler.
    //
    // The Handler must satisfy the CanHandle concept for any message types
    // it wishes to process (same pattern as parse_stream / DispatchTable).
    //
    // Returns statistics about the parsing run.
    // ─────────────────────────────────────────────────────────────────────
    template<typename Handler>
    [[nodiscard]] FileReaderStats process_file(Handler& handler) noexcept {
        FileReaderStats stats{};
        stats.total_file_bytes = file_.size();

        const auto t0 = std::chrono::steady_clock::now();

        reset();
        const char* buf = file_.data();
        const std::size_t len = file_.size();

        // Use DispatchTable for O(1) dispatch on the hot path
        DispatchTable<Handler> table;

        while (offset_ + 2 <= len) {
            uint16_t msg_len = detail::read_be16(buf + offset_);
            if (msg_len == 0 || offset_ + 2 + msg_len > len) break;

            char type = buf[offset_ + 2];

            if (table.dispatch(handler, type, buf + offset_ + 2)) {
                stats.messages_parsed++;
            } else {
                stats.unknown_messages++;
            }

            offset_ += 2 + msg_len;
        }

        stats.bytes_processed = offset_;

        const auto t1 = std::chrono::steady_clock::now();
        stats.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

        return stats;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Raw buffer access (for advanced use / integration with parse_stream)
    // ─────────────────────────────────────────────────────────────────────
    [[nodiscard]] const char* data() const noexcept { return file_.data(); }

private:
    detail::MappedFile file_;
    std::size_t        offset_ = 0;
};

} // namespace itch50
