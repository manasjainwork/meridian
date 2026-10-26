#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include "meridian/journal.hpp"

namespace meridian::journal {

// Appends records to a file through a user-space buffer. CRC is computed here, off the engine thread.
// append() only copies into the buffer; write() happens when the buffer fills; sync() = write + fdatasync.
class FileWriter {
public:
    explicit FileWriter(const char* path, size_t buf_bytes = 1u << 16) {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        buf_.reserve(buf_bytes);
        cap_ = buf_bytes;
        if (fd_ >= 0) {
            const Journal::Header h = Journal::kRef;
            ok_ = ::write(fd_, &h, sizeof h) == static_cast<ssize_t>(sizeof h);
        }
    }
    ~FileWriter() { if (fd_ >= 0) ::close(fd_); }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool ok() const noexcept { return fd_ >= 0 && ok_; }

    void append(Record r) {
        r.crc = record_crc(r);
        const auto* p = reinterpret_cast<const unsigned char*>(&r);
        buf_.insert(buf_.end(), p, p + sizeof r);
        if (buf_.size() + sizeof r > cap_) flush();
    }
    void flush() {
        size_t off = 0;
        while (off < buf_.size()) {
            const ssize_t n = ::write(fd_, buf_.data() + off, buf_.size() - off);
            if (n <= 0) { ok_ = false; break; }
            off += static_cast<size_t>(n);
        }
        buf_.clear();
    }
    void sync() {
        flush();
        if (::fdatasync(fd_) != 0) ok_ = false;
    }

private:
    int fd_ = -1;
    bool ok_ = false;
    size_t cap_ = 0;
    std::vector<unsigned char> buf_;
};

}  // namespace meridian::journal
