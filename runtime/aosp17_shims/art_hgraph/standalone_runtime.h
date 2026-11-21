#pragma once

#include <cstdint>
#include <iosfwd>

#include "base/logging.h"

namespace art {
class CompilerOptions;
class DexCompilationUnit;
class HInstruction;
}

namespace wa::hgraph {

[[noreturn]] inline void RequiresArtRuntime(const char* operation) {
  LOG(FATAL) << operation << " requires the ART runtime";
  UNREACHABLE();
}

[[noreturn]] inline const art::CompilerOptions& NoNativeCodeGenerator() {
  RequiresArtRuntime("native code generation");
}

bool IsStandaloneMethod(const art::DexCompilationUnit& unit);
bool IsLocalStaticTarget(const art::DexCompilationUnit& unit, uint32_t method_index);
std::ostream& PrintInstruction(std::ostream& output, art::HInstruction& instruction);

}  // namespace wa::hgraph
