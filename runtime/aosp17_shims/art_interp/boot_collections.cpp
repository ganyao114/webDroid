#include "boot.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <tuple>

namespace wa::interp {
namespace {

constexpr std::string_view kCollection = "Ljava/util/Collection;";
constexpr std::string_view kList = "Ljava/util/List;";
constexpr std::string_view kArrayList = "Ljava/util/ArrayList;";
constexpr std::string_view kListIterator = "Ljava/util/ListIterator;";
constexpr std::string_view kIterator = "Ljava/util/Iterator;";
constexpr std::string_view kMap = "Ljava/util/Map;";
constexpr std::string_view kHashMap = "Ljava/util/HashMap;";
constexpr std::string_view kSet = "Ljava/util/Set;";
constexpr std::string_view kHashSet = "Ljava/util/HashSet;";
constexpr std::string_view kEntry = "Ljava/util/Map$Entry;";
constexpr std::string_view kObjectArray = "[Ljava/lang/Object;";
constexpr auto kListCursor = "Lwa/runtime/ListIterator;";
constexpr auto kMapCursor = "Lwa/runtime/MapIterator;";
constexpr auto kMapEntry = "Lwa/runtime/MapEntry;";
constexpr auto kReadOnlyList = "Lwa/runtime/UnmodifiableList;";
constexpr auto kReadOnlyCursor = "Lwa/runtime/UnmodifiableIterator;";
constexpr auto kReadOnlyMap = "Lwa/runtime/UnmodifiableMap;";
constexpr auto kReadOnlyEntry = "Lwa/runtime/UnmodifiableEntry;";
constexpr auto kReadOnlyCollectionCursor = "Lwa/runtime/UnmodifiableCollectionIterator;";
constexpr auto kReadOnlyEntryCursor = "Lwa/runtime/UnmodifiableEntryIterator;";
constexpr auto kComparator = "Ljava/util/Comparator;";

using Declaration = std::pair<const char*, const char*>;

void Abstract(ClassLinker& linker, Class& cls, std::initializer_list<Declaration> methods) {
  for (auto [name, signature] : methods) linker.AddMethod(cls, name, signature, art::kAccAbstract);
}

Value NoOp(Runtime&, Object*, Values) { return {}; }

void CheckMutable(Runtime& vm, const Object& object) {
  if (object.read_only) vm.Fail("Ljava/lang/UnsupportedOperationException;");
}

Value Unsupported(Runtime& vm, Object*, Values) {
  vm.Fail("Ljava/lang/UnsupportedOperationException;");
}

struct Interfaces {
  Class& collection;
  Class& list;
  Class& comparator;
};

Interfaces DeclareInterfaces(ClassLinker& cl) {
  auto& iterator = cl.Define(kIterator, kObject, kInterface);
  Abstract(cl, iterator, {{"hasNext", "()Z"}, {"next", "()Ljava/lang/Object;"}});
  cl.AddMethod(iterator, "remove", "()V", 0, Unsupported);
  Abstract(cl, cl.Define(kListIterator, kObject, kInterface, {kIterator}), {
      {"hasPrevious", "()Z"}, {"previous", "()Ljava/lang/Object;"}, {"nextIndex", "()I"},
      {"previousIndex", "()I"}, {"set", "(Ljava/lang/Object;)V"}, {"add", "(Ljava/lang/Object;)V"}});
  auto& iterable = cl.Define("Ljava/lang/Iterable;", kObject, kInterface);
  Abstract(cl, iterable, {{"iterator", "()Ljava/util/Iterator;"}});
  cl.AddMethod(iterable, "forEach", "(Ljava/util/function/Consumer;)V", 0,
      [](Runtime& vm, Object* self, Values a) {
        vm.NonNull(a[0].ref());
        auto* cursor = CallVirtual(vm, self, "iterator", "()Ljava/util/Iterator;").ref();
        return CallVirtual(vm, cursor, "forEachRemaining", "(Ljava/util/function/Consumer;)V", a);
      });
  cl.AddMethod(iterable, "spliterator", "()Ljava/util/Spliterator;", 0,
      [](Runtime& vm, Object* self, Values) {
        RootScope roots(vm);
        auto* cursor = roots.Hold(CallVirtual(vm, self, "iterator", "()Ljava/util/Iterator;").ref());
        auto& spliterators = vm.linker.Resolve("Ljava/util/Spliterators;");
        auto& factory = vm.linker.FindMethod(spliterators, "spliteratorUnknownSize",
            "(Ljava/util/Iterator;I)Ljava/util/Spliterator;", true);
        return vm.Invoke(factory, nullptr, std::array{Value::Ref(cursor), Value::Int(0)});
      });
  auto& collection = cl.Define(kCollection, kObject, kInterface, {"Ljava/lang/Iterable;"});
  cl.AddMethod(collection, "stream", "()Ljava/util/stream/Stream;", 0,
      [](Runtime& vm, Object* self, Values) {
        auto& stream = vm.linker.Resolve("Ljava/util/stream/Stream;");
        auto& factory = vm.linker.FindMethod(stream, "fromCollection",
            "(Ljava/util/Collection;)Ljava/util/stream/Stream;", true);
        return vm.Invoke(factory, nullptr, std::array{Value::Ref(self)});
      });
  Abstract(cl, collection, {
      {"size", "()I"}, {"isEmpty", "()Z"}, {"clear", "()V"}, {"add", "(Ljava/lang/Object;)Z"},
      {"remove", "(Ljava/lang/Object;)Z"}, {"contains", "(Ljava/lang/Object;)Z"},
      {"toArray", "()[Ljava/lang/Object;"}, {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"}});
  auto& list = cl.Define(kList, kObject, kInterface, {kCollection});
  Abstract(cl, list, {
      {"get", "(I)Ljava/lang/Object;"}, {"set", "(ILjava/lang/Object;)Ljava/lang/Object;"},
      {"add", "(ILjava/lang/Object;)V"}, {"remove", "(I)Ljava/lang/Object;"},
      {"indexOf", "(Ljava/lang/Object;)I"}, {"lastIndexOf", "(Ljava/lang/Object;)I"},
      {"listIterator", "()Ljava/util/ListIterator;"}, {"listIterator", "(I)Ljava/util/ListIterator;"},
      {"addAll", "(ILjava/util/Collection;)Z"}});
  cl.Define(kSet, kObject, kInterface, {kCollection});
  auto& map = cl.Define(kMap, kObject, kInterface);
  Abstract(cl, map, {
      {"size", "()I"}, {"isEmpty", "()Z"}, {"clear", "()V"},
      {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"containsKey", "(Ljava/lang/Object;)Z"},
      {"containsValue", "(Ljava/lang/Object;)Z"},
      {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
      {"putAll", "(Ljava/util/Map;)V"},
      {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"remove", "(Ljava/lang/Object;Ljava/lang/Object;)Z"},
      {"keySet", "()Ljava/util/Set;"}, {"values", "()Ljava/util/Collection;"}, {"entrySet", "()Ljava/util/Set;"}});
  cl.AddMethod(map, "getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        const auto value = CallVirtual(vm, self, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", a.first(1));
        return value.ref() || CallVirtual(vm, self, "containsKey", "(Ljava/lang/Object;)Z", a.first(1)).i()
            ? value : a[1];
      });
  cl.AddMethod(map, "putIfAbsent", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        const auto value = CallVirtual(vm, self, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", a.first(1));
        return value.ref() ? value
            : CallVirtual(vm, self, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", a);
      });
  cl.AddMethod(map, "computeIfAbsent", "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        auto* mapping = &vm.NonNull(a[1].ref());
        const auto value = CallVirtual(vm, self, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", a.first(1));
        if (value.ref()) return value;
        const auto computed = CallVirtual(vm, mapping, "apply", "(Ljava/lang/Object;)Ljava/lang/Object;", a.first(1));
        if (computed.ref()) {
          CallVirtual(vm, self, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                      std::array{a[0], computed});
        }
        return computed;
      });
  cl.AddMethod(map, "forEach", "(Ljava/util/function/BiConsumer;)V", 0,
      [](Runtime& vm, Object* self, Values a) {
        auto* action = &vm.NonNull(a[0].ref());
        RootScope roots(vm);
        auto* entries = roots.Hold(CallVirtual(vm, self, "entrySet", "()Ljava/util/Set;", {}).ref());
        auto* cursor = roots.Hold(CallVirtual(vm, entries, "iterator", "()Ljava/util/Iterator;", {}).ref());
        while (CallVirtual(vm, cursor, "hasNext", "()Z", {}).i()) {
          RootScope row_roots(vm);
          auto* entry = row_roots.Hold(CallVirtual(vm, cursor, "next", "()Ljava/lang/Object;", {}).ref());
          const auto key = row_roots.Hold(CallVirtual(vm, entry, "getKey", "()Ljava/lang/Object;", {}));
          const auto value = CallVirtual(vm, entry, "getValue", "()Ljava/lang/Object;", {});
          CallVirtual(vm, action, "accept", "(Ljava/lang/Object;Ljava/lang/Object;)V", std::array{key, value});
        }
        return Value{};
      });
  Abstract(cl, cl.Define(kEntry, kObject, kInterface), {
      {"getKey", "()Ljava/lang/Object;"}, {"getValue", "()Ljava/lang/Object;"},
      {"setValue", "(Ljava/lang/Object;)Ljava/lang/Object;"}});
  auto& comparator = cl.Define(kComparator, kObject, kInterface);
  Abstract(cl, comparator, {{"compare", "(Ljava/lang/Object;Ljava/lang/Object;)I"}});
  Abstract(cl, cl.Define("Ljava/util/function/Predicate;", kObject, kInterface),
      {{"test", "(Ljava/lang/Object;)Z"}});
  Abstract(cl, cl.Define("Ljava/util/function/Supplier;", kObject, kInterface),
      {{"get", "()Ljava/lang/Object;"}});
  Abstract(cl, cl.Define("Ljava/util/function/Function;", kObject, kInterface),
      {{"apply", "(Ljava/lang/Object;)Ljava/lang/Object;"}});
  Abstract(cl, cl.Define("Ljava/util/function/Consumer;", kObject, kInterface),
      {{"accept", "(Ljava/lang/Object;)V"}});
  cl.AddMethod(iterator, "forEachRemaining", "(Ljava/util/function/Consumer;)V", 0,
      [](Runtime& vm, Object* self, Values a) {
        auto* callback = &vm.NonNull(a[0].ref());
        while (CallVirtual(vm, self, "hasNext", "()Z").i()) {
          const auto value = CallVirtual(vm, self, "next", "()Ljava/lang/Object;");
          CallVirtual(vm, callback, "accept", "(Ljava/lang/Object;)V", std::array{value});
        }
        return Value{};
      });
  cl.Define("Ljava/util/RandomAccess;", kObject, kInterface);
  return {collection, list, comparator};
}

size_t Index(Runtime& vm, int32_t value, size_t size, bool insertion = false) {
  if (value < 0 || static_cast<size_t>(value) >= size + insertion) {
    vm.Fail("Ljava/lang/IndexOutOfBoundsException;", std::to_string(value));
  }
  return value;
}

int32_t JavaHash(Runtime& vm, Object* object) {
  if (!object) return 0;
  if (object->type == vm.string_class) {
    uint32_t hash = 0;
    for (char16_t ch : object->text) hash = hash * 31 + ch;
    return hash;
  }
  return CallVirtual(vm, object, "hashCode", "()I").i();
}

Value MakeArray(Runtime& vm, Values values, Object* supplied = nullptr) {
  RootScope roots(vm, values);
  Class& type = supplied ? *supplied->type : vm.linker.Resolve(kObjectArray);
  Object* array = supplied && supplied->elements.size() >= values.size()
      ? supplied : vm.NewArray(type, values.size());
  roots.Hold(array);
  for (size_t i = 0; i < values.size(); ++i) vm.StoreArrayElement(*array, i, values[i]);
  if (array->elements.size() > values.size()) array->elements[values.size()] = {};
  return Value::Ref(array);
}

Value MakeList(Runtime& vm, Values values, bool immutable = false) {
  RootScope roots(vm, values);
  auto* list = vm.heap.Allocate(vm, vm.linker.Resolve(kArrayList));
  list->elements.assign(values.begin(), values.end());
  list->read_only = immutable;
  return Value::Ref(list);
}

std::vector<Value>& ListElements(Object& list) {
  return list.payload.ref() ? list.payload.ref()->elements : list.elements;
}

void CheckListResize(Runtime& vm, const Object& list) {
  CheckMutable(vm, list);
  if (list.payload.ref()) vm.Fail("Ljava/lang/UnsupportedOperationException;");
}

Value ListSet(Runtime& vm, Object& list, size_t index, Value value) {
  CheckMutable(vm, list);
  auto& slot = ListElements(list)[index];
  const Value previous = slot;
  if (list.payload.ref()) vm.StoreArrayElement(*list.payload.ref(), index, value);
  else slot = value;
  return previous;
}

int32_t ListIndexOf(Runtime& vm, Object& list, Object* value, bool reverse) {
  auto& items = ListElements(list);
  const size_t size = items.size();
  const uint32_t version = list.modifications;
  for (size_t i = 0; i < size; ++i) {
    const size_t at = reverse ? size - i - 1 : i;
    const bool match = JavaEquals(vm, value, items[at].ref());
    if (version != list.modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
    if (match) return at;
  }
  return -1;
}

Value ListRemove(Runtime& vm, Object& list, size_t index) {
  CheckListResize(vm, list);
  const Value previous = list.elements[index];
  list.elements.erase(list.elements.begin() + index);
  ++list.modifications;
  return previous;
}

Value ListGet(Runtime& vm, Object* list, size_t index) {
  return CallVirtual(vm, list, "get", "(I)Ljava/lang/Object;", std::array{Value::Int(index)});
}

struct Ordering {
  Ordering(Runtime& vm, Object* comparator) : vm(vm), comparator(comparator) {
    if (comparator) {
      Class& type = *comparator->type;
      Method& method = vm.linker.FindMethod(type, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", false);
      compare = &vm.linker.VirtualMethod(type, method);
    }
  }

  int Compare(Value left, Value right) const {
    if (compare) return vm.Invoke(*compare, comparator, std::array{left, right}).i();
    return CallVirtual(vm, left.ref(), "compareTo", "(Ljava/lang/Object;)I", std::array{right}).i();
  }

  Runtime& vm;
  Object* comparator;
  Method* compare = nullptr;
};

void SortObjects(Runtime& vm, std::vector<Value>& values, Object* comparator) {
  const Ordering order(vm, comparator);
  // A comparator may call back into Java, mutating a container or causing collection.
  RootScope roots(vm);
  roots.values = values;
  std::stable_sort(values.begin(), values.end(),
                   [&](Value left, Value right) { return order.Compare(left, right) < 0; });
}

enum CursorField : size_t { Position, Last, Version, Kind, Snapshot };
enum CursorKind : int32_t { ListItems, Keys, MappedValues, Entries };

Object* NewCursor(Runtime& vm, Object& owner, CursorKind kind, size_t position = 0) {
  const auto descriptor = kind == ListItems ? kListCursor : kMapCursor;
  auto* cursor = vm.heap.Allocate(vm, vm.linker.Resolve(descriptor));
  cursor->payload = Value::Ref(&owner);
  cursor->elements = {Value::Int(position), Value::Int(-1), Value::Int(owner.modifications), Value::Int(kind)};
  if (kind != ListItems && owner.table) {
    for (auto* entry : owner.table->order) cursor->elements.push_back(Value::Ref(entry));
  }
  return cursor;
}

Object& CheckCursor(Runtime& vm, Object& cursor) {
  Object& owner = *cursor.payload.ref();
  if (cursor.elements[Version].bits() != owner.modifications) {
    vm.Fail("Ljava/util/ConcurrentModificationException;");
  }
  return owner;
}

size_t LastReturned(Runtime& vm, Object& cursor) {
  const int32_t last = cursor.elements[Last].i();
  if (last < 0) vm.Fail("Ljava/lang/IllegalStateException;");
  return last;
}

void InstallListCursor(ClassLinker& cl) {
  auto& cursor = cl.Define(kListCursor, kObject, art::kAccFinal, {kListIterator});
  cl.AddMethod(cursor, "hasNext", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->elements[Position].bits() != ListElements(*self->payload.ref()).size());
  });
  cl.AddMethod(cursor, "hasPrevious", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->elements[Position].i() != 0);
  });
  for (bool previous : {false, true}) {
    cl.AddMethod(cursor, previous ? "previous" : "next", "()Ljava/lang/Object;", 0,
        [previous](Runtime& vm, Object* self, Values) {
          const auto& items = ListElements(CheckCursor(vm, *self));
          const int32_t index = self->elements[Position].i() - previous;
          if (index < 0 || static_cast<size_t>(index) >= items.size()) vm.Fail("Ljava/util/NoSuchElementException;");
          self->elements[Last] = Value::Int(index);
          self->elements[Position] = Value::Int(index + !previous);
          return items[index];
        });
    cl.AddMethod(cursor, previous ? "previousIndex" : "nextIndex", "()I", 0,
        [previous](Runtime&, Object* self, Values) { return Value::Int(self->elements[Position].i() - previous); });
  }
  cl.AddMethod(cursor, "remove", "()V", 0, [](Runtime& vm, Object* self, Values) {
    auto& owner = CheckCursor(vm, *self);
    const size_t last = LastReturned(vm, *self);
    ListRemove(vm, owner, last);
    self->elements[Position] = Value::Int(last);
    self->elements[Last] = Value::Int(-1);
    self->elements[Version] = Value::Int(owner.modifications);
    return Value{};
  });
  cl.AddMethod(cursor, "set", "(Ljava/lang/Object;)V", 0, [](Runtime& vm, Object* self, Values a) {
    auto& owner = CheckCursor(vm, *self);
    ListSet(vm, owner, LastReturned(vm, *self), a[0]);
    return Value{};
  });
  cl.AddMethod(cursor, "add", "(Ljava/lang/Object;)V", 0, [](Runtime& vm, Object* self, Values a) {
    auto& owner = CheckCursor(vm, *self);
    CheckListResize(vm, owner);
    const size_t pos = self->elements[Position].bits();
    owner.elements.insert(owner.elements.begin() + pos, a[0]);
    self->elements[Position] = Value::Int(pos + 1);
    self->elements[Last] = Value::Int(-1);
    self->elements[Version] = Value::Int(++owner.modifications);
    return Value{};
  });
}

void InstallList(ClassLinker& cl, Class& iface) {
  InstallListCursor(cl);
  Class& list = cl.Define(kArrayList, kObject, 0,
      {kList, "Ljava/lang/Cloneable;", "Ljava/io/Serializable;", "Ljava/util/RandomAccess;"});
  const auto add = [&](auto name, auto sig, Builtin body) { cl.AddMethod(list, name, sig, 0, std::move(body)); };
  add("<init>", "()V", NoOp);
  add("<init>", "(I)V", [](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Negative capacity");
    self->elements.reserve(a[0].i());
    return Value{};
  });
  add("<init>", "(Ljava/util/Collection;)V", [](Runtime& vm, Object* self, Values a) {
    self->elements = vm.NonNull(CallVirtual(vm, a[0].ref(), "toArray", "()[Ljava/lang/Object;").ref()).elements;
    return Value{};
  });
  add("add", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckListResize(vm, *self);
    self->elements.push_back(a[0]);
    ++self->modifications;
    return Value::Int(1);
  });
  add("add", "(ILjava/lang/Object;)V", [](Runtime& vm, Object* self, Values a) {
    CheckListResize(vm, *self);
    const size_t at = Index(vm, a[0].i(), self->elements.size(), true);
    self->elements.insert(self->elements.begin() + at, a[1]);
    ++self->modifications;
    return Value{};
  });
  const Builtin add_all = [](Runtime& vm, Object* self, Values a) {
    CheckListResize(vm, *self);
    const size_t at = a.size() == 1 ? self->elements.size() : Index(vm, a[0].i(), self->elements.size(), true);
    const uint32_t version = self->modifications;
    const auto& additions = vm.NonNull(CallVirtual(vm, a.back().ref(), "toArray", "()[Ljava/lang/Object;").ref()).elements;
    if (version != self->modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
    if (additions.empty()) return Value::Int(0);
    self->elements.insert(self->elements.begin() + at, additions.begin(), additions.end());
    ++self->modifications;
    return Value::Int(1);
  };
  add("addAll", "(Ljava/util/Collection;)Z", add_all);
  add("addAll", "(ILjava/util/Collection;)Z", add_all);
  add("get", "(I)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    const auto& values = ListElements(*self);
    return values[Index(vm, a[0].i(), values.size())];
  });
  add("set", "(ILjava/lang/Object;)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    return ListSet(vm, *self, Index(vm, a[0].i(), ListElements(*self).size()), a[1]);
  });
  add("remove", "(I)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    return ListRemove(vm, *self, Index(vm, a[0].i(), ListElements(*self).size()));
  });
  add("remove", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckListResize(vm, *self);
    const int32_t at = ListIndexOf(vm, *self, a[0].ref(), false);
    if (at < 0) return Value::Int(0);
    ListRemove(vm, *self, at);
    return Value::Int(1);
  });
  for (bool last : {false, true}) {
    add(last ? "lastIndexOf" : "indexOf", "(Ljava/lang/Object;)I", [last](Runtime& vm, Object* self, Values a) {
      return Value::Int(ListIndexOf(vm, *self, a[0].ref(), last));
    });
  }
  add("contains", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(ListIndexOf(vm, *self, a[0].ref(), false) >= 0);
  });
  add("size", "()I", [](Runtime&, Object* self, Values) { return Value::Int(ListElements(*self).size()); });
  add("isEmpty", "()Z", [](Runtime&, Object* self, Values) { return Value::Int(ListElements(*self).empty()); });
  add("equals", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    auto* other = a[0].ref();
    if (self == other) return Value::Int(1);
    if (!other || !vm.linker.IsAssignable(vm.linker.Resolve(kList), *other->type)) return Value::Int(0);
    const auto values = ListElements(*self);
    RootScope roots(vm, values);
    if (CallVirtual(vm, other, "size", "()I").bits() != values.size()) return Value::Int(0);
    for (size_t i = 0; i < values.size(); ++i) {
      if (!JavaEquals(vm, values[i].ref(), ListGet(vm, other, i).ref())) return Value::Int(0);
    }
    return Value::Int(1);
  });
  add("hashCode", "()I", [](Runtime& vm, Object* self, Values) {
    const auto values = ListElements(*self);
    RootScope roots(vm, values);
    uint32_t hash = 1;
    for (Value item : values) hash = hash * 31 + JavaHash(vm, item.ref());
    return Value::Int(hash);
  });
  add("clear", "()V", [](Runtime& vm, Object* self, Values) {
    CheckListResize(vm, *self);
    self->elements.clear();
    ++self->modifications;
    return Value{};
  });
  add("ensureCapacity", "(I)V", [](Runtime&, Object* self, Values a) {
    if (a[0].i() > 0) self->elements.reserve(a[0].i());
    return Value{};
  });
  add("trimToSize", "()V", [](Runtime&, Object* self, Values) {
    self->elements.shrink_to_fit();
    return Value{};
  });
  const Builtin iter = [](Runtime& vm, Object* self, Values a) {
    const size_t start = a.empty() ? 0 : Index(vm, a[0].i(), ListElements(*self).size(), true);
    return Value::Ref(NewCursor(vm, *self, ListItems, start));
  };
  add("iterator", "()Ljava/util/Iterator;", iter);
  add("listIterator", "()Ljava/util/ListIterator;", iter);
  add("listIterator", "(I)Ljava/util/ListIterator;", iter);
  const Builtin to_array = [](Runtime& vm, Object* self, Values a) {
    return MakeArray(vm, ListElements(*self), a.empty() ? nullptr : &vm.NonNull(a[0].ref()));
  };
  add("toArray", "()[Ljava/lang/Object;", to_array);
  add("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", to_array);
  add("sort", "(Ljava/util/Comparator;)V", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const uint32_t version = self->modifications;
    auto values = ListElements(*self);
    SortObjects(vm, values, a[0].ref());
    if (version != self->modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
    ListElements(*self) = std::move(values);
    ++self->modifications;
    return Value{};
  });
  cl.AddMethod(iface, "sort", "(Ljava/util/Comparator;)V", 0, [](Runtime& vm, Object* self, Values a) {
    auto values = CallVirtual(vm, self, "toArray", "()[Ljava/lang/Object;").ref()->elements;
    RootScope roots(vm, values);
    SortObjects(vm, values, a[0].ref());
    for (size_t i = 0; i < values.size(); ++i) {
      CallVirtual(vm, self, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", std::array{Value::Int(i), values[i]});
    }
    return Value{};
  });

  const Builtin of = [](Runtime& vm, Object*, Values a) {
    for (Value value : a) vm.NonNull(value.ref());
    return MakeList(vm, a, true);
  };
  std::string parameters;
  for (int count = 0; count <= 10; ++count) {
    cl.AddMethod(iface, "of", "(" + parameters + ")Ljava/util/List;", art::kAccStatic, of);
    parameters += "Ljava/lang/Object;";
  }
  cl.AddMethod(iface, "of", "([Ljava/lang/Object;)Ljava/util/List;", art::kAccStatic,
      [of](Runtime& vm, Object*, Values a) { return of(vm, nullptr, vm.NonNull(a[0].ref()).elements); });
  cl.AddMethod(iface, "copyOf", "(Ljava/util/Collection;)Ljava/util/List;", art::kAccStatic,
      [of](Runtime& vm, Object*, Values a) {
        RootScope roots(vm);
        const auto array = roots.Hold(CallVirtual(vm, a[0].ref(), "toArray", "()[Ljava/lang/Object;"));
        return of(vm, nullptr, vm.NonNull(array.ref()).elements);
      });
}

