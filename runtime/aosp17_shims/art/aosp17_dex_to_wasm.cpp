#include "aosp17_dex_to_wasm.h"

#include "base/mem_map.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/signature-inl.h"
#include "dex/utf-inl.h"
#include "dex_internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace wa::aosp17 {
namespace {

using namespace wa::art;

void init_mem_map() {
    static std::once_flag init;
    std::call_once(init, &::art::MemMap::Init);
}

std::vector<uint16_t> type_list(const ::art::dex::TypeList* list) {
    std::vector<uint16_t> result;
    if (list != nullptr) {
        result.reserve(list->Size());
        for (uint32_t i = 0; i < list->Size(); ++i)
            result.push_back(list->GetTypeItem(i).type_idx_.index_);
    }
    return result;
}

uint32_t type_index(::art::dex::TypeIndex index) {
    return index.IsValid() ? index.index_ : kNone;
}

Code project_code(const ::art::CodeItemDataAccessor& accessor) {
    Code code;
    if (!accessor.HasCodeItem())
        return code;
    code.registers = accessor.RegistersSize();
    code.ins = accessor.InsSize();
    code.outs = accessor.OutsSize();
    code.words.assign(accessor.Insns(), accessor.Insns() + accessor.InsnsSizeInCodeUnits());
    for (const auto& item : accessor.TryItems()) {
        Try entry{item.start_addr_, item.start_addr_ + item.insn_count_, {}};
        for (::art::CatchHandlerIterator handler(accessor, item); handler.HasNext(); handler.Next())
            entry.handlers.push_back({type_index(handler.GetHandlerTypeIndex()),
                                      handler.GetHandlerAddress()});
        code.tries.push_back(std::move(entry));
    }
    return code;
}

void project_initial_value(Field& field, const ::art::EncodedStaticFieldValueIterator& values) {
    using Value = ::art::EncodedArrayValueIterator;
    const auto& value = values.GetJavaValue();
    switch (values.GetValueType()) {
    case Value::kNull: break;
    case Value::kString: field.initial_string = static_cast<uint32_t>(value.i); break;
    case Value::kLong:
    case Value::kDouble: field.initial_value = static_cast<uint64_t>(value.j); break;
    case Value::kFloat: field.initial_value = static_cast<uint32_t>(value.i); break;
    case Value::kBoolean:
    case Value::kByte:
    case Value::kShort:
    case Value::kChar:
    case Value::kInt: field.initial_value = static_cast<uint64_t>(value.i); break;
    default: throw std::runtime_error("Unsupported static initializer in CodeGeneratorWasm");
    }
}

std::unique_ptr<wa_art_dex> project(const ::art::DexFile& file) {
    auto dex = std::make_unique<wa_art_dex>();
    dex->signature = file.GetSha1();
    dex->strings.reserve(file.NumStringIds());
    for (uint32_t i = 0; i < file.NumStringIds(); ++i)
        dex->strings.emplace_back(file.GetStringView(::art::dex::StringIndex(i)));
    const auto string = [&](::art::dex::StringIndex index) -> std::string_view {
        return dex->strings[index.index_];
    };

    dex->types.reserve(file.NumTypeIds());
    for (uint32_t i = 0; i < file.NumTypeIds(); ++i)
        dex->types.push_back(string(file.GetTypeId(::art::dex::TypeIndex(i)).descriptor_idx_));
    dex->protos.reserve(file.NumProtoIds());
    for (uint32_t i = 0; i < file.NumProtoIds(); ++i) {
        const auto& id = file.GetProtoId(::art::dex::ProtoIndex(i));
        dex->protos.push_back({id.return_type_idx_.index_, type_list(file.GetProtoParameters(id)),
                               file.GetProtoSignature(id).ToString()});
    }
    dex->fields.reserve(file.NumFieldIds());
    for (uint32_t i = 0; i < file.NumFieldIds(); ++i) {
        const auto& id = file.GetFieldId(i);
        dex->fields.push_back({id.class_idx_.index_, id.type_idx_.index_, string(id.name_idx_)});
    }
    dex->methods.reserve(file.NumMethodIds());
    for (uint32_t i = 0; i < file.NumMethodIds(); ++i) {
        const auto& id = file.GetMethodId(i);
        dex->methods.push_back({id.class_idx_.index_, id.proto_idx_.index_, string(id.name_idx_),
                                0, false, false, {}});
    }

    dex->classes.reserve(file.NumClassDefs());
    for (const ::art::ClassAccessor clazz : file.GetClasses()) {
        const auto& def = clazz.GetClassDef();
        Class entry{def.class_idx_.index_, type_index(def.superclass_idx_),
                    type_list(file.GetInterfacesList(def)), {}, {}};
        ::art::EncodedStaticFieldValueIterator values(file, def);
        for (const auto& member : clazz.GetFields()) {
            const auto index = member.GetIndex();
            auto& field = dex->fields[index];
            field.access = member.GetAccessFlags();
            field.declared = true;
            if (member.IsStatic() && values.HasNext()) {
                project_initial_value(field, values);
                if (!values.MaybeNext())
                    throw std::runtime_error("Unsupported static initializer in CodeGeneratorWasm");
            }
            entry.fields.push_back(index);
        }
        for (const auto& member : clazz.GetMethods()) {
            const auto index = member.GetIndex();
            auto& method = dex->methods[index];
            method.access = member.GetAccessFlags();
            method.declared = true;
            method.is_virtual = !member.IsStaticOrDirect();
            method.code = project_code(member.GetInstructionsAndData());
            entry.methods.push_back(index);
        }
        dex->classes.push_back(std::move(entry));
    }
    return dex;
}

std::unique_ptr<wa_art_dex> open_for_backend(std::span<const uint8_t> input, uint32_t offset) {
    if (offset == 0) {
        const auto files = OpenDexFiles(input, "<memory>");
        return project(*files.front());
    }
    if (offset >= input.size() || offset % alignof(::art::DexFile::Header) != 0)
        throw std::invalid_argument("Invalid DEX container header offset");

    std::string error;
    ::art::ArtDexFileLoader loader(std::vector<uint8_t>(input.begin(), input.end()), "<memory>");
    auto file = loader.OpenOne(offset, /*location_checksum=*/0, /*oat_dex_file=*/nullptr,
                               /*verify=*/true, /*verify_checksum=*/true, &error);
    if (file == nullptr)
        throw std::runtime_error(error);
    return project(*file);
}

} // namespace

