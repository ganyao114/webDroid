"""Prepare the pinned ART compiler sources for a build without native codegen."""

from pathlib import Path
import sys


def replace(source, original, replacement, count=1):
    if source.count(original) != count:
        raise ValueError(f"ART source layout changed at {original!r}")
    return source.replace(original, replacement)


def replace_body(source, signature, body):
    start = source.index("{", source.index(signature)) + 1
    end = source.index("\n}", start)
    return source[:start] + "\n" + body + source[end:]


def replace_range(source, first, after, replacement=""):
    start = source.index(first)
    end = source.index(after, start + len(first))
    return source[:start] + replacement + source[end:]


def without_native_codegen(source):
    return replace(source, '#include "code_generator.h"\n', "")


def compiler_options(source):
    # Keep the real default options constructor and destructor. Command-line
    # parsers, profiles and image options are outside the standalone compiler.
    source = replace_range(source, "#include <fstream>", "namespace art HIDDEN", """\
#include <fstream>
#include "arch/instruction_set_features.h"

""")
    return source[:source.index("\nnamespace {")] + "\n\n}  // namespace art\n"


def dex_compilation_unit(source):
    source = replace(source, '#include "scoped_thread_state_change-inl.h"',
                     '#include "standalone_runtime.h"')
    return replace_range(source, "    // Decoding class data", "  } else {\n    // Iterate",
                         '    wa::hgraph::RequiresArtRuntime("resolved constructor");\n')


def builder(source):
    source = without_native_codegen(source)
    source = replace(source, '#include "nodes.h"\n',
                     '#include "nodes.h"\n#include "standalone_runtime.h"\n')
    source = replace_body(source, "bool HGraphBuilder::SkipCompilation()", """\
  CHECK(code_generator_ == nullptr);
  return false;""")
    source = replace_body(source, "void HGraphBuilder::BuildIntrinsicGraph(",
                          '  wa::hgraph::RequiresArtRuntime("intrinsic graph builder");')
    return replace(source, "  DCHECK(code_item_accessor_.HasCodeItem());", """\
  if (dex_compilation_unit_ == nullptr ||
      graph_->GetMethodIdx() != dex_compilation_unit_->GetDexMethodIndex() ||
      &graph_->GetDexFile() != dex_compilation_unit_->GetDexFile() ||
      graph_->GetArtMethod() != nullptr ||
      !wa::hgraph::IsStandaloneMethod(*dex_compilation_unit_) ||
      graph_->IsDebuggable() || graph_->IsCompilingOsr() || graph_->IsCompilingBaseline() ||
      graph_->GetProfilingInfo() != nullptr || !code_item_accessor_.HasCodeItem()) {
    return kAnalysisSkipped;
  }
  if ((graph_->GetInvokeType() == kStatic) != dex_compilation_unit_->IsStatic() ||
      (!wa::hgraph::IsObjectCompilation(graph_->GetDexFile()) &&
       (graph_->GetInvokeType() != kStatic || code_item_accessor_.TriesSize() != 0))) {
    return kAnalysisSkipped;
  }
  DCHECK(code_item_accessor_.HasCodeItem());""")


def graph_checker(source):
    source = without_native_codegen(source)
    source = replace(source, '#include "intrinsics.h"', '#include "upstream/optimizing/intrinsics.h"')
    source = replace_range(source, "    bool check_values = false;",
                           "  } else {\n    if (!input->IsLoadClass()", """\
    AddError("Bitstring type checks require the ART runtime.");
""")
    start = source.index("  if (codegen_ == nullptr) {", source.index("void GraphChecker::VisitVecOperation"))
    end = source.index("\n}", start)
    return source[:start] + "  CHECK(codegen_ == nullptr);" + source[end:]


