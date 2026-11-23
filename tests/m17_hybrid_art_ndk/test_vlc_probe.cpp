#include "foreground_test.h"
#include "hybrid_native_runtime.h"

#include <iostream>

namespace {

using namespace hybrid_test;

void Names(std::string_view label, const std::vector<std::string>& names) {
    std::cout << "  " << label << " (" << names.size() << ")";
    for (const auto& name : names) std::cout << ' ' << name;
    std::cout << '\n';
}

std::string DexName(size_t index) {
    return index ? "classes" + std::to_string(index + 1) + ".dex" : "classes.dex";
}

} // namespace

int main(int argc, char** argv) try {
    Require(argc == 5, "Expected framework DEX, fallback DEX, resources and VLC APK");
    const std::filesystem::path apk = argv[4];
    if (!std::filesystem::exists(apk)) {
        std::cout << "VLC probe skipped because the local APK was not installed\n";
        return 0;
    }
    {
        wa::interp::Vm vm;
        vm.LoadApkDexFiles(apk.string());
        const auto dex = vm.GetDexStats();
        Require(dex.files.size() == 6, "Expected six VLC DEX entries");
        for (size_t i = 0; i < dex.files.size(); ++i) {
            const auto& file = dex.files[i];
            Require(file.location.ends_with("!" + DexName(i)), "VLC DEX entries are out of order");
            std::cout << "dex " << DexName(i) << " " << file.class_definitions << " classes\n";
        }
        Require(vm.HasClass("Lorg/videolan/vlc/StartActivity;") && vm.HasClass("Lorg/videolan/mobile/app/VLCApplication;"),
                "Missing VLC launcher or Application DEX class");
    }
    const auto native = wa::system::ProbeApkNativeLibraries(apk.string(), WA_ARCH_ARM64);
    const std::vector<std::string> expected{"libc++_shared.so", "libmla.so", "libvlc.so", "libvlcjni.so"};
    Require(native.size() == expected.size(), "Expected four VLC ARM64 libraries");
    for (size_t i = 0; i < native.size(); ++i) {
        const auto& library = native[i];
        Require(library.name == expected[i] && library.elf_status == WA_OK, "Invalid production ELF " + library.name);
        std::cout << "elf " << library.name << " mapped_base=" << library.mapped_base << '\n';
        Names("DT_NEEDED", library.needed);
        Names("missing DT_NEEDED libraries", library.missing_libraries);
        Names("resolvable imports", library.resolved);
        Names("missing strong imports", library.missing);
        Names("unresolved weak imports", library.weak_undefined);
        if (!library.load_error.empty()) std::cout << "  load stopped: " << library.load_error << '\n';
    }
    Foreground foreground(argv[1], argv[2], argv[3]);
    const auto result = foreground.Boot(apk);
    const auto error = result < 0 ? LastError() : std::string{};
    std::cout << "activity boot_status=" << result << " " << State() << '\n';
    if (!error.empty()) std::cout << "  boot stopped: " << error << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
