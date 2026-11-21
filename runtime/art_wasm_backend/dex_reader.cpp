#include "dex_internal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace wa::art {
namespace {

[[noreturn]] void invalid(std::string_view text) {
    throw std::runtime_error(std::string(text));
}

struct Bytes {
    std::span<const uint8_t> data;

    void check(size_t at, size_t size) const {
        if (at > data.size() || size > data.size() - at)
            invalid("DEX offset or item size is out of bounds");
    }

    void table(size_t at, size_t count, size_t width) const {
        check(at, 0);
        if (count > (data.size() - at) / width)
            invalid("DEX table is out of bounds");
    }

    uint8_t u8(size_t at) const {
        check(at, 1);
        return data[at];
    }

    uint16_t u16(size_t at) const {
        check(at, 2);
        return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
    }

    uint32_t u32(size_t at) const {
        check(at, 4);
        return u16(at) | (static_cast<uint32_t>(u16(at + 2)) << 16);
    }
};

struct Cursor {
    Bytes bytes;
    size_t at;

    uint8_t byte() {
        const auto value = bytes.u8(at);
        ++at;
        return value;
    }

    uint32_t uleb() {
        uint32_t result = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            const auto value = byte();
            if (shift == 28 && value > 0x0f)
                invalid("DEX ULEB128 exceeds 32 bits");
            result |= uint32_t(value & 0x7f) << shift;
            if ((value & 0x80) == 0)
                return result;
        }
        invalid("Unterminated DEX ULEB128");
    }

    int32_t sleb() {
        uint32_t result = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            const auto value = byte();
            if (shift == 28 && value > 0x07 && (value < 0x78 || value > 0x7f))
                invalid("DEX SLEB128 exceeds 32 bits");
            result |= uint32_t(value & 0x7f) << shift;
            if ((value & 0x80) == 0) {
                if (shift < 28 && (value & 0x40))
                    result |= UINT32_MAX << (shift + 7);
                return std::bit_cast<int32_t>(result);
            }
        }
        invalid("Unterminated DEX SLEB128");
    }
};

uint16_t mutf_unit(Cursor& cursor, uint8_t lead) {
    if (lead < 0x80)
        return lead;
    const auto tail = cursor.byte();
    if ((tail & 0xc0) != 0x80)
        invalid("Invalid DEX modified UTF-8 continuation");
    if ((lead & 0xe0) == 0xc0) {
        const auto value = ((lead & 0x1f) << 6) | (tail & 0x3f);
        if (value < 0x80 && value != 0)
            invalid("Overlong DEX modified UTF-8 code unit");
        return static_cast<uint16_t>(value);
    }
    if ((lead & 0xf0) != 0xe0)
        invalid("Invalid DEX modified UTF-8 leading byte");
    const auto last = cursor.byte();
    const auto value = ((lead & 0x0f) << 12) | ((tail & 0x3f) << 6) | (last & 0x3f);
    if ((last & 0xc0) != 0x80 || value < 0x800)
        invalid("Invalid DEX modified UTF-8 code unit");
    return static_cast<uint16_t>(value);
}

bool valid_descriptor(std::string_view descriptor) {
    const auto first = descriptor.find_first_not_of('[');
    if (first == std::string_view::npos || first > 255)
        return false;
    descriptor.remove_prefix(first);
    if (descriptor.front() == 'L')
        return descriptor.size() > 2 && descriptor.back() == ';' &&
               descriptor.find_first_of(".;[", 1) == descriptor.size() - 1;
    if (descriptor.size() != 1)
        return false;
    const auto primitives = first ? "ZBCSIJFD" : "VZBCSIJFD";
    return std::strchr(primitives, descriptor.front()) != nullptr;
}

uint32_t index(uint32_t value, size_t size, std::string_view name) {
    if (value >= size)
        invalid(name);
    return value;
}

struct Header {
    Bytes bytes;
    size_t base = 0;
    size_t header_end = 0;
    size_t data_begin = 0;
    size_t data_end = 0;

