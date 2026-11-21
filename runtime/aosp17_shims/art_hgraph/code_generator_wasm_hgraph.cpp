#include "code_generator_wasm_hgraph.h"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/nodes.h"
#include "standalone_runtime.h"

namespace wa::hgraph {
namespace {

using Type = art::DataType::Type;

enum class Value : uint8_t { I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, Void = 0x40 };

enum Opcode : uint8_t {
  Unreachable = 0x00, Block = 0x02, Loop = 0x03, If = 0x04, Else = 0x05, Throw = 0x08,
  End = 0x0b, Br = 0x0c, BrIf = 0x0d, BrTable = 0x0e, Return = 0x0f, Call = 0x10,
  CallIndirect = 0x11, Select = 0x1b, TryTable = 0x1f,
  LocalGet = 0x20, LocalSet = 0x21, LocalTee = 0x22,
  TableGet = 0x25, I32Load = 0x28, I32Store = 0x36,
  I32Const = 0x41, I64Const = 0x42, F32Const = 0x43, F64Const = 0x44,
  I32Eqz = 0x45, I32Eq = 0x46, I32LtU = 0x49, I32GtU = 0x4b, I32GeU = 0x4f,
  I64Eqz = 0x50, I64Eq = 0x51,
  F64Lt = 0x63, F64Gt = 0x64, F64Ge = 0x66,
  I32Add = 0x6a, I32Sub = 0x6b, I32And = 0x71, I32Or = 0x72, I32Xor = 0x73, I32Shl = 0x74,
  I64Sub = 0x7d, I64Xor = 0x85,
  F32Abs = 0x8b, F32Neg = 0x8c, F64Abs = 0x99, F64Neg = 0x9a, F64Sub = 0xa1,
  F64Mul = 0xa2, F64Copysign = 0xa6,
  I32WrapI64 = 0xa7, I64ExtendI32S = 0xac, I64ExtendI32U = 0xad,
  F32ConvertI32S = 0xb2, F32ConvertI32U = 0xb3, F32ConvertI64S = 0xb4, F32ConvertI64U = 0xb5,
  F32DemoteF64 = 0xb6,
  F64ConvertI32S = 0xb7, F64ConvertI32U = 0xb8, F64ConvertI64S = 0xb9, F64ConvertI64U = 0xba,
  F64PromoteF32 = 0xbb, I32ReinterpretF32 = 0xbc, I64ReinterpretF64 = 0xbd,
  F32ReinterpretI32 = 0xbe, F64ReinterpretI64 = 0xbf, I32Extend8S = 0xc0, I32Extend16S = 0xc1,
  RefIsNull = 0xd1,
  MiscPrefix = 0xfc,
};

Value WasmType(Type type) {
  switch (art::DataType::Kind(type)) {
    case Type::kReference:
    case Type::kInt32: return Value::I32;
    case Type::kInt64: return Value::I64;
    case Type::kFloat32: return Value::F32;
    case Type::kFloat64: return Value::F64;
    case Type::kVoid: return Value::Void;
    default: throw std::runtime_error("Unsupported HGraph value type");
  }
}

bool IsFloat(Value type) {
  return type == Value::F32 || type == Value::F64;
}

struct Signature {
  std::vector<Value> params;
  Value result;

