#pragma once

#include "guest_abi.h"
#include "jni_bridge.h"

#include <androidfw/ResourceTypes.h>

namespace wa::dbt { struct FastHostCall; }
namespace wa::ui_jni::detail {
struct BitmapData;
}
namespace android { class Asset; }

namespace wa::system {

class HybridJniBridge {
  public:
    HybridJniBridge(interp::JniBridge& jni, wa_dbt& dbt, wa_softmmu_t* mmu, wa_guest_arch_t arch);
    ~HybridJniBridge();

    void AddImage(uint64_t base);
    void RemoveImage(uint64_t base);
    std::string OnLoad(uint64_t address);
    void OnUnload(uint64_t address);
    std::optional<interp::Value> Invoke(interp::Method& method, interp::Object* receiver,
                                        std::span<const interp::Value> args);
    std::unique_ptr<interp::DirectBufferMemory> AllocateMemory(size_t size);

  private:
    enum class Slot : uint16_t;
    struct Thunk {
        HybridJniBridge* bridge = nullptr;
        uint16_t slot = 0;
        bool invocation = false;
        interp::Method* method = nullptr;
    };
    struct Reference {
        jobject ref;
        jobjectRefType type;
        uint64_t transaction;
        bool retained = false;
    };
    struct LocalFrames {
        std::vector<std::vector<uint32_t>> frames;
        size_t floor = 0;
    };
    struct Registration {
        uint64_t address = 0;
        uint64_t transaction = 0;
    };
    struct PreviousRegistration {
        Registration registration;
        std::optional<uint64_t> entry;
    };
    struct Transaction {
        uint64_t number;
        std::map<interp::Method*, PreviousRegistration> previous;
    };
    struct GuestMethod {
        uint64_t id = 0;
        uint64_t default_entry = 0;
        uint64_t platform_entry = 0;
        Thunk platform;
    };
    struct ArrayCopy {
        jarray array;
        int32_t length;
    };
    struct Monitor {
        jobject ref;
        unsigned depth;
    };
    struct BitmapLock {
        std::unique_ptr<interp::DirectBufferMemory> pixels;
        unsigned depth = 1;
    };
    enum class BitmapOp { Info, Lock, Unlock };
    struct GuestAsset {
        std::unique_ptr<android::Asset> asset;
        std::unique_ptr<interp::DirectBufferMemory> buffer;
    };
    enum class AssetOp { Manager, Open, Close, Length, Length64, Buffer, Read, Allocated };
    enum class ConfigurationOp { New, Delete, FromAssetManager, Density, WidthDp, HeightDp };
    struct GuestWindow {
        jobject surface;
        jobject canvas = nullptr;
        jobject bitmap = nullptr;
        unsigned references = 1;
        std::unique_ptr<interp::DirectBufferMemory> pixels;
    };
    enum class WindowOp { FromSurface, Acquire, Release, Width, Height, Format, Geometry, Lock, Post };

