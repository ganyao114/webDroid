#pragma once

#include "guest_abi.h"
#include "jni_bridge.h"

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

  private:
    enum class Slot : uint16_t;
    struct Thunk {
        HybridJniBridge* bridge = nullptr;
        uint16_t slot = 0;
        bool invocation = false;
    };
    struct Reference {
        jobject ref;
        jobjectRefType type;
        uint64_t transaction;
    };
    struct Registration {
        uint64_t address = 0;
        uint64_t transaction = 0;
    };
    struct Transaction {
        uint64_t number;
        std::map<interp::Method*, Registration> previous;
    };
    struct ArrayCopy {
        jarray array;
        int32_t length;
    };
    struct Monitor {
        jobject ref;
        unsigned depth;
    };

    class Frame;
    static int32_t Trampoline(wa_host_call_ctx_t* ctx, void* user);
    void Dispatch(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchVm(uint16_t slot, wa_host_call_ctx_t& ctx);
    void DispatchCalls(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchStrings(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchArrays(Slot slot, wa_host_call_ctx_t& ctx);
    void DispatchDirectBuffers(Slot slot, wa_host_call_ctx_t& ctx);

    void PushFrame(size_t capacity);
    void ForgetFrames(size_t depth);
    void DeleteReference(uint64_t id, jobjectRefType type);
    jobject Ref(uint64_t id) const;
    interp::Method* Method(uint64_t id) const;
    interp::Field* Field(uint64_t id) const;
    uint32_t Track(jobject ref, jobjectRefType type = JNILocalRefType);
    uint32_t Track(jmethodID id);
    uint32_t Track(jfieldID id);
    uint32_t NextHandle();
    uint64_t CopyToGuest(const void* bytes, size_t count);
    void SetRegistration(interp::Method* method, uint64_t address);
    void Rollback(Transaction& transaction);
    jint RegisterNatives(jclass cls, uint64_t methods, jint count);
    void ReadArguments(const interp::Method& method, wa_host_call_ctx_t& ctx, unsigned fixed,
                       unsigned variant, std::span<jvalue> out);
    uint64_t Lookup(interp::Method& method) const;
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
    size_t frame_floor_ = 0;
    std::vector<std::vector<uint32_t>> frames_;
    std::vector<uint64_t> images_;
    std::unordered_map<uint32_t, Reference> refs_;
    std::unordered_map<uint32_t, interp::Method*> methods_;
    std::unordered_map<uint32_t, interp::Field*> fields_;
    std::unordered_map<jmethodID, uint32_t> method_ids_;
    std::unordered_map<jfieldID, uint32_t> field_ids_;
    std::map<interp::Method*, Registration> registrations_;
    std::unordered_map<uint64_t, ArrayCopy> array_copies_;
    std::set<uint64_t> string_copies_;
    std::unordered_map<interp::Object*, Monitor> monitors_;
};

} // namespace wa::system