struct MapLookup {
  int32_t hash;
  Object* entry = nullptr;
  HashEntries::Order::iterator position{};
};

MapLookup Lookup(Runtime& vm, Object& map, Object* key) {
  const int32_t hash = JavaHash(vm, key);
  if (map.table) {
    auto [first, last] = map.table->buckets.equal_range(hash);
    const auto version = map.modifications;
    for (; first != last; ++first) {
      auto pos = first->second;
      Object* entry = *pos;
      const bool match = JavaEquals(vm, key, entry->elements[0].ref());
      if (version != map.modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
      if (match) return {hash, entry, pos};
    }
  }
  return {hash};
}

HashEntries& Table(Object& map) {
  if (!map.table) {
    map.table = std::make_unique<HashEntries>();
    map.table->buckets.max_load_factor(0.75f);
  }
  return *map.table;
}

void InsertEntry(Runtime& vm, Object& map, int32_t hash, Value key, Value value) {
  auto* node = vm.heap.Allocate(vm, vm.linker.Resolve(kMapEntry));
  node->elements = {key, value};
  node->payload = Value::Int(hash);
  auto& table = Table(map);
  if (table.initial_capacity != 0) {
    table.buckets.rehash(table.initial_capacity);
    table.initial_capacity = 0;
  }
  const auto pos = table.order.insert(table.order.end(), node);
  try {
    table.buckets.emplace(hash, pos);
  } catch (...) {
    table.order.erase(pos);
    throw;
  }
  ++map.modifications;
}

bool EraseEntry(Object& map, Object* node) {
  if (!map.table) return false;
  const auto [first, last] = map.table->buckets.equal_range(node->payload.i());
  for (auto it = first; it != last; ++it) {
    if (*it->second != node) continue;
    map.table->order.erase(it->second);
    map.table->buckets.erase(it);
    ++map.modifications;
    return true;
  }
  return false;
}

void AccessEntry(Object& map, MapLookup match) {
  if (map.table->access_order && std::next(match.position) != map.table->order.end()) {
    map.table->order.splice(map.table->order.end(), map.table->order, match.position);
    ++map.modifications;
  }
}

Value MapPut(Runtime& vm, Object& map, Value key, Value value, bool only_absent) {
  CheckMutable(vm, map);
  const auto match = Lookup(vm, map, key.ref());
  if (!match.entry) {
    InsertEntry(vm, map, match.hash, key, value);
    return {};
  }
  const auto previous = match.entry->elements[1];
  if (!only_absent || !previous.ref()) match.entry->elements[1] = value;
  AccessEntry(map, match);
  return previous;
}

Value MapSize(Runtime&, Object* self, Values) {
  Object& map = self->payload.ref() ? *self->payload.ref() : *self;
  return Value::Int(map.table ? map.table->order.size() : 0);
}

Value ClearMap(Runtime& vm, Object* self, Values) {
  Object& map = self->payload.ref() ? *self->payload.ref() : *self;
  CheckMutable(vm, map);
  if (map.table) {
    map.table->buckets.clear();
    map.table->order.clear();
    ++map.modifications;
  }
  return {};
}

Object* FindValue(Runtime& vm, Object& map, Object* value) {
  if (!map.table) return nullptr;
  const auto version = map.modifications;
  for (auto* entry : map.table->order) {
    const bool match = JavaEquals(vm, value, entry->elements[1].ref());
    if (version != map.modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
    if (match) return entry;
  }
  return nullptr;
}

Value EntryValue(Object& entry, CursorKind kind) {
  if (kind == Entries) return Value::Ref(&entry);
  return entry.elements[kind == MappedValues];
}

constexpr std::array kMapViews{"Lwa/runtime/MapKeys;", "Lwa/runtime/MapValues;", "Lwa/runtime/MapEntries;"};

void InstallMapCursor(ClassLinker& cl) {
  auto& cursor = cl.Define(kMapCursor, kObject, art::kAccFinal, {kIterator});
  cl.AddMethod(cursor, "hasNext", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(Snapshot + self->elements[Position].bits() < self->elements.size());
  });
  cl.AddMethod(cursor, "next", "()Ljava/lang/Object;", 0, [](Runtime& vm, Object* self, Values) {
    CheckCursor(vm, *self);
    const auto at = self->elements[Position].bits();
    if (Snapshot + at >= self->elements.size()) vm.Fail("Ljava/util/NoSuchElementException;");
    self->elements[Last] = Value::Int(at);
    self->elements[Position] = Value::Int(at + 1);
    return EntryValue(*self->elements[Snapshot + at].ref(), static_cast<CursorKind>(self->elements[Kind].i()));
  });
  cl.AddMethod(cursor, "remove", "()V", 0, [](Runtime& vm, Object* self, Values) {
    auto& owner = CheckCursor(vm, *self);
    CheckMutable(vm, owner);
    const size_t last = LastReturned(vm, *self);
    EraseEntry(owner, self->elements[Snapshot + last].ref());
    self->elements[Snapshot + last] = {};
    self->elements[Last] = Value::Int(-1);
    self->elements[Version] = Value::Int(owner.modifications);
    return Value{};
  });
}

void InstallMapEntry(ClassLinker& cl) {
  auto& node = cl.Define(kMapEntry, kObject, art::kAccFinal, {kEntry});
  cl.AddMethod(node, "getKey", "()Ljava/lang/Object;", 0,
      [](Runtime&, Object* self, Values) { return self->elements[0]; });
  cl.AddMethod(node, "getValue", "()Ljava/lang/Object;", 0,
      [](Runtime&, Object* self, Values) { return self->elements[1]; });
  cl.AddMethod(node, "setValue", "(Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime&, Object* self, Values a) {
        const auto previous = self->elements[1];
        self->elements[1] = a[0];
        return previous;
      });
  cl.AddMethod(node, "equals", "(Ljava/lang/Object;)Z", 0, [](Runtime& vm, Object* self, Values a) {
    Object* other = a[0].ref();
    if (self == other) return Value::Int(1);
    if (!other || !vm.linker.IsAssignable(vm.linker.Resolve(kEntry), *other->type)) return Value::Int(0);
    const auto key = CallVirtual(vm, other, "getKey", "()Ljava/lang/Object;");
    if (!JavaEquals(vm, self->elements[0].ref(), key.ref())) return Value::Int(0);
    const auto value = CallVirtual(vm, other, "getValue", "()Ljava/lang/Object;");
    return Value::Int(JavaEquals(vm, self->elements[1].ref(), value.ref()));
  });
  cl.AddMethod(node, "hashCode", "()I", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(JavaHash(vm, self->elements[0].ref()) ^ JavaHash(vm, self->elements[1].ref()));
  });
}