def instruction_builder(source):
    for header in ("art_method-inl.h", "class_linker-inl.h", "code_generator.h",
                   "entrypoints/entrypoint_utils-inl.h", "handle_cache-inl.h", "imtable-inl.h",
                   "intrinsics.h", "intrinsics_utils.h", "jit/jit.h", "jit/profiling_info.h",
                   "mirror/dex_cache.h", "oat/oat_file.h", "reflective_handle_scope-inl.h",
                   "sharpening.h", "well_known_classes.h"):
        source = replace(source, f'#include "{header}"\n', "")
    source = replace(source, '#include "ssa_builder.h"\n',
                     '#include "ssa_builder.h"\n#include "standalone_runtime.h"\n')
    source = replace_range(source, "namespace {", "HInstructionBuilder::HInstructionBuilder")
    source = replace_range(source, "void HInstructionBuilder::BuildIntrinsic(",
                           "ArenaBitVector* HInstructionBuilder::FindNativeDebugInfoLocations()")
    source = replace_range(source, "  ProfilingInfo* info = graph_->GetProfilingInfo();",
                           "  AppendInstruction(if_instr);")
    source = replace_range(source, "      // Note: There may be no Thread for gtests.",
                           "      if (!ProcessDexInstruction(")
    source = replace_range(source, "      DCHECK(Thread::Current()", "\n    }")
    # Substitute managed method resolution. Keep ART's argument setup, receiver
    # null checks, environments and move-result handling.
    source = replace_range(source, "// Try to resolve a method using the class linker.",
                           "bool HInstructionBuilder::BuildInvoke(")
    source = replace_range(source, "  MethodReference resolved_method_reference(nullptr, 0u);",
                           "  HInvoke* invoke = nullptr;\n  if (invoke_type == kDirect",
                           '#include "dex_invoke.inc"\n\n')
    source = replace_range(source, "    // For sharpening, we create another MethodReference,",
                           "    invoke = new (allocator_) HInvokeStaticOrDirect(allocator_", """\
    if (invoke_type == kSuper) {
      method_reference.index = resolved;
    }
    const bool recursive = graph_->GetMethodIdx() == resolved;
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        recursive ? MethodLoadKind::kRecursive : MethodLoadKind::kRuntimeCall,
        recursive ? CodePtrLocation::kCallSelf : CodePtrLocation::kCallArtMethod,
        0u};
""")
    source = replace_range(source,
                           "    if (kIsDebugBuild) {\n"
                           "      ScopedObjectAccess soa(Thread::Current());\n"
                           "      DCHECK(resolved_method->GetDeclaringClass()->IsInterface());",
                           "    invoke = new (allocator_) HInvokeInterface(",
                           "    constexpr MethodLoadKind load_kind = MethodLoadKind::kRuntimeCall;\n")
    start = source.index("bool HInstructionBuilder::BuildInvoke(")
    end = source.index("\n}", start)
    source = (source[:start] +
              replace(source[start:end], "!graph_->IsDebuggable());",
                      "/*enable_intrinsic_opt=*/false);", 3) +
              source[end:])
    source = replace_range(source, "// This function initializes `VarHandleOptimizations`",
                           "HNewInstance* HInstructionBuilder::BuildNewInstance")
    source = replace_range(source, "static bool IsInImage(ObjPtr<mirror::Class>",
                           "bool HInstructionBuilder::SetupInvokeArguments")
    source = replace_range(source,
                           "  ScopedObjectAccess soa(Thread::Current());\n\n"
                           "  HLoadClass* load_class = BuildLoadClass(type_index, dex_pc);",
                           "  HNewInstance* new_instance", """\
  HInstruction* cls = BuildLoadClass(type_index, dex_pc);
  constexpr QuickEntrypointEnum entrypoint = kQuickAllocObjectWithChecks;
  constexpr bool finalizable = false;

""")
    source = replace_range(source, "  if (allocation->IsNewInstance()) {",
                           "  // JLS 17.4.5", """\
  if (allocation->IsNewInstance() && allocation->AsNewInstance()->IsStringAlloc()) {
    return;
  }

""")
    source = replace_range(source, "  if (invoke->IsInvokePolymorphic()) {",
                           "  return true;\n}", "")
    source = replace_range(source, "  if (invoke->IsInvokePolymorphic()) {",
                           "  return true;\n}", "")
    source = replace_range(source, "bool HInstructionBuilder::BuildSimpleIntrinsic",
                           "static DataType::Type GetFieldAccessType")
    source = replace_range(source, "  ScopedObjectAccess soa(Thread::Current());\n"
                           "  ArtField* resolved_field = ResolveField(field_index, /* is_static= */ false",
                           "  DataType::Type field_type = GetFieldAccessType(*dex_file_, field_index);",
                           """\
  const auto field = wa::hgraph::ResolveField(*dex_file_, field_index, /*is_static=*/false);
  if (!field.owner.IsValid()) {
    return false;
  }
  HInstruction* object = LoadNullCheckedLocal(obj_reg, dex_pc);

""")
    for access in ("set", "get"):
        source = replace_range(source, f"    HInstruction* field_{access} = nullptr;",
                               f"field_{access} = new (allocator_) HInstanceField",
                               "    auto* ")
        source = replace(source, f"dex_pc);\n    }}\n    AppendInstruction(field_{access});",
                         f"dex_pc);\n    AppendInstruction(field_{access});")
    source = replace_range(source,
                           "ArtField* HInstructionBuilder::ResolveField(",
                           "void HInstructionBuilder::BuildStaticFieldAccess")
    source = replace_range(source, "  ScopedObjectAccess soa(Thread::Current());\n"
                           "  ArtField* resolved_field = ResolveField(field_index, /* is_static= */ true",
                           "  if (is_put) {\n    // We need to keep the class alive", """\
  const auto field = wa::hgraph::ResolveField(*dex_file_, field_index, /*is_static=*/true);
  const DataType::Type field_type = GetFieldAccessType(*dex_file_, field_index);
  if (!field.owner.IsValid()) {
    BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
    return;
  }
  HInstruction* cls = BuildLoadClass(field.owner, dex_pc);

""")
    # Field nodes compare offsets in optimizer passes. Use a unique offset
    # per resolved field identity. The Wasm emitter accesses fields by DEX ID.
    for original, replacement in (
        ("resolved_field,", "/*resolved_field=*/nullptr,"),
        ("resolved_field->GetOffset(),", "MemberOffset(field.index * sizeof(uint64_t)),"),
        ("resolved_field->IsVolatile(),", "field.is_volatile,"),
        ("class_def_index,\n", "field.class_def_index,\n"),
    ):
        source = replace(source, original, replacement, 4)

    source = replace_range(source, "  HSharpening::ProcessLoadString(load_string,",
                           "  AppendInstruction(load_string);")
    source = replace_range(source, "  ScopedObjectAccess soa(Thread::Current());\n"
                           "  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();\n"
                           "  Handle<mirror::Class> klass = ResolveClass(soa, type_index);",
                           "  HLoadClass* load_class = new (allocator_) HLoadClass(", """\
  const DexFile* actual_dex_file = dex_file_;
  Handle<mirror::Class> klass;
  const bool is_referrers_class = false;
  const bool needs_access_check = false;
""")
    source = replace_range(source, "  HLoadClass::LoadKind load_kind = HSharpening",
                           "  AppendInstruction(load_class);")
    source = replace_range(source, "Handle<mirror::Class> HInstructionBuilder::ResolveClass(",
                           "void HInstructionBuilder::BuildTypeCheck(bool")
    source = replace_range(source, "  ScopedObjectAccess soa(Thread::Current());\n"
                           "  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();",
                           "  DCHECK(class_or_null != nullptr);", """\
  Handle<mirror::Class> klass;
  constexpr TypeCheckKind check_kind = TypeCheckKind::kUnresolvedCheck;
  HInstruction* class_or_null = BuildLoadClass(type_index, dex_pc);
  HIntConstant* bitstring_path_to_root = nullptr;
  HIntConstant* bitstring_mask = nullptr;
""")
    source = replace_range(source, "    // We emit a CheckCast followed by a BoundType.",
                           "    AppendInstruction(\n        new (allocator_) HCheckCast", """\
    // The Wasm runtime checks the cast without changing the reference value.
""")
    source = replace(source, "    AppendInstruction(new (allocator_) HBoundType(object, dex_pc));\n",
                     "")
    source = replace(source, """\
  } else {
    DCHECK_EQ(instruction.Opcode(), Instruction::CHECK_CAST);
    UpdateLocal(reference, current_block_->GetLastInstruction());
""", "")
    source = replace_range(source, "ObjPtr<mirror::Class> HInstructionBuilder::LookupResolvedType",
                           "}  // namespace art")

    # Keep upstream opcode decoding and IR construction for the operations
    # above. Bytecodes that require the managed runtime fail as unsupported.
    for first, after in (("INVOKE_POLYMORPHIC:", "NEG_INT:"),
                         ("CONST_CLASS:", "MOVE_EXCEPTION:"),
                         ("MONITOR_ENTER:", "SPARSE_SWITCH:")):
        source = replace_range(source, f"    case Instruction::{first}", f"    case Instruction::{after}")
    source = replace_range(source, "    case Instruction::UNUSED_3E",
                           '      VLOG(compiler) << "Did not compile "', "    default: {\n")
    return replace(source, "code_generator_->GetCompilerOptions()",
                   "wa::hgraph::NoNativeCodeGenerator()", 4)


