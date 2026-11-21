#pragma once

#include <wasmandroid/surfaceflinger.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wa::interp {
class Vm;
}

namespace wa::ui_jni {

struct IntentRequest {
    std::optional<std::string> action;
    std::optional<std::string> package_name;
    std::optional<std::string> activity_name;
    std::optional<std::string> data_uri;
    std::optional<std::string> mime_type;
    std::vector<std::string> categories;
    uint64_t query_flags = 0;
    uint32_t intent_flags = 0;
    int32_t user_id = 0;
};

struct PackageActivity {
    std::string package_name;
    std::string activity_name;
    std::string label;
    std::string apk_path;
    uint32_t icon_resource = 0;
    bool enabled = true;
};

struct PackageCallbacks {
    std::function<std::vector<PackageActivity>(const IntentRequest&)> query_activities;
    std::function<std::optional<PackageActivity>(std::string_view package_name, uint64_t flags,
                                                 int32_t user_id)>
        get_application;
    std::function<bool(const IntentRequest&)> start_activity;
};

struct ActivityConfig {
    std::string apk_path;
    // Empty selects the manifest's first MAIN/LAUNCHER or MAIN/HOME activity.
    std::string class_name;
    std::string locale = "en-US";
    int32_t density_dpi = 160;
    int32_t width = 0;
    int32_t height = 0;
    std::string framework_resource_apk;
};

namespace detail {
struct State;
}

// Call on the VM thread. The VM and the bound scene must outlive the bridge.
class UiBridge {
  public:
    UiBridge();
    ~UiBridge();
    UiBridge(const UiBridge&) = delete;
    UiBridge& operator=(const UiBridge&) = delete;

    // Load the framework DEX and application DEX before registering and launching.
    void RegisterNatives(interp::Vm& vm);
    void BindSurfaceFlinger(wa_sf_t* sf, uint32_t layer_id);
    void SetPackageCallbacks(PackageCallbacks callbacks);

    void LaunchActivity(interp::Vm& vm, const ActivityConfig& config);
    void PauseActivity(interp::Vm& vm);
    void StopActivity(interp::Vm& vm);
    void ResumeActivity(interp::Vm& vm);
    void DestroyActivity(interp::Vm& vm);

    // The no-argument tick reads the compositor's latest vsync. On WA_SF_BUSY
    // the next tick retries the already recorded transaction before drawing again.
    int32_t DoFrame(interp::Vm& vm);
    int32_t DoFrame(interp::Vm& vm, const wa_sf_vsync_state_t& tick);
    size_t DispatchInputEvents(interp::Vm& vm, wa_sf_t* sf);

  private:
    std::unique_ptr<detail::State> state_;
};

} // namespace wa::ui_jni
