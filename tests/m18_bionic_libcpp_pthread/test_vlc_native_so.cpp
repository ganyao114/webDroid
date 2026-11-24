#include "foreground_test.h"
#include "hybrid_native_runtime.h"

#include <array>
#include <iostream>

int main(int argc, char** argv) try {
    using hybrid_test::Require;
    using wa::system::NativeProbeAction;
    using wa::system::NativeProbeRequest;
    Require(argc == 4, "Expected the VLC ARM64 APK and two framework DEX files");
    if (!std::filesystem::exists(argv[1])) {
        std::cout << "VLC native test skipped because the local APK is absent\n";
        return 0;
    }

    constexpr NativeProbeRequest requests[] = {
        {"libvlc.so", "libvlc_get_version", NativeProbeAction::CallString},
        {"libvlc.so", "libvlc_get_compiler", NativeProbeAction::CallString},
        {"libvlc.so", "libvlc_get_changeset", NativeProbeAction::CallString},
        {"libvlcjni.so", "JNI_OnLoad", NativeProbeAction::CallJniOnLoad},
    };
    wa::interp::Vm vm;
    vm.LoadDexFile(argv[2]);
    vm.LoadFallbackDexFile(argv[3]);
    vm.LoadApkDexFiles(argv[1]);
    const auto libraries =
        wa::system::ProbeApkNativeLibraries(argv[1], WA_ARCH_ARM64, requests, vm.JniEnv());
    const std::array<std::string_view, 4> expected{"libc++_shared.so", "libmla.so", "libvlc.so",
                                                   "libvlcjni.so"};
    Require(libraries.size() == expected.size(), "Expected all four VLC ARM64 libraries");
    size_t called = 0;
    for (size_t i = 0; i < libraries.size(); ++i) {
        const auto& library = libraries[i];
        Require(library.name == expected[i] && library.elf_status == WA_OK,
                "Invalid " + library.name);
        std::cout << library.name << " base=0x" << std::hex << library.mapped_base << std::dec
                  << " missing DT_NEEDED=" << library.missing_libraries.size()
                  << " missing strong=" << library.missing.size()
                  << " unresolved weak=" << library.weak_undefined.size() << std::endl;
        for (const auto* missing :
             {&library.missing_libraries, &library.missing, &library.weak_undefined})
            for (const auto& name : *missing)
                std::cout << "  unresolved " << name << '\n';
        Require(library.missing_libraries.empty() && library.missing.empty() &&
                    library.weak_undefined.empty(),
                "Unresolved production ELF imports for " + library.name + " " + library.load_error);
        Require(library.mapped_base && library.load_error.empty(),
                library.name + " " + library.load_error);
        for (const auto& entry : library.entrypoints) {
            std::cout << entry.name << " at 0x" << std::hex << entry.address << std::dec << ' '
                      << (entry.name == "JNI_OnLoad" && entry.diagnostic.empty() ? "OK"
                                                                                 : entry.value)
                      << entry.diagnostic << '\n';
            Require(entry.address != 0, "Missing " + entry.name);
            if (entry.name != "JNI_OnLoad") {
                Require(entry.diagnostic.empty() && !entry.value.empty(),
                        entry.name + " " + entry.diagnostic);
                if (entry.name == "libvlc_get_version")
                    Require(entry.value.starts_with("3.") && entry.value.size() > 3 &&
                                entry.value[2] >= '0' && entry.value[2] <= '9',
                            "Invalid VLC 3.x version");
            }
            ++called;
        }
    }
    Require(called == std::size(requests), "Expected four native entrypoint checks");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
