#include "hybrid_jni_bridge.h"

#include "abi_layouts.h"
#include "bionic_internal.h"
#include "bitmap_access.h"
#include "dbt_internal.h"
#include "elf_image.h"

#include <android_runtime/android_util_AssetManager.h>

#include <cstdio>
#include <tuple>
#include <type_traits>

namespace wa::system {

using namespace interp;
using dbt::ArgKind;
using dbt::CheckGuest;
using dbt::GuestFault;

enum class HybridJniBridge::Slot : uint16_t {
#define JNI_SLOT(Index, Name) Name = Index,
    WA_FFI_JNI_SLOTS(JNI_SLOT)
#undef JNI_SLOT
};

#define JNI_SLOT(Index, Name) static_assert(offsetof(JNINativeInterface, Name) == Index * sizeof(void*));
WA_FFI_JNI_SLOTS(JNI_SLOT)
#undef JNI_SLOT
static_assert(offsetof(JNIInvokeInterface, DestroyJavaVM) == 3 * sizeof(void*));
static_assert(offsetof(JNIInvokeInterface, AttachCurrentThread) == 4 * sizeof(void*));
static_assert(offsetof(JNIInvokeInterface, DetachCurrentThread) == 5 * sizeof(void*));
static_assert(offsetof(JNIInvokeInterface, GetEnv) == 6 * sizeof(void*));
static_assert(offsetof(JNIInvokeInterface, AttachCurrentThreadAsDaemon) == 7 * sizeof(void*));

namespace {

constexpr std::string_view kPrimitiveTypes = "ZBCSIJFD";

ArgKind Kind(char type) {
    switch (type) {
    case 'J': return ArgKind::Long;
    case 'F': return ArgKind::Float;
    case 'D': return ArgKind::Double;
    default: return ArgKind::Word;
    }
}

// Fixed prefix of AOSP ArtMethod. Its two following fields have guest pointer width.
struct ArtMethodHeader {
    uint32_t declaring_class;
    uint32_t flags;
    uint32_t dex_method_index;
    uint16_t method_index;
    uint16_t hotness_count;
};
static_assert(sizeof(ArtMethodHeader) == 16);
constexpr auto kNativeEntry = sizeof(ArtMethodHeader);

struct NullableString {
    std::optional<std::string> value;
    operator const char*() const { return value ? value->c_str() : nullptr; }
};

std::string Mangle(std::string_view name) {
    std::string mangled;
    for (const char16_t unit : DecodeUtf8(name)) {
        switch (unit) {
        case '/': mangled += '_'; break;
        case '_': mangled += "_1"; break;
        case ';': mangled += "_2"; break;
        case '[': mangled += "_3"; break;
        default:
            if ((unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') ||
                (unit >= '0' && unit <= '9')) {
                mangled += static_cast<char>(unit);
            } else {
                char escape[7];
                std::snprintf(escape, sizeof(escape), "_0%04x", unsigned(unit));
                mangled += escape;
            }
        }
    }
    return mangled;
}

std::string CallFailure(const wa_dbt& dbt, std::string_view name, int32_t status) {
    char location[24];
    std::snprintf(location, sizeof(location), "0x%llx", static_cast<unsigned long long>(dbt.last_pc));
    auto message = std::string(name) + ": guest status " + std::to_string(status) + " at PC " + location;
    if (dbt.last_pc >= WA_HOST_STUB_BASE && dbt.last_pc < WA_HOST_STUB_LIMIT) {
        const auto index = (dbt.last_pc - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
        if (index < dbt.hosts.size()) message += " in " + dbt.hosts[index].name;
    }
    if (status == WA_ERR_FAULT && dbt.last_fault_address) {
        std::snprintf(location, sizeof(location), "0x%llx",
                      static_cast<unsigned long long>(dbt.last_fault_address));
        message += " accessing guest address ";
        message += location;
    }
    return message;
}

void CheckRegion(Runtime& vm, size_t size, int32_t start, int32_t length, std::string_view exception) {
    if (start < 0 || length < 0 || size_t(start) > size || size_t(length) > size - start)
        vm.Fail(exception);
}

class GuestBufferMemory final : public DirectBufferMemory {
  public:
    GuestBufferMemory(wa_dbt& dbt, size_t capacity, uint64_t address)
        : DirectBufferMemory(capacity), dbt_(dbt), address_(address), owned_(!address) {
        if (owned_) {
            address_ = dbt::AllocateGuest(dbt_, capacity);
            if (!address_) throw GuestFault{WA_ERR_NOMEM};
        }
    }
    ~GuestBufferMemory() override {
        if (owned_) dbt::FreeGuest(dbt_, address_);
    }
    bool Read(size_t offset, std::span<uint8_t> bytes) const override {
        return wa_softmmu_read(dbt_.mmu, address_ + offset, bytes.data(), bytes.size()) == WA_OK;
    }
    bool Write(size_t offset, std::span<const uint8_t> bytes) override {
        return wa_softmmu_write(dbt_.mmu, address_ + offset, bytes.data(), bytes.size()) == WA_OK;
    }
    uint64_t GuestAddress() const override { return address_; }

  private:
    wa_dbt& dbt_;
    uint64_t address_;
    const bool owned_;
};

} // namespace

class HybridJniBridge::Frame {
  public:
    explicit Frame(HybridJniBridge& owner)
        : owner_(owner), locals_(owner.CurrentFrames()), depth_(locals_.frames.size()), old_floor_(locals_.floor),
          host_depth_(owner.jni_.FrameDepth()) {
        owner_.PushFrame(16);
        locals_.floor = locals_.frames.size();
        host_floor_ = owner_.jni_.SetFrameFloor(owner_.jni_.FrameDepth());
    }

    ~Frame() {
        owner_.jni_.RestoreFrameDepth(host_depth_);
        owner_.jni_.SetFrameFloor(host_floor_);
        owner_.ForgetFrames(locals_, depth_);
        locals_.floor = old_floor_;
    }

  private:
    HybridJniBridge& owner_;
    LocalFrames& locals_;
    size_t depth_, old_floor_, host_depth_, host_floor_;
};

HybridJniBridge::HybridJniBridge(JniBridge& jni, wa_dbt& dbt, wa_softmmu_t* mmu, wa_guest_arch_t arch)
    : jni_(jni), dbt_(dbt), arch_(arch), memory_(mmu, arch) {
    constexpr std::array env_names{
#define JNI_SLOT(Index, Name) #Name,
        WA_FFI_JNI_SLOTS(JNI_SLOT)
#undef JNI_SLOT
    };
    constexpr std::array vm_names{"reserved0", "reserved1", "reserved2", "DestroyJavaVM",
        "AttachCurrentThread", "DetachCurrentThread", "GetEnv", "AttachCurrentThreadAsDaemon"};
    const auto width = memory_.width();
    jni_env_ = dbt::AllocateGuest(dbt, (2 + env_names.size() + vm_names.size()) * width);
    if (!jni_env_) throw GuestFault{WA_ERR_NOMEM};
    java_vm_ = jni_env_ + width;
    const auto env_table = java_vm_ + width;
    const auto vm_table = env_table + env_names.size() * width;

    const auto populate = [&](auto& thunks, const auto& names, uint64_t table, unsigned first, bool vm) {
        for (unsigned i = first; i < names.size(); ++i) {
            auto& thunk = thunks[i];
            thunk = {this, static_cast<uint16_t>(i), vm};
            uint64_t address;
            CheckGuest(dbt::RegisterHost(dbt, vm ? "JavaVM" : "JNIEnv", names[i], Trampoline,
                                        &thunk, false, &address));
#ifdef __EMSCRIPTEN__
            if (!vm) {
                const auto slot = static_cast<Slot>(i);
                if (slot == Slot::IsSameObject || slot == Slot::DeleteLocalRef ||
                    slot == Slot::ExceptionCheck || slot == Slot::GetArrayLength)
                    dbt::RegisterFastHost(dbt, address, FastEnvCall);
            }
#endif
            memory_.WriteWord(table + i * width, address);
        }
    };
    populate(env_thunks_, env_names, env_table, WA_FFI_JNI_FIRST_FUNCTION_SLOT, false);
    populate(vm_thunks_, vm_names, vm_table, 3, true);
    memory_.WriteWord(jni_env_, env_table);
    memory_.WriteWord(java_vm_, vm_table);
    CheckGuest(wa_dbt_register_symbol(&dbt_, "libjnigraphics.so", "AndroidBitmap_getInfo",
                                      BitmapCall<BitmapOp::Info>, this));
    CheckGuest(wa_dbt_register_symbol(&dbt_, "libjnigraphics.so", "AndroidBitmap_lockPixels",
                                      BitmapCall<BitmapOp::Lock>, this));
    CheckGuest(wa_dbt_register_symbol(&dbt_, "libjnigraphics.so", "AndroidBitmap_unlockPixels",
                                      BitmapCall<BitmapOp::Unlock>, this));
    const dbt::Builtin asset_functions[] = {
        {"AAssetManager_fromJava", AssetCall<AssetOp::Manager>},
        {"AAssetManager_open", AssetCall<AssetOp::Open>},
        {"AAsset_close", AssetCall<AssetOp::Close>},
        {"AAsset_getLength", AssetCall<AssetOp::Length>},
        {"AAsset_getLength64", AssetCall<AssetOp::Length64>},
        {"AAsset_getBuffer", AssetCall<AssetOp::Buffer>},
        {"AAsset_read", AssetCall<AssetOp::Read>},
        {"AAsset_isAllocated", AssetCall<AssetOp::Allocated>},
        {"AConfiguration_new", ConfigurationCall<ConfigurationOp::New>},
        {"AConfiguration_delete", ConfigurationCall<ConfigurationOp::Delete>},
        {"AConfiguration_fromAssetManager", ConfigurationCall<ConfigurationOp::FromAssetManager>},
        {"AConfiguration_getDensity", ConfigurationCall<ConfigurationOp::Density>},
        {"AConfiguration_getScreenWidthDp", ConfigurationCall<ConfigurationOp::WidthDp>},
        {"AConfiguration_getScreenHeightDp", ConfigurationCall<ConfigurationOp::HeightDp>},
    };
    for (const auto& function : asset_functions)
        CheckGuest(wa_dbt_register_symbol(&dbt_, "libandroid.so", function.name, function.function, this));
    RegisterNativeWindow();
    dbt_.on_thread_exit = [](void* context, uint64_t thread) {
        static_cast<HybridJniBridge*>(context)->DetachThread(thread);
    };
    dbt_.thread_exit_context = this;
}

HybridJniBridge::~HybridJniBridge() {
    dbt_.on_thread_exit = nullptr;
    dbt_.thread_exit_context = nullptr;
    for (const auto& [id, manager] : asset_managers_) jni_.DeleteWeak(manager);
    for (auto& [id, window] : windows_) {
        ClearWindowLock(window);
        jni_.DeleteGlobal(window.surface);
    }
    for (const auto& [id, ref] : refs_) {
        if (ref.type == JNIGlobalRefType || ref.retained) jni_.DeleteGlobal(ref.ref);
        if (ref.type == JNIWeakGlobalRefType) jni_.DeleteWeak(static_cast<jweak>(ref.ref));
    }
    for (const auto& [address, copy] : array_copies_) jni_.DeleteGlobal(copy.array);
    for (const auto& [object, monitor] : monitors_) {
        object->monitor_depth -= monitor.depth;
        jni_.DeleteGlobal(monitor.ref);
    }
}

template<HybridJniBridge::AssetOp operation>
int32_t HybridJniBridge::AssetCall(wa_host_call_ctx_t* ctx, void* user) {
    try {
        static_cast<HybridJniBridge*>(user)->DispatchAsset(operation, *ctx);
    } catch (const ManagedException&) {
        return WA_OK;
    } catch (const GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    return WA_OK;
}

template<HybridJniBridge::ConfigurationOp operation>
int32_t HybridJniBridge::ConfigurationCall(wa_host_call_ctx_t* ctx, void* user) {
    try {
        static_cast<HybridJniBridge*>(user)->DispatchConfiguration(operation, *ctx);
    } catch (const ManagedException&) {
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    return WA_OK;
}

void HybridJniBridge::DispatchConfiguration(ConfigurationOp operation, wa_host_call_ctx_t& ctx) {
    if (operation == ConfigurationOp::New) {
        const auto id = NextHandle();
        configurations_.emplace(id, android::ResTable_config{});
        ctx.ret0 = id;
        return;
    }
    const auto found = configurations_.find(ctx.args[0]);
    if (found == configurations_.end()) return;
    auto& config = found->second;
    switch (operation) {
    case ConfigurationOp::Delete: configurations_.erase(found); break;
    case ConfigurationOp::FromAssetManager: {
        const auto manager = asset_managers_.find(ctx.args[1]);
        if (manager == asset_managers_.end() || !JniBridge::Read(manager->second)) return;
        if (auto* guarded = android::AssetManagerForJavaObject(Env(), manager->second)) {
            android::ScopedLock lock(*guarded);
            if (!lock->GetConfigurations().empty()) config = lock->GetConfigurations().front();
        }
        break;
    }
    case ConfigurationOp::Density: ctx.ret0 = config.density; break;
    case ConfigurationOp::WidthDp: ctx.ret0 = config.screenWidthDp; break;
    case ConfigurationOp::HeightDp: ctx.ret0 = config.screenHeightDp; break;
    case ConfigurationOp::New: break;
    }
}

void HybridJniBridge::DispatchAsset(AssetOp operation, wa_host_call_ctx_t& ctx) {
    const auto handle = ctx.args[0];
    if (operation == AssetOp::Manager) {
        if (handle != jni_env_ || !HasLocalFrame()) return;
        auto* object = JniBridge::Read(Ref(ctx.args[1]));
        if (!object || object->type->descriptor != "Landroid/content/res/AssetManager;") return;
        for (const auto& [id, manager] : asset_managers_) {
            if (JniBridge::Read(manager) == object) {
                ctx.ret0 = id;
                return;
            }
        }
        const auto id = NextHandle();
        auto* weak = jni_.Weak(object);
        try {
            asset_managers_.emplace(id, weak);
        } catch (...) {
            jni_.DeleteWeak(weak);
            throw;
        }
        ctx.ret0 = id;
        return;
    }
    if (operation == AssetOp::Open) {
        const auto manager = asset_managers_.find(handle);
        if (manager == asset_managers_.end() || !JniBridge::Read(manager->second) || !ctx.args[1] ||
            ctx.args[2] > android::Asset::ACCESS_BUFFER) return;
        auto* guarded = android::AssetManagerForJavaObject(Env(), manager->second);
        if (!guarded) return;
        const auto filename = memory_.String(ctx.args[1]);
        android::ScopedLock lock(*guarded);
        auto active = lock->StartOperation();
        auto asset = lock->Open(filename, static_cast<android::Asset::AccessMode>(ctx.args[2]));
        if (!asset) return;
        const auto id = NextHandle();
        assets_.emplace(id, GuestAsset{.asset = std::move(asset)});
        ctx.ret0 = id;
        return;
    }
    const auto found = assets_.find(handle);
    if (found == assets_.end()) {
        if (operation == AssetOp::Read) ctx.ret0 = UINT64_MAX;
        return;
    }
    auto& [asset, buffer] = found->second;
    switch (operation) {
    case AssetOp::Close: assets_.erase(found); break;
    case AssetOp::Length: ctx.ret0 = asset->getLength(); break;
    case AssetOp::Length64: dbt::SetScalarResult(ctx, ArgKind::Long, asset->getLength()); break;
    case AssetOp::Allocated: ctx.ret0 = buffer || asset->isAllocated(); break;
    case AssetOp::Read: {
        const auto limit = std::min<uint64_t>({ctx.args[2], uint64_t(INT32_MAX),
                                              uint64_t(std::max<off64_t>(0, asset->getRemainingLength()))});
        std::array<uint8_t, 16 * 1024> bytes;
        uint64_t copied = 0;
        while (copied < limit) {
            const auto read = asset->read(bytes.data(), std::min<uint64_t>(bytes.size(), limit - copied));
            if (read <= 0) {
                if (read < 0 && !copied) copied = UINT64_MAX;
                break;
            }
            memory_.Write(ctx.args[1] + copied, bytes.data(), read);
            copied += read;
        }
        ctx.ret0 = copied;
        break;
    }
    case AssetOp::Buffer: {
        if (!buffer) {
            const auto length = asset->getLength();
            if (length < 0 || static_cast<uint64_t>(length) > SIZE_MAX) return;
            const auto* bytes = static_cast<const uint8_t*>(asset->getBuffer(false));
            if (!bytes && length) return;
            auto copy = AllocateMemory(static_cast<size_t>(length));
            if (!copy->Write(0, {bytes, static_cast<size_t>(length)})) return;
            buffer = std::move(copy);
        }
        ctx.ret0 = buffer->GuestAddress();
        break;
    }
    case AssetOp::Manager: case AssetOp::Open: break;
    }
}

int32_t HybridJniBridge::Trampoline(wa_host_call_ctx_t* ctx, void* user) {
    const auto& thunk = *static_cast<Thunk*>(user);
    auto& bridge = *thunk.bridge;
    if (ctx->args[0] != (thunk.invocation ? bridge.java_vm_ : bridge.jni_env_) ||
        (!thunk.invocation && !bridge.HasLocalFrame())) return WA_ERR_FAULT;
    try {
        if (thunk.method) bridge.DispatchPlatform(*thunk.method, *ctx);
        else if (thunk.invocation) bridge.DispatchVm(thunk.slot, *ctx);
        else bridge.Dispatch(static_cast<Slot>(thunk.slot), *ctx);
        return WA_OK;
    } catch (const ManagedException&) {
        // JNI returns to native code with the exception pending.
        return WA_OK;
    } catch (const GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

#ifdef __EMSCRIPTEN__
bool HybridJniBridge::FastEnvCall(dbt::FastHostCall& call) try {
    const auto& thunk = *static_cast<Thunk*>(call.user);
    auto& bridge = *thunk.bridge;
    if (call.args[0] != bridge.jni_env_ || !bridge.HasLocalFrame()) return false;
    switch (static_cast<Slot>(thunk.slot)) {
    case Slot::IsSameObject:
        call.result = bridge.SameObject(call.args[1], call.args[2]);
        return true;
    case Slot::DeleteLocalRef:
        bridge.DeleteReference(call.args[1], JNILocalRefType);
        return true;
    case Slot::ExceptionCheck:
        call.result = bridge.Vm().exception != nullptr;
        return true;
    case Slot::GetArrayLength: {
        const auto* array = JniBridge::Read(bridge.Ref(call.args[1]));
        if (!array || !array->type->IsArray()) return false;
        call.result = static_cast<uint32_t>(bridge.Vm().ArrayLength(*array));
        return true;
    }
    default: return false;
    }
} catch (const GuestFault&) {
    return false;
}
#endif

template<HybridJniBridge::BitmapOp operation>
int32_t HybridJniBridge::BitmapCall(wa_host_call_ctx_t* ctx, void* user) {
    auto& bridge = *static_cast<HybridJniBridge*>(user);
    jint result = ANDROID_BITMAP_RESULT_BAD_PARAMETER;
    try {
        if (ctx->args[0] == bridge.jni_env_ && bridge.HasLocalFrame()) {
            result = bridge.DispatchBitmap(operation, *ctx);
        }
    } catch (const ManagedException&) {
        result = ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
    } catch (const GuestFault& fault) {
        if (fault.status == WA_ERR_NOMEM) result = ANDROID_BITMAP_RESULT_ALLOCATION_FAILED;
    } catch (const std::bad_alloc&) {
        result = ANDROID_BITMAP_RESULT_ALLOCATION_FAILED;
    }
    bridge.Return(*ctx, result);
    return WA_OK;
}

jint HybridJniBridge::DispatchBitmap(BitmapOp operation, wa_host_call_ctx_t& ctx) {
    const auto bitmap = ui_jni::AccessNativeBitmap(Env(), Ref(ctx.args[1]));
    if (Env()->ExceptionCheck()) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
    if (!bitmap.data) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
    const dbt::GuestMemory memory(ctx);
    if (operation == BitmapOp::Info) {
        if (ctx.args[2]) memory.Write(ctx.args[2], bitmap.info);
        return ANDROID_BITMAP_RESULT_SUCCESS;
    }
    const auto found = bitmap_locks_.find(bitmap.data);
    if (operation == BitmapOp::Unlock) {
        if (found == bitmap_locks_.end()) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
        if (found->second.depth > 1) {
            --found->second.depth;
        } else {
            auto pixels = std::move(found->second.pixels);
            bitmap_locks_.erase(found);
            if (bitmap.writable) {
                if (pixels->size != size_t(bitmap.info.stride) * bitmap.info.height) {
                    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
                }
                std::vector<uint8_t> bytes(pixels->size);
                if (!pixels->Read(0, bytes)) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
                ui_jni::CopyNativeBitmapPixels(bitmap, bytes, true);
            }
        }
        return ANDROID_BITMAP_RESULT_SUCCESS;
    }
    if (found != bitmap_locks_.end()) {
        if (ctx.args[2]) memory.WriteWord(ctx.args[2], found->second.pixels->GuestAddress());
        ++found->second.depth;
        return ANDROID_BITMAP_RESULT_SUCCESS;
    }
    std::vector<uint8_t> bytes(size_t(bitmap.info.stride) * bitmap.info.height);
    ui_jni::CopyNativeBitmapPixels(bitmap, bytes, false);
    auto pixels = AllocateMemory(bytes.size());
    if (!pixels->Write(0, bytes)) return ANDROID_BITMAP_RESULT_ALLOCATION_FAILED;
    const auto address = pixels->GuestAddress();
    bitmap_locks_.emplace(bitmap.data, BitmapLock{std::move(pixels)});
    try {
        if (ctx.args[2]) memory.WriteWord(ctx.args[2], address);
    } catch (const GuestFault&) {
        bitmap_locks_.erase(bitmap.data);
        throw;
    }
    return ANDROID_BITMAP_RESULT_SUCCESS;
}

void HybridJniBridge::DispatchVm(uint16_t slot, wa_host_call_ctx_t& ctx) {
    auto* vm = jni_.JavaVm();
    switch (slot) {
    case 3: Return(ctx, vm->DestroyJavaVM()); break;
    case 4: case 7: {
        JNIEnv* env = nullptr;
        const jint status = slot == 4 ? vm->AttachCurrentThread(&env, nullptr)
                                     : vm->AttachCurrentThreadAsDaemon(&env, nullptr);
        if (env && !IsMainThread()) {
            auto [it, inserted] = attached_threads_.try_emplace(dbt_.running->id);
            if (inserted) {
                try {
                    it->second.frames.emplace_back();
                    it->second.floor = 1;
                } catch (...) {
                    attached_threads_.erase(it);
                    throw;
                }
            }
        }
        memory_.WriteWord(ctx.args[1], env ? jni_env_ : 0);
        Return(ctx, status);
        break;
    }
    case 5: {
        if (IsMainThread()) return Return(ctx, vm->DetachCurrentThread());
        const auto thread = attached_threads_.find(dbt_.running->id);
        if (thread != attached_threads_.end() && thread->second.floor > 1) return Return(ctx, JNI_ERR);
        DetachThread(dbt_.running->id);
        Return(ctx, JNI_OK);
        break;
    }
    case 6: {
        void* env = nullptr;
        auto status = vm->GetEnv(&env, static_cast<jint>(ctx.args[2]));
        if (status == JNI_OK && !IsMainThread() && !attached_threads_.contains(dbt_.running->id)) {
            env = nullptr;
            status = JNI_EDETACHED;
        }
        memory_.WriteWord(ctx.args[1], env ? jni_env_ : 0);
        Return(ctx, status);
        break;
    }
    default: throw GuestFault{WA_ERR_INVALID};
    }
}

bool HybridJniBridge::IsMainThread() const {
    return dbt_.running == &dbt_.bionic->main;
}

bool HybridJniBridge::HasLocalFrame() const {
    return IsMainThread() ? !main_locals_.frames.empty() : attached_threads_.contains(dbt_.running->id);
}

HybridJniBridge::LocalFrames& HybridJniBridge::CurrentFrames() {
    if (IsMainThread()) return main_locals_;
    const auto found = attached_threads_.find(dbt_.running->id);
    if (found == attached_threads_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second;
}

void HybridJniBridge::PushFrame(size_t capacity) {
    auto& frames = CurrentFrames().frames;
    frames.emplace_back();
    try {
        if (IsMainThread()) jni_.PushFrame(capacity);
        else frames.back().reserve(capacity);
    } catch (...) {
        frames.pop_back();
        throw;
    }
}

void HybridJniBridge::ForgetFrames(LocalFrames& locals, size_t depth) {
    while (locals.frames.size() > depth) {
        for (const auto id : locals.frames.back()) {
            const auto found = refs_.find(id);
            if (found == refs_.end()) continue;
            if (found->second.retained) jni_.DeleteGlobal(found->second.ref);
            refs_.erase(found);
        }
        locals.frames.pop_back();
    }
}

void HybridJniBridge::DetachThread(uint64_t thread) {
    const auto found = attached_threads_.find(thread);
    if (found == attached_threads_.end()) return;
    ForgetFrames(found->second, 0);
    attached_threads_.erase(found);
}

uint32_t HybridJniBridge::NextHandle() {
    if (next_handle_ == UINT32_MAX) throw GuestFault{WA_ERR_OVERFLOW};
    return next_handle_++;
}

jobject HybridJniBridge::Ref(uint64_t id) const {
    if (!id) return nullptr;
    const auto found = refs_.find(static_cast<uint32_t>(id));
    if (id > UINT32_MAX || found == refs_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second.ref;
}

bool HybridJniBridge::SameObject(uint64_t left, uint64_t right) const {
    const auto ref = Ref(left);
    return left == right || JniBridge::Read(ref) == JniBridge::Read(Ref(right));
}

Method* HybridJniBridge::Method(uint64_t id) const {
    const auto found = methods_.find(id);
    if (found == methods_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second;
}

Field* HybridJniBridge::Field(uint64_t id) const {
    const auto found = fields_.find(static_cast<uint32_t>(id));
    if (id > UINT32_MAX || found == fields_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second;
}

uint32_t HybridJniBridge::Track(jobject ref, jobjectRefType type) {
    if (!ref) return 0;
    if (auto* object = JniBridge::Read(ref); object && object->type->super &&
        object->type->super->descriptor == "Ljava/lang/reflect/Executable;" && object->reflected_method) {
        const auto& field = object->type->super->fields.at(MemberKey("artMethod", "J"));
        if (!object->fields[field.slot].j()) {
            object->fields[field.slot] = Value::Long(Track(reinterpret_cast<jmethodID>(object->reflected_method)));
        }
    }
    const auto id = NextHandle();
    const bool retained = type == JNILocalRefType && !IsMainThread();
    if (retained) {
        // The caller's host JNI frame can unwind while an attached guest thread is parked.
        auto* local = ref;
        ref = jni_.Global(JniBridge::Read(local));
        jni_.DeleteLocal(local);
    }
    try {
        if (type == JNILocalRefType) CurrentFrames().frames.back().push_back(id);
        refs_.emplace(id, Reference{ref, type, transaction_ ? transaction_->number : 0, retained});
    } catch (...) {
        if (type == JNIGlobalRefType || retained) jni_.DeleteGlobal(ref);
        if (type == JNIWeakGlobalRefType) jni_.DeleteWeak(static_cast<jweak>(ref));
        throw;
    }
    return id;
}

uint64_t HybridJniBridge::Track(jmethodID handle) {
    if (!handle) return 0;
    auto& method = *reinterpret_cast<interp::Method*>(handle);
    if (const auto found = method_ids_.find(&method); found != method_ids_.end()) {
        if (method.IsNative() && !found->second.default_entry) RefreshEntry(method, found->second, false);
        return found->second.id;
    }
    const auto size = sizeof(ArtMethodHeader) + 2 * memory_.width();
    if (next_method_ + size > method_page_end_) {
        const auto page = dbt::AllocateGuest(dbt_, WA_PAGE_SIZE);
        if (!page) throw GuestFault{WA_ERR_NOMEM};
        next_method_ = page;
        method_page_end_ = next_method_ + WA_PAGE_SIZE;
    }
    const auto id = std::exchange(next_method_, next_method_ + size);
    memory_.Write(id, ArtMethodHeader{0, method.flags, method.index, 0, 0});
    const auto record = method_ids_.try_emplace(&method, GuestMethod{.id = id}).first;
    try {
        methods_.emplace(id, &method);
        if (method.IsNative()) {
            RefreshEntry(method, record->second, true);
            if (method.builtin || method.native) {
                record->second.platform = {.bridge = this, .method = &method};
                CheckGuest(dbt::RegisterHost(dbt_, "ART JNI", method.QualifiedName(), Trampoline,
                    &record->second.platform, false, &record->second.platform_entry));
                if (!record->second.default_entry) {
                    record->second.default_entry = record->second.platform_entry;
                    memory_.WriteWord(id + kNativeEntry, record->second.default_entry);
                }
            }
        }
    } catch (...) {
        methods_.erase(id);
        method_ids_.erase(record);
        throw;
    }
    return id;
}

uint32_t HybridJniBridge::Track(jfieldID field) {
    if (!field) return 0;
    if (const auto found = field_ids_.find(field); found != field_ids_.end()) return found->second;
    const auto id = NextHandle();
    fields_.emplace(id, reinterpret_cast<interp::Field*>(field));
    field_ids_.emplace(field, id);
    return id;
}

void HybridJniBridge::DeleteReference(uint64_t id, jobjectRefType type) {
    if (!id) return;
    const auto found = refs_.find(static_cast<uint32_t>(id));
    if (id > UINT32_MAX || found == refs_.end() || found->second.type != type)
        throw GuestFault{WA_ERR_INVALID};
    const auto ref = found->second.ref;
    if (type == JNILocalRefType && !found->second.retained) jni_.DeleteLocal(ref);
    if (type == JNIGlobalRefType || found->second.retained) jni_.DeleteGlobal(ref);
    if (type == JNIWeakGlobalRefType) jni_.DeleteWeak(static_cast<jweak>(ref));
    refs_.erase(found);
}

template<class T> auto HybridJniBridge::ReadArgument(dbt::CallReader& reader) {
    if constexpr (std::is_same_v<T, const char*>) {
        const auto address = reader.Next();
        return NullableString{address ? std::optional(memory_.String(address)) : std::nullopt};
    } else if constexpr (std::is_convertible_v<T, jobject>) {
        return static_cast<T>(Ref(reader.Next()));
    } else if constexpr (std::is_same_v<T, jmethodID>) {
        return reinterpret_cast<T>(Method(reader.Next()));
    } else if constexpr (std::is_same_v<T, jfieldID>) {
        return reinterpret_cast<T>(Field(reader.Next()));
    } else if constexpr (std::is_same_v<T, jlong>) {
        return std::bit_cast<jlong>(reader.Next(ArgKind::Long));
    } else if constexpr (std::is_same_v<T, jdouble>) {
        return std::bit_cast<jdouble>(reader.Next(ArgKind::Double));
    } else if constexpr (std::is_same_v<T, jfloat>) {
        return std::bit_cast<jfloat>(static_cast<uint32_t>(reader.Next(ArgKind::Float)));
    } else {
        return static_cast<T>(reader.Next());
    }
}

template<class T> void HybridJniBridge::Return(wa_host_call_ctx_t& ctx, T value) {
    if constexpr (std::is_convertible_v<T, jobject> || std::is_same_v<T, jmethodID> ||
                  std::is_same_v<T, jfieldID>) {
        ctx.ret0 = Track(value);
    } else if constexpr (std::is_same_v<T, jfloat>) {
        dbt::SetScalarResult(ctx, ArgKind::Float, std::bit_cast<uint32_t>(value));
    } else if constexpr (std::is_same_v<T, jdouble>) {
        dbt::SetScalarResult(ctx, ArgKind::Double, std::bit_cast<uint64_t>(value));
    } else if constexpr (std::is_same_v<T, jlong>) {
        dbt::SetScalarResult(ctx, ArgKind::Long, static_cast<uint64_t>(value));
    } else {
        ctx.ret0 = static_cast<uint32_t>(value);
    }
}

template<class R, class... Args>
void HybridJniBridge::Forward(wa_host_call_ctx_t& ctx, R (*function)(JNIEnv*, Args...)) {
    dbt::CallReader reader(ctx);
    reader.Next(); // JNIEnv*
    const std::tuple arguments{ReadArgument<Args>(reader)...};
    std::apply([&](const auto&... args) {
        if constexpr (std::is_void_v<R>) function(Env(), args...);
        else Return(ctx, function(Env(), args...));
    }, arguments);
}

void HybridJniBridge::SetRegistration(interp::Method* method, uint64_t address) {
    const auto record = method_ids_.find(method);
    if (!transaction_) {
        if (address) registrations_.insert_or_assign(method, Registration{address});
        else registrations_.erase(method);
    } else {
        const auto found = registrations_.find(method);
        if (!transaction_->previous.contains(method)) {
            const auto entry = record == method_ids_.end() ? std::nullopt :
                std::optional(memory_.ReadWord(record->second.id + kNativeEntry));
            transaction_->previous.emplace(method, PreviousRegistration{
                found == registrations_.end() ? Registration{} : found->second, entry});
        }
        registrations_.insert_or_assign(method, Registration{address, transaction_->number});
    }
    if (record != method_ids_.end()) RefreshEntry(*method, record->second, true);
}

void HybridJniBridge::RefreshEntry(interp::Method& method, GuestMethod& record, bool replace) {
    const auto native = FindNative(method);
    const auto entry = native ? native : record.platform_entry;
    if (replace || memory_.ReadWord(record.id + kNativeEntry) == record.default_entry)
        memory_.WriteWord(record.id + kNativeEntry, entry);
    record.default_entry = entry;
}

void HybridJniBridge::Rollback(Transaction& transaction) {
    for (const auto& [method, before] : transaction.previous) {
        const auto current = registrations_.find(method);
        if (current == registrations_.end() || current->second.transaction != transaction.number) continue;
        if (before.registration.transaction || before.registration.address) current->second = before.registration;
        else registrations_.erase(current);
        if (const auto record = method_ids_.find(method); record != method_ids_.end()) {
            RefreshEntry(*method, record->second, true);
            if (before.entry) memory_.WriteWord(record->second.id + kNativeEntry, *before.entry);
        }
    }
    for (auto it = refs_.begin(); it != refs_.end();) {
        const auto [id, reference] = *it++;
        if (reference.transaction == transaction.number &&
            (reference.type != JNILocalRefType || reference.retained))
            DeleteReference(id, reference.type);
    }
}

jint HybridJniBridge::RegisterNatives(jclass cls, uint64_t methods, jint count) {
    if (count < 0) return JNI_ERR;
    auto& type = jni_.AsClass(cls);
    std::vector<std::pair<interp::Method*, uint64_t>> entries;
    if (static_cast<size_t>(count) > type.methods.size()) return JNI_ERR;
    entries.reserve(count);
    const auto width = memory_.width();
    for (jint i = 0; i < count; ++i, methods += 3 * width) {
        const auto name = memory_.String(memory_.ReadWord(methods));
        const auto signature = memory_.String(memory_.ReadWord(methods + width));
        const auto address = memory_.ReadWord(methods + 2 * width);
        const auto found = type.methods.find(MemberKey(name, signature));
        if (found == type.methods.end() || !found->second.IsNative())
            Vm().Fail("Ljava/lang/NoSuchMethodError;", type.descriptor + "->" + name + signature);
        if (!dbt::IsExecutable(dbt_, address))
            Vm().Fail("Ljava/lang/UnsatisfiedLinkError;", found->second.QualifiedName());
        entries.emplace_back(&found->second, address);
    }
    for (const auto& [method, address] : entries) SetRegistration(method, address);
    return JNI_OK;
}

void HybridJniBridge::AddImage(uint64_t base) { images_.push_back(base); }

void HybridJniBridge::RemoveImage(uint64_t base) {
    std::erase(images_, base);
    for (auto& [method, record] : method_ids_) {
        if (method->IsNative() && record.default_entry && record.default_entry != record.platform_entry)
            RefreshEntry(*method, record, false);
    }
}

std::string HybridJniBridge::OnLoad(uint64_t address) {
    Frame frame(*this);
    Transaction transaction{++next_transaction_, {}};
    struct Restore {
        Transaction*& slot;
        Transaction* previous;
        ~Restore() { slot = previous; }
    } restore{transaction_, std::exchange(transaction_, &transaction)};
    try {
        const wa_guest_call_frame_t args{.gpr = {java_vm_, 0}};
        wa_guest_call_result_t result{};
        const auto status = dbt::CallInitializer(dbt_, address, args, result);
        std::string failure;
        if (Vm().exception) {
            failure = "JNI_OnLoad: " + Vm().ExceptionSummary();
            Vm().exception = nullptr;
        } else if (status != WA_OK) {
            failure = CallFailure(dbt_, "JNI_OnLoad", status);
        } else {
            void* env = nullptr;
            const auto version = static_cast<jint>(result.gpr[0]);
            if (jni_.JavaVm()->GetEnv(&env, version) != JNI_OK)
                failure = "JNI_OnLoad returned invalid version " + std::to_string(version);
        }
        if (!failure.empty()) Rollback(transaction);
        return failure;
    } catch (...) {
        Rollback(transaction);
        throw;
    }
}

void HybridJniBridge::OnUnload(uint64_t address) {
    Frame frame(*this);
    const uint64_t args[]{java_vm_, 0};
    uint64_t result;
    wa_dbt_call(&dbt_, address, args, 2, &result);
    Vm().exception = nullptr;
}

uint64_t HybridJniBridge::FindNative(interp::Method& method) const {
    if (const auto entry = registrations_.find(&method); entry != registrations_.end() && entry->second.address)
        return entry->second.address;
    const std::string_view owner = method.owner->descriptor;
    const auto short_name = "Java_" + Mangle(owner.substr(1, owner.size() - 2)) + "_" + Mangle(method.name);
    const std::string_view signature = method.signature;
    const auto long_name = short_name + "__" + Mangle(signature.substr(1, signature.find(')') - 1));
    for (const auto base : images_) {
        if (const auto address = dbt::FindElfSymbol(dbt_, base, short_name)) return address;
        if (const auto address = dbt::FindElfSymbol(dbt_, base, long_name)) return address;
    }
    return 0;
}

uint64_t HybridJniBridge::Lookup(interp::Method& method) {
    if (const auto found = method_ids_.find(&method); found != method_ids_.end()) {
        auto& record = found->second;
        if (!record.default_entry) RefreshEntry(method, record, false);
        const auto entry = memory_.ReadWord(record.id + kNativeEntry);
        // The original platform entry can run directly unless native code has replaced it.
        return entry == record.platform_entry ? 0 : entry;
    }
    if ((method.builtin || method.native) && !registrations_.contains(&method)) return 0;
    return FindNative(method);
}

std::unique_ptr<DirectBufferMemory> HybridJniBridge::AllocateMemory(size_t size) {
    return std::make_unique<GuestBufferMemory>(dbt_, size, 0);
}

void HybridJniBridge::DispatchPlatform(interp::Method& method, wa_host_call_ctx_t& ctx) {
    dbt::CallReader reader(ctx);
    reader.Next(); // JNIEnv*
    auto* receiver = JniBridge::Read(Ref(reader.Next()));
    std::vector<Value> args;
    args.reserve(method.shorty.size() - 1);
    for (const auto type : std::string_view(method.shorty).substr(1)) {
        const auto bits = reader.Next(Kind(type));
        args.push_back(IsReference(type) ? Value::Ref(JniBridge::Read(Ref(bits))) : Normalize(Value::Bits(bits), type));
    }
    const auto value = jni_.InvokePlatform(method, method.IsStatic() ? nullptr : receiver, args);
    const auto type = method.shorty.front();
    if (IsReference(type)) Return(ctx, jni_.Local(value.ref()));
    else if (type != 'V') dbt::SetScalarResult(ctx, Kind(type), value.bits());
}

std::optional<Value> HybridJniBridge::Invoke(interp::Method& method, Object* receiver,
                                            std::span<const Value> args) {
    try {
        const auto address = Lookup(method);
        if (!address) return std::nullopt;
        Frame frame(*this);
        dbt::CallArguments arguments(arch_);
        arguments.Add(ArgKind::Word, jni_env_);
        arguments.Add(ArgKind::Word, Track(jni_.Local(method.IsStatic() ? Vm().ClassObject(*method.owner) : receiver)));
        for (size_t i = 0; i < args.size(); ++i) {
            const char type = method.shorty[i + 1];
            arguments.Add(Kind(type), IsReference(type) ? Track(jni_.Local(args[i].ref())) : args[i].bits());
        }
        const auto registers = arguments.Frame();
        wa_guest_call_result_t result{};
        const auto status = dbt::CallJniMethod(dbt_, address, registers, result);
        if (Vm().exception) throw ManagedException{};
        if (status != WA_OK) Vm().Fail("Ljava/lang/UnsatisfiedLinkError;", CallFailure(dbt_, method.QualifiedName(), status));
        const char type = method.shorty.front();
        if (type == 'V') return Value{};
        if (IsReference(type)) return Value::Ref(JniBridge::Read(Ref(result.gpr[0])));
        uint64_t bits = result.gpr[0];
        if (arch_ == WA_ARCH_ARM64 && (type == 'F' || type == 'D')) bits = result.fp_bits;
        if (arch_ == WA_ARCH_ARM32 && IsWide(type)) bits |= result.gpr[1] << 32;
        return Normalize(Value::Bits(bits), type);
    } catch (const GuestFault& fault) {
        Vm().Fail("Ljava/lang/UnsatisfiedLinkError;", CallFailure(dbt_, method.QualifiedName(), fault.status));
    } catch (const std::bad_alloc&) {
        Vm().Fail("Ljava/lang/OutOfMemoryError;", method.QualifiedName());
    }
}

void HybridJniBridge::ReadArguments(const interp::Method& method, wa_host_call_ctx_t& ctx,
                                    unsigned fixed, unsigned variant, std::span<jvalue> out) {
    dbt::CallReader call(ctx);
    for (unsigned i = 0; i < fixed; ++i) call.Next();
    const auto decode = [&](auto&& next) {
        for (size_t i = 0; i < out.size(); ++i) {
            const char type = method.shorty[i + 1];
            const auto bits = next(type, i);
            auto& j = out[i];
            switch (type) {
            case 'L': case '[': j.l = Ref(bits); break;
            case 'Z': j.z = static_cast<jboolean>(bits); break;
            case 'B': j.b = static_cast<jbyte>(bits); break;
            case 'C': j.c = static_cast<jchar>(bits); break;
            case 'S': j.s = static_cast<jshort>(bits); break;
            case 'I': j.i = static_cast<jint>(bits); break;
            case 'J': j.j = std::bit_cast<jlong>(bits); break;
            case 'D': j.d = std::bit_cast<jdouble>(bits); break;
            case 'F':
                j.f = variant == 2 ? std::bit_cast<jfloat>(static_cast<uint32_t>(bits))
                                   : static_cast<jfloat>(std::bit_cast<jdouble>(bits));
                break;
            default: throw GuestFault{WA_ERR_INVALID};
            }
        }
    };
    if (variant == 2) {
        const auto values = call.Next();
        decode([&](char type, size_t i) {
            const auto address = values + 8 * i;
            if (IsReference(type)) return memory_.ReadWord(address);
            uint64_t value = 0;
            memory_.Read(address, &value, PrimitiveWidth(type));
            return value;
        });
    } else if (variant == 1 && !out.empty()) {
        dbt::VaListReader list(ctx, call.Next());
        decode([&](char type, size_t) { return list.Next(type == 'F' ? ArgKind::Double : Kind(type)); });
    } else {
        decode([&](char type, size_t) { return call.Next(type == 'F' ? ArgKind::Double : Kind(type)); });
    }
}

void HybridJniBridge::DispatchCalls(Slot slot, wa_host_call_ctx_t& ctx) {
    if (slot >= Slot::NewObject && slot <= Slot::NewObjectA) {
        const auto method = Method(ctx.args[2]);
        std::vector<jvalue> args(method->shorty.size() - 1);
        ReadArguments(*method, ctx, 3, unsigned(slot) - unsigned(Slot::NewObject), args);
        Return(ctx, Env()->NewObjectA(static_cast<jclass>(Ref(ctx.args[1])),
                                      reinterpret_cast<jmethodID>(method), args.data()));
        return;
    }
    const bool is_static = slot >= Slot::CallStaticObjectMethod;
    const bool nonvirtual = slot >= Slot::CallNonvirtualObjectMethod && !is_static;
    const auto first = is_static ? Slot::CallStaticObjectMethod :
        nonvirtual ? Slot::CallNonvirtualObjectMethod : Slot::CallObjectMethod;
    const unsigned index = unsigned(slot) - unsigned(first);
    const auto method = Method(ctx.args[nonvirtual ? 3 : 2]);
    const auto id = reinterpret_cast<jmethodID>(method);
    std::vector<jvalue> args(method->shorty.size() - 1);
    ReadArguments(*method, ctx, nonvirtual ? 4 : 3, index % 3, args);
    const auto receiver = Ref(ctx.args[1]);
    const auto cls = static_cast<jclass>(is_static ? receiver : nonvirtual ? Ref(ctx.args[2]) : nullptr);
    auto* env = Env();
    // Match the native table's Object, primitive, and Void result groups.
    switch (index / 3) {
#define CALL(Index, Name) \
    case Index: \
        if (is_static) Return(ctx, env->CallStatic##Name##MethodA(cls, id, args.data())); \
        else if (nonvirtual) Return(ctx, env->CallNonvirtual##Name##MethodA(receiver, cls, id, args.data())); \
        else Return(ctx, env->Call##Name##MethodA(receiver, id, args.data())); \
        return;
        CALL(0, Object)
        CALL(1, Boolean)
        CALL(2, Byte)
        CALL(3, Char)
        CALL(4, Short)
        CALL(5, Int)
        CALL(6, Long)
        CALL(7, Float)
        CALL(8, Double)
#undef CALL
    case 9:
        if (is_static) env->CallStaticVoidMethodA(cls, id, args.data());
        else if (nonvirtual) env->CallNonvirtualVoidMethodA(receiver, cls, id, args.data());
        else env->CallVoidMethodA(receiver, id, args.data());
        return;
    default: throw GuestFault{WA_ERR_INVALID};
    }
}

uint64_t HybridJniBridge::CopyToGuest(const void* data, size_t size) {
    const auto address = dbt::AllocateGuest(dbt_, size);
    if (!address) throw GuestFault{WA_ERR_NOMEM};
    try {
        memory_.Write(address, data, size);
        return address;
    } catch (...) {
        dbt::FreeGuest(dbt_, address);
        throw;
    }
}

void HybridJniBridge::DispatchStrings(Slot slot, wa_host_call_ctx_t& ctx) {
    switch (slot) {
    case Slot::NewString: {
        const auto length = static_cast<jsize>(ctx.args[2]);
        if (length < 0) Vm().Fail("Ljava/lang/IllegalArgumentException;", "Negative string length");
        std::u16string text(length, u'\0');
        memory_.Read(ctx.args[1], text.data(), text.size() * sizeof(char16_t));
        Return(ctx, jni_.Local(Vm().NewString(std::move(text))));
        return;
    }
    case Slot::ReleaseStringChars: case Slot::ReleaseStringUTFChars: case Slot::ReleaseStringCritical:
        if (string_copies_.erase(ctx.args[2])) dbt::FreeGuest(dbt_, ctx.args[2]);
        return;
    default: break;
    }
    const auto& string = Vm().NonNull(JniBridge::Read(Ref(ctx.args[1])));
    if (string.type->descriptor != "Ljava/lang/String;")
        Vm().Fail("Ljava/lang/IllegalArgumentException;", "Expected a string");
    const auto& text = string.text;
    if (slot == Slot::GetStringRegion || slot == Slot::GetStringUTFRegion) {
        const auto start = static_cast<int32_t>(ctx.args[2]), length = static_cast<int32_t>(ctx.args[3]);
        CheckRegion(Vm(), text.size(), start, length, "Ljava/lang/StringIndexOutOfBoundsException;");
        const auto region = std::u16string_view(text).substr(start, length);
        const auto buffer = arch_ == WA_ARCH_ARM32 ? memory_.ReadWord(ctx.sp) : ctx.args[4];
        if (slot == Slot::GetStringRegion) {
            memory_.Write(buffer, region.data(), region.size() * sizeof(char16_t));
        } else {
            const auto utf8 = EncodeUtf8(region);
            memory_.Write(buffer, utf8.data(), utf8.size());
        }
        return;
    }
    if (ctx.args[2]) memory_.Write<jboolean>(ctx.args[2], JNI_TRUE);
    const auto utf8 = slot == Slot::GetStringUTFChars ? EncodeUtf8(text) : std::string{};
    const auto address = slot == Slot::GetStringUTFChars
        ? CopyToGuest(utf8.c_str(), utf8.size() + 1)
        : CopyToGuest(text.c_str(), (text.size() + 1) * sizeof(char16_t));
    try {
        string_copies_.insert(address);
    } catch (...) {
        dbt::FreeGuest(dbt_, address);
        throw;
    }
    ctx.ret0 = address;
}

void HybridJniBridge::DispatchArrays(Slot slot, wa_host_call_ctx_t& ctx) {
    const auto component = [&](Slot first) { return kPrimitiveTypes[unsigned(slot) - unsigned(first)]; };
    if (slot >= Slot::NewBooleanArray && slot <= Slot::NewDoubleArray) {
        const char type[]{'[', component(Slot::NewBooleanArray), '\0'};
        const auto object = Vm().NewArray(Vm().linker.Resolve(type), static_cast<jsize>(ctx.args[1]));
        Return(ctx, jni_.Local(object));
        return;
    }
    auto& vm = Vm();
    auto& array = vm.NonNull(JniBridge::Read(Ref(ctx.args[1])));
    if (!array.type->IsArray() || !array.type->component->IsPrimitive())
        vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected a primitive array");
    const char type = array.type->component->kind;
    const auto size = vm.ArrayLength(array);
    const size_t width = PrimitiveWidth(type);

    const auto check_type = [&](char expected) {
        if (type != expected) vm.Fail("Ljava/lang/IllegalArgumentException;", "Incorrect primitive array type");
    };
    const auto transfer = [&](bool into_java, uint64_t address, int32_t index, size_t count) {
        std::array<uint8_t, 1024> bytes;
        for (size_t done = 0; done < count;) {
            const size_t chunk = std::min(count - done, bytes.size() / width);
            if (into_java) memory_.Read(address + done * width, bytes.data(), chunk * width);
            for (size_t i = 0; i < chunk; ++i) {
                auto* part = bytes.data() + i * width;
                const auto element = static_cast<int32_t>(index + done + i);
                if (into_java) {
                    uint64_t bits = 0;
                    std::memcpy(&bits, part, width);
                    vm.StoreArrayElement(array, element, Value::Bits(bits));
                } else {
                    const auto bits = vm.ArrayElement(array, element).bits();
                    std::memcpy(part, &bits, width);
                }
            }
            if (!into_java) memory_.Write(address + done * width, bytes.data(), chunk * width);
            done += chunk;
        }
    };

    if (slot == Slot::GetPrimitiveArrayCritical ||
        (slot >= Slot::GetBooleanArrayElements && slot <= Slot::GetDoubleArrayElements)) {
        if (slot != Slot::GetPrimitiveArrayCritical) check_type(component(Slot::GetBooleanArrayElements));
        const auto address = dbt::AllocateGuest(dbt_, uint64_t(size) * width);
        if (!address) throw GuestFault{WA_ERR_NOMEM};
        jarray ref = nullptr;
        try {
            transfer(false, address, 0, size);
            if (ctx.args[2]) memory_.Write<jboolean>(ctx.args[2], JNI_TRUE);
            ref = static_cast<jarray>(jni_.Global(&array));
            array_copies_.emplace(address, ArrayCopy{ref, static_cast<jsize>(size)});
        } catch (...) {
            jni_.DeleteGlobal(ref);
            dbt::FreeGuest(dbt_, address);
            throw;
        }
        ctx.ret0 = address;
        return;
    }
    if (slot == Slot::ReleasePrimitiveArrayCritical ||
        (slot >= Slot::ReleaseBooleanArrayElements && slot <= Slot::ReleaseDoubleArrayElements)) {
        if (slot != Slot::ReleasePrimitiveArrayCritical) check_type(component(Slot::ReleaseBooleanArrayElements));
        const auto found = array_copies_.find(ctx.args[2]);
        const auto mode = static_cast<jint>(ctx.args[3]);
        if (found == array_copies_.end() || JniBridge::Read(found->second.array) != &array ||
            (mode != 0 && mode != JNI_COMMIT && mode != JNI_ABORT))
            throw GuestFault{WA_ERR_INVALID};
        if (mode != JNI_ABORT) transfer(true, found->first, 0, found->second.length);
        if (mode != JNI_COMMIT) {
            jni_.DeleteGlobal(found->second.array);
            dbt::FreeGuest(dbt_, found->first);
            array_copies_.erase(found);
        }
        return;
    }
    const bool into_java = slot >= Slot::SetBooleanArrayRegion;
    check_type(component(into_java ? Slot::SetBooleanArrayRegion : Slot::GetBooleanArrayRegion));
    const auto start = static_cast<jsize>(ctx.args[2]), length = static_cast<jsize>(ctx.args[3]);
    CheckRegion(vm, size, start, length, "Ljava/lang/ArrayIndexOutOfBoundsException;");
    const auto buffer = arch_ == WA_ARCH_ARM32 ? memory_.ReadWord(ctx.sp) : ctx.args[4];
    transfer(into_java, buffer, start, length);
}

void HybridJniBridge::DispatchDirectBuffers(Slot slot, wa_host_call_ctx_t& ctx) {
    if (slot == Slot::NewDirectByteBuffer) {
        dbt::CallReader args(ctx);
        args.Next();
        const auto address = args.Next();
        const auto capacity = std::bit_cast<jlong>(args.Next(ArgKind::Long));
        if (!address || address >= WA_HOST_STUB_BASE || capacity < 0 || capacity > INT32_MAX ||
            static_cast<uint64_t>(capacity) > WA_HOST_STUB_BASE - address)
            Vm().Fail("Ljava/lang/IllegalArgumentException;", "Invalid JNI direct buffer");
        auto memory = std::make_unique<GuestBufferMemory>(dbt_, capacity, address);
        Return(ctx, jni_.Local(Vm().NewDirectBuffer(capacity, std::move(memory))));
        return;
    }
    const auto view = Vm().DirectBuffer(JniBridge::Read(Ref(ctx.args[1])));
    if (slot == Slot::GetDirectBufferCapacity) {
        Return(ctx, static_cast<jlong>(view ? view->capacity : -1));
        return;
    }
    if (!view) return;
    auto& memory = *view->backing;
    if (!memory->GuestAddress()) {
        // Migrate once. All existing Java views refer to this backing array.
        auto guest = std::make_unique<GuestBufferMemory>(dbt_, memory->size, 0);
        std::array<uint8_t, WA_PAGE_SIZE> bytes;
        for (size_t offset = 0; offset < memory->size; offset += bytes.size()) {
            auto chunk = std::span(bytes).first(std::min(bytes.size(), memory->size - offset));
            if (!memory->Read(offset, chunk) || !guest->Write(offset, chunk)) throw GuestFault{WA_ERR_FAULT};
        }
        memory = std::move(guest);
    }
    ctx.ret0 = memory->GuestAddress() + view->offset;
}

void HybridJniBridge::Dispatch(Slot slot, wa_host_call_ctx_t& ctx) {
    if ((slot >= Slot::NewObject && slot <= Slot::NewObjectA) ||
        (slot >= Slot::CallObjectMethod && slot <= Slot::CallNonvirtualVoidMethodA) ||
        (slot >= Slot::CallStaticObjectMethod && slot <= Slot::CallStaticVoidMethodA))
        return DispatchCalls(slot, ctx);
    if ((slot >= Slot::NewBooleanArray && slot <= Slot::SetDoubleArrayRegion) ||
        slot == Slot::GetPrimitiveArrayCritical || slot == Slot::ReleasePrimitiveArrayCritical)
        return DispatchArrays(slot, ctx);
    if ((slot >= Slot::NewString && slot <= Slot::ReleaseStringUTFChars) &&
        slot != Slot::NewStringUTF && slot != Slot::GetStringLength && slot != Slot::GetStringUTFLength)
        return DispatchStrings(slot, ctx);
    if (slot >= Slot::NewDirectByteBuffer && slot <= Slot::GetDirectBufferCapacity)
        return DispatchDirectBuffers(slot, ctx);

    const auto& table = *Env()->functions;
    switch (slot) {
#define FORWARD(Name) case Slot::Name: return Forward(ctx, table.Name);
    FORWARD(GetVersion)
    FORWARD(FindClass)
    FORWARD(GetSuperclass)
    FORWARD(IsAssignableFrom)
    FORWARD(Throw)
    FORWARD(ThrowNew)
    FORWARD(ExceptionOccurred)
    FORWARD(ExceptionDescribe)
    FORWARD(ExceptionClear)
    FORWARD(ExceptionCheck)
    FORWARD(NewLocalRef)
    FORWARD(EnsureLocalCapacity)
    FORWARD(AllocObject)
    FORWARD(GetObjectClass)
    FORWARD(IsInstanceOf)
    FORWARD(GetMethodID)
    FORWARD(GetStaticMethodID)
    FORWARD(GetFieldID)
    FORWARD(GetStaticFieldID)
    FORWARD(NewStringUTF)
    FORWARD(GetStringLength)
    FORWARD(GetStringUTFLength)
    FORWARD(GetArrayLength)
    FORWARD(NewObjectArray)
    FORWARD(GetObjectArrayElement)
    FORWARD(SetObjectArrayElement)
#define FORWARD_FIELDS(Type) \
    FORWARD(Get##Type##Field) FORWARD(Set##Type##Field) \
    FORWARD(GetStatic##Type##Field) FORWARD(SetStatic##Type##Field)
    FORWARD_FIELDS(Object)
    FORWARD_FIELDS(Boolean)
    FORWARD_FIELDS(Byte)
    FORWARD_FIELDS(Char)
    FORWARD_FIELDS(Short)
    FORWARD_FIELDS(Int)
    FORWARD_FIELDS(Long)
    FORWARD_FIELDS(Float)
    FORWARD_FIELDS(Double)
#undef FORWARD_FIELDS
#undef FORWARD
    case Slot::GetStringRegion: case Slot::GetStringUTFRegion:
    case Slot::GetStringCritical: case Slot::ReleaseStringCritical:
        return DispatchStrings(slot, ctx);
    case Slot::IsSameObject:
        ctx.ret0 = SameObject(ctx.args[1], ctx.args[2]);
        return;
    case Slot::GetObjectRefType: {
        const auto id = ctx.args[1];
        const auto found = refs_.find(static_cast<uint32_t>(id));
        Return(ctx, id <= UINT32_MAX && found != refs_.end() ? found->second.type : JNIInvalidRefType);
        return;
    }
    case Slot::NewGlobalRef:
        ctx.ret0 = Track(jni_.Global(JniBridge::Read(Ref(ctx.args[1]))), JNIGlobalRefType);
        return;
    case Slot::NewWeakGlobalRef:
        ctx.ret0 = Track(jni_.Weak(JniBridge::Read(Ref(ctx.args[1]))), JNIWeakGlobalRefType);
        return;
    case Slot::DeleteGlobalRef: return DeleteReference(ctx.args[1], JNIGlobalRefType);
    case Slot::DeleteWeakGlobalRef: return DeleteReference(ctx.args[1], JNIWeakGlobalRefType);
    case Slot::DeleteLocalRef: return DeleteReference(ctx.args[1], JNILocalRefType);
    case Slot::PushLocalFrame: {
        const auto capacity = static_cast<jint>(ctx.args[1]);
        if (capacity < 0) Return(ctx, JNI_ERR);
        else PushFrame(capacity);
        return;
    }
    case Slot::PopLocalFrame: {
        const auto keep = Ref(ctx.args[1]);
        auto& locals = CurrentFrames();
        if (locals.frames.size() <= locals.floor) {
            ctx.ret0 = ctx.args[1];
        } else {
            const auto result = IsMainThread() ? jni_.PopFrame(keep) : jni_.Local(JniBridge::Read(keep));
            ForgetFrames(locals, locals.frames.size() - 1);
            Return(ctx, result);
        }
        return;
    }
    case Slot::RegisterNatives:
        try {
            Return(ctx, RegisterNatives(static_cast<jclass>(Ref(ctx.args[1])), ctx.args[2], static_cast<jint>(ctx.args[3])));
        } catch (const ManagedException&) {
            Return(ctx, JNI_ERR);
        }
        return;
    case Slot::UnregisterNatives:
        for (auto& [key, method] : jni_.AsClass(static_cast<jclass>(Ref(ctx.args[1]))).methods)
            if (method.IsNative()) SetRegistration(&method, 0);
        return;
    case Slot::GetJavaVM:
        if (ctx.args[1]) memory_.WriteWord(ctx.args[1], java_vm_);
        else Return(ctx, JNI_ERR);
        return;
    case Slot::MonitorEnter: {
        auto& object = Vm().NonNull(JniBridge::Read(Ref(ctx.args[1])));
        auto [monitor, inserted] = monitors_.try_emplace(&object, Monitor{nullptr, 0});
        if (inserted) {
            try {
                monitor->second.ref = jni_.Global(&object);
            } catch (...) {
                monitors_.erase(monitor);
                throw;
            }
        }
        ++monitor->second.depth;
        ++object.monitor_depth;
        return;
    }
    case Slot::MonitorExit: {
        auto& object = Vm().NonNull(JniBridge::Read(Ref(ctx.args[1])));
        const auto monitor = monitors_.find(&object);
        if (monitor == monitors_.end()) {
            Return(ctx, JNI_ERR);
            Vm().Fail("Ljava/lang/IllegalMonitorStateException;");
        }
        --object.monitor_depth;
        if (!--monitor->second.depth) {
            jni_.DeleteGlobal(monitor->second.ref);
            monitors_.erase(monitor);
        }
        return;
    }
    case Slot::FromReflectedMethod: {
        const auto& mirror = Vm().NonNull(JniBridge::Read(Ref(ctx.args[1])));
        if (mirror.type->descriptor != "Ljava/lang/reflect/Method;" &&
            mirror.type->descriptor != "Ljava/lang/reflect/Constructor;")
            Vm().Fail("Ljava/lang/IllegalArgumentException;", "Expected a reflected method");
        Return(ctx, reinterpret_cast<jmethodID>(mirror.reflected_method));
        return;
    }
    case Slot::FromReflectedField: {
        const auto& mirror = Vm().NonNull(JniBridge::Read(Ref(ctx.args[1])));
        if (mirror.type->descriptor != "Ljava/lang/reflect/Field;")
            Vm().Fail("Ljava/lang/IllegalArgumentException;", "Expected a reflected field");
        Return(ctx, reinterpret_cast<jfieldID>(mirror.reflected_field));
        return;
    }
    case Slot::ToReflectedMethod: {
        const auto method = Method(ctx.args[2]);
        Return(ctx, jni_.Local(Vm().NewReflectedMethod(*method, ctx.args[2])));
        return;
    }
    case Slot::ToReflectedField: {
        auto& type = Vm().linker.Resolve("Ljava/lang/reflect/Field;");
        auto* mirror = Vm().heap.Allocate(Vm(), type);
        mirror->reflected_field = Field(ctx.args[2]);
        Return(ctx, jni_.Local(mirror));
        return;
    }
    case Slot::DefineClass:
        Vm().Fail("Ljava/lang/UnsupportedOperationException;", "JNI DefineClass requires a Java classfile loader");
    case Slot::FatalError:
        Vm().exception = Vm().MakeException("Ljava/lang/InternalError;", memory_.String(ctx.args[1]));
        throw GuestFault{WA_ERR_FAULT};
    default: throw GuestFault{WA_ERR_NOSYS};
    }
}

} // namespace wa::system