Object* ViewMatch(Runtime& vm, Object& map, CursorKind kind, Object* value) {
  if (kind == Keys) return Lookup(vm, map, value).entry;
  if (kind == MappedValues) return FindValue(vm, map, value);
  if (!value || !vm.linker.IsAssignable(vm.linker.Resolve(kEntry), *value->type)) return nullptr;
  RootScope roots(vm);
  const auto key = roots.Hold(CallVirtual(vm, value, "getKey", "()Ljava/lang/Object;"));
  const auto val = roots.Hold(CallVirtual(vm, value, "getValue", "()Ljava/lang/Object;"));
  auto* entry = roots.Hold(Lookup(vm, map, key.ref()).entry);
  return entry && JavaEquals(vm, val.ref(), entry->elements[1].ref()) ? entry : nullptr;
}

void InstallMapViews(ClassLinker& cl) {
  for (const auto kind : {Keys, MappedValues, Entries}) {
    auto& view = cl.Define(kMapViews[kind - Keys], kObject, art::kAccFinal,
                           {kind == MappedValues ? kCollection : kSet});
    const auto add = [&](auto name, auto sig, Builtin fn) { cl.AddMethod(view, name, sig, 0, std::move(fn)); };
    add("size", "()I", MapSize);
    add("isEmpty", "()Z", [](Runtime& vm, Object* self, Values a) { return Value::Int(!MapSize(vm, self, a).i()); });
    add("clear", "()V", ClearMap);
    add("contains", "(Ljava/lang/Object;)Z", [kind](Runtime& vm, Object* self, Values a) {
      return Value::Int(ViewMatch(vm, *self->payload.ref(), kind, a[0].ref()) != nullptr);
    });
    add("remove", "(Ljava/lang/Object;)Z", [kind](Runtime& vm, Object* self, Values a) {
      Object& owner = *self->payload.ref();
      CheckMutable(vm, owner);
      auto* entry = ViewMatch(vm, owner, kind, a[0].ref());
      return Value::Int(entry && EraseEntry(owner, entry));
    });
    add("add", "(Ljava/lang/Object;)Z", Unsupported);
    add("iterator", "()Ljava/util/Iterator;", [kind](Runtime& vm, Object* self, Values) {
      return Value::Ref(NewCursor(vm, *self->payload.ref(), kind));
    });
    const Builtin to_array = [kind](Runtime& vm, Object* self, Values a) {
      std::vector<Value> values;
      if (const auto& table = self->payload.ref()->table) {
        values.reserve(table->order.size());
        for (auto* entry : table->order) values.push_back(EntryValue(*entry, kind));
      }
      return MakeArray(vm, values, a.empty() ? nullptr : &vm.NonNull(a[0].ref()));
    };
    add("toArray", "()[Ljava/lang/Object;", to_array);
    add("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", to_array);
  }
}

