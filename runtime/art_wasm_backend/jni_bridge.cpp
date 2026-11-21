#include "runtime_internal.h"

#include "jni_dbt.h"

#include <bit>
#include <cstring>

namespace wa::art {
namespace {

using namespace wa::ffi;

constexpr uint32_t kFileId = 1;
constexpr uint64_t kEnv = 0x10000;
constexpr uint64_t kScratch = 0x20000;
constexpr uint32_t kScratchSize = 64 * 1024;
constexpr uint64_t kLoadBias = 0x1000000;
constexpr uint64_t kClassHandle = 1ull << 32;
constexpr uint64_t kMethodHandle = 2ull << 32;
constexpr uint64_t kObjectHandle = 3ull << 32;

uint64_t handle(uint64_t tag, uint32_t id) {
    return tag | (uint64_t(id) + 1);
}

uint32_t id_of(uint64_t value, uint64_t tag, size_t count) {
    const auto id = static_cast<uint32_t>(value) - 1;
    return ((value & ~uint64_t{UINT32_MAX}) == tag && id < count) ? id : kNone;
}

uint64_t object_handle(uint32_t ref) {
    return ref ? handle(kObjectHandle, ref) : 0;
}

struct UtfBuffer {
    uint64_t address;
    uint32_t ref;
    bool released = false;
};

} // namespace

struct JniBridge {
    struct Frame {
        JniBridge& bridge;
        Frame* previous;
        wa_art_thread_t& state;
        uint32_t thread;
        uint32_t roots;
        uint32_t scratch;
        std::vector<UtfBuffer> utf;

        Frame(JniBridge& bridge, wa_art_thread_t& state, uint32_t thread)
            : bridge(bridge), previous(bridge.active), state(state), thread(thread),
              roots(state.ref_top), scratch(bridge.scratch_top) {
            bridge.active = this;
        }

        ~Frame() {
            state.ref_top = roots;
            bridge.scratch_top = scratch;
            bridge.active = previous;
        }
    };

