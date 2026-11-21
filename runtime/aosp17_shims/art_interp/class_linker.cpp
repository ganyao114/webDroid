#include "runtime_internal.h"

#include <algorithm>
#include <stdexcept>

#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_verifier.h"

namespace wa::interp {

std::string MemberKey(std::string_view name, std::string_view signature) {
  std::string key(name);
  key += ':';
  key += signature;
  return key;
}

std::string Shorty(std::string_view signature) {
  size_t end = signature.find(')');
  if (signature.empty() || signature.front() != '(' || end >= signature.size() - 1) {
    throw std::invalid_argument("Invalid method descriptor " + std::string(signature));
  }
  const auto short_type = [](char type) { return IsReference(type) ? 'L' : type; };
  std::string shorty(1, short_type(signature[end + 1]));
  for (size_t i = 1; i < end; ++i) {
    shorty += short_type(signature[i]);
    while (i < end && signature[i] == '[') ++i;
    if (signature[i] == 'L') i = signature.find(';', i);
    if (i >= end) throw std::invalid_argument("Invalid parameter in " + std::string(signature));
  }
  return shorty;
}

bool Method::IsVirtual() const {
  return (flags & (art::kAccStatic | art::kAccPrivate)) == 0 && !name.starts_with('<');
}

std::string Method::QualifiedName() const { return owner->descriptor + "->" + name + signature; }

void ClassLinker::LoadDexFile(std::string location, std::vector<uint8_t> bytes) {
  DexUnit unit{std::move(bytes), nullptr};
  art::ArtDexFileLoader loader(unit.bytes.data(), unit.bytes.size(), location);
  std::string error;
  unit.dex = loader.OpenOne(0, 0, nullptr, /*verify=*/false, /*verify_checksum=*/false, &error);
  if (!unit.dex || !art::dex::Verify(unit.dex.get(), location.c_str(), true, &error)) {
    throw std::runtime_error("DEX " + location + ": " + error);
  }

  decltype(classes_) pending;
  for (const art::ClassAccessor accessor : unit.dex->GetClasses()) {
    std::string descriptor(accessor.GetDescriptorView());
    if (classes_.contains(descriptor) || pending.contains(descriptor)) {
      throw std::runtime_error("Duplicate DEX class " + descriptor + " from " + location);
    }
    Class& cls = pending[descriptor];
    cls.descriptor = std::move(descriptor);
    cls.dex = unit.dex.get();
    cls.definition = accessor.GetClassDefIndex();
    const auto& def = unit.dex->GetClassDef(cls.definition);
    cls.flags = def.access_flags_;
    if (def.superclass_idx_.IsValid()) {
      cls.superclass_descriptor = unit.dex->GetTypeDescriptorView(def.superclass_idx_);
    }
    if (const auto* list = unit.dex->GetInterfacesList(def)) {
      for (uint32_t i = 0; i < list->Size(); ++i) {
        cls.interface_descriptors.emplace_back(unit.dex->GetTypeDescriptorView(list->GetTypeItem(i).type_idx_));
      }
    }
  }
  dex_files_.push_back(std::move(unit));
  classes_.merge(pending);
}

Class& ClassLinker::Define(std::string_view name, std::string_view super, uint32_t flags,
                          std::initializer_list<std::string_view> interfaces) {
  auto [it, added] = classes_.try_emplace(std::string(name));
  if (!added) throw std::logic_error("Duplicate boot class " + std::string(name));
  Class& cls = it->second;
  cls.descriptor = name;
  cls.superclass_descriptor = super;
  cls.flags = flags;
  cls.interface_descriptors.assign(interfaces.begin(), interfaces.end());
  return cls;
}

Field& ClassLinker::AddField(Class& cls, std::string_view name, std::string_view type,
                            bool is_static, Value initial) {
  auto [it, added] = cls.fields.emplace(MemberKey(name, type),
      Field{&cls, std::string(name), std::string(type), is_static, 0, initial});
  if (!added) throw std::logic_error("Duplicate field " + cls.descriptor + "->" + std::string(name));
  cls.field_order.push_back(&it->second);
  return it->second;
}

Method& ClassLinker::AddMethod(Class& cls, std::string_view name, std::string_view signature,
                              uint32_t flags, Builtin builtin, std::string_view shorty) {
  auto [it, added] = cls.methods.emplace(MemberKey(name, signature),
      Method{&cls, std::string(name), std::string(signature),
             shorty.empty() ? Shorty(signature) : std::string(shorty), flags});
  if (!added) throw std::logic_error("Duplicate method " + it->second.QualifiedName());
  it->second.builtin = std::move(builtin);
  return it->second;
}

Class& ClassLinker::Resolve(std::string_view descriptor) {
  auto it = classes_.find(descriptor);
  if (it == classes_.end()) {
    if (!descriptor.starts_with('[') || descriptor.size() == 1 || descriptor.substr(1) == "V") {
      runtime_.Fail("Ljava/lang/NoClassDefFoundError;", descriptor);
    }
    Define(descriptor, "Ljava/lang/Object;", art::kAccFinal,
           {"Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
    it = classes_.find(descriptor);
  }
  Link(it->second);
  return it->second;
}

void ClassLinker::LoadMembers(Class& cls) {
  const auto& dex = *cls.dex;
  const art::ClassAccessor access(dex, dex.GetClassDef(cls.definition));
  for (const auto& field : access.GetFields()) {
    const auto& id = dex.GetFieldId(field.GetIndex());
    AddField(cls, dex.GetFieldNameView(id), dex.GetFieldTypeDescriptorView(id),
             (field.GetAccessFlags() & art::kAccStatic) != 0);
  }
  for (const auto& method : access.GetMethods()) {
    const auto& id = dex.GetMethodId(method.GetIndex());
    Method& target = AddMethod(cls, dex.GetMethodNameView(id), dex.GetMethodSignature(id).ToString(),
                               method.GetAccessFlags(), {}, dex.GetMethodShortyView(id));
    target.index = method.GetIndex();
    target.code = method.GetCodeItem();
  }
}

void ClassLinker::Link(Class& cls) {
  if (cls.link_state == LinkState::Linked) return;
  if (cls.link_state == LinkState::Linking) runtime_.Fail("Ljava/lang/ClassCircularityError;", cls.descriptor);
  if (cls.link_state == LinkState::Failed) runtime_.Fail("Ljava/lang/NoClassDefFoundError;", cls.descriptor);
  cls.link_state = LinkState::Linking;
  try {
    if (!cls.superclass_descriptor.empty()) {
      cls.super = &Resolve(cls.superclass_descriptor);
      cls.instance_slots = cls.super->instance_slots;
      cls.vtable = cls.super->vtable;
    }
    if (cls.IsArray()) cls.component = &Resolve(std::string_view(cls.descriptor).substr(1));
    for (const auto& iface : cls.interface_descriptors) cls.interfaces.push_back(&Resolve(iface));
    if (cls.dex) LoadMembers(cls);
    for (Field* field : cls.field_order) {
      if (!field->is_static) field->slot = cls.instance_slots++;
    }
    for (auto& [key, method] : cls.methods) {
      if (method.IsVirtual()) cls.vtable.insert_or_assign(key, &method);
    }

    std::vector<Class*> interfaces;
    const auto add_interfaces = [&](auto&& self, const Class& current) -> void {
      for (Class* iface : current.interfaces) {
        if (std::ranges::find(interfaces, iface) == interfaces.end()) {
          interfaces.push_back(iface);
          self(self, *iface);
        }
      }
    };
    for (const Class* current = &cls; current; current = current->super) add_interfaces(add_interfaces, *current);

    // Class methods take precedence. For interface defaults, retain only the most specific
    // declaration. An abstract redeclaration also shadows a less specific default.
    std::unordered_map<std::string, std::vector<Method*>> candidates;
    for (Class* iface : interfaces) {
      for (auto& [key, declaration] : iface->methods) {
        if (declaration.IsVirtual()) candidates[key].push_back(&declaration);
      }
    }
    for (const auto& [key, declarations] : candidates) {
      auto implementation = cls.vtable.find(key);
      Method* target = implementation == cls.vtable.end() ? nullptr : implementation->second;
      if (!target || (target->owner != &cls && target->owner->IsInterface())) {
        target = nullptr;
        for (Method* candidate : declarations) {
          if (candidate->IsAbstract()) continue;
          const bool shadowed = std::ranges::any_of(declarations, [&](const Method* other) {
            return other != candidate && IsAssignable(*candidate->owner, *other->owner);
          });
          if (shadowed) continue;
          if (target) {  // No unique default.
            target = nullptr;
            break;
          }
          target = candidate;
        }
        if (target) cls.vtable.insert_or_assign(key, target);
        else cls.vtable.erase(key);
      }
      for (Method* declaration : declarations) cls.itable.emplace(declaration, target);
    }
    cls.link_state = LinkState::Linked;
  } catch (...) {
    cls.link_state = LinkState::Failed;
    throw;
  }
}

void ClassLinker::InitializeConstants(Class& cls) {
  if (!cls.dex) return;
  const auto& dex = *cls.dex;
  using Encoded = art::EncodedStaticFieldValueIterator;
  Encoded constants(dex, dex.GetClassDef(cls.definition));
  for (Field* field : cls.field_order) {
    if (!field->is_static || !constants.HasNext()) break;
    const jvalue literal = constants.GetJavaValue();
    switch (constants.GetValueType()) {
      case Encoded::kByte: field->value = Value::Int(literal.b); break;
      case Encoded::kShort: field->value = Value::Int(literal.s); break;
      case Encoded::kChar: field->value = Value::Int(literal.c); break;
      case Encoded::kInt: field->value = Value::Int(literal.i); break;
      case Encoded::kLong: field->value = Value::Long(literal.j); break;
      case Encoded::kFloat: field->value = Value::Float(literal.f); break;
      case Encoded::kDouble: field->value = Value::Double(literal.d); break;
      case Encoded::kBoolean: field->value = Value::Int(literal.z); break;
      case Encoded::kNull: field->value = {}; break;
      case Encoded::kString:
        field->value = Value::Ref(runtime_.Intern(DecodeUtf8(dex.GetStringData(art::dex::StringIndex(literal.i)))));
        break;
      case Encoded::kType:
        field->value = Value::Ref(runtime_.ClassObject(Resolve(dex.GetTypeDescriptorView(art::dex::TypeIndex(literal.i)))));
        break;
      default: runtime_.Fail("Ljava/lang/VerifyError;", "Unsupported static field constant");
    }
    constants.Next();
  }
}

void ClassLinker::Initialize(Class& cls) {
  switch (cls.init_state) {
    case InitState::Initialized:
    case InitState::Initializing: return;
    case InitState::Failed: runtime_.Fail("Ljava/lang/NoClassDefFoundError;", cls.descriptor);
    case InitState::Uninitialized: break;
  }
  cls.init_state = InitState::Initializing;
  try {
    InitializeConstants(cls);
    if (!cls.IsInterface()) {
      if (cls.super) Initialize(*cls.super);
      const auto init_defaults = [&](auto&& self, Class& current) -> void {
        for (Class* iface : current.interfaces) self(self, *iface);
        if (std::ranges::any_of(current.methods, [](const auto& entry) {
              return entry.second.IsVirtual() && !entry.second.IsAbstract();
            })) {
          Initialize(current);
        }
      };
      for (Class* iface : cls.interfaces) init_defaults(init_defaults, *iface);
    }
    auto method = cls.methods.find(MemberKey("<clinit>", "()V"));
    if (method != cls.methods.end()) runtime_.Invoke(method->second, nullptr, {});
    cls.init_state = InitState::Initialized;
  } catch (const ManagedException&) {
    cls.init_state = InitState::Failed;
    if (!IsAssignable(Resolve("Ljava/lang/Error;"), *runtime_.exception->type)) {
      Object* cause = runtime_.exception;
      runtime_.exception = runtime_.MakeException("Ljava/lang/ExceptionInInitializerError;", cls.descriptor);
      runtime_.exception->payload = Value::Ref(cause);
    }
    throw;
  } catch (...) {
    cls.init_state = InitState::Failed;
    throw;
  }
}

Method& ClassLinker::FindMethod(Class& cls, std::string_view name,
                               std::string_view signature, bool is_static) {
  const std::string key = MemberKey(name, signature);
  for (Class* current = &cls; current; current = current->super) {
    auto it = current->methods.find(key);
    if (it != current->methods.end()) {
      if (it->second.IsStatic() != is_static) {
        runtime_.Fail("Ljava/lang/IncompatibleClassChangeError;", it->second.QualifiedName());
      }
      return it->second;
    }
    if (name.starts_with('<') || (is_static && cls.IsInterface())) break;
  }
  if (!is_static && !name.starts_with('<')) {
    auto virtual_method = cls.vtable.find(key);
    if (virtual_method != cls.vtable.end()) return *virtual_method->second;
    for (const auto& [declaration, target] : cls.itable) {
      if (declaration->name == name && declaration->signature == signature) {
        return *declaration;
      }
    }
  }
  runtime_.Fail("Ljava/lang/NoSuchMethodError;", cls.descriptor + "->" + std::string(name) + std::string(signature));
}

Method& ClassLinker::VirtualMethod(Class& receiver, Method& method) {
  if (!method.IsVirtual()) return method;
  if (!IsAssignable(*method.owner, receiver)) {
    runtime_.Fail("Ljava/lang/IncompatibleClassChangeError;", method.QualifiedName());
  }
  if (method.owner->IsInterface()) {
    const auto it = receiver.itable.find(&method);
    if (it != receiver.itable.end() && it->second) return *it->second;
  } else {
    const auto it = receiver.vtable.find(MemberKey(method.name, method.signature));
    if (it != receiver.vtable.end()) return *it->second;
  }
  runtime_.Fail("Ljava/lang/AbstractMethodError;", method.QualifiedName());
}

Field& ClassLinker::FindField(Class& cls, std::string_view name, std::string_view type, bool is_static) {
  const std::string key = MemberKey(name, type);
  const auto find = [&](auto&& self, Class& current) -> Field* {
    const auto it = current.fields.find(key);
    if (it != current.fields.end()) return &it->second;
    for (Class* iface : current.interfaces) {
      if (Field* match = self(self, *iface)) return match;
    }
    return current.super ? self(self, *current.super) : nullptr;
  };
  Field* field = find(find, cls);
  if (!field) runtime_.Fail("Ljava/lang/NoSuchFieldError;", cls.descriptor + "->" + key);
  if (field->is_static != is_static) runtime_.Fail("Ljava/lang/IncompatibleClassChangeError;", key);
  return *field;
}

bool ClassLinker::IsAssignable(const Class& target, const Class& source) const {
  if (&target == &source) return true;
  if (target.IsPrimitive() || source.IsPrimitive()) return false;
  if (target.IsArray()) {
    return source.IsArray() && IsAssignable(*target.component, *source.component);
  }
  for (const Class* cls = &source; cls; cls = cls->super) {
    if (cls == &target) return true;
    for (Class* iface : cls->interfaces) {
      if (IsAssignable(target, *iface)) return true;
    }
  }
  return false;
}

void ClassLinker::VisitRoots(const std::function<void(Object*)>& mark) const {
  for (const auto& [name, cls] : classes_) {
    mark(cls.mirror);
    for (const Field* field : cls.field_order) {
      if (field->is_static) mark(field->value.ref());
    }
  }
}

}  // namespace wa::interp
