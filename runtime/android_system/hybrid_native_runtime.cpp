#include "hybrid_native_runtime.h"

#include "dbt_internal.h"
#include "elf_image.h"
#include "hybrid_jni_bridge.h"
#include "gles_host_bridge.h"
#include "mounted_apk.h"
#include "ziparchive/zip_archive.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace wa::system {
namespace {

using dbt::CheckGuest;
using dbt::GuestFault;
using dbt::GuestMemory;

constexpr std::array kArchitectures{WA_ARCH_ARM64, WA_ARCH_ARM32};
constexpr uint64_t kFirstImage = 0x10000000;
constexpr uint64_t kProcessHandle = 1;
constexpr size_t kDlErrorCapacity = 4096;

std::string_view Abi(wa_guest_arch_t arch) {
    return arch == WA_ARCH_ARM64 ? "arm64-v8a" : "armeabi-v7a";
}

std::string_view ExtractedAbi(wa_guest_arch_t arch) {
    return arch == WA_ARCH_ARM64 ? "arm64" : "arm";
}

std::string EntryName(wa_guest_arch_t arch, std::string_view name) {
    return "lib/" + std::string(Abi(arch)) + "/" + std::string(name);
}

bool LibraryName(std::string_view name) {
    return name.starts_with("lib") && name.ends_with(".so") &&
        name.find_first_of("/\\") == name.npos && name.find('\0') == name.npos;
}

std::string Error(std::string_view stage, int32_t status) {
    return std::string(stage) + " (" + std::to_string(status) + ")";
}

class ApkArchive {
  public:
    explicit ApkArchive(std::string path) : path(std::move(path)) {
        ZipArchiveHandle handle = nullptr;
        const auto status = OpenArchive(this->path.c_str(), &handle);
        zip.reset(handle);
        if (status != 0) throw std::runtime_error(Error("Open APK for native libraries", status));
    }

    std::optional<ZipEntry64> Entry(wa_guest_arch_t arch, std::string_view name) const {
        ZipEntry64 entry{};
        if (FindEntry(zip.get(), EntryName(arch, name), &entry) != 0) return std::nullopt;
        return entry;
    }

    std::vector<uint8_t> Extract(const ZipEntry64& entry) const {
        if (!entry.uncompressed_length || entry.uncompressed_length >= dbt::kHeapBase - kFirstImage ||
            !std::in_range<size_t>(entry.uncompressed_length))
            throw std::runtime_error("Invalid ELF size");
        std::vector<uint8_t> bytes(static_cast<size_t>(entry.uncompressed_length));
        const auto status = ExtractToMemory(zip.get(), &entry, bytes.data(), bytes.size());
        if (status) throw std::runtime_error(Error("Extract ELF from APK", status));
        return bytes;
    }

    std::vector<std::string> Libraries(wa_guest_arch_t arch) const {
        auto prefix = EntryName(arch, "");
        void* cookie = nullptr;
        if (StartIteration(zip.get(), &cookie, prefix, ".so") != 0)
            throw std::runtime_error("Cannot enumerate APK native libraries");
        const std::unique_ptr<void, decltype(&EndIteration)> iteration(cookie, EndIteration);
        std::vector<std::string> names;
        ZipEntry64 entry{};
        std::string_view path;
        while (Next(cookie, &entry, &path) == 0) {
            const auto name = path.substr(prefix.size());
            if (LibraryName(name)) names.emplace_back(name);
        }
        std::ranges::sort(names);
        return names;
    }

