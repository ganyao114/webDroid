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
#include <unordered_map>
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
  bool verify_dex = true;  // Disable only for trusted, previously verified inputs.
  bool capture_exception_stack = false;
  bool sandboxed_files = false;
  // Java's process state is supplied by the embedder.
  std::unordered_map<std::string, std::string> system_properties;
  std::unordered_map<std::string, std::string> environment;
};

struct DexFileStats {
  std::string location;
  size_t bytes;
  size_t class_definitions;
};

struct DexStats {
  size_t indexed_classes;
  size_t linked_classes;
  std::vector<DexFileStats> files;
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

  // Loads a raw DEX or an archive with consecutive classes*.dex entries as parent classes.
  void LoadDexFile(const std::string& path);
  void LoadDexFile(std::string location, std::vector<uint8_t> bytes);
  // Fills holes in the existing class path without replacing earlier definitions.
  void LoadFallbackDexFile(const std::string& path);
  void LoadFallbackDexFile(std::string location, std::vector<uint8_t> bytes);
  // Copies an immutable file into this VM's guest filesystem.
  void MountReadOnlyFile(std::string_view path, std::span<const uint8_t> bytes);
  // Adds the numbered DEX entries atomically, in DEX order. DEX files loaded
  // via LoadDexFile or LoadFallbackDexFile form the parent class path.
  // Keep the APK immutable while it is in use because entries can be file-backed.
  // A companion APK may reuse dependencies already indexed from the primary APK.
  void LoadApkDexFiles(const std::string& apk_path, bool fallback = false);
  DexStats GetDexStats() const;
  std::string DexLocation(std::string_view descriptor) const;
  void RegisterResourceNatives();
  // Supply a typed adapter before RegisterNatives for additional JNI libraries.
  void AddNativeAdapter(void* function, NativeThunk adapter);
  JNIEnv* JniEnv();

  Value CallStatic(std::string_view owner, std::string_view name, std::string_view signature,
                   std::span<const Value> args = {});
  Value CallVirtual(Object* receiver, std::string_view name, std::string_view signature,
                    std::span<const Value> args = {});
  Value ReadField(Object* receiver, std::string_view name, std::string_view descriptor);
  Value ReadStaticField(std::string_view owner, std::string_view name, std::string_view descriptor);
  Object* NewObject(std::string_view type, std::string_view constructor = "()V",
                    std::span<const Value> args = {});
  Object* NewString(std::string_view modified_utf8);
  std::string StringUtf8(const Object* string) const;
  std::string ExceptionSummary() const;
  std::vector<std::string> ExceptionStackTrace() const;
  void ClearException();

  GcStats Collect();
  void CollectIfNeeded();
  GcStats HeapStats() const;
  MethodStats GetMethodStats(std::string_view owner, std::string_view name,
                             std::string_view signature);
  MethodStats GetInstanceMethodStats(std::string_view owner, std::string_view name,
                                     std::string_view signature);
  size_t InterpretedMethodCount(std::string_view descriptor) const;
  size_t ExecutedOpcodeCount() const;
  static size_t SupportedOpcodeCount();
  uint64_t NativeCalls(std::string_view owner) const;

  // Looks up a DEX descriptor in the VM's class namespace.
  bool HasClass(std::string_view descriptor) const;
  // Defers allocation-triggered collections. Explicit Collect() remains available.
  void SetGcPaused(bool paused);
  // Switches resolution caches off or on for the same loaded methods.
  void SetInlineCachesEnabled(bool enabled);

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
