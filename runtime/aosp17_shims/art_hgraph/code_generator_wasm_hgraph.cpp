#include "code_generator_wasm_hgraph.h"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dex/dex_file-inl.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/nodes.h"

namespace wa::hgraph {
namespace {

using Type = art::DataType::Type;

enum class Value : uint8_t { I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, Void = 0x40 };

enum Opcode : uint8_t {
  Unreachable = 0x00, Block = 0x02, Loop = 0x03, If = 0x04, Else = 0x05, End = 0x0b,
  Br = 0x0c, BrIf = 0x0d, BrTable = 0x0e, Return = 0x0f, Call = 0x10, Select = 0x1b,
  LocalGet = 0x20, LocalSet = 0x21, LocalTee = 0x22,
  I32Const = 0x41, I64Const = 0x42, F32Const = 0x43, F64Const = 0x44,
  I32Eqz = 0x45, I32Eq = 0x46, I64Eqz = 0x50, I64Eq = 0x51,
  F64Lt = 0x63, F64Gt = 0x64, F64Ge = 0x66,
  I32Sub = 0x6b, I32And = 0x71, I32Xor = 0x73,
  I64Sub = 0x7d, I64Xor = 0x85,
  F32Abs = 0x8b, F32Neg = 0x8c, F64Abs = 0x99, F64Neg = 0x9a, F64Sub = 0xa1,
  F64Mul = 0xa2, F64Copysign = 0xa6,
  I32WrapI64 = 0xa7, I64ExtendI32S = 0xac, I64ExtendI32U = 0xad,
  F32ConvertI32S = 0xb2, F32ConvertI32U = 0xb3, F32ConvertI64S = 0xb4, F32ConvertI64U = 0xb5,
  F32DemoteF64 = 0xb6,
  F64ConvertI32S = 0xb7, F64ConvertI32U = 0xb8, F64ConvertI64S = 0xb9, F64ConvertI64U = 0xba,
  F64PromoteF32 = 0xbb, I32Extend8S = 0xc0, I32Extend16S = 0xc1,
  MiscPrefix = 0xfc,
};

Value WasmType(Type type) {
  switch (art::DataType::Kind(type)) {
    case Type::kInt32: return Value::I32;
    case Type::kInt64: return Value::I64;
    case Type::kFloat32: return Value::F32;
    case Type::kFloat64: return Value::F64;
    case Type::kVoid: return Value::Void;
    default: throw std::runtime_error("Wasm HGraph compiler requires primitive values");
  }
}

bool IsFloat(Value type) {
  return type == Value::F32 || type == Value::F64;
}

struct Signature {
  std::vector<Value> params;
  Value result;
};

Signature GetSignature(const art::HGraph& graph) {
  const char* shorty = graph.GetDexFile().GetMethodShorty(graph.GetMethodIdx());
  Signature signature{{}, WasmType(art::DataType::FromShorty(*shorty++))};
  for (; *shorty != '\0'; ++shorty) {
    signature.params.push_back(WasmType(art::DataType::FromShorty(*shorty)));
  }
  return signature;
}

struct Bytes {
  std::vector<uint8_t> data;

  void Byte(uint8_t value) { data.push_back(value); }
  void Type(Value value) { Byte(static_cast<uint8_t>(value)); }
  void Append(const Bytes& other) { data.insert(data.end(), other.data.begin(), other.data.end()); }

  void Uleb(uint32_t value) {
    do {
      const uint8_t low = value & 0x7f;
      value >>= 7;
      Byte(low | (value != 0 ? 0x80 : 0));
    } while (value != 0);
  }

  void Sleb(int64_t value) {
    bool more;
    do {
      const uint8_t low = value & 0x7f;
      value >>= 7;
      more = !((value == 0 && (low & 0x40) == 0) || (value == -1 && (low & 0x40) != 0));
      Byte(low | (more ? 0x80 : 0));
    } while (more);
  }

  void Name(std::string_view name) {
    Uleb(name.size());
    data.insert(data.end(), name.begin(), name.end());
  }