    const std::string path;
    std::unique_ptr<ZipArchive, decltype(&CloseArchive)> zip{nullptr, CloseArchive};
};

struct LibraryRequest {
    std::string_view name;
    std::optional<wa_guest_arch_t> abi;
};

std::optional<LibraryRequest> ParseLibraryPath(const ApkArchive& apk, std::string_view path) {
    if (!path.starts_with('/') || path.find('\0') != path.npos ||
        path.find("/../") != path.npos || path.find("/./") != path.npos) return std::nullopt;
    const auto slash = path.rfind('/');
    const auto name = path.substr(slash + 1);
    if (!LibraryName(name)) return std::nullopt;
    const auto parent = path.substr(0, slash);
    const auto lib_dir = parent.rfind("/lib/");
    if (lib_dir == parent.npos) return std::nullopt;
    const auto root = parent.substr(0, lib_dir);
    const auto directory = parent.substr(lib_dir + 5);
    if (!root.starts_with("/data/app/") && root != apk.path && root != apk.path + "!")
        return std::nullopt;
    for (auto arch : kArchitectures)
        if (directory == Abi(arch) || directory == ExtractedAbi(arch)) return LibraryRequest{name, arch};
    return std::nullopt;
}

class HybridNativeRuntime {
  public:
    HybridNativeRuntime(const ApkArchive& apk, wa_guest_arch_t arch, interp::JniBridge* jni)
        : apk_(apk), arch_(arch), mmu_(wa_softmmu_create(ReadElf, this), wa_softmmu_destroy),
          dbt_(wa_dbt_create(arch, mmu_.get()), wa_dbt_destroy), memory_(mmu_.get(), arch) {
        if (!dbt_) throw std::bad_alloc();
        CheckGuest(wa_hal_register_graphics_symbols(dbt_.get()));
        CheckGuest(wa_dbt_register_symbol(dbt_.get(), "libdl.so", "dlopen", DlDispatch<&HybridNativeRuntime::DlOpen>, this));
        CheckGuest(wa_dbt_register_symbol(dbt_.get(), "libdl.so", "dlsym", DlDispatch<&HybridNativeRuntime::DlSym>, this));
        CheckGuest(wa_dbt_register_symbol(dbt_.get(), "libdl.so", "dlclose", DlDispatch<&HybridNativeRuntime::DlClose>, this));
        CheckGuest(wa_dbt_register_symbol(dbt_.get(), "libdl.so", "dlerror", DlDispatch<&HybridNativeRuntime::DlError>, this));
        if (jni) jni_ = std::make_unique<HybridJniBridge>(*jni, *dbt_, mmu_.get(), arch);
    }

    wa_guest_arch_t Arch() const { return arch_; }
    HybridJniBridge& Jni() { return *jni_; }

    interp::GuestNativeStats Stats() const {
        wa_dbt_stats_t stats{};
        wa_dbt_get_stats(dbt_.get(), &stats);
        return {Abi(arch_), load_order_.size(), stats.host_calls, stats.reverse_callbacks};
    }

    std::string LoadForJava(std::string_view name) {
        auto& library = LoadImage(name);
        if (!library.error.empty()) return library.error;
        if (library.java_state != Library::JavaState::Uninitialized) return library.java_error;
        jni_->AddImage(library.base);
        library.java_state = Library::JavaState::Initializing;
        try {
            if (const auto on_load = dbt::FindElfSymbol(*dbt_, library.base, "JNI_OnLoad"))
                library.java_error = jni_->OnLoad(on_load);
        } catch (const GuestFault& fault) {
            library.java_error = Error("Initialize guest JNI", fault.status);
        } catch (...) {
            jni_->RemoveImage(library.base);
            library.java_state = Library::JavaState::Uninitialized;
            throw;
        }
        if (!library.java_error.empty()) {
            jni_->RemoveImage(library.base);
            library.java_state = Library::JavaState::Failed;
            return library.java_error;
        }
        library.java_state = Library::JavaState::Ready;
        return {};
    }

    void Shutdown() noexcept {
        // OnUnload may run JNI, so keep the bridge, mappings, and APK alive.
        if (jni_) {
            const auto count = load_order_.size();
            for (size_t i = count; i-- > 0;) {
                const auto& library = *load_order_[i];
                if (library.java_state != Library::JavaState::Ready) continue;
                try {
                    if (const auto on_unload = dbt::FindElfSymbol(*dbt_, library.base, "JNI_OnUnload"))
                        jni_->OnUnload(on_unload);
                } catch (...) {
                    // A failure in one finalizer cannot prevent teardown.
                }
            }
        }
        dbt::RunExitHandlers(*dbt_);
    }

