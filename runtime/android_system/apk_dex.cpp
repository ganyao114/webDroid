#include "apk_dex.h"

#include "code_generator_wasm_hgraph.h"

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
#include "optimizing/data_type-inl.h"
#include "optimizing/dead_code_elimination.h"
#include "optimizing/graph_checker.h"
#include "optimizing/nodes.h"

#include <optional>
#include <string>

namespace wa::system {
namespace {

std::optional<uint32_t> FindClassIndex(const art::DexFile& dex, std::string_view descriptor) {
    for (uint32_t i = 0; i < dex.NumClassDefs(); ++i) {
        const art::ClassAccessor cls(dex, i);
        if (cls.GetDescriptorView() == descriptor)
            return i;
    }
    return std::nullopt;
}

bool CheckGraph(art::HGraph& graph) {
    art::GraphChecker checker(&graph);
    checker.Run();
    return checker.IsValid();
}

uint32_t CountInstructions(const art::HGraph& graph) {
    uint32_t count = 0;
    for (const auto* block : graph.GetReversePostOrder()) {
        for (const auto* instruction = block->GetFirstInstruction(); instruction;
             instruction = instruction->GetNext())
            ++count;
    }
    return count;
}

} // namespace

int32_t CompileApkDex(std::span<const uint8_t> bytes, std::string_view descriptor,
                      std::vector<uint8_t>& wasm, wa_system_apk_dex_stats_t& stats) {
    constexpr char location[] = "APK/classes.dex";
    std::string error;
    art::ArtDexFileLoader loader(bytes.data(), bytes.size(), location);
    const auto dex = loader.OpenOne(0, 0, nullptr, /*verify=*/false, /*verify_checksum=*/false, &error);
    if (!dex || !art::dex::Verify(dex.get(), location, /*verify_checksum=*/true, &error))
        return WA_ERR_INVALID;
    if (descriptor.empty())
        return WA_ERR_NOENT;
    const auto nested = std::string(descriptor.substr(0, descriptor.size() - 1)) + "$Wasm;";
    auto class_index = FindClassIndex(*dex, nested);
    if (!class_index)
        class_index = FindClassIndex(*dex, descriptor);
    if (!class_index)
        return WA_ERR_NOENT;

    const art::ClassAccessor cls(*dex, *class_index);
    art::CallocArenaPool pool;
    art::ArenaAllocator allocator(&pool);
    art::ArenaStack stack(&pool);
    const art::CompilerOptions options;
    std::vector<hgraph::WasmMethod> methods;
    wa_system_apk_dex_stats_t counts{};
    for (const auto& method : cls.GetDirectMethods()) {
        const auto flags = method.GetAccessFlags();
        if ((flags & art::kAccStatic) == 0)
            continue;
        const auto index = method.GetIndex();
        const art::DexCompilationUnit unit({}, nullptr, *dex, method.GetCodeItem(),
                                           cls.GetClassDefIndex(), index, flags, nullptr, {});
        auto* graph = new (&allocator) art::HGraph(&allocator, &stack, nullptr, *dex, index,
                                                  art::InstructionSet::kNone, art::kStatic);
        const art::CodeItemDebugInfoAccessor code(*dex, method.GetCodeItem(), index);
        art::HGraphBuilder builder(graph, &unit, code, art::DataType::FromShorty(unit.GetShorty()[0]));
        if (builder.BuildGraph() != art::kAnalysisSuccess || !graph->IsInSsaForm() || !CheckGraph(*graph))
            return WA_ERR_NOTSUP;

        counts.blocks_before += graph->GetReversePostOrder().size();
        counts.instructions_before += CountInstructions(*graph);
        art::HConstantFolding(graph, options).Run();
        counts.instructions_after_folding += CountInstructions(*graph);
        art::HDeadCodeElimination(graph, nullptr, art::HDeadCodeElimination::kDeadCodeEliminationPassName)
            .Run();
        if (!CheckGraph(*graph))
            return WA_ERR_INTERNAL;
        counts.instructions_after_dce += CountInstructions(*graph);
        counts.blocks_after_dce += graph->GetReversePostOrder().size();
        methods.push_back({graph, dex->GetMethodName(index)});
    }
    if (methods.empty())
        return WA_ERR_NOENT;
    wasm = hgraph::CodeGeneratorWasm::Compile(methods);
    counts.methods = methods.size();
    stats = counts;
    return WA_OK;
}

} // namespace wa::system