  bool operator==(const Signature&) const = default;
};

Signature GetSignature(const art::DexFile& dex, uint32_t method, bool managed, bool is_static) {
  const char* shorty = dex.GetMethodShorty(method);
  Signature signature{{}, WasmType(art::DataType::FromShorty(*shorty++))};
  if (managed) {
    signature.params.push_back(Value::I32);  // wa_art_thread_t address.
  }
  if (!is_static) {
    signature.params.push_back(Value::I32);  // Receiver reference.
  }
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

  void Memory(Opcode op, uint32_t offset) {
    Byte(op);
    Uleb(2);  // i32 alignment.
    Uleb(offset);
  }

  void CatchJavaException() {
    Open(TryTable);
    Uleb(1);  // One catch clause.
    Byte(0);  // catch, carrying the tag's i32 payload.
    Uleb(0);  // java_exception tag.
    Uleb(0);  // The block immediately enclosing the try_table.
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

enum Runtime : uint32_t {
  NewObject, NewArray, ConstString, GetField, PutField, GetStatic, PutStatic,
  ArrayLength, ArrayGet, ArrayPut, IsInstance, VirtualMethod, Raise, Safepoint,
};

struct RuntimeImport {
  std::string_view name;
  std::initializer_list<Value> params;
  Value result;
};

constexpr auto i32 = Value::I32;
constexpr auto i64 = Value::I64;
constexpr auto kVoid = Value::Void;
constexpr RuntimeImport kRuntime[] = {
    {"rt_new_object", {i32, i32}, i32},
    {"rt_new_array", {i32, i32, i32}, i32},
    {"rt_const_string", {i32, i32}, i32},
    {"rt_get_field", {i32, i32, i32}, i64},
    {"rt_put_field", {i32, i32, i32, i64}, kVoid},
    {"rt_get_static", {i32, i32}, i64},
    {"rt_put_static", {i32, i32, i64}, kVoid},
    {"rt_array_length", {i32, i32}, i32},
    {"rt_array_get", {i32, i32, i32}, i64},
    {"rt_array_put", {i32, i32, i32, i64}, kVoid},
    {"rt_is_instance", {i32, i32, i32}, i32},
    {"rt_virtual_method", {i32, i32, i32}, i32},
    {"rt_raise", {i32, i32}, i32},
    {"rt_safepoint", {i32}, kVoid},
};
static_assert(std::size(kRuntime) == Safepoint + 1);

struct ModuleFunctions {
  explicit ModuleFunctions(bool managed) : managed(managed) {}

  uint32_t FunctionIndex(uint32_t definition) const {
    return (managed ? std::size(kRuntime) : 0) + definition;
  }

  uint32_t TypeIndex(Signature signature) {
    const auto found = std::find(types.begin(), types.end(), signature);
    if (found != types.end()) {
      return found - types.begin();
    }
    types.push_back(std::move(signature));
    return types.size() - 1;
  }

  bool managed;
  FunctionIndices indices;
  std::vector<Signature> methods;
  std::vector<Signature> types;
  bool needs_float_rem = false;
};

bool IsRoot(const art::HInstruction* inst) {
  return inst != nullptr && inst->GetType() == Type::kReference &&
         !inst->IsNullConstant() && !inst->IsLoadClass();
}

bool IsSafepoint(const art::HInstruction* inst) {
  return inst->IsInvoke() || inst->IsSuspendCheck() ||
         inst->IsNewInstance() || inst->IsNewArray() || inst->IsLoadString() ||
         inst->IsNullCheck() || inst->IsBoundsCheck() || inst->IsDivZeroCheck() ||
         inst->IsThrow() || inst->IsCheckCast() ||
         inst->IsFieldAccess() || inst->IsUnresolvedStaticFieldGet() ||
         inst->IsUnresolvedStaticFieldSet() || inst->IsArrayLength() ||
         inst->IsArrayGet() || inst->IsArraySet();
}

struct Handler {
  const art::HBasicBlock* block;
  art::dex::TypeIndex type;
};

const art::HInstruction* CatchInput(const art::HInstruction* inst, const art::HPhi* phi) {
  const auto* site = inst;
  while (site != nullptr && site->GetDexPc() == inst->GetDexPc()) {
    if (const auto* env = site->GetEnvironment(); env != nullptr) {
      if (const auto* value = env->GetInstructionAt(phi->GetRegNumber()); value != nullptr) {
        return value;
      }
    }
    site = site->GetPrevious();
  }
  throw std::runtime_error("Missing SSA environment input for a catch phi");
}

class ReferenceLiveness {
 public:
  explicit ReferenceLiveness(const art::HGraph& graph)
      : roots_(graph.GetCurrentInstructionId()), handlers_(roots_.size()) {
    if (graph.HasTryCatch()) {
      FindHandlers(graph);
    }

    // A block's entry set includes its live phi results. Transfer across each
    // edge substitutes only the inputs selected on that edge.
    const auto& blocks = graph.GetReversePostOrder();
    std::vector<LiveSet> entries(graph.GetBlocks().size());
    bool changed;
    do {
      changed = false;
      for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        auto* block = *it;
        auto live = ScanBlock(block, entries, /*record=*/false);
        auto& entry = entries[block->GetBlockId()];
        if (entry != live) {
          entry = std::move(live);
          changed = true;
        }
      }
    } while (changed);

    for (auto* block : blocks) {
      ScanBlock(block, entries, /*record=*/true);
    }
  }

  std::span<const uint32_t> Roots(const art::HInstruction* inst) const {
    return roots_[inst->GetId()];
  }

  std::span<const Handler> Handlers(const art::HInstruction* inst) const {
    return handlers_[inst->GetId()];
  }

  uint32_t MaxRoots() const { return max_roots_; }

 private:
  using LiveSet = std::set<uint32_t>;

  void FindHandlers(const art::HGraph& graph) {
    const auto& dex = graph.GetDexFile();
    const auto method = graph.GetMethodIdx();
    const auto* cls = dex.FindClassDef(dex.GetMethodId(method).class_idx_);
    if (cls == nullptr) {
      throw std::runtime_error("Missing DEX class for a try/catch graph");
    }
    const art::CodeItemDataAccessor code(dex, dex.GetCodeItem(dex.FindCodeItemOffset(*cls, method)));
    for (const auto* block : graph.GetReversePostOrder()) {
      if (!block->IsTryBlock()) {
        continue;
      }
      const auto targets = block->GetTryCatchInformation()->GetTryEntry().GetExceptionHandlers();
      for (const auto* inst = block->GetFirstInstruction(); inst != nullptr; inst = inst->GetNext()) {
        if (!IsSafepoint(inst) || (!inst->CanThrow() && !inst->IsStaticFieldGet())) {
          continue;
        }
        const auto* item = code.FindTryItem(inst->GetDexPc());
        if (item == nullptr) {
          throw std::runtime_error("Missing DEX try item for a throwing SSA instruction");
        }
        // The graph has one edge per handler block. The original DEX keeps
        // handler order and all types for Java multi-catch clauses.
        auto& handlers = handlers_[inst->GetId()];
        for (art::CatchHandlerIterator handler(code, *item); handler.HasNext(); handler.Next()) {
          const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto* candidate) {
            return candidate->GetDexPc() == handler.GetHandlerAddress();
          });
          if (target == targets.end()) {
            throw std::runtime_error("DEX catch target missing from the HGraph");
          }
          handlers.push_back({*target, handler.GetHandlerTypeIndex()});
        }
      }
    }
  }

