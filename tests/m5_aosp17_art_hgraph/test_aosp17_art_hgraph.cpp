#include "code_generator_wasm_hgraph.h"
#include "standalone_runtime.h"
#include "wasmandroid/art_wasm.h"

#include "arch/instruction_set.h"
#include "base/calloc_arena_pool.h"
#include "base/scoped_arena_allocator.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_verifier.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing/builder.h"
#include "optimizing/constant_folding.h"
#include "optimizing/control_flow_simplifier.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/dead_code_elimination.h"
#include "optimizing/graph_checker.h"
#include "optimizing/instruction_simplifier.h"
#include "optimizing/nodes.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

static_assert(offsetof(wa_art_thread_t, ref_stack) == wa::hgraph::kRefStack);
static_assert(offsetof(wa_art_thread_t, ref_top) == wa::hgraph::kRefTop);
static_assert(offsetof(wa_art_thread_t, suspend_flag) == wa::hgraph::kSuspendFlag);
static_assert(offsetof(wa_art_thread_t, pending_exception) == wa::hgraph::kPendingException);
static_assert(offsetof(wa_art_thread_t, ref_capacity) == wa::hgraph::kRefCapacity);
using wa::hgraph::JavaException;
static_assert(static_cast<uint32_t>(JavaException::NullPointer) == WA_ART_NULL_POINTER);
static_assert(static_cast<uint32_t>(JavaException::Arithmetic) == WA_ART_ARITHMETIC);
static_assert(static_cast<uint32_t>(JavaException::ArrayBounds) == WA_ART_ARRAY_BOUNDS);
static_assert(static_cast<uint32_t>(JavaException::ClassCast) == WA_ART_CLASS_CAST);
static_assert(static_cast<uint32_t>(JavaException::StackOverflow) == WA_ART_STACK_OVERFLOW);
static_assert(static_cast<uint32_t>(JavaException::NoSuchMethod) == WA_ART_NO_SUCH_METHOD);

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct GraphCounts {
  size_t blocks = 0;
  size_t instructions = 0;
  size_t loop_phis = 0;
  size_t branches = 0;
  size_t multiplies = 0;
  size_t divides = 0;
  size_t calls = 0;
  size_t recursive_calls = 0;
  size_t adds_with_67 = 0;
  size_t selects = 0;
  size_t rotates = 0;
};

GraphCounts Count(art::HGraph& graph) {
  GraphCounts counts;
  for (art::HBasicBlock* block : graph.GetReversePostOrder()) {
    ++counts.blocks;
    for (const auto* phi = block->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      counts.loop_phis += block->IsLoopHeader();
    }
    for (const auto* instruction = block->GetFirstInstruction(); instruction != nullptr;
         instruction = instruction->GetNext()) {
      ++counts.instructions;
      counts.branches += instruction->IsIf();
      counts.multiplies += instruction->IsMul();
      counts.divides += instruction->IsDiv();
      counts.selects += instruction->IsSelect();
      counts.rotates += instruction->IsRor() || instruction->IsRol();
      if (instruction->IsInvokeStaticOrDirect()) {
        ++counts.calls;
        counts.recursive_calls += instruction->AsInvokeStaticOrDirect()->IsRecursive();
      }
      if (instruction->IsAdd()) {
        for (const auto* input : instruction->GetInputs()) {
          counts.adds_with_67 += input->IsIntConstant() &&
                                input->AsIntConstant()->GetValue() == 67;
        }
      }
    }
  }
  return counts;
}

void CheckGraph(art::HGraph& graph, std::string_view method, std::string_view stage) {
  art::GraphChecker checker(&graph);
  checker.Run();
  if (!checker.IsValid()) {
    checker.Dump(std::cerr);
    throw std::runtime_error(std::string(method) + " failed GraphChecker " + std::string(stage));
  }
}

void CheckOptimizations(std::string_view method,
                        const GraphCounts& before,
                        const GraphCounts& folded,
                        const GraphCounts& after,
                        bool folded_changed,
                        bool dce_changed) {
  if (method == "folded") {
    Require(before.branches == 1 && before.multiplies == 1 && before.divides == 1,
            "DEX lost the constant arithmetic or dead branch before ART");
    Require(folded_changed && folded.instructions < before.instructions &&
            folded.multiplies == 0 && folded.divides == 0 && folded.branches == 1,
            "Upstream constant folding did not replace the constant expression");
    Require(dce_changed && after.blocks < folded.blocks &&
            after.instructions < folded.instructions && after.branches == 0 &&
            after.adds_with_67 == 1,
            "Upstream DCE did not remove the dead branch and keep x + 67");
    std::cout << "folded instructions " << before.instructions << " -> " << folded.instructions
              << " -> " << after.instructions << ", blocks " << before.blocks << " -> "
              << folded.blocks << " -> " << after.blocks << " (SSA, folding, DCE)\n";
  } else if (method == "sumLoop" || method == "gcd" || method == "swapLoop") {
    Require(after.loop_phis >= 2, "Expected live loop-carried phis after upstream optimization");
    std::cout << method << " has " << after.loop_phis << " live loop phis\n";
  } else if (method == "fib") {
    Require(after.calls == 2 && after.recursive_calls == 2, "Expected two recursive invokes");
  } else if (method == "callHelper") {
    Require(after.calls == 1 && after.recursive_calls == 0, "Expected a local static helper call");
  }
}

