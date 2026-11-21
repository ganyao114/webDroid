// Expose the M0 imports from Emscripten's JS library under the wa namespace.
// This hook runs for both the main instance and its pthread workers.
const binderWasmImports = getWasmImports;
getWasmImports = () => {
  const imports = binderWasmImports();
  imports.wa = imports.env;
  return imports;
};