  template <typename PhiInput>
  static void Transfer(LiveSet& live,
                       const std::vector<LiveSet>& entries,
                       const art::HBasicBlock* target,
                       const PhiInput& input) {
    const auto& entry = entries[target->GetBlockId()];
    auto edge = entry;
    for (const auto* phi = target->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      edge.erase(phi->GetId());
    }
    live.insert(edge.begin(), edge.end());
    for (const auto* phi = target->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      if (entry.contains(phi->GetId())) {
        const auto* value = input(phi->AsPhi());
        if (IsRoot(value)) {
          live.insert(value->GetId());
        }
      }
    }
  }

  LiveSet ScanBlock(art::HBasicBlock* block,
                    const std::vector<LiveSet>& entries,
                    bool record) {
    LiveSet live;
    for (const auto* successor : block->GetNormalSuccessors()) {
      Transfer(live, entries, successor, [&](const art::HPhi* phi) {
        return phi->InputAt(successor->GetPredecessorIndexOf(block));
      });
    }
    for (const auto* inst = block->GetLastInstruction(); inst != nullptr; inst = inst->GetPrevious()) {
      live.erase(inst->GetId());
      if (!inst->IsConstructorFence()) {
        for (const auto* input : inst->GetInputs()) {
          if (IsRoot(input)) {
            live.insert(input->GetId());
          }
        }
      }
      for (const auto& handler : Handlers(inst)) {
        Transfer(live, entries, handler.block,
                 [&](const art::HPhi* phi) { return CatchInput(inst, phi); });
      }
      if (record && IsSafepoint(inst)) {
        roots_[inst->GetId()].assign(live.begin(), live.end());
        max_roots_ = std::max(max_roots_, static_cast<uint32_t>(live.size()));
      }
    }
    return live;
  }

  std::vector<std::vector<uint32_t>> roots_;
  std::vector<std::vector<Handler>> handlers_;
  uint32_t max_roots_ = 0;
};

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
  FunctionEmitter(art::HGraph* graph, ModuleFunctions& module, uint32_t index)
      : HGraphVisitor(graph),
        module_(module),
        signature_(module.methods[index]),
        locals_by_id_(graph->GetCurrentInstructionId(), kUnassigned),
        cases_by_id_(graph->GetBlocks().size(), kUnassigned) {
    if (module_.managed) {
      liveness_.emplace(*graph);
    }
  }