    std::vector<NativeLibraryProbe> Probe(std::span<const NativeProbeRequest> requests) {
        std::vector<NativeLibraryProbe> report;
        std::map<std::string, dbt::ElfInfo, std::less<>> images;
        for (const auto& name : apk_.Libraries(arch_)) {
            auto& entry = report.emplace_back();
            entry.name = name;
            auto bytes = apk_.Extract(*apk_.Entry(arch_, name));
            auto& image = images[name];
            entry.elf_status = dbt::InspectElf(arch_, bytes, image, true);
            entry.needed = image.needed;
            if (entry.elf_status != WA_OK) entry.load_error = Error("Inspect ELF", entry.elf_status);
        }

        for (auto& entry : report) {
            if (entry.elf_status != WA_OK) continue;
            const auto& image = images.at(entry.name);
            std::vector<std::string> scope = image.needed;
            for (size_t i = 0; i < scope.size(); ++i) {
                const auto match = images.find(scope[i]);
                if (match == images.end()) continue;
                for (const auto& dependency : match->second.needed)
                    if (std::ranges::find(scope, dependency) == scope.end()) scope.push_back(dependency);
            }
            for (const auto& needed : image.needed) {
                if (!images.contains(needed) && !Virtual(needed)) entry.missing_libraries.push_back(needed);
            }
            for (const auto& symbol : image.imports) {
                const auto available = std::ranges::any_of(scope, [&](const auto& dep) {
                    const auto match = images.find(dep);
                    return (match != images.end() && std::ranges::binary_search(match->second.exports, symbol.name)) ||
                        dbt::FindVirtualSymbol(*dbt_, dep, symbol.name);
                });
                if (available) entry.resolved.push_back(symbol.name);
                else if (symbol.weak) entry.weak_undefined.push_back(symbol.name);
                else entry.missing.push_back(symbol.name);
            }
            auto& loaded = LoadImage(entry.name);
            entry.mapped_base = loaded.base;
            entry.load_error = loaded.error;
        }
        for (const auto& request : requests) {
            auto entry = std::ranges::find(report, request.library, &NativeLibraryProbe::name);
            if (entry == report.end()) continue;
            auto& probe = entry->entrypoints.emplace_back();
            probe.name = request.symbol;
            if (!entry->mapped_base) {
                probe.diagnostic = entry->load_error;
                continue;
            }
            probe.address = dbt::FindElfSymbol(*dbt_, entry->mapped_base, request.symbol);
            if (!probe.address) {
                probe.diagnostic = "Guest export is missing";
            } else if (request.action == NativeProbeAction::CallString) {
                uint64_t value = 0;
                const auto status = wa_dbt_call(dbt_.get(), probe.address, nullptr, 0, &value);
                if (status != WA_OK)
                    probe.diagnostic = Error("Guest call at PC " + std::to_string(dbt_->last_pc), status);
                else if (!value)
                    probe.diagnostic = "Guest function returned NULL";
                else {
                    try {
                        probe.value = memory_.String(value, 4096);
                    } catch (const GuestFault& fault) {
                        probe.diagnostic = Error("Read guest result string", fault.status);
                    }
                }
            } else if (request.action == NativeProbeAction::CallJniOnLoad) {
                if (request.symbol != "JNI_OnLoad") probe.diagnostic = "Expected JNI_OnLoad";
                else if (!jni_) probe.diagnostic = "JNI_OnLoad requires a guest JavaVM";
                else probe.diagnostic = LoadForJava(entry->name);
            }
        }
        return report;
    }

  private:
    struct Library {
        enum class JavaState { Uninitialized, Initializing, Ready, Failed };
        dbt::ElfInfo info;
        std::vector<uint8_t> backing;
        ZipEntry64 entry{};
        uint64_t base = 0;
        uint32_t file_id = 0;
        std::string error, java_error;
        JavaState java_state = JavaState::Uninitialized;
    };

