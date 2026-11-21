#include "dalvik_ir.h"
#include "runtime_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_set>

namespace wa::art {
namespace {

enum Wasm : uint8_t {
    Unreachable = 0x00, BlockOp = 0x02, Loop = 0x03, If = 0x04, Else = 0x05, Throw = 0x08,
    End = 0x0b, Br = 0x0c, BrTable = 0x0e, Return = 0x0f, Call = 0x10, CallIndirect = 0x11,
    Drop = 0x1a, TryTable = 0x1f, LocalGet = 0x20, LocalSet = 0x21, LocalTee = 0x22,
    TableGet = 0x25, I32Load = 0x28, I32Store = 0x36, I32Const = 0x41, I64Const = 0x42,
    I32Eqz = 0x45, I32Eq = 0x46, I32Ne = 0x47, I32LtS = 0x48, I32LtU = 0x49,
    I32GtS = 0x4a, I32GtU = 0x4b, I32LeS = 0x4c, I32GeS = 0x4e, I64Eqz = 0x50,
    I64Eq = 0x51, I32Add = 0x6a, I32Sub = 0x6b, I32Or = 0x72, I32Shl = 0x74,
    I64Sub = 0x7d, I32WrapI64 = 0xa7, I64ExtendI32S = 0xac, I64ExtendI32U = 0xad,
    I32ReinterpretF32 = 0xbc, F32ReinterpretI32 = 0xbe, RefIsNull = 0xd1,
};

constexpr uint8_t kVoid = 0x40;
constexpr uint8_t kI32 = 0x7f;
constexpr uint8_t kI64 = 0x7e;
constexpr uint32_t kTop = offsetof(wa_art_thread_t, ref_top);
constexpr uint32_t kStack = offsetof(wa_art_thread_t, ref_stack);
constexpr uint32_t kSuspend = offsetof(wa_art_thread_t, suspend_flag);
constexpr uint32_t kPending = offsetof(wa_art_thread_t, pending_exception);
constexpr uint32_t kCapacity = offsetof(wa_art_thread_t, ref_capacity);

enum Runtime : uint32_t {
    NewObject, NewArray, ConstString, GetField, PutField, GetStatic, PutStatic, ArrayLength,
    ArrayGet, ArrayPut, IsInstance, VirtualMethod, Raise, Safepoint, Native, RuntimeCount,
};

struct RuntimeImport {
    std::string_view name;
    std::initializer_list<uint8_t> args;
    uint8_t result;
};

constexpr RuntimeImport kRuntime[] = {
    {"rt_new_object", {kI32, kI32}, kI32},
    {"rt_new_array", {kI32, kI32, kI32}, kI32},
    {"rt_const_string", {kI32, kI32}, kI32},
    {"rt_get_field", {kI32, kI32, kI32}, kI64},
    {"rt_put_field", {kI32, kI32, kI32, kI64}, kVoid},
    {"rt_get_static", {kI32, kI32}, kI64},
    {"rt_put_static", {kI32, kI32, kI64}, kVoid},
    {"rt_array_length", {kI32, kI32}, kI32},
    {"rt_array_get", {kI32, kI32, kI32}, kI64},
    {"rt_array_put", {kI32, kI32, kI32, kI64}, kVoid},
    {"rt_is_instance", {kI32, kI32, kI32}, kI32},
    {"rt_virtual_method", {kI32, kI32, kI32}, kI32},
    {"rt_raise", {kI32, kI32}, kI32},
    {"rt_safepoint", {kI32}, kVoid},
    {"rt_native", {kI32, kI32, kI64, kI64, kI64, kI64, kI64, kI64}, kI64},
};
static_assert(std::size(kRuntime) == RuntimeCount);

struct Bytes {
    std::vector<uint8_t> data;

