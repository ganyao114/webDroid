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
namespace {

using I = art::Instruction;
using art::interpreter::SafeAdd;
using art::interpreter::SafeMul;
using art::interpreter::SafeSub;

class ShadowFrame {
 public:
  ShadowFrame(Runtime& runtime, size_t registers)
      : roots_(runtime, registers + 2), registers_(registers) {}

  ~ShadowFrame() {
    for (size_t i = registers_ + 2; i < roots_.values.size(); ++i) {
      --roots_.values[i].ref()->monitor_depth;
    }
  }

  Value& Reg(size_t i) {
    if (i >= registers_) roots_.runtime.Fail("Ljava/lang/VerifyError;", "Invalid register index");
    return roots_.values[i];
  }

  Value Wide(size_t i) {
    return Value::Bits(Reg(i).bits() | (Reg(i + 1).bits() << 32));
  }

  void SetWide(size_t i, Value value) {
    Reg(i) = Value::Bits(static_cast<uint32_t>(value.bits()));
    Reg(i + 1) = Value::Bits(value.bits() >> 32);
  }

  Value Read(size_t i, char type) { return IsWide(type) ? Wide(i) : Normalize(Reg(i), type); }
  void Write(size_t i, Value value, char type) {
    if (IsWide(type)) SetWide(i, value);
    else Reg(i) = Normalize(value, type);
  }

  Value& Result() { return roots_.values[registers_]; }
  Value& Caught() { return roots_.values[registers_ + 1]; }

  void EnterMonitor(Object& object) {
    roots_.Hold(&object);
    ++object.monitor_depth;
  }

  void ExitMonitor(Object& object) {
    const auto first = roots_.values.begin() + registers_ + 2;
    for (auto it = roots_.values.end(); it != first;) {
      if ((--it)->ref() == &object) {
        --object.monitor_depth;
        roots_.values.erase(it);
        return;
      }
    }
    roots_.runtime.Fail("Ljava/lang/IllegalMonitorStateException;");
  }

