#include "wasmandroid/art_wasm.h"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kUsage = "Usage: dex2wasm <input.dex> -o <output.wasm> "
                                    "[--profile <methods.txt>]\n";

struct Options {
    const char* input = nullptr;
    const char* output = nullptr;
    const char* profile = nullptr;
};

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-o" || arg == "--profile") {
            auto& value = arg == "-o" ? options.output : options.profile;
            if (value || ++i == argc)
                throw std::invalid_argument("Expected one path after " + std::string(arg));
            value = argv[i];
        } else if (arg.starts_with('-') || options.input) {
            throw std::invalid_argument("Unexpected argument " + std::string(arg));
        } else {
            options.input = argv[i];
        }
    }
    if (!options.input || !options.output)
        throw std::invalid_argument("An input DEX and an output Wasm path are required");
    return options;
}

template <typename Buffer>
Buffer read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not open " + std::string(path));
    Buffer data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad())
        throw std::runtime_error("Could not read " + std::string(path));
    return data;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        std::cout << kUsage;
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        const auto input = read_file<std::vector<uint8_t>>(options.input);
        const auto profile = options.profile ? read_file<std::string>(options.profile) : "";
        if (profile.find('\0') != std::string::npos)
            throw std::invalid_argument("Profile contains a NUL byte");
        std::array<char, 512> error{};
        const std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)> dex(
            wa_art_dex_open(input.data(), input.size(), 0, error.data(), error.size()),
            wa_art_dex_close);
        if (!dex)
            throw std::runtime_error(error.data());
        uint8_t* bytes = nullptr;
        size_t size = 0;
        const auto status = wa_art_compile(dex.get(), options.profile ? profile.c_str() : nullptr,
                                           &bytes, &size, error.data(), error.size());
        const std::unique_ptr<uint8_t, decltype(&wa_art_free_buffer)> wasm(bytes, wa_art_free_buffer);
        if (status != WA_OK)
            throw std::runtime_error(error.data());
        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char*>(wasm.get()), size))
            throw std::runtime_error("Could not write " + std::string(options.output));
        output.close();
        if (!output)
            throw std::runtime_error("Could not close " + std::string(options.output));
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "dex2wasm: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "dex2wasm: " << error.what() << '\n';
        return 1;
    }
}
