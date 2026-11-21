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
      graph_->GetInvokeType() != kStatic || graph_->GetArtMethod() != nullptr ||
      !wa::hgraph::IsStandaloneMethod(*dex_compilation_unit_) ||
      graph_->IsDebuggable() || graph_->IsCompilingOsr() || graph_->IsCompilingBaseline() ||
      graph_->GetProfilingInfo() != nullptr || !code_item_accessor_.HasCodeItem() ||
      code_item_accessor_.TriesSize() != 0) {
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
    source = replace_range(source, "// Does the method being compiled need any constructor barriers",
                           "template <DataType::Type kType>\nALWAYS_INLINE inline void HInstructionBuilder::BuildMove")
    source = replace_range(source, "    // Only <init>", "    if (graph_->IsDebuggable()")
    source = replace(source, "    DCHECK(!RequiresConstructorBarrier(dex_compilation_unit_));\n", "")

    # Substitute method resolution only. Keep ART's argument setup, environments
    # and move-result handling for the same HInvokeStaticOrDirect instruction.
    source = replace_range(source, "static InvokeType GetInvokeTypeFromOpCode",
                           "bool HInstructionBuilder::SetupInvokeArguments",
                           '#include "static_invoke.inc"\n\n')
    source = replace_range(source, "  if (invoke->IsInvokePolymorphic()) {",
                           "  return true;\n}", "")
    source = replace_range(source, "  if (invoke->IsInvokePolymorphic()) {",
                           "  return true;\n}", "")
    source = replace_range(source, "bool HInstructionBuilder::BuildSimpleIntrinsic",
                           "void HInstructionBuilder::BuildCheckedDivRem")
    source = replace_range(source, "void HInstructionBuilder::BuildArrayAccess",
                           "bool HInstructionBuilder::ProcessDexInstruction")
    source = replace_range(source, "ObjPtr<mirror::Class> HInstructionBuilder::LookupResolvedType",
                           "}  // namespace art")

    # Retain upstream decoding of all primitive instructions. Fail through the
    # existing diagnostic path for bytecodes that depend on the runtime.
    for first, after in (("MOVE_OBJECT:", "RETURN_VOID:"), ("RETURN_OBJECT:", "RETURN_WIDE:"),
                         ("INVOKE_POLYMORPHIC:", "NEG_INT:"), ("NEW_INSTANCE:", "MOVE_RESULT:"),
                         ("IGET:", "UNUSED_3E")):
        source = replace_range(source, f"    case Instruction::{first}", f"    case Instruction::{after}")
    for suffix in ("", "_RANGE"):
        for kind in ("DIRECT", "INTERFACE", "SUPER"):
            source = replace(source, f"    case Instruction::INVOKE_{kind}{suffix}:\n", "")
        source = replace(source,
                         f"    case Instruction::INVOKE_STATIC{suffix}:\n"
                         f"    case Instruction::INVOKE_VIRTUAL{suffix}: {{",
                         f"    case Instruction::INVOKE_STATIC{suffix}: {{")
    source = replace(source,
                     "    case Instruction::MOVE_RESULT_WIDE:\n    case Instruction::MOVE_RESULT_OBJECT: {",
                     "    case Instruction::MOVE_RESULT_WIDE: {")
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
    source = replace_body(source, "HNullConstant* HGraph::GetNullConstant()",
                          '  wa::hgraph::RequiresArtRuntime("reference constants");')
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
    source = replace_range(source, "static HArrayGet* FindFloatOrDoubleEquivalentOfArrayGet(",
                           "bool SsaBuilder::HasAliasInEnvironments(")
    source = replace_range(source, "HArrayGet* SsaBuilder::GetFloatOrDoubleEquivalentOfArrayGet(",
                           "HInstruction* SsaBuilder::GetFloatOrDoubleEquivalent(")
    source = replace(source,
                     "  if (value->IsArrayGet()) {\n"
                     "    return GetFloatOrDoubleEquivalentOfArrayGet(value->AsArrayGet());\n"
                     "  } else if (value->IsLongConstant()) {",
                     "  CHECK(!value->IsArrayGet());\n  if (value->IsLongConstant()) {")
    return replace_range(source, "  // Compute type of reference type instructions.",
                         "  // Mark dead phis.", """\
  // The standalone builder admits only primitive values and has no class metadata.
  CHECK(ambiguous_agets_.empty() && ambiguous_asets_.empty());

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