void CopyMap(Runtime& vm, Object& to, Object* from) {
  vm.NonNull(from);
  if (&to == from) return;
  RootScope roots(vm);
  const auto entries = roots.Hold(CallVirtual(vm, from, "entrySet", "()Ljava/util/Set;"));
  const auto iter = roots.Hold(CallVirtual(vm, entries.ref(), "iterator", "()Ljava/util/Iterator;"));
  while (CallVirtual(vm, iter.ref(), "hasNext", "()Z").i()) {
    RootScope item(vm);
    const auto entry = item.Hold(CallVirtual(vm, iter.ref(), "next", "()Ljava/lang/Object;"));
    const auto key = item.Hold(CallVirtual(vm, entry.ref(), "getKey", "()Ljava/lang/Object;"));
    const auto value = item.Hold(CallVirtual(vm, entry.ref(), "getValue", "()Ljava/lang/Object;"));
    MapPut(vm, to, key, value, false);
  }
}

void CopySet(Runtime& vm, Object& to, Object* from) {
  RootScope roots(vm);
  const auto values = roots.Hold(CallVirtual(vm, from, "toArray", "()[Ljava/lang/Object;"));
  for (Value value : vm.NonNull(values.ref()).elements) {
    const auto found = Lookup(vm, to, value.ref());
    if (!found.entry) InsertEntry(vm, to, found.hash, value, {});
  }
}

void InstallMaps(ClassLinker& cl) {
  InstallMapEntry(cl);
  InstallMapCursor(cl);
  InstallMapViews(cl);
  auto& map = cl.Define(kHashMap, "Ljava/util/AbstractMap;", 0, {kMap, "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
  // The standalone VM profiles can run with only the boot collections installed.
  map.optional_boot_superclass = true;
  cl.AddMethod(map, "tableSizeFor", "(I)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    const auto capacity = static_cast<uint32_t>(std::clamp(a[0].i(), 1, 1 << 30));
    return Value::Int(std::bit_ceil(capacity));
  });
  auto& ordered = cl.Define("Ljava/util/LinkedHashMap;", map.descriptor);
  auto& set = cl.Define(kHashSet, "Ljava/util/AbstractSet;", 0, {kSet, "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
  set.optional_boot_superclass = true;
  auto& ordered_set = cl.Define("Ljava/util/LinkedHashSet;", set.descriptor);
  for (Class* type : {&map, &ordered, &set, &ordered_set}) {
    cl.AddMethod(*type, "<init>", "()V", 0, NoOp);
    const Builtin capacity = [](Runtime& vm, Object* self, Values a) {
      if (a[0].i() < 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Negative capacity");
      const auto load = a.size() >= 2 ? a[1].f() : 0.75f;
      if (!(load > 0)) vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid load factor");
      auto& table = Table(*self);
      if (std::isfinite(load)) table.buckets.max_load_factor(load);
      table.initial_capacity = std::min<int32_t>(1 << 30, a[0].i());
      table.access_order = a.size() == 3 && a[2].i();
      return Value{};
    };
    cl.AddMethod(*type, "<init>", "(I)V", 0, capacity);
    cl.AddMethod(*type, "<init>", "(IF)V", 0, capacity);
    if (type == &ordered) cl.AddMethod(*type, "<init>", "(IFZ)V", 0, capacity);
    if (type == &set || type == &ordered_set) {
      cl.AddMethod(*type, "<init>", "(Ljava/util/Collection;)V", 0,
          [](Runtime& vm, Object* self, Values a) { CopySet(vm, *self, a[0].ref()); return Value{}; });
    } else {
      cl.AddMethod(*type, "<init>", "(Ljava/util/Map;)V", 0,
          [](Runtime& vm, Object* self, Values a) { CopyMap(vm, *self, a[0].ref()); return Value{}; });
    }
  }

  const auto add = [&](auto name, auto sig, Builtin fn) { cl.AddMethod(map, name, sig, 0, std::move(fn)); };
  const Builtin get = [](Runtime& vm, Object* self, Values a) {
    const auto match = Lookup(vm, *self, a[0].ref());
    if (!match.entry) return a.size() == 2 ? a[1] : Value{};
    AccessEntry(*self, match);
    return match.entry->elements[1];
  };
  add("get", "(Ljava/lang/Object;)Ljava/lang/Object;", get);
  add("getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", get);
  add("containsKey", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(Lookup(vm, *self, a[0].ref()).entry != nullptr);
  });
  add("containsValue", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(FindValue(vm, *self, a[0].ref()) != nullptr);
  });
  for (bool absent : {false, true}) {
    add(absent ? "putIfAbsent" : "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [absent](Runtime& vm, Object* self, Values a) { return MapPut(vm, *self, a[0], a[1], absent); });
  }
  add("putAll", "(Ljava/util/Map;)V", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    CopyMap(vm, *self, a[0].ref());
    return Value{};
  });
  const Builtin remove = [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const auto match = Lookup(vm, *self, a[0].ref());
    if (!match.entry) return Value{};
    const auto old = match.entry->elements[1];
    if (a.size() == 2) {
      const auto version = self->modifications;
      if (!JavaEquals(vm, a[1].ref(), old.ref())) return Value::Int(0);
      if (version != self->modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
    }
    EraseEntry(*self, match.entry);
    return a.size() == 2 ? Value::Int(1) : old;
  };
  add("remove", "(Ljava/lang/Object;)Ljava/lang/Object;", remove);
  add("remove", "(Ljava/lang/Object;Ljava/lang/Object;)Z", remove);
  add("size", "()I", MapSize);
  add("isEmpty", "()Z", [](Runtime& vm, Object* self, Values a) { return Value::Int(!MapSize(vm, self, a).i()); });
  add("clear", "()V", ClearMap);
  for (const auto kind : {Keys, MappedValues, Entries}) {
    constexpr std::array names{"keySet", "values", "entrySet"};
    const size_t index = kind - Keys;
    add(names[index], kind == MappedValues ? "()Ljava/util/Collection;" : "()Ljava/util/Set;",
        [index](Runtime& vm, Object* self, Values) {
          if (self->elements.empty()) self->elements.resize(kMapViews.size());
          if (!self->elements[index].ref()) {
            auto* view = vm.heap.Allocate(vm, vm.linker.Resolve(kMapViews[index]));
            view->payload = Value::Ref(self);
            self->elements[index] = Value::Ref(view);
          }
          return self->elements[index];
        });
  }

  const auto bind = [&](auto name, auto sig, Builtin fn) { cl.AddMethod(set, name, sig, 0, std::move(fn)); };
  bind("size", "()I", MapSize);
  bind("isEmpty", "()Z", [](Runtime& vm, Object* self, Values a) { return Value::Int(!MapSize(vm, self, a).i()); });
  bind("clear", "()V", ClearMap);
  bind("contains", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(Lookup(vm, *self, a[0].ref()).entry != nullptr);
  });
  bind("add", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const auto match = Lookup(vm, *self, a[0].ref());
    if (match.entry) return Value::Int(0);
    InsertEntry(vm, *self, match.hash, a[0], {});
    return Value::Int(1);
  });
  bind("remove", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const auto match = Lookup(vm, *self, a[0].ref());
    return Value::Int(match.entry && EraseEntry(*self, match.entry));
  });
  bind("iterator", "()Ljava/util/Iterator;", [](Runtime& vm, Object* self, Values) {
    return Value::Ref(NewCursor(vm, *self, Keys));
  });
  const Builtin to_array = [](Runtime& vm, Object* self, Values a) {
    std::vector<Value> keys;
    if (self->table) {
      keys.reserve(self->table->order.size());
      for (auto* entry : self->table->order) keys.push_back(entry->elements[0]);
    }
    return MakeArray(vm, keys, a.empty() ? nullptr : &vm.NonNull(a[0].ref()));
  };
  bind("toArray", "()[Ljava/lang/Object;", to_array);
  bind("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", to_array);
}

std::pair<size_t, size_t> ArrayRange(Runtime& vm, const Object& array, int32_t from, int32_t to) {
  if (from > to) vm.Fail("Ljava/lang/IllegalArgumentException;");
  if (from < 0 || static_cast<size_t>(to) > array.elements.size()) {
    vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
  }
  return {from, to};
}

template<class Real> int RealCompare(Real a, Real b) {
  if (std::isnan(a)) return !std::isnan(b);
  if (std::isnan(b)) return -1;
  if (a == 0 && b == 0) return std::signbit(b) - std::signbit(a);
  return (a > b) - (a < b);
}

int PrimitiveCompare(char type, Value a, Value b) {
  if (type == 'F') return RealCompare(a.f(), b.f());
  if (type == 'D') return RealCompare(a.d(), b.d());
  if (type == 'J') return (a.j() > b.j()) - (a.j() < b.j());
  return (a.i() > b.i()) - (a.i() < b.i());
}

int PrimitiveHash(char type, Value value) {
  if (type == 'Z') return value.i() ? 1231 : 1237;
  if (type == 'F') return std::isnan(value.f()) ? 0x7fc00000 : value.i();
  if (type == 'D' && std::isnan(value.d())) return 0x7ff80000;
  return static_cast<int32_t>(value.bits() ^ (value.bits() >> 32));
}

Builtin CopyArray(bool range) {
  return [range](Runtime& vm, Object*, Values a) {
    auto& array = vm.NonNull(a[0].ref());
    const int32_t start = range ? a[1].i() : 0;
    const int32_t end = a[range ? 2 : 1].i();
    if (range && start > end) vm.Fail("Ljava/lang/IllegalArgumentException;");
    if (start < 0 || static_cast<size_t>(start) > array.elements.size()) {
      vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
    }
    Class* type = array.type;
    if (a.size() == (range ? 4 : 3)) {
      type = vm.NonNull(a.back().ref()).represented_class;
      if (!type->IsArray() || type->component->IsPrimitive()) vm.Fail("Ljava/lang/ClassCastException;");
    }
    auto* copy = vm.NewArray(*type, end - start);
    const size_t count = std::min(array.elements.size() - start, copy->elements.size());
    if (type == array.type) {
      std::copy_n(array.elements.begin() + start, count, copy->elements.begin());
    } else {
      for (size_t i = 0; i < count; ++i) vm.StoreArrayElement(*copy, i, array.elements[start + i]);
    }
    return Value::Ref(copy);
  };
}

void AppendArray(Runtime& vm, Object* array, std::u16string& text, std::vector<Object*>& parents) {
  if (std::ranges::contains(parents, array)) {
    text += u"[...]";
    return;
  }
  parents.push_back(array);
  text += u'[';
  const auto& component = *array->type->component;
  for (size_t i = 0; i < array->elements.size(); ++i) {
    if (i) text += u", ";
    const auto item = array->elements[i];
    if (component.IsPrimitive()) {
      char type = component.descriptor.front();
      std::string signature = "(I)Ljava/lang/String;";
      if (type != 'B' && type != 'S') signature[1] = type;
      auto& value_of = vm.linker.FindMethod(*vm.string_class, "valueOf", signature, true);
      text += vm.Invoke(value_of, nullptr, std::array{item}).ref()->text;
    } else if (auto* object = item.ref(); object && object->type->IsArray()) {
      AppendArray(vm, object, text, parents);
    } else {
      text += JavaText(vm, item.ref());
    }
  }
  text += u']';
  parents.pop_back();
}