    static int64_t ReadElf(void* self, uint32_t id, void* output, size_t count, uint64_t offset) {
        const auto& runtime = *static_cast<const HybridNativeRuntime*>(self);
        const auto found = runtime.files_.find(id);
        if (found == runtime.files_.end()) return WA_ERR_NOENT;
        const auto& library = *found->second;
        const auto& entry = library.entry;
        if (offset > entry.uncompressed_length || count > entry.uncompressed_length - offset)
            return WA_ERR_FAULT;
        if (entry.method == kCompressStored) {
            const int fd = GetFileDescriptor(runtime.apk_.zip.get());
            size_t done = 0;
            while (done < count) {
                const auto got = pread(fd, static_cast<uint8_t*>(output) + done, count - done,
                                       static_cast<off_t>(entry.offset + offset + done));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return WA_ERR_FAULT;
                done += got;
            }
        } else {
            std::memcpy(output, library.backing.data() + offset, count);
        }
        return count;
    }

    Library& LoadImage(std::string_view name) {
        const auto [it, inserted] = libraries_.try_emplace(std::string(name));
        auto& library = it->second;
        if (!inserted) return library;

        // A pending entry distinguishes dependency cycles from a completed load.
        library.error = std::string(name) + ": cyclic ELF dependency";
        try {
            LoadNewImage(name, library);
        } catch (const std::exception& exception) {
            library.error = std::string(name) + ": " + exception.what();
        }
        return library;
    }

    void LoadNewImage(std::string_view name, Library& library) {
        const auto entry = apk_.Entry(arch_, name);
        if (!entry) throw std::runtime_error("APK entry absent for " + std::string(Abi(arch_)));
        if (depth_ >= 64) throw std::runtime_error("Too many nested ELF dependencies");
        struct Depth {
            unsigned& depth;
            explicit Depth(unsigned& depth) : depth(depth) { ++depth; }
            ~Depth() { --depth; }
        } depth(depth_);
        library.entry = *entry;
        auto bytes = apk_.Extract(*entry);
        auto status = dbt::InspectElf(arch_, bytes, library.info);
        if (status != WA_OK) throw std::runtime_error(Error("Inspect ELF", status));

        for (const auto& needed : library.info.needed) {
            if (needed == name) continue;
            if (LibraryName(needed) && apk_.Entry(arch_, needed)) {
                const auto& dependency = LoadImage(needed);
                if (!dependency.error.empty())
                    throw std::runtime_error("DT_NEEDED " + dependency.error);
            } else if (!Virtual(needed)) {
                throw std::runtime_error("DT_NEEDED library unavailable: " + needed);
            }
        }

        const uint64_t bias = dbt::AlignUp(next_bias_, library.info.alignment);
        if (bias >= dbt::kHeapBase || library.info.end > dbt::kHeapBase - bias)
            throw std::runtime_error("ELF address space exhausted");
        next_bias_ = bias + library.info.end;
        if (entry->method != kCompressStored) library.backing.swap(bytes);
        library.file_id = next_file_id_++;
        files_.emplace(library.file_id, &library);
        const auto image = entry->method == kCompressStored ? std::span(bytes) : std::span(library.backing);
        std::string detail;
        try {
            status = dbt::LoadElf(*dbt_, library.file_id, image, bias, name, library.base, &detail);
        } catch (...) {
            files_.erase(library.file_id);
            library.backing = {};
            throw;
        }
        if (status != WA_OK) {
            files_.erase(library.file_id);
            std::vector<uint8_t>().swap(library.backing);
            throw std::runtime_error(Error(detail.empty() ? "Map or relocate ELF" : detail, status));
        }
        load_order_.push_back(&library);
        library.error.clear();
    }

    bool Virtual(std::string_view name) const {
        return dbt::IsVirtualLibrary(*dbt_, name);
    }

    template<uint64_t (HybridNativeRuntime::*Op)(const wa_host_call_ctx_t&)>
    static int32_t DlDispatch(wa_host_call_ctx_t* ctx, void* self) {
        try {
            ctx->ret0 = (static_cast<HybridNativeRuntime*>(self)->*Op)(*ctx);
            return WA_OK;
        } catch (const GuestFault& fault) {
            return fault.status;
        } catch (const std::bad_alloc&) {
            return WA_ERR_NOMEM;
        }
    }

