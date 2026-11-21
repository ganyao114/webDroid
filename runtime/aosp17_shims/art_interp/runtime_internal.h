#pragma once

#include "art_interp.h"
#include "native_thunk.h"

#include <array>
#include <bitset>
#include <map>
#include <unordered_map>

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
Value Normalize(Value value, char type);

struct Field {
  Class* owner;
  std::string name;
  std::string type;
  bool is_static;
  size_t slot;
  Value value;
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

  bool IsArray() const { return descriptor.front() == '['; }
  bool IsInterface() const { return (flags & art::kAccInterface) != 0; }
  bool IsPrimitive() const { return descriptor.size() == 1; }
};

struct Object {
  Class* type;
  Class* represented_class = nullptr;  // Set for java.lang.Class instances.
  bool marked = false;
  unsigned monitor_depth = 0;
  std::vector<Value> fields;
  std::vector<Value> elements;
  std::u16string text;
  Value payload;
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
  explicit RootScope(Runtime& runtime, size_t slots = 0);
  ~RootScope();
  RootScope(const RootScope&) = delete;
  RootScope& operator=(const RootScope&) = delete;

  Value Hold(Value value) { values.push_back(value); return value; }
  Object* Hold(Object* object) { return Hold(Value::Ref(object)).ref(); }

  Runtime& runtime;
  RootScope* previous;
  std::vector<Value> values;
};

class GcHeap {
 public:
  Object* Allocate(Runtime& runtime, Class& cls);
  GcStats Collect(Runtime& runtime);
  GcStats Stats() const { return stats_; }

 private:
  std::unordered_map<Object*, std::unique_ptr<Object>> objects_;
  size_t since_collection_ = 0;
  GcStats stats_;
};

struct Runtime {
  explicit Runtime(Options options);
  ~Runtime();
  void InstallBootClasses();
  Value Invoke(Method& method, Object* receiver, std::span<const Value> args);
  Value Interpret(const Method& method, Object* receiver, std::span<const Value> args);
  void Promote(Method& method);
  Object* NewObject(Class& cls, Method& constructor, std::span<const Value> args);
  Object* NewArray(Class& cls, int32_t length);
  Object* NewString(std::u16string text);
  Object* Intern(Object* string);
  Object* Intern(std::u16string text);
  Object* ClassObject(Class& cls);
  Object& NonNull(Object* object);
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
  std::bitset<256> executed_opcodes;
  size_t call_depth = 0;
};

}  // namespace wa::interp