  Bytes Emit() {
    const auto& blocks = GetGraph()->GetReversePostOrder();
    if (blocks.empty() || blocks.front() != GetGraph()->GetEntryBlock()) {
      throw std::runtime_error("Wasm compilation requires the HGraph reverse postorder");
    }

    // ART indexes parameters by incoming DEX register, including wide pairs.
    std::vector<uint32_t> params;
    for (uint32_t index = module_.managed ? 1 : 0; index < signature_.params.size(); ++index) {
      params.push_back(index);
      if (signature_.params[index] == Value::I64 || signature_.params[index] == Value::F64) {
        params.push_back(kUnassigned);
      }
    }

    pc_ = NewLocal(Value::I32);
    if (module_.managed) {
      frame_top_ = NewLocal(Value::I32);
      if (liveness_->MaxRoots() != 0) {
        frame_stack_ = NewLocal(Value::I32);
      }
      scratch_ = NewLocal(Value::I32);
      if (GetGraph()->HasTryCatch()) {
        exception_ = NewLocal(Value::I32);
      }
    }
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
                   !instruction->IsConstant() && !instruction->IsCurrentMethod() &&
                   !instruction->IsLoadClass()) {
          BindLocal(instruction);
        }
      }
    }

    WriteLocals(body_, locals_);
    if (module_.managed) {
      LoadThread(kRefTop);
      body_.Index(LocalSet, frame_top_);
      body_.Open(Block, Value::I32);
      body_.CatchJavaException();
      PrepareRoots();
    }
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
    body_.Byte(End);  // Invalid pc sink.
    body_.Byte(Unreachable);
    body_.Byte(End);  // Dispatch loop.
    body_.Byte(Unreachable);
    if (module_.managed) {
      body_.Byte(End);  // Function-wide try_table.
      body_.Byte(Unreachable);
      body_.Byte(End);  // Exception payload on the stack.
      RestoreRoots();
      body_.Index(Throw, 0);
    }
    body_.Byte(End);
    return std::move(body_);
  }

 private:
  void VisitBasicBlock(art::HBasicBlock* block) override {
    for (auto* inst = block->GetFirstInstruction(); inst != nullptr; inst = inst->GetNext()) {
      if (module_.managed && IsSafepoint(inst) && !inst->IsSuspendCheck()) {
        EmitSafepoint(inst);
      } else {
        if (!module_.managed && inst->GetType() == Type::kReference) {
          VisitInstruction(inst);
        }
        Dispatch(inst);
      }
    }
  }

  void EmitSafepoint(art::HInstruction* inst) {
    const auto handlers = liveness_->Handlers(inst);
    if (!handlers.empty()) {
      body_.Open(Block);  // Normal continuation.
      body_.Open(Block, Value::I32);
      body_.CatchJavaException();
    }

    const auto roots = liveness_->Roots(inst);
    PublishRoots(roots);
    Dispatch(inst);
    if (!roots.empty()) {
      RestoreRoots();
    }
    if (handlers.empty()) {
      return;
    }

    body_.Byte(End);  // try_table
    body_.Index(Br, 1);
    body_.Byte(End);  // Java exception payload.
    body_.Index(LocalSet, exception_);
    RestoreRoots();
    for (const auto& handler : handlers) {
      if (handler.type.IsValid()) {
        body_.Index(LocalGet, 0);
        body_.Index(LocalGet, exception_);
        body_.Int(handler.type.index_);
        body_.Index(Call, IsInstance);
        body_.Open(If);
        EnterHandler(inst, handler.block, dispatch_depth_ + 2);
        body_.Byte(End);
      } else {
        EnterHandler(inst, handler.block, dispatch_depth_ + 1);
        break;
      }
    }
    body_.Index(LocalGet, exception_);
    body_.Index(Throw, 0);
    body_.Byte(End);
  }

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
  void VisitNullConstant(art::HNullConstant*) override {}
  void VisitLoadClass(art::HLoadClass*) override {}  // DEX type metadata in this backend.
  void VisitConstructorFence(art::HConstructorFence*) override {}  // Cooperative mutators.
  void VisitSuspendCheck(art::HSuspendCheck* inst) override {
    if (!module_.managed) {
      return;
    }
    // ART puts the loop check after header phis. Every backedge thus polls
    // with the new iteration's reference values already installed.
    LoadThread(kSuspendFlag);
    body_.Open(If);
    const auto roots = liveness_->Roots(inst);
    PublishRoots(roots);
    body_.Index(LocalGet, 0);
    body_.Index(Call, Safepoint);
    if (!roots.empty()) {
      RestoreRoots();
    }
    body_.Byte(End);
  }
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
    module_.needs_float_rem = true;
    body_.Index(Call, module_.FunctionIndex(module_.methods.size()));
    if (narrow) body_.Byte(F32DemoteF64);
    Write(inst);
  }

  void VisitDivZeroCheck(art::HDivZeroCheck* inst) override {
    Read(inst->InputAt(0));
    body_.Index(LocalTee, Local(inst));
    body_.Byte(WasmType(inst->GetType()) == Value::I64 ? I64Eqz : I32Eqz);
    body_.Open(If);
    if (module_.managed) {
      RaiseException(JavaException::Arithmetic);
    } else {
      body_.Byte(Unreachable);
    }
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

  void VisitTryBoundary(art::HTryBoundary* inst) override {
    Edge(inst->GetBlock(), inst->GetNormalFlowSuccessor());
    body_.Index(Br, dispatch_depth_);
  }

  void VisitPackedSwitch(art::HPackedSwitch* inst) override {
    auto* block = inst->GetBlock();
    for (uint32_t i = 0; i < inst->GetNumEntries(); ++i) {
      Read(inst->InputAt(0));
      body_.Int(std::bit_cast<int32_t>(static_cast<uint32_t>(inst->GetStartValue()) + i));
      body_.Byte(I32Eq);
      body_.Open(If);
      Edge(block, block->GetSuccessors()[i]);
      body_.Index(Br, dispatch_depth_ + 1);
      body_.Byte(End);
    }
    Edge(block, inst->GetDefaultBlock());
    body_.Index(Br, dispatch_depth_);
  }

  void VisitReturn(art::HReturn* inst) override {
    if (WasmType(inst->InputAt(0)->GetType()) != signature_.result) {
      throw std::runtime_error("HReturn disagrees with its DEX signature");
    }
    Read(inst->InputAt(0));
    if (module_.managed) {
      RestoreRoots();
    }
    body_.Byte(Return);
  }

  void VisitReturnVoid(art::HReturnVoid*) override {
    if (signature_.result != Value::Void) {
      throw std::runtime_error("HReturnVoid disagrees with its DEX signature");
    }
    if (module_.managed) {
      RestoreRoots();
    }
    body_.Byte(Return);
  }

  void VisitInvokeStaticOrDirect(art::HInvokeStaticOrDirect* inst) override {
    const auto target = inst->GetResolvedMethodReference();
    if ((!module_.managed && !inst->IsStatic()) || inst->IsIntrinsic() ||
        target.dex_file != &GetGraph()->GetDexFile() ||
        inst->GetClinitCheckRequirement() != art::HInvokeStaticOrDirect::ClinitCheckRequirement::kNone) {
      throw std::runtime_error("Unsupported HInvokeStaticOrDirect target");
    }
    const auto function = module_.indices.find(target.index);
    if (function == module_.indices.end()) {
      if (module_.managed && !inst->IsStatic() && IsBuiltinConstructor(*target.dex_file, target.index)) {
        return;  // The standalone runtime's built-in objects have no constructor-side fields.
      }
      throw std::runtime_error("Missing HGraph for " + target.dex_file->PrettyMethod(target.index));
    }
    const Signature& callee = module_.methods[function->second];
    ReadArguments(inst, callee);
    body_.Index(Call, module_.FunctionIndex(function->second));
    if (callee.result != Value::Void) {
      Write(inst);
    }
  }

  void VisitInvokeVirtual(art::HInvokeVirtual* inst) override { VirtualInvoke(inst); }
  void VisitInvokeInterface(art::HInvokeInterface* inst) override { VirtualInvoke(inst); }

  void VirtualInvoke(art::HInvoke* inst) {
    if (!module_.managed || inst->IsIntrinsic()) {
      VisitInstruction(inst);
    }
    const auto target = inst->GetMethodReference();
    const auto callee = GetSignature(*target.dex_file, target.index, true, false);
    // Resolve through the receiver class's virtual methods, then use the
    // returned DEX method ID to select the typed Wasm function.
    body_.Index(LocalGet, 0);
    Read(inst->InputAt(0));
    body_.Int(target.index);
    body_.Index(Call, VirtualMethod);
    CheckPendingException();
    body_.Index(LocalTee, scratch_);
    body_.Index(TableGet, 0);
    body_.Byte(RefIsNull);
    body_.Open(If);
    RaiseException(JavaException::NoSuchMethod);
    body_.Byte(End);
    ReadArguments(inst, callee);
    body_.Index(LocalGet, scratch_);
    body_.Index(CallIndirect, module_.TypeIndex(callee));
    body_.Uleb(0);  // dispatch_table
    if (callee.result != Value::Void) {
      Write(inst);
    }
  }

  void ReadArguments(const art::HInvoke* inst, const Signature& callee) {
    const size_t first = module_.managed ? 1 : 0;
    if (inst->GetNumberOfArguments() != callee.params.size() - first ||
        WasmType(inst->GetType()) != callee.result) {
      throw std::runtime_error("HInvoke disagrees with its target signature");
    }
    if (module_.managed) {
      body_.Index(LocalGet, 0);
    }
    for (size_t i = 0; i < inst->GetNumberOfArguments(); ++i) {
      if (WasmType(inst->InputAt(i)->GetType()) != callee.params[i + first]) {
        throw std::runtime_error("HInvoke argument type mismatch");
      }
      Read(inst->InputAt(i));
    }
  }

  void VisitNewInstance(art::HNewInstance* inst) override {
    body_.Index(LocalGet, 0);
    body_.Int(inst->GetTypeIndex().index_);
    body_.Index(Call, NewObject);
    CheckPendingException();
    Write(inst);
  }

  void VisitNewArray(art::HNewArray* inst) override {
    body_.Index(LocalGet, 0);
    Read(inst->InputAt(0));  // HLoadClass, represented by a DEX type ID.
    Read(inst->InputAt(1));
    body_.Index(Call, NewArray);
    CheckPendingException();
    Write(inst);
  }

  void VisitLoadString(art::HLoadString* inst) override {
    body_.Index(LocalGet, 0);
    body_.Int(inst->GetStringIndex().index_);
    body_.Index(Call, ConstString);
    CheckPendingException();
    Write(inst);
  }

  void VisitInstanceFieldGet(art::HInstanceFieldGet* inst) override {
    EmitGet(inst, GetField, inst->GetFieldInfo().GetFieldIndex(), inst->InputAt(0));
  }

  void VisitInstanceFieldSet(art::HInstanceFieldSet* inst) override {
    EmitPut(PutField, inst->GetFieldInfo().GetFieldIndex(), inst->InputAt(0), inst->GetValue());
  }

  void VisitStaticFieldGet(art::HStaticFieldGet* inst) override {
    EmitGet(inst, GetStatic, inst->GetFieldInfo().GetFieldIndex(), nullptr);
  }

  void VisitStaticFieldSet(art::HStaticFieldSet* inst) override {
    EmitPut(PutStatic, inst->GetFieldInfo().GetFieldIndex(), nullptr, inst->GetValue());
  }

  void VisitUnresolvedStaticFieldGet(art::HUnresolvedStaticFieldGet* inst) override {
    EmitGet(inst, GetStatic, inst->GetFieldIndex(), nullptr);
  }

  void VisitUnresolvedStaticFieldSet(art::HUnresolvedStaticFieldSet* inst) override {
    EmitPut(PutStatic, inst->GetFieldIndex(), nullptr, inst->InputAt(0));
  }

  void EmitGet(art::HInstruction* inst, Runtime helper, uint32_t field,
               const art::HInstruction* object) {
    body_.Index(LocalGet, 0);
    if (object != nullptr) {
      Read(object);
    }
    body_.Int(field);
    body_.Index(Call, helper);
    CheckPendingException();
    WriteBits(inst);
  }

  void EmitPut(Runtime helper, uint32_t field, const art::HInstruction* object,
               const art::HInstruction* value) {
    body_.Index(LocalGet, 0);
    if (object != nullptr) {
      Read(object);
    }
    body_.Int(field);
    ReadBits(value);
    body_.Index(Call, helper);
    CheckPendingException();
  }

  void VisitArrayLength(art::HArrayLength* inst) override {
    body_.Index(LocalGet, 0);
    Read(inst->InputAt(0));
    body_.Index(Call, ArrayLength);
    CheckPendingException();
    Write(inst);
  }

  void VisitArrayGet(art::HArrayGet* inst) override {
    body_.Index(LocalGet, 0);
    Read(inst->GetArray());
    Read(inst->GetIndex());
    body_.Index(Call, ArrayGet);
    CheckPendingException();
    WriteBits(inst);
  }

  void VisitArraySet(art::HArraySet* inst) override {
    body_.Index(LocalGet, 0);
    Read(inst->GetArray());
    Read(inst->GetIndex());
    ReadBits(inst->GetValue());
    body_.Index(Call, ArrayPut);
    CheckPendingException();
  }

  void VisitNullCheck(art::HNullCheck* inst) override {
    Read(inst->InputAt(0));
    body_.Index(LocalTee, Local(inst));
    body_.Byte(I32Eqz);
    body_.Open(If);
    RaiseException(JavaException::NullPointer);
    body_.Byte(End);
  }

  void VisitBoundsCheck(art::HBoundsCheck* inst) override {
    Read(inst->InputAt(0));
    body_.Index(LocalTee, Local(inst));
    Read(inst->InputAt(1));
    body_.Byte(I32GeU);  // Negative Java indices also fail the unsigned comparison.
    body_.Open(If);
    RaiseException(JavaException::ArrayBounds);
    body_.Byte(End);
  }

  void VisitInstanceOf(art::HInstanceOf* inst) override {
    EmitIsInstance(inst);
    Write(inst);
  }

  void VisitCheckCast(art::HCheckCast* inst) override {
    Read(inst->InputAt(0));
    body_.Open(If);  // A null reference always passes check-cast.
    EmitIsInstance(inst);
    body_.Byte(I32Eqz);
    body_.Open(If);
    RaiseException(JavaException::ClassCast);
    body_.Byte(End);
    body_.Byte(End);
  }

  void EmitIsInstance(art::HTypeCheckInstruction* inst) {
    body_.Index(LocalGet, 0);
    Read(inst->InputAt(0));
    Read(inst->InputAt(1));
    body_.Index(Call, IsInstance);
  }

  void VisitThrow(art::HThrow* inst) override {
    Read(inst->InputAt(0));
    body_.Byte(I32Eqz);
    body_.Open(If);
    RaiseException(JavaException::NullPointer);
    body_.Byte(End);
    body_.Index(LocalGet, 0);
    Read(inst->InputAt(0));
    body_.Memory(I32Store, kPendingException);
    Read(inst->InputAt(0));
    body_.Index(Throw, 0);
  }

  void VisitLoadException(art::HLoadException* inst) override {
    body_.Index(LocalGet, exception_);
    Write(inst);
  }

  void VisitClearException(art::HClearException*) override {
    body_.Int(0);
    body_.Index(LocalSet, exception_);
  }

  void LoadThread(uint32_t offset) {
    body_.Index(LocalGet, 0);
    body_.Memory(I32Load, offset);
  }

  void RaiseException(JavaException kind) {
    body_.Index(LocalGet, 0);
    body_.Int(static_cast<uint32_t>(kind));
    body_.Index(Call, Raise);
    body_.Index(Throw, 0);
  }

  void CheckPendingException() {
    LoadThread(kPendingException);
    body_.Index(LocalTee, scratch_);
    body_.Open(If);
    body_.Index(LocalGet, scratch_);
    body_.Index(Throw, 0);
    body_.Byte(End);
  }

  void PrepareRoots() {
    const uint32_t count = liveness_->MaxRoots();
    if (count == 0) {
      return;
    }
    body_.Index(LocalGet, frame_top_);
    LoadThread(kRefCapacity);
    body_.Byte(I32GtU);
    LoadThread(kRefCapacity);
    body_.Index(LocalGet, frame_top_);
    body_.Byte(I32Sub);
    body_.Int(count);
    body_.Byte(I32LtU);
    body_.Byte(I32Or);
    body_.Open(If);
    RaiseException(JavaException::StackOverflow);
    body_.Byte(End);

    LoadThread(kRefStack);
    body_.Index(LocalGet, frame_top_);
    body_.Int(2);
    body_.Byte(I32Shl);
    body_.Byte(I32Add);
    body_.Index(LocalSet, frame_stack_);
  }

  void PublishRoots(std::span<const uint32_t> roots) {
    if (roots.empty()) {
      return;
    }
    for (uint32_t i = 0; i < roots.size(); ++i) {
      body_.Index(LocalGet, frame_stack_);
      body_.Index(LocalGet, locals_by_id_.at(roots[i]));
      body_.Memory(I32Store, i * sizeof(uint32_t));
    }
    body_.Index(LocalGet, 0);
    body_.Index(LocalGet, frame_top_);
    body_.Int(roots.size());
    body_.Byte(I32Add);
    body_.Memory(I32Store, kRefTop);
  }

  void RestoreRoots() {
    body_.Index(LocalGet, 0);
    body_.Index(LocalGet, frame_top_);
    body_.Memory(I32Store, kRefTop);
  }

  void EnterHandler(const art::HInstruction* site, const art::HBasicBlock* handler, uint32_t depth) {
    // Read catch-phi inputs from the throwing DEX instruction's environment.
    for (const auto* phi = handler->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      const auto* input = CatchInput(site, phi->AsPhi());
      Read(input);
      const auto from = WasmType(input->GetType());
      const auto to = WasmType(phi->GetType());
      if (from != to) {
        Reinterpret(from, to);
      }
    }
    for (const auto* phi = handler->GetLastPhi(); phi != nullptr; phi = phi->GetPrevious()) {
      Write(phi);
    }
    body_.Index(LocalGet, 0);
    body_.Int(0);
    body_.Memory(I32Store, kPendingException);
    SetCase(handler);
    body_.Index(Br, depth);
  }

  void Reinterpret(Value from, Value to) {
    if (from == Value::I32 && to == Value::F32) {
      body_.Byte(F32ReinterpretI32);
    } else if (from == Value::F32 && to == Value::I32) {
      body_.Byte(I32ReinterpretF32);
    } else if (from == Value::I64 && to == Value::F64) {
      body_.Byte(F64ReinterpretI64);
    } else if (from == Value::F64 && to == Value::I64) {
      body_.Byte(I64ReinterpretF64);
    } else {
      throw std::runtime_error("Incompatible types for a DEX register");
    }
  }

  void ReadBits(const art::HInstruction* inst) {
    Read(inst);
    switch (WasmType(inst->GetType())) {
      case Value::F32:
        body_.Byte(I32ReinterpretF32);
        [[fallthrough]];
      case Value::I32: body_.Byte(I64ExtendI32U); break;
      case Value::F64: body_.Byte(I64ReinterpretF64); break;
      default: break;
    }
  }

  void WriteBits(art::HInstruction* inst) {
    switch (WasmType(inst->GetType())) {
      case Value::I32: body_.Byte(I32WrapI64); break;
      case Value::F32: body_.Byte(I32WrapI64); body_.Byte(F32ReinterpretI32); break;
      case Value::F64: body_.Byte(F64ReinterpretI64); break;
      default: break;
    }
    Write(inst);
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
    } else if (inst->IsNullConstant()) {
      body_.Int(0);
    } else if (inst->IsLoadClass()) {
      body_.Int(inst->AsLoadClass()->GetTypeIndex().index_);
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
    SetCase(to);
  }

  void SetCase(const art::HBasicBlock* to) {
    const uint32_t target = cases_by_id_.at(to->GetBlockId());
    if (target == kUnassigned) {
      throw std::runtime_error("SSA edge leaves the Wasm dispatch table");
    }
    body_.Int(std::bit_cast<int32_t>(target));
    body_.Index(LocalSet, pc_);
  }

  ModuleFunctions& module_;
  const Signature& signature_;
  std::optional<ReferenceLiveness> liveness_;
  std::vector<uint32_t> locals_by_id_;
  std::vector<uint32_t> cases_by_id_;
  std::vector<Value> locals_;
  Bytes body_;
  uint32_t pc_ = 0;
  uint32_t dispatch_depth_ = 0;
  uint32_t frame_top_ = kUnassigned;
  uint32_t frame_stack_ = kUnassigned;
  uint32_t scratch_ = kUnassigned;
  uint32_t exception_ = kUnassigned;
};

