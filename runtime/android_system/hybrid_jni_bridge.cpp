#include "hybrid_jni_bridge.h"

#include "abi_layouts.h"
#include "dbt_internal.h"
#include "elf_image.h"

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

size_t ComponentWidth(char type) {
    switch (type) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
    }
}

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
    return std::string(name) + ": guest status " + std::to_string(status) + " at PC " + location;
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
        : owner_(owner), depth_(owner.frames_.size()), old_floor_(owner.frame_floor_),
          host_depth_(owner.jni_.FrameDepth()) {
        owner_.PushFrame(16);
        owner_.frame_floor_ = owner_.frames_.size();
        host_floor_ = owner_.jni_.SetFrameFloor(owner_.jni_.FrameDepth());
    }

    ~Frame() {
        owner_.jni_.RestoreFrameDepth(host_depth_);
        owner_.jni_.SetFrameFloor(host_floor_);
        owner_.ForgetFrames(depth_);
        owner_.frame_floor_ = old_floor_;
    }

  private:
    HybridJniBridge& owner_;
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
            memory_.WriteWord(table + i * width, address);
        }
    };
    populate(env_thunks_, env_names, env_table, WA_FFI_JNI_FIRST_FUNCTION_SLOT, false);
    populate(vm_thunks_, vm_names, vm_table, 3, true);
    memory_.WriteWord(jni_env_, env_table);
    memory_.WriteWord(java_vm_, vm_table);
}

HybridJniBridge::~HybridJniBridge() {
    for (const auto& [id, ref] : refs_) {
        if (ref.type == JNIGlobalRefType) jni_.DeleteGlobal(ref.ref);
        if (ref.type == JNIWeakGlobalRefType) jni_.DeleteWeak(static_cast<jweak>(ref.ref));
    }
    for (const auto& [address, copy] : array_copies_) jni_.DeleteGlobal(copy.array);
    for (const auto& [object, monitor] : monitors_) {
        object->monitor_depth -= monitor.depth;
        jni_.DeleteGlobal(monitor.ref);
    }
}