def instruction_simplifier(source):
    for header in ("art_method-inl.h", "class_linker-inl.h", "class_root-inl.h",
                   "driver/compiler_options.h", "escape.h", "handle_cache-inl.h",
                   "intrinsic_objects.h", "intrinsics_utils.h", "mirror/class-inl.h",
                   "scoped_thread_state_change-inl.h", "sharpening.h", "string_builder_append.h",
                   "well_known_classes.h"):
        source = replace(source, f'#include "{header}"\n', "")
    source = replace(source, '#include "intrinsics.h"', '#include "upstream/optimizing/intrinsics.h"')
    for declaration in ("bool CanUseKnownImageVarHandle(HInvoke* invoke);",
                        "static bool CanEnsureNotNullAt(HInstruction* input, HInstruction* at);",
                        "void VisitInstanceFieldSet(HInstanceFieldSet* equal);",
                        "void VisitStaticFieldSet(HStaticFieldSet* equal);",
                        "void VisitArraySet(HArraySet* equal);",
                        "void VisitNullCheck(HNullCheck* instruction);",
                        "void VisitArrayLength(HArrayLength* instruction);",
                        "void VisitCheckCast(HCheckCast* instruction);",
                        "void VisitInstanceOf(HInstanceOf* instruction);",
                        "void VisitInvoke(HInvoke* invoke);"):
        source = replace(source, "  " + declaration + "\n", "")
    for first, after in (
        ("  void SimplifyBoxUnbox(", "  // Returns an instruction with the opposite Boolean value"),
        ("void InstructionSimplifierVisitor::VisitNullCheck(", "static IfCondition GetOppositeCondition"),
        ("// TODO(solanes): This optimization should be in ConstantFolding", "static bool IsTypeConversionLossless("),
        ("// Recognize the following pattern:\n// obj.getClass()", "static HInstruction* CreateUnsignedConditionReplacement("),
        ("  if (condition->IsEqual() || condition->IsNotEqual()) {\n    if (RecognizeAndSimplifyClassCheck",
         "  // Reverse condition if left is constant."),
        ("void InstructionSimplifierVisitor::SimplifyBoxUnbox(", "void InstructionSimplifierVisitor::VisitDeoptimize("),
    ):
        source = replace_range(source, first, after)
    return source


