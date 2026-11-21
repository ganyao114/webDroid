#include "boot.h"

#include <algorithm>
#include <limits>

namespace wa::interp {
namespace {

constexpr std::string_view kList = "Ljava/util/List;";
constexpr std::string_view kArrayList = "Ljava/util/ArrayList;";
constexpr std::string_view kObjectArray = "[Ljava/lang/Object;";
constexpr std::string_view kIterator = "Ljava/util/Iterator;";
constexpr auto kCursor = "Lwa/runtime/CollectionIterator;";
constexpr auto kKeySet = "Lwa/runtime/MapKeySet;";

using Declaration = std::pair<const char*, const char*>;

void Abstract(ClassLinker& linker, Class& cls, std::initializer_list<Declaration> methods) {
  for (auto [name, signature] : methods) linker.AddMethod(cls, name, signature, art::kAccAbstract);
}

void DeclareInterfaces(ClassLinker& cl) {
  Abstract(cl, cl.Define(kIterator, kObject, kInterface),
      {{"hasNext", "()Z"}, {"next", "()Ljava/lang/Object;"}});
  Abstract(cl, cl.Define("Ljava/lang/Iterable;", kObject, kInterface),
      {{"iterator", "()Ljava/util/Iterator;"}});
  Abstract(cl, cl.Define("Ljava/util/Collection;", kObject, kInterface, {"Ljava/lang/Iterable;"}), {
      {"size", "()I"}, {"isEmpty", "()Z"}, {"clear", "()V"}, {"add", "(Ljava/lang/Object;)Z"},
      {"remove", "(Ljava/lang/Object;)Z"}, {"contains", "(Ljava/lang/Object;)Z"},
      {"toArray", "()[Ljava/lang/Object;"}, {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;"}});
  Abstract(cl, cl.Define(kList, kObject, kInterface, {"Ljava/util/Collection;"}), {
      {"get", "(I)Ljava/lang/Object;"}, {"set", "(ILjava/lang/Object;)Ljava/lang/Object;"},
      {"add", "(ILjava/lang/Object;)V"}, {"remove", "(I)Ljava/lang/Object;"},
      {"indexOf", "(Ljava/lang/Object;)I"}});
  cl.Define("Ljava/util/Set;", kObject, kInterface, {"Ljava/util/Collection;"});
  Abstract(cl, cl.Define("Ljava/util/Map;", kObject, kInterface), {
      {"size", "()I"}, {"isEmpty", "()Z"}, {"clear", "()V"},
      {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"containsKey", "(Ljava/lang/Object;)Z"},
      {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
      {"putIfAbsent", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"},
      {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"keySet", "()Ljava/util/Set;"}});
  Abstract(cl, cl.Define("Ljava/util/Comparator;", kObject, kInterface),
      {{"compare", "(Ljava/lang/Object;Ljava/lang/Object;)I"}});
  Abstract(cl, cl.Define("Ljava/util/function/Predicate;", kObject, kInterface),
      {{"test", "(Ljava/lang/Object;)Z"}});
  Abstract(cl, cl.Define("Ljava/util/function/Supplier;", kObject, kInterface),
      {{"get", "()Ljava/lang/Object;"}});
  Abstract(cl, cl.Define("Ljava/util/function/Function;", kObject, kInterface),
      {{"apply", "(Ljava/lang/Object;)Ljava/lang/Object;"}});
  cl.Define("Ljava/util/RandomAccess;", kObject, kInterface);
}

std::vector<Value>& ListElements(Object& list) {
  return list.payload.ref() ? list.payload.ref()->elements : list.elements;
}

size_t Index(Runtime& vm, int32_t value, size_t size, bool insertion = false) {
  if (value < 0 || static_cast<size_t>(value) >= size + insertion) {
    vm.Fail("Ljava/lang/IndexOutOfBoundsException;", std::to_string(value));
  }
  return value;
}

size_t Find(Runtime& vm, const std::vector<Value>& values, Object* needle, size_t stride = 1) {
  size_t i = 0;
  for (; i < values.size(); i += stride) {
    if (JavaEquals(vm, needle, values[i].ref())) return i;
  }
  return values.size();
}

Value MakeArray(Runtime& vm, const std::vector<Value>& values, Object* supplied = nullptr) {
  RootScope roots(vm);
  // Values are owned by the rooted receiver or by the scope that assembled the snapshot.
  Class& type = supplied ? *supplied->type : vm.linker.Resolve(kObjectArray);
  Object* array = supplied && supplied->elements.size() >= values.size()
      ? supplied : vm.NewArray(type, values.size());
  roots.Hold(array);
  for (size_t i = 0; i < values.size(); ++i) vm.StoreArrayElement(*array, i, values[i]);
  if (array->elements.size() > values.size()) array->elements[values.size()] = {};
  return Value::Ref(array);
}

Value MakeList(Runtime& vm, Values values, bool immutable = false) {
  Object* list = vm.heap.Allocate(vm, vm.linker.Resolve(kArrayList));
  list->elements.assign(values.begin(), values.end());
  list->payload = Value::Int(immutable);
  return Value::Ref(list);
}

void CheckMutable(Runtime& vm, const Object& list, bool changes_size = true) {
  if (list.payload.i() || (changes_size && list.payload.ref())) vm.Fail("Ljava/lang/UnsupportedOperationException;");
}

Value MakeCursor(Runtime& vm, std::vector<Value> values) {
  Object* iterator = vm.heap.Allocate(vm, vm.linker.Resolve(kCursor));
  iterator->elements = std::move(values);
  return Value::Ref(iterator);
}

void InstallList(Runtime& vm) {
  auto& cl = vm.linker;
  auto& iterator = cl.Define(kCursor, kObject, art::kAccFinal, {kIterator});
  cl.AddMethod(iterator, "hasNext", "()Z", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->payload.bits() < self->elements.size());
  });
  cl.AddMethod(iterator, "next", "()Ljava/lang/Object;", 0, [](Runtime& vm, Object* self, Values) {
    auto index = self->payload.bits();
    if (index >= self->elements.size()) vm.Fail("Ljava/util/NoSuchElementException;");
    self->payload = Value::Bits(index + 1);
    return self->elements[index];
  });

  Class& list = cl.Define(kArrayList, kObject, 0,
      {kList, "Ljava/lang/Cloneable;", "Ljava/io/Serializable;", "Ljava/util/RandomAccess;"});
  const auto add = [&](auto name, auto signature, Builtin body) { cl.AddMethod(list, name, signature, 0, std::move(body)); };
  add("<init>", "()V", [](Runtime&, Object*, Values) { return Value{}; });
  add("<init>", "(I)V", [](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Negative capacity");
    self->elements.reserve(a[0].i());
    return Value{};
  });
  add("<init>", "(Ljava/util/Collection;)V", [](Runtime& vm, Object* self, Values a) {
    Object* array = CallVirtual(vm, a[0].ref(), "toArray", "()[Ljava/lang/Object;").ref();
    self->elements = array->elements;
    return Value{};
  });
  add("add", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    self->elements.push_back(a[0]);
    return Value::Int(1);
  });
  add("add", "(ILjava/lang/Object;)V", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    self->elements.insert(self->elements.begin() + Index(vm, a[0].i(), self->elements.size(), true), a[1]);
    return Value{};
  });
  add("get", "(I)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    const auto& values = ListElements(*self);
    return values[Index(vm, a[0].i(), values.size())];
  });
  add("set", "(ILjava/lang/Object;)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self, false);
    auto& values = ListElements(*self);
    const auto i = Index(vm, a[0].i(), values.size());
    const auto previous = values[i];
    if (self->payload.ref()) vm.StoreArrayElement(*self->payload.ref(), i, a[1]);
    else values[i] = a[1];
    return previous;
  });
  add("remove", "(I)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const auto it = self->elements.begin() + Index(vm, a[0].i(), self->elements.size());
    const Value previous = *it;
    self->elements.erase(it);
    return previous;
  });
  add("remove", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    CheckMutable(vm, *self);
    const auto i = Find(vm, self->elements, a[0].ref());
    if (i == self->elements.size()) return Value::Int(0);
    self->elements.erase(self->elements.begin() + i);
    return Value::Int(1);
  });
  add("indexOf", "(Ljava/lang/Object;)I", [](Runtime& vm, Object* self, Values a) {
    const auto& values = ListElements(*self);
    const auto i = Find(vm, values, a[0].ref());
    return Value::Int(i == values.size() ? -1 : static_cast<int32_t>(i));
  });
  add("contains", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    const auto& values = ListElements(*self);
    return Value::Int(Find(vm, values, a[0].ref()) < values.size());
  });
  add("size", "()I", [](Runtime&, Object* self, Values) { return Value::Int(ListElements(*self).size()); });
  add("isEmpty", "()Z", [](Runtime&, Object* self, Values) { return Value::Int(ListElements(*self).empty()); });
  add("clear", "()V", [](Runtime& vm, Object* self, Values) {
    CheckMutable(vm, *self);
    self->elements.clear();
    return Value{};
  });
  add("iterator", "()Ljava/util/Iterator;", [](Runtime& vm, Object* self, Values) {
    return MakeCursor(vm, ListElements(*self));
  });
  const Builtin to_array = [](Runtime& vm, Object* self, Values a) {
    return MakeArray(vm, ListElements(*self), a.empty() ? nullptr : &vm.NonNull(a[0].ref()));
  };
  add("toArray", "()[Ljava/lang/Object;", to_array);
  add("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", to_array);

  auto& iface = cl.Resolve(kList);
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
}