std::vector<uint8_t> CodeGeneratorWasm::Compile(std::span<const WasmMethod> methods) {
  return CompileModule(methods, /*managed=*/false);
}

std::vector<uint8_t> CodeGeneratorWasm::CompileObjects(std::span<const WasmMethod> methods) {
  return CompileModule(methods, /*managed=*/true);
}

std::vector<uint8_t> CodeGeneratorWasm::CompileModule(std::span<const WasmMethod> methods,
                                                       bool managed) {
  if (methods.empty() || methods.front().graph == nullptr) {
    throw std::invalid_argument("Wasm compilation needs at least one HGraph");
  }
  const art::DexFile& dex = methods.front().graph->GetDexFile();
  ModuleFunctions context(managed);
  std::unordered_set<std::string_view> names;
  context.methods.reserve(methods.size());
  for (const WasmMethod& method : methods) {
    if (method.graph == nullptr || &method.graph->GetDexFile() != &dex ||
        method.graph->GetMethodIdx() >= dex.NumMethodIds() ||
        !method.graph->IsInSsaForm() ||
        (!managed && method.graph->GetInvokeType() != art::kStatic)) {
      throw std::invalid_argument("Wasm compilation requires compatible SSA graphs from one DEX");
    }
    if (!context.indices.emplace(method.graph->GetMethodIdx(), context.methods.size()).second ||
        ((!managed || !method.export_name.empty()) && !names.insert(method.export_name).second)) {
      throw std::invalid_argument("Duplicate Wasm method or export name");
    }
    context.methods.push_back(GetSignature(dex, method.graph->GetMethodIdx(), managed,
                                           method.graph->GetInvokeType() == art::kStatic));
  }
  context.types = context.methods;

  Bytes imports;
  if (managed) {
    imports.Uleb(std::size(kRuntime) + 3);
    const auto import = [&](std::string_view name, uint8_t kind) {
      imports.Name("env");
      imports.Name(name);
      imports.Byte(kind);
    };
    import("memory", 2);
    imports.Byte(0);  // Minimum only.
    imports.Uleb(1);
    import("dispatch_table", 1);
    imports.Byte(0x70);  // funcref
    imports.Byte(0);
    imports.Uleb(dex.NumMethodIds());
    import("java_exception", 4);
    imports.Byte(0);  // Exception tag.
    imports.Uleb(context.TypeIndex({{Value::I32}, Value::Void}));
    for (const auto& helper : kRuntime) {
      import(helper.name, 0);
      imports.Uleb(context.TypeIndex({helper.params, helper.result}));
    }
  }

  std::vector<Bytes> bodies;
  Bytes function_types;
  for (uint32_t index = 0; index < methods.size(); ++index) {
    try {
      FunctionEmitter emitter(methods[index].graph, context, index);
      bodies.push_back(emitter.Emit());
    } catch (const std::runtime_error& error) {
      throw std::runtime_error(dex.PrettyMethod(methods[index].graph->GetMethodIdx()) +
                               ": " + error.what());
    }
    function_types.Uleb(index);
  }
  if (context.needs_float_rem) {
    function_types.Uleb(context.types.size());
    context.types.push_back({{Value::F64, Value::F64}, Value::F64});
    bodies.push_back(EmitFloatRemainder());
  }

  Bytes types, functions, exports, elements, code;
  types.Uleb(context.types.size());
  functions.Uleb(bodies.size());
  functions.Append(function_types);
  exports.Uleb(names.size());
  code.Uleb(bodies.size());
  for (const auto& signature : context.types) {
    types.Byte(0x60);  // Function type.
    types.Uleb(signature.params.size());
    for (Value type : signature.params) {
      types.Type(type);
    }
    types.Byte(signature.result == Value::Void ? 0 : 1);
    if (signature.result != Value::Void) {
      types.Type(signature.result);
    }
  }
  for (const auto& body : bodies) {
    code.Payload(body);
  }
  if (managed) {
    elements.Uleb(methods.size());
  }
  for (uint32_t index = 0; index < methods.size(); ++index) {
    const uint32_t function = context.FunctionIndex(index);
    if (!managed || !methods[index].export_name.empty()) {
      exports.Name(methods[index].export_name);
      exports.Byte(0);  // Function export.
      exports.Uleb(function);
    }
    if (managed) {
      elements.Uleb(0);  // Active element segment in dispatch_table.
      elements.Int(methods[index].graph->GetMethodIdx());
      elements.Byte(End);
      elements.Uleb(1);
      elements.Uleb(function);
    }
  }

  Bytes module{{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}};
  module.Section(1, types);
  if (managed) {
    module.Section(2, imports);
  }
  module.Section(3, functions);
  module.Section(7, exports);
  if (managed) {
    module.Section(9, elements);
  }
  module.Section(10, code);
  return module.data;
}

}  // namespace wa::hgraph
