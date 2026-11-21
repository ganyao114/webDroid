#include "dalvik_ir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <sstream>

namespace wa::art {
namespace {

[[noreturn]] void reject(uint32_t pc, std::string_view message) {
    std::ostringstream error;
    error << "Dalvik pc 0x" << std::hex << pc << ": " << message;
    throw std::runtime_error(error.str());
}

Kind field_variant(unsigned variant) {
    if (variant == 1)
        return Kind::Wide;
    if (variant == 2)
        return Kind::Reference;
    return Kind::Scalar;
}

Kind move_variant(unsigned variant) {
    return variant == 0 ? Kind::Scalar : field_variant(variant);
}

bool is_static(Invoke kind) {
    return kind == Invoke::Static;
}

struct Decoder {
    const wa_art_dex_t& dex;
    const Method& method;
    uint32_t pc;

    uint16_t word(uint32_t part) const {
        if (part >= method.code.words.size() - pc)
            reject(pc, "truncated instruction");
        return method.code.words[pc + part];
    }

    uint32_t dword(uint32_t part) const {
        return word(part) | (uint32_t(word(part + 1)) << 16);
    }

    uint32_t branch(int32_t offset) const {
        const int64_t target = int64_t(pc) + offset;
        if (target < 0 || static_cast<uint64_t>(target) >= method.code.words.size())
            reject(pc, "branch target outside code item");
        return static_cast<uint32_t>(target);
    }

    void reg(uint16_t value, Kind kind) const {
        if (uint32_t(value) + (kind == Kind::Wide) >= method.code.registers)
            reject(pc, "register outside code item");
    }

    void result(Instruction& ins, Kind kind) const {
        reg(ins.a, kind);
        ins.output = kind;
    }

    void read(Instruction& ins, uint16_t r, Kind kind) const {
        reg(r, kind);
        ins.reads.push_back({r, kind});
    }

    void check_id(uint32_t id, size_t size, std::string_view message) const {
        if (id >= size)
            reject(pc, message);
    }

    void type(Instruction& ins, char expected) const {
        ins.id = word(1);
        check_id(ins.id, dex.types.size(), "invalid type index");
        if (expected && dex.types[ins.id].front() != expected)
            reject(pc, "unexpected type descriptor");
    }

    void field(Instruction& ins, unsigned variant, bool statik) const {
        ins.id = word(1);
        check_id(ins.id, dex.fields.size(), "invalid field index");
        const auto& field = dex.fields[ins.id];
        const auto desc = dex.types[field.type];
        const auto expected = field_variant(variant);
        constexpr std::array<char, 7> kinds = {0, 0, 0, 'Z', 'B', 'C', 'S'};
        if (kind(desc) != expected || (kinds[variant] && desc.front() != kinds[variant]) ||
            (variant == 0 && desc != "I" && desc != "F") ||
            (field.declared && (((field.access & kStatic) != 0) != statik)))
            reject(pc, "field opcode does not match the field declaration");
    }

    void invoke(Instruction& ins, uint8_t opcode) const {
        const bool range = opcode >= 0x74;
        const auto base = range ? opcode - 0x74 : opcode - 0x6e;
        ins.op = Op::Invoke;
        ins.invoke = base == 3 ? Invoke::Static : base == 2 ? Invoke::Direct : Invoke::Virtual;
        ins.may_throw = true;
        ins.width = 3;
        ins.id = word(1);
        check_id(ins.id, dex.methods.size(), "invalid invoke method index");
        const auto& callee = dex.methods[ins.id];
        if (callee.declared && is_static(ins.invoke) != ((callee.access & kStatic) != 0))
            reject(pc, "invoke opcode does not match the method declaration");

        const unsigned count = range ? word(0) >> 8 : word(0) >> 12;
        const auto list = word(2);
        if (count > method.code.outs || (!range && count > 5) ||
            (range && uint32_t(list) + count > method.code.registers))
            reject(pc, "invalid invoke register count");
        const std::array<uint16_t, 5> explicit_regs = {
            uint16_t(list & 15), uint16_t(list >> 4 & 15), uint16_t(list >> 8 & 15),
            uint16_t(list >> 12), uint16_t(word(0) >> 8 & 15),
        };
        const auto at = [&](unsigned i) -> uint16_t { return range ? list + i : explicit_regs[i]; };
        unsigned i = 0;
        for (auto kind : parameters(dex, callee, is_static(ins.invoke))) {
            if (i >= count)
                reject(pc, "too few invoke argument words");
            const auto low = at(i++);
            read(ins, low, kind);
            if (kind == Kind::Wide && (i >= count || at(i++) != uint32_t(low) + 1))
                reject(pc, "invoke wide argument is not a register pair");
        }
        if (i != count)
            reject(pc, "too many invoke argument words");
    }