    class Frame;
    static int32_t Trampoline(wa_host_call_ctx_t* ctx, void* user);
#ifdef __EMSCRIPTEN__
    static bool FastEnvCall(dbt::FastHostCall& call);
#endif
    template<BitmapOp operation> static int32_t BitmapCall(wa_host_call_ctx_t* ctx, void* user);
    template<AssetOp operation> static int32_t AssetCall(wa_host_call_ctx_t* ctx, void* user);
    void DispatchAsset(AssetOp operation, wa_host_call_ctx_t& ctx);
    template<ConfigurationOp operation> static int32_t ConfigurationCall(wa_host_call_ctx_t* ctx, void* user);
    void DispatchConfiguration(ConfigurationOp operation, wa_host_call_ctx_t& ctx);
    template<WindowOp operation> static int32_t WindowCall(wa_host_call_ctx_t* ctx, void* user);
    void RegisterNativeWindow();
    void DispatchWindow(WindowOp operation, wa_host_call_ctx_t& ctx);
    void ClearWindowLock(GuestWindow& window);
    jint DispatchBitmap(BitmapOp operation, wa_host_call_ctx_t& ctx);
    void Dispatch(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchVm(uint16_t slot, wa_host_call_ctx_t& ctx);
    void DispatchCalls(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchStrings(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchArrays(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchDirectBuffers(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchPlatform(interp::Method& method, wa_host_call_ctx_t& ctx);

    bool IsMainThread() const;
    bool HasLocalFrame() const;
    LocalFrames& CurrentFrames();
    void PushFrame(size_t capacity);
    void ForgetFrames(LocalFrames& locals, size_t depth);
    void DetachThread(uint64_t thread);
    void DeleteReference(uint64_t id, jobjectRefType type);
    jobject Ref(uint64_t id) const;
    bool SameObject(uint64_t left, uint64_t right) const;
    interp::Method* Method(uint64_t id) const;
    interp::Field* Field(uint64_t id) const;
    uint32_t Track(jobject ref, jobjectRefType type = JNILocalRefType);
    uint64_t Track(jmethodID id);
    uint32_t Track(jfieldID id);
    uint32_t NextHandle();
    uint64_t CopyToGuest(const void* bytes, size_t count);
    void SetRegistration(interp::Method* method, uint64_t address);
    void RefreshEntry(interp::Method& method, GuestMethod& record, bool replace);
    void Rollback(Transaction& transaction);
    jint RegisterNatives(jclass cls, uint64_t methods, jint count);
    void ReadArguments(const interp::Method& method, wa_host_call_ctx_t& ctx, unsigned fixed,
                       unsigned variant, std::span<jvalue> out);
    uint64_t Lookup(interp::Method& method);
    uint64_t FindNative(interp::Method& method) const;
    interp::Runtime& Vm() { return jni_.Vm(); }
    JNIEnv* Env() { return jni_.Env(); }

    template<class T> auto ReadArgument(dbt::CallReader& reader);
    template<class T> void Return(wa_host_call_ctx_t& ctx, T value);
    template<class R, class... Args> void Forward(wa_host_call_ctx_t& ctx, R (*function)(JNIEnv*, Args...));

    interp::JniBridge& jni_;
    wa_dbt& dbt_;
    const wa_guest_arch_t arch_;
    dbt::GuestMemory memory_;
    uint64_t java_vm_ = 0, jni_env_ = 0;
    std::array<Thunk, 233> env_thunks_;
    std::array<Thunk, 8> vm_thunks_;
    uint32_t next_handle_ = 0x10000;
    uint64_t next_transaction_ = 0;
    Transaction* transaction_ = nullptr;
    LocalFrames main_locals_;
    std::unordered_map<uint64_t, LocalFrames> attached_threads_;
    std::vector<uint64_t> images_;
    std::unordered_map<uint32_t, Reference> refs_;
    std::unordered_map<uint64_t, interp::Method*> methods_;
    std::unordered_map<uint32_t, interp::Field*> fields_;
    std::unordered_map<interp::Method*, GuestMethod> method_ids_;
    uint64_t next_method_ = 0, method_page_end_ = 0;
    std::unordered_map<jfieldID, uint32_t> field_ids_;
    std::map<interp::Method*, Registration> registrations_;
    std::unordered_map<uint64_t, ArrayCopy> array_copies_;
    std::set<uint64_t> string_copies_;
    std::unordered_map<interp::Object*, Monitor> monitors_;
    std::map<std::shared_ptr<ui_jni::detail::BitmapData>, BitmapLock> bitmap_locks_;
    std::map<uint64_t, jweak> asset_managers_;
    std::map<uint64_t, GuestAsset> assets_;
    std::map<uint64_t, android::ResTable_config> configurations_;
    std::map<uint64_t, GuestWindow> windows_;
};

} // namespace wa::system