def intrinsics_header(source):
    source = without_native_codegen(source)
    source = replace(source, "class DexFile;", "class CompilerOptions;\nclass DexFile;")
    return replace_range(source, "  static void MoveArguments(",
                         "  static void ComputeValueOfLocations(")


def nodes(source):
    source = without_native_codegen(source)
    source = replace(source, '#include "intrinsics.h"',
                     '#include "upstream/optimizing/intrinsics.h"\n#include "standalone_runtime.h"')
    for name, value in (("IntMin", "INT32_MIN"), ("IntMax", "INT32_MAX"),
                        ("LongMin", "INT64_MIN"), ("LongMax", "INT64_MAX")):
        source = source.replace(f"kPrim{name}", value)
    source = replace(source, "HGraphVisualizer::DumpInstruction(&os, graph, this);",
                     "wa::hgraph::PrintInstruction(os, *this);")
    source = replace(source, "HGraphVisualizer::DumpInstruction(&os, graph, instruction);",
                     "wa::hgraph::PrintInstruction(os, *instruction);")
    source = replace_body(source, "bool HBoundType::InstructionDataEquals(",
                          '  wa::hgraph::RequiresArtRuntime("bound reference types");')
    source = replace_body(source, "static void CheckAgainstUpperBound(",
                          '  wa::hgraph::RequiresArtRuntime("reference type bounds");')
    source = replace_range(source, "  ScopedObjectAccess soa(Thread::Current());",
                           "\n}\n\nstd::ostream& operator<<(", """\
  if (caller != nullptr) {
    wa::hgraph::RequiresArtRuntime("resolved method dex cache");
  }
  return GetBlock()->GetGraph()->GetDexFile();""")
    for type_name in ("Class", "String"):
        source = replace(source, "      ScopedObjectAccess soa(Thread::Current());\n"
                         f"      return Get{type_name}().Get() == "
                         f"other_load_{type_name.lower()}->Get{type_name}().Get();",
                         f'      wa::hgraph::RequiresArtRuntime("resolved {type_name.lower()}");')
    return replace_range(source, "  mirror::VarHandle::AccessModeTemplate access_mode_template =",
                         "\n}\n\nbool HInvokeVirtual::CanDoImplicitNullCheckOn(",
                         '  wa::hgraph::RequiresArtRuntime("VarHandle invoke");')