void InstallArrays(ClassLinker& cl) {
  auto& arrays = cl.Define("Ljava/util/Arrays;", kObject, art::kAccFinal);
  const auto add = [&](auto name, auto sig, Builtin fn) {
    cl.AddMethod(arrays, name, sig, art::kAccStatic, std::move(fn));
  };
  add("stream", "([Ljava/lang/Object;)Ljava/util/stream/Stream;", [](Runtime& vm, Object*, Values a) {
    auto& stream = vm.linker.Resolve("Ljava/util/stream/Stream;");
    auto& from_array = vm.linker.FindMethod(stream, "fromArray",
        "([Ljava/lang/Object;)Ljava/util/stream/Stream;", true);
    return vm.Invoke(from_array, nullptr, a);
  });
  add("asList", "([Ljava/lang/Object;)Ljava/util/List;", [](Runtime& vm, Object*, Values a) {
    vm.NonNull(a[0].ref());
    auto* list = vm.heap.Allocate(vm, vm.linker.Resolve(kArrayList));
    list->payload = a[0];
    return Value::Ref(list);
  });
  add("toString", "([Ljava/lang/Object;)Ljava/lang/String;", [](Runtime& vm, Object*, Values a) {
    if (!a[0].ref()) return StringValue(vm, u"null");
    std::u16string text = u"[";
    const auto& items = a[0].ref()->elements;
    for (size_t i = 0; i < items.size(); ++i) {
      if (i) text += u", ";
      text += items[i].ref() == a[0].ref() ? u"(this array)" : JavaText(vm, items[i].ref());
    }
    text += u']';
    return StringValue(vm, std::move(text));
  });
  add("deepToString", "([Ljava/lang/Object;)Ljava/lang/String;", [](Runtime& vm, Object*, Values a) {
    auto* array = a[0].ref();
    if (!array) return StringValue(vm, u"null");
    std::u16string text;
    std::vector<Object*> parents;
    AppendArray(vm, array, text, parents);
    return StringValue(vm, std::move(text));
  });
  for (char kind : std::string_view("ZBCSIJFDL")) {
    const std::string component = kind == 'L' ? "Ljava/lang/Object;" : std::string(1, kind);
    const std::string array = "[" + component;
    add("copyOf", "(" + array + "I)" + array, CopyArray(false));
    add("copyOfRange", "(" + array + "II)" + array, CopyArray(true));
    const Builtin fill = [](Runtime& vm, Object*, Values a) {
      auto& array = vm.NonNull(a[0].ref());
      const auto [first, last] = a.size() == 2 ? std::pair<size_t, size_t>{0, array.elements.size()}
          : ArrayRange(vm, array, a[1].i(), a[2].i());
      if (first != last) {
        vm.StoreArrayElement(array, first, a.back());
        std::fill(array.elements.begin() + first + 1, array.elements.begin() + last, array.elements[first]);
      }
      return Value{};
    };
    add("fill", "(" + array + component + ")V", fill);
    add("fill", "(" + array + "II" + component + ")V", fill);
    add("equals", "(" + array + array + ")Z", [kind](Runtime& vm, Object*, Values a) {
      const auto* left = a[0].ref();
      const auto* right = a[1].ref();
      if (left == right) return Value::Int(1);
      if (!left || !right || left->elements.size() != right->elements.size()) return Value::Int(0);
      for (size_t i = 0; i < left->elements.size(); ++i) {
        const auto l = left->elements[i], r = right->elements[i];
        if (kind == 'L' ? !JavaEquals(vm, l.ref(), r.ref()) : PrimitiveCompare(kind, l, r) != 0) {
          return Value::Int(0);
        }
      }
      return Value::Int(1);
    });
    add("hashCode", "(" + array + ")I", [kind](Runtime& vm, Object*, Values a) {
      const auto* array = a[0].ref();
      if (!array) return Value::Int(0);
      uint32_t hash = 1;
      for (size_t i = 0; i < array->elements.size(); ++i) {
        const auto item = array->elements[i];
        hash = hash * 31 + (kind == 'L' ? JavaHash(vm, item.ref()) : PrimitiveHash(kind, item));
      }
      return Value::Int(hash);
    });
    if (kind == 'Z') continue;
    for (bool range : {false, true}) {
      const std::string parameters = "(" + array + (range ? "II" : "");
      const Builtin sort = [kind, range](Runtime& vm, Object*, Values a) {
        auto& array = vm.NonNull(a[0].ref());
        const auto [first, last] = range ? ArrayRange(vm, array, a[1].i(), a[2].i())
            : std::pair<size_t, size_t>{0, array.elements.size()};
        if (first == last) return Value{};
        auto begin = array.elements.begin() + first;
        auto end = array.elements.begin() + last;
        if (kind == 'L') {
          std::vector<Value> values(begin, end);
          SortObjects(vm, values, a.size() > (range ? 3 : 1) ? a.back().ref() : nullptr);
          std::copy(values.begin(), values.end(), array.elements.begin() + first);
        } else {
          std::sort(begin, end, [kind](Value a, Value b) { return PrimitiveCompare(kind, a, b) < 0; });
        }
        return Value{};
      };
      const Builtin search = [kind, range](Runtime& vm, Object*, Values a) {
        auto& array = vm.NonNull(a[0].ref());
        auto [first, last] = range ? ArrayRange(vm, array, a[1].i(), a[2].i())
            : std::pair<size_t, size_t>{0, array.elements.size()};
        const size_t key = range ? 3 : 1;
        const Ordering order(vm, a.size() > key + 1 ? a.back().ref() : nullptr);
        while (first < last) {
          const size_t mid = first + (last - first - 1) / 2;
          const Value item = array.elements[mid];
          const int cmp = kind == 'L' ? order.Compare(item, a[key]) : PrimitiveCompare(kind, item, a[key]);
          if (cmp == 0) return Value::Int(mid);
          if (cmp < 0) first = mid + 1;
          else last = mid;
        }
        return Value::Int(-static_cast<int32_t>(first) - 1);
      };
      add("sort", parameters + ")V", sort);
      add("binarySearch", parameters + component + ")I", search);
      if (kind == 'L') {
        add("sort", parameters + kComparator + ")V", sort);
        add("binarySearch", parameters + component + kComparator + ")I", search);
      }
    }
  }
  add("copyOf", "([Ljava/lang/Object;ILjava/lang/Class;)[Ljava/lang/Object;", CopyArray(false));
  add("copyOfRange", "([Ljava/lang/Object;IILjava/lang/Class;)[Ljava/lang/Object;", CopyArray(true));
}

