#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace wa {

// Foreground boot and its synchronous callbacks run on the same host thread.
// Keep exclusive times so, for example, a page fault inside DBT is counted once.
struct BootProfile {
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    enum Phase {
        Setup, Dex, DexExtract, DexVerify, Java, AttachBaseContext, Providers, ApplicationCreate,
        ActivityLifecycle, ActivityTransition, FirstFrame, Elf, Relocations, PageFault,
        ApkEntryRead, ApkEntryInflate, Dbt, PhaseCount
    };
    static constexpr std::array<std::string_view, PhaseCount> names{
        "setupMs", "dexIndexAndOpenMs", "dexExtractMs", "dexVerifyMs", "javaMs",
        "attachBaseContextMs", "providersMs", "applicationCreateMs", "activityLifecycleMs",
        "activityTransitionMs", "firstFrameMs", "elfLoadMs", "relocationsMs", "pageFaultMs",
        "apkEntryReadMs", "apkEntryInflateMs", "dbtMs",
    };

    struct NativeCalls {
        Duration elapsed{};
        uint64_t instructions = 0;
    };
    struct Library {
        NativeCalls constructors, jni_on_load;
    };
    struct DbtCounters {
        uint64_t wasm_units_compiled = 0, wasm_bytes_emitted = 0;
        uint64_t jit_entries = 0, jit_compile_failures = 0, tier0_insns = 0;
        uint64_t jit_chains = 0, jit_evictions = 0, jit_fast_host_calls = 0, jit_exec_samples = 0;
        uint64_t iotlb_slow_reads = 0, iotlb_slow_writes = 0;
        double jit_compile_ms = 0, jit_exec_ms = 0;
    };
    struct HostCallStats {
        Duration elapsed{};
        uint64_t calls = 0;
    };

    std::array<Duration, PhaseCount> elapsed{};
    std::map<std::string, Library, std::less<>> libraries;
    bool collect_hotspots = false;
    std::map<std::string, HostCallStats, std::less<>> host_calls;
    std::map<std::string, uint64_t, std::less<>> inflated_entries;
    uint64_t arm64_instructions = 0, arm64_fast_instructions = 0;
    DbtCounters dbt_stats{};
    uint64_t entry_read_calls = 0, entry_read_bytes = 0, inflated_bytes = 0;
    uint64_t replayed_inflated_bytes = 0;
    Phase phase = Setup;
    Clock::time_point last;
    static inline thread_local BootProfile* active = nullptr;

    void SwitchTo(Phase next) {
        const auto now = Clock::now();
        elapsed[phase] += now - last;
        last = now;
        phase = next;
    }

    struct Capture {
        BootProfile& profile;
        BootProfile* previous;

        explicit Capture(BootProfile& profile) : profile(profile), previous(active) {
            profile.last = Clock::now();
            active = &profile;
        }
        ~Capture() {
            profile.SwitchTo(Setup);
            active = previous;
        }
    };

    struct Scope {
        BootProfile* profile = active;
        Phase previous;

        explicit Scope(Phase phase) {
            if (!profile || profile->phase == phase) {
                profile = nullptr;
                return;
            }
            previous = profile->phase;
            profile->SwitchTo(phase);
        }
        ~Scope() { if (profile) profile->SwitchTo(previous); }
    };

    struct HostCall {
        HostCallStats* stats = nullptr;
        Clock::time_point start;
        bool counted = true;

        explicit HostCall(std::string_view name) {
            if (!active || !active->collect_hotspots) return;
            auto& calls = active->host_calls;
            auto entry = calls.lower_bound(name);
            if (entry == calls.end() || entry->first != name)
                entry = calls.emplace_hint(entry, name, HostCallStats{});
            stats = &entry->second;
            start = Clock::now();
        }
        ~HostCall() {
            if (!stats) return;
            stats->calls += counted;
            stats->elapsed += Clock::now() - start;
        }
    };

    // Inclusive for this entry point, including dependencies and guest callbacks.
    struct LibraryCall {
        NativeCalls* stats = nullptr;
        const uint64_t& instructions;
        uint64_t before = 0;
        Clock::time_point start;

        LibraryCall(std::string_view name, NativeCalls Library::*member, const uint64_t& instructions)
            : instructions(instructions) {
            if (!active) return;
            auto& libraries = active->libraries;
            auto entry = libraries.lower_bound(name);
            if (entry == libraries.end() || entry->first != name)
                entry = libraries.emplace_hint(entry, name, Library{});
            stats = &(entry->second.*member);
            before = instructions;
            start = Clock::now();
        }
        ~LibraryCall() {
            if (!stats) return;
            stats->elapsed += Clock::now() - start;
            stats->instructions += instructions - before;
        }
    };
};

} // namespace wa