  void Payload(const Bytes& payload) {
    Uleb(payload.data.size());
    Append(payload);
  }

  void Section(uint8_t id, const Bytes& payload) {
    Byte(id);
    Payload(payload);
  }

  void Index(Opcode op, uint32_t index) {
    Byte(op);
    Uleb(index);
  }

  void Open(Opcode op, Value result = Value::Void) {
    Byte(op);
    Type(result);
  }

  void Int(int32_t value) {
    Byte(I32Const);
    Sleb(value);
  }

  void Long(int64_t value) {
    Byte(I64Const);
    Sleb(value);
  }

  void Float(float value) {
    Byte(F32Const);
    LittleEndian(std::bit_cast<uint32_t>(value));
  }

  void Double(double value) {
    Byte(F64Const);
    LittleEndian(std::bit_cast<uint64_t>(value));
  }

 private:
  template <typename UInt>
  void LittleEndian(UInt bits) {
    for (size_t i = 0; i < sizeof(bits); ++i) {
      Byte(bits & 0xff);
      bits >>= 8;
    }
  }
};

void WriteLocals(Bytes& body, std::span<const Value> locals) {
  // The binary format groups consecutive locals of the same type.
  std::vector<std::pair<uint32_t, Value>> groups;
  for (Value type : locals) {
    if (groups.empty() || groups.back().second != type) {
      groups.emplace_back(1, type);
    } else {
      ++groups.back().first;
    }
  }
  body.Uleb(groups.size());
  for (const auto& [count, type] : groups) {
    body.Uleb(count);
    body.Type(type);
  }
}

struct NumericOpcodes {
  uint8_t i32;
  uint8_t i64;
  uint8_t f32 = 0;
  uint8_t f64 = 0;

  uint8_t For(Value type) const {
    switch (type) {
      case Value::I32: return i32;
      case Value::I64: return i64;
      case Value::F32: return f32;
      case Value::F64: return f64;
      case Value::Void: return 0;
    }
    return 0;
  }
};

const NumericOpcodes kComparisons[] = {
    {0x46, 0x51, 0x5b, 0x61},  // EQ
    {0x47, 0x52, 0x5c, 0x62},  // NE
    {0x48, 0x53, 0x5d, 0x63},  // LT
    {0x4c, 0x57, 0x5f, 0x65},  // LE
    {0x4a, 0x55, 0x5e, 0x64},  // GT
    {0x4e, 0x59, 0x60, 0x66},  // GE
    {0x49, 0x54},              // B
    {0x4d, 0x58},              // BE
    {0x4b, 0x56},              // A
    {0x4f, 0x5a},              // AE
};
static_assert(std::size(kComparisons) == art::kCondLast + 1);

constexpr uint32_t kUnassigned = std::numeric_limits<uint32_t>::max();
using FunctionIndices = std::unordered_map<uint32_t, uint32_t>;

// Java floating remainder has no Wasm opcode. Subtract powers of two times
// the divisor, highest first. Each subtraction is exact since its operands
// differ by less than a factor of two. Promote f32 operands to use this too.
Bytes EmitFloatRemainder() {
  constexpr uint32_t dividend = 0, divisor = 1, remaining = 2, magnitude = 3, scale = 4;
  Bytes body;
  constexpr std::array locals{Value::F64, Value::F64, Value::F64};
  WriteLocals(body, locals);
  body.Index(LocalGet, dividend);
  body.Byte(F64Abs);
  body.Index(LocalTee, remaining);
  body.Double(std::numeric_limits<double>::infinity());
  body.Byte(F64Lt);
  body.Index(LocalGet, divisor);
  body.Byte(F64Abs);
  body.Index(LocalTee, magnitude);
  body.Double(0);
  body.Byte(F64Gt);
  body.Byte(I32And);
  body.Byte(I32Eqz);
  body.Open(If);
  body.Double(std::numeric_limits<double>::quiet_NaN());
  body.Byte(Return);
  body.Byte(End);

  body.Index(LocalGet, magnitude);
  body.Index(LocalSet, scale);
  body.Open(Block);
  body.Open(Loop);
  body.Index(LocalGet, scale);
  body.Index(LocalGet, remaining);
  body.Double(0.5);
  body.Byte(F64Mul);
  body.Byte(F64Gt);
  body.Index(BrIf, 1);
  body.Index(LocalGet, scale);
  body.Double(2);
  body.Byte(F64Mul);
  body.Index(LocalSet, scale);
  body.Index(Br, 0);
  body.Byte(End);
  body.Byte(End);

  body.Open(Block);
  body.Open(Loop);
  body.Index(LocalGet, remaining);
  body.Index(LocalGet, magnitude);
  body.Byte(F64Lt);
  body.Index(BrIf, 1);
  body.Index(LocalGet, remaining);
  body.Index(LocalGet, scale);
  body.Byte(F64Ge);
  body.Open(If);
  body.Index(LocalGet, remaining);
  body.Index(LocalGet, scale);
  body.Byte(F64Sub);
  body.Index(LocalSet, remaining);
  body.Byte(End);
  body.Index(LocalGet, scale);
  body.Double(0.5);
  body.Byte(F64Mul);
  body.Index(LocalSet, scale);
  body.Index(Br, 0);
  body.Byte(End);
  body.Byte(End);

  body.Index(LocalGet, remaining);
  body.Index(LocalGet, dividend);
  body.Byte(F64Copysign);
  body.Byte(End);
  return body;
}

}  // namespace

class CodeGeneratorWasm::FunctionEmitter final : public art::HGraphVisitor {
 public:
  FunctionEmitter(art::HGraph* graph, const std::vector<Signature>& signatures,
                  uint32_t index, const FunctionIndices& functions, bool& needs_float_rem)
      : HGraphVisitor(graph),
        signatures_(signatures),
        signature_(signatures[index]),
        functions_(functions),
        needs_float_rem_(needs_float_rem),
        locals_by_id_(graph->GetCurrentInstructionId(), kUnassigned),
        cases_by_id_(graph->GetBlocks().size(), kUnassigned) {}