void InstallUnmodifiableList(ClassLinker& cl) {
  auto& cursor = cl.Define(kReadOnlyCursor, kObject, art::kAccFinal, {kListIterator});
  for (auto [name, sig] : {Declaration{"hasNext", "()Z"}, {"next", "()Ljava/lang/Object;"},
        {"hasPrevious", "()Z"}, {"previous", "()Ljava/lang/Object;"}, {"nextIndex", "()I"}, {"previousIndex", "()I"}}) {
    cl.AddMethod(cursor, name, sig, 0, [name, sig](Runtime& vm, Object* self, Values a) {
      return CallVirtual(vm, self->payload.ref(), name, sig, a);
    });
  }
  cl.AddMethod(cursor, "remove", "()V", 0, Unsupported);
  cl.AddMethod(cursor, "set", "(Ljava/lang/Object;)V", 0, Unsupported);
  cl.AddMethod(cursor, "add", "(Ljava/lang/Object;)V", 0, Unsupported);

  auto& list = cl.Define(kReadOnlyList, kObject, art::kAccFinal, {kList});
  for (auto [name, sig] : {Declaration{"size", "()I"}, {"isEmpty", "()Z"}, {"get", "(I)Ljava/lang/Object;"},
        {"contains", "(Ljava/lang/Object;)Z"}, {"indexOf", "(Ljava/lang/Object;)I"}, {"lastIndexOf", "(Ljava/lang/Object;)I"},
        {"toArray", "()[Ljava/lang/Object;"}, {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"},
        {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}, {"containsAll", "(Ljava/util/Collection;)Z"}}) {
    cl.AddMethod(list, name, sig, 0, [name, sig](Runtime& vm, Object* self, Values a) {
      return CallVirtual(vm, self->payload.ref(), name, sig, a);
    });
  }
  for (auto [name, sig] : {Declaration{"add", "(Ljava/lang/Object;)Z"}, {"add", "(ILjava/lang/Object;)V"},
        {"clear", "()V"}, {"set", "(ILjava/lang/Object;)Ljava/lang/Object;"}, {"remove", "(I)Ljava/lang/Object;"},
        {"remove", "(Ljava/lang/Object;)Z"}, {"addAll", "(Ljava/util/Collection;)Z"}, {"addAll", "(ILjava/util/Collection;)Z"},
        {"removeAll", "(Ljava/util/Collection;)Z"}, {"retainAll", "(Ljava/util/Collection;)Z"},
        {"sort", "(Ljava/util/Comparator;)V"}}) {
    cl.AddMethod(list, name, sig, 0, Unsupported);
  }
  for (auto [name, sig] : {Declaration{"iterator", "()Ljava/util/Iterator;"}, {"listIterator", "()Ljava/util/ListIterator;"},
        {"listIterator", "(I)Ljava/util/ListIterator;"}}) {
    cl.AddMethod(list, name, sig, 0, [name, sig](Runtime& vm, Object* self, Values a) {
      RootScope roots(vm);
      const Value iter = roots.Hold(CallVirtual(vm, self->payload.ref(), name, sig, a));
      auto* proxy = vm.heap.Allocate(vm, vm.linker.Resolve(kReadOnlyCursor));
      proxy->payload = iter;
      return Value::Ref(proxy);
    });
  }
}

Value WrapReadOnly(Runtime& vm, std::string_view descriptor, Value value) {
  RootScope roots(vm, {}, &vm.NonNull(value.ref()));
  auto* wrapper = vm.heap.Allocate(vm, vm.linker.Resolve(descriptor));
  wrapper->payload = value;
  return Value::Ref(wrapper);
}

void ForwardReadOnly(ClassLinker& cl, Class& wrapper, std::initializer_list<Declaration> methods) {
  for (auto [name, sig] : methods) {
    cl.AddMethod(wrapper, name, sig, 0, [name, sig, equals = std::string_view(name) == "equals"](
        Runtime& vm, Object* self, Values a) {
      if (equals && self == a[0].ref()) return Value::Int(1);
      return CallVirtual(vm, self->payload.ref(), name, sig, a);
    });
  }
}

void InstallUnmodifiableMap(ClassLinker& cl, Class& collections) {
  auto& entry = cl.Define(kReadOnlyEntry, kObject, art::kAccFinal, {kEntry});
  ForwardReadOnly(cl, entry, {{"getKey", "()Ljava/lang/Object;"}, {"getValue", "()Ljava/lang/Object;"},
                             {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}});
  cl.AddMethod(entry, "setValue", "(Ljava/lang/Object;)Ljava/lang/Object;", 0, Unsupported);
  auto& plain_cursor = cl.Define(kReadOnlyCollectionCursor, kObject, 0, {kIterator});
  ForwardReadOnly(cl, plain_cursor, {{"hasNext", "()Z"}, {"next", "()Ljava/lang/Object;"}});
  auto& cursor = cl.Define(kReadOnlyEntryCursor, kReadOnlyCollectionCursor, art::kAccFinal);
  cl.AddMethod(cursor, "next", "()Ljava/lang/Object;", 0, [](Runtime& vm, Object* self, Values a) {
    return WrapReadOnly(vm, kReadOnlyEntry, CallVirtual(vm, self->payload.ref(), "next", "()Ljava/lang/Object;", a));
  });

  auto& map = cl.Define(kReadOnlyMap, kObject, art::kAccFinal, {kMap, "Ljava/io/Serializable;"});
  ForwardReadOnly(cl, map, {{"size", "()I"}, {"isEmpty", "()Z"}, {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"},
                            {"containsKey", "(Ljava/lang/Object;)Z"}, {"containsValue", "(Ljava/lang/Object;)Z"},
                            {"getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
                            {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}});
  for (auto [name, sig] : {Declaration{"clear", "()V"}, {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
        {"putIfAbsent", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"}, {"putAll", "(Ljava/util/Map;)V"},
        {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"remove", "(Ljava/lang/Object;Ljava/lang/Object;)Z"},
        {"replace", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
        {"replace", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z"},
        {"replaceAll", "(Ljava/util/function/BiFunction;)V"},
        {"compute", "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"},
        {"computeIfAbsent", "(Ljava/lang/Object;Ljava/util/function/Function;)Ljava/lang/Object;"},
        {"computeIfPresent", "(Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"},
        {"merge", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/function/BiFunction;)Ljava/lang/Object;"}}) {
    cl.AddMethod(map, name, sig, 0, Unsupported);
  }

  for (auto [kind, name, descriptor] :
       {std::tuple{Keys, "keySet", "Lwa/runtime/UnmodifiableSet;"},
        {MappedValues, "values", "Lwa/runtime/UnmodifiableCollection;"},
        {Entries, "entrySet", "Lwa/runtime/UnmodifiableEntrySet;"}}) {
    const auto iface = kind == MappedValues ? kCollection : kSet;
    auto& view = cl.Define(descriptor, kObject, art::kAccFinal, {iface});
    ForwardReadOnly(cl, view, {{"size", "()I"}, {"isEmpty", "()Z"}, {"contains", "(Ljava/lang/Object;)Z"},
                               {"containsAll", "(Ljava/util/Collection;)Z"}, {"toString", "()Ljava/lang/String;"}});
    if (kind != MappedValues) {
      ForwardReadOnly(cl, view, {{"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}});
    }
    for (auto [mutator, sig] : {Declaration{"add", "(Ljava/lang/Object;)Z"}, {"remove", "(Ljava/lang/Object;)Z"},
          {"addAll", "(Ljava/util/Collection;)Z"}, {"removeAll", "(Ljava/util/Collection;)Z"},
          {"retainAll", "(Ljava/util/Collection;)Z"}, {"removeIf", "(Ljava/util/function/Predicate;)Z"}, {"clear", "()V"}}) {
      cl.AddMethod(view, mutator, sig, 0, Unsupported);
    }
    cl.AddMethod(view, "iterator", "()Ljava/util/Iterator;", 0, [kind](Runtime& vm, Object* self, Values) {
      return WrapReadOnly(vm, kind == Entries ? kReadOnlyEntryCursor : kReadOnlyCollectionCursor,
                          CallVirtual(vm, self->payload.ref(), "iterator", "()Ljava/util/Iterator;"));
    });
    const Builtin to_array = [kind](Runtime& vm, Object* self, Values a) {
      if (kind != Entries) {
        return CallVirtual(vm, self->payload.ref(), "toArray",
                           a.empty() ? "()[Ljava/lang/Object;" : "([Ljava/lang/Object;)[Ljava/lang/Object;", a);
      }
      RootScope roots(vm);
      auto* array = roots.Hold(CallVirtual(vm, self->payload.ref(), "toArray", "()[Ljava/lang/Object;")).ref();
      for (Value& value : array->elements) value = WrapReadOnly(vm, kReadOnlyEntry, value);
      return a.empty() ? Value::Ref(array) : MakeArray(vm, array->elements, &vm.NonNull(a[0].ref()));
    };
    cl.AddMethod(view, "toArray", "()[Ljava/lang/Object;", 0, to_array);
    cl.AddMethod(view, "toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", 0, to_array);
    cl.AddMethod(map, name, "()" + std::string(iface), 0,
        [name, descriptor, signature = "()" + std::string(iface)](Runtime& vm, Object* self, Values) {
          return WrapReadOnly(vm, descriptor, CallVirtual(vm, self->payload.ref(), name, signature));
        });
  }
  cl.AddMethod(collections, "unmodifiableMap", "(Ljava/util/Map;)Ljava/util/Map;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return WrapReadOnly(vm, kReadOnlyMap, a[0]); });
}

void InstallConcurrentQueueAndExecutor(ClassLinker& cl) {
  constexpr auto kQueue = "Ljava/util/Queue;";
  constexpr auto kBlockingQueue = "Ljava/util/concurrent/BlockingQueue;";
  constexpr auto kLinkedQueue = "Ljava/util/concurrent/LinkedBlockingQueue;";
  constexpr auto kNode = "Ljava/util/concurrent/LinkedBlockingQueue$Node;";
  auto& queue = cl.Define(kQueue, kObject, kInterface, {kCollection});
  Abstract(cl, queue, {{"offer", "(Ljava/lang/Object;)Z"}, {"poll", "()Ljava/lang/Object;"},
                       {"peek", "()Ljava/lang/Object;"}});
  auto& blocking = cl.Define(kBlockingQueue, kObject, kInterface, {kQueue});
  Abstract(cl, blocking, {{"remainingCapacity", "()I"}});
  auto& node = cl.Define(kNode, kObject, art::kAccFinal);
  auto& item = cl.AddField(node, "item", kObject, false);
  auto& next = cl.AddField(node, "next", kNode, false);
  auto& linked = cl.Define(kLinkedQueue, kObject, 0, {kBlockingQueue, "Ljava/io/Serializable;"});
  auto& capacity = cl.AddField(linked, "capacity", "I", false);
  auto& count = cl.AddField(linked, "count", "I", false);
  auto& first = cl.AddField(linked, "head", kNode, false);
  auto& last = cl.AddField(linked, "last", kNode, false);
  const auto construct = [&capacity](Runtime& vm, Object* self, Values a) {
    const int32_t limit = a.empty() ? INT32_MAX : a[0].i();
    if (limit <= 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Queue capacity");
    self->fields[capacity.slot] = Value::Int(limit);
    return Value{};
  };
  cl.AddMethod(linked, "<init>", "()V", 0, construct);
  cl.AddMethod(linked, "<init>", "(I)V", 0, construct);
  const auto offer = [&node, &item, &next, &capacity, &count, &first, &last](Runtime& vm, Object* self, Values a) {
    vm.NonNull(a[0].ref());
    const auto size = self->fields[count.slot].i();
    if (size == self->fields[capacity.slot].i()) return Value::Int(0);
    auto* entry = vm.heap.Allocate(vm, vm.linker.Resolve(node.descriptor));
    entry->fields[item.slot] = a[0];
    if (auto* tail = self->fields[last.slot].ref()) tail->fields[next.slot] = Value::Ref(entry);
    else self->fields[first.slot] = Value::Ref(entry);
    self->fields[last.slot] = Value::Ref(entry);
    self->fields[count.slot] = Value::Int(size + 1);
    ++self->modifications;
    return Value::Int(1);
  };
  cl.AddMethod(linked, "offer", "(Ljava/lang/Object;)Z", 0, offer);
  cl.AddMethod(linked, "add", "(Ljava/lang/Object;)Z", 0, [offer](Runtime& vm, Object* self, Values a) {
    if (!offer(vm, self, a).i()) vm.Fail("Ljava/lang/IllegalStateException;", "Queue full");
    return Value::Int(1);
  });
  cl.AddMethod(linked, "peek", "()Ljava/lang/Object;", 0, [&first, &item](Runtime&, Object* self, Values) {
    const auto* head = self->fields[first.slot].ref();
    return head ? head->fields[item.slot] : Value{};
  });
  cl.AddMethod(linked, "poll", "()Ljava/lang/Object;", 0,
      [&first, &last, &next, &item, &count](Runtime&, Object* self, Values) {
        auto* head = self->fields[first.slot].ref();
        if (!head) return Value{};
        self->fields[first.slot] = head->fields[next.slot];
        if (!self->fields[first.slot].ref()) self->fields[last.slot] = {};
        self->fields[count.slot] = Value::Int(self->fields[count.slot].i() - 1);
        ++self->modifications;
        return head->fields[item.slot];
      });
  cl.AddMethod(linked, "remove", "(Ljava/lang/Object;)Z", 0,
      [&first, &last, &next, &item, &count](Runtime& vm, Object* self, Values args) {
        auto* wanted = args[0].ref();
        if (!wanted) return Value::Int(0);
        const auto version = self->modifications;
        Object* previous = nullptr;
        for (auto* entry = self->fields[first.slot].ref(); entry; entry = entry->fields[next.slot].ref()) {
          const bool match = JavaEquals(vm, wanted, entry->fields[item.slot].ref());
          if (version != self->modifications) vm.Fail("Ljava/util/ConcurrentModificationException;");
          if (match) {
            auto& link = previous ? previous->fields[next.slot] : self->fields[first.slot];
            link = entry->fields[next.slot];
            if (self->fields[last.slot].ref() == entry) self->fields[last.slot] = Value::Ref(previous);
            self->fields[count.slot] = Value::Int(self->fields[count.slot].i() - 1);
            ++self->modifications;
            return Value::Int(1);
          }
          previous = entry;
        }
        return Value::Int(0);
      });
  cl.AddMethod(linked, "clear", "()V", 0, [&first, &last, &count](Runtime&, Object* self, Values) {
    if (self->fields[count.slot].i()) ++self->modifications;
    self->fields[first.slot] = self->fields[last.slot] = self->fields[count.slot] = {};
    return Value{};
  });
  cl.AddMethod(linked, "size", "()I", 0, [&count](Runtime&, Object* self, Values) { return self->fields[count.slot]; });
  cl.AddMethod(linked, "isEmpty", "()Z", 0,
      [&count](Runtime&, Object* self, Values) { return Value::Int(!self->fields[count.slot].i()); });
  cl.AddMethod(linked, "remainingCapacity", "()I", 0, [&capacity, &count](Runtime&, Object* self, Values) {
    return Value::Int(self->fields[capacity.slot].i() - self->fields[count.slot].i());
  });

  constexpr auto kAbstract = "Ljava/util/concurrent/AbstractExecutorService;";
  auto& executor = cl.Define(kAbstract, kObject, art::kAccAbstract, {"Ljava/util/concurrent/ExecutorService;"});
  cl.AddMethod(executor, "<init>", "()V", 0, NoOp);
  auto& factory = cl.Define("Ljava/util/concurrent/ThreadFactory;", kObject, kInterface);
  Abstract(cl, factory, {{"newThread", "(Ljava/lang/Runnable;)Ljava/lang/Thread;"}});
  auto& pool = cl.Define("Ljava/util/concurrent/ThreadPoolExecutor;", kAbstract);
  auto& work_queue = cl.AddField(pool, "workQueue", kBlockingQueue, false);
  auto& core_size = cl.AddField(pool, "corePoolSize", "I", false);
  auto& max_size = cl.AddField(pool, "maximumPoolSize", "I", false);
  auto& keep_alive = cl.AddField(pool, "keepAliveTime", "J", false);
  auto& core_timeout = cl.AddField(pool, "allowCoreThreadTimeOut", "Z", false);
  auto& thread_factory = cl.AddField(pool, "threadFactory", factory.descriptor, false);
  auto& handler = cl.AddField(pool, "handler", "Ljava/util/concurrent/RejectedExecutionHandler;", false);
  auto& running = cl.AddField(pool, "running", "Z", false);
  auto& shutdown = cl.AddField(pool, "shutdown", "Z", false);
  // Executors create workers on demand. Initialization only needs the queue and configuration.
  const Builtin init_pool =
      [&work_queue, &core_size, &max_size, &keep_alive, &thread_factory, &handler](
          Runtime& vm, Object* self, Values a) {
        if (a[0].i() < 0 || a[1].i() <= 0 || a[1].i() < a[0].i() || a[2].j() < 0) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid executor size or keep-alive time");
        }
        vm.NonNull(a[4].ref());
        self->fields[keep_alive.slot] = CallVirtual(vm, a[3].ref(), "toNanos", "(J)J", a.subspan(2, 1));
        self->fields[core_size.slot] = a[0];
        self->fields[max_size.slot] = a[1];
        self->fields[work_queue.slot] = a[4];
        if (a.size() >= 6) self->fields[thread_factory.slot] = Value::Ref(&vm.NonNull(a[5].ref()));
        if (a.size() == 7) self->fields[handler.slot] = Value::Ref(&vm.NonNull(a[6].ref()));
        return Value{};
      };
  cl.AddMethod(pool, "<init>", "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;)V",
               0, init_pool);
  cl.AddMethod(pool, "<init>", "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;"
               "Ljava/util/concurrent/ThreadFactory;)V", 0, init_pool);
  cl.AddMethod(pool, "<init>", "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;"
               "Ljava/util/concurrent/ThreadFactory;Ljava/util/concurrent/RejectedExecutionHandler;)V", 0, init_pool);
  cl.AddMethod(pool, "execute", "(Ljava/lang/Runnable;)V", 0,
      [&work_queue, &running, &handler, &shutdown](Runtime& vm, Object* self, Values a) {
        vm.NonNull(a[0].ref());
        auto* queue = self->fields[work_queue.slot].ref();
        const bool stopped = self->fields[shutdown.slot].i();
        if (stopped || !CallVirtual(vm, queue, "offer", "(Ljava/lang/Object;)Z", a).i()) {
          if (auto* reject = self->fields[handler.slot].ref()) {
            return CallVirtual(vm, reject, "rejectedExecution",
                               "(Ljava/lang/Runnable;Ljava/util/concurrent/ThreadPoolExecutor;)V",
                               std::array{a[0], Value::Ref(self)});
          }
          vm.Fail("Ljava/util/concurrent/RejectedExecutionException;",
                  stopped ? "Executor is shut down" : "Executor queue is full");
        }
        if (self->fields[running.slot].i()) return Value{};
        self->fields[running.slot] = Value::Int(1);
        try {
          while (auto* task = CallVirtual(vm, queue, "poll", "()Ljava/lang/Object;", {}).ref()) {
            CallVirtual(vm, task, "run", "()V", {});
          }
        } catch (...) {
          self->fields[running.slot] = {};
          throw;
        }
        self->fields[running.slot] = {};
        return Value{};
      });
  cl.AddMethod(pool, "shutdown", "()V", 0, [&shutdown](Runtime&, Object* self, Values) {
    self->fields[shutdown.slot] = Value::Int(1);
    return Value{};
  });
  cl.AddMethod(pool, "isShutdown", "()Z", 0,
      [&shutdown](Runtime&, Object* self, Values) { return self->fields[shutdown.slot]; });
  const Builtin terminated = [&shutdown, &running](Runtime&, Object* self, Values) {
    return Value::Int(self->fields[shutdown.slot].i() && !self->fields[running.slot].i());
  };
  cl.AddMethod(pool, "isTerminated", "()Z", 0, terminated);
  cl.AddMethod(pool, "awaitTermination", "(JLjava/util/concurrent/TimeUnit;)Z", 0, terminated);
  cl.AddMethod(pool, "getQueue", "()Ljava/util/concurrent/BlockingQueue;", 0,
      [&work_queue](Runtime&, Object* self, Values) { return self->fields[work_queue.slot]; });
  cl.AddMethod(pool, "getCorePoolSize", "()I", 0,
      [&core_size](Runtime&, Object* self, Values) { return self->fields[core_size.slot]; });
  cl.AddMethod(pool, "getMaximumPoolSize", "()I", 0,
      [&max_size](Runtime&, Object* self, Values) { return self->fields[max_size.slot]; });
  cl.AddMethod(pool, "allowsCoreThreadTimeOut", "()Z", 0,
      [&core_timeout](Runtime&, Object* self, Values) { return self->fields[core_timeout.slot]; });
  cl.AddMethod(pool, "allowCoreThreadTimeOut", "(Z)V", 0,
      [&core_timeout, &keep_alive](Runtime& vm, Object* self, Values args) {
        if (args[0].i() && self->fields[keep_alive.slot].j() <= 0) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Core threads must have nonzero keep alive times");
        }
        self->fields[core_timeout.slot] = args[0];
        return Value{};
      });
  cl.AddMethod(pool, "getKeepAliveTime", "(Ljava/util/concurrent/TimeUnit;)J", 0,
      [&keep_alive](Runtime& vm, Object* self, Values a) {
        auto& unit = vm.linker.Resolve("Ljava/util/concurrent/TimeUnit;");
        vm.linker.Initialize(unit);
        const auto nanos = vm.linker.FindField(unit, "NANOSECONDS", unit.descriptor, true).value;
        return CallVirtual(vm, a[0].ref(), "convert", "(JLjava/util/concurrent/TimeUnit;)J",
                           std::array{self->fields[keep_alive.slot], nanos});
      });

}

void InstallUtilities(ClassLinker& cl, const Interfaces& interfaces) {
  auto& objects = cl.Define("Ljava/util/Objects;", kObject, art::kAccFinal);
  cl.AddMethod(objects, "checkFromIndexSize", "(III)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        const int from = a[0].i(), size = a[1].i(), length = a[2].i();
        if (from < 0 || size < 0 || length < 0 || from > length - size) {
          vm.Fail("Ljava/lang/IndexOutOfBoundsException;", "Range exceeds bounds");
        }
        return a[0];
      });
  cl.AddMethod(objects, "equals", "(Ljava/lang/Object;Ljava/lang/Object;)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Int(JavaEquals(vm, a[0].ref(), a[1].ref())); });
  cl.AddMethod(objects, "toString", "(Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        return a[0].ref() ? CallVirtual(vm, a[0].ref(), "toString", "()Ljava/lang/String;")
                          : StringValue(vm, "null");
      });
  const Builtin non_null = [](Runtime& vm, Object*, Values a) {
    if (!a[0].ref()) vm.Fail("Ljava/lang/NullPointerException;",
        a.size() > 1 && a[1].ref() ? EncodeUtf8(a[1].ref()->text) : std::string{});
    return a[0];
  };
  cl.AddMethod(objects, "requireNonNull", "(Ljava/lang/Object;)Ljava/lang/Object;", art::kAccStatic, non_null);
  cl.AddMethod(objects, "requireNonNull", "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;", art::kAccStatic, non_null);
  cl.AddMethod(objects, "hashCode", "(Ljava/lang/Object;)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Int(JavaHash(vm, a[0].ref())); });
  cl.AddMethod(objects, "hash", "([Ljava/lang/Object;)I", art::kAccStatic, [](Runtime& vm, Object*, Values a) {
    if (!a[0].ref()) return Value::Int(0);
    uint32_t result = 1;
    for (Value item : a[0].ref()->elements) result = 31 * result + JavaHash(vm, item.ref());
    return Value::Int(static_cast<int32_t>(result));
  });

  InstallArrays(cl);
  InstallUnmodifiableList(cl);
  auto& collections = cl.Define("Ljava/util/Collections;", kObject, art::kAccFinal);
  InstallUnmodifiableMap(cl, collections);
  for (const auto& [name, type] : {std::pair{"synchronizedCollection", kCollection},
                                  {"synchronizedList", kList}, {"synchronizedSet", kSet}}) {
    const auto signature = std::string("(").append(type).append(")").append(type);
    cl.AddMethod(collections, name, signature, art::kAccStatic,
        [name, signature](Runtime& vm, Object*, Values args) {
          auto& views = vm.linker.Resolve("Ljava/util/EmbeddedSynchronizedCollections;");
          return vm.Invoke(vm.linker.FindMethod(views, name, signature, true), nullptr, args);
        });
  }
  cl.AddMethod(collections, "unmodifiableSet", "(Ljava/util/Set;)Ljava/util/Set;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return WrapReadOnly(vm, "Lwa/runtime/UnmodifiableSet;", a[0]); });
  auto& map_set = cl.Define("Ljava/util/Collections$SetFromMap;", kObject, art::kAccFinal,
      {kSet, "Ljava/io/Serializable;"});
  auto& set_map = cl.AddField(map_set, "m", kMap, false);
  ForwardReadOnly(cl, map_set, {
      {"size", "()I"}, {"isEmpty", "()Z"}, {"clear", "()V"}, {"iterator", "()Ljava/util/Iterator;"},
      {"contains", "(Ljava/lang/Object;)Z"}, {"remove", "(Ljava/lang/Object;)Z"},
      {"toArray", "()[Ljava/lang/Object;"}, {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"},
      {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}});
  cl.AddMethod(map_set, "add", "(Ljava/lang/Object;)Z", 0, [&set_map](Runtime& vm, Object* self, Values a) {
    auto& boolean = vm.linker.Resolve("Ljava/lang/Boolean;");
    auto& value_of = vm.linker.FindMethod(boolean, "valueOf", "(Z)Ljava/lang/Boolean;", true);
    const auto yes = vm.Invoke(value_of, nullptr, std::array{Value::Int(1)});
    const auto previous = CallVirtual(vm, self->fields[set_map.slot].ref(), "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", std::array{a[0], yes});
    return Value::Int(previous.ref() == nullptr);
  });
  cl.AddMethod(collections, "newSetFromMap", "(Ljava/util/Map;)Ljava/util/Set;", art::kAccStatic,
      [&map_set, &set_map](Runtime& vm, Object*, Values a) {
        if (!CallVirtual(vm, a[0].ref(), "isEmpty", "()Z").i()) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Map is non-empty");
        }
        RootScope roots(vm);
        auto* set = roots.Hold(vm.heap.Allocate(vm, vm.linker.Resolve(map_set.descriptor)));
        set->fields[set_map.slot] = a[0];
        set->payload = CallVirtual(vm, a[0].ref(), "keySet", "()Ljava/util/Set;");
        return Value::Ref(set);
      });
  for (auto [name, iface, implementation] : {std::tuple{"List", kList, kArrayList},
        {"Map", kMap, kHashMap}, {"Set", kSet, kHashSet}}) {
    std::string upper = name;
    for (char& ch : upper) ch = std::toupper(static_cast<unsigned char>(ch));
    auto& empty = cl.AddField(collections, "EMPTY_" + upper, iface, true);
    cl.AddMethod(collections, std::string("empty") + name, "()" + std::string(iface), art::kAccStatic,
        [&empty](Runtime&, Object*, Values) { return empty.value; });
  }
  cl.AddMethod(collections, "<clinit>", "()V", art::kAccStatic, [&collections](Runtime& vm, Object*, Values) {
    const std::array implementations{kArrayList, kHashMap, kHashSet};
    for (size_t i = 0; i < implementations.size(); ++i) {
      auto* instance = vm.heap.Allocate(vm, vm.linker.Resolve(implementations[i]));
      instance->read_only = true;
      collections.field_order[i]->value = Value::Ref(instance);
    }
    return Value{};
  });
  cl.AddMethod(collections, "singletonList", "(Ljava/lang/Object;)Ljava/util/List;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return MakeList(vm, a, true); });
  cl.AddMethod(collections, "singleton", "(Ljava/lang/Object;)Ljava/util/Set;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        RootScope roots(vm);
        auto* set = roots.Hold(vm.heap.Allocate(vm, vm.linker.Resolve(kHashSet)));
        InsertEntry(vm, *set, JavaHash(vm, a[0].ref()), a[0], {});
        set->read_only = true;
        return Value::Ref(set);
      });
  cl.AddMethod(collections, "synchronizedMap", "(Ljava/util/Map;)Ljava/util/Map;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        // The interpreter already serializes map access across cooperative guest threads.
        return Value::Ref(&vm.NonNull(a[0].ref()));
      });
  cl.AddMethod(collections, "singletonMap", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/Map;",
      art::kAccStatic, [](Runtime& vm, Object*, Values a) {
        RootScope roots(vm);
        auto* map = roots.Hold(vm.heap.Allocate(vm, vm.linker.Resolve(kHashMap)));
        InsertEntry(vm, *map, JavaHash(vm, a[0].ref()), a[0], a[1]);
        return WrapReadOnly(vm, kReadOnlyMap, Value::Ref(map));
      });
  const Builtin map_of = [](Runtime& vm, Object*, Values a) {
    RootScope roots(vm);
    auto* map = roots.Hold(vm.heap.Allocate(vm, vm.linker.Resolve(kHashMap)));
    for (size_t i = 0; i < a.size(); i += 2) {
      vm.NonNull(a[i].ref());
      vm.NonNull(a[i + 1].ref());
      const auto hash = JavaHash(vm, a[i].ref());
      if (Lookup(vm, *map, a[i].ref()).entry) {
        vm.Fail("Ljava/lang/IllegalArgumentException;", "Duplicate key");
      }
      InsertEntry(vm, *map, hash, a[i], a[i + 1]);
    }
    return WrapReadOnly(vm, kReadOnlyMap, Value::Ref(map));
  };
  std::string parameters;
  for (int pairs = 0; pairs <= 10; ++pairs) {
    cl.AddMethod(cl.Resolve(kMap), "of", "(" + parameters + ")Ljava/util/Map;", art::kAccStatic, map_of);
    parameters += "Ljava/lang/Object;Ljava/lang/Object;";
  }
  cl.AddMethod(collections, "unmodifiableList", "(Ljava/util/List;)Ljava/util/List;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        vm.NonNull(a[0].ref());
        auto* wrapper = vm.heap.Allocate(vm, vm.linker.Resolve(kReadOnlyList));
        wrapper->payload = a[0];
        return Value::Ref(wrapper);
      });
  const Builtin sort = [](Runtime& vm, Object*, Values a) {
    return CallVirtual(vm, a[0].ref(), "sort", "(Ljava/util/Comparator;)V",
                       std::array{a.size() == 2 ? a[1] : Value{}});
  };
  cl.AddMethod(collections, "sort", "(Ljava/util/List;)V", art::kAccStatic, sort);
  cl.AddMethod(collections, "sort", "(Ljava/util/List;Ljava/util/Comparator;)V", art::kAccStatic, sort);
  cl.AddMethod(collections, "reverse", "(Ljava/util/List;)V", art::kAccStatic, [](Runtime& vm, Object*, Values a) {
    auto* list = a[0].ref();
    const int32_t size = CallVirtual(vm, list, "size", "()I").i();
    for (int32_t left = 0; left < size / 2; ++left) {
      RootScope roots(vm);
      const int32_t right = size - left - 1;
      const auto first = roots.Hold(ListGet(vm, list, left));
      const auto last = ListGet(vm, list, right);
      CallVirtual(vm, list, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", std::array{Value::Int(left), last});
      CallVirtual(vm, list, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", std::array{Value::Int(right), first});
    }
    return Value{};
  });
  cl.AddMethod(collections, "addAll", "(Ljava/util/Collection;[Ljava/lang/Object;)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        bool changed = false;
        const auto& array = vm.NonNull(a[1].ref()).elements;
        for (size_t i = 0; i < array.size(); ++i) {
          changed |= CallVirtual(vm, a[0].ref(), "add", "(Ljava/lang/Object;)Z", std::array{array[i]}).i() != 0;
        }
        return Value::Int(changed);
      });

  for (bool reverse : {false, true}) {
    auto& type = cl.Define(reverse ? "Lwa/runtime/ReverseComparator;" : "Lwa/runtime/NaturalComparator;",
                           kObject, art::kAccFinal, {kComparator});
    cl.AddMethod(type, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", 0,
        [reverse](Runtime& vm, Object*, Values a) {
          return CallVirtual(vm, a[reverse].ref(), "compareTo", "(Ljava/lang/Object;)I", std::array{a[!reverse]});
        });
    const auto factory = [&type](Runtime& vm, Object*, Values) {
      return Value::Ref(vm.heap.Allocate(vm, vm.linker.Resolve(type.descriptor)));
    };
    cl.AddMethod(interfaces.comparator, reverse ? "reverseOrder" : "naturalOrder", "()Ljava/util/Comparator;",
        art::kAccStatic, factory);
    if (reverse) cl.AddMethod(collections, "reverseOrder", "()Ljava/util/Comparator;", art::kAccStatic, factory);
  }
  const char compare_signature[] = "(Ljava/lang/Object;Ljava/lang/Object;)I";
  auto& keys = cl.Define("Lwa/runtime/KeyComparator;", kObject, art::kAccFinal, {kComparator});
  cl.AddMethod(keys, "compare", compare_signature, 0, [](Runtime& vm, Object* self, Values a) {
    auto* extractor = self->elements[0].ref();
    RootScope roots(vm);
    const auto left = roots.Hold(CallVirtual(vm, extractor, "apply",
        "(Ljava/lang/Object;)Ljava/lang/Object;", a.first(1)));
    const auto right = CallVirtual(vm, extractor, "apply",
        "(Ljava/lang/Object;)Ljava/lang/Object;", a.last(1));
    auto* order = self->elements[1].ref();
    if (order) return CallVirtual(vm, order, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I",
                                  std::array{left, right});
    return CallVirtual(vm, left.ref(), "compareTo", "(Ljava/lang/Object;)I", std::array{right});
  });
  const Builtin comparing = [&keys](Runtime& vm, Object*, Values a) {
    vm.NonNull(a[0].ref());
    if (a.size() == 2) vm.NonNull(a[1].ref());
    auto* object = vm.heap.Allocate(vm, vm.linker.Resolve(keys.descriptor));
    object->elements = {a[0], a.size() == 2 ? a[1] : Value{}};
    return Value::Ref(object);
  };
  cl.AddMethod(interfaces.comparator, "comparing", "(Ljava/util/function/Function;)Ljava/util/Comparator;",
      art::kAccStatic, comparing);
  cl.AddMethod(interfaces.comparator, "comparing",
      "(Ljava/util/function/Function;Ljava/util/Comparator;)Ljava/util/Comparator;", art::kAccStatic, comparing);
  auto& int_keys = cl.Define("Lwa/runtime/IntKeyComparator;", kObject, art::kAccFinal, {kComparator});
  cl.AddMethod(int_keys, "compare", compare_signature, 0, [](Runtime& vm, Object* self, Values a) {
    auto* extractor = self->elements[0].ref();
    constexpr auto apply_signature = "(Ljava/lang/Object;)I";
    const int left = CallVirtual(vm, extractor, "applyAsInt", apply_signature, a.first(1)).i();
    const int right = CallVirtual(vm, extractor, "applyAsInt", apply_signature, a.last(1)).i();
    return Value::Int((left > right) - (left < right));
  });
  cl.AddMethod(interfaces.comparator, "comparingInt", "(Ljava/util/function/ToIntFunction;)Ljava/util/Comparator;",
      art::kAccStatic, [&int_keys](Runtime& vm, Object*, Values a) {
        vm.NonNull(a[0].ref());
        auto* object = vm.heap.Allocate(vm, vm.linker.Resolve(int_keys.descriptor));
        object->elements = {a[0]};
        return Value::Ref(object);
      });
  auto& paired = cl.Define("Lwa/runtime/ChainedComparator;", kObject, art::kAccFinal, {kComparator});
  cl.AddMethod(paired, "compare", compare_signature, 0, [](Runtime& vm, Object* self, Values a) {
    const auto first = CallVirtual(vm, self->elements[0].ref(), "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", a);
    return first.i() ? first : CallVirtual(vm, self->elements[1].ref(), "compare",
                                          "(Ljava/lang/Object;Ljava/lang/Object;)I", a);
  });
  cl.AddMethod(interfaces.comparator, "thenComparing", "(Ljava/util/function/Function;)Ljava/util/Comparator;", 0,
      [comparing, &paired](Runtime& vm, Object* self, Values a) {
        RootScope roots(vm);
        const auto next = roots.Hold(comparing(vm, nullptr, a));
        auto* object = vm.heap.Allocate(vm, vm.linker.Resolve(paired.descriptor));
        object->elements = {Value::Ref(self), next};
        return Value::Ref(object);
      });
}

void InstallCollectionDefaults(ClassLinker& cl, Class& iface) {
  cl.AddMethod(iface, "removeIf", "(Ljava/util/function/Predicate;)Z", 0,
      [](Runtime& vm, Object* self, Values a) {
        auto* predicate = &vm.NonNull(a[0].ref());
        RootScope roots(vm);
        auto* cursor = roots.Hold(CallVirtual(vm, self, "iterator", "()Ljava/util/Iterator;").ref());
        bool changed = false;
        while (CallVirtual(vm, cursor, "hasNext", "()Z").i()) {
          const auto item = CallVirtual(vm, cursor, "next", "()Ljava/lang/Object;");
          if (CallVirtual(vm, predicate, "test", "(Ljava/lang/Object;)Z", std::array{item}).i()) {
            CallVirtual(vm, cursor, "remove", "()V");
            changed = true;
          }
        }
        return Value::Int(changed);
      });
  for (bool contains : {false, true}) {
    cl.AddMethod(iface, contains ? "containsAll" : "addAll", "(Ljava/util/Collection;)Z", 0,
        [contains](Runtime& vm, Object* self, Values a) {
          RootScope roots(vm);
          const auto values = roots.Hold(CallVirtual(vm, a[0].ref(), "toArray", "()[Ljava/lang/Object;"));
          bool changed = false;
          for (Value value : vm.NonNull(values.ref()).elements) {
            const bool result = CallVirtual(vm, self, contains ? "contains" : "add",
                "(Ljava/lang/Object;)Z", std::array{value}).i();
            if (contains && !result) return Value::Int(0);
            changed |= result;
          }
          return Value::Int(contains || changed);
        });
  }
  for (bool retain : {false, true}) {
    cl.AddMethod(iface, retain ? "retainAll" : "removeAll", "(Ljava/util/Collection;)Z", 0,
        [retain](Runtime& vm, Object* self, Values a) {
          auto* other = &vm.NonNull(a[0].ref());
          RootScope roots(vm);
          const auto cursor = roots.Hold(CallVirtual(vm, self, "iterator", "()Ljava/util/Iterator;"));
          bool changed = false;
          while (CallVirtual(vm, cursor.ref(), "hasNext", "()Z").i()) {
            const auto item = CallVirtual(vm, cursor.ref(), "next", "()Ljava/lang/Object;");
            const bool present = CallVirtual(vm, other, "contains", "(Ljava/lang/Object;)Z", std::array{item}).i();
            if (present != retain) {
              CallVirtual(vm, cursor.ref(), "remove", "()V");
              changed = true;
            }
          }
          return Value::Int(changed);
        });
  }
}

}  // namespace

void InstallCollections(Runtime& vm) {
  auto& cl = vm.linker;
  const auto interfaces = DeclareInterfaces(cl);
  InstallCollectionDefaults(cl, interfaces.collection);
  InstallList(cl, interfaces.list);
  InstallMaps(cl);
  InstallUtilities(cl, interfaces);
  InstallConcurrentQueueAndExecutor(cl);
}

}  // namespace wa::interp