def graph(source):
    source = replace(source, '#include "graph_visualizer.h"', '#include "standalone_runtime.h"')
    # kNone is the standalone ISA. A current-method node is invoke metadata only.
    source = replace(source, "Is64BitInstructionSet(instruction_set_) ?",
                     "(instruction_set_ != InstructionSet::kNone &&\n"
                     "         Is64BitInstructionSet(instruction_set_)) ?")
    source = replace(source, "    cached_null_constant_->SetReferenceTypeInfo(GetInexactObjectRti());\n", "")
    source = replace(source, """\
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(cached_null_constant_->GetReferenceTypeInfo().IsValid());
  }
""", "")
    return replace_body(source, "std::ostream& HGraph::Dump(", """\
  CHECK(codegen == nullptr);
  for (HBasicBlock* block : GetReversePostOrder()) {
    os << "Block " << block->GetBlockId() << " at dex pc " << block->GetDexPc() << '\\n';
    for (HInstruction* phi = block->GetFirstPhi(); phi != nullptr; phi = phi->GetNext()) {
      wa::hgraph::PrintInstruction(os, *phi) << '\\n';
    }
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;
         instruction = instruction->GetNext()) {
      wa::hgraph::PrintInstruction(os, *instruction) << '\\n';
    }
  }
  return os;""")


def ssa_builder(source):
    for header in ("mirror/class-inl.h", "reference_type_propagation.h",
                   "scoped_thread_state_change-inl.h"):
        source = replace(source, f'#include "{header}"\n', "")
    source = replace(source, '#include "nodes.h"\n',
                     '#include "nodes.h"\n#include "standalone_runtime.h"\n')
    source = replace_range(source, "static DataType::Type GetPrimitiveArrayComponentType(",
                           "bool SsaBuilder::FixAmbiguousArrayOps()")
    source = replace(source, "  {\n    ScopedObjectAccess soa(Thread::Current());\n",
                     "  {\n")
    source = replace(source, """\
      ScopedObjectAccess soa(Thread::Current());
      DCHECK(DataType::IsIntOrLongType(GetPrimitiveArrayComponentType(aget->GetArray())));
""", """\
      DCHECK(DataType::IsIntOrLongType(
          wa::hgraph::ArrayComponentType(aget->GetArray(), aget, aget->GetType())));
""")
    source = replace(source, "      HArrayGet* aget_float = FindFloatOrDoubleEquivalentOfArrayGet(aget_int);\n",
                     "")
    for op in ("aget_int", "aset"):
        if op == "aget_int":
            hint = """\
      HArrayGet* aget_float = FindFloatOrDoubleEquivalentOfArrayGet(aget_int);
      const DataType::Type hint = (aget_float == nullptr) ? aget_int->GetType() : aget_float->GetType();
"""
        else:
            hint = "      const DataType::Type hint = DataType::Kind(aset->GetValue()->GetType());\n"
        source = replace(source, f"""\
      HInstruction* array = {op}->GetArray();
      if (!array->GetReferenceTypeInfo().IsValid() ||
          !array->GetReferenceTypeInfo().IsPrimitiveArrayClass()) {{
""", hint + f"""\
      DataType::Type array_type = wa::hgraph::ArrayComponentType({op}->GetArray(), {op}, hint);
      if (array_type == DataType::Type::kVoid) {{
""")
    source = replace(source, "      DataType::Type array_type = GetPrimitiveArrayComponentType(array);\n",
                     "", 2)
    return replace_range(source, "  // Compute type of reference type instructions.",
                         "  // HInstructionBuilder duplicated", """\
  // Managed reference handles are unavailable. FixAmbiguousArrayOps resolves
  // primitive array descriptors directly from DEX-backed graph definitions.

""")


