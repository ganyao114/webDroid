#include "code_generator_wasm_hgraph.h"

#include "arch/instruction_set.h"
#include "base/calloc_arena_pool.h"
#include "base/scoped_arena_allocator.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_verifier.h"
#include "dex/dex_instruction-inl.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing/builder.h"
#include "optimizing/data_type-inl.h"
#include "optimizing/graph_checker.h"
#include "optimizing/nodes.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kDeferredError = "Landroid/content/pm/parsing/result/ParseInput$DeferredError;";
constexpr std::string_view kProbe = "getTargetSdkForChange";
constexpr std::array kRequiredClasses = {
    std::string_view("Landroid/content/pm/parsing/ApkLite;"),
    std::string_view("Landroid/content/pm/parsing/ApkLiteParseUtils;"),
    std::string_view("Landroid/content/pm/parsing/FrameworkParsingPackageUtils;"),
    std::string_view("Landroid/content/pm/parsing/PackageLite;"),
    std::string_view("Landroid/content/pm/parsing/result/ParseInput;"),
    std::string_view("Landroid/content/pm/parsing/result/ParseResult;"),
    std::string_view("Landroid/content/pm/parsing/result/ParseTypeImpl;"),
    std::string_view("Lcom/android/internal/pm/pkg/parsing/ParsingUtils;"),
    std::string_view("Landroid/content/pm/SharedLibraryInfo;"),
    std::string_view("Landroid/content/pm/VerifierInfo;"),
    std::string_view("Lcom/android/internal/pm/pkg/component/ParsedIntentInfo;"),
    std::string_view("Lcom/android/internal/pm/pkg/component/ParsedIntentInfoImpl;"),
    kDeferredError,
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::ifstream Open(const char* path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.is_open(), std::string("Cannot open ") + path);
  return input;
}

void CompileProbe(const art::DexFile& dex, const art::ClassAccessor& cls,
                  const art::ClassAccessor::Method& method, const char* output) {
  Require((method.GetAccessFlags() & art::kAccStatic) != 0 &&
              dex.GetMethodShortyView(method.GetIndex()) == "IJ",
          "Expected the upstream static long-to-int SDK gate");
  art::CallocArenaPool pool;
  art::ArenaAllocator allocator(&pool);
  art::ArenaStack stack(&pool);
  const uint32_t index = method.GetIndex();
  const art::DexCompilationUnit unit({}, nullptr, dex, method.GetCodeItem(),
                                     cls.GetClassDefIndex(), index, method.GetAccessFlags(),
                                     nullptr, {});
  auto* graph = new (&allocator) art::HGraph(&allocator, &stack, nullptr, dex, index,
                                            art::InstructionSet::kNone, art::kStatic);
  const art::CodeItemDebugInfoAccessor debug(dex, method.GetCodeItem(), index);
  art::HGraphBuilder builder(graph, &unit, debug, art::DataType::FromShorty(unit.GetShorty()[0]));
  const auto result = builder.BuildGraph();
  Require(result == art::kAnalysisSuccess && graph->IsInSsaForm(),
          "ART could not build SSA for the SDK gate, analysis result " + std::to_string(result));
  art::GraphChecker checker(graph);
  checker.Run();
  if (!checker.IsValid()) {
    checker.Dump(std::cerr);
    throw std::runtime_error("ART GraphChecker rejected the SDK gate");
  }

  const std::array methods = {wa::hgraph::WasmMethod{graph, kProbe}};
  const auto wasm = wa::hgraph::CodeGeneratorWasm::Compile(methods);
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(wasm.data()), wasm.size());
  file.close();
  Require(static_cast<bool>(file), "Could not write the HGraph WebAssembly module");
  std::cout << "HGraph compiled " << kDeferredError << "->" << kProbe << "(J)I\n";
}

struct ClassCounts {
  size_t methods = 0;
  size_t code_bytes = 0;
};

