#include "runtime_internal.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/mman.h>

#include "base/mem_map.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file-inl.h"
#include "ziparchive/zip_archive.h"
#include "ziparchive/zip_error.h"

namespace wa::interp {
namespace {

std::unique_ptr<const art::DexFile> OpenDex(art::ArtDexFileLoader& loader,
                                           const std::string& location, bool verify,
                                           uint32_t checksum = 0) {
  std::string error;
  auto dex = loader.OpenOne(0, checksum, nullptr, verify, verify, &error);
  if (!dex) throw std::runtime_error("DEX " + location + ": " + error);
  return dex;
}

size_t DexEntryNumber(std::string_view name) {
  if (name == "classes.dex") return 1;
  name.remove_prefix(std::string_view("classes").size());
  name.remove_suffix(std::string_view(".dex").size());
  if (name.empty() || name.front() == '0') return 0;
  size_t index = 0;
  const char* end = name.data() + name.size();
  const auto result = std::from_chars(name.data(), end, index);
  return result.ec == std::errc{} && result.ptr == end && index >= 2 ? index : 0;
}

std::unique_ptr<const art::DexFile> OpenApkEntry(ZipArchiveHandle archive, const ZipEntry64& entry,
                                                const std::string& location, bool verify) {
  if (entry.uncompressed_length == 0 || entry.uncompressed_length > UINT32_MAX ||
      entry.uncompressed_length > std::vector<uint8_t>().max_size() || (entry.gpbf & 1)) {
    throw std::runtime_error("Invalid or encrypted DEX entry " + location);
  }
  const size_t size = static_cast<size_t>(entry.uncompressed_length);
#ifndef __EMSCRIPTEN__
  if (entry.method == kCompressStored && entry.compressed_length == size &&
      entry.offset % alignof(art::DexFile::Header) == 0) {
    static std::mutex mem_map_init;
    {
      std::lock_guard lock(mem_map_init);
      art::MemMap::Init();
    }
    std::string error;
    auto mapping = art::MemMap::MapFile(size, PROT_READ, MAP_PRIVATE, GetFileDescriptor(archive),
                                        entry.offset, false, location.c_str(), &error);
    if (mapping.IsValid()) {
      art::ArtDexFileLoader loader(std::move(mapping), location);
      return OpenDex(loader, location, verify, entry.crc32);
    }
  }
#endif
  // A compressed entry needs a backing buffer. Wasm also uses this path because
  // mmap of a virtual filesystem file copies it into linear memory.
  std::vector<uint8_t> bytes(size);
  if (const auto status = ExtractToMemory(archive, &entry, bytes.data(), size); status != 0) {
    throw std::runtime_error("DEX " + location + ": " + ErrorCodeString(status));
  }
  art::ArtDexFileLoader loader(std::move(bytes), location);
  return OpenDex(loader, location, verify, entry.crc32);
}

}  // namespace

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

void ClassLinker::LoadDexFile(std::string location, std::vector<uint8_t> bytes, bool fallback) {
  DexFiles files;
  art::ArtDexFileLoader loader(std::move(bytes), location);
  files.push_back(OpenDex(loader, location, runtime_.options.verify_dex));
  IndexDexFiles(std::move(files), fallback ? DexSource::Fallback : DexSource::File);
}

void ClassLinker::LoadApkDexFiles(const std::string& path) {
  ZipArchiveHandle handle = nullptr;
  const int32_t opened = OpenArchive(path.c_str(), &handle);
  const std::unique_ptr<ZipArchive, decltype(&CloseArchive)> archive(handle, CloseArchive);
  if (opened != 0) throw std::runtime_error("APK " + path + ": " + ErrorCodeString(opened));

  std::vector<std::pair<size_t, ZipEntry64>> entries;
  {
    void* cookie = nullptr;
    if (const auto status = StartIteration(handle, &cookie, "classes", ".dex"); status != 0) {
      throw std::runtime_error("APK " + path + ": " + ErrorCodeString(status));
    }
    const std::unique_ptr<void, decltype(&EndIteration)> iteration(cookie, EndIteration);
    int32_t status;
    ZipEntry64 entry{};
    std::string_view name;
    while ((status = Next(cookie, &entry, &name)) == 0) {
      if (const size_t number = DexEntryNumber(name); number != 0) entries.emplace_back(number, entry);
    }
    if (status != kIterationEnd) throw std::runtime_error("APK " + path + ": " + ErrorCodeString(status));
  }

  std::ranges::sort(entries, {}, &std::pair<size_t, ZipEntry64>::first);
  if (entries.empty() || entries.front().first != 1) {
    throw std::runtime_error("APK " + path + ": missing classes.dex");
  }
  DexFiles files;
  files.reserve(entries.size());
  for (const auto& [number, entry] : entries) {
    const std::string location = path + '!' + art::DexFileLoader::GetMultiDexZipEntryName(number - 1);
    files.push_back(OpenApkEntry(handle, entry, location, runtime_.options.verify_dex));
  }
  IndexDexFiles(std::move(files), DexSource::Apk);
}

void ClassLinker::IndexDexFiles(DexFiles files, DexSource source) {
  size_t definitions = 0;
  for (const auto& dex : files) {
    if (dex->NumClassDefs() > static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1) {
      throw std::runtime_error("Too many class definitions in " + dex->GetLocation());
    }
    definitions += dex->NumClassDefs();
  }
  decltype(dex_index_) pending;
  pending.reserve(definitions);
  for (const auto& dex : files) {
    for (uint32_t i = 0; i < dex->NumClassDefs(); ++i) {
      const auto definition = static_cast<uint16_t>(i);
      const auto descriptor = dex->GetTypeDescriptorView(dex->GetClassDef(definition).class_idx_);
      // Installed boot classes are always the parent namespace.
      const auto loaded = classes_.find(descriptor);
      if (loaded != classes_.end() && !loaded->second.dex) continue;
      const auto existing = dex_index_.find(descriptor);
      const bool inherits = existing != dex_index_.end() &&
          (source == DexSource::Fallback || (source == DexSource::Apk && !existing->second.from_apk));
      if ((existing != dex_index_.end() && !inherits) ||
          !pending.try_emplace(descriptor, DexClass{dex.get(), definition, source == DexSource::Apk}).second) {
        throw std::runtime_error("Duplicate DEX class " + std::string(descriptor) + " from " +
                                 dex->GetLocation());
      }
    }
  }
  if (source != DexSource::File) {
    std::erase_if(pending, [&](const auto& entry) { return dex_index_.contains(entry.first); });
  }
  dex_files_.reserve(dex_files_.size() + files.size());
  dex_index_.reserve(dex_index_.size() + pending.size());
  dex_index_.merge(pending);
  for (auto& dex : files) dex_files_.push_back(std::move(dex));
}

DexStats ClassLinker::GetDexStats() const {
  DexStats stats{dex_index_.size(), linked_dex_classes_, {}};
  stats.files.reserve(dex_files_.size());
  for (const auto& dex : dex_files_) {
    stats.files.push_back({dex->GetLocation(), dex->Size(), dex->NumClassDefs()});
  }
  return stats;
}

std::string ClassLinker::DexLocation(std::string_view descriptor) const {
  const auto it = dex_index_.find(descriptor);
  return it == dex_index_.end() ? std::string{} : it->second.dex->GetLocation();
}

Class& ClassLinker::Define(std::string_view name, std::string_view super, uint32_t flags,
                          std::initializer_list<std::string_view> interfaces) {
  if (dex_index_.contains(name)) throw std::logic_error("Duplicate boot class " + std::string(name));
  auto [it, added] = classes_.try_emplace(std::string(name));
  if (!added) throw std::logic_error("Duplicate boot class " + std::string(name));
  Class& cls = it->second;
  cls.descriptor = name;
  cls.kind = name.front();
  cls.superclass_descriptor = super;
  cls.flags = flags;
  cls.interface_descriptors.assign(interfaces.begin(), interfaces.end());
  return cls;
}

Field& ClassLinker::AddField(Class& cls, std::string_view name, std::string_view type,
                            bool is_static, Value initial) {
  auto [it, added] = cls.fields.emplace(MemberKey(name, type),
      Field{&cls, std::string(name), std::string(type), type.front(), is_static,
            is_static ? art::kAccStatic : 0u, 0, initial});
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
    if (const auto dex = dex_index_.find(descriptor); dex != dex_index_.end()) {
      it = classes_.try_emplace(std::string(descriptor)).first;
      Class& cls = it->second;
      cls.descriptor = descriptor;
      cls.dex = dex->second.dex;
      cls.definition = dex->second.definition;
    } else {
      if (!descriptor.starts_with('[') || descriptor.size() == 1 || descriptor.substr(1) == "V") {
        runtime_.Fail("Ljava/lang/NoClassDefFoundError;", descriptor);
      }
      Class& array = Define(descriptor, "Ljava/lang/Object;", art::kAccFinal,
                            {"Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
      Link(array);
      return array;
    }
  }
  Class& cls = it->second;
  Link(cls);
  return cls;
}

bool ClassLinker::HasClass(std::string_view descriptor) const {
  const size_t component = descriptor.find_first_not_of('[');
  if (component == std::string_view::npos || component > 255) return false;
  descriptor.remove_prefix(component);
  return (component == 0 || descriptor != "V") &&
      (classes_.contains(descriptor) || dex_index_.contains(descriptor));
}

void ClassLinker::LoadMembers(Class& cls) {
  const auto& dex = *cls.dex;
  const art::ClassAccessor access(dex, dex.GetClassDef(cls.definition));
  for (const auto& field : access.GetFields()) {
    const auto& id = dex.GetFieldId(field.GetIndex());
    const auto flags = field.GetAccessFlags();
    AddField(cls, dex.GetFieldNameView(id), dex.GetFieldTypeDescriptorView(id),
             (flags & art::kAccStatic) != 0).flags = flags;
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
  if (cls.link_state == LinkState::Failed) {
    runtime_.Fail("Ljava/lang/NoClassDefFoundError;", cls.descriptor, cls.failure);
  }
  cls.link_state = LinkState::Linking;
  try {
    if (cls.dex) {
      const auto& def = cls.dex->GetClassDef(cls.definition);
      cls.flags = def.access_flags_;
      if (def.superclass_idx_.IsValid()) {
        cls.super = &Resolve(cls.dex->GetTypeDescriptorView(def.superclass_idx_));
      }
      if (const auto* list = cls.dex->GetInterfacesList(def)) {
        cls.interfaces.reserve(list->Size());
        for (uint32_t i = 0; i < list->Size(); ++i) {
          cls.interfaces.push_back(&Resolve(cls.dex->GetTypeDescriptorView(list->GetTypeItem(i).type_idx_)));
        }
      }
    } else {
      if (!cls.superclass_descriptor.empty()) cls.super = &Resolve(cls.superclass_descriptor);
      for (const auto& iface : cls.interface_descriptors) cls.interfaces.push_back(&Resolve(iface));
    }
    if (cls.super) {
      cls.instance_slots = cls.super->instance_slots;
      cls.vtable = cls.super->vtable;
    }
    if (cls.IsArray()) cls.component = &Resolve(std::string_view(cls.descriptor).substr(1));
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
    if (cls.dex) ++linked_dex_classes_;
  } catch (const ManagedException&) {
    cls.failure = runtime_.exception;
    cls.link_state = LinkState::Failed;
    throw;
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
        field->value = Value::Ref(runtime_.InternLiteral(DecodeUtf8(dex.GetStringData(art::dex::StringIndex(literal.i)))));
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
    case InitState::Failed: runtime_.Fail("Ljava/lang/NoClassDefFoundError;", cls.descriptor, cls.failure);
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
    cls.failure = runtime_.exception;
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
  if (!target.IsInterface()) {
    for (const Class* cls = source.super; cls; cls = cls->super) {
      if (cls == &target) return true;
    }
    return false;
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
    mark(cls.failure);
    for (const Field* field : cls.field_order) {
      if (field->is_static) mark(field->value.ref());
    }
  }
}

}  // namespace wa::interp
