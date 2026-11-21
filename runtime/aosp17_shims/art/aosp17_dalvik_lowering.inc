// Included by the build-local dalvik_ir.cpp inside its anonymous namespace.
using DexInstruction = ::art::Instruction;

Kind field_variant(unsigned variant) {
    if (variant == 1)
        return Kind::Wide;
    if (variant == 2)
        return Kind::Reference;
    return Kind::Scalar;
}

Kind move_variant(unsigned variant) {
    return variant == 0 ? Kind::Scalar : field_variant(variant);
}

struct Decoder {
    const wa_art_dex_t& dex;
    const Method& method;
    uint32_t pc;

    uint32_t branch(const DexInstruction& decoded) const {
        const int64_t target = int64_t(pc) + decoded.GetTargetOffset();
        if (target < 0 || static_cast<uint64_t>(target) >= method.code.words.size())
            reject(pc, "branch target outside code item");
        return static_cast<uint32_t>(target);
    }

    void reg(uint32_t value, Kind kind) const {
        if (value + (kind == Kind::Wide) >= method.code.registers)
            reject(pc, "register outside code item");
    }

    void result(Instruction& ins, Kind kind) const {
        reg(ins.a, kind);
        ins.output = kind;
    }

    void read(Instruction& ins, uint32_t source, Kind kind) const {
        reg(source, kind);
        ins.reads.push_back({static_cast<uint16_t>(source), kind});
    }

    void check_id(uint32_t id, size_t size, std::string_view message) const {
        if (id >= size)
            reject(pc, message);
    }

    void type(Instruction& ins, uint32_t id, char expected) const {
        ins.id = id;
        check_id(id, dex.types.size(), "invalid type index");
        if (expected != 0 && dex.types[id].front() != expected)
            reject(pc, "unexpected type descriptor");
    }

    void field(Instruction& ins, uint32_t id, unsigned variant, bool statik) const {
        ins.id = id;
        check_id(id, dex.fields.size(), "invalid field index");
        const auto& field = dex.fields[id];
        const auto desc = dex.types[field.type];
        constexpr std::array<char, 7> kinds = {0, 0, 0, 'Z', 'B', 'C', 'S'};
        if (kind(desc) != field_variant(variant) ||
            (kinds[variant] != 0 && desc.front() != kinds[variant]) ||
            (variant == 0 && desc != "I" && desc != "F") ||
            (field.declared && (((field.access & kStatic) != 0) != statik)))
            reject(pc, "field opcode does not match the field declaration");
    }

    void invoke(Instruction& ins, const DexInstruction& decoded) const {
        const auto opcode = decoded.Opcode();
        const auto type = ::art::InvokeInstructionType(opcode);
        const bool statik = type == ::art::kDexInvokeStatic;
        ins.op = Op::Invoke;
        ins.invoke = statik ? Invoke::Static :
            type == ::art::kDexInvokeDirect ? Invoke::Direct : Invoke::Virtual;
        ins.may_throw = true;
        ins.id = decoded.VRegB();
        check_id(ins.id, dex.methods.size(), "invalid invoke method index");
        const auto& callee = dex.methods[ins.id];
        if (callee.declared && statik != ((callee.access & kStatic) != 0))
            reject(pc, "invoke opcode does not match the method declaration");

        const bool range = ::art::IsInvokeInstructionRange(opcode);
        const uint32_t count = decoded.VRegA();
        const uint32_t first = range ? decoded.VRegC_3rc() : 0;
        if (count > method.code.outs || (!range && count > DexInstruction::kMaxVarArgRegs) ||
            (range && first + count > method.code.registers))
            reject(pc, "invalid invoke register count");
        uint32_t registers[DexInstruction::kMaxVarArgRegs] = {};
        if (!range)
            decoded.GetVarArgs(registers);
        const ::art::RangeInstructionOperands range_args(first, count);
        const ::art::VarArgsInstructionOperands explicit_args(registers, count);
        const ::art::InstructionOperands& args = range
            ? static_cast<const ::art::InstructionOperands&>(range_args)
            : explicit_args;
        uint32_t i = 0;
        for (auto kind : parameters(dex, callee, statik)) {
            if (i >= count)
                reject(pc, "too few invoke argument words");
            const auto low = args.GetOperand(i++);
            read(ins, low, kind);
            if (kind == Kind::Wide && (i >= count || args.GetOperand(i++) != low + 1))
                reject(pc, "invoke wide argument is not a register pair");
        }
        if (i != count)
            reject(pc, "too many invoke argument words");
    }