void InstallMaps(Runtime& vm) {
  auto& cl = vm.linker;
  Class& map = cl.Define("Ljava/util/HashMap;", kObject, 0,
      {"Ljava/util/Map;", "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
  const auto add = [&](auto name, auto signature, Builtin body) { cl.AddMethod(map, name, signature, 0, std::move(body)); };
  const Builtin no_op = [](Runtime&, Object*, Values) { return Value{}; };
  const Builtin capacity = [](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Negative capacity");
    self->elements.reserve(static_cast<size_t>(a[0].i()) * 2);
    return Value{};
  };
  add("<init>", "()V", no_op);
  add("<init>", "(I)V", capacity);
  add("get", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    const auto i = Find(vm, self->elements, a[0].ref(), 2);
    return i < self->elements.size() ? self->elements[i + 1] : Value{};
  });
  add("containsKey", "(Ljava/lang/Object;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(Find(vm, self->elements, a[0].ref(), 2) < self->elements.size());
  });
  const auto put = [](bool only_absent) -> Builtin {
    return [only_absent](Runtime& vm, Object* self, Values a) {
      const auto i = Find(vm, self->elements, a[0].ref(), 2);
      if (i == self->elements.size()) {
        self->elements.insert(self->elements.end(), a.begin(), a.end());
        return Value{};
      }
      Value previous = self->elements[i + 1];
      if (!only_absent || !previous.ref()) self->elements[i + 1] = a[1];
      return previous;
    };
  };
  add("put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", put(false));
  add("putIfAbsent", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", put(true));
  add("remove", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& vm, Object* self, Values a) {
    const auto i = Find(vm, self->elements, a[0].ref(), 2);
    if (i == self->elements.size()) return Value{};
    Value previous = self->elements[i + 1];
    self->elements.erase(self->elements.begin() + i, self->elements.begin() + i + 2);
    return previous;
  });
  add("size", "()I", [](Runtime&, Object* self, Values) { return Value::Int(self->elements.size() / 2); });
  add("isEmpty", "()Z", [](Runtime&, Object* self, Values) { return Value::Int(self->elements.empty()); });
  add("clear", "()V", [](Runtime&, Object* self, Values) { self->elements.clear(); return Value{}; });
  add("keySet", "()Ljava/util/Set;", [](Runtime& vm, Object* self, Values) {
    if (!self->payload.ref()) {
      auto* view = vm.heap.Allocate(vm, vm.linker.Resolve(kKeySet));
      view->payload = Value::Ref(self);
      self->payload = Value::Ref(view);
    }
    return self->payload;
  });

  Class& ordered = cl.Define("Ljava/util/LinkedHashMap;", map.descriptor);
  cl.AddMethod(ordered, "<init>", "()V", 0, no_op);
  cl.AddMethod(ordered, "<init>", "(I)V", 0, capacity);

  for (bool map_view : {false, true}) {
    auto& set = cl.Define(map_view ? kKeySet : "Ljava/util/HashSet;", kObject, 0,
                          {"Ljava/util/Set;", "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
    const auto bind = [&](auto name, auto sig, Builtin body) { cl.AddMethod(set, name, sig, 0, std::move(body)); };
    const auto values = [map_view](Object& self) -> std::vector<Value>& {
      return map_view ? self.payload.ref()->elements : self.elements;
    };
    const size_t stride = map_view ? 2 : 1;
    const auto items = [values, stride](Object& self) {
      const auto& source = values(self);
      std::vector<Value> snapshot;
      snapshot.reserve(source.size() / stride);
      for (size_t i = 0; i < source.size(); i += stride) snapshot.push_back(source[i]);
      return snapshot;
    };
    if (!map_view) bind("<init>", "()V", no_op);
    bind("size", "()I", [values, stride](Runtime&, Object* self, Values) { return Value::Int(values(*self).size() / stride); });
    bind("isEmpty", "()Z", [values](Runtime&, Object* self, Values) { return Value::Int(values(*self).empty()); });
    bind("clear", "()V", [values](Runtime&, Object* self, Values) { values(*self).clear(); return Value{}; });
    bind("contains", "(Ljava/lang/Object;)Z", [values, stride](Runtime& vm, Object* self, Values a) {
      const auto& entries = values(*self);
      return Value::Int(Find(vm, entries, a[0].ref(), stride) < entries.size());
    });
    bind("add", "(Ljava/lang/Object;)Z", [map_view](Runtime& vm, Object* self, Values a) {
      if (map_view) vm.Fail("Ljava/lang/UnsupportedOperationException;");
      if (Find(vm, self->elements, a[0].ref()) < self->elements.size()) return Value::Int(0);
      self->elements.push_back(a[0]);
      return Value::Int(1);
    });
    bind("remove", "(Ljava/lang/Object;)Z", [values, stride](Runtime& vm, Object* self, Values a) {
      auto& entries = values(*self);
      const auto i = Find(vm, entries, a[0].ref(), stride);
      if (i == entries.size()) return Value::Int(0);
      entries.erase(entries.begin() + i, entries.begin() + i + stride);
      return Value::Int(1);
    });
    bind("iterator", "()Ljava/util/Iterator;", [items](Runtime& vm, Object* self, Values) {
      return MakeCursor(vm, items(*self));
    });
    const Builtin to_array = [items](Runtime& vm, Object* self, Values a) {
      return MakeArray(vm, items(*self), a.empty() ? nullptr : &vm.NonNull(a[0].ref()));
    };
    bind("toArray", "()[Ljava/lang/Object;", to_array);
    bind("toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", to_array);
  }
}

void InstallUtilities(Runtime& vm) {
  auto& cl = vm.linker;
  auto& objects = cl.Define("Ljava/util/Objects;", kObject, art::kAccFinal);
  cl.AddMethod(objects, "equals", "(Ljava/lang/Object;Ljava/lang/Object;)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Int(JavaEquals(vm, a[0].ref(), a[1].ref())); });
  const Builtin non_null = [](Runtime& vm, Object*, Values a) {
    if (!a[0].ref()) vm.Fail("Ljava/lang/NullPointerException;",
        a.size() > 1 && a[1].ref() ? EncodeUtf8(a[1].ref()->text) : std::string{});
    return a[0];
  };
  cl.AddMethod(objects, "requireNonNull", "(Ljava/lang/Object;)Ljava/lang/Object;", art::kAccStatic, non_null);
  cl.AddMethod(objects, "requireNonNull", "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;", art::kAccStatic, non_null);
  cl.AddMethod(objects, "hashCode", "(Ljava/lang/Object;)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return a[0].ref() ? CallVirtual(vm, a[0].ref(), "hashCode", "()I") : Value{}; });

  auto& arrays = cl.Define("Ljava/util/Arrays;", kObject, art::kAccFinal);
  cl.AddMethod(arrays, "asList", "([Ljava/lang/Object;)Ljava/util/List;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        vm.NonNull(a[0].ref());
        Object* list = vm.heap.Allocate(vm, vm.linker.Resolve(kArrayList));
        list->payload = a[0];
        return Value::Ref(list);
      });
  cl.AddMethod(arrays, "sort", "([Ljava/lang/Object;Ljava/util/Comparator;)V", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        auto& objects = vm.NonNull(a[0].ref()).elements;
        RootScope roots(vm);
        roots.values = objects;
        auto sorted = objects;
        std::stable_sort(sorted.begin(), sorted.end(), [&](Value lhs, Value rhs) {
          if (!a[1].ref()) {
            return CallVirtual(vm, lhs.ref(), "compareTo", "(Ljava/lang/Object;)I", std::array{rhs}).i() < 0;
          }
          return CallVirtual(vm, a[1].ref(), "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I",
                             std::array{lhs, rhs}).i() < 0;
        });
        objects = std::move(sorted);
        return Value{};
      });
  cl.AddMethod(arrays, "toString", "([Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        if (!a[0].ref()) return StringValue(vm, u"null");
        std::u16string text = u"[";
        const auto& items = a[0].ref()->elements;
        for (size_t i = 0; i < items.size(); ++i) {
          if (i) text += u", ";
          text += JavaText(vm, items[i].ref() == a[0].ref() ? nullptr : items[i].ref());
        }
        text += u']';
        return StringValue(vm, std::move(text));
      });

  auto& collections = cl.Define("Ljava/util/Collections;", kObject, art::kAccFinal);
  auto& empty_list = cl.AddField(collections, "EMPTY_LIST", kList, true);
  cl.AddMethod(collections, "<clinit>", "()V", art::kAccStatic,
      [&empty_list](Runtime& vm, Object*, Values) { empty_list.value = MakeList(vm, {}, true); return Value{}; });
  cl.AddMethod(collections, "emptyList", "()Ljava/util/List;", art::kAccStatic,
      [&empty_list](Runtime&, Object*, Values) { return empty_list.value; });
  cl.AddMethod(collections, "singletonList", "(Ljava/lang/Object;)Ljava/util/List;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return MakeList(vm, a, true); });
  cl.AddMethod(collections, "addAll", "(Ljava/util/Collection;[Ljava/lang/Object;)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        bool changed = false;
        const auto& array = vm.NonNull(a[1].ref()).elements;
        for (size_t i = 0; i < array.size(); ++i) {
          changed |= CallVirtual(vm, a[0].ref(), "add", "(Ljava/lang/Object;)Z", std::array{array[i]}).i() != 0;
        }
        return Value::Int(changed);
      });
}

}  // namespace

void InstallCollections(Runtime& vm) {
  DeclareInterfaces(vm.linker);
  InstallList(vm);
  InstallMaps(vm);
  InstallUtilities(vm);
}

}  // namespace wa::interp
