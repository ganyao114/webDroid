#include "binder_service_entry.h"

#include "apk_manifest.h"
#include "binder_bridge.h"
#include "wa_binder_driver.h"

#include <ServiceManager.h>
#include <android/wasm/BnActivityManagerLite.h>
#include <android/wasm/BnPackageManagerLite.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>

EM_JS(int32_t, wa_binder_js_init, (size_t bytes, uint32_t pid), {
    return Module['binderHost']?.binder_init(bytes, pid) ?? -9;
});
EM_JS(int32_t, wa_binder_js_send,
      (uint32_t dst, uint32_t txn, uint32_t code, uint32_t flags, const void* data, uint32_t size), {
    return Module['binderHost']?.binder_send(dst, txn, code, flags, data, size) ?? -9;
});
EM_JS(int32_t, wa_binder_js_recv,
      (wa_binder_msg_t* header, void* data, uint32_t size, int32_t timeout_ms), {
    return Module['binderHost']?.binder_recv(header, data, size, timeout_ms) ?? -9;
});

extern "C" int32_t host_init(size_t bytes, uint32_t pid) {
    return wa_binder_js_init(bytes, pid);
}

extern "C" int32_t host_send(uint32_t dst, uint32_t txn, uint32_t code, uint32_t flags,
                             const void* data, uint32_t size) {
    return wa_binder_js_send(dst, txn, code, flags, data, size);
}

extern "C" int32_t host_recv(wa_binder_msg_t* header, void* data, uint32_t size, int32_t timeout_ms) {
    return wa_binder_js_recv(header, data, size, timeout_ms);
}

extern "C" int64_t host_clock_ns(int32_t realtime) {
    timespec now{};
    clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
    return int64_t{now.tv_sec} * 1'000'000'000 + now.tv_nsec;
}
#endif

namespace {

using android::IBinder;
using android::IPCThreadState;
using android::ProcessState;
using android::sp;
using android::String16;
using android::binder::Status;
using namespace android::wasm;

constexpr uint32_t kManagerPid = 1;
constexpr uint32_t kServerPid = 2;
constexpr char kPackageService[] = "package";
constexpr char kActivityService[] = "activity";

Status ServiceStatus(int32_t result) {
    return result == WA_OK ? Status::ok() : Status::fromServiceSpecificError(result);
}

int32_t BinderStatus(android::status_t result) {
    switch (result) {
    case android::OK: return WA_OK;
    case android::NO_MEMORY: return WA_ERR_NOMEM;
    case android::BAD_VALUE: return WA_ERR_INVALID;
    case android::NAME_NOT_FOUND: return WA_ERR_NOENT;
    case android::DEAD_OBJECT:
    case android::NO_INIT: return WA_ERR_BADF;
    case android::FAILED_TRANSACTION: return WA_ERR_OVERFLOW;
    default: return WA_ERR_INTERNAL;
    }
}

int32_t BinderStatus(const Status& status) {
    if (status.isOk())
        return WA_OK;
    if (status.exceptionCode() == Status::EX_SERVICE_SPECIFIC)
        return status.serviceSpecificErrorCode();
    const auto transaction = status.transactionError();
    return transaction == android::OK ? WA_ERR_INTERNAL : BinderStatus(transaction);
}

struct InstalledPackage {
    PackageRecord record;
    std::string home;
    std::vector<std::string> activities;
};

class PackageManager final : public BnPackageManagerLite {
  public:
    Status installApkBytes(const std::vector<uint8_t>& bytes, const std::string& path,
                           PackageRecord* out, int32_t* result) override {
        if (path.empty() || path.contains('\0')) {
            *result = WA_ERR_INVALID;
            return Status::ok();
        }
        wa::system::ApkManifest manifest;
        *result = wa::system::ReadApkManifest(bytes, manifest);
        if (*result != WA_OK)
            return Status::ok();

        out->packageName = std::move(manifest.package);
        out->label = std::move(manifest.label);
        out->versionCode = manifest.version_code;
        out->mainActivity = manifest.launcher.empty() ? manifest.home : std::move(manifest.launcher);
        out->apkPath = path;
        out->isHomeLauncher = !manifest.home.empty();
        out->abiMask = manifest.abi_mask;
        packages_.insert_or_assign(out->packageName,
            InstalledPackage{*out, std::move(manifest.home), std::move(manifest.activities)});
        return Status::ok();
    }