    void binary(Instruction& ins, uint8_t opcode, uint16_t nibble_a, uint16_t nibble_b) const {
        // Register encodings are 23x and 12x. Literal forms are 22s and 22b.
        const bool two_addr = opcode >= 0xb0 && opcode <= 0xc9;
        ins.literal = opcode >= 0xd0;
        ins.width = two_addr ? 1 : 2;
        auto group = opcode;
        if (two_addr) {
            ins.a = ins.b = nibble_a;
            ins.c = nibble_b;
            group -= 0x20;
        } else if (opcode >= 0xd0 && opcode <= 0xd7) {
            ins.a = nibble_a;
            ins.b = nibble_b;
            ins.bits = static_cast<uint32_t>(static_cast<int16_t>(word(1)));
            group = (opcode - 0xd0) + 0x90;
        } else {
            ins.b = word(1) & 255;
            ins.c = word(1) >> 8;
            if (ins.literal) {
                ins.bits = static_cast<uint32_t>(static_cast<int8_t>(ins.c));
                group = (opcode - 0xd8) + 0x90;
            }
        }
        ins.reverse = ins.literal && group == 0x91;
        if (group < 0x9b) {
            ins.op = Op::IntBinary;
            ins.binary = static_cast<Binary>(group - 0x90);
        } else if (group < 0xa6) {
            ins.op = Op::LongBinary;
            ins.binary = static_cast<Binary>(group - 0x9b);
        } else {
            ins.op = Op::FloatBinary;
            ins.binary = static_cast<Binary>(group - 0xa6);
        }
        const auto kind = ins.op == Op::LongBinary ? Kind::Wide : Kind::Scalar;
        result(ins, kind);
        read(ins, ins.b, kind);
        if (!ins.literal)
            read(ins, ins.c, ins.binary >= Binary::Shl ? Kind::Scalar : kind);
        ins.may_throw = ins.op != Op::FloatBinary &&
                        (ins.binary == Binary::Div || ins.binary == Binary::Rem);
    }