    Header(std::span<const uint8_t> input, uint32_t offset) : bytes{input}, base(offset) {
        if (base % 4 || input.size() > UINT32_MAX)
            invalid("Invalid DEX container size or header alignment");
        bytes.check(base, 112);
        const auto* magic = input.data() + base;
        if (std::memcmp(magic, "dex\n03", 6) && std::memcmp(magic, "dex\n04", 6))
            invalid("Invalid DEX magic");
        if (magic[6] < '0' || magic[6] > '9' || magic[7] != 0)
            invalid("Invalid DEX version");
        const unsigned version = (magic[5] - '0') * 10 + (magic[6] - '0');
        if (version < 35 || version > 41)
            invalid("Unsupported DEX version");
        const uint32_t header_size = version == 41 ? 120 : 112;
        const auto file_size = read(32);
        if (read(36) != header_size || read(40) != 0x12345678 || file_size < header_size)
            invalid("Invalid DEX header size, file size, or endian tag");
        bytes.check(base, file_size);
        header_end = base + header_size;
        if (version == 41) {
            if (read(112) != input.size() || read(116) != base)
                invalid("Invalid DEX v041 container header");
            data_begin = header_end;
            data_end = input.size();
        } else {
            if (base || file_size != input.size())
                invalid("Invalid standalone DEX file size");
            const auto length = read(104);
            const auto offset_data = read(108);
            if (length) {
                if (offset_data < header_end || offset_data % 4 || length % 4)
                    invalid("Invalid DEX data section");
                bytes.check(offset_data, length);
                data_begin = offset_data;
                data_end = data_begin + length;
            } else if (offset_data) {
                invalid("Empty DEX data section has a nonzero offset");
            }
        }
        const auto link_size = read(44);
        const auto link_at = read(48);
        if (link_size) {
            if (link_at < header_end)
                invalid("Invalid DEX link section");
            bytes.check(link_at, link_size);
        } else if (link_at) {
            invalid("Empty DEX link section has a nonzero offset");
        }
        if (const auto map_at = read(52)) {
            const auto map = data_at(map_at, 4);
            map.table(size_t(map_at) + 4, map.u32(map_at), 12);
        }
    }

    uint32_t read(size_t relative) const {
        return bytes.u32(base + relative);
    }

    std::pair<size_t, size_t> table(size_t relative, size_t width) const {
        const auto count = read(relative);
        const auto offset = read(relative + 4);
        if (count == 0 && offset == 0)
            return {};
        if (!count || offset < header_end || offset % 4)
            invalid("Invalid DEX identifier table location");
        bytes.table(offset, count, width);
        return {offset, count};
    }

    Bytes data_at(size_t offset, size_t alignment = 1) const {
        if (offset < data_begin || offset >= data_end || offset % alignment)
            invalid("Invalid DEX data item location");
        return {bytes.data.first(data_end)};
    }
};

std::string read_string(const Header& header, size_t offset) {
    Cursor cursor{header.data_at(offset), offset};
    const auto count = cursor.uleb();
    const auto begin = cursor.at;
    for (uint32_t i = 0; i < count; ++i) {
        const auto lead = cursor.byte();
        if (lead == 0)
            invalid("DEX string is shorter than its UTF-16 length");
        mutf_unit(cursor, lead);
    }
    if (cursor.byte() != 0)
        invalid("DEX string is missing its terminator");
    return {reinterpret_cast<const char*>(cursor.bytes.data.data() + begin), cursor.at - begin - 1};
}

std::vector<uint16_t> type_list(const Header& header, size_t offset, size_t limit) {
    if (!offset)
        return {};
    const auto bytes = header.data_at(offset, 4);
    const auto size = bytes.u32(offset);
    bytes.table(offset + 4, size, 2);
    std::vector<uint16_t> result;
    result.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        const auto id = bytes.u16(offset + 4 + size_t(i) * 2);
        index(id, limit, "Invalid DEX type list index");
        result.push_back(id);
    }
    return result;
}