    Status queryLauncherActivities(std::vector<PackageRecord>* out) override {
        for (const auto& [_, package] : packages_) {
            if (!package.record.mainActivity.empty())
                out->push_back(package.record);
        }
        return Status::ok();
    }

    Status resolveHomeActivity(PackageRecord* out) override {
        for (const auto& [_, package] : packages_) {
            if (!package.home.empty()) {
                *out = package.record;
                out->mainActivity = package.home;
                return Status::ok();
            }
        }
        return ServiceStatus(WA_ERR_NOENT);
    }

    Status getPackageInfo(const std::string& name, PackageRecord* out) override {
        const auto* package = find(name);
        if (!package)
            return ServiceStatus(WA_ERR_NOENT);
        *out = package->record;
        return Status::ok();
    }

    const InstalledPackage* find(const std::string& name) const {
        const auto it = packages_.find(name);
        return it == packages_.end() ? nullptr : &it->second;
    }

  private:
    std::map<std::string, InstalledPackage> packages_;
};

class ActivityManager final : public BnActivityManagerLite {
  public:
    explicit ActivityManager(sp<PackageManager> packages) : packages_(std::move(packages)) {}

    Status startActivity(const std::string& packageName, const std::string& activityName,
                         ActivityState* out) override {
        if (packageName.empty() || activityName.empty())
            return ServiceStatus(WA_ERR_INVALID);

        const auto* package = packages_->find(packageName);
        if (!package)
            return ServiceStatus(WA_ERR_NOENT);
        const auto name = wa::system::ActivityClassName(packageName, activityName);
        if (std::ranges::find(package->activities, name) == package->activities.end())
            return ServiceStatus(WA_ERR_NOENT);
        return launch(packageName, name, out);
    }

    Status startHomeActivity(ActivityState* out) override {
        PackageRecord home;
        const auto status = packages_->resolveHomeActivity(&home);
        return status.isOk() ? launch(home.packageName, home.mainActivity, out) : status;
    }

    Status getTopActivity(ActivityState* out) override {
        if (tasks_.empty())
            return ServiceStatus(WA_ERR_NOENT);
        *out = tasks_.back();
        return Status::ok();
    }

    Status finishTopActivity(int32_t* result) override {
        if (tasks_.empty()) {
            *result = WA_ERR_NOENT;
        } else {
            tasks_.pop_back();
            *result = WA_OK;
        }
        return Status::ok();
    }

  private:
    Status launch(const std::string& packageName, const std::string& activityName, ActivityState* out) {
        if (lastTaskId_ == std::numeric_limits<int32_t>::max())
            return ServiceStatus(WA_ERR_OVERFLOW);
        auto& count = launches_[{packageName, activityName}];
        out->packageName = packageName;
        out->activityName = activityName;
        out->taskId = lastTaskId_ + 1;
        out->state = "RESUMED";
        out->launchedCount = count + 1;
        tasks_.push_back(*out);
        ++lastTaskId_;
        ++count;
        return Status::ok();
    }

