#include "runtime_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_instruction-inl.h"
#include "interpreter/safe_math.h"

namespace wa::interp {

InvokeRegisters::InvokeRegisters(const art::Instruction& instruction, bool range) : range(range) {
  if (range) {
    count = instruction.VRegA_3rc();
    first = instruction.VRegC_3rc();
  } else {
    count = instruction.VRegA_35c();
    if (count <= registers.size()) instruction.GetVarArgs(registers.data());
  }
}

namespace {

using I = art::Instruction;
using art::interpreter::SafeAdd;
using art::interpreter::SafeMul;
using art::interpreter::SafeSub;

std::span<Value> ReserveRegisters(Runtime& runtime, size_t count, std::vector<Value>& overflow) {
  if (count > runtime.frame_registers.size() - runtime.register_top) {
    overflow.resize(count);
    return overflow;
  }
  auto* base = runtime.frame_registers.data() + runtime.register_top;
  std::fill_n(base, count, Value{});
  runtime.register_top += count;
  return {base, count};
}

class ShadowFrame {
 public:
  ShadowFrame(Runtime& runtime, size_t registers, Class& declaring_class)
      : saved_top_(runtime.register_top), slots_(ReserveRegisters(runtime, registers + 2, overflow_)),
        registers_(registers), roots_(runtime, slots_) {
    roots_.declaring_class = &declaring_class;
  }

  ~ShadowFrame() {
    for (Value monitor : roots_.values) --monitor.ref()->monitor_depth;
    roots_.runtime.register_top = saved_top_;
  }

  Value& Reg(size_t i) { return slots_[i]; }

  Value Wide(size_t i) {
    CheckWide(i);
    return Value::Bits(Reg(i).bits() | (Reg(i + 1).bits() << 32));
  }

  void SetWide(size_t i, Value value) {
    CheckWide(i);
    Reg(i) = Value::Bits(static_cast<uint32_t>(value.bits()));
    Reg(i + 1) = Value::Bits(value.bits() >> 32);
  }

  Value Read(size_t i, char type) { return IsWide(type) ? Wide(i) : Normalize(Reg(i), type); }
  void Write(size_t i, Value value, char type) {
    if (IsWide(type)) SetWide(i, value);
    else Reg(i) = Normalize(value, type);
  }

  Value& Result() { return slots_[registers_]; }
  Value& Caught() { return slots_[registers_ + 1]; }

  void EnterMonitor(Object& object) {
    roots_.Hold(&object);
    ++object.monitor_depth;
  }

  void ExitMonitor(Object& object) {
    for (auto it = roots_.values.end(); it != roots_.values.begin();) {
      if ((--it)->ref() == &object) {
        --object.monitor_depth;
        roots_.values.erase(it);
        return;
      }
    }
    roots_.runtime.Fail("Ljava/lang/IllegalMonitorStateException;");
  }

 private:
  void CheckWide(size_t i) {
    // Field and array component types are resolved from the object at runtime.
    if (i + 1 >= registers_) roots_.runtime.Fail("Ljava/lang/VerifyError;", "Invalid wide register");
  }

  size_t saved_top_;
  std::vector<Value> overflow_;
  std::span<Value> slots_;
  size_t registers_;
  RootScope roots_;
};

enum class Binary { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr, Ushr };

template<class T> T Integral(Runtime& runtime, Binary op, T lhs, T rhs) {
  using Unsigned = std::make_unsigned_t<T>;
  const auto shift = static_cast<unsigned>(rhs) & (sizeof(T) * 8 - 1);
  switch (op) {
    case Binary::Add: return SafeAdd(lhs, rhs);
    case Binary::Sub: return SafeSub(lhs, rhs);
    case Binary::Mul: return SafeMul(lhs, rhs);
    case Binary::Div:
    case Binary::Rem:
      if (rhs == 0) runtime.Fail("Ljava/lang/ArithmeticException;", "/ by zero");
      if (lhs == std::numeric_limits<T>::min() && rhs == -1) return op == Binary::Div ? lhs : 0;
      return op == Binary::Div ? lhs / rhs : lhs % rhs;
    case Binary::And: return lhs & rhs;
    case Binary::Or: return lhs | rhs;
    case Binary::Xor: return lhs ^ rhs;
    case Binary::Shl: return static_cast<T>(static_cast<Unsigned>(lhs) << shift);
    case Binary::Shr: return lhs >> shift;
    case Binary::Ushr: return static_cast<T>(static_cast<Unsigned>(lhs) >> shift);
  }
  std::unreachable();
}

template<class T> T Floating(Binary op, T lhs, T rhs) {
  switch (op) {
    case Binary::Add: return lhs + rhs;
    case Binary::Sub: return lhs - rhs;
    case Binary::Mul: return lhs * rhs;
    case Binary::Div: return lhs / rhs;
    case Binary::Rem: return std::fmod(lhs, rhs);
    default: std::unreachable();
  }
}

template<class Int, class Float> Int FloatToInt(Float value) {
  if (std::isnan(value)) return 0;
  if (value >= static_cast<Float>(std::numeric_limits<Int>::max())) return std::numeric_limits<Int>::max();
  if (value <= static_cast<Float>(std::numeric_limits<Int>::min())) return std::numeric_limits<Int>::min();
  return static_cast<Int>(value);
}

template<class T> int Compare(T lhs, T rhs, int nan = 0) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(lhs) || std::isnan(rhs)) return nan;
  }
  return (lhs > rhs) - (lhs < rhs);
}