Code read_code(const Header& header, size_t at, size_t type_count) {
    const auto bytes = header.data_at(at, 4);
    bytes.check(at, 16);
    Code code;
    code.registers = bytes.u16(at);
    code.ins = bytes.u16(at + 2);
    code.outs = bytes.u16(at + 4);
    const auto tries_size = bytes.u16(at + 6);
    const auto debug_at = bytes.u32(at + 8);
    if (debug_at)
        header.data_at(debug_at);
    const auto count = bytes.u32(at + 12);
    if (code.ins > code.registers || count == 0)
        invalid("Invalid DEX code item register or instruction count");
    at += 16;
    bytes.table(at, count, 2);
    code.words.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        code.words.push_back(bytes.u16(at + size_t(i) * 2));
    if (!tries_size)
        return code;
    at += size_t(count) * 2;
    if (count & 1) {
        if (bytes.u16(at) != 0)
            invalid("Nonzero DEX try item padding");
        at += 2;
    }
    bytes.table(at, tries_size, 8);
    const auto handlers_at = at + size_t(tries_size) * 8;
    Cursor cursor{bytes, handlers_at};
    const auto list_size = cursor.uleb();
    if (!list_size || list_size > bytes.data.size() - cursor.at)
        invalid("Invalid DEX catch handler list size");
    std::unordered_map<uint32_t, std::vector<Catch>> handlers;
    for (uint32_t i = 0; i < list_size; ++i) {
        const auto offset = static_cast<uint32_t>(cursor.at - handlers_at);
        const int64_t size = cursor.sleb();
        const auto typed_count = static_cast<uint32_t>(size < 0 ? -size : size);
        if (typed_count > (bytes.data.size() - cursor.at) / 2)
            invalid("Invalid DEX typed catch count");
        std::vector<Catch> entries;
        entries.reserve(typed_count + (size <= 0));
        for (uint32_t j = 0; j < typed_count; ++j) {
            const auto type = index(cursor.uleb(), type_count, "Invalid DEX catch type");
            const auto target = index(cursor.uleb(), count, "Invalid DEX catch address");
            entries.push_back({type, target});
        }
        if (size <= 0)
            entries.push_back({kNone, index(cursor.uleb(), count, "Invalid DEX catch-all address")});
        handlers.emplace(offset, std::move(entries));
    }
    uint32_t previous_end = 0;
    for (uint32_t i = 0; i < tries_size; ++i, at += 8) {
        const auto start = bytes.u32(at);
        const auto length = bytes.u16(at + 4);
        const auto found = handlers.find(bytes.u16(at + 6));
        if (start < previous_end || start >= count || !length || length > count - start ||
            found == handlers.end())
            invalid("Invalid DEX try range or catch offset");
        previous_end = start + length;
        code.tries.push_back({start, previous_end, found->second});
    }
    return code;
}

void static_values(const Header& header, size_t at, const Class& clazz, size_t static_count,
                   wa_art_dex_t& dex) {
    if (!at)
        return;
    Cursor cursor{header.data_at(at), at};
    const auto count = cursor.uleb();
    if (count > static_count)
        invalid("DEX encoded static values exceed static field count");
    for (uint32_t i = 0; i < count; ++i) {
        auto& field = dex.fields[clazz.fields[i]];
        const auto descriptor = dex.types[field.type];
        const auto tag = cursor.byte();
        const auto encoding = tag & 31;
        const auto arg = tag >> 5;
        unsigned width = 0;
        bool signed_value = false;
        bool right_zero = false;
        switch (encoding) {
        case 0x00: width = 1; signed_value = true; break;
        case 0x02: width = 2; signed_value = true; break;
        case 0x03: width = 2; break;
        case 0x04: width = 4; signed_value = true; break;
        case 0x06: width = 8; signed_value = true; break;
        case 0x10: width = 4; right_zero = true; break;
        case 0x11: width = 8; right_zero = true; break;
        case 0x17: width = 4; break;
        case 0x1e:
            if (arg || kind(descriptor) != Kind::Reference)
                invalid("Invalid null DEX static initializer");
            continue;
        case 0x1f:
            if (arg > 1 || descriptor != "Z")
                invalid("Invalid boolean DEX static initializer");
            field.initial_value = arg;
            continue;
        default: invalid("Unsupported DEX encoded static value");
        }
        const auto encoded_width = static_cast<unsigned>(arg) + 1;
        if (encoded_width > width)
            invalid("Invalid DEX static value width");
        const auto expected = [&] {
            switch (descriptor.front()) {
            case 'B': return 0x00;
            case 'S': return 0x02;
            case 'C': return 0x03;
            case 'I': return 0x04;
            case 'J': return 0x06;
            case 'F': return 0x10;
            case 'D': return 0x11;
            case 'L': return descriptor == "Ljava/lang/String;" ? 0x17 : -1;
            default: return -1;
            }
        }();
        if (encoding != expected)
            invalid("DEX static initializer does not match its field type");
        uint64_t value = 0;
        for (unsigned j = 0; j < encoded_width; ++j)
            value |= uint64_t(cursor.byte()) << (8 * j);
        if (right_zero)
            value <<= (width - encoded_width) * 8;
        if (signed_value && encoded_width < 8 && (value >> (8 * encoded_width - 1)))
            value |= UINT64_MAX << (8 * encoded_width);
        if (encoding == 0x17)
            field.initial_string = index(static_cast<uint32_t>(value), dex.strings.size(),
                                         "Invalid DEX static string index");
        else
            field.initial_value = value;
    }
}