void WriteWasm(const char* output, std::span<const uint8_t> wasm) {
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(wasm.data()), wasm.size());
  file.close();
  Require(static_cast<bool>(file), "Could not write the emitted WebAssembly module");
}

void RunObjects(const art::DexFile& dex, const char* output) {
  art::CallocArenaPool pool;
  art::ArenaAllocator allocator(&pool);
  art::ArenaStack stack(&pool);
  const art::CompilerOptions compiler_options;
  const wa::hgraph::ScopedObjectCompilation objects(dex);
  std::vector<wa::hgraph::WasmMethod> methods;
  std::set<std::string_view> nodes;
  size_t reference_loop_phis = 0;
  size_t catch_phis = 0;
  for (const art::ClassAccessor cls : dex.GetClasses()) {
    for (const auto& method : cls.GetMethods()) {
      if (method.GetCodeItem() == nullptr) {
        continue;  // Abstract declarations remain available for virtual resolution.
      }
      const uint32_t index = method.GetIndex();
      const auto flags = method.GetAccessFlags();
      const auto kind = method.GetInvokeType(cls.GetClassDef().access_flags_);
      const std::string name = dex.PrettyMethod(index);
      const art::DexCompilationUnit unit({}, nullptr, dex, method.GetCodeItem(),
                                         cls.GetClassDefIndex(), index, flags, nullptr, {});
      auto* graph = new (&allocator) art::HGraph(&allocator, &stack, nullptr, dex, index,
                                                art::InstructionSet::kNone, kind);
      const art::CodeItemDebugInfoAccessor debug_code(dex, method.GetCodeItem(), index);
      art::HGraphBuilder builder(graph, &unit, debug_code,
                                 art::DataType::FromShorty(unit.GetShorty()[0]));
      const auto result = builder.BuildGraph();
      Require(result == art::kAnalysisSuccess && graph->IsInSsaForm(),
              name + " failed HGraphBuilder (analysis result " + std::to_string(result) + ")");
      CheckGraph(*graph, name, "after SSA");
      art::HConstantFolding(graph, compiler_options).Run();
      CheckGraph(*graph, name, "after constant folding");
      art::HDeadCodeElimination(
          graph, nullptr, art::HDeadCodeElimination::kDeadCodeEliminationPassName).Run();
      CheckGraph(*graph, name, "after dead code elimination");
      for (const auto* block : graph->GetReversePostOrder()) {
        for (const auto* phi = block->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
          reference_loop_phis += block->IsLoopHeader() &&
                                 phi->GetType() == art::DataType::Type::kReference;
          catch_phis += block->IsCatchBlock();
        }
        for (const auto* inst = block->GetFirstInstruction(); inst != nullptr; inst = inst->GetNext()) {
          nodes.insert(inst->DebugName());
        }
      }
      const bool exported = cls.GetDescriptorView() == "LHGraphObjectsProbe;" &&
                            kind == art::kStatic && (flags & art::kAccPublic) != 0;
      methods.push_back({graph, exported ? dex.GetMethodName(index) : std::string_view{}});
    }
  }
  for (const auto* node : {"NewInstance", "NewArray", "ArrayLength", "ArrayGet", "ArraySet",
                           "InstanceFieldGet", "InstanceFieldSet", "StaticFieldGet",
                           "StaticFieldSet", "InvokeStaticOrDirect", "InvokeVirtual",
                           "InvokeInterface", "LoadString", "CheckCast", "InstanceOf",
                           "NullCheck", "BoundsCheck", "Throw", "LoadException", "TryBoundary"}) {
    Require(nodes.contains(node), std::string("Expected optimized HGraph node ") + node);
  }
  Require(reference_loop_phis != 0 && catch_phis != 0, "Expected reference loop and catch phis");
  WriteWasm(output, wa::hgraph::CodeGeneratorWasm::CompileObjects(methods));
  std::cout << "ART verified DEX and checked " << methods.size() << " optimized object SSA graphs, "
            << reference_loop_phis << " reference loop phis and " << catch_phis << " catch phis\n";
}