void Unary(ShadowFrame& f, const I& insn) {
  const auto dest = insn.VRegA_12x(), src = insn.VRegB_12x();
  switch (insn.Opcode()) {
    case I::NEG_INT: f.Reg(dest) = Value::Int(SafeSub(0, f.Reg(src).i())); break;
    case I::NOT_INT: f.Reg(dest) = Value::Int(~f.Reg(src).i()); break;
    case I::NEG_LONG: f.SetWide(dest, Value::Long(SafeSub(int64_t{0}, f.Wide(src).j()))); break;
    case I::NOT_LONG: f.SetWide(dest, Value::Long(~f.Wide(src).j())); break;
    case I::NEG_FLOAT: f.Reg(dest) = Value::Float(-f.Reg(src).f()); break;
    case I::NEG_DOUBLE: f.SetWide(dest, Value::Double(-f.Wide(src).d())); break;
    case I::INT_TO_LONG: f.SetWide(dest, Value::Long(f.Reg(src).i())); break;
    case I::INT_TO_FLOAT: f.Reg(dest) = Value::Float(f.Reg(src).i()); break;
    case I::INT_TO_DOUBLE: f.SetWide(dest, Value::Double(f.Reg(src).i())); break;
    case I::LONG_TO_INT: f.Reg(dest) = Value::Int(f.Reg(src).i()); break;
    case I::LONG_TO_FLOAT: f.Reg(dest) = Value::Float(f.Wide(src).j()); break;
    case I::LONG_TO_DOUBLE: f.SetWide(dest, Value::Double(f.Wide(src).j())); break;
    case I::FLOAT_TO_INT: f.Reg(dest) = Value::Int(FloatToInt<int32_t>(f.Reg(src).f())); break;
    case I::FLOAT_TO_LONG: f.SetWide(dest, Value::Long(FloatToInt<int64_t>(f.Reg(src).f()))); break;
    case I::FLOAT_TO_DOUBLE: f.SetWide(dest, Value::Double(f.Reg(src).f())); break;
    case I::DOUBLE_TO_INT: f.Reg(dest) = Value::Int(FloatToInt<int32_t>(f.Wide(src).d())); break;
    case I::DOUBLE_TO_LONG: f.SetWide(dest, Value::Long(FloatToInt<int64_t>(f.Wide(src).d()))); break;
    case I::DOUBLE_TO_FLOAT: f.Reg(dest) = Value::Float(f.Wide(src).d()); break;
    case I::INT_TO_BYTE: f.Reg(dest) = Normalize(f.Reg(src), 'B'); break;
    case I::INT_TO_CHAR: f.Reg(dest) = Normalize(f.Reg(src), 'C'); break;
    case I::INT_TO_SHORT: f.Reg(dest) = Normalize(f.Reg(src), 'S'); break;
    default: std::unreachable();
  }
}

void Arithmetic(Runtime& runtime, ShadowFrame& f, const I& insn) {
  int opcode = insn.Opcode();
  if (opcode >= I::ADD_INT_LIT16) {
    const bool lit8 = opcode >= I::ADD_INT_LIT8;
    const auto op = static_cast<Binary>(opcode - (lit8 ? I::ADD_INT_LIT8 : I::ADD_INT_LIT16));
    int32_t lhs = f.Reg(lit8 ? insn.VRegB_22b() : insn.VRegB_22s()).i();
    int32_t rhs = lit8 ? insn.VRegC_22b() : insn.VRegC_22s();
    if (op == Binary::Sub) std::swap(lhs, rhs);
    f.Reg(lit8 ? insn.VRegA_22b() : insn.VRegA_22s()) = Value::Int(Integral(runtime, op, lhs, rhs));
    return;
  }

  const bool two = opcode >= I::ADD_INT_2ADDR;
  const auto dest = two ? insn.VRegA_12x() : insn.VRegA_23x();
  const auto left = two ? dest : insn.VRegB_23x();
  const auto right = two ? insn.VRegB_12x() : insn.VRegC_23x();
  if (two) opcode -= I::ADD_INT_2ADDR - I::ADD_INT;

  if (opcode < I::ADD_LONG) {
    const auto op = static_cast<Binary>(opcode - I::ADD_INT);
    f.Reg(dest) = Value::Int(Integral(runtime, op, f.Reg(left).i(), f.Reg(right).i()));
  } else if (opcode < I::ADD_FLOAT) {
    const auto op = static_cast<Binary>(opcode - I::ADD_LONG);
    const int64_t rhs = op >= Binary::Shl ? f.Reg(right).i() : f.Wide(right).j();
    f.SetWide(dest, Value::Long(Integral(runtime, op, f.Wide(left).j(), rhs)));
  } else if (opcode < I::ADD_DOUBLE) {
    f.Reg(dest) = Value::Float(Floating(static_cast<Binary>(opcode - I::ADD_FLOAT),
                                      f.Reg(left).f(), f.Reg(right).f()));
  } else {
    f.SetWide(dest, Value::Double(Floating(static_cast<Binary>(opcode - I::ADD_DOUBLE),
                                         f.Wide(left).d(), f.Wide(right).d())));
  }
}