    wa_art_heap_t& heap;
    const std::vector<uint8_t> elf;
    const wa_art_invoke_java_fn invoke;
    void* const user;
    std::vector<uint64_t> methods;
    wa_art_jni_stats_t stats{};
    uint32_t scratch_top = 0;
    Frame* active = nullptr;
    std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)> mmu{nullptr, wa_softmmu_destroy};
    JniCallbacks callbacks;
    std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)> dbt{nullptr, wa_dbt_destroy};

    JniBridge(wa_art_heap_t& heap, std::span<const uint8_t> bytes, wa_art_invoke_java_fn invoke,
              void* user)
        : heap(heap), elf(bytes.begin(), bytes.end()), invoke(invoke), user(user),
          methods(heap.dex.methods.size()) {
        callbacks.user = this;
        callbacks.find_class = find_class;
        callbacks.new_string_utf = new_string_utf;
        callbacks.register_natives = register_natives;
        callbacks.raw = raw;
    }

    static int64_t read_page(void* opaque, uint32_t file, void* dst, size_t size, uint64_t off) {
        const auto& self = *static_cast<const JniBridge*>(opaque);
        if (file != kFileId || off > self.elf.size() || size > self.elf.size() - off)
            return WA_ERR_FAULT;
        std::memcpy(dst, self.elf.data() + off, size);
        return static_cast<int64_t>(size);
    }

    int32_t load(const char* registration_symbol) {
        mmu.reset(wa_softmmu_create(read_page, this));
        if (!mmu)
            return WA_ERR_NOMEM;
        auto status = wa_softmmu_map_anon(mmu.get(), kEnv, WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_WRITE);
        if (status != WA_OK)
            return status;
        status = wa_softmmu_map_anon(mmu.get(), kScratch, kScratchSize, WA_PROT_READ | WA_PROT_WRITE);
        if (status != WA_OK)
            return status;
        dbt.reset(wa_dbt_create(WA_ARCH_ARM64, mmu.get()));
        if (!dbt)
            return WA_ERR_NOMEM;
        JniStubPcs stubs{};
        status = bind_jni_stubs(dbt.get(), callbacks, stubs);
        if (status != WA_OK)
            return status;
        status = build_jni_env(mmu.get(), kEnv, stubs);
        if (status != WA_OK)
            return status;
        uint64_t base = 0;
        status = wa_dbt_load_elf(dbt.get(), kFileId, elf.data(), elf.size(), kLoadBias, &base);
        if (status != WA_OK)
            return status;
        uint64_t entry = 0;
        status = wa_dbt_find_symbol(dbt.get(), registration_symbol, &entry);
        if (status != WA_OK)
            return status;
        uint64_t result = 0;
        status = wa_dbt_call(dbt.get(), entry, &kEnv, 1, &result);
        if (status != WA_OK)
            return status;
        return static_cast<uint32_t>(result) == 0 ? WA_OK : WA_ERR_INVALID;
    }

    static int32_t find_class(void* user, uint64_t env, std::string_view name, uint64_t& result) {
        if (env != kEnv || name.empty())
            return WA_ERR_INVALID;
        auto& self = *static_cast<JniBridge*>(user);
        const auto descriptor = name.front() == '[' ? std::string(name) :
                                "L" + std::string(name) + ";";
        const auto type = self.heap.types.find(descriptor);
        if (type != self.heap.types.end())
            result = handle(kClassHandle, type->second);
        return WA_OK;
    }

    static int32_t new_string_utf(void* user, uint64_t env, std::string_view text,
                                  uint64_t& result) {
        auto& self = *static_cast<JniBridge*>(user);
        if (env != kEnv || !self.active)
            return WA_ERR_INVALID;
        const auto thread = self.active->thread;
        if (self.active->state.pending_exception)
            return WA_OK;
        const auto ref = self.heap.new_string(thread, text);
        if (ref && !self.heap.push_root(thread, ref)) {
            wa_art_rt_raise(&self.heap, thread, WA_ART_STACK_OVERFLOW);
            return WA_OK;
        }
        result = object_handle(ref);
        return WA_OK;
    }

    static int32_t register_natives(void* user, uint64_t env, uint64_t clazz,
                                     std::span<const JniNativeRegistration> entries,
                                     int32_t& result) {
        auto& self = *static_cast<JniBridge*>(user);
        const auto type = id_of(clazz, kClassHandle, self.heap.classes.size());
        if (env != kEnv || type == kNone)
            return WA_ERR_INVALID;
        std::vector<std::pair<uint32_t, uint64_t>> pending;
        pending.reserve(entries.size());
        for (const auto& entry : entries) {
            const auto id = self.heap.dex.find_method(type, entry.name, entry.signature);
            if (id == kNone || !(self.heap.dex.methods[id].access & kNative) ||
                (entry.guest_function & 3) || entry.guest_function >= WA_HOST_STUB_BASE) {
                result = -1;
                return WA_OK;
            }
            const auto& method = self.heap.dex.methods[id];
            const auto& proto = self.heap.dex.protos[method.proto];
            const auto& dex = self.heap.dex;
            if (proto.parameters.size() + !(method.access & kStatic) > 6 ||
                dex.types[proto.result] == "F" || dex.types[proto.result] == "D" ||
                std::any_of(proto.parameters.begin(), proto.parameters.end(),
                            [&](auto type) { return dex.types[type] == "D"; })) {
                result = -1;
                return WA_OK;
            }
            pending.emplace_back(id, entry.guest_function);
        }
        for (const auto& [method, address] : pending) {
            if (!self.methods[method])
                ++self.stats.registered_methods;
            self.methods[method] = address;
        }
        result = 0;
        return WA_OK;
    }

    uint32_t reference(uint64_t value) const {
        if (!value)
            return 0;
        const auto ref = id_of(value, kObjectHandle, UINT32_MAX);
        return heap.object(ref) ? ref : kNone;
    }

    int32_t get_utf(wa_host_call_ctx_t& ctx) {
        const auto ref = reference(ctx.args[1]);
        std::string_view text;
        if (!heap.string_value(ref, text))
            return WA_ERR_INVALID;
        const auto length = text.size() + 1;
        if (length > kScratchSize - scratch_top) {
            wa_art_rt_raise(&heap, active->thread, WA_ART_OUT_OF_MEMORY);
            return detail::jni_result(ctx, WA_OK, 0);
        }
        const uint64_t address = kScratch + scratch_top;
        auto status = wa_softmmu_write(ctx.mmu, address, text.data(), length);
        if (status != WA_OK)
            return status;
        if (ctx.args[2]) {
            const uint8_t is_copy = 1;
            status = wa_softmmu_write(ctx.mmu, ctx.args[2], &is_copy, sizeof(is_copy));
            if (status != WA_OK)
                return status;
        }
        active->utf.push_back({address, ref});
        scratch_top += static_cast<uint32_t>(length);
        return detail::jni_result(ctx, WA_OK, address);
    }

    int32_t release_utf(wa_host_call_ctx_t& ctx) {
        const auto ref = reference(ctx.args[1]);
        auto& buffers = active->utf;
        const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto& buffer) {
            return buffer.address == ctx.args[2] && buffer.ref == ref && !buffer.released;
        });
        if (found == buffers.end())
            return WA_ERR_INVALID;
        found->released = true;
        while (!buffers.empty() && buffers.back().released) {
            scratch_top = static_cast<uint32_t>(buffers.back().address - kScratch);
            buffers.pop_back();
        }
        return detail::jni_result(ctx, WA_OK, 0);
    }

    int32_t static_method_id(wa_host_call_ctx_t& ctx) {
        auto clazz = id_of(ctx.args[1], kClassHandle, heap.classes.size());
        if (clazz == kNone)
            return WA_ERR_INVALID;
        std::string name;
        std::string signature;
        const auto limit = callbacks.limits.string_bytes;
        auto status = detail::read_jni_string(ctx.mmu, ctx.args[2], limit, name);
        if (status != WA_OK)
            return status;
        status = detail::read_jni_string(ctx.mmu, ctx.args[3], limit, signature);
        if (status != WA_OK)
            return status;
        for (; clazz != kNone; clazz = heap.classes[clazz].parent) {
            const auto method = heap.dex.find_method(clazz, name, signature);
            if (method != kNone && (heap.dex.methods[method].access & kStatic))
                return detail::jni_result(ctx, WA_OK, handle(kMethodHandle, method));
        }
        wa_art_rt_raise(&heap, active->thread, WA_ART_NO_SUCH_METHOD);
        return detail::jni_result(ctx, WA_OK, 0);
    }

    int32_t call_static_int(wa_host_call_ctx_t& ctx, bool array) {
        const auto clazz = id_of(ctx.args[1], kClassHandle, heap.classes.size());
        const auto id = id_of(ctx.args[2], kMethodHandle, heap.dex.methods.size());
        if (clazz == kNone || id == kNone)
            return WA_ERR_INVALID;
        const auto& method = heap.dex.methods[id];
        const auto& proto = heap.dex.protos[method.proto];
        if (!(method.access & kStatic) || heap.dex.types[proto.result] != "I" ||
            !heap.assignable(clazz, method.owner))
            return WA_ERR_INVALID;
        if (active->state.pending_exception)
            return detail::jni_result(ctx, WA_OK, 0);

        const auto count = proto.parameters.size();
        std::vector<JValue> jvalues(array ? count : 0);
        if (array) {
            if (count && !ctx.args[3])
                return WA_ERR_FAULT;
            const auto status = read_jvalues(ctx.mmu, ctx.args[3], jvalues);
            if (status != WA_OK)
                return status;
        } else if (count > 5) {
            return WA_ERR_NOTSUP;
        }
        std::vector<uint64_t> args;
        args.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto descriptor = heap.dex.types[proto.parameters[i]];
            const auto type = kind(descriptor);
            uint64_t bits;
            if (descriptor == "D" || descriptor == "F") {
                if (!array)
                    return WA_ERR_NOTSUP;
                bits = descriptor == "F" ? std::bit_cast<uint32_t>(jvalues[i].get_f()) :
                                           std::bit_cast<uint64_t>(jvalues[i].get_d());
            } else if (type == Kind::Reference) {
                bits = reference(array ? jvalues[i].get_l() : ctx.args[i + 3]);
                if (bits == kNone)
                    return WA_ERR_INVALID;
            } else if (type == Kind::Wide) {
                bits = array ? static_cast<uint64_t>(jvalues[i].get_j()) : ctx.args[i + 3];
            } else if (!array) {
                bits = static_cast<uint32_t>(ctx.args[i + 3]);
            } else {
                switch (descriptor.front()) {
                case 'Z': bits = jvalues[i].get_z(); break;
                case 'B': bits = static_cast<uint32_t>(jvalues[i].get_b()); break;
                case 'C': bits = jvalues[i].get_c(); break;
                case 'S': bits = static_cast<uint32_t>(jvalues[i].get_s()); break;
                default: bits = static_cast<uint32_t>(jvalues[i].get_i()); break;
                }
            }
            args.push_back(bits);
        }
        uint64_t result = 0;
        ++stats.java_callbacks;
        const auto status = invoke(user, active->thread, id, args.data(),
                                   static_cast<uint32_t>(args.size()), &result);
        return detail::jni_result(ctx, status, static_cast<uint32_t>(result));
    }

    static int32_t raw(void* user, JniSlot slot, wa_host_call_ctx_t& ctx) {
        auto& self = *static_cast<JniBridge*>(user);
        if (ctx.args[0] != kEnv || !self.active)
            return WA_ERR_INVALID;
        switch (slot) {
        case JniSlot::GetStringUTFChars: return self.get_utf(ctx);
        case JniSlot::ReleaseStringUTFChars: return self.release_utf(ctx);
        case JniSlot::GetStaticMethodID: return self.static_method_id(ctx);
        case JniSlot::CallStaticIntMethod: return self.call_static_int(ctx, false);
        case JniSlot::CallStaticIntMethodA: return self.call_static_int(ctx, true);
        case JniSlot::ExceptionCheck:
            return detail::jni_result(ctx, WA_OK, self.active->state.pending_exception != 0);
        case JniSlot::ExceptionClear:
            self.active->state.pending_exception = 0;
            return detail::jni_result(ctx, WA_OK, 0);
        default: return WA_ERR_NOSYS;
        }
    }

    uint64_t call(uint32_t thread, uint32_t id, std::span<const uint64_t> java) {
        auto* state = heap.thread(thread);
        if (!state)
            return 0;
        if (id >= methods.size() || !methods[id]) {
            wa_art_rt_raise(&heap, thread, WA_ART_UNSATISFIED_LINK);
            return 0;
        }
        Frame frame(*this, *state, thread);
        const auto& method = heap.dex.methods[id];
        const auto& prototype = heap.dex.protos[method.proto];
        const bool statik = method.access & kStatic;
        std::array<uint64_t, 8> general{kEnv, statik ? handle(kClassHandle, method.owner) :
                                                      object_handle(static_cast<uint32_t>(java[0]))};
        std::array<float, 6> floats{};
        uint32_t gprs = 2;
        uint32_t fprs = 0;
        uint32_t arg = statik ? 0 : 1;
        for (auto type : prototype.parameters) {
            const auto descriptor = heap.dex.types[type];
            const auto bits = java[arg++];
            if (descriptor == "F") {
                floats[fprs++] = std::bit_cast<float>(static_cast<uint32_t>(bits));
            } else {
                general[gprs++] = kind(descriptor) == Kind::Reference ?
                                  object_handle(static_cast<uint32_t>(bits)) : bits;
            }
        }
        uint64_t returned = 0;
        ++stats.native_calls;
        const auto status = wa_dbt_call_f32(dbt.get(), methods[id], general.data(), gprs,
                                            floats.data(), fprs, &returned);
        if (status != WA_OK && !state->pending_exception)
            wa_art_rt_raise(&heap, thread, WA_ART_UNSATISFIED_LINK);
        const auto result = kind(heap.dex.types[prototype.result]);
        if (result == Kind::Reference) {
            const auto ref = reference(returned);
            if (ref == kNone) {
                if (!state->pending_exception)
                    wa_art_rt_raise(&heap, thread, WA_ART_CLASS_CAST);
                return 0;
            }
            return ref;
        }
        return returned;
    }
};

