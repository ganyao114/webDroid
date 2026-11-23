#include "jni_bridge.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "dex/utf-inl.h"

namespace wa::interp {
namespace {

struct LoopSuspended {
  std::vector<Runtime::SuspendedFrame> frames;
};

std::vector<uint8_t> ReadDexFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Cannot open DEX " + path);
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > std::vector<uint8_t>().max_size() ||
      size > std::numeric_limits<std::streamsize>::max()) {
    throw std::runtime_error("Invalid DEX file size for " + path);
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!input.seekg(0) || !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("Cannot read DEX " + path);
  }
  return bytes;
}

}  // namespace

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

Runtime::Runtime(Options opts)
    : options(std::move(opts)), linker(*this), jni(std::make_unique<JniBridge>(*this)) {
  InstallBootClasses();
}

Runtime::~Runtime() = default;

Value Runtime::InvokeSlow(Method& method, Object* receiver, std::span<const Value> args) {
  const bool initializing = method.IsStatic() && method.owner->init_state != InitState::Initialized;
  const bool synchronized = (method.flags & (art::kAccSynchronized | art::kAccDeclaredSynchronized)) != 0;
  RootScope pinned(*this, args, receiver);
  if (initializing) linker.Initialize(*method.owner);
  Object* monitor = nullptr;
  if (synchronized) {
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

[[noreturn]] void Runtime::SuspendMainLooper() {
  if (!loop_dispatch) Fail("Ljava/lang/IllegalStateException;", "Main Looper dispatch is unavailable");
  LoopSuspended suspended;
  size_t depth = call_depth;
  for (auto* frame = active_trace; frame != loop_dispatch->boundary; frame = frame->caller) {
    constexpr auto monitor = art::kAccSynchronized | art::kAccDeclaredSynchronized;
    if (!frame || frame->depth != --depth || (frame->method->flags & monitor) ||
        frame->method->name == "<clinit>" || !frame->frame_roots.values.empty()) {
      Fail("Ljava/lang/IllegalStateException;", "Cannot suspend the main Looper across a native or locked frame");
    }
    const auto slots = frame->frame_roots.borrowed;
    suspended.frames.push_back({frame->method, *frame->dex_pc, {slots.begin(), slots.end()}});
  }
  if (suspended.frames.empty() || depth != loop_dispatch->depth + 1) {
    Fail("Ljava/lang/IllegalStateException;", "Missing the main Looper caller");
  }
  std::ranges::reverse(suspended.frames);
  throw suspended;
}

Value Runtime::ResumeSuspendedLoop(std::span<const SuspendedFrame> frames) {
  // The innermost Looper.loop call observes the failed message as a Java throw.
  if (frames.empty()) throw ManagedException{};
  CallDepth depth(call_depth);
  return Interpret(*frames.front().method, nullptr, {}, frames);
}

Value Runtime::DispatchMainMessage(Object& looper, Value now) {
  if (loop_dispatch) Fail("Ljava/lang/IllegalStateException;", "Main Looper dispatch is already active");
  const LoopDispatch dispatch{active_trace, call_depth};
  loop_dispatch = &dispatch;
  struct Reset {
    const LoopDispatch*& dispatch;
    ~Reset() { dispatch = nullptr; }
  } reset{loop_dispatch};
  try {
    try {
      auto& method = linker.FindMethod(*looper.type, "loopOnce", "(Landroid/os/Looper;JI)Z", true);
      return Invoke(method, nullptr, std::array{Value::Ref(&looper), now, Value::Int(-1)});
    } catch (const ManagedException&) {
      // Feed the failure to the innermost suspended Looper.loop invocation first.
      while (!suspended_loops.empty()) {
        try {
          const Value result = ResumeSuspendedLoop(suspended_loops.back());
          suspended_loops.pop_back();
          return result;
        } catch (const ManagedException&) {
          suspended_loops.pop_back();
        } catch (const LoopSuspended&) {
          suspended_loops.pop_back();
          throw;
        }
      }
      throw;
    }
  } catch (LoopSuspended& suspended) {
    suspended_loops.push_back(std::move(suspended.frames));
    return Value::Int(1);
  }
}

Object* Runtime::NewObject(Class& cls, Method& constructor, std::span<const Value> args) {
  RootScope pinned(*this, args);
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
  if (!string_class) string_class = &linker.Resolve("Ljava/lang/String;");
  Object* string = heap.Allocate(*this, *string_class);
  string->text = std::move(text);
  return string;
}

Object* Runtime::Intern(Object* string) {
  auto [it, inserted] = interned.try_emplace(string->text, string);
  return it->second;
}

Object* Runtime::InternLiteral(std::u16string text) {
  const auto it = interned.find(text);
  Object* string = it == interned.end() ? Intern(NewString(std::move(text))) : it->second;
  if (!string->dex_literal) {
    dex_literals.push_back(string);
    string->dex_literal = true;
  }
  return string;
}

Object* Runtime::ClassObject(Class& cls) {
  if (!cls.mirror) {
    cls.mirror = heap.Allocate(*this, linker.Resolve("Ljava/lang/Class;"));
    cls.mirror->represented_class = &cls;
  }
  return cls.mirror;
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

void Runtime::CaptureThrowableStack(Object& throwable) {
  if (!options.capture_exception_stack || !active_trace) return;
  throwable.exception_stack = std::make_unique<std::vector<Object::TraceFrame>>();
  throwable.stack_at_construction = true;
  auto* frame = active_trace;
  while (frame && frame->method->name == "<init>") frame = frame->caller;
  for (; frame; frame = frame->caller) {
    throwable.exception_stack->push_back({frame->method, *frame->dex_pc});
  }
}

void Runtime::AppendExceptionFrame(const Method& method, uint32_t pc) {
  if (!exception) return;
  if (!exception->exception_stack) {
    exception->exception_stack = std::make_unique<std::vector<Object::TraceFrame>>();
  }
  exception->exception_stack->push_back({&method, pc});
}

void Runtime::Fail(std::string_view type, std::string_view message, Object* cause) {
  RootScope pinned(*this, {}, cause);
  exception = MakeException(type, message);
  exception->payload = Value::Ref(cause);
  throw ManagedException{};
}

void Runtime::Throw(Object* object) {
  Object& thrown = NonNull(object);
  if (!linker.IsAssignable(linker.Resolve("Ljava/lang/Throwable;"), *thrown.type)) {
    Fail("Ljava/lang/VerifyError;", "Expected a Throwable");
  }
  if (thrown.stack_at_construction) {
    thrown.exception_stack.reset();
    thrown.stack_at_construction = false;
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

std::vector<std::string> Runtime::ExceptionStackTrace() const {
  std::vector<std::string> trace;
  std::unordered_set<const Object*> seen;
  for (const Object* current = exception; current && seen.insert(current).second;
       current = current->payload.ref()) {
    if (!current->exception_stack || current->exception_stack->empty()) continue;
    if (current != exception) trace.push_back("Caused by " + current->type->descriptor);
    for (const auto& [method, pc] : *current->exception_stack) {
      trace.push_back(method->QualifiedName() + " @ dex pc " + std::to_string(pc));
    }
  }
  return trace;
}

Vm::Vm(Options options) : runtime_(std::make_unique<Runtime>(std::move(options))) {}
Vm::~Vm() = default;

void Vm::LoadDexFile(const std::string& path) {
  LoadDexFile(path, ReadDexFile(path));
}

void Vm::LoadDexFile(std::string location, std::vector<uint8_t> bytes) {
  runtime_->linker.LoadDexFile(std::move(location), std::move(bytes));
}

void Vm::LoadFallbackDexFile(const std::string& path) {
  LoadFallbackDexFile(path, ReadDexFile(path));
}

void Vm::LoadFallbackDexFile(std::string location, std::vector<uint8_t> bytes) {
  runtime_->linker.LoadDexFile(std::move(location), std::move(bytes), true);
}

void Vm::LoadApkDexFiles(const std::string& apk_path, bool fallback) {
  runtime_->linker.LoadApkDexFiles(apk_path, fallback);
}
DexStats Vm::GetDexStats() const { return runtime_->linker.GetDexStats(); }
std::string Vm::DexLocation(std::string_view descriptor) const {
  return runtime_->linker.DexLocation(descriptor);
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

Value Vm::ReadField(Object* receiver, std::string_view name, std::string_view descriptor) {
  auto& object = runtime_->NonNull(receiver);
  const auto& field = runtime_->linker.FindField(*object.type, name, descriptor, false);
  return object.fields[field.slot];
}

Value Vm::ReadStaticField(std::string_view owner, std::string_view name, std::string_view descriptor) {
  auto& linker = runtime_->linker;
  const auto& field = linker.FindField(linker.Resolve(owner), name, descriptor, true);
  linker.Initialize(*field.owner);
  return field.value;
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
std::vector<std::string> Vm::ExceptionStackTrace() const { return runtime_->ExceptionStackTrace(); }
void Vm::ClearException() { runtime_->exception = nullptr; }
GcStats Vm::Collect() { return runtime_->heap.Collect(*runtime_); }
void Vm::CollectIfNeeded() {
  if (!runtime_->gc_paused && runtime_->heap.ShouldCollect(runtime_->options.gc_interval))
    Collect();
}
GcStats Vm::HeapStats() const { return runtime_->heap.Stats(); }
MethodStats Vm::GetMethodStats(std::string_view owner, std::string_view name, std::string_view signature) {
  return runtime_->linker.FindMethod(runtime_->linker.Resolve(owner), name, signature, true).stats;
}
MethodStats Vm::GetInstanceMethodStats(std::string_view owner, std::string_view name,
                                       std::string_view signature) {
  return runtime_->linker.FindMethod(runtime_->linker.Resolve(owner), name, signature, false).stats;
}
size_t Vm::InterpretedMethodCount(std::string_view descriptor) const {
  const auto* cls = runtime_->linker.FindLinkedClass(descriptor);
  return cls ? std::ranges::count_if(cls->methods, [](const auto& entry) {
    return entry.second.stats.interpreted != 0;
  }) : 0;
}
size_t Vm::ExecutedOpcodeCount() const { return std::ranges::count(runtime_->executed_opcodes, 1); }
uint64_t Vm::NativeCalls(std::string_view owner) const { return runtime_->jni->Calls(owner); }
bool Vm::HasClass(std::string_view descriptor) const { return runtime_->linker.HasClass(descriptor); }
void Vm::SetGcPaused(bool paused) { runtime_->gc_paused = paused; }
void Vm::SetInlineCachesEnabled(bool enabled) { runtime_->inline_caches = enabled; }

ScopedHandleScope::ScopedHandleScope(Vm& vm) : roots_(std::make_unique<RootScope>(*vm.runtime_)) {}
ScopedHandleScope::~ScopedHandleScope() = default;
Object* ScopedHandleScope::Hold(Object* object) { return roots_->Hold(object); }
Value ScopedHandleScope::Hold(Value value) { return roots_->Hold(value); }

}  // namespace wa::interp