bool Branch(I::Code op, Value lhs, Value rhs) {
  if (op >= I::IF_EQZ) op = static_cast<I::Code>(op - I::IF_EQZ + I::IF_EQ);
  switch (op) {
    case I::IF_EQ: return lhs.ref() == rhs.ref() && lhs.i() == rhs.i();
    case I::IF_NE: return lhs.ref() != rhs.ref() || lhs.i() != rhs.i();
    case I::IF_LT: return lhs.i() < rhs.i();
    case I::IF_GE: return lhs.i() >= rhs.i();
    case I::IF_GT: return lhs.i() > rhs.i();
    case I::IF_LE: return lhs.i() <= rhs.i();
    default: std::unreachable();
  }
}

size_t CodeWidth(const art::CodeItemDataAccessor& code, uint32_t pc) {
  const auto& instruction = code.InstructionAt(pc);
  const size_t remaining = code.InsnsSizeInCodeUnits() - pc;
  switch (instruction.Fetch16(0)) {
    case I::kPackedSwitchSignature:
    case I::kSparseSwitchSignature:
      if (remaining < 2) return 0;
      break;
    case I::kArrayDataSignature: {
      if (remaining < 4) return 0;
      const uint32_t count = instruction.Fetch16(2) | (uint32_t{instruction.Fetch16(3)} << 16);
      const uint64_t bytes = uint64_t{count} * instruction.Fetch16(1);
      if (bytes >= UINT32_MAX || (bytes + 1) / 2 > remaining - 4) return 0;
      break;
    }
  }
  const size_t width = instruction.SizeInCodeUnits();
  return width <= remaining ? width : 0;
}

const I& PayloadAt(Runtime& runtime, const art::CodeItemDataAccessor& code, uint32_t pc, uint16_t signature) {
  if (pc >= code.InsnsSizeInCodeUnits() || (pc & 1) != 0 || code.Insns()[pc] != signature || !CodeWidth(code, pc)) {
    runtime.Fail("Ljava/lang/VerifyError;", "Invalid DEX payload");
  }
  return code.InstructionAt(pc);
}

int32_t SwitchOffset(const I& table, I::Code op, int32_t key) {
  // Offsets in ART's payload structures are relative to the switch instruction.
  if (op == I::PACKED_SWITCH) {
    const auto& payload = *reinterpret_cast<const I::PackedSwitchPayload*>(&table);
    const int64_t index = static_cast<int64_t>(key) - payload.first_key;
    if (index >= 0 && index < payload.case_count) return payload.targets[index];
  } else {
    const auto& payload = *reinterpret_cast<const I::SparseSwitchPayload*>(&table);
    const int32_t* keys = payload.GetKeys();
    const int32_t* end = keys + payload.case_count;
    const int32_t* match = std::lower_bound(keys, end, key);
    if (match != end && *match == key) return payload.GetTargets()[match - keys];
  }
  return I::SizeInCodeUnits(I::k31t);
}

void FillArray(Runtime& runtime, Object& array, const I& table) {
  const auto& payload = *reinterpret_cast<const I::ArrayDataPayload*>(&table);
  if (!array.type->IsArray() || !array.type->component->IsPrimitive()) {
    runtime.Fail("Ljava/lang/VerifyError;", "Array payload requires a primitive array");
  }
  const char type = array.type->component->descriptor.front();
  unsigned width = 4;
  if (type == 'Z' || type == 'B') width = 1;
  if (type == 'S' || type == 'C') width = 2;
  if (IsWide(type)) width = 8;
  if (width != payload.element_width) runtime.Fail("Ljava/lang/VerifyError;", "Array payload width");
  if (payload.element_count > array.elements.size()) runtime.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
  static_assert(std::endian::native == std::endian::little);
  for (size_t i = 0; i < payload.element_count; ++i) {
    uint64_t bits = 0;
    std::memcpy(&bits, payload.data + i * width, width);
    array.elements[i] = Normalize(Value::Bits(bits), type);
  }
}

bool IsInvoke(uint8_t opcode) {
  return (opcode >= I::INVOKE_VIRTUAL && opcode <= I::INVOKE_INTERFACE) ||
         (opcode >= I::INVOKE_VIRTUAL_RANGE && opcode <= I::INVOKE_INTERFACE_RANGE);
}

bool IsMemberAccess(uint8_t opcode) {
  return (opcode >= I::CONST_STRING && opcode <= I::CONST_CLASS) ||
         (opcode >= I::CHECK_CAST && opcode <= I::INSTANCE_OF) ||
         (opcode >= I::NEW_INSTANCE && opcode <= I::FILLED_NEW_ARRAY_RANGE) ||
         (opcode >= I::IGET && opcode <= I::SPUT_SHORT);
}