void read_class_data(const Header& header, size_t at, size_t values, Class& clazz,
                     wa_art_dex_t& dex) {
    if (!at) {
        if (values)
            invalid("DEX static values are present without class data");
        return;
    }
    Cursor cursor{header.data_at(at), at};
    const auto static_count = cursor.uleb();
    const auto instance_count = cursor.uleb();
    const auto direct_count = cursor.uleb();
    const auto virtual_count = cursor.uleb();
    if (uint64_t(static_count) + instance_count > dex.fields.size() ||
        uint64_t(direct_count) + virtual_count > dex.methods.size())
        invalid("Invalid DEX class data counts");

    const auto fields = [&](uint32_t count, bool is_static) {
        uint32_t id = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto delta = cursor.uleb();
            if ((i && !delta) || delta > UINT32_MAX - id)
                invalid("Invalid DEX field index delta");
            id = index(id + delta, dex.fields.size(), "Invalid DEX class field index");
            auto& field = dex.fields[id];
            field.access = cursor.uleb();
            if (field.declared || field.owner != clazz.type ||
                ((field.access & kStatic) != 0) != is_static)
                invalid("Invalid DEX field declaration");
            field.declared = true;
            clazz.fields.push_back(id);
        }
    };
    const auto methods = [&](uint32_t count, bool is_virtual) {
        uint32_t id = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto delta = cursor.uleb();
            if ((i && !delta) || delta > UINT32_MAX - id)
                invalid("Invalid DEX method index delta");
            id = index(id + delta, dex.methods.size(), "Invalid DEX class method index");
            auto& method = dex.methods[id];
            method.access = cursor.uleb();
            const auto code_at = cursor.uleb();
            if (method.declared || method.owner != clazz.type ||
                (method.access & (kNative | kAbstract) ? code_at != 0 : code_at == 0) ||
                (is_virtual && (method.access & kStatic)))
                invalid("Invalid DEX method declaration");
            method.declared = true;
            method.is_virtual = is_virtual;
            if (code_at)
                method.code = read_code(header, code_at, dex.types.size());
            clazz.methods.push_back(id);
        }
    };
    fields(static_count, true);
    fields(instance_count, false);
    methods(direct_count, false);
    methods(virtual_count, true);
    static_values(header, values, clazz, static_count, dex);
}