void Run(const char* dex_path, const char* profile_path, const char* probe_path) {
  auto input = Open(dex_path);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  art::ArtDexFileLoader loader(bytes.data(), bytes.size(), dex_path);
  std::string error;
  const auto dex = loader.OpenOne(0, 0, nullptr, /*verify=*/false, /*verify_checksum=*/false, &error);
  Require(dex != nullptr, error);
  const bool verified = art::dex::Verify(dex.get(), dex_path, /*verify_checksum=*/true, &error);
  Require(verified, error);

  std::map<std::string_view, ClassCounts> classes;
  std::array<size_t, art::kNumPackedOpcodes> histogram{};
  std::array<size_t, art::kNumPackedOpcodes> probe_histogram{};
  std::map<uint16_t, size_t> payloads;
  size_t methods = 0;
  size_t methods_with_code = 0;
  size_t code_bytes = 0;
  bool found_probe = false;
  for (const art::ClassAccessor cls : dex->GetClasses()) {
    const auto descriptor = cls.GetDescriptorView();
    auto& counts = classes[descriptor];
    for (const auto& method : cls.GetMethods()) {
      ++counts.methods;
      const bool is_probe =
          descriptor == kDeferredError && dex->GetMethodName(method.GetIndex()) == kProbe;
      const art::CodeItemDataAccessor code = method.GetInstructionsAndData();
      if (!code.HasCodeItem()) {
        Require(!is_probe, "The SDK gate has no DEX code item");
        continue;
      }
      ++methods_with_code;
      size_t width = 0;
      for (auto it = code.begin(), end = code.end(); it != end; ++it) {
        Require(it.DexPc() == width, "Noncontiguous ART instruction offsets");
        width += it->SizeInCodeUnits();
        const auto opcode = it->Opcode();
        // Payloads have the NOP opcode with a nonzero signature in the high byte.
        if (opcode == art::Instruction::NOP && it->VRegA_10x() != 0) {
          ++payloads[it->VRegA_10x() << 8];
        } else {
          ++histogram[opcode];
          if (is_probe) {
            ++probe_histogram[opcode];
          }
        }
      }
      Require(width == code.InsnsSizeInCodeUnits(), "ART instruction widths disagree");
      counts.code_bytes += 2 * width;
      if (is_probe) {
        Require(!found_probe, "More than one upstream SDK gate");
        CompileProbe(*dex, cls, method, probe_path);
        found_probe = true;
      }
    }
    methods += counts.methods;
    code_bytes += counts.code_bytes;
  }

  for (const auto descriptor : kRequiredClasses) {
    Require(classes.contains(descriptor), "Missing upstream class " + std::string(descriptor));
  }
  Require(found_probe, "Missing upstream ParseInput.DeferredError.getTargetSdkForChange");

  auto profile_file = Open(profile_path);
  const std::string profile{std::istreambuf_iterator<char>(profile_file),
                            std::istreambuf_iterator<char>()};
  std::ostringstream rows;
  rows << "\ndex_bytes " << bytes.size() << "\nclass_count " << classes.size()
       << "\nmethod_ids " << dex->NumMethodIds() << "\ndefined_methods " << methods
       << "\ncode_bytes " << code_bytes << "\n# descriptor defined_methods code_bytes\n";
  for (const auto& [descriptor, counts] : classes) {
    rows << descriptor << ' ' << counts.methods << ' ' << counts.code_bytes << '\n';
  }
  Require(profile.ends_with(rows.str()), "The boot profile disagrees with ART class data");

  const size_t instructions = std::accumulate(histogram.begin(), histogram.end(), size_t{0});
  const size_t covered = std::accumulate(probe_histogram.begin(), probe_histogram.end(), size_t{0});
  std::cout << "ART DexFileVerifier passed " << bytes.size() << " DEX bytes, " << classes.size()
            << " classes, " << methods << " defined methods (" << methods_with_code
            << " with code, " << dex->NumMethodIds() << " method IDs), " << code_bytes
            << " instruction bytes, " << instructions << " instructions\n";
  std::cout << "Opcode                  DEX     HGraph probe\n";
  for (size_t i = 0; i < histogram.size(); ++i) {
    if (histogram[i] != 0) {
      const auto opcode = static_cast<art::Instruction::Code>(i);
      std::cout << std::left << std::setw(23) << art::Instruction::Name(opcode)
                << std::right << std::setw(6) << histogram[i] << std::setw(13) << probe_histogram[i] << '\n';
    }
  }
  for (const auto& [signature, count] : payloads) {
    std::cout << "Payload signature 0x" << std::hex << signature << std::dec << ' ' << count << '\n';
  }
  std::cout << "HGraph probe compiled " << covered << " DEX instructions\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: test_aosp17_pm_dex aosp17_pm_core.dex pm_boot_profile.txt probe.wasm\n";
    return 2;
  }
  try {
    Run(argv[1], argv[2], argv[3]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