  Bytes Emit() {
    const auto& blocks = GetGraph()->GetReversePostOrder();
    if (blocks.empty() || blocks.front() != GetGraph()->GetEntryBlock()) {
      throw std::runtime_error("Wasm compilation requires the HGraph reverse postorder");
    }

    // ART indexes parameters by incoming DEX register, including wide pairs.
    std::vector<uint32_t> params;
    for (uint32_t index = 0; index < signature_.params.size(); ++index) {
      params.push_back(index);
      if (signature_.params[index] == Value::I64 || signature_.params[index] == Value::F64) {
        params.push_back(kUnassigned);
      }
    }

    pc_ = NewLocal(Value::I32);
    for (uint32_t index = 0; index < blocks.size(); ++index) {
      art::HBasicBlock* block = blocks[index];
      cases_by_id_.at(block->GetBlockId()) = index;
      for (const auto* phi = block->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
        BindLocal(phi);
      }
      for (const auto* instruction = block->GetFirstInstruction(); instruction != nullptr;
           instruction = instruction->GetNext()) {
        if (instruction->IsParameterValue()) {
          const auto* param = instruction->AsParameterValue();
          const uint32_t position = params.at(param->GetInputVRegIndex());
          if (position == kUnassigned || WasmType(param->GetType()) != signature_.params[position]) {
            throw std::runtime_error("HParameterValue disagrees with its DEX signature");
          }
          locals_by_id_.at(param->GetId()) = position;
        } else if (instruction->GetType() != Type::kVoid &&
                   !instruction->IsConstant() && !instruction->IsCurrentMethod()) {
          BindLocal(instruction);
        }
      }
    }

    WriteLocals(body_, locals_);
    // Block zero is the entry and pc starts at zero. Each br_table label
    // exits its enclosing block and lands at the corresponding HBasicBlock.
    body_.Open(Loop);
    body_.Open(Block);  // Invalid pc sink.
    for (size_t i = 0; i < blocks.size(); ++i) {
      body_.Open(Block);
    }
    body_.Index(LocalGet, pc_);
    body_.Byte(BrTable);
    body_.Uleb(blocks.size());
    for (uint32_t depth = 0; depth < blocks.size(); ++depth) {
      body_.Uleb(depth);
    }
    body_.Uleb(blocks.size());

    dispatch_depth_ = blocks.size();
    for (art::HBasicBlock* block : blocks) {
      body_.Byte(End);
      VisitBasicBlock(block);
      --dispatch_depth_;
    }
    body_.Byte(End);
    body_.Byte(Unreachable);
    body_.Byte(End);
    body_.Byte(Unreachable);
    body_.Byte(End);
    return std::move(body_);
  }