    void binary(Instruction& ins, const DexInstruction& decoded) const {
        const auto opcode = decoded.Opcode();
        const auto format = DexInstruction::FormatOf(opcode);
        int group = opcode;
        switch (format) {
        case DexInstruction::k12x:
            ins.b = ins.a;
            ins.c = decoded.VRegB_12x();
            group -= DexInstruction::ADD_INT_2ADDR - DexInstruction::ADD_INT;
            break;
        case DexInstruction::k22s:
            ins.literal = true;
            ins.b = decoded.VRegB_22s();
            ins.bits = static_cast<uint32_t>(decoded.VRegC_22s());
            group -= DexInstruction::ADD_INT_LIT16 - DexInstruction::ADD_INT;
            break;
        case DexInstruction::k22b:
            ins.literal = true;
            ins.b = decoded.VRegB_22b();
            ins.bits = static_cast<uint32_t>(decoded.VRegC_22b());
            group -= DexInstruction::ADD_INT_LIT8 - DexInstruction::ADD_INT;
            break;
        default:
            ins.b = decoded.VRegB_23x();
            ins.c = decoded.VRegC_23x();
            break;
        }
        ins.reverse = opcode == DexInstruction::RSUB_INT || opcode == DexInstruction::RSUB_INT_LIT8;
        if (group < DexInstruction::ADD_LONG) {
            ins.op = Op::IntBinary;
            ins.binary = static_cast<Binary>(group - DexInstruction::ADD_INT);
        } else if (group < DexInstruction::ADD_FLOAT) {
            ins.op = Op::LongBinary;
            ins.binary = static_cast<Binary>(group - DexInstruction::ADD_LONG);
        } else {
            ins.op = Op::FloatBinary;
            ins.binary = static_cast<Binary>(group - DexInstruction::ADD_FLOAT);
        }
        const auto kind = ins.op == Op::LongBinary ? Kind::Wide : Kind::Scalar;
        result(ins, kind);
        read(ins, ins.b, kind);
        if (!ins.literal)
            read(ins, ins.c, ins.binary >= Binary::Shl ? Kind::Scalar : kind);
        ins.may_throw = decoded.IsThrow();
    }

