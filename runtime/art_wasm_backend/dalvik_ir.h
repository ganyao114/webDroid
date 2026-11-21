#pragma once

#include "dex_internal.h"

namespace wa::art {

enum class Op {
    Nop, Move, MoveResult, MoveException, Return, Constant, String, Goto, Branch, Throw,
    NegInt, IntToLong, LongToInt, FloatToInt, IntBinary, LongBinary, FloatBinary,
    NewObject, NewArray, ArrayLength, ArrayGet, ArrayPut, GetField, PutField, GetStatic,
    PutStatic, CheckCast, InstanceOf, Invoke,
};

enum class Binary : uint8_t { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr, Ushr };
enum class Invoke : uint8_t { Static, Direct, Virtual };

struct Use {
    uint16_t reg;
    Kind kind;
};

struct Instruction {
    Op op = Op::Nop;
    uint32_t pc = 0;
    uint32_t width = 1;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    uint32_t id = 0;
    uint32_t target = kNone;
    uint32_t try_index = kNone;
    uint64_t bits = 0;
    Kind output = Kind::Unset;
    Binary binary = Binary::Add;
    Invoke invoke = Invoke::Static;
    bool literal = false;
    bool reverse = false;
    bool may_throw = false;
    bool reachable = false;
    std::vector<Use> reads;
    std::vector<uint16_t> roots;

    bool ends_block() const;
};

struct Block {
    uint32_t begin;
    uint32_t end;
};

struct MethodIR {
    std::vector<Instruction> instructions;
    std::vector<uint32_t> pc_to_index;
    std::vector<Block> blocks;
    std::vector<uint32_t> block_by_instruction;
    uint32_t max_roots = 0;
};

std::vector<Kind> parameters(const wa_art_dex_t& dex, const Method& method, bool is_static);
MethodIR analyze_method(const wa_art_dex_t& dex, const Method& method);

} // namespace wa::art