    sp<PackageManager> packages_;
    std::vector<ActivityState> tasks_;
    std::map<std::pair<std::string, std::string>, int32_t> launches_;
    int32_t lastTaskId_ = 0;
};

struct SystemServer {
    sp<PackageManager> package;
    sp<ActivityManager> activity;
    bool packageRegistered = false;
    bool activityRegistered = false;
};

[[clang::no_destroy]] sp<android::ServiceManager> manager;
[[clang::no_destroy]] SystemServer server;
std::atomic<uint32_t> binderPid{0};
std::mutex startupMutex;
bool looperReady = false;
thread_local bool polling = false;

int32_t StartPolling() {
    int fd = -1;
    const auto result = IPCThreadState::self()->setupPolling(&fd);
    looperReady = polling = result == android::OK;
    return BinderStatus(result);
}

int32_t Pump(uint32_t pid, int timeout_ms) {
    if (binderPid != pid || !polling || timeout_ms < 0)
        return WA_ERR_INVALID;
    return BinderStatus(wa::aosp_binder::pollCommands(timeout_ms));
}

template <typename Interface>
sp<Interface> FindService(const char* name) {
    const auto binder = android::defaultServiceManager()->checkService(String16(name));
    return binder && binder->remoteBinder() ? android::interface_cast<Interface>(binder) : nullptr;
}

std::string JsonString(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string json(1, '"');
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            json += '\\';
            json += static_cast<char>(ch);
        } else if (ch < 0x20) {
            json += "\\u00";
            json += hex[ch >> 4];
            json += hex[ch & 0xf];
        } else {
            json += static_cast<char>(ch);
        }
    }
    json += '"';
    return json;
}

std::string Json(const PackageRecord& record) {
    return "{\"packageName\":" + JsonString(record.packageName) +
           ",\"label\":" + JsonString(record.label) +
           ",\"versionCode\":" + std::to_string(record.versionCode) +
           ",\"mainActivity\":" + JsonString(record.mainActivity) +
           ",\"apkPath\":" + JsonString(record.apkPath) +
           ",\"isHomeLauncher\":" + (record.isHomeLauncher ? "true" : "false") +
           ",\"abiMask\":" + std::to_string(record.abiMask) + '}';
}

std::string Json(const std::vector<PackageRecord>& records) {
    std::string json = "[";
    for (const auto& record : records) {
        if (json.size() > 1)
            json += ',';
        json += Json(record);
    }
    json += ']';
    return json;
}

std::string Json(const ActivityState& state) {
    return "{\"packageName\":" + JsonString(state.packageName) +
           ",\"activityName\":" + JsonString(state.activityName) +
           ",\"taskId\":" + std::to_string(state.taskId) +
           ",\"state\":" + JsonString(state.state) +
           ",\"launchedCount\":" + std::to_string(state.launchedCount) + '}';
}

template <typename Interface, typename Result, typename Call>
int32_t CallForJson(const char* name, char* out, size_t capacity, Call call) {
    if (!out || !capacity)
        return WA_ERR_INVALID;
    out[0] = '\0';
    if (binderPid <= kServerPid)
        return WA_ERR_BADF;
    const auto service = FindService<Interface>(name);
    if (!service)
        return WA_ERR_NOENT;
    Result result;
    const auto status = call(*service, &result);
    if (!status.isOk())
        return BinderStatus(status);
    const auto json = Json(result);
    if (json.size() >= capacity)
        return WA_ERR_OVERFLOW;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return WA_OK;
}

} // namespace

int32_t wa_system_binder_attach(void* ring_base, size_t ring_bytes, uint32_t pid) {
    const auto result = wa::aosp_binder::attach(ring_base, ring_bytes, pid);
    if (result == WA_OK)
        binderPid = pid;
    return result;
}

int32_t wa_system_servicemanager_start() {
    if (binderPid != kManagerPid)
        return WA_ERR_INVALID;
    if (polling)
        return WA_OK;
    std::lock_guard lock(startupMutex);
    if (looperReady)
        return WA_ERR_BUSY;

    const auto process = ProcessState::self();
    if (!manager) {
        if (const auto status = process->setThreadPoolMaxThreadCount(0); status != android::OK)
            return BinderStatus(status);
        auto service = sp<android::ServiceManager>::make(std::make_unique<android::Access>());
        IPCThreadState::self()->setTheContextObject(service);
        if (!process->becomeContextManager())
            return WA_ERR_INTERNAL;
        manager = std::move(service);
    }
    return StartPolling();
}

int32_t wa_system_servicemanager_pump(int timeout_ms) {
    return Pump(kManagerPid, timeout_ms);
}