[[gnu::noinline]] Method* ResolveInvoke(Runtime& runtime, const Method& caller, const I& insn,
                                       I::Code op, Object* receiver, InvokeCache* cache) {
  const bool is_static = op == I::INVOKE_STATIC;
  Class* owner = cache ? cache->owner : nullptr;
  Method* target = cache ? cache->declaration : nullptr;
  if (!target) {
    const bool range = insn.Opcode() >= I::INVOKE_VIRTUAL_RANGE;
    const auto& dex = *caller.owner->dex;
    const auto index = range ? insn.VRegB_3rc() : insn.VRegB_35c();
    if (index >= dex.NumMethodIds()) runtime.Fail("Ljava/lang/VerifyError;", "Bad DEX method index");
    const auto& id = dex.GetMethodId(index);
    owner = &runtime.linker.Resolve(dex.GetMethodDeclaringClassDescriptorView(id));
    target = &runtime.linker.FindMethod(*owner, dex.GetMethodNameView(id),
                                        dex.GetMethodSignature(id).ToString(), is_static);
    if (cache) {
      cache->owner = owner;
      cache->declaration = target;
    }
  }
  if (receiver && !runtime.linker.IsAssignable(*owner, *receiver->type)) {
    runtime.Fail("Ljava/lang/IncompatibleClassChangeError;", target->QualifiedName());
  }
  if (op == I::INVOKE_VIRTUAL || op == I::INVOKE_INTERFACE) {
    target = &runtime.linker.VirtualMethod(*receiver->type, *target);
  } else if (op == I::INVOKE_SUPER && !owner->IsInterface()) {
    if (!caller.owner->super) runtime.Fail("Ljava/lang/NoSuchMethodError;", target->QualifiedName());
    target = &runtime.linker.VirtualMethod(*caller.owner->super, *target);
  }
  if (cache && receiver) {
    cache->receivers[cache->replacement++ % cache->receivers.size()] = {receiver->type, target};
  }
  return target;
}

Value InvokeInstruction(Runtime& runtime, ShadowFrame& f, const Method& caller, const I& insn,
                        uint8_t opcode, const InvokeRegisters& registers, InvokeCache* cache) {
  if (!registers.range && registers.size() > I::kMaxVarArgRegs) {
    runtime.Fail("Ljava/lang/VerifyError;", "Invalid invoke argument count");
  }
  const auto op = static_cast<I::Code>(opcode - (registers.range ? I::INVOKE_VIRTUAL_RANGE - I::INVOKE_VIRTUAL : 0));
  Object* receiver = nullptr;
  if (op != I::INVOKE_STATIC) {
    if (registers.size() == 0) runtime.Fail("Ljava/lang/VerifyError;", "Missing receiver");
    receiver = &runtime.NonNull(f.Reg(registers[0]).ref());
  }
  Method* target = nullptr;
  if (cache && cache->declaration) {
    if (!receiver) {
      target = cache->declaration;
    } else {
      for (const auto& entry : cache->receivers) {
        if (entry.receiver == receiver->type) {
          target = entry.target;
          break;
        }
      }
    }
  }
  if (!target) target = ResolveInvoke(runtime, caller, insn, op, receiver, cache);
  const size_t count = target->shorty.size() - 1;
  size_t pos = receiver ? 1 : 0;
  if (count == 0 && pos == registers.size()) return runtime.Invoke(*target, receiver, {});
  if (count == 1 && pos + 1 == registers.size() && !IsWide(target->shorty[1])) {
    const std::array arg{f.Read(registers[pos], target->shorty[1])};
    return runtime.Invoke(*target, receiver, arg);
  }
  std::array<Value, I::kMaxVarArgRegs> local;
  std::vector<Value> overflow(count > local.size() ? count : 0);
  const std::span<Value> args(overflow.empty() ? local.data() : overflow.data(), count);
  for (size_t i = 0; i < count; ++i) {
    const char type = target->shorty[i + 1];
    const size_t words = IsWide(type) ? 2 : 1;
    if (pos + words > registers.size() || (words == 2 && registers[pos + 1] != registers[pos] + 1)) {
      runtime.Fail("Ljava/lang/VerifyError;", "Invalid invoke registers for " + target->QualifiedName());
    }
    args[i] = f.Read(registers[pos], type);
    pos += words;
  }
  if (pos != registers.size()) runtime.Fail("Ljava/lang/VerifyError;", "Excess invoke registers");
  return runtime.Invoke(*target, receiver, args);
}

[[gnu::noinline]] Field* ResolveField(Runtime& runtime, const art::DexFile& dex, uint16_t index,
                                     bool is_static, MemberCache* cache) {
  if (index >= dex.NumFieldIds()) runtime.Fail("Ljava/lang/VerifyError;", "Bad DEX field index");
  const auto& id = dex.GetFieldId(index);
  Class& owner = runtime.linker.Resolve(dex.GetFieldDeclaringClassDescriptorView(id));
  auto* field = &runtime.linker.FindField(owner, dex.GetFieldNameView(id), dex.GetFieldTypeDescriptorView(id), is_static);
  if (cache) cache->resolved = field;
  return field;
}

