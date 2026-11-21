#include "standalone_runtime.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "dex/class_accessor-inl.h"
#include "dex/dex_file-inl.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/nodes.h"
#include "runtime_globals.h"

// Field nodes link the ART page-size ABI through their native implicit-check
// methods. This backend uses explicit checks and never calls those methods.
#ifdef ART_PAGE_SIZE_AGNOSTIC
const size_t art::PageSize::value_ = art::GetPageSizeSlow();
#endif

namespace wa::hgraph {
namespace {

using Type = art::DataType::Type;
using TypeIndex = art::dex::TypeIndex;

thread_local const art::DexFile* object_dex = nullptr;

bool IsPrimitiveStatic(uint32_t access, std::string_view shorty) {
  constexpr uint32_t excluded = art::kAccNative | art::kAccAbstract | art::kAccConstructor |
                               art::kAccSynchronized | art::kAccDeclaredSynchronized;
  return (access & (art::kAccStatic | excluded)) == art::kAccStatic &&
         shorty.find('L') == std::string_view::npos;
}

const art::HInstruction* UnwrapReference(const art::HInstruction* value) {
  while (value->IsNullCheck() || value->IsBoundType()) {
    value = value->InputAt(0);
  }
  return value;
}

// ART's primitive SSA propagation only needs the array component descriptor.
// Read it from checked casts and typed definitions without managed Class handles.
std::string_view Descriptor(const art::HInstruction* value,
                            const art::HInstruction* use,
                            std::unordered_set<const art::HInstruction*>& active);

std::string_view DefinitionDescriptor(const art::HInstruction* value,
                                      std::unordered_set<const art::HInstruction*>& active) {
  const art::DexFile& dex = value->GetBlock()->GetGraph()->GetDexFile();
  if (value->IsNewArray()) {
    const auto* cls = value->AsNewArray()->InputAt(0)->AsLoadClass();
    return cls->GetDexFile().GetTypeDescriptorView(cls->GetTypeIndex());
  }
  if (value->IsParameterValue()) {
    return dex.GetTypeDescriptorView(value->AsParameterValue()->GetTypeIndex());
  }
  if (value->IsInstanceFieldGet() || value->IsStaticFieldGet()) {
    const auto& field = value->AsFieldAccess()->GetFieldInfo();
    return field.GetDexFile().GetFieldTypeDescriptorView(field.GetFieldIndex());
  }
  if (value->IsInvoke()) {
    const auto target = value->AsInvoke()->GetMethodReference();
    const auto& method = target.dex_file->GetMethodId(target.index);
    return target.dex_file->GetTypeDescriptorView(
        target.dex_file->GetMethodPrototype(method).return_type_idx_);
  }
  if (value->IsArrayGet() && value->GetType() == Type::kReference) {
    const auto array = Descriptor(value->InputAt(0), value, active);
    return array.starts_with('[') ? array.substr(1) : std::string_view{};
  }
  if (value->IsPhi() || value->IsSelect()) {
    std::string_view descriptor;
    const auto* block = value->GetBlock();
    const size_t count = value->IsPhi() ? value->InputCount() : 2;
    for (size_t i = 0; i < count; ++i) {
      const auto* input_use = value->IsPhi()
          ? (block->IsCatchBlock() ? nullptr : block->GetPredecessors()[i]->GetLastInstruction())
          : value;
      const auto input = Descriptor(value->InputAt(i), input_use, active);
      if (input.empty()) {
        continue;  // A null or a loop-carried alias adds no type information.
      }
      if (!descriptor.empty() && descriptor != input) {
        return {};
      }
      descriptor = input;
    }
    return descriptor;
  }
  return {};
}

std::string_view Descriptor(const art::HInstruction* value,
                            const art::HInstruction* use,
                            std::unordered_set<const art::HInstruction*>& active) {
  value = UnwrapReference(value);
  if (use != nullptr) {
    for (auto* block = use->GetBlock(); block != nullptr; block = block->GetDominator()) {
      for (const auto* inst = block == use->GetBlock() ? use->GetPrevious()
                                                       : block->GetLastInstruction();
           inst != nullptr;
           inst = inst->GetPrevious()) {
        if (inst->IsCheckCast() && UnwrapReference(inst->InputAt(0)) == value) {
          const auto* cls = inst->InputAt(1)->AsLoadClass();
          const auto descriptor = cls->GetDexFile().GetTypeDescriptorView(cls->GetTypeIndex());
          if (descriptor.starts_with('[')) {
            return descriptor;
          }
        }
      }
    }
  }
  if (!active.insert(value).second) {
    return {};
  }
  auto descriptor = DefinitionDescriptor(value, active);
  active.erase(value);
  return descriptor;
}

}  // namespace

ScopedObjectCompilation::ScopedObjectCompilation(const art::DexFile& dex)
    : previous_(object_dex) {
  object_dex = &dex;
}

ScopedObjectCompilation::~ScopedObjectCompilation() {
  object_dex = previous_;
}

bool IsObjectCompilation(const art::DexFile& dex) {
  return object_dex == &dex;
}

bool IsStandaloneMethod(const art::DexCompilationUnit& unit) {
  const art::DexFile& dex = *unit.GetDexFile();
  if (unit.GetClassLinker() != nullptr || unit.GetDexMethodIndex() >= dex.NumMethodIds() ||
      unit.GetClassDefIndex() >= dex.NumClassDefs() || !unit.GetCodeItemAccessor().HasCodeItem()) {
    return false;
  }

  const art::dex::ClassDef& cls = dex.GetClassDef(unit.GetClassDefIndex());
  if (IsObjectCompilation(dex)) {
    constexpr uint32_t excluded = art::kAccNative | art::kAccAbstract |
                                  art::kAccSynchronized | art::kAccDeclaredSynchronized;
    return cls.class_idx_ == dex.GetMethodId(unit.GetDexMethodIndex()).class_idx_ &&
           (unit.GetAccessFlags() & excluded) == 0;
  }

  // A standalone entry has no superclass, interface or class initializer to run.
  if (!IsPrimitiveStatic(unit.GetAccessFlags(), unit.GetShorty()) ||
      cls.class_idx_ != dex.GetMethodId(unit.GetDexMethodIndex()).class_idx_ ||
      !cls.superclass_idx_.IsValid() || cls.interfaces_off_ != 0 ||
      dex.GetTypeDescriptorView(cls.superclass_idx_) != "Ljava/lang/Object;") {
    return false;
  }
  for (const auto& method : art::ClassAccessor(dex, cls).GetDirectMethods()) {
    if (std::string_view(dex.GetMethodName(method.GetIndex())) == "<clinit>") {
      return false;
    }
  }
  return true;
}

bool IsLocalStaticTarget(const art::DexCompilationUnit& unit, uint32_t method_index) {
  const art::DexFile& dex = *unit.GetDexFile();
  if (method_index >= dex.NumMethodIds()) {
    return false;
  }
  const art::dex::ClassDef& cls = dex.GetClassDef(unit.GetClassDefIndex());
  const art::dex::MethodId& target = dex.GetMethodId(method_index);
  if (target.class_idx_ != cls.class_idx_) {
    return false;
  }
  for (const auto& method : art::ClassAccessor(dex, cls).GetDirectMethods()) {
    if (method.GetIndex() == method_index) {
      return method.GetCodeItem() != nullptr &&
             IsPrimitiveStatic(method.GetAccessFlags(), dex.GetMethodShorty(target));
    }
  }
  return false;
}

bool IsBuiltinConstructor(const art::DexFile& dex, uint32_t method_index) {
  if (std::string_view(dex.GetMethodName(method_index)) != "<init>") {
    return false;
  }
  const auto& id = dex.GetMethodId(method_index);
  const auto owner = dex.GetTypeDescriptorView(id.class_idx_);
  const auto shorty = std::string_view(dex.GetMethodShorty(id));
  if (owner == "Ljava/lang/Object;") {
    return shorty == "V";
  }
  constexpr std::string_view exceptions[] = {
      "Ljava/lang/Throwable;", "Ljava/lang/Exception;", "Ljava/lang/RuntimeException;",
      "Ljava/lang/Error;", "Ljava/lang/VirtualMachineError;", "Ljava/lang/LinkageError;",
      "Ljava/lang/IncompatibleClassChangeError;", "Ljava/lang/IndexOutOfBoundsException;",
      "Ljava/lang/NullPointerException;", "Ljava/lang/ArithmeticException;",
      "Ljava/lang/ArrayIndexOutOfBoundsException;", "Ljava/lang/NegativeArraySizeException;",
      "Ljava/lang/ClassCastException;", "Ljava/lang/ArrayStoreException;",
      "Ljava/lang/OutOfMemoryError;", "Ljava/lang/StackOverflowError;",
      "Ljava/lang/UnsatisfiedLinkError;", "Ljava/lang/NoSuchMethodError;",
      "Ljava/lang/NoSuchFieldError;",
  };
  return (shorty == "V" || shorty == "VL") &&
         std::find(std::begin(exceptions), std::end(exceptions), owner) != std::end(exceptions);
}

ResolvedField ResolveField(const art::DexFile& dex, uint32_t index, bool is_static) {
  const auto& symbolic = dex.GetFieldId(index);
  std::vector<TypeIndex> pending{symbolic.class_idx_};
  std::unordered_set<uint16_t> seen;
  while (!pending.empty()) {
    const auto owner = pending.back();
    pending.pop_back();
    if (!owner.IsValid() || !seen.insert(owner.index_).second) {
      continue;
    }
    const auto* def = dex.FindClassDef(owner);
    if (def == nullptr) {
      continue;
    }
    const art::ClassAccessor cls(dex, *def);
    for (const auto& field : cls.GetFields()) {
      const auto& candidate = dex.GetFieldId(field.GetIndex());
      if (candidate.name_idx_ == symbolic.name_idx_ && candidate.type_idx_ == symbolic.type_idx_ &&
          field.IsStatic() == is_static) {
        return {owner, field.GetIndex(), static_cast<uint16_t>(cls.GetClassDefIndex()),
                (field.GetAccessFlags() & art::kAccVolatile) != 0};
      }
    }
    pending.push_back(def->superclass_idx_);
    if (const auto* interfaces = dex.GetInterfacesList(*def)) {
      for (size_t i = interfaces->Size(); i > 0; --i) {
        pending.push_back(interfaces->GetTypeItem(i - 1).type_idx_);
      }
    }
  }
  return {TypeIndex::Invalid(), 0, 0, false};
}

uint32_t ResolveMethod(const art::DexCompilationUnit& unit,
                       uint32_t index,
                       art::InvokeType invoke_type) {
  const auto& dex = *unit.GetDexFile();
  if (!IsObjectCompilation(dex)) {
    return invoke_type == art::kStatic && IsLocalStaticTarget(unit, index)
        ? index : art::dex::kDexNoIndex;
  }
  if (invoke_type == art::kDirect && IsBuiltinConstructor(dex, index)) {
    return index;
  }
  const auto& symbolic = dex.GetMethodId(index);
  const auto owner = invoke_type == art::kSuper
      ? dex.GetClassDef(unit.GetClassDefIndex()).superclass_idx_ : symbolic.class_idx_;
  std::vector<TypeIndex> pending{owner};
  std::unordered_set<uint16_t> seen;
  while (!pending.empty()) {
    const auto type = pending.back();
    pending.pop_back();
    if (!type.IsValid() || !seen.insert(type.index_).second) {
      continue;
    }
    const auto* def = dex.FindClassDef(type);
    if (def == nullptr) {
      continue;
    }
    for (const auto& method : art::ClassAccessor(dex, *def).GetMethods()) {
      const auto& candidate = dex.GetMethodId(method.GetIndex());
      if (candidate.name_idx_ != symbolic.name_idx_ || candidate.proto_idx_ != symbolic.proto_idx_) {
        continue;
      }
      const auto flags = method.GetAccessFlags();
      if (invoke_type == art::kVirtual || invoke_type == art::kInterface) {
        if (!method.IsStaticOrDirect()) {
          return method.GetIndex();
        }
      } else if ((flags & (art::kAccStatic | art::kAccNative | art::kAccAbstract)) ==
                     (invoke_type == art::kStatic ? art::kAccStatic : 0u) &&
                 method.GetCodeItem() != nullptr &&
                 (invoke_type == art::kSuper || method.IsStaticOrDirect())) {
        return method.GetIndex();
      }
    }
    if (invoke_type != art::kDirect) {
      pending.push_back(def->superclass_idx_);
    }
    if (invoke_type == art::kInterface) {
      if (const auto* interfaces = dex.GetInterfacesList(*def)) {
        for (size_t i = interfaces->Size(); i > 0; --i) {
          pending.push_back(interfaces->GetTypeItem(i - 1).type_idx_);
        }
      }
    }
  }
  return art::dex::kDexNoIndex;
}

Type ArrayComponentType(const art::HInstruction* array,
                        const art::HInstruction* access,
                        Type null_component) {
  std::unordered_set<const art::HInstruction*> active;
  const auto descriptor = Descriptor(array, access, active);
  if (descriptor.size() == 2 && descriptor.front() == '[') {
    return art::DataType::FromShorty(descriptor[1]);
  }
  // A literal null reaches the check before any array operation. Its DEX
  // register carries only the width and type evidence from the operation's uses.
  return UnwrapReference(array)->IsNullConstant() ? null_component : Type::kVoid;
}

std::ostream& PrintInstruction(std::ostream& output, art::HInstruction& instruction) {
  output << instruction.DebugName() << ':' << instruction.GetId();
  for (const art::HInstruction* input : instruction.GetInputs()) {
    if (input != nullptr) {
      output << ' ' << input->GetId();
    }
  }
  return output;
}

}  // namespace wa::hgraph
