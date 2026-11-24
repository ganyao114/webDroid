#pragma once

#include "art_interp.h"
#include "native_thunk.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <list>
#include <map>
#include <set>
#include <unordered_map>

#include "dex/code_item_accessors.h"
#include "dex/dex_file.h"
#include "dex/modifiers.h"

namespace wa::interp {

struct Class;
struct Method;
class JniBridge;

using Builtin = std::function<Value(Runtime&, Object*, std::span<const Value>)>;

inline bool IsReference(char shorty) { return shorty == 'L' || shorty == '['; }
inline bool IsWide(char shorty) { return shorty == 'J' || shorty == 'D'; }

std::string MemberKey(std::string_view name, std::string_view signature);
std::string Shorty(std::string_view signature);
std::u16string DecodeUtf8(std::string_view text);
std::string EncodeUtf8(std::u16string_view text, bool modified = true);
inline Value Normalize(Value value, char type) {
  if (type == 'I' || type == 'F') return Value::Int(value.i());
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

struct Field {
  Class* owner;
  std::string name;
  std::string type;
  char value_type;
  bool is_static;
  uint32_t flags;
  size_t slot;
  Value value;
};

struct MemberCache {
  void* resolved = nullptr;  // Field, class or string, as determined by the site's opcode.
  Class* receiver = nullptr;
};

struct InvokeRegisters {
  InvokeRegisters(const art::Instruction& instruction, bool range);
  uint32_t operator[](size_t i) const { return range ? first + i : registers[i]; }
  size_t size() const { return count; }

  bool range;
  uint8_t count;
  uint16_t first = 0;
  std::array<uint32_t, art::Instruction::kMaxVarArgRegs> registers{};
};

struct InvokeCache {
  struct Dispatch {
    Class* receiver = nullptr;
    Method* target = nullptr;
  };

  explicit InvokeCache(InvokeRegisters operands) : operands(operands) {}

  InvokeRegisters operands;
  Class* owner = nullptr;
  Method* declaration = nullptr;
  std::array<Dispatch, 4> receivers{};
  unsigned replacement = 0;
};

struct MethodCodeCache {
  MethodCodeCache(const art::DexFile& dex, const art::dex::CodeItem* item);
  void VerifyRegisters(Runtime& runtime, const Method& method);
  void PrepareSites();

  art::CodeItemDataAccessor code;
  std::vector<uint8_t> instruction_widths;
  std::vector<uint32_t> site_indices;
  std::vector<MemberCache> members;
  std::vector<InvokeCache> invokes;
  bool sites_prepared = false;
};

struct Method {
  Class* owner;
  std::string name;
  std::string signature;
  std::string shorty;
  uint32_t flags;
  uint32_t index = 0;
  const art::dex::CodeItem* code = nullptr;
  Builtin builtin;
  NativeThunk native = nullptr;
  CompiledMethod compiled;
  MethodStats stats;
  std::unique_ptr<MethodCodeCache> code_cache;

  bool IsStatic() const { return (flags & art::kAccStatic) != 0; }
  bool IsNative() const { return (flags & art::kAccNative) != 0; }
  bool IsAbstract() const { return (flags & art::kAccAbstract) != 0; }
  bool IsVirtual() const;
  std::string QualifiedName() const;
};

enum class LinkState { Unlinked, Linking, Linked, Failed };
enum class InitState { Uninitialized, Initializing, Initialized, Failed };

struct Class {
  std::string descriptor;
  char kind = 'L';
  bool optional_boot_superclass = false;
  const art::DexFile* dex = nullptr;
  uint16_t definition = 0;
  uint32_t flags = 0;
  std::string superclass_descriptor;
  std::vector<std::string> interface_descriptors;
  Class* super = nullptr;
  Class* component = nullptr;
  std::vector<Class*> interfaces;
  LinkState link_state = LinkState::Unlinked;
  InitState init_state = InitState::Uninitialized;
  size_t instance_slots = 0;
  uint64_t native_calls = 0;
  std::map<std::string, Field> fields;
  std::vector<Field*> field_order;
  std::map<std::string, Method> methods;
  std::unordered_map<std::string, Method*> vtable;
  std::unordered_map<Method*, Method*> itable;
  Object* mirror = nullptr;
  Object* failure = nullptr;

  bool IsArray() const { return kind == '['; }
  bool IsInterface() const { return (flags & art::kAccInterface) != 0; }
  bool IsPrimitive() const { return !IsReference(kind); }
};

struct HashEntries {
  using Order = std::list<Object*>;
  Order order;
  std::unordered_multimap<int32_t, Order::iterator> buckets;
  size_t initial_capacity = 0;
  bool access_order = false;
};

struct Object {
  Class* type = nullptr;
  Class* represented_class = nullptr;  // Set for java.lang.Class instances.
  union {
    Method* reflected_method = nullptr;
    Field* reflected_field;
  };
  Object* weak_referent = nullptr;
  Object* weak_queue = nullptr;
  Object* next_free = nullptr;
  bool marked = false;
  bool dex_literal = false;
  bool read_only = false;
  bool stack_at_construction = false;
  unsigned monitor_depth = 0;
  uint32_t modifications = 0;
  std::vector<Value> fields;
  std::vector<Value> elements;
  std::u16string text;
  Value payload;
  std::unique_ptr<HashEntries> table;
  struct TraceFrame {
    const Method* method;
    uint32_t dex_pc;
  };
  std::unique_ptr<std::vector<TraceFrame>> exception_stack;
};

class DirectBufferMemory {
 public:
  explicit DirectBufferMemory(size_t size) : size(size) {}
  virtual ~DirectBufferMemory() = default;
  virtual bool Read(size_t offset, std::span<uint8_t> bytes) const = 0;
  virtual bool Write(size_t offset, std::span<const uint8_t> bytes) = 0;
  virtual uint64_t GuestAddress() const { return 0; }
  const size_t size;
};

using DirectArrays = std::unordered_map<const Object*, std::unique_ptr<DirectBufferMemory>>;

struct DirectBufferView {
  std::unique_ptr<DirectBufferMemory>* backing;
  size_t offset;
  int32_t capacity;
};

class ClassLinker {
 public:
  explicit ClassLinker(Runtime& runtime) : runtime_(runtime) {}
  void LoadDexFile(std::string location, std::vector<uint8_t> bytes, bool fallback = false);
  void LoadApkDexFiles(const std::string& path, bool fallback);
  std::optional<std::vector<uint8_t>> ReadApkResource(std::string_view name) const;
  DexStats GetDexStats() const;
  std::span<const std::unique_ptr<const art::DexFile>> LoadedDexFiles() const { return dex_files_; }
  std::string DexLocation(std::string_view descriptor) const;
  Class& Define(std::string_view name, std::string_view super, uint32_t flags = 0,
                std::initializer_list<std::string_view> interfaces = {});
  Field& AddField(Class& cls, std::string_view name, std::string_view type, bool is_static,
                  Value initial = {});
  Method& AddMethod(Class& cls, std::string_view name, std::string_view signature, uint32_t flags,
                    Builtin builtin = {}, std::string_view shorty = {});
  void AddBootNative(std::string_view owner, std::string_view name, std::string_view signature,
                     Builtin builtin);
  Class& Resolve(std::string_view descriptor);
  const Class* FindLinkedClass(std::string_view descriptor) const;
  bool HasClass(std::string_view descriptor) const;
  void Initialize(Class& cls);
  Method& FindMethod(Class& cls, std::string_view name, std::string_view signature, bool is_static,
                     bool dex_direct_invoke = false);
  Method& VirtualMethod(Class& receiver, Method& method);
  Field& FindField(Class& cls, std::string_view name, std::string_view type, bool is_static);
  bool IsAssignable(const Class& target, const Class& source) const;
  void VisitRoots(const std::function<void(Object*)>& mark) const;

 private:
  struct DexClass {
    const art::DexFile* dex;
    uint16_t definition;
    bool from_apk;
  };
  enum class DexSource { File, Fallback, Apk, FallbackApk };
  struct DescriptorHash {
    using is_transparent = void;
    size_t operator()(std::string_view name) const noexcept {
      return std::hash<std::string_view>{}(name);
    }
  };
  using DexFiles = std::vector<std::unique_ptr<const art::DexFile>>;

  void IndexDexFiles(DexFiles files, DexSource source);
  void Link(Class& cls);
  void LoadMembers(Class& cls);
  void InitializeConstants(Class& cls);
  Runtime& runtime_;
  DexFiles dex_files_;
  std::vector<std::string> apk_paths_;
  std::unordered_map<std::string_view, DexClass> dex_index_;
  std::unordered_map<std::string, Class, DescriptorHash, std::equal_to<>> classes_;
  std::map<std::string, std::map<std::string, Builtin>, std::less<>> boot_natives_;
  size_t linked_dex_classes_ = 0;
};

struct RootScope {
  explicit RootScope(Runtime& runtime, std::span<const Value> borrowed = {},
                     Object* receiver = nullptr);
  ~RootScope();
  RootScope(const RootScope&) = delete;
  RootScope& operator=(const RootScope&) = delete;

  Value Hold(Value value) { values.push_back(value); return value; }
  Object* Hold(Object* object) { return Hold(Value::Ref(object)).ref(); }

  Runtime& runtime;
  RootScope* previous;
  std::span<const Value> borrowed;
  Object* receiver;
  std::vector<Value> values;
  Class* declaring_class = nullptr;
};

class GcHeap {
 public:
  Object* Allocate(Runtime& runtime, Class& cls);
  bool ShouldCollect(size_t interval) const;
  GcStats Collect(Runtime& runtime);
  GcStats Stats() const { return stats_; }

 private:
  static constexpr size_t kSlabSize = 256;
  std::vector<std::unique_ptr<Object[]>> slabs_;
  std::vector<Object*> worklist_;
  Object* free_ = nullptr;
  size_t since_collection_ = 0;
  size_t next_collection_ = 0;
  GcStats stats_;
};

struct Runtime {
  explicit Runtime(Options options);
  ~Runtime();
  void InstallBootClasses();
  struct CallDepth {
    size_t& depth;
    explicit CallDepth(size_t& depth) : depth(depth) { ++depth; }
    ~CallDepth() { --depth; }
  };
  struct SuspendedFrame {
    Method* method;
    uint32_t dex_pc;
    std::vector<Value> slots;
    std::vector<Value> monitors;
  };
  [[gnu::always_inline]] Value Invoke(Method& method, Object* receiver, std::span<const Value> args) {
    if (args.size() + 1 != method.shorty.size())
      Fail("Ljava/lang/VerifyError;", "Wrong argument count for " + method.QualifiedName());
    if (call_depth >= 384) Fail("Ljava/lang/StackOverflowError;");
    if (!method.IsStatic()) NonNull(receiver);
    CallDepth depth(call_depth);

    constexpr auto monitor = art::kAccSynchronized | art::kAccDeclaredSynchronized;
    if (method.code && !method.compiled && !(method.flags & monitor) &&
        (!method.IsStatic() || method.owner->init_state == InitState::Initialized) &&
        (method.stats.invocations + 1 != options.hotness_threshold || !options.install_wasm)) {
      ++method.stats.invocations;
      ++method.stats.interpreted;
      const Value result = Interpret(method, receiver, args);
      ++method.stats.returned;
      return result;
    }
    return InvokeSlow(method, receiver, args);
  }
  Value InvokeSlow(Method& method, Object* receiver, std::span<const Value> args);
  Value Interpret(Method& method, Object* receiver, std::span<const Value> args,
                  std::span<const SuspendedFrame> suspended = {});
  Value ResumeSuspendedLoop(std::span<const SuspendedFrame> frames);
  Value DispatchMainMessage(Object& looper, Value now);
  [[noreturn]] void SuspendMainLooper();
  void StartThread(Object* thread);
  bool HasThread(const Object* thread) const;
  bool WakeThread(Object* thread, bool interrupt);
  void NotifyWaiters(Object& monitor, bool all);
  void RunReadyThreads();
  [[noreturn]] void SuspendWorker(std::optional<int64_t> delay_ns, bool interruptible,
                                  Object* monitor = nullptr);
  void Promote(Method& method);
  Object* NewObject(Class& cls, Method& constructor, std::span<const Value> args);
  Object* NewArray(Class& cls, int32_t length);
  Object* NewString(std::u16string text);
  Object* Intern(Object* string);
  Object* InternLiteral(std::u16string text);
  Object* ClassObject(Class& cls);
  Object& NonNull(Object* object) {
    if (!object) Fail("Ljava/lang/NullPointerException;");
    return *object;
  }
  size_t ArrayLength(const Object& array) const;
  Value ArrayElement(const Object& array, int32_t index);
  void StoreArrayElement(Object& array, int32_t index, Value value);
  DirectBufferMemory* DirectArray(const Object& array) const;
  Object* NewDirectBuffer(int32_t capacity, std::unique_ptr<DirectBufferMemory> memory = {});
  std::optional<DirectBufferView> DirectBuffer(Object* buffer);
  GuestNativeLibraries* NativeLibraries();
  void ReleaseGuestNativeSource() noexcept;
  Object* MakeException(std::string_view type, std::string_view message);
  void CaptureThrowableStack(Object& throwable);
  void AppendExceptionFrame(const Method& method, uint32_t pc);
  [[noreturn]] void Fail(std::string_view type, std::string_view message = {}, Object* cause = nullptr);
  [[noreturn]] void Throw(Object* exception);
  std::string ExceptionSummary() const;
  std::vector<std::string> ExceptionStackTrace() const;

  decltype(Options::system_properties) system_properties;
  const Options options;
  ClassLinker linker;
  GcHeap heap;
  std::unique_ptr<JniBridge> jni;
  GuestNativeFactory guest_native_factory = nullptr;
  std::shared_ptr<const void> guest_native_source;
  std::unique_ptr<GuestNativeLibraries> guest_native;
  std::function<void(std::string_view)> guest_library_observer;
  std::unique_ptr<DirectArrays> direct_arrays;
  struct GuestFile {
    std::vector<uint8_t> bytes;
    int64_t last_modified;
    Vm::FileReader reader;
    uint64_t backing_size = 0;

    uint64_t Size() const { return reader ? backing_size : bytes.size(); }
  };
  struct FileHandle {
    std::shared_ptr<GuestFile> file;
    uint64_t position = 0;
    bool readable;
    bool writable;
    bool append;
  };
  // An empty pointer denotes a directory. Open descriptors keep unlinked files alive.
  std::map<std::string, std::shared_ptr<GuestFile>, std::less<>> guest_files{{"/", {}}};
  std::set<std::string, std::less<>> read_only_paths;
  std::unordered_map<const Object*, FileHandle> file_handles;
  std::unordered_map<const Object*, std::shared_ptr<const void>> regex_programs;
  int32_t next_file_descriptor = 3;
  RootScope* roots = nullptr;
  Object* exception = nullptr;
  std::unordered_map<std::u16string, Object*> interned;  // Weak entries, pruned during sweep.
  std::vector<Object*> dex_literals;
  Class* string_class = nullptr;
  std::array<Value, 2048> frame_registers;
  size_t register_top = 0;
  std::array<uint8_t, 256> executed_opcodes{};
  size_t call_depth = 0;
  struct ActiveTraceFrame {
    Method* method;
    const uint32_t* dex_pc;
    ActiveTraceFrame* caller;
    const RootScope& frame_roots;
    size_t depth;
  };
  ActiveTraceFrame* active_trace = nullptr;
  struct LoopDispatch {
    ActiveTraceFrame* boundary;
    size_t depth;
  };
  const LoopDispatch* loop_dispatch = nullptr;
  std::vector<std::vector<SuspendedFrame>> suspended_loops;
  std::vector<SuspendedFrame> CaptureSuspendedFrames(const LoopDispatch& dispatch,
                                                      const Object* released_monitor = nullptr);
  struct Worker {
    Object* thread;
    std::vector<SuspendedFrame> frames;
    std::optional<std::chrono::steady_clock::time_point> due = std::chrono::steady_clock::time_point::min();
    bool interruptible = false;
    bool interrupted = false;
    Object* monitor = nullptr;
  };
  std::list<Worker> workers;
  Worker* active_worker = nullptr;
  const LoopDispatch* worker_dispatch = nullptr;
  bool gc_paused = false;
  bool inline_caches = true;
};

}  // namespace wa::interp