void read_dex(const Header& header, wa_art_dex_t& dex) {
    const auto signature = header.bytes.data.subspan(header.base + 12, dex.signature.size());
    std::copy(signature.begin(), signature.end(), dex.signature.begin());
    const auto [strings_at, strings_count] = header.table(56, 4);
    const auto [types_at, types_count] = header.table(64, 4);
    const auto [protos_at, protos_count] = header.table(72, 12);
    const auto [fields_at, fields_count] = header.table(80, 8);
    const auto [methods_at, methods_count] = header.table(88, 8);
    const auto [classes_at, classes_count] = header.table(96, 32);
    if (types_count > 65535 || protos_count > 65535 || classes_count > types_count)
        invalid("DEX type, prototype, or class count exceeds format limit");
    const auto bytes = header.bytes;
    dex.strings.reserve(strings_count);
    for (size_t i = 0; i < strings_count; ++i)
        dex.strings.push_back(read_string(header, bytes.u32(strings_at + i * 4)));
    const auto string = [&](uint32_t id) -> std::string_view {
        return dex.strings[index(id, strings_count, "Invalid DEX string index")];
    };
    const auto type = [&](uint32_t id) {
        return index(id, types_count, "Invalid DEX type index");
    };

    dex.types.reserve(types_count);
    for (size_t i = 0; i < types_count; ++i) {
        const auto descriptor = string(bytes.u32(types_at + i * 4));
        if (!valid_descriptor(descriptor))
            invalid("Invalid DEX type descriptor");
        dex.types.push_back(descriptor);
    }
    dex.protos.reserve(protos_count);
    for (size_t i = 0, at = protos_at; i < protos_count; ++i, at += 12) {
        const auto shorty = string(bytes.u32(at));
        Prototype proto{type(bytes.u32(at + 4)), type_list(header, bytes.u32(at + 8), types_count),
                        "("};
        if (shorty.size() != proto.parameters.size() + 1)
            invalid("Invalid DEX shorty descriptor length");
        const auto short_kind = [&](uint32_t id) {
            const auto desc = dex.types[id];
            return kind(desc) == Kind::Reference ? 'L' : desc.front();
        };
        if (shorty.front() != short_kind(proto.result))
            invalid("Invalid DEX shorty return type");
        for (size_t j = 0; j < proto.parameters.size(); ++j) {
            const auto param = proto.parameters[j];
            if (dex.types[param] == "V" || shorty[j + 1] != short_kind(param))
                invalid("Invalid DEX parameter type");
            proto.signature += dex.types[param];
        }
        proto.signature += ')';
        proto.signature += dex.types[proto.result];
        dex.protos.push_back(std::move(proto));
    }

    const auto owner_type = [&](uint32_t id) {
        if (dex.types[type(id)].front() != 'L')
            invalid("Invalid DEX member owner");
        return id;
    };
    dex.fields.reserve(fields_count);
    for (size_t i = 0, at = fields_at; i < fields_count; ++i, at += 8) {
        const auto field_type = type(bytes.u16(at + 2));
        if (dex.types[field_type] == "V")
            invalid("DEX field has void type");
        dex.fields.push_back({owner_type(bytes.u16(at)), field_type, string(bytes.u32(at + 4))});
    }
    dex.methods.reserve(methods_count);
    for (size_t i = 0, at = methods_at; i < methods_count; ++i, at += 8) {
        const auto proto = index(bytes.u16(at + 2), protos_count, "Invalid DEX method prototype");
        dex.methods.push_back({owner_type(bytes.u16(at)), proto, string(bytes.u32(at + 4)),
                               0, false, false, {}});
    }

    std::vector<bool> defined(types_count);
    dex.classes.reserve(classes_count);
    for (size_t i = 0, at = classes_at; i < classes_count; ++i, at += 32) {
        const auto clazz = owner_type(bytes.u32(at));
        const auto parent = bytes.u32(at + 8);
        const auto source = bytes.u32(at + 16);
        const auto annotations = bytes.u32(at + 20);
        if (defined[clazz] || clazz == parent)
            invalid("Duplicate or self-derived DEX class");
        if (parent != kNone)
            owner_type(parent);
        if (source != kNone)
            string(source);
        if (annotations)
            header.data_at(annotations, 4);
        defined[clazz] = true;
        Class entry{clazz, parent, type_list(header, bytes.u32(at + 12), types_count), {}, {}};
        for (auto interface : entry.interfaces)
            owner_type(interface);
        read_class_data(header, bytes.u32(at + 24), bytes.u32(at + 28), entry, dex);
        dex.classes.push_back(std::move(entry));
    }
}

} // namespace

void copy_error(char* error, size_t capacity, std::string_view text) {
    if (!error || !capacity)
        return;
    const auto length = std::min(text.size(), capacity - 1);
    std::memcpy(error, text.data(), length);
    error[length] = 0;
}

std::string utf8_name(std::string_view mutf8) {
    Cursor cursor{{std::span(reinterpret_cast<const uint8_t*>(mutf8.data()), mutf8.size())}, 0};
    std::string result;
    while (cursor.at < mutf8.size()) {
        uint32_t value = mutf_unit(cursor, cursor.byte());
        if (value >= 0xd800 && value <= 0xdbff) {
            const auto trail = mutf_unit(cursor, cursor.byte());
            if (trail < 0xdc00 || trail > 0xdfff)
                invalid("Unpaired surrogate in DEX identifier");
            value = 0x10000 + ((value - 0xd800) << 10) + (trail - 0xdc00);
        } else if (value >= 0xdc00 && value <= 0xdfff) {
            invalid("Unpaired surrogate in DEX identifier");
        }
        const auto append = [&](uint32_t byte) { result.push_back(static_cast<char>(byte)); };
        if (value < 0x80) {
            append(value);
        } else if (value < 0x800) {
            append(0xc0 | (value >> 6));
            append(0x80 | (value & 0x3f));
        } else {
            if (value >= 0x10000) {
                append(0xf0 | (value >> 18));
                append(0x80 | ((value >> 12) & 0x3f));
            } else {
                append(0xe0 | (value >> 12));
            }
            append(0x80 | ((value >> 6) & 0x3f));
            append(0x80 | (value & 0x3f));
        }
    }
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
        if (!data)
            throw std::invalid_argument("Missing DEX bytes");
        auto dex = std::make_unique<wa_art_dex>();
        const wa::art::Header header({data, size}, header_offset);
        wa::art::read_dex(header, *dex);
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
