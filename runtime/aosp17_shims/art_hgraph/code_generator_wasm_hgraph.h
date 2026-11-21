#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace art {
class HGraph;
}

namespace wa::hgraph {

struct WasmMethod {
  art::HGraph* graph;
  std::string_view export_name;
};

class CodeGeneratorWasm {
 public:
  // The methods are checked, optimized static SSA graphs from one DEX file.
  // The module contains every call target and requires no imports.
  static std::vector<uint8_t> Compile(std::span<const WasmMethod> methods);

 private:
  class FunctionEmitter;
};

}  // namespace wa::hgraph