 private:
  void VisitInstruction(art::HInstruction* inst) override {
    throw std::runtime_error(std::string("Unsupported ART instruction ") + inst->DebugName() +
                             " at dex pc " + std::to_string(inst->GetDexPc()));
  }

  void VisitPhi(art::HPhi*) override {}  // Assigned by incoming edges.
  void VisitParameterValue(art::HParameterValue*) override {}
  void VisitCurrentMethod(art::HCurrentMethod*) override {}  // Native invoke metadata.
  void VisitIntConstant(art::HIntConstant*) override {}
  void VisitLongConstant(art::HLongConstant*) override {}
  void VisitFloatConstant(art::HFloatConstant*) override {}
  void VisitDoubleConstant(art::HDoubleConstant*) override {}
  void VisitSuspendCheck(art::HSuspendCheck*) override {}  // No managed threads.
  void VisitNop(art::HNop*) override {}
  void VisitExit(art::HExit*) override { body_.Byte(Unreachable); }

  void VisitAdd(art::HAdd* inst) override { Binary(inst, {0x6a, 0x7c, 0x92, 0xa0}); }
  void VisitSub(art::HSub* inst) override { Binary(inst, {0x6b, 0x7d, 0x93, 0xa1}); }
  void VisitMul(art::HMul* inst) override { Binary(inst, {0x6c, 0x7e, 0x94, 0xa2}); }
  void VisitAnd(art::HAnd* inst) override { Binary(inst, {0x71, 0x83}); }
  void VisitOr(art::HOr* inst) override { Binary(inst, {0x72, 0x84}); }
  void VisitXor(art::HXor* inst) override { Binary(inst, {0x73, 0x85}); }
  void VisitShl(art::HShl* inst) override { Binary(inst, {0x74, 0x86}, true); }
  void VisitShr(art::HShr* inst) override { Binary(inst, {0x75, 0x87}, true); }
  void VisitUShr(art::HUShr* inst) override { Binary(inst, {0x76, 0x88}, true); }
  void VisitRol(art::HRol* inst) override { Binary(inst, {0x77, 0x89}, true); }
  void VisitRor(art::HRor* inst) override { Binary(inst, {0x78, 0x8a}, true); }

  void VisitMin(art::HMin* inst) override { MinMax(inst, true); }
  void VisitMax(art::HMax* inst) override { MinMax(inst, false); }

  void VisitAbs(art::HAbs* inst) override {
    const Value type = WasmType(inst->GetType());
    Read(inst->InputAt(0));
    if (IsFloat(type)) {
      body_.Byte(type == Value::F32 ? F32Abs : F64Abs);
    } else {
      IntegerConstant(type, 0);
      body_.Byte(kComparisons[art::kCondLT].For(type));
      body_.Open(If, type);
      IntegerConstant(type, 0);
      Read(inst->InputAt(0));
      body_.Byte(type == Value::I64 ? I64Sub : I32Sub);
      body_.Byte(Else);
      Read(inst->InputAt(0));
      body_.Byte(End);
    }
    Write(inst);
  }

