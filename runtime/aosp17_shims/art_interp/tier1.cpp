#include "runtime_internal.h"

#if WA_INTERP_HGRAPH
#include <stdexcept>

#include "base/arena_allocator.h"
#include "base/calloc_arena_pool.h"
#include "base/scoped_arena_allocator.h"
#include "code_generator_wasm_hgraph.h"
#include "dex/code_item_accessors-inl.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing/builder.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/graph_checker.h"
#include "optimizing/nodes.h"
#include "runtime_globals.h"
#include "standalone_runtime.h"
#endif

namespace wa::interp {

void Runtime::Promote(Method& method) {
#if WA_INTERP_HGRAPH
  if (!method.code || !method.IsStatic() || method.shorty.find('L') != std::string::npos) return;
  const art::DexFile& dex = *method.owner->dex;
  const art::DexCompilationUnit unit({}, nullptr, dex, method.code, method.owner->definition,
                                    method.index, method.flags, nullptr, {});
  if (!wa::hgraph::IsStandaloneMethod(unit)) return;

  const art::CodeItemDebugInfoAccessor code(dex, method.code, method.index);
  if (code.TriesSize()) return;
  // The import-free tier does not need the heap, class initialization callbacks,
  // or Java exception delivery. Restrict promotion to scalar, non-throwing instructions.
  using I = art::Instruction;
  for (const auto& instruction : code) {
    const auto op = instruction->Opcode();
    const bool scalar = (op >= I::MOVE && op <= I::MOVE_WIDE_16 &&
                         (op < I::MOVE_OBJECT || op > I::MOVE_OBJECT_16)) ||
        (op >= I::RETURN_VOID && op <= I::RETURN_WIDE) ||
        (op >= I::CONST_4 && op <= I::CONST_WIDE_HIGH16) ||
        (op >= I::GOTO && op <= I::IF_LEZ) ||
        (op >= I::NEG_INT && op <= I::USHR_INT_LIT8) || op == I::NOP;
    if (!scalar || (op != I::NOP && instruction->IsThrow())) return;
  }

  try {
    art::CallocArenaPool pool;
    art::ArenaAllocator allocator(&pool);
    art::ArenaStack stack(&pool);
    auto* graph = new (&allocator) art::HGraph(&allocator, &stack, nullptr, dex, method.index,
                                             art::InstructionSet::kNone, art::kStatic);
    art::HGraphBuilder builder(graph, &unit, code, art::DataType::FromShorty(method.shorty[0]));
    if (builder.BuildGraph() != art::kAnalysisSuccess || !graph->IsInSsaForm()) return;
    art::GraphChecker checker(graph);
    checker.Run();
    if (!checker.IsValid()) return;

    constexpr std::string_view entry = "invoke";
    const std::array methods{wa::hgraph::WasmMethod{graph, entry}};
    auto wasm = wa::hgraph::CodeGeneratorWasm::Compile(methods);
    method.compiled = options.install_wasm(wasm, entry, method.shorty);
    if (method.compiled) method.stats.compiled_bytes = wasm.size();
  } catch (const std::runtime_error&) {
    // Unsupported SSA or a host that cannot install the module stays in Tier 0.
  }
#endif
}

}  // namespace wa::interp
