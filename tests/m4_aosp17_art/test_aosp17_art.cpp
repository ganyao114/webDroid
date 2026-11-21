#include "aosp17_dex_to_wasm.h"
#include "wasmandroid/art_wasm.h"

#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_file_verifier.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), std::string("Cannot read ") + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void inspect(const art::DexFile& dex) {
    unsigned classes = 0;
    unsigned static_fields = 0;
    unsigned instance_fields = 0;
    unsigned methods = 0;
    unsigned instructions = 0;
    unsigned tries = 0;
    unsigned catches = 0;
    bool branch = false;
    bool call = false;
    bool typed_catch = false;
    for (const art::ClassAccessor clazz : dex.GetClasses()) {
        ++classes;
        require(clazz.GetDescriptorView() == "LAospArtProbe;", "Unexpected class descriptor");
        for (const auto& field : clazz.GetFields()) {
            require(field.GetIndex() < dex.NumFieldIds(), "ART returned an invalid field index");
            if (field.IsStatic())
                ++static_fields;
            else
                ++instance_fields;
        }
        for (const auto& method : clazz.GetMethods()) {
            ++methods;
            const art::CodeItemDataAccessor code = method.GetInstructionsAndData();
            require(code.HasCodeItem(), "Probe method is missing code");
            const art::DexInstructionIterator end = code.end();
            uint32_t width = 0;
            for (art::DexInstructionIterator ins = code.begin(); ins != end; ++ins) {
                require(ins.DexPc() == width, "ART instruction offsets are discontinuous");
                width += ins->SizeInCodeUnits();
                branch |= ins->IsBranch();
                call |= ins->IsInvoke();
                ++instructions;
            }
            require(width == code.InsnsSizeInCodeUnits(), "ART instruction widths disagree");
            for (const auto& item : code.TryItems()) {
                ++tries;
                require(item.start_addr_ + item.insn_count_ <= width, "Bad try range");
                for (art::CatchHandlerIterator handler(code, item); handler.HasNext();
                     handler.Next()) {
                    ++catches;
                    const auto type = handler.GetHandlerTypeIndex();
                    typed_catch |= type.IsValid() &&
                        dex.GetTypeDescriptorView(type) == "Ljava/lang/ArithmeticException;";
                    require(handler.GetHandlerAddress() < width, "Bad catch target");
                }
            }
        }
    }
    require(classes == 1 && static_fields == 2 && instance_fields == 1 && methods == 7,
            "Unexpected class data");
    require(instructions > methods && branch && call, "Expected loop, calls and method bodies");
    require(tries == 1 && catches == 1 && typed_catch, "Expected typed arithmetic catch handler");
    std::cout << "ART walked " << classes << " class, " << static_fields + instance_fields
              << " fields, " << methods << " methods, " << instructions << " instructions, "
              << tries << " try item and " << catches << " catch handler\n";
}

void check_rejection(std::vector<uint8_t> bytes, size_t offset, std::string_view diagnostic) {
    bytes.at(offset) ^= 1;
    const std::string location = "corrupted.dex";
    art::ArtDexFileLoader loader(bytes.data(), bytes.size(), location);
    std::string error;
    auto file = loader.OpenOne(0, 0, nullptr, /*verify=*/false, /*verify_checksum=*/false, &error);
    require(file != nullptr, error);
    const bool verified = art::dex::Verify(file.get(), location.c_str(), true, &error);
    require(!verified && error.find(diagnostic) != std::string::npos, error);
    std::cout << "Upstream verifier rejected " << error << '\n';

    std::array<char, 512> api_error{};
    const std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)> parsed(
        wa_art_dex_open(bytes.data(), bytes.size(), 0, api_error.data(), api_error.size()),
        wa_art_dex_close);
    require(parsed == nullptr && std::string_view(api_error.data()).contains(diagnostic),
            "The backend accepted or misreported corrupt DEX");
}

void compile(const std::vector<uint8_t>& input, const char* output) {
    std::array<char, 512> error{};
    const std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)> dex(
        wa_art_dex_open(input.data(), input.size(), 0, error.data(), error.size()), wa_art_dex_close);
    require(dex != nullptr, error.data());
    require(wa_art_dex_find_type(dex.get(), "LAospArtProbe;") != WA_ART_NO_INDEX,
            "Backend type projection failed");
    require(wa_art_dex_find_field(dex.get(), "LAospArtProbe;->MARKER:I") != WA_ART_NO_INDEX,
            "Backend field projection failed");
    require(wa_art_dex_find_method(dex.get(), "LAospArtProbe;->blend(JJI)J") != WA_ART_NO_INDEX,
            "Backend method projection failed");

    uint8_t* data = nullptr;
    size_t size = 0;
    const auto status = wa_art_compile(dex.get(), nullptr, &data, &size, error.data(), error.size());
    const std::unique_ptr<uint8_t, decltype(&wa_art_free_buffer)> wasm(data, wa_art_free_buffer);
    require(status == WA_OK, error.data());
    const auto direct = wa::aosp17::CompileDexToWasm(input);
    require(direct == std::vector<uint8_t>(data, data + size), "Compiler entry points disagree");
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data), size);
    file.close();
    require(file.good(), "Cannot write Wasm output");
    std::cout << "CodeGeneratorWasm emitted " << size << " bytes\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 5, "Expected input path, DEX version, input format and Wasm output path");
#ifdef __EMSCRIPTEN__
        static_assert(sizeof(void*) == 4);
        std::cout << "ART verifier and compiler running on wasm32\n";
#endif
        const auto input = read_file(argv[1]);
        const auto version = std::stoul(argv[2]);
        const bool zip = std::string_view(argv[3]) == "apk";
        const auto files = wa::aosp17::OpenDexFiles(input, argv[1]);
        require(files.size() == 1, "Expected one logical DEX");
        const auto& dex = *files.front();
        require(dex.GetDexVersion() == version, "Wrong DEX fixture version");
        require(dex.GetContainer()->IsZip() == zip, "Wrong ART container type");
        std::string error;
        const bool verified = art::dex::Verify(&dex, dex.GetLocation().c_str(), true, &error);
        require(verified, error);
        std::cout << "Upstream verifier accepted DEX v" << dex.GetDexVersion()
                  << (zip ? " from ZIP\n" : "\n");
        inspect(dex);
        if (!zip) {
            check_rejection(input, dex.Size() - 1, "Bad checksum");
            check_rejection(input, offsetof(art::DexFile::Header, endian_tag_), "endian_tag");
        }
        compile(input, argv[4]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
