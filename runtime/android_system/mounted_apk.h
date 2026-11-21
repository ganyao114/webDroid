#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <unistd.h>

namespace wa::system {

class MountedApk {
  public:
    static constexpr char kPath[] = "/tmp/wa-art-XXXXXX";

    explicit MountedApk(std::span<const uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
            throw std::invalid_argument("Invalid APK size");
        const int fd = mkstemp(path_);
        if (fd < 0) throw std::runtime_error("Cannot create APK backing file");
        while (!bytes.empty()) {
            const auto count = write(fd, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                close(fd);
                unlink(path_);
                throw std::runtime_error("Cannot write APK backing file");
            }
            bytes = bytes.subspan(count);
        }
        if (close(fd) != 0) {
            unlink(path_);
            throw std::runtime_error("Cannot close APK backing file");
        }
    }

    ~MountedApk() { unlink(path_); }
    MountedApk(const MountedApk&) = delete;
    MountedApk& operator=(const MountedApk&) = delete;
    const char* path() const { return path_; }

  private:
    char path_[sizeof(kPath)] = "/tmp/wa-art-XXXXXX";
};

}  // namespace wa::system
