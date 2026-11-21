#include "art_sf_entry.h"

#include "art_interp.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {

using wa::interp::Value;
using wa::interp::Vm;

constexpr char kApkAssets[] = "Landroid/content/res/ApkAssets;";
constexpr char kMountPath[] = "/tmp/wa-art-XXXXXX";

struct MountedApk {
    ~MountedApk() {
        if (created) unlink(path);
    }

    void Write(const uint8_t* bytes, size_t size) {
        const int fd = mkstemp(path);
        if (fd < 0) throw std::runtime_error("Cannot create APK backing file");
        created = true;
        while (size != 0) {
            const auto count = write(fd, bytes, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                close(fd);
                throw std::runtime_error("Cannot write APK backing file");
            }
            bytes += count;
            size -= count;
        }
        if (close(fd) != 0) throw std::runtime_error("Cannot close APK backing file");
    }

    char path[sizeof(kMountPath)] = "/tmp/wa-art-XXXXXX";
    bool created = false;
};

struct ArtSession {
    explicit ArtSession(uint32_t interval) : vm(wa::interp::Options{.gc_interval = interval}) {}

    Vm vm;
    std::vector<std::unique_ptr<MountedApk>> mounts;
};

std::unique_ptr<ArtSession> art;
std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> scene(nullptr, wa_sf_destroy);
std::string last_error;

template <class Fn> int32_t ArtCall(Fn call) {
    last_error.clear();
    try {
        return call();
    } catch (const wa::interp::ManagedException&) {
        last_error = art->vm.ExceptionSummary();
        art->vm.ClearException();
        return WA_ERR_INVALID;
    } catch (const std::bad_alloc&) {
        last_error = "ART allocation failed";
        return WA_ERR_NOMEM;
    } catch (const std::exception& failure) {
        last_error = failure.what();
        return WA_ERR_INVALID;
    }
}

ArtSession& ActiveArt() {
    if (!art) throw std::logic_error("ART has not been created");
    return *art;
}

int32_t CopyText(std::string_view text, char* out, size_t cap) {
    if (!out || cap == 0) return WA_ERR_INVALID;
    if (text.size() >= cap) {
        out[0] = '\0';
        return WA_ERR_OVERFLOW;
    }
    if (!text.empty()) std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return WA_OK;
}

void RequireCall(const char* owner, const char* name, const char* sig, std::string_view result) {
    if (!owner || !name || !sig || !std::string_view(sig).starts_with("()")) {
        throw std::invalid_argument("Expected a no-argument ART method with the requested return type");
    }
    const std::string_view type = std::string_view(sig).substr(2);
    if (type != result && (type.size() != 1 || !result.contains(type.front()))) {
        throw std::invalid_argument("Unexpected ART return type");
    }
}

}  // namespace

int32_t wa_system_art_create(uint32_t gc_interval) {
    return ArtCall([&] {
        if (art) return WA_ERR_BUSY;
        art = std::make_unique<ArtSession>(gc_interval);
        return WA_OK;
    });
}

void wa_system_art_destroy(void) {
    art.reset();
    last_error.clear();
}

int32_t wa_system_art_load_dex(const char* location, const uint8_t* bytes, size_t len) {
    return ArtCall([&] {
        if (!location || !bytes || !len) return WA_ERR_INVALID;
        ActiveArt().vm.LoadDexFile(location, {bytes, bytes + len});
        return WA_OK;
    });
}

int32_t wa_system_art_register_resource_natives(void) {
    return ArtCall([] {
        ActiveArt().vm.RegisterResourceNatives();
        return WA_OK;
    });
}