    uint64_t DlFailure(std::string text, uint64_t result = 0) {
        dl_error_ = std::move(text);
        return result;
    }

    struct DlHandle {
        uint64_t base = 0;
        std::string name;
        size_t references = 0;
    };

    uint64_t OpenHandle(uint64_t base, std::string_view name) {
        const auto id = base ? base : name.empty() ? kProcessHandle : virtual_handles_.size() + 2;
        const auto handle = !base && !name.empty()
            ? virtual_handles_.try_emplace(std::string(name), id).first->second
            : id;
        auto& entry = dl_handles_.try_emplace(handle, DlHandle{base, std::string(name)}).first->second;
        ++entry.references;
        return handle;
    }

    uint64_t DlOpen(const wa_host_call_ctx_t& ctx) {
        if (!ctx.args[0]) return OpenHandle(0, {});
        const auto path = memory_.String(ctx.args[0]);
        std::string_view name = path;
        const auto explicit_path = path.starts_with('/');
        if (explicit_path) {
            if (auto request = ParseLibraryPath(apk_, path); request && request->abi == arch_) {
                name = request->name;
            } else if (path.starts_with("/system/lib/") || path.starts_with("/system/lib64/")) {
                name = std::string_view(path).substr(path.rfind('/') + 1);
                if (Virtual(name)) return OpenHandle(0, name);
                return DlFailure("dlopen: unknown system library " + path);
            } else {
                return DlFailure("dlopen: unsupported library path " + path);
            }
        }
        if (LibraryName(name) && apk_.Entry(arch_, name)) {
            const auto& library = LoadImage(name);
            return library.error.empty() ? OpenHandle(library.base, name) : DlFailure(library.error);
        }
        return Virtual(name) ? OpenHandle(0, name) : DlFailure("dlopen: library unavailable: " + path);
    }

    uint64_t DlSym(const wa_host_call_ctx_t& ctx) {
        const auto name = memory_.String(ctx.args[1]);
        const auto handle = ctx.args[0];
        const auto entry = dl_handles_.find(handle);
        const bool open = entry != dl_handles_.end() && entry->second.references;
        const auto default_handle = arch_ == WA_ARCH_ARM64 ? uint64_t{0} : uint64_t{UINT32_MAX};
        uint64_t address = 0;
        if (handle == default_handle || (handle == kProcessHandle && open)) {
            address = dbt::FindElfSymbol(*dbt_, 0, name);
            if (!address) address = dbt::FindVirtualSymbol(*dbt_, {}, name);
        } else if (open) {
            if (entry->second.base) {
                address = dbt::FindElfSymbol(*dbt_, entry->second.base, name);
                if (!address) {
                    const auto& needed = libraries_.at(entry->second.name).info.needed;
                    const std::vector<std::string_view> scope(needed.begin(), needed.end());
                    dbt::ResolveImport(*dbt_, scope, name, address);
                }
            } else {
                address = dbt::FindVirtualSymbol(*dbt_, entry->second.name, name);
            }
        } else {
            return DlFailure("dlsym: invalid library handle");
        }
        return address ? address : DlFailure("dlsym: symbol unavailable: " + name);
    }

    uint64_t DlClose(const wa_host_call_ctx_t& ctx) {
        const auto entry = dl_handles_.find(ctx.args[0]);
        if (entry == dl_handles_.end() || !entry->second.references)
            return DlFailure("dlclose: invalid library handle", UINT32_MAX);
        // The Java process retains loaded images until shutdown.
        --entry->second.references;
        return 0;
    }