  void VisitDiv(art::HDiv* inst) override {
    const Value type = WasmType(inst->GetType());
    if (IsFloat(type)) {
      Binary(inst, {0, 0, 0x95, 0xa3});
      return;
    }
    // Java wraps MIN_VALUE / -1. Wasm signed division traps at that input.
    const bool wide = type == Value::I64;
    Read(inst->GetRight());
    IntegerConstant(type, -1);
    body_.Byte(wide ? I64Eq : I32Eq);
    body_.Open(If, type);
    IntegerConstant(type, 0);
    Read(inst->GetLeft());
    body_.Byte(wide ? I64Sub : I32Sub);
    body_.Byte(Else);
    Read(inst->GetLeft());
    Read(inst->GetRight());
    body_.Byte(wide ? 0x7f : 0x6d);  // i64.div_s / i32.div_s
    body_.Byte(End);
    Write(inst);
  }

  void VisitRem(art::HRem* inst) override {
    const Value type = WasmType(inst->GetType());
    if (!IsFloat(type)) {
      Binary(inst, {0x6f, 0x81});
      return;
    }
    const bool narrow = type == Value::F32;
    Read(inst->GetLeft());
    if (narrow) body_.Byte(F64PromoteF32);
    Read(inst->GetRight());
    if (narrow) body_.Byte(F64PromoteF32);
    needs_float_rem_ = true;
    body_.Index(Call, signatures_.size());
    if (narrow) body_.Byte(F32DemoteF64);
    Write(inst);
  }

  void VisitDivZeroCheck(art::HDivZeroCheck* inst) override {
    Read(inst->InputAt(0));
    body_.Index(LocalTee, Local(inst));
    body_.Byte(WasmType(inst->GetType()) == Value::I64 ? I64Eqz : I32Eqz);
    body_.Open(If);
    body_.Byte(Unreachable);
    body_.Byte(End);
  }

  void VisitNeg(art::HNeg* inst) override {
    const Value type = WasmType(inst->GetType());
    if (IsFloat(type)) {
      Read(inst->InputAt(0));
      body_.Byte(type == Value::F32 ? F32Neg : F64Neg);
    } else {
      IntegerConstant(type, 0);
      Read(inst->InputAt(0));
      body_.Byte(type == Value::I64 ? I64Sub : I32Sub);
    }
    Write(inst);
  }

  void VisitNot(art::HNot* inst) override {
    Read(inst->InputAt(0));
    const Value type = WasmType(inst->GetType());
    IntegerConstant(type, -1);
    body_.Byte(type == Value::I64 ? I64Xor : I32Xor);
    Write(inst);
  }

  void VisitBooleanNot(art::HBooleanNot* inst) override {
    Read(inst->InputAt(0));
    body_.Byte(I32Eqz);
    Write(inst);
  }

  void VisitEqual(art::HEqual* inst) override { Condition(inst); }
  void VisitNotEqual(art::HNotEqual* inst) override { Condition(inst); }
  void VisitLessThan(art::HLessThan* inst) override { Condition(inst); }
  void VisitLessThanOrEqual(art::HLessThanOrEqual* inst) override { Condition(inst); }
  void VisitGreaterThan(art::HGreaterThan* inst) override { Condition(inst); }
  void VisitGreaterThanOrEqual(art::HGreaterThanOrEqual* inst) override { Condition(inst); }
  void VisitBelow(art::HBelow* inst) override { Condition(inst); }
  void VisitBelowOrEqual(art::HBelowOrEqual* inst) override { Condition(inst); }
  void VisitAbove(art::HAbove* inst) override { Condition(inst); }
  void VisitAboveOrEqual(art::HAboveOrEqual* inst) override { Condition(inst); }

  void VisitCompare(art::HCompare* inst) override {
    const Type type = inst->GetComparisonType();
    if (IsFloat(WasmType(type))) {
      // cmpg/cmpl differ only for unordered operands. Negating the
      // opposite ordered relation treats unordered as greater or less.
      const bool gt_bias = inst->IsGtBias();
      Compare(inst, gt_bias ? art::kCondLE : art::kCondGT);
      if (gt_bias) body_.Byte(I32Eqz);
      Compare(inst, gt_bias ? art::kCondLT : art::kCondGE);
      if (!gt_bias) body_.Byte(I32Eqz);
    } else {
      const bool uns = art::DataType::IsUnsignedType(type);
      Compare(inst, uns ? art::kCondA : art::kCondGT);
      Compare(inst, uns ? art::kCondB : art::kCondLT);
    }
    body_.Byte(I32Sub);
    Write(inst);
  }

