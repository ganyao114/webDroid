#pragma once

#include <jni.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wa::interp {

struct Object;
struct Runtime;
struct RootScope;

// References occupy their own slot, so primitive data is never traced as a pointer.
class Value {
 public:
  static Value Int(int32_t value) { return Bits(static_cast<uint32_t>(value)); }
  static Value Long(int64_t value) { return Bits(static_cast<uint64_t>(value)); }
  static Value Float(float value) { return Bits(std::bit_cast<uint32_t>(value)); }
  static Value Double(double value) { return Bits(std::bit_cast<uint64_t>(value)); }
  static Value Ref(Object* value) { return Value(0, value); }
  static Value Bits(uint64_t value) { return Value(value, nullptr); }

  Value() = default;
  uint64_t bits() const { return bits_; }
  int32_t i() const { return std::bit_cast<int32_t>(static_cast<uint32_t>(bits_)); }
  int64_t j() const { return std::bit_cast<int64_t>(bits_); }
  float f() const { return std::bit_cast<float>(static_cast<uint32_t>(bits_)); }
  double d() const { return std::bit_cast<double>(bits_); }
  Object* ref() const { return ref_; }

 private:
  Value(uint64_t bits, Object* ref) : bits_(bits), ref_(ref) {}
  uint64_t bits_ = 0;
  Object* ref_ = nullptr;
};

// A host engine installs the emitted, import-free module and returns a callable.
// The shorty gives the result type followed by the parameter types.
using CompiledMethod = std::function<Value(std::span<const Value>)>;
using InstallWasm = std::function<CompiledMethod(std::span<const uint8_t> module,
                                                 std::string_view export_name,
                                                 std::string_view shorty)>;
using NativeThunk = jvalue (*)(JNIEnv*, jobject, const jvalue*);

struct Options {
  size_t gc_interval = 1024;  // In allocations. Zero enables explicit collection only.
  uint64_t hotness_threshold = 0;
  InstallWasm install_wasm;
};

struct GcStats {
  size_t collections = 0;
  size_t live = 0;
  size_t reclaimed = 0;
};

struct MethodStats {
  uint64_t invocations = 0;
  uint64_t interpreted = 0;
  uint64_t tier1_hits = 0;
  size_t compiled_bytes = 0;
};

// A Java exception is pending in the VM when this is propagated to the host.
struct ManagedException {};

class Vm {
 public:
  explicit Vm(Options options = {});
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  void LoadDexFile(const std::string& path);
  void LoadDexFile(std::string location, std::vector<uint8_t> bytes);
  void RegisterResourceNatives();
  // Supply a typed adapter before RegisterNatives for additional JNI libraries.
  void AddNativeAdapter(void* function, NativeThunk adapter);
  JNIEnv* JniEnv();

  Value CallStatic(std::string_view owner, std::string_view name, std::string_view signature,
                   std::span<const Value> args = {});
  Value CallVirtual(Object* receiver, std::string_view name, std::string_view signature,
                    std::span<const Value> args = {});
  Object* NewObject(std::string_view type, std::string_view constructor = "()V",
                    std::span<const Value> args = {});
  Object* NewString(std::string_view modified_utf8);
  std::string StringUtf8(const Object* string) const;
  std::string ExceptionSummary() const;
  void ClearException();

  GcStats Collect();
  GcStats HeapStats() const;
  MethodStats GetMethodStats(std::string_view owner, std::string_view name,
                             std::string_view signature);
  size_t ExecutedOpcodeCount() const;
  static size_t SupportedOpcodeCount();
  uint64_t NativeCalls(std::string_view owner) const;

 private:
  friend class ScopedHandleScope;
  std::unique_ptr<Runtime> runtime_;
};

// Host references that survive another VM call or collection belong in this scope.
// Use a single VM thread and destroy scopes in reverse construction order.
class ScopedHandleScope {
 public:
  explicit ScopedHandleScope(Vm& vm);
  ~ScopedHandleScope();
  ScopedHandleScope(const ScopedHandleScope&) = delete;
  ScopedHandleScope& operator=(const ScopedHandleScope&) = delete;

  Object* Hold(Object* object);
  Value Hold(Value value);

 private:
  std::unique_ptr<RootScope> roots_;
};

}  // namespace wa::interp
