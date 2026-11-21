// Included in ART's BuildInvoke at the managed resolution boundary.
  const uint32_t resolved = wa::hgraph::ResolveMethod(*dex_compilation_unit_, method_idx, invoke_type);
  if (resolved == dex::kDexNoIndex) {
    return false;
  }

  MethodReference method_reference(dex_file_, method_idx);
  const MethodReference resolved_method_reference(dex_file_, resolved);
  ArtMethod* const resolved_method = nullptr;
  // The Wasm runtime uses DEX IDs for virtual and interface lookup.
  const uint32_t imt_or_vtable_index = resolved;
  constexpr auto clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kNone;
  HClinitCheck* const clinit_check = nullptr;