def constant_folding(source):
    for header in ("art_field-inl.h", "base/locks.h", "base/sdk_version.h", "class_root-inl.h",
                   "driver/compiler_options.h", "mirror/method_type.h", "mirror/object.h",
                   "obj_ptr.h", "handle_cache-inl.h", "runtime.h"):
        source = replace(source, f'#include "{header}"\n', "")
    source = replace_range(source, "static bool IsUnmodifiableAndInitialized(",
                           "void HConstantFoldingVisitor::VisitLoadConstantTableEntry(")
    source = replace_range(source, "void HConstantFoldingVisitor::FoldFieldValue(",
                           "void InstructionWithAbsorbingInputSimplifier::HandleShift(")
    source = replace_range(source, "static ObjPtr<mirror::Object> ExtractConstant(",
                           "void InstructionWithAbsorbingInputSimplifier::HandleEquality(")
    start = source.index("  } else {\n    ScopedObjectAccess", source.index(
        "void InstructionWithAbsorbingInputSimplifier::HandleEquality("))
    end = source.index("\n}", start)
    source = source[:start] + "  }" + source[end:]
    for line in ("  void VisitStaticFieldGet(HStaticFieldGet* inst);",
                 "  void VisitInstanceFieldGet(HInstanceFieldGet* inst);",
                 "  void FoldFieldValue(HFieldAccess* inst, ObjPtr<mirror::Object> receiver)\n"
                 "      REQUIRES_SHARED(Locks::mutator_lock_);"):
        source = replace(source, line + "\n", "")
    return source


def diagnostic_operator(source, operand_type, headers):
    # Retain the diagnostic overload without the native code generator.
    start = source.index(f"std::ostream& operator<<(std::ostream& os, const {operand_type}&")
    end = source.index("\n}", start) + 2
    includes = "".join(f'#include "{header}"\n' for header in headers)
    return (source[:source.index("#include")] + "#include <ostream>\n" + includes +
            "\nnamespace art HIDDEN {\n\n" + source[start:end] + "\n\n}  // namespace art\n")


def reference_type_info(source):
    source = diagnostic_operator(source, "ReferenceTypeInfo", ("reference_type_info.h",))
    source = replace(source, "  ScopedObjectAccess soa(Thread::Current());\n", "")
    return replace(source,
                     '     << " type=" << (!rhs.IsValid() ? "?" : mirror::Class::PrettyClass(rhs.GetTypeHandle().Get()))\n',
                     "")


def control_flow_simplifier(source):
    source = replace(source, '#include "reference_type_propagation.h"',
                     '#include "standalone_runtime.h"')
    return replace(source,
                   "ReferenceTypePropagation::FixUpSelectType(select, graph_->GetHandleCache());",
                   'wa::hgraph::RequiresArtRuntime("reference select");')


def main(art, output):
    transformations = {
        "driver/compiler_options.cc": compiler_options,
        "driver/dex_compilation_unit.cc": dex_compilation_unit,
        "optimizing/builder.cc": builder,
        "optimizing/constant_folding.cc": constant_folding,
        "optimizing/control_flow_simplifier.cc": control_flow_simplifier,
        "optimizing/graph.cc": graph,
        "optimizing/graph_checker.cc": graph_checker,
        "optimizing/instruction_builder.cc": instruction_builder,
        "optimizing/instruction_simplifier.cc": instruction_simplifier,
        "optimizing/intrinsics.cc": lambda source: diagnostic_operator(
            source, "Intrinsics", ("intrinsics_enum.h", "intrinsics_list.h")),
        "optimizing/locations.cc": lambda source: diagnostic_operator(
            source, "Location", ("locations.h",)),
        "optimizing/nodes.cc": nodes,
        "optimizing/reference_type_info.cc": reference_type_info,
        "optimizing/intrinsics.h": intrinsics_header,
        "optimizing/ssa_builder.cc": ssa_builder,
    }
    for name, transform in transformations.items():
        original = (Path(art) / "compiler" / name).read_text(encoding="utf-8")
        try:
            updated = transform(original)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from error
        path = Path(output) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding="utf-8") != updated:
            path.write_text(updated, encoding="utf-8")


if __name__ == "__main__":
    main(*sys.argv[1:])
