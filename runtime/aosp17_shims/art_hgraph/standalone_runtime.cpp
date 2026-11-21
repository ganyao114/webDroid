#include "standalone_runtime.h"

#include <string_view>

#include "dex/class_accessor-inl.h"
#include "dex/dex_file-inl.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing/nodes.h"

namespace wa::hgraph {
namespace {

bool IsPrimitiveStatic(uint32_t access, std::string_view shorty) {
  constexpr uint32_t excluded = art::kAccNative | art::kAccAbstract | art::kAccConstructor |
                               art::kAccSynchronized | art::kAccDeclaredSynchronized;
  return (access & (art::kAccStatic | excluded)) == art::kAccStatic &&
         shorty.find('L') == std::string_view::npos;
}

}  // namespace

bool IsStandaloneMethod(const art::DexCompilationUnit& unit) {
  const art::DexFile& dex = *unit.GetDexFile();
  if (unit.GetClassLinker() != nullptr || unit.GetDexMethodIndex() >= dex.NumMethodIds() ||
      unit.GetClassDefIndex() >= dex.NumClassDefs() || !unit.GetCodeItemAccessor().HasCodeItem() ||
      !IsPrimitiveStatic(unit.GetAccessFlags(), unit.GetShorty())) {
    return false;
  }

  const art::dex::ClassDef& cls = dex.GetClassDef(unit.GetClassDefIndex());
  // A standalone entry has no superclass, interface or class initializer to run.
  if (cls.class_idx_ != dex.GetMethodId(unit.GetDexMethodIndex()).class_idx_ ||
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