template<bool is_static>
void FieldInstruction(Runtime& runtime, ShadowFrame& f, const art::DexFile& dex, const I& insn,
                      uint8_t op, MemberCache* cache) {
  const bool is_put = is_static ? op >= I::SPUT : op >= I::IPUT;
  const auto reg = is_static ? insn.VRegA_21c() : insn.VRegA_22c();
  Field* resolved = cache ? static_cast<Field*>(cache->resolved) : nullptr;
  if (!resolved) {
    const auto index = is_static ? insn.VRegB_21c() : insn.VRegC_22c();
    resolved = ResolveField(runtime, dex, index, is_static, cache);
  }
  Field& field = *resolved;
  Value* storage = &field.value;
  if constexpr (is_static) {
    if (field.owner->init_state != InitState::Initialized) runtime.linker.Initialize(*field.owner);
  } else {
    Object& object = runtime.NonNull(f.Reg(insn.VRegB_22c()).ref());
    if (field.owner != object.type && (!cache || cache->receiver != object.type)) {
      if (!runtime.linker.IsAssignable(*field.owner, *object.type)) {
        runtime.Fail("Ljava/lang/IncompatibleClassChangeError;", field.name);
      }
      if (cache) cache->receiver = object.type;
    }
    storage = &object.fields[field.slot];
  }
  if (is_put) *storage = f.Read(reg, field.value_type);
  else f.Write(reg, *storage, field.value_type);
}

bool FindHandler(Runtime& runtime, ShadowFrame& f, const art::DexFile& dex,
                 const art::CodeItemDataAccessor& code, uint32_t& pc) {
  const art::dex::TryItem* item = code.TriesSize() ? code.FindTryItem(pc) : nullptr;
  if (!item) return false;
  for (art::CatchHandlerIterator handler(code, *item); handler.HasNext(); handler.Next()) {
    const auto type = handler.GetHandlerTypeIndex();
    if (!type.IsValid() || runtime.linker.IsAssignable(runtime.linker.Resolve(dex.GetTypeDescriptorView(type)),
                                                       *runtime.exception->type)) {
      pc = handler.GetHandlerAddress();
      if (pc < code.InsnsSizeInCodeUnits() && code.InstructionAt(pc).Opcode() == I::MOVE_EXCEPTION) {
        f.Caught() = Value::Ref(runtime.exception);
      }
      runtime.exception = nullptr;
      return true;
    }
  }
  return false;
}

}  // namespace

MethodCodeCache::MethodCodeCache(const art::DexFile& dex, const art::dex::CodeItem* item)
    : code(dex, item) {}

void MethodCodeCache::VerifyRegisters(Runtime& runtime, const Method& method) {
  size_t input_slots = !method.IsStatic();
  for (char type : std::string_view(method.shorty).substr(1)) input_slots += IsWide(type) ? 2 : 1;
  const uint32_t registers = code.RegistersSize();
  if (input_slots != code.InsSize() || input_slots > registers) {
    runtime.Fail("Ljava/lang/VerifyError;", "Invalid code item arguments");
  }

  const auto check = [&](uint32_t reg, bool wide) {
    if (reg >= registers || (wide && reg + 1 >= registers)) {
      runtime.Fail("Ljava/lang/VerifyError;", "Invalid DEX register");
    }
  };
  instruction_widths.resize(code.InsnsSizeInCodeUnits());
  for (uint32_t pc = 0; pc < code.InsnsSizeInCodeUnits();) {
    const I& insn = code.InstructionAt(pc);
    const size_t width = CodeWidth(code, pc);
    if (!width) runtime.Fail("Ljava/lang/VerifyError;", "Truncated DEX instruction");
    const auto op = insn.Opcode();
    if (op != I::NOP || insn.Fetch16(0) == 0) {
      const auto flags = I::VerifyFlagsOf(op);
      if (flags & (I::kVerifyRegA | I::kVerifyRegAWide)) check(insn.VRegA(), flags & I::kVerifyRegAWide);
      if (flags & (I::kVerifyRegB | I::kVerifyRegBWide)) check(insn.VRegB(), flags & I::kVerifyRegBWide);
      if (flags & (I::kVerifyRegC | I::kVerifyRegCWide)) check(insn.VRegC(), flags & I::kVerifyRegCWide);
      if (flags & (I::kVerifyVarArg | I::kVerifyVarArgNonZero |
                   I::kVerifyVarArgRange | I::kVerifyVarArgRangeNonZero)) {
        const auto format = I::FormatOf(op);
        const InvokeRegisters operands(insn, format == I::k3rc || format == I::k4rcc);
        if (!operands.range && operands.size() > I::kMaxVarArgRegs) {
          runtime.Fail("Ljava/lang/VerifyError;", "Invalid DEX argument count");
        }
        for (size_t i = 0; i < operands.size(); ++i) check(operands[i], false);
      }
      instruction_widths[pc] = width;
    }
    pc += width;
  }
}