int32_t wa_system_art_mount_apk(const uint8_t* bytes, size_t len, char* path, size_t cap) {
    return ArtCall([&] {
        if (!path || !cap) return WA_ERR_INVALID;
        path[0] = '\0';
        if (!bytes || !len || len > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
            return WA_ERR_INVALID;
        }
        if (cap < sizeof(kMountPath)) return WA_ERR_OVERFLOW;
        auto& session = ActiveArt();
        session.vm.RegisterResourceNatives();
        auto mounted = std::make_unique<MountedApk>();
        mounted->Write(bytes, len);
        wa::interp::ScopedHandleScope roots(session.vm);
        const auto arg = roots.Hold(Value::Ref(session.vm.NewString(mounted->path)));
        auto* assets = roots.Hold(session.vm.CallStatic(kApkAssets, "loadFromPath",
            "(Ljava/lang/String;)Landroid/content/res/ApkAssets;", std::array{arg}).ref());
        session.vm.CallVirtual(assets, "close", "()V");
        session.mounts.push_back(std::move(mounted));
        CopyText(session.mounts.back()->path, path, cap);
        return WA_OK;
    });
}

int32_t wa_system_art_call_static_int(const char* owner, const char* name, const char* sig) {
    int32_t result = 0;
    ArtCall([&] {
        RequireCall(owner, name, sig, "ZBCSI");
        result = ActiveArt().vm.CallStatic(owner, name, sig).i();
        return WA_OK;
    });
    return result;
}

int32_t wa_system_art_call_static_string(const char* owner, const char* name, const char* sig,
                                         char* out, size_t cap) {
    return ArtCall([&] {
        if (!out || !cap) return static_cast<int32_t>(WA_ERR_INVALID);
        out[0] = '\0';
        if (!sig || std::string_view(sig) != "()Ljava/lang/String;") {
            return static_cast<int32_t>(WA_ERR_INVALID);
        }
        RequireCall(owner, name, sig, "Ljava/lang/String;");
        auto& vm = ActiveArt().vm;
        return CopyText(vm.StringUtf8(vm.CallStatic(owner, name, sig).ref()), out, cap);
    });
}

int32_t wa_system_art_call_static_void(const char* owner, const char* name, const char* sig) {
    return ArtCall([&] {
        RequireCall(owner, name, sig, "V");
        ActiveArt().vm.CallStatic(owner, name, sig);
        return WA_OK;
    });
}

int32_t wa_system_art_last_error(char* out, size_t cap) { return CopyText(last_error, out, cap); }

int32_t wa_system_art_stats_json(char* out, size_t cap) {
    return ArtCall([&] {
        const auto& vm = ActiveArt().vm;
        const auto stats = vm.HeapStats();
        char json[256];
        std::snprintf(json, sizeof(json),
            "{\"collections\":%zu,\"live\":%zu,\"reclaimed\":%zu,\"executedOpcodes\":%zu,"
            "\"apkAssetsNativeCalls\":%llu}", stats.collections, stats.live, stats.reclaimed,
            vm.ExecutedOpcodeCount(), static_cast<unsigned long long>(vm.NativeCalls(kApkAssets)));
        return CopyText(json, out, cap);
    });
}

int32_t wa_system_art_gc_collect(void) {
    return ArtCall([] { ActiveArt().vm.Collect(); return WA_OK; });
}

int32_t wa_system_art_set_gc_paused(int32_t paused) {
    return ArtCall([&] { ActiveArt().vm.SetGcPaused(paused != 0); return WA_OK; });
}

int32_t wa_system_art_set_inline_caches_enabled(int32_t enabled) {
    return ArtCall([&] { ActiveArt().vm.SetInlineCachesEnabled(enabled != 0); return WA_OK; });
}

int32_t wa_system_sf_init(int32_t width, int32_t height, float density) {
    const double dpi = std::round(density * 160.0);
    if (width <= 0 || height <= 0 || !(dpi >= 1 && dpi <= UINT32_MAX)) return WA_SF_INVALID;
    if (scene) return WA_SF_STATE;
    decltype(scene) next(wa_sf_create(), wa_sf_destroy);
    if (!next) return WA_SF_FULL;
    const auto status = wa_sf_begin(next.get(), width, height, static_cast<uint32_t>(dpi));
    if (status != WA_SF_OK) return status;
    const auto published = wa_sf_commit(next.get());
    if (published == WA_SF_OK) scene = std::move(next);
    return published;
}

wa_sf_t* wa_system_sf_ctx(void) { return scene.get(); }
void wa_system_sf_destroy(void) { scene.reset(); }