  void VisitTypeConversion(art::HTypeConversion* inst) override {
    const Type input = inst->GetInputType();
    const Type output = inst->GetResultType();
    const Value from = WasmType(input);
    const Value to = WasmType(output);
    Read(inst->GetInput());

    if (IsFloat(from) && IsFloat(to)) {
      if (from != to) body_.Byte(to == Value::F64 ? F64PromoteF32 : F32DemoteF64);
    } else if (IsFloat(from)) {
      // trunc_sat implements Java's NaN-to-zero and signed saturation.
      const bool uns = output == Type::kUint32 || output == Type::kUint64;
      const uint32_t conversion = (to == Value::I64 ? 4 : 0) +
                                  (from == Value::F64 ? 2 : 0) + (uns ? 1 : 0);
      body_.Index(MiscPrefix, conversion);
    } else if (IsFloat(to)) {
      const bool uns = art::DataType::IsUnsignedType(input);
      if (to == Value::F32) {
        body_.Byte(from == Value::I64 ? (uns ? F32ConvertI64U : F32ConvertI64S)
                                     : (uns ? F32ConvertI32U : F32ConvertI32S));
      } else {
        body_.Byte(from == Value::I64 ? (uns ? F64ConvertI64U : F64ConvertI64S)
                                     : (uns ? F64ConvertI32U : F64ConvertI32S));
      }
    } else if (from != to) {
      body_.Byte(to == Value::I32 ? I32WrapI64 :
                 (art::DataType::IsUnsignedType(input) ? I64ExtendI32U : I64ExtendI32S));
    }

    switch (output) {
      case Type::kInt8: body_.Byte(I32Extend8S); break;
      case Type::kInt16: body_.Byte(I32Extend16S); break;
      case Type::kUint8: body_.Int(0xff); body_.Byte(I32And); break;
      case Type::kUint16: body_.Int(0xffff); body_.Byte(I32And); break;
      default: break;
    }
    Write(inst);
  }

  void VisitSelect(art::HSelect* inst) override {
    Read(inst->GetTrueValue());
    Read(inst->GetFalseValue());
    Read(inst->GetCondition());
    body_.Byte(Select);
    Write(inst);
  }

  void VisitIf(art::HIf* inst) override {
    Read(inst->InputAt(0));
    body_.Open(If);
    Edge(inst->GetBlock(), inst->IfTrueSuccessor());
    body_.Byte(Else);
    Edge(inst->GetBlock(), inst->IfFalseSuccessor());
    body_.Byte(End);
    body_.Index(Br, dispatch_depth_);
  }

  void VisitGoto(art::HGoto* inst) override {
    Edge(inst->GetBlock(), inst->GetBlock()->GetSingleSuccessor());
    body_.Index(Br, dispatch_depth_);
  }

  void VisitReturn(art::HReturn* inst) override {
    if (WasmType(inst->InputAt(0)->GetType()) != signature_.result) {
      throw std::runtime_error("HReturn disagrees with its DEX signature");
    }
    Read(inst->InputAt(0));
    body_.Byte(Return);
  }

  void VisitReturnVoid(art::HReturnVoid*) override {
    if (signature_.result != Value::Void) {
      throw std::runtime_error("HReturnVoid disagrees with its DEX signature");
    }
    body_.Byte(Return);
  }

