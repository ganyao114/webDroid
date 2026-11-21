#include "jni_bridge.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "dex/utf-inl.h"

namespace wa::interp {

std::u16string DecodeUtf8(std::string_view text) {
  if (text.empty()) return {};
  std::vector<uint16_t> units(art::CountModifiedUtf8Chars(text.data(), text.size()));
  art::ConvertModifiedUtf8ToUtf16(units.data(), units.size(), text.data(), text.size());
  return std::u16string(units.begin(), units.end());
}

std::string EncodeUtf8(std::u16string_view text, bool modified) {
  if (text.empty()) return {};
  const std::vector<uint16_t> units(text.begin(), text.end());
  std::string result;
  if (modified) {
    result.resize(art::CountModifiedUtf8BytesInUtf16(units.data(), units.size()));
    art::ConvertUtf16ToModifiedUtf8(result.data(), result.size(), units.data(), units.size());
  } else {
    art::ConvertUtf16ToUtf8<true, true, true>(units.data(), units.size(),
        [&](uint8_t byte) { result.push_back(byte); });
  }
  return result;
}

Value Normalize(Value value, char type) {
  switch (type) {
    case 'L': case '[': return Value::Ref(value.ref());
    case 'J': case 'D': return Value::Bits(value.bits());
    case 'Z': return Value::Int(value.i() & 1);
    case 'B': return Value::Int(static_cast<int8_t>(value.i()));
    case 'C': return Value::Int(static_cast<uint16_t>(value.i()));
    case 'S': return Value::Int(static_cast<int16_t>(value.i()));
    default: return Value::Int(value.i());
  }
}

Runtime::Runtime(Options opts)
    : options(std::move(opts)), linker(*this), jni(std::make_unique<JniBridge>(*this)) {
  InstallBootClasses();
}

Runtime::~Runtime() = default;

Value Runtime::Invoke(Method& method, Object* receiver, std::span<const Value> args) {
  RootScope pinned(*this);
  pinned.Hold(receiver);
  pinned.values.insert(pinned.values.end(), args.begin(), args.end());
  if (args.size() + 1 != method.shorty.size()) {
    Fail("Ljava/lang/VerifyError;", "Wrong argument count for " + method.QualifiedName());
  }
  if (call_depth >= 384) Fail("Ljava/lang/StackOverflowError;");
  struct CallDepth {
    size_t& depth;
    explicit CallDepth(size_t& depth) : depth(depth) { ++depth; }
    ~CallDepth() { --depth; }
  } depth(call_depth);

  if (method.IsStatic()) linker.Initialize(*method.owner);
  else NonNull(receiver);
  Object* monitor = nullptr;
  if ((method.flags & (art::kAccSynchronized | art::kAccDeclaredSynchronized)) != 0) {
    monitor = pinned.Hold(method.IsStatic() ? ClassObject(*method.owner) : receiver);
    ++monitor->monitor_depth;
  }
  struct MonitorGuard {
    Object* monitor;
    ~MonitorGuard() { if (monitor) --monitor->monitor_depth; }
  } monitor_guard{monitor};

  ++method.stats.invocations;
  if (method.stats.invocations == options.hotness_threshold && options.install_wasm) Promote(method);
  if (method.compiled) {
    Value result = method.compiled(args);
    ++method.stats.tier1_hits;
    return result;
  }
  if (method.builtin) return method.builtin(*this, receiver, args);
  if (method.IsNative()) return jni->Invoke(method, receiver, args);
  if (method.IsAbstract()) Fail("Ljava/lang/AbstractMethodError;", method.QualifiedName());
  if (!method.code) Fail("Ljava/lang/VerifyError;", "Missing code for " + method.QualifiedName());
  ++method.stats.interpreted;
  return Interpret(method, receiver, args);
}

Object* Runtime::NewObject(Class& cls, Method& constructor, std::span<const Value> args) {
  RootScope pinned(*this);
  pinned.values.insert(pinned.values.end(), args.begin(), args.end());
  linker.Initialize(cls);
  if ((cls.flags & (art::kAccInterface | art::kAccAbstract)) != 0 || cls.IsArray() || cls.IsPrimitive()) {
    Fail("Ljava/lang/InstantiationError;", cls.descriptor);
  }
  Object* object = pinned.Hold(heap.Allocate(*this, cls));
  Invoke(constructor, object, args);
  return object;
}

Object* Runtime::NewArray(Class& cls, int32_t length) {
  if (!cls.IsArray()) Fail("Ljava/lang/VerifyError;", cls.descriptor);
  if (length < 0) Fail("Ljava/lang/NegativeArraySizeException;", std::to_string(length));
  if (static_cast<size_t>(length) > std::vector<Value>().max_size()) Fail("Ljava/lang/OutOfMemoryError;");
  Object* array = heap.Allocate(*this, cls);
  try {
    array->elements.resize(length);
  } catch (const std::bad_alloc&) {
    Fail("Ljava/lang/OutOfMemoryError;");
  }
  return array;
}

Object* Runtime::NewString(std::u16string text) {
  Object* string = heap.Allocate(*this, linker.Resolve("Ljava/lang/String;"));
  string->text = std::move(text);
  return string;
}

Object* Runtime::Intern(Object* string) {
  auto [it, inserted] = interned.try_emplace(string->text, string);
  return it->second;
}

Object* Runtime::Intern(std::u16string text) {
  const auto it = interned.find(text);
  return it == interned.end() ? Intern(NewString(std::move(text))) : it->second;
}

Object* Runtime::ClassObject(Class& cls) {
  if (!cls.mirror) {
    cls.mirror = heap.Allocate(*this, linker.Resolve("Ljava/lang/Class;"));
    cls.mirror->represented_class = &cls;
  }
  return cls.mirror;
}

Object& Runtime::NonNull(Object* object) {
  if (!object) Fail("Ljava/lang/NullPointerException;");
  return *object;
}

Value& Runtime::ArrayElement(Object& array, int32_t index) {
  if (!array.type->IsArray()) Fail("Ljava/lang/VerifyError;", "Expected an array");
  if (index < 0 || static_cast<size_t>(index) >= array.elements.size()) {
    Fail("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(index));
  }
  return array.elements[index];
}

void Runtime::StoreArrayElement(Object& array, int32_t index, Value value) {
  Value& slot = ArrayElement(array, index);
  Class& component = *array.type->component;
  if (!component.IsPrimitive() && value.ref() && !linker.IsAssignable(component, *value.ref()->type)) {
    Fail("Ljava/lang/ArrayStoreException;", value.ref()->type->descriptor);
  }
  slot = Normalize(value, component.descriptor.front());
}

Object* Runtime::MakeException(std::string_view type, std::string_view message) {
  Object* object = heap.Allocate(*this, linker.Resolve(type));
  object->text = DecodeUtf8(message);
  return object;
}

void Runtime::Fail(std::string_view type, std::string_view message) {
  exception = MakeException(type, message);
  throw ManagedException{};
}

void Runtime::Throw(Object* object) {
  Object& thrown = NonNull(object);
  if (!linker.IsAssignable(linker.Resolve("Ljava/lang/Throwable;"), *thrown.type)) {
    Fail("Ljava/lang/VerifyError;", "Expected a Throwable");
  }
  exception = &thrown;
  throw ManagedException{};
}

std::string Runtime::ExceptionSummary() const {
  if (!exception) return {};
  std::string description;
  std::unordered_set<const Object*> seen;
  for (const Object* current = exception; current && seen.insert(current).second;
       current = current->payload.ref()) {
    if (!description.empty()) description += " caused by ";
    description += current->type->descriptor;
    if (!current->text.empty()) description += ": " + EncodeUtf8(current->text, false);
  }
  return description;
}

Vm::Vm(Options options) : runtime_(std::make_unique<Runtime>(std::move(options))) {}
Vm::~Vm() = default;

void Vm::LoadDexFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open DEX " + path);
  LoadDexFile(path, {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()});
}

void Vm::LoadDexFile(std::string location, std::vector<uint8_t> bytes) {
  runtime_->linker.LoadDexFile(std::move(location), std::move(bytes));
}

void Vm::RegisterResourceNatives() { runtime_->jni->RegisterResources(); }
void Vm::AddNativeAdapter(void* function, NativeThunk adapter) {
  runtime_->jni->RegisterAdapter(function, adapter);
}
JNIEnv* Vm::JniEnv() { return runtime_->jni->Env(); }

Value Vm::CallStatic(std::string_view owner, std::string_view name, std::string_view signature,
                     std::span<const Value> args) {
  ClassLinker& linker = runtime_->linker;
  auto& method = linker.FindMethod(linker.Resolve(owner), name, signature, true);
  return runtime_->Invoke(method, nullptr, args);
}

Value Vm::CallVirtual(Object* receiver, std::string_view name, std::string_view signature,
                      std::span<const Value> args) {
  ClassLinker& linker = runtime_->linker;
  Class& type = *runtime_->NonNull(receiver).type;
  return runtime_->Invoke(linker.VirtualMethod(type, linker.FindMethod(type, name, signature, false)),
                          receiver, args);
}

Object* Vm::NewObject(std::string_view type, std::string_view constructor,
                      std::span<const Value> args) {
  Class& cls = runtime_->linker.Resolve(type);
  return runtime_->NewObject(cls, runtime_->linker.FindMethod(cls, "<init>", constructor, false), args);
}

Object* Vm::NewString(std::string_view utf8) { return runtime_->NewString(DecodeUtf8(utf8)); }
std::string Vm::StringUtf8(const Object* string) const {
  if (!string || string->type->descriptor != "Ljava/lang/String;") {
    throw std::invalid_argument("Expected a Java string");
  }
  return EncodeUtf8(string->text, false);
}

std::string Vm::ExceptionSummary() const { return runtime_->ExceptionSummary(); }
void Vm::ClearException() { runtime_->exception = nullptr; }
GcStats Vm::Collect() { return runtime_->heap.Collect(*runtime_); }
GcStats Vm::HeapStats() const { return runtime_->heap.Stats(); }
MethodStats Vm::GetMethodStats(std::string_view owner, std::string_view name, std::string_view signature) {
  return runtime_->linker.FindMethod(runtime_->linker.Resolve(owner), name, signature, true).stats;
}
size_t Vm::ExecutedOpcodeCount() const { return runtime_->executed_opcodes.count(); }
uint64_t Vm::NativeCalls(std::string_view owner) const { return runtime_->jni->Calls(owner); }

ScopedHandleScope::ScopedHandleScope(Vm& vm) : roots_(std::make_unique<RootScope>(*vm.runtime_)) {}
ScopedHandleScope::~ScopedHandleScope() = default;
Object* ScopedHandleScope::Hold(Object* object) { return roots_->Hold(object); }
Value ScopedHandleScope::Hold(Value value) { return roots_->Hold(value); }

}  // namespace wa::interp