void MethodCodeCache::PrepareSites() {
  // The code-unit table holds offsets into the two compact site arrays.
  std::vector<uint32_t> indices;
  for (uint32_t pc = 0; pc < code.InsnsSizeInCodeUnits();) {
    const I& instruction = code.InstructionAt(pc);
    const size_t width = CodeWidth(code, pc);
    if (!width) break;
    const auto op = instruction.Opcode();
    const bool invoke = IsInvoke(op);
    if (invoke || IsMemberAccess(op)) {
      if (indices.empty()) indices.resize(code.InsnsSizeInCodeUnits(), UINT32_MAX);
      if (invoke) {
        indices[pc] = invokes.size();
        invokes.emplace_back(InvokeRegisters(instruction, op >= I::INVOKE_VIRTUAL_RANGE));
      } else {
        indices[pc] = members.size();
        members.emplace_back();
      }
    }
    pc += width;
  }
  site_indices = std::move(indices);
  sites_prepared = true;
}

Value Runtime::Interpret(Method& method, Object* receiver, std::span<const Value> args) {
  const auto& dex = *method.owner->dex;
  if (!method.code_cache) {
    auto cache = std::make_unique<MethodCodeCache>(dex, method.code);
    cache->VerifyRegisters(*this, method);
    method.code_cache = std::move(cache);
  }
  auto& cache = *method.code_cache;
  const auto& code = cache.code;
  const uint32_t* indices = nullptr;
  if (inline_caches) {
    if (!cache.sites_prepared) cache.PrepareSites();
    if (!cache.site_indices.empty()) indices = cache.site_indices.data();
  }
  ShadowFrame f(*this, code.RegistersSize(), *method.owner);
  size_t reg = code.RegistersSize() - code.InsSize();
  if (!method.IsStatic()) f.Reg(reg++) = Value::Ref(receiver);
  for (size_t i = 0; i < args.size(); ++i) {
    const char type = method.shorty[i + 1];
    f.Write(reg, args[i], type);
    reg += IsWide(type) ? 2 : 1;
  }
  uint32_t pc = 0;
  const auto member = [&]() -> MemberCache* {
    if (!indices) return nullptr;
    if (indices[pc] == UINT32_MAX) Fail("Ljava/lang/VerifyError;", "Invalid member instruction");
    return &cache.members[indices[pc]];
  };
  const auto type = [&](uint16_t index) -> Class& {
    MemberCache* entry = member();
    if (entry && entry->resolved) return *static_cast<Class*>(entry->resolved);
    if (index >= dex.NumTypeIds()) Fail("Ljava/lang/VerifyError;", "Bad DEX type index");
    Class& cls = linker.Resolve(dex.GetTypeDescriptorView(art::dex::TypeIndex(index)));
    if (entry) entry->resolved = &cls;
    return cls;
  };
  const auto string = [&](uint32_t index) {
    MemberCache* entry = member();
    if (entry && entry->resolved) return Value::Ref(static_cast<Object*>(entry->resolved));
    if (index >= dex.NumStringIds()) Fail("Ljava/lang/VerifyError;", "Bad DEX string index");
    Object* result = InternLiteral(DecodeUtf8(dex.GetStringData(art::dex::StringIndex(index))));
    if (entry) entry->resolved = result;
    return Value::Ref(result);
  };
  const uint16_t* insns = code.Insns();
  const uint32_t size = code.InsnsSizeInCodeUnits();
  const uint8_t* widths = cache.instruction_widths.data();
  const auto move_result_at = [insns, size](uint32_t next) {
    if (next >= size) return false;
    const uint8_t opcode = static_cast<uint8_t>(insns[next]);
    return opcode >= I::MOVE_RESULT && opcode <= I::MOVE_RESULT_OBJECT;
  };
  for (;;) {
    try {
      for (;;) {
        if (pc >= size || widths[pc] == 0) Fail("Ljava/lang/VerifyError;", "Invalid DEX instruction address");
        const uint16_t* address = insns + pc;
        const uint8_t op = static_cast<uint8_t>(*address);
        const I& insn = *I::At(address);
        executed_opcodes[op] = 1;
        uint32_t next = pc + widths[pc];
        switch (op) {
          case I::NOP: break;
          case I::MOVE: f.Write(insn.VRegA_12x(), f.Reg(insn.VRegB_12x()), 'I'); break;
          case I::MOVE_FROM16: f.Write(insn.VRegA_22x(), f.Reg(insn.VRegB_22x()), 'I'); break;
          case I::MOVE_16: f.Write(insn.VRegA_32x(), f.Reg(insn.VRegB_32x()), 'I'); break;
          case I::MOVE_WIDE: f.SetWide(insn.VRegA_12x(), f.Wide(insn.VRegB_12x())); break;
          case I::MOVE_WIDE_FROM16: f.SetWide(insn.VRegA_22x(), f.Wide(insn.VRegB_22x())); break;
          case I::MOVE_WIDE_16: f.SetWide(insn.VRegA_32x(), f.Wide(insn.VRegB_32x())); break;
          case I::MOVE_OBJECT: f.Write(insn.VRegA_12x(), f.Reg(insn.VRegB_12x()), 'L'); break;
          case I::MOVE_OBJECT_FROM16: f.Write(insn.VRegA_22x(), f.Reg(insn.VRegB_22x()), 'L'); break;
          case I::MOVE_OBJECT_16: f.Write(insn.VRegA_32x(), f.Reg(insn.VRegB_32x()), 'L'); break;
          case I::MOVE_RESULT: f.Write(insn.VRegA_11x(), f.Result(), 'I'); f.Result() = {}; break;
          case I::MOVE_RESULT_WIDE: f.SetWide(insn.VRegA_11x(), f.Result()); f.Result() = {}; break;
          case I::MOVE_RESULT_OBJECT: f.Write(insn.VRegA_11x(), f.Result(), 'L'); f.Result() = {}; break;
          case I::MOVE_EXCEPTION: f.Write(insn.VRegA_11x(), f.Caught(), 'L'); f.Caught() = {}; break;
          case I::RETURN_VOID: return {};
          case I::RETURN: return f.Read(insn.VRegA_11x(), method.shorty.front());
          case I::RETURN_WIDE: return f.Wide(insn.VRegA_11x());
          case I::RETURN_OBJECT: return Value::Ref(f.Reg(insn.VRegA_11x()).ref());

          case I::CONST_4: f.Reg(insn.VRegA_11n()) = Value::Int(insn.VRegB_11n()); break;
          case I::CONST_16: f.Reg(insn.VRegA_21s()) = Value::Int(insn.VRegB_21s()); break;
          case I::CONST: f.Reg(insn.VRegA_31i()) = Value::Int(insn.VRegB_31i()); break;
          case I::CONST_HIGH16: f.Reg(insn.VRegA_21h()) = Value::Bits(uint32_t{insn.VRegB_21h()} << 16); break;
          case I::CONST_WIDE_16: f.SetWide(insn.VRegA_21s(), Value::Long(insn.VRegB_21s())); break;
          case I::CONST_WIDE_32: f.SetWide(insn.VRegA_31i(), Value::Long(insn.VRegB_31i())); break;
          case I::CONST_WIDE: f.SetWide(insn.VRegA_51l(), Value::Bits(insn.VRegB_51l())); break;
          case I::CONST_WIDE_HIGH16: f.SetWide(insn.VRegA_21h(), Value::Bits(uint64_t{insn.VRegB_21h()} << 48)); break;
          case I::CONST_STRING: f.Reg(insn.VRegA_21c()) = string(insn.VRegB_21c()); break;
          case I::CONST_STRING_JUMBO: f.Reg(insn.VRegA_31c()) = string(insn.VRegB_31c()); break;
          case I::CONST_CLASS: f.Reg(insn.VRegA_21c()) = Value::Ref(ClassObject(type(insn.VRegB_21c()))); break;

          case I::MONITOR_ENTER: f.EnterMonitor(NonNull(f.Reg(insn.VRegA_11x()).ref())); break;
          case I::MONITOR_EXIT: f.ExitMonitor(NonNull(f.Reg(insn.VRegA_11x()).ref())); break;
          case I::CHECK_CAST: {
            Class& target = type(insn.VRegB_21c());
            Object* object = f.Reg(insn.VRegA_21c()).ref();
            if (object && !linker.IsAssignable(target, *object->type)) {
              Fail("Ljava/lang/ClassCastException;", object->type->descriptor + " cannot be cast to " + target.descriptor);
            }
            break;
          }
          case I::INSTANCE_OF: {
            Class& target = type(insn.VRegC_22c());
            Object* object = f.Reg(insn.VRegB_22c()).ref();
            f.Reg(insn.VRegA_22c()) = Value::Int(object && linker.IsAssignable(target, *object->type));
            break;
          }
          case I::ARRAY_LENGTH: {
            Object& array = NonNull(f.Reg(insn.VRegB_12x()).ref());
            if (!array.type->IsArray()) Fail("Ljava/lang/VerifyError;", "Expected an array");
            f.Reg(insn.VRegA_12x()) = Value::Int(array.elements.size());
            break;
          }
          case I::NEW_INSTANCE: {
            Class& cls = type(insn.VRegB_21c());
            if (cls.IsArray() || cls.IsPrimitive() || (cls.flags & (art::kAccAbstract | art::kAccInterface))) {
              Fail("Ljava/lang/InstantiationError;", cls.descriptor);
            }
            linker.Initialize(cls);
            f.Reg(insn.VRegA_21c()) = Value::Ref(heap.Allocate(*this, cls));
            break;
          }
          case I::NEW_ARRAY:
            f.Reg(insn.VRegA_22c()) = Value::Ref(NewArray(type(insn.VRegC_22c()), f.Reg(insn.VRegB_22c()).i()));
            break;
          case I::FILLED_NEW_ARRAY:
          case I::FILLED_NEW_ARRAY_RANGE: {
            const bool range = op == I::FILLED_NEW_ARRAY_RANGE;
            const InvokeRegisters registers(insn, range);
            if (!range && registers.size() > I::kMaxVarArgRegs) Fail("Ljava/lang/VerifyError;", "Invalid filled array count");
            auto* array = NewArray(type(range ? insn.VRegB_3rc() : insn.VRegB_35c()), registers.size());
            f.Result() = Value::Ref(array);
            const char component = array->type->component->descriptor.front();
            if (IsWide(component)) Fail("Ljava/lang/VerifyError;", "Wide filled-new-array component");
            for (size_t i = 0; i < registers.size(); ++i) StoreArrayElement(*array, i, f.Read(registers[i], component));
            if (!move_result_at(next)) f.Result() = {};
            break;
          }
          case I::FILL_ARRAY_DATA:
            FillArray(*this, NonNull(f.Reg(insn.VRegA_31t()).ref()),
                      PayloadAt(*this, code, pc + insn.VRegB_31t(), I::kArrayDataSignature));
            break;
          case I::THROW: Throw(f.Reg(insn.VRegA_11x()).ref());
          case I::GOTO: next = pc + insn.VRegA_10t(); break;
          case I::GOTO_16: next = pc + insn.VRegA_20t(); break;
          case I::GOTO_32: next = pc + insn.VRegA_30t(); break;
          case I::PACKED_SWITCH:
          case I::SPARSE_SWITCH: {
            const auto payload = op == I::PACKED_SWITCH ? I::kPackedSwitchSignature : I::kSparseSwitchSignature;
            next = pc + SwitchOffset(PayloadAt(*this, code, pc + insn.VRegB_31t(), payload),
                                     static_cast<I::Code>(op), f.Reg(insn.VRegA_31t()).i());
            break;
          }

          case I::CMPL_FLOAT ... I::CMP_LONG: {
            const auto left = insn.VRegB_23x(), right = insn.VRegC_23x();
            int comparison;
            if (op == I::CMP_LONG) comparison = Compare(f.Wide(left).j(), f.Wide(right).j());
            else if (op <= I::CMPG_FLOAT) comparison = Compare(f.Reg(left).f(), f.Reg(right).f(), op == I::CMPG_FLOAT ? 1 : -1);
            else comparison = Compare(f.Wide(left).d(), f.Wide(right).d(), op == I::CMPG_DOUBLE ? 1 : -1);
            f.Reg(insn.VRegA_23x()) = Value::Int(comparison);
            break;
          }
          case I::IF_EQ ... I::IF_LE:
            if (Branch(static_cast<I::Code>(op), f.Reg(insn.VRegA_22t()), f.Reg(insn.VRegB_22t()))) next = pc + insn.VRegC_22t();
            break;
          case I::IF_EQZ ... I::IF_LEZ:
            if (Branch(static_cast<I::Code>(op), f.Reg(insn.VRegA_21t()), {})) next = pc + insn.VRegB_21t();
            break;

          case I::AGET ... I::APUT_SHORT: {
            Object& array = NonNull(f.Reg(insn.VRegB_23x()).ref());
            const auto index = f.Reg(insn.VRegC_23x()).i();
            Value& slot = ArrayElement(array, index);
            const char component = array.type->component->kind;
            if (op >= I::APUT) StoreArrayElement(array, index, f.Read(insn.VRegA_23x(), component));
            else f.Write(insn.VRegA_23x(), slot, component);
            break;
          }
          case I::IGET ... I::IPUT_SHORT: FieldInstruction<false>(*this, f, dex, insn, op, member()); break;
          case I::SGET ... I::SPUT_SHORT: FieldInstruction<true>(*this, f, dex, insn, op, member()); break;
          case I::INVOKE_VIRTUAL ... I::INVOKE_INTERFACE:
          case I::INVOKE_VIRTUAL_RANGE ... I::INVOKE_INTERFACE_RANGE: {
            Value result;
            if (indices) {
              if (indices[pc] == UINT32_MAX) Fail("Ljava/lang/VerifyError;", "Invalid invoke instruction");
              auto& site = cache.invokes[indices[pc]];
              result = InvokeInstruction(*this, f, method, insn, op, site.operands, &site);
            } else {
              const InvokeRegisters registers(insn, op >= I::INVOKE_VIRTUAL_RANGE);
              result = InvokeInstruction(*this, f, method, insn, op, registers, nullptr);
            }
            if (move_result_at(next)) f.Result() = result;
            break;
          }
          case I::NEG_INT ... I::INT_TO_SHORT: Unary(f, insn); break;
          case I::ADD_INT ... I::USHR_INT_LIT8: Arithmetic(*this, f, insn); break;
          default: Fail("Ljava/lang/VerifyError;", std::string("Unsupported instruction ") + insn.Name());
        }
        pc = next;
      }
    } catch (const ManagedException&) {
      f.Result() = {};
      if (!FindHandler(*this, f, dex, code, pc)) throw;
    }
  }
}

size_t Vm::SupportedOpcodeCount() {
  return (I::IF_LEZ - I::NOP + 1) + (I::INVOKE_INTERFACE - I::AGET + 1) +
         (I::INVOKE_INTERFACE_RANGE - I::INVOKE_VIRTUAL_RANGE + 1) + (I::USHR_INT_LIT8 - I::NEG_INT + 1);
}

}  // namespace wa::interp