  void VisitInvokeStaticOrDirect(art::HInvokeStaticOrDirect* inst) override {
    const auto target = inst->GetResolvedMethodReference();
    const auto function = functions_.find(target.index);
    if (!inst->IsStatic() || inst->IsIntrinsic() || target.dex_file != &GetGraph()->GetDexFile() ||
        function == functions_.end() || inst->GetClinitCheckRequirement() !=
                                           art::HInvokeStaticOrDirect::ClinitCheckRequirement::kNone) {
      throw std::runtime_error("HInvokeStaticOrDirect requires a local static target in the module");
    }
    const Signature& callee = signatures_[function->second];
    if (inst->GetNumberOfArguments() != callee.params.size() ||
        WasmType(inst->GetType()) != callee.result) {
      throw std::runtime_error("HInvokeStaticOrDirect disagrees with its target signature");
    }
    for (size_t i = 0; i < callee.params.size(); ++i) {
      if (WasmType(inst->InputAt(i)->GetType()) != callee.params[i]) {
        throw std::runtime_error("HInvokeStaticOrDirect argument type mismatch");
      }
      Read(inst->InputAt(i));
    }
    body_.Index(Call, function->second);
    if (callee.result != Value::Void) {
      Write(inst);
    }
  }

  uint32_t NewLocal(Value type) {
    const uint32_t index = signature_.params.size() + locals_.size();
    locals_.push_back(type);
    return index;
  }

  void BindLocal(const art::HInstruction* inst) {
    locals_by_id_.at(inst->GetId()) = NewLocal(WasmType(inst->GetType()));
  }

  uint32_t Local(const art::HInstruction* inst) const {
    const uint32_t local = locals_by_id_.at(inst->GetId());
    if (local == kUnassigned) {
      throw std::runtime_error(std::string("No Wasm local for ") + inst->DebugName());
    }
    return local;
  }

  void Read(const art::HInstruction* inst) {
    if (inst->IsIntConstant()) {
      body_.Int(inst->AsIntConstant()->GetValue());
    } else if (inst->IsLongConstant()) {
      body_.Long(inst->AsLongConstant()->GetValue());
    } else if (inst->IsFloatConstant()) {
      body_.Float(inst->AsFloatConstant()->GetValue());
    } else if (inst->IsDoubleConstant()) {
      body_.Double(inst->AsDoubleConstant()->GetValue());
    } else {
      body_.Index(LocalGet, Local(inst));
    }
  }

  void Write(const art::HInstruction* inst) { body_.Index(LocalSet, Local(inst)); }

  void IntegerConstant(Value type, int32_t value) {
    if (type == Value::I64) {
      body_.Long(value);
    } else {
      body_.Int(value);
    }
  }

  void Binary(art::HBinaryOperation* inst, NumericOpcodes opcodes, bool shift = false) {
    const Value type = WasmType(inst->GetType());
    const uint8_t opcode = opcodes.For(type);
    if (opcode == 0) {
      VisitInstruction(inst);
    }
    Read(inst->GetLeft());
    Read(inst->GetRight());
    if (shift && type == Value::I64) {
      // DEX uses an int distance even for a long shift. Wasm masks the low six bits.
      body_.Byte(I64ExtendI32U);
    }
    body_.Byte(opcode);
    Write(inst);
  }

  void Compare(art::HBinaryOperation* inst, art::IfCondition condition) {
    const uint8_t opcode = kComparisons[condition].For(WasmType(inst->GetLeft()->GetType()));
    if (opcode == 0) {
      VisitInstruction(inst);
    }
    Read(inst->GetLeft());
    Read(inst->GetRight());
    body_.Byte(opcode);
  }

  void Condition(art::HCondition* inst) {
    const auto condition = inst->GetCondition();
    const bool invert = IsFloat(WasmType(inst->GetLeft()->GetType())) &&
        ((inst->IsGtBias() && (condition == art::kCondGT || condition == art::kCondGE)) ||
         (inst->IsLtBias() && (condition == art::kCondLT || condition == art::kCondLE)));
    Compare(inst, invert ? inst->GetOppositeCondition() : condition);
    if (invert) body_.Byte(I32Eqz);
    Write(inst);
  }

  void MinMax(art::HBinaryOperation* inst, bool min) {
    if (IsFloat(WasmType(inst->GetType()))) {
      Binary(inst, min ? NumericOpcodes{0, 0, 0x96, 0xa4}
                       : NumericOpcodes{0, 0, 0x97, 0xa5});
    } else {
      Read(inst->GetLeft());
      Read(inst->GetRight());
      Compare(inst, min ? art::kCondLT : art::kCondGT);
      body_.Byte(Select);
      Write(inst);
    }
  }

