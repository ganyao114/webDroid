#include "binder_service_entry.h"
#include "wasmandroid/libkern.h"

#include <binder/IServiceManager.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kRingBytes = 8 * 1024 * 1024;
constexpr char kCubePackage[] = "org.wasmandroid.cube";
constexpr char kCubePath[] = "/data/app/cube/base.apk";
constexpr char kHomePath[] = "/data/app/home \"local\"/base.apk";
constexpr std::string_view kCubeRecord =
    R"({"packageName":"org.wasmandroid.cube","label":"WasmAndroid 3D Cube","versionCode":17,"mainActivity":"org.wasmandroid.cube.CubeActivity","apkPath":"/data/app/cube/base.apk","isHomeLauncher":false,"abiMask":3})";
constexpr std::string_view kHomeRecord =
    R"({"packageName":"org.wasmandroid.home","label":"WasmAndroid Home 🏠","versionCode":7,"mainActivity":"org.wasmandroid.home.HomeShortcut","apkPath":"/data/app/home \"local\"/base.apk","isHomeLauncher":true,"abiMask":0})";
constexpr std::string_view kHomeResolution =
    R"({"packageName":"org.wasmandroid.home","label":"WasmAndroid Home 🏠","versionCode":7,"mainActivity":"org.wasmandroid.home.HomeActivity","apkPath":"/data/app/home \"local\"/base.apk","isHomeLauncher":true,"abiMask":0})";
constexpr std::string_view kHomeActivity =
    R"({"packageName":"org.wasmandroid.home","activityName":"org.wasmandroid.home.HomeActivity","taskId":1,"state":"RESUMED","launchedCount":1})";
constexpr std::string_view kCubeActivity =
    R"({"packageName":"org.wasmandroid.cube","activityName":"org.wasmandroid.cube.CubeActivity","taskId":2,"state":"RESUMED","launchedCount":1})";
constexpr std::string_view kCubeRelaunch =
    R"({"packageName":"org.wasmandroid.cube","activityName":"org.wasmandroid.cube.CubeActivity","taskId":3,"state":"RESUMED","launchedCount":2})";

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            std::abort();                                                                    \
        }                                                                                    \
    } while (false)

struct Shared {
    std::atomic_bool stop{false};
    alignas(uint64_t) uint8_t ring[kRingBytes];
};

static_assert(std::atomic_bool::is_always_lock_free);

class Child {
  public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    ~Child() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            wait();
        }
    }

    template <typename Run>
    bool start(Run run) {
        int ready[2];
        if (pipe(ready) != 0)
            return false;
        pid_ = fork();
        if (pid_ == 0) {
            close(ready[0]);
            run([fd = ready[1]] {
                CHECK(write(fd, "R", 1) == 1);
                close(fd);
            });
            _exit(EXIT_SUCCESS);
        }
        close(ready[1]);
        char marker = 0;
        ssize_t count = -1;
        if (pid_ > 0) {
            do {
                count = read(ready[0], &marker, 1);
            } while (count < 0 && errno == EINTR);
        }
        close(ready[0]);
        return count == 1 && marker == 'R';
    }

    bool wait() {
        int status = 0;
        pid_t child;
        do {
            child = waitpid(pid_, &status, 0);
        } while (child < 0 && errno == EINTR);
        pid_ = -1;
        return child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
    }

  private:
    pid_t pid_ = -1;
};

