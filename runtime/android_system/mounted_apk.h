#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
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

    struct BrowserFile {};
    MountedApk(BrowserFile, const char* path) {
        constexpr std::string_view prefix = "/tmp/wa-browser-apk-";
        const std::string_view name(path ? path : "");
        if (!name.starts_with(prefix) || name.size() == prefix.size() ||
                name.substr(prefix.size()).find_first_not_of("0123456789") != name.npos)
            throw std::invalid_argument("Invalid browser APK backing path");
        const int fd = mkstemp(path_);
        if (fd < 0) throw std::runtime_error("Cannot reserve APK backing file");
        if (close(fd) != 0 || std::rename(path, path_) != 0) {
            unlink(path_);
            throw std::runtime_error("Cannot take ownership of browser APK backing file");
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
