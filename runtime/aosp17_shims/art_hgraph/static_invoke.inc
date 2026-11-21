// Included in ART's instruction_builder.cc at the class-linker boundary.
bool HInstructionBuilder::BuildInvoke(const Instruction& instruction,
                                      uint32_t dex_pc,
                                      uint32_t method_idx,
                                      const InstructionOperands& operands) {
  DCHECK(instruction.Opcode() == Instruction::INVOKE_STATIC ||
         instruction.Opcode() == Instruction::INVOKE_STATIC_RANGE);
  if (!wa::hgraph::IsLocalStaticTarget(*dex_compilation_unit_, method_idx)) {
    return false;
  }

  const char* shorty = dex_file_->GetMethodShorty(method_idx);
  MethodReference target(dex_file_, method_idx);
  const bool recursive = graph_->GetMethodIdx() == method_idx;
  HInvokeStaticOrDirect::DispatchInfo dispatch = {
      recursive ? MethodLoadKind::kRecursive : MethodLoadKind::kRuntimeCall,
      recursive ? CodePtrLocation::kCallSelf : CodePtrLocation::kCallArtMethod,
      0u};
  auto* invoke = new (allocator_) HInvokeStaticOrDirect(
      allocator_,
      strlen(shorty) - 1u,
      operands.GetNumberOfOperands(),
      DataType::FromShorty(shorty[0]),
      dex_pc,
      target,
      /*resolved_method=*/nullptr,
      dispatch,
      kStatic,
      target,
      HInvokeStaticOrDirect::ClinitCheckRequirement::kNone,
      /*enable_intrinsic_opt=*/false);
  return HandleInvoke(invoke, operands, shorty, /*is_unresolved=*/false);
}