 private:
  RootScope roots_;
  size_t registers_;
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

const I& InstructionAt(Runtime& runtime, const art::CodeItemDataAccessor& code, uint32_t pc) {
  if (pc >= code.InsnsSizeInCodeUnits()) {
    runtime.Fail("Ljava/lang/VerifyError;", "Instruction is outside the code item");
  }
  return *I::At(code.Insns() + pc);
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

class InvokeRegisters {
 public:
  InvokeRegisters(const I& insn, bool range) : range_(range) {
    if (range) {
      count_ = insn.VRegA_3rc();
      first_ = insn.VRegC_3rc();
    } else {
      count_ = insn.GetVarArgs(regs_.data());
    }
  }
  uint32_t operator[](size_t i) const { return range_ ? first_ + i : regs_[i]; }
  size_t size() const { return count_; }

 private:
  bool range_;
  size_t count_;
  uint32_t first_ = 0;
  std::array<uint32_t, I::kMaxVarArgRegs> regs_{};
};

void InvokeInstruction(Runtime& runtime, ShadowFrame& f, const Method& caller, const I& insn) {
  const auto& dex = *caller.owner->dex;
  const bool range = insn.Opcode() >= I::INVOKE_VIRTUAL_RANGE;
  const auto op = static_cast<I::Code>(insn.Opcode() - (range ? I::INVOKE_VIRTUAL_RANGE - I::INVOKE_VIRTUAL : 0));
  const bool is_static = op == I::INVOKE_STATIC;
  const auto& id = dex.GetMethodId(range ? insn.VRegB_3rc() : insn.VRegB_35c());
  Class& owner = runtime.linker.Resolve(dex.GetMethodDeclaringClassDescriptorView(id));
  Method* target = &runtime.linker.FindMethod(owner, dex.GetMethodNameView(id),
                                            dex.GetMethodSignature(id).ToString(), is_static);
  const InvokeRegisters registers(insn, range);
  if (!is_static && registers.size() == 0) runtime.Fail("Ljava/lang/VerifyError;", "Missing receiver");
  Object* receiver = is_static ? nullptr : &runtime.NonNull(f.Reg(registers[0]).ref());
  if (receiver && !runtime.linker.IsAssignable(owner, *receiver->type)) {
    runtime.Fail("Ljava/lang/IncompatibleClassChangeError;", target->QualifiedName());
  }
  if (op == I::INVOKE_VIRTUAL || op == I::INVOKE_INTERFACE) {
    target = &runtime.linker.VirtualMethod(*receiver->type, *target);
  } else if (op == I::INVOKE_SUPER && !owner.IsInterface()) {
    if (!caller.owner->super) runtime.Fail("Ljava/lang/NoSuchMethodError;", target->QualifiedName());
    target = &runtime.linker.VirtualMethod(*caller.owner->super, *target);
  }

  std::vector<Value> args;
  args.reserve(target->shorty.size() - 1);
  size_t pos = receiver ? 1 : 0;
  for (char type : std::string_view(target->shorty).substr(1)) {
    const size_t words = IsWide(type) ? 2 : 1;
    if (pos + words > registers.size() || (words == 2 && registers[pos + 1] != registers[pos] + 1)) {
      runtime.Fail("Ljava/lang/VerifyError;", "Invalid invoke registers for " + target->QualifiedName());
    }
    args.push_back(f.Read(registers[pos], type));
    pos += words;
  }
  if (pos != registers.size()) runtime.Fail("Ljava/lang/VerifyError;", "Excess invoke registers");
  f.Result() = runtime.Invoke(*target, receiver, args);
}

void FieldInstruction(Runtime& runtime, ShadowFrame& f, const art::DexFile& dex, const I& insn) {
  const auto op = insn.Opcode();
  const bool is_static = op >= I::SGET;
  const bool is_put = is_static ? op >= I::SPUT : op >= I::IPUT;
  const auto index = is_static ? insn.VRegB_21c() : insn.VRegC_22c();
  const auto reg = is_static ? insn.VRegA_21c() : insn.VRegA_22c();
  const auto& id = dex.GetFieldId(index);
  Class& owner = runtime.linker.Resolve(dex.GetFieldDeclaringClassDescriptorView(id));
  Field& field = runtime.linker.FindField(owner, dex.GetFieldNameView(id), dex.GetFieldTypeDescriptorView(id), is_static);
  Value* storage = &field.value;
  if (is_static) {
    runtime.linker.Initialize(*field.owner);
  } else {
    Object& object = runtime.NonNull(f.Reg(insn.VRegB_22c()).ref());
    if (!runtime.linker.IsAssignable(*field.owner, *object.type)) {
      runtime.Fail("Ljava/lang/IncompatibleClassChangeError;", field.name);
    }
    storage = &object.fields[field.slot];
  }
  if (is_put) *storage = f.Read(reg, field.type.front());
  else f.Write(reg, *storage, field.type.front());
}

bool FindHandler(Runtime& runtime, ShadowFrame& f, const art::DexFile& dex,
                 const art::CodeItemDataAccessor& code, uint32_t& pc) {
  const art::dex::TryItem* item = code.TriesSize() ? code.FindTryItem(pc) : nullptr;
  if (!item) return false;
  for (art::CatchHandlerIterator handler(code, *item); handler.HasNext(); handler.Next()) {
    const auto type = handler.GetHandlerTypeIndex();
    if (!type.IsValid() || runtime.linker.IsAssignable(runtime.linker.Resolve(dex.GetTypeDescriptorView(type)),
                                                       *runtime.exception->type)) {
      f.Caught() = Value::Ref(runtime.exception);
      runtime.exception = nullptr;
      pc = handler.GetHandlerAddress();
      return true;
    }
  }
  return false;
}

}  // namespace

Value Runtime::Interpret(const Method& method, Object* receiver, std::span<const Value> args) {
  const auto& dex = *method.owner->dex;
  const art::CodeItemDataAccessor code(dex, method.code);
  ShadowFrame f(*this, code.RegistersSize());
  size_t reg = code.RegistersSize() - code.InsSize();
  if (!method.IsStatic()) f.Reg(reg++) = Value::Ref(receiver);
  for (size_t i = 0; i < args.size(); ++i) {
    const char type = method.shorty[i + 1];
    f.Write(reg, args[i], type);
    reg += IsWide(type) ? 2 : 1;
  }
  if (reg != code.RegistersSize()) Fail("Ljava/lang/VerifyError;", "Invalid code item arguments");

  const auto type = [&](uint16_t index) -> Class& {
    return linker.Resolve(dex.GetTypeDescriptorView(art::dex::TypeIndex(index)));
  };
  const auto string = [&](uint32_t index) {
    return Value::Ref(Intern(DecodeUtf8(dex.GetStringData(art::dex::StringIndex(index)))));
  };
  uint32_t pc = 0;
  for (;;) {
    const I& insn = InstructionAt(*this, code, pc);
    const I::Code op = insn.Opcode();
    executed_opcodes.set(op);
    uint32_t next = pc + insn.SizeInCodeUnits();
    if (op < I::MOVE_RESULT || op > I::MOVE_RESULT_OBJECT) f.Result() = {};
    if (op != I::MOVE_EXCEPTION) f.Caught() = {};
    try {
      switch (op) {
        case I::NOP:
          if (insn.VRegA_10x() != 0) Fail("Ljava/lang/VerifyError;", "Executed a DEX payload");
          break;
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
          auto* array = NewArray(type(range ? insn.VRegB_3rc() : insn.VRegB_35c()), registers.size());
          f.Result() = Value::Ref(array);
          const char component = array->type->component->descriptor.front();
          if (IsWide(component)) Fail("Ljava/lang/VerifyError;", "Wide filled-new-array component");
          for (size_t i = 0; i < registers.size(); ++i) StoreArrayElement(*array, i, f.Read(registers[i], component));
          break;
        }
        case I::FILL_ARRAY_DATA:
          FillArray(*this, NonNull(f.Reg(insn.VRegA_31t()).ref()), InstructionAt(*this, code, pc + insn.VRegB_31t()));
          break;
        case I::THROW: Throw(f.Reg(insn.VRegA_11x()).ref());
        case I::GOTO: next = pc + insn.VRegA_10t(); break;
        case I::GOTO_16: next = pc + insn.VRegA_20t(); break;
        case I::GOTO_32: next = pc + insn.VRegA_30t(); break;
        case I::PACKED_SWITCH:
        case I::SPARSE_SWITCH:
          next = pc + SwitchOffset(InstructionAt(*this, code, pc + insn.VRegB_31t()), op, f.Reg(insn.VRegA_31t()).i());
          break;

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
          if (Branch(op, f.Reg(insn.VRegA_22t()), f.Reg(insn.VRegB_22t()))) next = pc + insn.VRegC_22t();
          break;
        case I::IF_EQZ ... I::IF_LEZ:
          if (Branch(op, f.Reg(insn.VRegA_21t()), {})) next = pc + insn.VRegB_21t();
          break;

        case I::AGET ... I::APUT_SHORT: {
          Object& array = NonNull(f.Reg(insn.VRegB_23x()).ref());
          const auto index = f.Reg(insn.VRegC_23x()).i();
          Value& slot = ArrayElement(array, index);
          const char component = array.type->component->descriptor.front();
          if (op >= I::APUT) StoreArrayElement(array, index, f.Read(insn.VRegA_23x(), component));
          else f.Write(insn.VRegA_23x(), slot, component);
          break;
        }
        case I::IGET ... I::SPUT_SHORT: FieldInstruction(*this, f, dex, insn); break;
        case I::INVOKE_VIRTUAL ... I::INVOKE_INTERFACE:
        case I::INVOKE_VIRTUAL_RANGE ... I::INVOKE_INTERFACE_RANGE:
          InvokeInstruction(*this, f, method, insn);
          break;
        case I::NEG_INT ... I::INT_TO_SHORT: Unary(f, insn); break;
        case I::ADD_INT ... I::USHR_INT_LIT8: Arithmetic(*this, f, insn); break;
        default: Fail("Ljava/lang/VerifyError;", std::string("Unsupported instruction ") + insn.Name());
      }
      pc = next;
    } catch (const ManagedException&) {
      if (!FindHandler(*this, f, dex, code, pc)) throw;
    }
  }
}

size_t Vm::SupportedOpcodeCount() {
  return (I::IF_LEZ - I::NOP + 1) + (I::INVOKE_INTERFACE - I::AGET + 1) +
         (I::INVOKE_INTERFACE_RANGE - I::INVOKE_VIRTUAL_RANGE + 1) + (I::USHR_INT_LIT8 - I::NEG_INT + 1);
}

}  // namespace wa::interp