void Run(const char* dex_path, const char* output, bool objects) {
  std::ifstream input(dex_path, std::ios::binary);
  Require(input.is_open(), "Could not open classes.dex");
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  std::string error;
  const std::string location = objects ? "HGraphObjectsProbe/classes.dex" : "HGraphProbe/classes.dex";
  art::ArtDexFileLoader loader(bytes.data(), bytes.size(), location);
  auto dex = loader.OpenOne(0, 0, nullptr, /*verify=*/false, /*verify_checksum=*/false, &error);
  Require(dex != nullptr, error);
  const bool verified = art::dex::Verify(dex.get(), location.c_str(), /*verify_checksum=*/true,
                                         &error);
  Require(verified, error);
  if (objects) {
    RunObjects(*dex, output);
    return;
  }
  Require(dex->NumClassDefs() == 1, "Expected one probe class");

  art::CallocArenaPool pool;
  art::ArenaAllocator allocator(&pool);
  art::ArenaStack stack(&pool);
  const art::CompilerOptions compiler_options;
  const art::ClassAccessor cls(*dex, 0u);
  Require(cls.GetDescriptorView() == "LHGraphProbe;", "Unexpected DEX class");
  std::vector<wa::hgraph::WasmMethod> methods;
  for (const auto& method : cls.GetDirectMethods()) {
    const bool is_static = (method.GetAccessFlags() & art::kAccStatic) != 0;
    const uint32_t index = method.GetIndex();
    const std::string_view name = dex->GetMethodName(index);
    const art::CodeItemDataAccessor code = method.GetInstructionsAndData();
    Require(code.HasCodeItem() && code.TriesSize() == 0, "Expected a primitive DEX method body");
    const art::DexCompilationUnit unit({}, nullptr, *dex, method.GetCodeItem(),
                                       cls.GetClassDefIndex(), index, method.GetAccessFlags(),
                                       nullptr, {});
    auto* graph = new (&allocator) art::HGraph(&allocator, &stack, nullptr, *dex, index,
                                              art::InstructionSet::kNone,
                                              is_static ? art::kStatic : art::kDirect);
    const art::CodeItemDebugInfoAccessor debug_code(*dex, method.GetCodeItem(), index);
    art::HGraphBuilder builder(graph, &unit, debug_code,
                               art::DataType::FromShorty(unit.GetShorty()[0]));
    const auto result = builder.BuildGraph();
    if (!is_static) {
      Require(name == "<init>" && result == art::kAnalysisSkipped && graph->GetBlocks().empty(),
              "Expected the standalone builder to reject the instance constructor");
      continue;
    }
    Require(result == art::kAnalysisSuccess && graph->IsInSsaForm(),
            std::string("HGraphBuilder failed for ") + std::string(name) +
            " (analysis result " + std::to_string(result) + ")");
    CheckGraph(*graph, name, "after SSA");
    const GraphCounts before = Count(*graph);
    const bool folded_changed = art::HConstantFolding(graph, compiler_options).Run();
    CheckGraph(*graph, name, "after constant folding");
    const GraphCounts folded = Count(*graph);
    const bool dce_changed = art::HDeadCodeElimination(
        graph, nullptr, art::HDeadCodeElimination::kDeadCodeEliminationPassName).Run();
    CheckGraph(*graph, name, "after dead code elimination");
    CheckOptimizations(name, before, folded, Count(*graph), folded_changed, dce_changed);
    const bool simplified = art::InstructionSimplifier(graph, nullptr).Run();
    CheckGraph(*graph, name, "after instruction simplification");
    if (name == "twist" || name == "hashMix") {
      Require(simplified && Count(*graph).rotates == 1,
              "Upstream instruction simplifier did not form a rotate");
    }
    const bool control_flow_changed = art::HControlFlowSimplifier(graph, nullptr).Run();
    CheckGraph(*graph, name, "after control-flow simplification");
    if (name == "choose") {
      Require(control_flow_changed && Count(*graph).selects == 1,
              "Upstream control-flow simplifier did not form HSelect");
    }
    methods.push_back({graph, name});
  }
  WriteWasm(output, wa::hgraph::CodeGeneratorWasm::Compile(methods));
  std::cout << "ART verified DEX and checked " << methods.size() << " optimized SSA graphs\n";
}

}  // namespace

int main(int argc, char** argv) {
  const bool objects = argc == 4 && std::string_view(argv[3]) == "--objects";
  if (argc != 3 && !objects) {
    std::cerr << "Usage: test_aosp17_art_hgraph classes.dex output.wasm [--objects]\n";
    return 2;
  }
  try {
    Run(argv[1], argv[2], objects);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