std::vector<std::unique_ptr<const ::art::DexFile>> OpenDexFiles(
    std::span<const uint8_t> input, const std::string& location) {
    if (input.empty())
        throw std::invalid_argument("Missing DEX or APK bytes");
    init_mem_map();
    ::art::ArtDexFileLoader loader(std::vector<uint8_t>(input.begin(), input.end()),
                                  location.empty() ? "<memory>" : location);
    std::vector<std::unique_ptr<const ::art::DexFile>> files;
    std::string error;
    // This overload dispatches ZIP inputs and calls art::dex::Verify on each logical DEX.
    if (!loader.Open(/*verify=*/true, /*verify_checksum=*/true, &error, &files))
        throw std::runtime_error(error);
    return files;
}

std::vector<uint8_t> CompileDexToWasm(std::span<const uint8_t> input, const char* profile) {
    return wa::art::generate_wasm(*open_for_backend(input, 0), profile);
}

} // namespace wa::aosp17

namespace wa::art {

void copy_error(char* error, size_t capacity, std::string_view text) {
    if (error == nullptr || capacity == 0)
        return;
    const auto length = std::min(text.size(), capacity - 1);
    std::memcpy(error, text.data(), length);
    error[length] = 0;
}

std::string utf8_name(std::string_view name) {
    if (name.empty())
        return {};
    const auto length = ::art::CountModifiedUtf8Chars(name.data(), name.size());
    std::vector<uint16_t> utf16(length);
    ::art::ConvertModifiedUtf8ToUtf16(utf16.data(), length, name.data(), name.size());
    std::string result;
    result.reserve(name.size());
    ::art::ConvertUtf16ToUtf8<true, true, true>(
        utf16.data(), utf16.size(), [&](char byte) { result.push_back(byte); });
    return result;
}

Kind kind(std::string_view descriptor) {
    switch (descriptor.front()) {
    case 'V': return Kind::Unset;
    case 'J':
    case 'D': return Kind::Wide;
    case 'L':
    case '[': return Kind::Reference;
    default: return Kind::Scalar;
    }
}

wa_art_element_kind_t element_kind(std::string_view descriptor) {
    switch (descriptor.front()) {
    case 'Z': return WA_ART_BOOLEAN;
    case 'B': return WA_ART_BYTE;
    case 'C': return WA_ART_CHAR;
    case 'S': return WA_ART_SHORT;
    case 'I': return WA_ART_INT;
    case 'J': return WA_ART_LONG;
    case 'F': return WA_ART_FLOAT;
    case 'D': return WA_ART_DOUBLE;
    default: return WA_ART_REFERENCE;
    }
}

} // namespace wa::art