    Instruction decode() const {
        const auto first = word(0);
        const auto opcode = static_cast<uint8_t>(first);
        const auto nibble_a = uint16_t(first >> 8 & 15);
        const auto nibble_b = uint16_t(first >> 12);
        Instruction ins;
        ins.pc = pc;
        ins.a = first >> 8;
        if (opcode >= 0x01 && opcode <= 0x09) {
            const unsigned form = (opcode - 1) % 3;
            const auto kind = move_variant((opcode - 1) / 3);
            ins.op = Op::Move;
            ins.width = form + 1;
            if (form == 0) {
                ins.a = nibble_a;
                ins.b = nibble_b;
            } else if (form == 1) {
                ins.b = word(1);
            } else {
                ins.a = word(1);
                ins.b = word(2);
            }
            result(ins, kind);
            read(ins, ins.b, kind);
        } else if (opcode >= 0x0a && opcode <= 0x0c) {
            ins.op = Op::MoveResult;
            result(ins, move_variant(opcode - 0x0a));
        } else if (opcode >= 0x0e && opcode <= 0x11) {
            ins.op = Op::Return;
            const auto actual = opcode == 0x0e ? Kind::Unset : move_variant(opcode - 0x0f);
            if (actual != kind(dex.types[dex.protos[method.proto].result]))
                reject(pc, "return opcode does not match the method prototype");
            if (opcode != 0x0e)
                read(ins, ins.a, actual);
        } else if (opcode >= 0x12 && opcode <= 0x19) {
            ins.op = Op::Constant;
            const auto value16 = [&] { return static_cast<int64_t>(static_cast<int16_t>(word(1))); };
            const auto value32 = [&] { return static_cast<int64_t>(std::bit_cast<int32_t>(dword(1))); };
            switch (opcode) {
            case 0x12:
                ins.a = nibble_a;
                ins.bits = nibble_b < 8 ? nibble_b : int(nibble_b) - 16;
                break;
            case 0x13:
            case 0x16: ins.width = 2; ins.bits = value16(); break;
            case 0x14:
            case 0x17: ins.width = 3; ins.bits = value32(); break;
            case 0x15: ins.width = 2; ins.bits = uint32_t(word(1)) << 16; break;
            case 0x18: ins.width = 5; ins.bits = dword(1) | (uint64_t(dword(3)) << 32); break;
            case 0x19: ins.width = 2; ins.bits = uint64_t(word(1)) << 48; break;
            }
            result(ins, opcode >= 0x16 ? Kind::Wide : ins.bits ? Kind::Scalar : Kind::Zero);
        } else if (opcode >= 0x28 && opcode <= 0x2a) {
            ins.op = Op::Goto;
            ins.width = opcode - 0x27;
            const auto delta = opcode == 0x28 ? static_cast<int8_t>(ins.a) :
                               opcode == 0x29 ? static_cast<int16_t>(word(1)) :
                                                std::bit_cast<int32_t>(dword(1));
            ins.target = branch(delta);
        } else if (opcode >= 0x32 && opcode <= 0x3d) {
            ins.op = Op::Branch;
            ins.width = 2;
            ins.literal = opcode >= 0x38; // Compare one register against zero.
            ins.id = (opcode - 0x32) % 6;
            ins.b = ins.literal ? ins.a : nibble_a;
            ins.c = nibble_b;
            const auto comparable = ins.id < 2 ? Kind::Unset : Kind::Scalar;
            read(ins, ins.b, comparable);
            if (!ins.literal)
                read(ins, ins.c, comparable);
            ins.target = branch(static_cast<int16_t>(word(1)));
        } else if (opcode >= 0x44 && opcode <= 0x51) {
            const bool put = opcode >= 0x4b;
            const auto kind = field_variant((opcode - 0x44) % 7);
            ins.op = put ? Op::ArrayPut : Op::ArrayGet;
            ins.width = 2;
            ins.b = word(1) & 255;
            ins.c = word(1) >> 8;
            ins.may_throw = true;
            read(ins, ins.b, Kind::Reference);
            read(ins, ins.c, Kind::Scalar);
            if (put)
                read(ins, ins.a, kind);
            else
                result(ins, kind);
        } else if (opcode >= 0x52 && opcode <= 0x6d) {
            const unsigned group = (opcode - 0x52) / 7;
            const unsigned variant = (opcode - 0x52) % 7;
            const auto kind = field_variant(variant);
            const bool statik = group >= 2;
            const bool put = group % 2;
            if (statik) {
                ins.op = put ? Op::PutStatic : Op::GetStatic;
            } else {
                ins.op = put ? Op::PutField : Op::GetField;
                ins.a = nibble_a;
                ins.b = nibble_b;
                read(ins, ins.b, Kind::Reference);
            }
            ins.width = 2;
            ins.may_throw = true;
            field(ins, variant, statik);
            if (put)
                read(ins, ins.a, kind);
            else
                result(ins, kind);
        } else if (opcode == 0x6e || opcode == 0x70 || opcode == 0x71 ||
                   opcode == 0x74 || opcode == 0x76 || opcode == 0x77) {
            invoke(ins, opcode);
        } else if ((opcode >= 0x90 && opcode <= 0xa9) || (opcode >= 0xb0 && opcode <= 0xc9) ||
                   (opcode >= 0xd0 && opcode <= 0xe2)) {
            binary(ins, opcode, nibble_a, nibble_b);
        } else {
            switch (opcode) {
            case 0x00:
                if (first != 0)
                    reject(pc, "unsupported instruction payload");
                break;
            case 0x0d:
                ins.op = Op::MoveException;
                result(ins, Kind::Reference);
                break;
            case 0x1a:
            case 0x1b:
                ins.op = Op::String;
                ins.width = opcode == 0x1a ? 2 : 3;
                ins.id = opcode == 0x1a ? word(1) : dword(1);
                check_id(ins.id, dex.strings.size(), "invalid const-string index");
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case 0x1f:
                ins.op = Op::CheckCast;
                ins.width = 2;
                type(ins, 0);
                if (kind(dex.types[ins.id]) != Kind::Reference)
                    reject(pc, "check-cast expects a reference descriptor");
                read(ins, ins.a, Kind::Reference);
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case 0x20:
                ins.op = Op::InstanceOf;
                ins.width = 2;
                ins.a = nibble_a;
                ins.b = nibble_b;
                type(ins, 0);
                if (kind(dex.types[ins.id]) != Kind::Reference)
                    reject(pc, "instance-of expects a reference descriptor");
                read(ins, ins.b, Kind::Reference);
                result(ins, Kind::Scalar);
                break;
            case 0x21:
                ins.op = Op::ArrayLength;
                ins.a = nibble_a;
                ins.b = nibble_b;
                result(ins, Kind::Scalar);
                read(ins, ins.b, Kind::Reference);
                ins.may_throw = true;
                break;
            case 0x22:
                ins.op = Op::NewObject;
                ins.width = 2;
                type(ins, 'L');
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case 0x23:
                ins.op = Op::NewArray;
                ins.width = 2;
                ins.a = nibble_a;
                ins.b = nibble_b;
                type(ins, '[');
                result(ins, Kind::Reference);
                read(ins, ins.b, Kind::Scalar);
                ins.may_throw = true;
                break;
            case 0x27:
                ins.op = Op::Throw;
                read(ins, ins.a, Kind::Reference);
                ins.may_throw = true;
                break;
            case 0x7b:
            case 0x81:
            case 0x84:
            case 0x87:
                ins.op = opcode == 0x7b ? Op::NegInt : opcode == 0x81 ? Op::IntToLong :
                         opcode == 0x84 ? Op::LongToInt : Op::FloatToInt;
                ins.a = nibble_a;
                ins.b = nibble_b;
                result(ins, opcode == 0x81 ? Kind::Wide : Kind::Scalar);
                read(ins, ins.b, opcode == 0x84 ? Kind::Wide : Kind::Scalar);
                break;
            default: {
                std::ostringstream message;
                message << "unsupported opcode 0x" << std::hex << unsigned(opcode);
                reject(pc, message.str());
            }
            }
        }
        return ins;
    }
};

Kind merge_kind(Kind a, Kind b) {
    if (a == b)
        return a;
    if (a == Kind::Zero && (b == Kind::Scalar || b == Kind::Reference))
        return b;
    if (b == Kind::Zero && (a == Kind::Scalar || a == Kind::Reference))
        return a;
    return Kind::Conflict;
}

bool matches(Kind actual, Kind expected) {
    if (expected == Kind::Unset)
        return actual == Kind::Zero || actual == Kind::Scalar || actual == Kind::Reference;
    return actual == expected ||
           (actual == Kind::Zero && (expected == Kind::Scalar || expected == Kind::Reference));
}

void clobber(std::span<Kind> state, uint16_t reg) {
    if (state[reg] == Kind::Wide && size_t(reg) + 1 < state.size())
        state[reg + 1] = Kind::Unset;
    if (state[reg] == Kind::WideHigh && reg)
        state[reg - 1] = Kind::Unset;
}

using Successors = std::vector<uint32_t>;

struct Flow {
    std::array<Successors, 2> edges; // Normal, exceptional.
    std::vector<uint32_t> predecessors;
};

bool terminal(const Instruction& ins) {
    return ins.op == Op::Return || ins.op == Op::Throw || ins.op == Op::Goto;
}

void build_edges(MethodIR& ir, const Method& method, const wa_art_dex_t& dex,
                 std::vector<Flow>& flow) {
    const auto& code = method.code;
    const auto instruction_at = [&](uint32_t pc) {
        if (pc >= ir.pc_to_index.size() || ir.pc_to_index[pc] == kNone)
            reject(pc, "target is not an instruction boundary");
        return ir.pc_to_index[pc];
    };
    std::vector<bool> handler_entry(ir.instructions.size());
    for (const auto& item : code.tries) {
        instruction_at(item.start);
        if (item.end != code.words.size())
            instruction_at(item.end);
        for (const auto& handler : item.handlers) {
            if (handler.type != kNone && dex.types[handler.type].front() != 'L')
                reject(handler.target, "non-class catch type");
            handler_entry[instruction_at(handler.target)] = true;
        }
    }
    uint32_t try_index = 0;
    for (uint32_t i = 0; i < ir.instructions.size(); ++i) {
        auto& ins = ir.instructions[i];
        auto& edges = flow[i].edges;
        if (ins.op == Op::MoveException && !handler_entry[i])
            reject(ins.pc, "move-exception outside a handler entry");
        if (ins.op == Op::MoveResult) {
            if (i == 0 || ir.instructions[i - 1].op != Op::Invoke)
                reject(ins.pc, "move-result does not follow an invoke");
            const auto& callee = dex.methods[ir.instructions[i - 1].id];
            if (kind(dex.types[dex.protos[callee.proto].result]) != ins.output)
                reject(ins.pc, "move-result does not match the invoke result");
        }
        if (ins.target != kNone) {
            const auto target = instruction_at(ins.target);
            if (ir.instructions[target].op == Op::MoveResult)
                reject(ins.pc, "branch enters move-result");
            edges[0].push_back(target);
        }
        if (!terminal(ins) && i + 1 < ir.instructions.size())
            edges[0].push_back(i + 1);
        while (try_index < code.tries.size() && code.tries[try_index].end <= ins.pc)
            ++try_index;
        if (ins.may_throw && try_index < code.tries.size() && code.tries[try_index].start <= ins.pc) {
            ins.try_index = try_index;
            for (const auto& handler : code.tries[try_index].handlers) {
                const auto target = instruction_at(handler.target);
                if (ir.instructions[target].op == Op::MoveResult)
                    reject(handler.target, "catch enters move-result");
                edges[1].push_back(target);
            }
        }
        for (auto to : edges[0]) {
            if (ir.instructions[to].op == Op::MoveException)
                reject(ins.pc, "normal control flow enters move-exception");
        }
        for (const auto& destinations : edges)
            for (auto to : destinations)
                flow[to].predecessors.push_back(i);
    }
    if (ir.instructions.front().op == Op::MoveException)
        reject(0, "method entry is a move-exception");
}

void find_roots(MethodIR& ir, const Method& method, const wa_art_dex_t& dex,
                const std::vector<Flow>& flow) {
    const auto count = ir.instructions.size();
    const auto registers = method.code.registers;
    constexpr size_t kMaxDataflowCells = 32 * 1024 * 1024;
    if (count > kMaxDataflowCells / (size_t(registers) + 1))
        reject(0, "method exceeds register analysis limit");
    std::vector<Kind> types(count * registers);
    const auto state = [&](uint32_t i) {
        return std::span(types).subspan(size_t(i) * registers, registers);
    };
    const auto incoming = parameters(dex, method, method.access & kStatic);
    uint32_t slot = registers - method.code.ins;
    for (auto kind : incoming) {
        if (slot + (kind == Kind::Wide) >= registers)
            reject(0, "prototype arguments exceed incoming register count");
        state(0)[slot++] = kind;
        if (kind == Kind::Wide)
            state(0)[slot++] = Kind::WideHigh;
    }
    if (slot != registers)
        reject(0, "incoming register count does not match prototype");

    std::deque<uint32_t> queue{0};
    std::vector<bool> enqueued(count);
    enqueued[0] = true;
    ir.instructions[0].reachable = true;
    std::vector<Kind> after(registers);
    while (!queue.empty()) {
        const auto i = queue.front();
        queue.pop_front();
        enqueued[i] = false;
        const auto& ins = ir.instructions[i];
        const auto before = state(i);
        std::copy(before.begin(), before.end(), after.begin());
        if (ins.output != Kind::Unset) {
            clobber(after, ins.a);
            if (ins.output == Kind::Wide) {
                clobber(after, ins.a + 1);
                after[ins.a + 1] = Kind::WideHigh;
            }
            after[ins.a] = ins.op == Op::Move && before[ins.b] == Kind::Zero &&
                                  ins.output == Kind::Scalar ? Kind::Zero : ins.output;
        }
        for (unsigned edge_kind = 0; edge_kind < 2; ++edge_kind) {
            const auto source = edge_kind ? before : std::span(after);
            for (auto target : flow[i].edges[edge_kind]) {
                auto& successor = ir.instructions[target];
                const auto destination = state(target);
                bool changed = !successor.reachable;
                if (changed) {
                    std::copy(source.begin(), source.end(), destination.begin());
                    successor.reachable = true;
                } else {
                    for (size_t j = 0; j < registers; ++j) {
                        const auto joined = merge_kind(destination[j], source[j]);
                        changed |= joined != destination[j];
                        destination[j] = joined;
                    }
                }
                if (changed && !enqueued[target]) {
                    enqueued[target] = true;
                    queue.push_back(target);
                }
            }
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const auto& ins = ir.instructions[i];
        if (!ins.reachable)
            continue;
        if (!terminal(ins) && i + 1 == count)
            reject(ins.pc, "execution can fall off end of code item");
        const auto before = state(i);
        for (auto use : ins.reads) {
            if (!matches(before[use.reg], use.kind) ||
                (use.kind == Kind::Wide && before[use.reg + 1] != Kind::WideHigh))
                reject(ins.pc, "register v" + std::to_string(use.reg) + " has incompatible type");
        }
        if (ins.op == Op::Branch && ins.id < 2 && !ins.literal) {
            const auto a = before[ins.b];
            const auto b = before[ins.c];
            if ((a == Kind::Reference && b == Kind::Scalar) ||
                (a == Kind::Scalar && b == Kind::Reference))
                reject(ins.pc, "reference and scalar compared by the same branch");
        }
    }

    const auto rowsize = (size_t(registers) + 63) / 64;
    std::vector<uint64_t> liveness(count * rowsize);
    const auto live = [&](uint32_t i) {
        return std::span(liveness).subspan(size_t(i) * rowsize, rowsize);
    };
    for (uint32_t i = static_cast<uint32_t>(count); i > 0; --i) {
        if (ir.instructions[i - 1].reachable) {
            enqueued[i - 1] = true;
            queue.push_back(i - 1);
        }
    }
    std::vector<uint64_t> next(rowsize);
    while (!queue.empty()) {
        const auto i = queue.front();
        queue.pop_front();
        enqueued[i] = false;
        const auto& ins = ir.instructions[i];
        const auto join = [&](const Successors& successors) {
            for (auto to : successors) {
                const auto reachable = live(to);
                for (size_t j = 0; j < rowsize; ++j)
                    next[j] |= reachable[j];
            }
        };
        std::fill(next.begin(), next.end(), 0);
        join(flow[i].edges[0]);
        if (ins.output != Kind::Unset) {
            next[ins.a / 64] &= ~(uint64_t{1} << (ins.a % 64));
            if (ins.output == Kind::Wide) {
                const auto high = ins.a + 1;
                next[high / 64] &= ~(uint64_t{1} << (high % 64));
            }
        }
        // A failing instruction has not written its destination.
        join(flow[i].edges[1]);
        for (auto use : ins.reads)
            next[use.reg / 64] |= uint64_t{1} << (use.reg % 64);
        const auto current = live(i);
        if (!std::equal(next.begin(), next.end(), current.begin())) {
            std::copy(next.begin(), next.end(), current.begin());
            for (auto previous : flow[i].predecessors) {
                if (ir.instructions[previous].reachable && !enqueued[previous]) {
                    enqueued[previous] = true;
                    queue.push_back(previous);
                }
            }
        }
    }

    std::vector<bool> poll_target(count);
    for (const auto& ins : ir.instructions) {
        if (ins.reachable && ins.target != kNone && ins.target <= ins.pc)
            poll_target[ir.pc_to_index[ins.target]] = true;
    }
    for (uint32_t i = 0; i < count; ++i) {
        auto& ins = ir.instructions[i];
        if (!ins.reachable || (!ins.may_throw && !poll_target[i]))
            continue;
        const auto before = state(i);
        const auto row = live(i);
        for (uint32_t r = 0; r < registers; ++r) {
            if (!(row[r / 64] & (uint64_t{1} << (r % 64))))
                continue;
            if (before[r] == Kind::Reference)
                ins.roots.push_back(static_cast<uint16_t>(r));
            if (before[r] == Kind::Conflict || before[r] == Kind::Unset)
                reject(ins.pc, "no precise type for a live register at safepoint");
        }
        ir.max_roots = std::max(ir.max_roots, static_cast<uint32_t>(ins.roots.size()));
    }
}

void build_blocks(MethodIR& ir, const std::vector<Flow>& flow) {
    const auto count = ir.instructions.size();
    std::vector<bool> leader(count);
    leader[0] = true;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& ins = ir.instructions[i];
        if (!ins.reachable)
            continue;
        if (ins.ends_block() && i + 1 < count)
            leader[i + 1] = true;
        if (ins.target != kNone)
            leader[ir.pc_to_index[ins.target]] = true;
        for (auto target : flow[i].edges[1])
            leader[target] = true;
    }
    ir.block_by_instruction.resize(count, kNone);
    for (uint32_t i = 0; i < count;) {
        if (!ir.instructions[i].reachable) {
            ++i;
            continue;
        }
        const auto begin = i;
        const auto block = static_cast<uint32_t>(ir.blocks.size());
        do {
            ir.block_by_instruction[i++] = block;
        } while (i < count && ir.instructions[i].reachable && !leader[i]);
        ir.blocks.push_back({begin, i});
    }
}

} // namespace

bool Instruction::ends_block() const {
    return terminal(*this) || op == Op::Branch;
}

std::vector<Kind> parameters(const wa_art_dex_t& dex, const Method& method, bool is_static) {
    const auto& proto = dex.protos[method.proto];
    std::vector<Kind> result;
    result.reserve(proto.parameters.size() + !is_static);
    if (!is_static)
        result.push_back(Kind::Reference);
    for (auto type : proto.parameters)
        result.push_back(kind(dex.types[type]));
    return result;
}

MethodIR analyze_method(const wa_art_dex_t& dex, const Method& method) {
    MethodIR ir;
    const auto length = static_cast<uint32_t>(method.code.words.size());
    ir.pc_to_index.resize(length, kNone);
    for (uint32_t pc = 0; pc < length;) {
        ir.pc_to_index[pc] = static_cast<uint32_t>(ir.instructions.size());
        auto ins = Decoder{dex, method, pc}.decode();
        if (ins.width > length - pc)
            reject(pc, "truncated instruction");
        pc += ins.width;
        ir.instructions.push_back(std::move(ins));
    }
    std::vector<Flow> flow(ir.instructions.size());
    build_edges(ir, method, dex, flow);
    find_roots(ir, method, dex, flow);
    build_blocks(ir, flow);
    return ir;
}

} // namespace wa::art
