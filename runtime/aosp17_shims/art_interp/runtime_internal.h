#pragma once

#include "art_interp.h"
#include "native_thunk.h"

#include <array>
#include <list>
#include <map>
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
  std::map<std::string, Field> fields;
  std::vector<Field*> field_order;
  std::map<std::string, Method> methods;
  std::unordered_map<std::string, Method*> vtable;
  std::unordered_map<Method*, Method*> itable;
  Object* mirror = nullptr;

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
  Method* reflected_method = nullptr;
  Object* weak_referent = nullptr;
  Object* next_free = nullptr;
  bool marked = false;
  bool dex_literal = false;
  bool read_only = false;
  unsigned monitor_depth = 0;
  uint32_t modifications = 0;
  std::vector<Value> fields;
  std::vector<Value> elements;
  std::u16string text;
  Value payload;
  std::unique_ptr<HashEntries> table;
};

class ClassLinker {
 public:
  explicit ClassLinker(Runtime& runtime) : runtime_(runtime) {}
  void LoadDexFile(std::string location, std::vector<uint8_t> bytes);
  Class& Define(std::string_view name, std::string_view super, uint32_t flags = 0,
                std::initializer_list<std::string_view> interfaces = {});
  Field& AddField(Class& cls, std::string_view name, std::string_view type, bool is_static,
                  Value initial = {});
  Method& AddMethod(Class& cls, std::string_view name, std::string_view signature, uint32_t flags,
                    Builtin builtin = {}, std::string_view shorty = {});
  Class& Resolve(std::string_view descriptor);
  bool HasClass(std::string_view descriptor) const;
  void Initialize(Class& cls);
  Method& FindMethod(Class& cls, std::string_view name, std::string_view signature, bool is_static);
  Method& VirtualMethod(Class& receiver, Method& method);
  Field& FindField(Class& cls, std::string_view name, std::string_view type, bool is_static);
  bool IsAssignable(const Class& target, const Class& source) const;
  void VisitRoots(const std::function<void(Object*)>& mark) const;

 private:
  struct DexUnit {
    std::vector<uint8_t> bytes;
    std::unique_ptr<const art::DexFile> dex;
  };

  void Link(Class& cls);
  void LoadMembers(Class& cls);
  void InitializeConstants(Class& cls);
  Runtime& runtime_;
  std::vector<DexUnit> dex_files_;
  std::map<std::string, Class, std::less<>> classes_;
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
  Value Invoke(Method& method, Object* receiver, std::span<const Value> args);
  Value Interpret(Method& method, Object* receiver, std::span<const Value> args);
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
  Value& ArrayElement(Object& array, int32_t index);
  void StoreArrayElement(Object& array, int32_t index, Value value);
  Object* MakeException(std::string_view type, std::string_view message);
  [[noreturn]] void Fail(std::string_view type, std::string_view message = {});
  [[noreturn]] void Throw(Object* exception);
  std::string ExceptionSummary() const;

  const Options options;
  ClassLinker linker;
  GcHeap heap;
  std::unique_ptr<JniBridge> jni;
  RootScope* roots = nullptr;
  Object* exception = nullptr;
  std::unordered_map<std::u16string, Object*> interned;  // Weak entries, pruned during sweep.
  std::vector<Object*> dex_literals;
  Class* string_class = nullptr;
  std::array<Value, 2048> frame_registers;
  size_t register_top = 0;
  std::array<uint8_t, 256> executed_opcodes{};
  size_t call_depth = 0;
  bool gc_paused = false;
  bool inline_caches = true;
};

}  // namespace wa::interp
