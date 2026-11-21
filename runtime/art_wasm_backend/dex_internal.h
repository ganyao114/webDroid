#pragma once

#include "wasmandroid/art_wasm.h"

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wa::art {

constexpr uint32_t kNone = WA_ART_NO_INDEX;
constexpr uint32_t kStatic = 0x0008;
constexpr uint32_t kNative = 0x0100;
constexpr uint32_t kAbstract = 0x0400;

enum class Kind : uint8_t { Unset, Zero, Scalar, Reference, Wide, WideHigh, Conflict };

struct Prototype {
    uint32_t result;
    std::vector<uint16_t> parameters;
    std::string signature;
};

struct Field {
    uint32_t owner;
    uint32_t type;
    std::string_view name;
    uint32_t access = 0;
    bool declared = false;
    uint64_t initial_value = 0;
    uint32_t initial_string = kNone;
};

struct Catch {
    uint32_t type;
    uint32_t target;
};

struct Try {
    uint32_t start;
    uint32_t end;
    std::vector<Catch> handlers;
};

struct Code {
    uint16_t registers = 0;
    uint16_t ins = 0;
    uint16_t outs = 0;
    std::vector<uint16_t> words;
    std::vector<Try> tries;
};

struct Method {
    uint32_t owner;
    uint32_t proto;
    std::string_view name;
    uint32_t access = 0;
    bool declared = false;
    bool is_virtual = false;
    Code code;
};

struct Class {
    uint32_t type;
    uint32_t parent;
    std::vector<uint16_t> interfaces;
    std::vector<uint32_t> fields;
    std::vector<uint32_t> methods;
};

void copy_error(char* error, size_t capacity, std::string_view text);
std::string utf8_name(std::string_view mutf8);
Kind kind(std::string_view descriptor);
wa_art_element_kind_t element_kind(std::string_view descriptor);
std::vector<uint8_t> generate_wasm(const wa_art_dex_t& dex, const char* profile);

} // namespace wa::art

struct wa_art_dex {
    std::array<uint8_t, 20> signature{};
    std::vector<std::string> strings;
    std::vector<std::string_view> types;
    std::vector<wa::art::Prototype> protos;
    std::vector<wa::art::Field> fields;
    std::vector<wa::art::Method> methods;
    std::vector<wa::art::Class> classes;

    std::string method_name(uint32_t index) const;
    std::string field_name(uint32_t index) const;
    uint32_t find_type(std::string_view descriptor) const;
    uint32_t find_method(uint32_t owner, std::string_view name, std::string_view signature) const;
};