std::string wa_art_dex::method_name(uint32_t id) const {
    const auto& method = methods[id];
    return wa::art::utf8_name(std::string(types[method.owner]) + "->" + std::string(method.name) +
                              protos[method.proto].signature);
}

std::string wa_art_dex::field_name(uint32_t id) const {
    const auto& field = fields[id];
    return wa::art::utf8_name(std::string(types[field.owner]) + "->" + std::string(field.name) +
                              ":" + std::string(types[field.type]));
}

uint32_t wa_art_dex::find_type(std::string_view descriptor) const {
    const auto found = std::find(types.begin(), types.end(), descriptor);
    return found == types.end() ? WA_ART_NO_INDEX : static_cast<uint32_t>(found - types.begin());
}

uint32_t wa_art_dex::find_method(uint32_t owner, std::string_view name,
                                 std::string_view signature) const {
    for (uint32_t i = 0; i < methods.size(); ++i) {
        const auto& method = methods[i];
        if (method.owner == owner && method.name == name && protos[method.proto].signature == signature)
            return i;
    }
    return WA_ART_NO_INDEX;
}

wa_art_dex_t* wa_art_dex_open(const uint8_t* data, size_t size, uint32_t header_offset,
                              char* error, size_t error_size) {
    try {
        if (data == nullptr)
            throw std::invalid_argument("Missing DEX or APK bytes");
        auto dex = wa::aosp17::open_for_backend({data, size}, header_offset);
        wa::art::copy_error(error, error_size, "");
        return dex.release();
    } catch (const std::exception& exception) {
        wa::art::copy_error(error, error_size, exception.what());
        return nullptr;
    }
}

void wa_art_dex_close(wa_art_dex_t* dex) {
    delete dex;
}

uint32_t wa_art_dex_find_type(const wa_art_dex_t* dex, const char* descriptor) {
    return dex && descriptor ? dex->find_type(descriptor) : WA_ART_NO_INDEX;
}

uint32_t wa_art_dex_find_field(const wa_art_dex_t* dex, const char* descriptor) {
    if (dex && descriptor) {
        try {
            for (uint32_t i = 0; i < dex->fields.size(); ++i) {
                if (dex->field_name(i) == descriptor)
                    return i;
            }
        } catch (const std::exception&) {
            return WA_ART_NO_INDEX;
        }
    }
    return WA_ART_NO_INDEX;
}

uint32_t wa_art_dex_find_method(const wa_art_dex_t* dex, const char* descriptor) {
    if (dex && descriptor) {
        try {
            for (uint32_t i = 0; i < dex->methods.size(); ++i) {
                if (dex->method_name(i) == descriptor)
                    return i;
            }
        } catch (const std::exception&) {
            return WA_ART_NO_INDEX;
        }
    }
    return WA_ART_NO_INDEX;
}