    void byte(uint8_t x) { data.push_back(x); }
    void append(std::span<const uint8_t> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void uleb(uint32_t value) {
        while (value >= 0x80) {
            byte(static_cast<uint8_t>(value | 0x80));
            value >>= 7;
        }
        byte(static_cast<uint8_t>(value));
    }
    void sleb(int64_t value) {
        while (true) {
            const auto chunk = static_cast<uint8_t>(value & 0x7f);
            value >>= 7;
            if ((value == 0 && !(chunk & 0x40)) || (value == -1 && (chunk & 0x40))) {
                byte(chunk);
                return;
            }
            byte(chunk | 0x80);
        }
    }
    void length(size_t length) {
        if (length > UINT32_MAX)
            throw std::runtime_error("Wasm section exceeds 32-bit length");
        uleb(static_cast<uint32_t>(length));
    }
    void string(std::string_view value) {
        length(value.size());
        append({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
    }
    void vec(std::span<const uint8_t> value) {
        length(value.size());
        append(value);
    }
    void section(uint8_t id, const Bytes& payload) {
        byte(id);
        vec(payload.data);
    }
};

struct Signature {
    std::vector<uint8_t> args;
    uint8_t result;
    auto operator<=>(const Signature&) const = default;
};

uint8_t wasm_type(Kind kind) {
    return kind == Kind::Unset ? kVoid : kind == Kind::Wide ? kI64 : kI32;
}

struct TypePool {
    std::vector<Signature> sigs;
    std::map<Signature, uint32_t> indices;

    uint32_t add(Signature sig) {
        const auto [found, inserted] = indices.emplace(sig, static_cast<uint32_t>(sigs.size()));
        if (inserted)
            sigs.push_back(std::move(sig));
        return found->second;
    }

    uint32_t method(const wa_art_dex_t& dex, const Method& method, bool statik) {
        Signature signature{{kI32}, wasm_type(kind(dex.types[dex.protos[method.proto].result]))};
        for (auto param : parameters(dex, method, statik))
            signature.args.push_back(wasm_type(param));
        return add(std::move(signature));
    }

    Bytes encode() const {
        Bytes result;
        result.length(sigs.size());
        for (const auto& signature : sigs) {
            result.byte(0x60);
            result.vec(signature.args);
            if (signature.result == kVoid) {
                result.byte(0);
            } else {
                result.byte(1);
                result.byte(signature.result);
            }
        }
        return result;
    }
};

struct WasmCode {
    Bytes bytes;
    std::vector<uint32_t> labels;
    uint32_t next_label = 0;

    void op(Wasm value) { bytes.byte(value); }
    void with_index(Wasm value, uint32_t index) {
        op(value);
        bytes.uleb(index);
    }

    uint32_t open(Wasm value, uint8_t result = kVoid) {
        op(value);
        bytes.byte(result);
        labels.push_back(next_label++);
        return labels.back();
    }

    uint32_t depth(uint32_t label) const {
        const auto found = std::find(labels.rbegin(), labels.rend(), label);
        if (found == labels.rend())
            throw std::logic_error("Invalid Wasm control label");
        return static_cast<uint32_t>(found - labels.rbegin());
    }

    void close(uint32_t label) {
        if (labels.empty() || labels.back() != label)
            throw std::logic_error("Unbalanced Wasm control structure");
        labels.pop_back();
        op(End);
    }

    uint32_t catch_java(uint32_t label) {
        const auto target = depth(label);
        const auto current = open(TryTable);
        bytes.byte(1); // One catch clause.
        bytes.byte(0); // catch (typed, without an exnref).
        bytes.byte(0); // The imported Java exception tag.
        bytes.uleb(target);
        return current;
    }

    void branch(uint32_t label) { with_index(Br, depth(label)); }
    void get(uint32_t local) { with_index(LocalGet, local); }
    void set(uint32_t local) { with_index(LocalSet, local); }
    void tee(uint32_t local) { with_index(LocalTee, local); }
    void call(uint32_t function) { with_index(Call, function); }
    void constant(uint32_t value) {
        op(I32Const);
        bytes.sleb(std::bit_cast<int32_t>(value));
    }
    void wide(uint64_t value) {
        op(I64Const);
        bytes.sleb(std::bit_cast<int64_t>(value));
    }
    void memory(Wasm value, uint32_t offset) {
        op(value);
        bytes.byte(2); // log2 of four-byte alignment
        bytes.uleb(offset);
    }
    void thread_load(uint32_t offset) {
        get(0);
        memory(I32Load, offset);
    }
    void throw_java() { with_index(Throw, 0); }
};

struct Locals {
    uint32_t regs;
    uint32_t base;
    uint32_t root_base;
    uint32_t pc;
    uint32_t try_index;
    uint32_t caught;
    uint32_t scratch;
    uint32_t invoke_result;
    uint32_t divisor;

    Locals(uint32_t params, uint32_t registers)
        : regs(params), base(params + registers), root_base(base + 1), pc(base + 2),
          try_index(base + 3), caught(base + 4), scratch(base + 5), invoke_result(base + 6),
          divisor(base + 7) {}

    void encode(Bytes& bytes) const {
        bytes.byte(3); // register locals, i32 scratch, i64 scratch
        bytes.uleb(base - regs);
        bytes.byte(kI64);
        bytes.byte(6);
        bytes.byte(kI32);
        bytes.byte(2);
        bytes.byte(kI64);
    }
};

class CodeGeneratorWasm {
public:
    CodeGeneratorWasm(const wa_art_dex_t& dex, TypePool& types,
                      const std::vector<uint32_t>& functions,
                      const std::vector<uint32_t>& parents, uint32_t id)
        : dex(dex), types(types), functions(functions), id(id), method(dex.methods[id]),
          params(parameters(dex, method, method.access & kStatic)),
          locals(static_cast<uint32_t>(params.size()) + 1, method.code.registers), parents(parents) {}

    Bytes generate() {
        locals.encode(wasm.bytes);
        if (method.access & kNative) {
            native();
        } else {
            ir = analyze_method(dex, method);
            uint32_t reg = method.code.registers - method.code.ins;
            for (uint32_t i = 0; i < params.size(); ++i) {
                wasm.get(i + 1);
                extend(params[i]);
                wasm.set(locals.regs + reg);
                reg += params[i] == Kind::Wide ? 2 : 1;
            }
            prepare_roots(ir.max_roots);
            function_body();
        }
        wasm.op(End);
        return std::move(wasm.bytes);
    }

private:
    const wa_art_dex_t& dex;
    TypePool& types;
    const std::vector<uint32_t>& functions;
    uint32_t id;
    const Method& method;
    const std::vector<Kind> params;
    const Locals locals;
    const std::vector<uint32_t>& parents;
    WasmCode wasm;
    MethodIR ir;
    uint32_t dispatch = 0;

    void extend(Kind kind) {
        if (kind != Kind::Wide)
            wasm.op(I64ExtendI32U);
    }

    void read(uint16_t reg, Kind kind = Kind::Scalar) {
        wasm.get(locals.regs + reg);
        if (kind != Kind::Wide)
            wasm.op(I32WrapI64);
    }

    void write32(uint16_t reg) {
        wasm.op(I64ExtendI32U);
        wasm.set(locals.regs + reg);
    }

    void raise(uint32_t kind) {
        wasm.get(0);
        wasm.constant(kind);
        wasm.call(Raise);
        wasm.throw_java();
    }

    void check_pending() {
        wasm.thread_load(kPending);
        wasm.tee(locals.scratch);
        const auto block = wasm.open(If);
        wasm.get(locals.scratch);
        wasm.throw_java();
        wasm.close(block);
    }

    void check_null(uint16_t ref) {
        read(ref);
        wasm.op(I32Eqz);
        const auto block = wasm.open(If);
        raise(WA_ART_NULL_POINTER);
        wasm.close(block);
    }

    void prepare_roots(uint32_t count) {
        if (!count)
            return;
        wasm.thread_load(kTop);
        wasm.set(locals.base);
        wasm.get(locals.base);
        wasm.thread_load(kCapacity);
        wasm.op(I32GtU);
        wasm.thread_load(kCapacity);
        wasm.get(locals.base);
        wasm.op(I32Sub);
        wasm.constant(count);
        wasm.op(I32LtU);
        wasm.op(I32Or);
        const auto check = wasm.open(If);
        raise(WA_ART_STACK_OVERFLOW);
        wasm.close(check);
        wasm.thread_load(kStack);
        wasm.get(locals.base);
        wasm.constant(2);
        wasm.op(I32Shl);
        wasm.op(I32Add);
        wasm.set(locals.root_base);
    }

    void publish_roots(uint32_t count) {
        wasm.get(0);
        wasm.get(locals.base);
        wasm.constant(count);
        wasm.op(I32Add);
        wasm.memory(I32Store, kTop);
    }

    void restore_roots() {
        wasm.get(0);
        wasm.get(locals.base);
        wasm.memory(I32Store, kTop);
    }

    void spill(std::span<const uint16_t> roots) {
        if (roots.empty())
            return;
        uint32_t slot = 0;
        for (auto reg : roots) {
            wasm.get(locals.root_base);
            read(reg, Kind::Reference);
            wasm.memory(I32Store, slot * 4);
            ++slot;
        }
        publish_roots(slot);
    }

    void native() {
        if (params.size() > 6)
            throw std::runtime_error("ARM64 JNI stub exceeds six Java arguments");
        const auto count = static_cast<uint32_t>(std::count(params.begin(), params.end(),
                                                           Kind::Reference));
        prepare_roots(count);
        if (count) {
            uint32_t slot = 0;
            for (uint32_t i = 0; i < params.size(); ++i) {
                if (params[i] == Kind::Reference) {
                    wasm.get(locals.root_base);
                    wasm.get(i + 1);
                    wasm.memory(I32Store, slot++ * 4);
                }
            }
            publish_roots(count);
        }
        wasm.get(0);
        wasm.constant(id);
        for (uint32_t i = 0; i < 6; ++i) {
            if (i < params.size()) {
                wasm.get(i + 1);
                extend(params[i]);
            } else {
                wasm.wide(0);
            }
        }
        wasm.call(Native);
        if (count)
            restore_roots();
        check_pending();
        const auto result = kind(dex.types[dex.protos[method.proto].result]);
        if (result == Kind::Unset)
            wasm.op(Drop);
        else if (result != Kind::Wide)
            wasm.op(I32WrapI64);
    }

    uint32_t block_at(uint32_t dex_pc) const {
        return ir.block_by_instruction[ir.pc_to_index[dex_pc]];
    }

    void jump(uint32_t block) {
        if (block == kNone)
            throw std::logic_error("Wasm jump to unreachable Dalvik block");
        if (ir.blocks.size() > 1) {
            wasm.constant(block);
            wasm.set(locals.pc);
        }
        wasm.branch(dispatch);
    }

    void branch_to(const Instruction& ins) {
        if (ins.target <= ins.pc) {
            wasm.thread_load(kSuspend);
            const auto check = wasm.open(If);
            const auto& roots = ir.instructions[ir.pc_to_index[ins.target]].roots;
            spill(roots);
            wasm.get(0);
            wasm.call(Safepoint);
            if (!roots.empty())
                restore_roots();
            wasm.close(check);
        }
        jump(block_at(ins.target));
    }

    void handle(uint32_t target) {
        wasm.get(0);
        wasm.constant(0);
        wasm.memory(I32Store, kPending);
        jump(block_at(target));
    }

    void catch_handlers(std::span<const bool> active) {
        wasm.set(locals.caught);
        if (ir.max_roots)
            restore_roots();
        // Keep an uncaught payload rooted until its caller handles it.
        wasm.get(0);
        wasm.get(locals.caught);
        wasm.memory(I32Store, kPending);
        for (uint32_t i = 0; i < active.size(); ++i) {
            if (!active[i])
                continue;
            wasm.get(locals.try_index);
            wasm.constant(i);
            wasm.op(I32Eq);
            const auto range = wasm.open(If);
            for (const auto& handler : method.code.tries[i].handlers) {
                if (handler.type == kNone) {
                    handle(handler.target);
                } else {
                    wasm.get(0);
                    wasm.get(locals.caught);
                    wasm.constant(handler.type);
                    wasm.call(IsInstance);
                    const auto match = wasm.open(If);
                    handle(handler.target);
                    wasm.close(match);
                }
            }
            wasm.close(range);
        }
        wasm.get(locals.caught);
        wasm.throw_java();
    }

    void emit_block(const Block& block, bool has_handlers) {
        for (auto i = block.begin; i < block.end; ++i) {
            const auto& ins = ir.instructions[i];
            if (ins.may_throw) {
                if (has_handlers) {
                    wasm.constant(ins.try_index);
                    wasm.set(locals.try_index);
                }
                spill(ins.roots);
            }
            emit_instruction(ins);
            if (ins.op != Op::Throw && ins.may_throw && !ins.roots.empty())
                restore_roots();
        }
        if (!ir.instructions[block.end - 1].ends_block())
            jump(ir.block_by_instruction[block.end]);
    }

    void function_body() {
        const auto size = method.code.tries.size();
        auto active = std::make_unique<bool[]>(size);
        bool has_handlers = false;
        for (const auto& ins : ir.instructions) {
            if (ins.reachable && ins.try_index != kNone) {
                active[ins.try_index] = true;
                has_handlers = true;
            }
        }
        const bool needs_catch = ir.max_roots || has_handlers;
        dispatch = wasm.open(Loop);
        uint32_t caught = 0;
        uint32_t table = 0;
        if (needs_catch) {
            caught = wasm.open(BlockOp, kI32);
            table = wasm.catch_java(caught);
        }

        // Nested labels form a dense basic-block dispatch table. Straight-line
        // runs stay in one block, including the body of tight loop iterations.
        if (ir.blocks.size() == 1) {
            emit_block(ir.blocks.front(), has_handlers);
        } else {
            const auto invalid = wasm.open(BlockOp);
            std::vector<uint32_t> cases(ir.blocks.size());
            for (size_t i = cases.size(); i > 0; --i)
                cases[i - 1] = wasm.open(BlockOp);
            wasm.get(locals.pc);
            wasm.op(BrTable);
            wasm.bytes.length(cases.size());
            for (auto label : cases)
                wasm.bytes.uleb(wasm.depth(label));
            wasm.bytes.uleb(wasm.depth(invalid));
            for (size_t i = 0; i < cases.size(); ++i) {
                wasm.close(cases[i]);
                emit_block(ir.blocks[i], has_handlers);
            }
            wasm.close(invalid);
            wasm.op(Unreachable);
        }

        if (needs_catch) {
            wasm.close(table);
            wasm.op(Unreachable);
            wasm.close(caught);
            catch_handlers({active.get(), size});
        }
        wasm.close(dispatch);
        wasm.op(Unreachable);
    }

    void arithmetic(const Instruction& ins) {
        const auto op = static_cast<unsigned>(ins.binary);
        const bool wide = ins.op == Op::LongBinary;
        if (ins.op == Op::FloatBinary) {
            read(ins.b);
            wasm.op(F32ReinterpretI32);
            read(ins.c);
            wasm.op(F32ReinterpretI32);
            wasm.bytes.byte(0x92 + op); // f32.add, sub, mul, div
            wasm.op(I32ReinterpretF32);
            write32(ins.a);
            return;
        }
        constexpr uint8_t int_ops[] = {0x6a, 0x6b, 0x6c, 0x6d, 0x6f, 0x71,
                                       0x72, 0x73, 0x74, 0x75, 0x76};
        constexpr uint8_t long_ops[] = {0x7c, 0x7d, 0x7e, 0x7f, 0x81, 0x83,
                                        0x84, 0x85, 0x86, 0x87, 0x88};
        const auto integer_kind = wide ? Kind::Wide : Kind::Scalar;
        const auto lhs = [&] { read(ins.b, integer_kind); };
        const auto rhs = [&] {
            if (ins.literal) {
                wasm.constant(static_cast<uint32_t>(ins.bits));
            } else if (wide && ins.binary >= Binary::Shl) {
                read(ins.c);
                wasm.op(I64ExtendI32U);
            } else {
                read(ins.c, integer_kind);
            }
        };
        const auto binary = [&] { wasm.bytes.byte(wide ? long_ops[op] : int_ops[op]); };
        if (ins.binary == Binary::Div || ins.binary == Binary::Rem) {
            const auto divisor = wide ? locals.divisor : locals.scratch;
            rhs();
            wasm.tee(divisor);
            wasm.op(wide ? I64Eqz : I32Eqz);
            const auto zero = wasm.open(If);
            raise(WA_ART_ARITHMETIC);
            wasm.close(zero);
            if (ins.binary == Binary::Div) {
                // Dalvik wraps MIN_VALUE / -1. Wasm's signed divide traps.
                wasm.get(divisor);
                if (wide)
                    wasm.wide(UINT64_MAX);
                else
                    wasm.constant(UINT32_MAX);
                wasm.op(wide ? I64Eq : I32Eq);
                const auto overflow = wasm.open(If, wide ? kI64 : kI32);
                if (wide)
                    wasm.wide(0);
                else
                    wasm.constant(0);
                lhs();
                wasm.op(wide ? I64Sub : I32Sub);
                wasm.op(Else);
                lhs();
                wasm.get(divisor);
                binary();
                wasm.close(overflow);
            } else {
                lhs();
                wasm.get(divisor);
                binary();
            }
        } else {
            if (ins.reverse) {
                rhs();
                lhs();
            } else {
                lhs();
                rhs();
            }
            binary();
        }
        if (wide)
            wasm.set(locals.regs + ins.a);
        else
            write32(ins.a);
    }

    uint32_t static_target(uint32_t symbolic) const {
        const auto& method = dex.methods[symbolic];
        if (method.declared)
            return symbolic;
        auto owner = parents[method.owner];
        // Link inherited symbolic references to the body's declaring method ID.
        for (size_t depth = 0; owner != kNone && depth < parents.size(); ++depth) {
            const auto id = dex.find_method(owner, method.name, dex.protos[method.proto].signature);
            if (id != kNone && dex.methods[id].declared && (dex.methods[id].access & kStatic))
                return id;
            owner = parents[owner];
        }
        return symbolic;
    }

    void invoke(const Instruction& ins) {
        const bool statik = ins.invoke == Invoke::Static;
        const auto target_id = statik ? static_target(ins.id) : ins.id;
        const auto& target = dex.methods[target_id];
        const auto& proto = dex.protos[target.proto];
        if (!statik)
            check_null(ins.reads.front().reg);
        if (ins.invoke == Invoke::Direct && !target.declared && target.name == "<init>" &&
            proto.signature == "()V" && builtin_constructor(dex.types[target.owner]))
            return;
        const bool indirect = ins.invoke == Invoke::Virtual || functions[target_id] == kNone ||
                              (target.access & kNative);
        if (indirect) {
            if (ins.invoke == Invoke::Virtual) {
                wasm.get(0);
                read(ins.reads.front().reg);
                wasm.constant(target_id);
                wasm.call(VirtualMethod);
                check_pending();
            } else {
                wasm.constant(target_id);
            }
            wasm.tee(locals.scratch);
            wasm.with_index(TableGet, 0);
            wasm.op(RefIsNull);
            const auto missing = wasm.open(If);
            raise((target.access & kNative) ? WA_ART_UNSATISFIED_LINK : WA_ART_NO_SUCH_METHOD);
            wasm.close(missing);
        }
        wasm.get(0);
        for (const auto& use : ins.reads)
            read(use.reg, use.kind);
        if (indirect) {
            wasm.get(locals.scratch);
            wasm.with_index(CallIndirect, types.method(dex, target, statik));
            wasm.bytes.byte(0); // env.dispatch_table
        } else {
            wasm.call(functions[target_id]);
        }
        const auto result = kind(dex.types[proto.result]);
        if (result != Kind::Unset) {
            extend(result);
            wasm.set(locals.invoke_result);
        }
    }

    void emit_instruction(const Instruction& ins) {
        switch (ins.op) {
        case Op::Nop:
            break;
        case Op::Move:
            wasm.get(locals.regs + ins.b);
            wasm.set(locals.regs + ins.a);
            break;
        case Op::MoveResult:
            wasm.get(locals.invoke_result);
            wasm.set(locals.regs + ins.a);
            break;
        case Op::MoveException:
            wasm.get(locals.caught);
            write32(ins.a);
            break;
        case Op::Constant:
            wasm.wide(ins.bits);
            wasm.set(locals.regs + ins.a);
            break;
        case Op::Return:
            if (!ins.reads.empty())
                read(ins.a, ins.reads.front().kind);
            wasm.op(Return);
            break;
        case Op::Goto:
            branch_to(ins);
            break;
        case Op::Branch: {
            constexpr Wasm compare[] = {I32Eq, I32Ne, I32LtS, I32GeS, I32GtS, I32LeS};
            read(ins.b);
            if (ins.literal)
                wasm.constant(0);
            else
                read(ins.c);
            wasm.op(compare[ins.id]);
            const auto taken = wasm.open(If);
            branch_to(ins);
            wasm.close(taken);
            jump(block_at(ins.pc + ins.width));
            break;
        }
        case Op::Throw:
            check_null(ins.a);
            wasm.get(0);
            read(ins.a);
            wasm.memory(I32Store, kPending);
            read(ins.a);
            wasm.throw_java();
            break;
        case Op::NegInt:
            wasm.constant(0);
            read(ins.b);
            wasm.op(I32Sub);
            write32(ins.a);
            break;
        case Op::IntToLong:
            read(ins.b);
            wasm.op(I64ExtendI32S);
            wasm.set(locals.regs + ins.a);
            break;
        case Op::LongToInt:
            read(ins.b);
            write32(ins.a);
            break;
        case Op::FloatToInt:
            read(ins.b);
            wasm.op(F32ReinterpretI32);
            wasm.bytes.byte(0xfc);
            wasm.bytes.byte(0); // i32.trunc_sat_f32_s implements Java's narrowing cast.
            write32(ins.a);
            break;
        case Op::IntBinary:
        case Op::LongBinary:
        case Op::FloatBinary:
            arithmetic(ins);
            break;
        case Op::String:
        case Op::NewObject:
        case Op::NewArray:
            wasm.get(0);
            wasm.constant(ins.id);
            if (ins.op == Op::NewArray)
                read(ins.b);
            wasm.call(ins.op == Op::String ? ConstString :
                      ins.op == Op::NewObject ? NewObject : NewArray);
            check_pending();
            write32(ins.a);
            break;
        case Op::ArrayLength:
            check_null(ins.b);
            wasm.get(0);
            read(ins.b);
            wasm.call(ArrayLength);
            check_pending();
            write32(ins.a);
            break;
        case Op::ArrayGet:
        case Op::ArrayPut: {
            const bool put = ins.op == Op::ArrayPut;
            check_null(ins.b);
            wasm.get(0);
            read(ins.b);
            read(ins.c);
            if (put)
                wasm.get(locals.regs + ins.a);
            wasm.call(put ? ArrayPut : ArrayGet);
            check_pending();
            if (!put)
                wasm.set(locals.regs + ins.a);
            break;
        }
        case Op::GetField:
        case Op::PutField:
        case Op::GetStatic:
        case Op::PutStatic: {
            const bool statik = ins.op == Op::GetStatic || ins.op == Op::PutStatic;
            const bool put = ins.op == Op::PutField || ins.op == Op::PutStatic;
            if (!statik)
                check_null(ins.b);
            wasm.get(0);
            if (!statik)
                read(ins.b);
            wasm.constant(ins.id);
            if (put)
                wasm.get(locals.regs + ins.a);
            wasm.call(statik ? (put ? PutStatic : GetStatic) : (put ? PutField : GetField));
            check_pending();
            if (!put)
                wasm.set(locals.regs + ins.a);
            break;
        }
        case Op::CheckCast: {
            read(ins.a);
            const auto nonnull = wasm.open(If);
            wasm.get(0);
            read(ins.a);
            wasm.constant(ins.id);
            wasm.call(IsInstance);
            wasm.op(I32Eqz);
            const auto mismatch = wasm.open(If);
            raise(WA_ART_CLASS_CAST);
            wasm.close(mismatch);
            wasm.close(nonnull);
            break;
        }
        case Op::InstanceOf:
            wasm.get(0);
            read(ins.b);
            wasm.constant(ins.id);
            wasm.call(IsInstance);
            write32(ins.a);
            break;
        case Op::Invoke:
            invoke(ins);
            break;
        }
    }
};

struct Selection {
    std::vector<uint32_t> ids;
    std::vector<std::string> names;
    std::vector<uint32_t> indices;
};

Selection select(const wa_art_dex_t& dex, const char* profile) {
    Selection selection;
    selection.indices.resize(dex.methods.size(), kNone);
    std::unordered_set<std::string> requested;
    std::unordered_set<std::string> matched;
    if (profile) {
        std::istringstream lines(profile);
        std::string line;
        while (std::getline(lines, line)) {
            const auto begin = line.find_first_not_of(" \t\r");
            if (begin == std::string::npos || line[begin] == '#')
                continue;
            requested.insert(line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1));
        }
        if (requested.empty())
            throw std::runtime_error("Profile contains no method or class descriptors");
    }
    for (uint32_t id = 0; id < dex.methods.size(); ++id) {
        const auto& method = dex.methods[id];
        if (method.code.words.empty() && !(method.access & kNative))
            continue;
        auto name = dex.method_name(id);
        const auto clazz = utf8_name(dex.types[method.owner]);
        if (profile) {
            const bool has_method = requested.contains(name);
            const bool has_class = requested.contains(clazz);
            if (!has_method && !has_class)
                continue;
            if (has_method)
                matched.insert(name);
            if (has_class)
                matched.insert(clazz);
        }
        selection.indices[id] = RuntimeCount + static_cast<uint32_t>(selection.ids.size());
        selection.ids.push_back(id);
        selection.names.push_back(std::move(name));
    }
    for (const auto& entry : requested) {
        if (!matched.contains(entry))
            throw std::runtime_error("No compilable method matches profile entry " + entry);
    }
    if (selection.ids.empty())
        throw std::runtime_error("DEX contains no compilable methods");
    return selection;
}

} // namespace

std::vector<uint8_t> generate_wasm(const wa_art_dex_t& dex, const char* profile) {
    auto selection = select(dex, profile);
    std::vector<uint32_t> parents(dex.types.size(), kNone);
    for (const auto& clazz : dex.classes)
        parents[clazz.type] = clazz.parent;
    TypePool types;
    Bytes imports;
    imports.uleb(RuntimeCount + 3);
    const auto import = [&](std::string_view name, uint8_t kind) {
        imports.string("env");
        imports.string(name);
        imports.byte(kind);
    };
    import("memory", 2);
    imports.byte(0);
    imports.byte(1);
    import("dispatch_table", 1);
    imports.byte(0x70); // funcref
    imports.byte(0);
    imports.length(dex.methods.size());
    import("java_exception", 4);
    imports.byte(0);
    imports.uleb(types.add({{kI32}, kVoid}));
    for (const auto& rt : kRuntime) {
        import(rt.name, 0);
        imports.uleb(types.add({rt.args, rt.result}));
    }

    Bytes functions;
    Bytes exports;
    Bytes bodies;
    Bytes metadata;
    metadata.string("wa.art.methods");
    metadata.uleb(WA_ART_ABI_VERSION);
    metadata.append(dex.signature);
    metadata.length(dex.methods.size());
    metadata.length(selection.ids.size());
    functions.length(selection.ids.size());
    exports.length(selection.ids.size());
    bodies.length(selection.ids.size());
    for (size_t i = 0; i < selection.ids.size(); ++i) {
        const auto id = selection.ids[i];
        const auto& method = dex.methods[id];
        const auto& name = selection.names[i];
        functions.uleb(types.method(dex, method, method.access & kStatic));
        exports.string(name);
        exports.byte(0);
        exports.uleb(selection.indices[id]);
        metadata.uleb(id);
        metadata.string(name);
        try {
            CodeGeneratorWasm compiler(dex, types, selection.indices, parents, id);
            bodies.vec(compiler.generate().data);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(name + ": " + error.what());
        }
    }

    Bytes module;
    constexpr uint8_t header[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    module.append(header);
    module.section(1, types.encode());
    module.section(2, imports);
    module.section(3, functions);
    module.section(7, exports);
    module.section(10, bodies);
    module.section(0, metadata);
    return std::move(module.data);
}

} // namespace wa::art

int32_t wa_art_compile(const wa_art_dex_t* dex, const char* profile, uint8_t** output,
                       size_t* output_size, char* error, size_t error_size) {
    if (!output || !output_size)
        return WA_ERR_INVALID;
    *output = nullptr;
    *output_size = 0;
    try {
        if (!dex)
            throw std::invalid_argument("Missing parsed DEX");
        const auto wasm = wa::art::generate_wasm(*dex, profile);
        auto* buffer = static_cast<uint8_t*>(std::malloc(wasm.size()));
        if (!buffer)
            throw std::bad_alloc();
        std::memcpy(buffer, wasm.data(), wasm.size());
        *output = buffer;
        *output_size = wasm.size();
        wa::art::copy_error(error, error_size, "");
        return WA_OK;
    } catch (const std::bad_alloc&) {
        wa::art::copy_error(error, error_size, "Not enough memory to compile DEX");
        return WA_ERR_NOMEM;
    } catch (const std::exception& exception) {
        wa::art::copy_error(error, error_size, exception.what());
        return WA_ERR_INVALID;
    }
}

void wa_art_free_buffer(void* buffer) {
    std::free(buffer);
}