    Instruction decode() const {
        const auto* words = method.code.words.data();
        ::art::DexInstructionIterator position(words, pc);
        const auto& decoded = position.Inst();
        using D = DexInstruction;
        const auto opcode = decoded.Opcode();
        // The backend has no payload lowering. Reject these before asking the iterator
        // to advance, since a payload's length operand can span several code units.
        if (opcode == D::NOP && decoded.VRegA_10x() != 0)
            reject(pc, "unsupported instruction payload");
        const auto width = D::SizeInCodeUnits(D::FormatOf(opcode));
        if (width == 0 || width > method.code.words.size() - pc)
            reject(pc, "invalid format or truncated instruction");
        ++position;

        Instruction ins;
        ins.pc = pc;
        ins.width = position.DexPc() - pc;
        if (decoded.HasVRegA())
            ins.a = static_cast<uint16_t>(decoded.VRegA());
        if (opcode >= D::MOVE && opcode <= D::MOVE_OBJECT_16) {
            const auto kind = move_variant((opcode - D::MOVE) / 3);
            ins.op = Op::Move;
            ins.b = decoded.VRegB();
            result(ins, kind);
            read(ins, ins.b, kind);
        } else if (opcode >= D::MOVE_RESULT && opcode <= D::MOVE_RESULT_OBJECT) {
            ins.op = Op::MoveResult;
            result(ins, move_variant(opcode - D::MOVE_RESULT));
        } else if (::art::IsInstructionReturn(opcode)) {
            ins.op = Op::Return;
            const auto actual = opcode == D::RETURN_VOID ? Kind::Unset
                                                         : move_variant(opcode - D::RETURN);
            if (actual != kind(dex.types[dex.protos[method.proto].result]))
                reject(pc, "return opcode does not match the method prototype");
            if (actual != Kind::Unset)
                read(ins, ins.a, actual);
        } else if (::art::IsInstructionDirectConst(opcode)) {
            ins.op = Op::Constant;
            switch (opcode) {
            case D::CONST_WIDE: ins.bits = decoded.WideVRegB(); break;
            case D::CONST_HIGH16: ins.bits = uint32_t(decoded.VRegB_21h()) << 16; break;
            case D::CONST_WIDE_HIGH16: ins.bits = uint64_t(decoded.VRegB_21h()) << 48; break;
            default: ins.bits = static_cast<uint64_t>(decoded.VRegB()); break;
            }
            const auto kind = ::art::IsInstructionConstWide(opcode) ? Kind::Wide :
                              ins.bits != 0 ? Kind::Scalar : Kind::Zero;
            result(ins, kind);
        } else if (::art::IsInstructionGoto(opcode)) {
            ins.op = Op::Goto;
            ins.target = branch(decoded);
        } else if (::art::IsInstructionIfCc(opcode) || ::art::IsInstructionIfCcZ(opcode)) {
            ins.op = Op::Branch;
            ins.literal = ::art::IsInstructionIfCcZ(opcode);
            ins.id = ins.literal ? opcode - D::IF_EQZ : opcode - D::IF_EQ;
            ins.b = ins.a;
            const auto comparable = ins.id < 2 ? Kind::Unset : Kind::Scalar;
            read(ins, ins.b, comparable);
            if (!ins.literal) {
                ins.c = decoded.VRegB_22t();
                read(ins, ins.c, comparable);
            }
            ins.target = branch(decoded);
        } else if (::art::IsInstructionAGetOrAPut(opcode)) {
            const bool put = ::art::IsInstructionAPut(opcode);
            const auto kind = field_variant(::art::AGetOrAPutMemAccessType(opcode));
            ins.op = put ? Op::ArrayPut : Op::ArrayGet;
            ins.b = decoded.VRegB_23x();
            ins.c = decoded.VRegC_23x();
            ins.may_throw = true;
            read(ins, ins.b, Kind::Reference);
            read(ins, ins.c, Kind::Scalar);
            if (put)
                read(ins, ins.a, kind);
            else
                result(ins, kind);
        } else if (::art::IsInstructionIGetOrIPut(opcode) || ::art::IsInstructionSGetOrSPut(opcode)) {
            const bool statik = ::art::IsInstructionSGetOrSPut(opcode);
            const auto variant = statik ? ::art::SGetOrSPutMemAccessType(opcode)
                                        : ::art::IGetOrIPutMemAccessType(opcode);
            const auto kind = field_variant(variant);
            const bool put = ::art::IsInstructionSPut(opcode) || ::art::IsInstructionIPut(opcode);
            if (statik) {
                ins.op = put ? Op::PutStatic : Op::GetStatic;
                field(ins, decoded.VRegB_21c(), variant, true);
            } else {
                ins.op = put ? Op::PutField : Op::GetField;
                ins.b = decoded.VRegB_22c();
                read(ins, ins.b, Kind::Reference);
                field(ins, decoded.VRegC_22c(), variant, false);
            }
            ins.may_throw = true;
            if (put)
                read(ins, ins.a, kind);
            else
                result(ins, kind);
        } else if ((opcode >= D::ADD_INT && opcode <= D::DIV_FLOAT) ||
                   (opcode >= D::ADD_INT_2ADDR && opcode <= D::DIV_FLOAT_2ADDR) ||
                   (opcode >= D::ADD_INT_LIT16 && opcode <= D::USHR_INT_LIT8)) {
            binary(ins, decoded);
        } else {
            switch (opcode) {
            case D::NOP: break;
            case D::INVOKE_VIRTUAL:
            case D::INVOKE_DIRECT:
            case D::INVOKE_STATIC:
            case D::INVOKE_VIRTUAL_RANGE:
            case D::INVOKE_DIRECT_RANGE:
            case D::INVOKE_STATIC_RANGE:
                invoke(ins, decoded);
                break;
            case D::MOVE_EXCEPTION:
                ins.op = Op::MoveException;
                result(ins, Kind::Reference);
                break;
            case D::CONST_STRING:
            case D::CONST_STRING_JUMBO:
                ins.op = Op::String;
                ins.id = decoded.VRegB();
                check_id(ins.id, dex.strings.size(), "invalid const-string index");
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case D::CHECK_CAST:
                ins.op = Op::CheckCast;
                type(ins, decoded.VRegB_21c(), 0);
                if (kind(dex.types[ins.id]) != Kind::Reference)
                    reject(pc, "check-cast expects a reference descriptor");
                read(ins, ins.a, Kind::Reference);
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case D::INSTANCE_OF:
                ins.op = Op::InstanceOf;
                ins.b = decoded.VRegB_22c();
                type(ins, decoded.VRegC_22c(), 0);
                if (kind(dex.types[ins.id]) != Kind::Reference)
                    reject(pc, "instance-of expects a reference descriptor");
                read(ins, ins.b, Kind::Reference);
                result(ins, Kind::Scalar);
                break;
            case D::ARRAY_LENGTH:
                ins.op = Op::ArrayLength;
                ins.b = decoded.VRegB_12x();
                result(ins, Kind::Scalar);
                read(ins, ins.b, Kind::Reference);
                ins.may_throw = true;
                break;
            case D::NEW_INSTANCE:
                ins.op = Op::NewObject;
                type(ins, decoded.VRegB_21c(), 'L');
                result(ins, Kind::Reference);
                ins.may_throw = true;
                break;
            case D::NEW_ARRAY:
                ins.op = Op::NewArray;
                ins.b = decoded.VRegB_22c();
                type(ins, decoded.VRegC_22c(), '[');
                result(ins, Kind::Reference);
                read(ins, ins.b, Kind::Scalar);
                ins.may_throw = true;
                break;
            case D::THROW:
                ins.op = Op::Throw;
                read(ins, ins.a, Kind::Reference);
                ins.may_throw = true;
                break;
            case D::NEG_INT:
            case D::INT_TO_LONG:
            case D::LONG_TO_INT:
            case D::FLOAT_TO_INT:
                ins.op = opcode == D::NEG_INT ? Op::NegInt :
                         opcode == D::INT_TO_LONG ? Op::IntToLong :
                         opcode == D::LONG_TO_INT ? Op::LongToInt : Op::FloatToInt;
                ins.b = decoded.VRegB_12x();
                result(ins, opcode == D::INT_TO_LONG ? Kind::Wide : Kind::Scalar);
                read(ins, ins.b, opcode == D::LONG_TO_INT ? Kind::Wide : Kind::Scalar);
                break;
            default:
                reject(pc, std::string("unsupported opcode ") + decoded.Name());
            }
        }
        return ins;
    }
};