void JniDeleter::operator()(JniBridge* bridge) const {
    delete bridge;
}

} // namespace wa::art

int32_t wa_art_jni_load(wa_art_heap_t* heap, const uint8_t* bytes, size_t size,
                        const char* registration_symbol, wa_art_invoke_java_fn invoke,
                        void* user) {
    if (!heap || !bytes || !size || !registration_symbol || !*registration_symbol || !invoke)
        return WA_ERR_INVALID;
    if (heap->jni)
        return WA_ERR_EXIST;
    try {
        std::unique_ptr<wa::art::JniBridge, wa::art::JniDeleter> bridge(
            new wa::art::JniBridge(*heap, {bytes, size}, invoke, user));
        const auto status = bridge->load(registration_symbol);
        if (status != WA_OK)
            return status;
        heap->jni = std::move(bridge);
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_INTERNAL;
    }
}

void wa_art_jni_get_stats(const wa_art_heap_t* heap, wa_art_jni_stats_t* out) {
    if (!heap || !out)
        return;
    *out = {};
    if (!heap->jni)
        return;
    *out = heap->jni->stats;
    wa_dbt_stats_t guest{};
    wa_dbt_get_stats(heap->jni->dbt.get(), &guest);
    out->guest_instructions = guest.insns_executed;
    out->guest_jni_calls = guest.host_calls;
    out->guest_reentries = guest.reverse_callbacks;
}

uint64_t wa_art_rt_native(wa_art_heap_t* heap, uint32_t thread, uint32_t method,
                          uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (!heap->jni) {
        wa_art_rt_raise(heap, thread, WA_ART_UNSATISFIED_LINK);
        return 0;
    }
    const std::array args{arg0, arg1, arg2, arg3, arg4, arg5};
    return heap->jni->call(thread, method, args);
}
