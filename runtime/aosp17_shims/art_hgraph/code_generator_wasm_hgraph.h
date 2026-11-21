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

  // Compile graphs built with ScopedObjectCompilation. Exported methods take
  // the wa_art_thread_t address first, followed by the receiver (if any) and the
  // Java arguments. An empty export_name keeps a method internal. All methods
  // are installed in the imported env.dispatch_table at their DEX method IDs.
  // The caller binds env.memory and the env.rt_* imports to one shadow heap
  // and provides an env.java_exception tag with one i32 reference parameter.
  static std::vector<uint8_t> CompileObjects(std::span<const WasmMethod> methods);

 private:
  class FunctionEmitter;
  static std::vector<uint8_t> CompileModule(std::span<const WasmMethod> methods, bool managed);
};

}  // namespace wa::hgraph
