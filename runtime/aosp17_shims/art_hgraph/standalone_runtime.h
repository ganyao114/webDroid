#pragma once

#include <cstdint>
#include <iosfwd>

#include "base/logging.h"
#include "dex/dex_file_types.h"
#include "dex/invoke_type.h"
#include "optimizing/data_type.h"

namespace art {
class CompilerOptions;
class DexFile;
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

// Enable DEX-only object resolution for calls to ART's HGraphBuilder in this scope.
// With no scope, HGraphBuilder retains its original standalone scalar contract.
class ScopedObjectCompilation {
 public:
  explicit ScopedObjectCompilation(const art::DexFile& dex);
  ~ScopedObjectCompilation();
  ScopedObjectCompilation(const ScopedObjectCompilation&) = delete;
  ScopedObjectCompilation& operator=(const ScopedObjectCompilation&) = delete;

 private:
  const art::DexFile* previous_;
};

bool IsObjectCompilation(const art::DexFile& dex);
bool IsBuiltinConstructor(const art::DexFile& dex, uint32_t method_index);

struct ResolvedField {
  art::dex::TypeIndex owner;
  uint32_t index;
  uint16_t class_def_index;
  bool is_volatile;
};

ResolvedField ResolveField(const art::DexFile& dex, uint32_t index, bool is_static);
uint32_t ResolveMethod(const art::DexCompilationUnit& unit,
                       uint32_t index,
                       art::InvokeType invoke_type);
art::DataType::Type ArrayComponentType(const art::HInstruction* array,
                                      const art::HInstruction* access,
                                      art::DataType::Type null_component);

// Offsets in the wasm32 wa_art_thread_t ABI. Checked against art_wasm.h by the probe.
inline constexpr uint32_t kRefStack = 0;
inline constexpr uint32_t kRefTop = 4;
inline constexpr uint32_t kSuspendFlag = 8;
inline constexpr uint32_t kPendingException = 12;
inline constexpr uint32_t kRefCapacity = 16;

enum class JavaException : uint32_t {
  NullPointer = 0,
  Arithmetic = 1,
  ArrayBounds = 2,
  ClassCast = 4,
  StackOverflow = 7,
  NoSuchMethod = 9,
};

}  // namespace wa::hgraph