std::vector<uint8_t> ReadApk(const char* path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void CheckJson(const char* actual, std::string_view expected) {
    if (actual != expected) {
        std::fprintf(stderr, "Expected %.*s\nReceived %s\n", static_cast<int>(expected.size()),
                     expected.data(), actual);
        std::abort();
    }
}

void Exercise(Shared& shared, const char* cubePath, const char* homePath) {
    CHECK(wa_system_binder_attach(shared.ring, kRingBytes, 3) == WA_OK);
    const auto manager = android::defaultServiceManager();
    const auto services = manager->getServiceDebugInfo();
    for (const char* name : {"package", "activity"}) {
        CHECK(std::ranges::any_of(services, [&](const auto& service) {
            return service.name == name && service.pid == 2;
        }));
    }

    const auto cube = ReadApk(cubePath);
    const auto home = ReadApk(homePath);
    CHECK(cube.size() > WA_BINDER_MAX_PAYLOAD && !home.empty());
    char json[4096];
    CHECK(wa_system_client_query_launcher(json, sizeof(json)) == WA_OK);
    CheckJson(json, "[]");
    CHECK(wa_system_client_resolve_home_activity(json, sizeof(json)) == WA_ERR_NOENT);
    CHECK(wa_system_client_start_home_activity(json, sizeof(json)) == WA_ERR_NOENT);
    CHECK(wa_system_client_get_top_activity(json, sizeof(json)) == WA_ERR_NOENT);
    CHECK(wa_system_client_finish_top_activity() == WA_ERR_NOENT);

    const uint8_t badApk[]{1, 2, 3};
    CHECK(wa_system_client_install_apk(badApk, sizeof(badApk), kCubePath, json, sizeof(json)) ==
          WA_ERR_INVALID);
    CHECK(wa_system_client_install_apk(cube.data(), cube.size(), kCubePath, json, sizeof(json)) ==
          WA_OK);
    CheckJson(json, kCubeRecord);
    CHECK(wa_system_client_install_apk(home.data(), home.size(), kHomePath, json, sizeof(json)) ==
          WA_OK);
    CheckJson(json, kHomeRecord);
    CHECK(wa_system_client_query_launcher(json, sizeof(json)) == WA_OK);
    std::string expected = std::string("[").append(kCubeRecord).append(",").append(kHomeRecord) + ']';
    CheckJson(json, expected);
    CHECK(wa_system_client_get_package_info(kCubePackage, json, sizeof(json)) == WA_OK);
    CheckJson(json, kCubeRecord);
    CHECK(wa_system_client_get_package_info("org.wasmandroid.missing", json, sizeof(json)) ==
          WA_ERR_NOENT);
    CHECK(wa_system_client_resolve_home_activity(json, sizeof(json)) == WA_OK);
    CheckJson(json, kHomeResolution);

    CHECK(wa_system_client_start_home_activity(json, sizeof(json)) == WA_OK);
    CheckJson(json, kHomeActivity);
    CHECK(wa_system_client_start_activity(kCubePackage, ".Missing", json, sizeof(json)) == WA_ERR_NOENT);
    CHECK(wa_system_client_start_activity(kCubePackage, ".CubeActivity", json, sizeof(json)) == WA_OK);
    CheckJson(json, kCubeActivity);
    CHECK(wa_system_client_get_top_activity(json, 1) == WA_ERR_OVERFLOW && !json[0]);
    CHECK(wa_system_client_get_top_activity(json, sizeof(json)) == WA_OK);
    CheckJson(json, kCubeActivity);
    CHECK(wa_system_client_finish_top_activity() == WA_OK);
    CHECK(wa_system_client_get_top_activity(json, sizeof(json)) == WA_OK);
    CheckJson(json, kHomeActivity);
    CHECK(wa_system_client_start_activity(kCubePackage, "org.wasmandroid.cube.CubeActivity", json,
                                          sizeof(json)) == WA_OK);
    CheckJson(json, kCubeRelaunch);
    CHECK(wa_system_client_finish_top_activity() == WA_OK);
    CHECK(wa_system_client_finish_top_activity() == WA_OK);
    CHECK(wa_system_client_get_top_activity(json, sizeof(json)) == WA_ERR_NOENT);
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 3);
    void* mapping = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    auto& shared = *std::construct_at(static_cast<Shared*>(mapping));

    // libbinder keeps one ProcessState and driver per process.
    Child manager, server, client;
    if (!manager.start([&](auto ready) {
            CHECK(wa_system_binder_attach(shared.ring, kRingBytes, 1) == WA_OK);
            CHECK(wa_system_servicemanager_start() == WA_OK);
            CHECK(wa_system_servicemanager_pump(0) == WA_OK);
            CHECK(wa_system_servicemanager_pump(5) == WA_OK);
            ready();
            while (!shared.stop.load())
                CHECK(wa_system_servicemanager_pump(10) == WA_OK);
        }) || !server.start([&](auto ready) {
            CHECK(wa_system_binder_attach(shared.ring, kRingBytes, 2) == WA_OK);
            CHECK(wa_system_server_start() == WA_OK);
            CHECK(wa_system_server_pump(0) == WA_OK);
            ready();
            while (!shared.stop.load())
                CHECK(wa_system_server_pump(10) == WA_OK);
        }) || !client.start([&](auto ready) {
            ready();
            Exercise(shared, argv[1], argv[2]);
        }) || !client.wait())
        return EXIT_FAILURE;

    shared.stop.store(true);
    if (!server.wait() || !manager.wait())
        return EXIT_FAILURE;
    CHECK(munmap(mapping, sizeof(Shared)) == 0);
    std::puts("PASS three native vprocs, 23 exact PackageManager and ActivityManager RPCs");
}