    uint64_t DlError(const wa_host_call_ctx_t&) {
        if (dl_error_.empty()) return 0;
        if (!dl_error_address_) dl_error_address_ = dbt::AllocateGuest(*dbt_, kDlErrorCapacity);
        if (!dl_error_address_) throw GuestFault{WA_ERR_NOMEM};
        auto error = std::exchange(dl_error_, {});
        if (error.size() >= kDlErrorCapacity) error.resize(kDlErrorCapacity - 1);
        memory_.Write(dl_error_address_, error.c_str(), error.size() + 1);
        return dl_error_address_;
    }

    const ApkArchive& apk_;
    const wa_guest_arch_t arch_;
    std::map<std::string, Library, std::less<>> libraries_;
    std::map<uint32_t, Library*> files_;
    std::vector<Library*> load_order_;
    std::map<std::string, uint64_t, std::less<>> virtual_handles_;
    std::map<uint64_t, DlHandle> dl_handles_;
    uint64_t next_bias_ = kFirstImage, dl_error_address_ = 0;
    uint32_t next_file_id_ = 1;
    unsigned depth_ = 0;
    std::string dl_error_;
    std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)> mmu_;
    std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)> dbt_;
    GuestMemory memory_;
    std::unique_ptr<HybridJniBridge> jni_;
};

class ApkNativeLibraries final : public interp::GuestNativeLibraries {
  public:
    ApkNativeLibraries(JNIEnv* env, const std::string& path)
        : jni_(interp::JniBridge::From(env)), apk_(path) {}

    std::string FindLibrary(std::string_view name) override {
        if (name.empty() || name.find_first_of("/\\") != name.npos || name.find('\0') != name.npos)
            return {};
        const auto filename = "lib" + std::string(name) + ".so";
        if (const auto arch = Select(filename, {})) return apk_.path + "!/" + EntryName(*arch, filename);
        return {};
    }

    std::string Load(std::string_view path) override {
        const auto request = ParseLibraryPath(apk_, path);
        if (!request) return "Unsupported APK library path: " + std::string(path);
        const auto arch = Select(request->name, request->abi);
        if (!arch) return "No compatible APK entry for " + std::string(path);
        try {
            if (!runtime_) runtime_ = std::make_unique<HybridNativeRuntime>(apk_, *arch, &jni_);
            return runtime_->LoadForJava(request->name);
        } catch (const GuestFault& fault) {
            return Error("Initialize APK native runtime", fault.status);
        } catch (const std::exception& exception) {
            return exception.what();
        }
    }

    std::optional<interp::Value> Invoke(interp::Method& method, interp::Object* receiver,
                                        std::span<const interp::Value> args) override {
        return runtime_ ? runtime_->Jni().Invoke(method, receiver, args) : std::nullopt;
    }

    interp::GuestNativeStats Stats() const override {
        return runtime_ ? runtime_->Stats() : interp::GuestNativeStats{};
    }

    void Shutdown() noexcept override {
        if (runtime_) runtime_->Shutdown();
    }

  private:
    std::optional<wa_guest_arch_t> Select(std::string_view name, std::optional<wa_guest_arch_t> requested) const {
        for (auto arch : kArchitectures) {
            if ((!requested || arch == *requested) && (!runtime_ || arch == runtime_->Arch()) &&
                apk_.Entry(arch, name)) return arch;
        }
        return std::nullopt;
    }

    interp::JniBridge& jni_;
    ApkArchive apk_;
    std::unique_ptr<HybridNativeRuntime> runtime_;
};

} // namespace

std::unique_ptr<interp::GuestNativeLibraries> OpenGuestNativeApk(JNIEnv* env, const void* mounted_apk) {
    return std::make_unique<ApkNativeLibraries>(env, static_cast<const MountedApk*>(mounted_apk)->path());
}

std::vector<NativeLibraryProbe> ProbeApkNativeLibraries(const std::string& path, wa_guest_arch_t arch,
    std::span<const NativeProbeRequest> requests, JNIEnv* environment) {
    const ApkArchive apk(path);
    HybridNativeRuntime runtime(apk, arch, environment ? &interp::JniBridge::From(environment) : nullptr);
    auto report = runtime.Probe(requests);
    runtime.Shutdown();
    return report;
}

} // namespace wa::system