int32_t wa_system_server_start() {
    if (binderPid != kServerPid)
        return WA_ERR_INVALID;
    if (polling)
        return WA_OK;
    std::lock_guard lock(startupMutex);
    if (looperReady)
        return WA_ERR_BUSY;

    const auto sm = android::defaultServiceManager();
    if (!server.package) {
        server.package = sp<PackageManager>::make();
        server.activity = sp<ActivityManager>::make(server.package);
    }
    if (!server.packageRegistered) {
        const auto status =
            sm->addService(String16(kPackageService), android::IInterface::asBinder(server.package));
        if (status != android::OK)
            return BinderStatus(status);
        server.packageRegistered = true;
    }
    if (!server.activityRegistered) {
        const auto status =
            sm->addService(String16(kActivityService), android::IInterface::asBinder(server.activity));
        if (status != android::OK)
            return BinderStatus(status);
        server.activityRegistered = true;
    }
    return StartPolling();
}

int32_t wa_system_server_pump(int timeout_ms) {
    return Pump(kServerPid, timeout_ms);
}

int32_t wa_system_client_install_apk(const uint8_t* apk_bytes, size_t apk_len, const char* apk_path,
                                    char* json_out, size_t json_cap) {
    if (!apk_bytes || !apk_len || !apk_path || !apk_path[0])
        return WA_ERR_INVALID;
    if (apk_len >= wa::aosp_binder::kMaxBinderWireBytes)
        return WA_ERR_OVERFLOW;
    return CallForJson<IPackageManagerLite, PackageRecord>(
        kPackageService, json_out, json_cap, [&](auto& service, auto* record) {
            int32_t result = WA_ERR_INTERNAL;
            const auto status = service.installApkBytes({apk_bytes, apk_bytes + apk_len},
                                                        apk_path, record, &result);
            return status.isOk() ? ServiceStatus(result) : status;
        });
}

int32_t wa_system_client_query_launcher(char* json_out, size_t json_cap) {
    return CallForJson<IPackageManagerLite, std::vector<PackageRecord>>(
        kPackageService, json_out, json_cap,
        [](auto& service, auto* out) { return service.queryLauncherActivities(out); });
}

int32_t wa_system_client_resolve_home_activity(char* json_out, size_t json_cap) {
    return CallForJson<IPackageManagerLite, PackageRecord>(
        kPackageService, json_out, json_cap,
        [](auto& service, auto* out) { return service.resolveHomeActivity(out); });
}

int32_t wa_system_client_get_package_info(const char* package_name, char* json_out, size_t json_cap) {
    if (!package_name || !package_name[0])
        return WA_ERR_INVALID;
    return CallForJson<IPackageManagerLite, PackageRecord>(
        kPackageService, json_out, json_cap,
        [&](auto& service, auto* out) { return service.getPackageInfo(package_name, out); });
}

int32_t wa_system_client_start_activity(const char* package_name, const char* activity_name,
                                       char* json_out, size_t json_cap) {
    if (!package_name || !package_name[0] || !activity_name || !activity_name[0])
        return WA_ERR_INVALID;
    return CallForJson<IActivityManagerLite, ActivityState>(
        kActivityService, json_out, json_cap,
        [&](auto& service, auto* out) { return service.startActivity(package_name, activity_name, out); });
}

int32_t wa_system_client_start_home_activity(char* json_out, size_t json_cap) {
    return CallForJson<IActivityManagerLite, ActivityState>(
        kActivityService, json_out, json_cap,
        [](auto& service, auto* out) { return service.startHomeActivity(out); });
}

int32_t wa_system_client_get_top_activity(char* json_out, size_t json_cap) {
    return CallForJson<IActivityManagerLite, ActivityState>(
        kActivityService, json_out, json_cap,
        [](auto& service, auto* out) { return service.getTopActivity(out); });
}

int32_t wa_system_client_finish_top_activity() {
    if (binderPid <= kServerPid)
        return WA_ERR_BADF;
    const auto service = FindService<IActivityManagerLite>(kActivityService);
    if (!service)
        return WA_ERR_NOENT;
    int32_t result = WA_ERR_INTERNAL;
    const auto status = service->finishTopActivity(&result);
    return status.isOk() ? result : BinderStatus(status);
}