int32_t HybridJniBridge::Trampoline(wa_host_call_ctx_t* ctx, void* user) {
    const auto& thunk = *static_cast<Thunk*>(user);
    auto& bridge = *thunk.bridge;
    if (ctx->args[0] != (thunk.invocation ? bridge.java_vm_ : bridge.jni_env_) ||
        bridge.frames_.empty()) return WA_ERR_FAULT;
    try {
        if (thunk.invocation) bridge.DispatchVm(thunk.slot, *ctx);
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

void HybridJniBridge::DispatchVm(uint16_t slot, wa_host_call_ctx_t& ctx) {
    auto* vm = jni_.JavaVm();
    switch (slot) {
    case 3: Return(ctx, vm->DestroyJavaVM()); break;
    case 4: case 7: {
        JNIEnv* env = nullptr;
        const jint status = slot == 4 ? vm->AttachCurrentThread(&env, nullptr)
                                     : vm->AttachCurrentThreadAsDaemon(&env, nullptr);
        memory_.WriteWord(ctx.args[1], env ? jni_env_ : 0);
        Return(ctx, status);
        break;
    }
    case 5: Return(ctx, vm->DetachCurrentThread()); break;
    case 6: {
        void* env = nullptr;
        const auto status = vm->GetEnv(&env, static_cast<jint>(ctx.args[2]));
        memory_.WriteWord(ctx.args[1], env ? jni_env_ : 0);
        Return(ctx, status);
        break;
    }
    default: throw GuestFault{WA_ERR_INVALID};
    }
}

void HybridJniBridge::PushFrame(size_t capacity) {
    frames_.emplace_back();
    try {
        jni_.PushFrame(capacity);
    } catch (...) {
        frames_.pop_back();
        throw;
    }
}

void HybridJniBridge::ForgetFrames(size_t depth) {
    while (frames_.size() > depth) {
        for (const auto id : frames_.back()) refs_.erase(id);
        frames_.pop_back();
    }
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

Method* HybridJniBridge::Method(uint64_t id) const {
    const auto found = methods_.find(static_cast<uint32_t>(id));
    if (id > UINT32_MAX || found == methods_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second;
}

Field* HybridJniBridge::Field(uint64_t id) const {
    const auto found = fields_.find(static_cast<uint32_t>(id));
    if (id > UINT32_MAX || found == fields_.end()) throw GuestFault{WA_ERR_INVALID};
    return found->second;
}

uint32_t HybridJniBridge::Track(jobject ref, jobjectRefType type) {
    if (!ref) return 0;
    const auto id = NextHandle();
    if (type == JNILocalRefType) frames_.back().push_back(id);
    try {
        refs_.emplace(id, Reference{ref, type, transaction_ ? transaction_->number : 0});
    } catch (...) {
        if (type == JNIGlobalRefType) jni_.DeleteGlobal(ref);
        if (type == JNIWeakGlobalRefType) jni_.DeleteWeak(static_cast<jweak>(ref));
        throw;
    }
    return id;
}

uint32_t HybridJniBridge::Track(jmethodID method) {
    if (!method) return 0;
    if (const auto found = method_ids_.find(method); found != method_ids_.end()) return found->second;
    const auto id = NextHandle();
    methods_.emplace(id, reinterpret_cast<interp::Method*>(method));
    method_ids_.emplace(method, id);
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
    if (type == JNILocalRefType) jni_.DeleteLocal(ref);
    if (type == JNIGlobalRefType) jni_.DeleteGlobal(ref);
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
    if (!transaction_) {
        if (address) registrations_.insert_or_assign(method, Registration{address});
        else registrations_.erase(method);
        return;
    }
    const auto found = registrations_.find(method);
    transaction_->previous.try_emplace(method, found == registrations_.end() ? Registration{} : found->second);
    registrations_.insert_or_assign(method, Registration{address, transaction_->number});
}

void HybridJniBridge::Rollback(Transaction& transaction) {
    for (const auto& [method, before] : transaction.previous) {
        const auto current = registrations_.find(method);
        if (current == registrations_.end() || current->second.transaction != transaction.number) continue;
        if (before.transaction || before.address) current->second = before;
        else registrations_.erase(current);
    }
    for (auto it = refs_.begin(); it != refs_.end();) {
        const auto [id, reference] = *it++;
        if (reference.transaction == transaction.number && reference.type != JNILocalRefType)
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

void HybridJniBridge::RemoveImage(uint64_t base) { std::erase(images_, base); }

std::string HybridJniBridge::OnLoad(uint64_t address) {
    Frame frame(*this);
    Transaction transaction{++next_transaction_, {}};
    struct Restore {
        Transaction*& slot;
        Transaction* previous;
        ~Restore() { slot = previous; }
    } restore{transaction_, std::exchange(transaction_, &transaction)};
    try {
        const uint64_t args[]{java_vm_, 0};
        uint64_t result = 0;
        const auto status = wa_dbt_call(&dbt_, address, args, 2, &result);
        std::string failure;
        if (Vm().exception) {
            failure = "JNI_OnLoad: " + Vm().ExceptionSummary();
            Vm().exception = nullptr;
        } else if (status != WA_OK) {
            failure = CallFailure(dbt_, "JNI_OnLoad", status);
        } else {
            void* env = nullptr;
            if (jni_.JavaVm()->GetEnv(&env, static_cast<jint>(result)) != JNI_OK)
                failure = "JNI_OnLoad returned invalid version " + std::to_string(static_cast<jint>(result));
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

uint64_t HybridJniBridge::Lookup(interp::Method& method) const {
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

std::optional<Value> HybridJniBridge::Invoke(interp::Method& method, Object* receiver,
                                            std::span<const Value> args) {
    const uint64_t address = Lookup(method);
    if (!address) return std::nullopt;
    try {
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
        const auto status = wa_dbt_call_abi(&dbt_, address, &registers, &result);
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
            memory_.Read(address, &value, ComponentWidth(type));
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
    const size_t width = ComponentWidth(type);

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
    FORWARD(IsSameObject)
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
        if (frames_.size() <= frame_floor_) {
            ctx.ret0 = ctx.args[1];
        } else {
            const auto result = jni_.PopFrame(keep);
            ForgetFrames(frames_.size() - 1);
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
        auto& type = Vm().linker.Resolve(method->name == "<init>" ? "Ljava/lang/reflect/Constructor;"
                                                                 : "Ljava/lang/reflect/Method;");
        auto* mirror = Vm().heap.Allocate(Vm(), type);
        mirror->reflected_method = method;
        Return(ctx, jni_.Local(mirror));
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