  void Edge(art::HBasicBlock* from, art::HBasicBlock* to) {
    const size_t predecessor = to->GetPredecessorIndexOf(from);
    // Evaluate all phi sources first so back edges can swap or rotate locals.
    for (const auto* phi = to->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      Read(phi->InputAt(predecessor));
    }
    for (const auto* phi = to->GetLastPhi(); phi != nullptr; phi = phi->GetPrevious()) {
      Write(phi);
    }
    const uint32_t target = cases_by_id_.at(to->GetBlockId());
    if (target == kUnassigned) {
      throw std::runtime_error("SSA edge leaves the Wasm dispatch table");
    }
    body_.Int(std::bit_cast<int32_t>(target));
    body_.Index(LocalSet, pc_);
  }

  const std::vector<Signature>& signatures_;
  const Signature& signature_;
  const FunctionIndices& functions_;
  bool& needs_float_rem_;
  std::vector<uint32_t> locals_by_id_;
  std::vector<uint32_t> cases_by_id_;
  std::vector<Value> locals_;
  Bytes body_;
  uint32_t pc_ = 0;
  uint32_t dispatch_depth_ = 0;
};

std::vector<uint8_t> CodeGeneratorWasm::Compile(std::span<const WasmMethod> methods) {
  if (methods.empty() || methods.front().graph == nullptr) {
    throw std::invalid_argument("Wasm compilation needs at least one HGraph");
  }
  const art::DexFile& dex = methods.front().graph->GetDexFile();
  FunctionIndices indices;
  std::unordered_set<std::string_view> names;
  std::vector<Signature> signatures;
  signatures.reserve(methods.size());
  for (const WasmMethod& method : methods) {
    if (method.graph == nullptr || &method.graph->GetDexFile() != &dex ||
        !method.graph->IsInSsaForm() || method.graph->GetInvokeType() != art::kStatic) {
      throw std::invalid_argument("Wasm compilation requires static SSA graphs from one DEX file");
    }
    if (!indices.emplace(method.graph->GetMethodIdx(), signatures.size()).second ||
        !names.insert(method.export_name).second) {
      throw std::invalid_argument("Duplicate Wasm method or export name");
    }
    signatures.push_back(GetSignature(*method.graph));
  }

  bool needs_float_rem = false;
  std::vector<Bytes> bodies;
  for (uint32_t index = 0; index < methods.size(); ++index) {
    FunctionEmitter emitter(methods[index].graph, signatures, index, indices, needs_float_rem);
    bodies.push_back(emitter.Emit());
  }
  if (needs_float_rem) {
    signatures.push_back({{Value::F64, Value::F64}, Value::F64});
    bodies.push_back(EmitFloatRemainder());
  }

  Bytes types, functions, exports, code;
  types.Uleb(signatures.size());
  functions.Uleb(signatures.size());
  exports.Uleb(methods.size());
  code.Uleb(bodies.size());
  for (uint32_t index = 0; index < signatures.size(); ++index) {
    const Signature& signature = signatures[index];
    types.Byte(0x60);  // Function type.
    types.Uleb(signature.params.size());
    for (Value type : signature.params) {
      types.Type(type);
    }
    types.Byte(signature.result == Value::Void ? 0 : 1);
    if (signature.result != Value::Void) {
      types.Type(signature.result);
    }
    functions.Uleb(index);
    code.Payload(bodies[index]);
  }
  for (uint32_t index = 0; index < methods.size(); ++index) {
    exports.Name(methods[index].export_name);
    exports.Byte(0);  // Function export.
    exports.Uleb(index);
  }

  Bytes module{{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}};
  module.Section(1, types);
  module.Section(3, functions);
  module.Section(7, exports);
  module.Section(10, code);
  return module.data;
}

}  // namespace wa::hgraph
