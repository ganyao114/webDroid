#include "art_interp.h"
#include "art_sf_entry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {

constexpr const char* kBench = "Lorg/wasmandroid/m8/PerfBench;";
constexpr int kFrames = 200;
constexpr int kWarmup = 60;
constexpr uint32_t kExpectedHash = 3119144267u;

std::string Error() {
  std::array<char, 512> error{};
  wa_system_art_last_error(error.data(), error.size());
  return error.data();
}

void Check(bool condition, std::string_view message) {
  if (condition) return;
  throw std::runtime_error(std::string(message) + " " + Error());
}

std::vector<uint8_t> File(const char* path) {
  std::ifstream in(path, std::ios::binary);
  Check(in.good(), path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Load(const char* path) {
  auto bytes = File(path);
  Check(wa_system_art_load_dex(path, bytes.data(), bytes.size()) == WA_OK, path);
}

void CallVoid(const char* name) {
  Check(wa_system_art_call_static_void(kBench, name, "()V") == WA_OK, name);
}

int32_t CallInt(const char* name) {
  const int32_t value = wa_system_art_call_static_int(kBench, name, "()I");
  if (value == 0) Check(Error().empty(), name);
  return value;
}

size_t Stat(std::string_view key) {
  std::array<char, 256> json{};
  Check(wa_system_art_stats_json(json.data(), json.size()) == WA_OK, "ART stats");
  const std::string_view text(json.data());
  const std::string field = "\"" + std::string(key) + "\":";
  const size_t start = text.find(field);
  Check(start != text.npos, key);
  size_t number = 0;
  Check(std::from_chars(text.data() + start + field.size(), text.data() + text.size(), number).ec
            == std::errc{}, key);
  return number;
}

struct Run {
  double mean_ms;
  double p95_ms;
  uint32_t hash;
  uint32_t color;
};

Run Measure(bool cache) {
  Check(wa_system_art_set_inline_caches_enabled(cache) == WA_OK, "Set ART inline caches");
  CallVoid("prepare");
  for (int i = 0; i < kWarmup; ++i) CallInt("frame");
  CallVoid("prepare");

  std::array<double, kFrames> elapsed;
  double sum = 0;
  uint32_t hash = 0, frame = 0;
  for (double& ms : elapsed) {
    Check(wa_system_art_set_gc_paused(1) == WA_OK, "Pause collector for frame");
    const auto start = std::chrono::steady_clock::now();
    frame = static_cast<uint32_t>(CallInt("frame"));
    const auto end = std::chrono::steady_clock::now();
    Check(wa_system_art_set_gc_paused(0) == WA_OK, "Resume collector after frame");
    ms = std::chrono::duration<double, std::milli>(end - start).count();
    sum += ms;
    hash = hash * 31 + frame;
  }
  std::ranges::sort(elapsed);
  return {sum / kFrames, elapsed[kFrames * 95 / 100 - 1], hash, frame};
}

void CheckScene(uint32_t color) {
  Check(wa_system_sf_init(320, 480, 2) == WA_SF_OK, "Initialize SurfaceFlinger");
  auto* sf = wa_system_sf_ctx();
  Check(sf && wa_sf_begin(sf, 320, 480, 320) == WA_SF_OK, "Begin scene");
  wa_sf_layer_desc_t layer{};
  layer.layer_id = 7;
  layer.width = 320;
  layer.height = 480;
  layer.scale_x = layer.scale_y = layer.alpha = 1;
  layer.flags = WA_SF_VISIBLE;
  layer.surface_kind = WA_SF_SURFACE_DISPLAY_LIST;
  Check(wa_sf_set_layer(sf, &layer) == WA_SF_OK, "Set layer");
  auto* canvas = wa_canvas_begin(sf, layer.layer_id);
  Check(canvas && wa_canvas_draw_color(canvas, color) == WA_SF_OK && wa_canvas_end(canvas) == WA_SF_OK,
        "Encode ART frame color");
  Check(wa_sf_commit(sf) == WA_SF_OK, "Commit scene");

  const auto* packet = wa_sf_frame_acquire(sf);
  Check(packet && packet->magic == WA_SF_FRAME_MAGIC && packet->layer_count == 1
            && packet->viewport_w == 320 && packet->viewport_h == 480 && packet->density_dpi == 320, "Scene packet");
  const auto* layers = wa_sf_frame_layers(packet);
  Check(layers->desc.layer_id == 7 && layers->list_size == 8, "Display list");
  const auto* list = wa_sf_frame_list(packet, layers);
  wa_canvas_record_t record{};
  std::memcpy(&record, list, sizeof(record));
  uint32_t argb = 0;
  std::memcpy(&argb, list + sizeof(record), sizeof(argb));
  Check(record.opcode == WA_CANVAS_DRAW_COLOR && record.byte_size == 8 && argb == color,
        "Published ART frame color");
  wa_sf_frame_release(sf, packet);

  std::array<uint8_t, 4> pixel{12, 34, 56, 255};
  auto* bitmaps = wa_sf_bitmaps(sf);
  Check(wa_bitmap_put(bitmaps, 3, 1, 1, 4, pixel.data()) == WA_SF_OK, "Publish bitmap");
  const auto* view = wa_bitmap_acquire(bitmaps, 3);
  Check(view && std::memcmp(wa_bitmap_pixels(view), pixel.data(), pixel.size()) == 0, "Read bitmap");
  wa_bitmap_release(bitmaps, view);
  Check(wa_bitmap_remove(bitmaps, 3) == WA_SF_OK, "Remove bitmap");
  wa_system_sf_destroy();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 5, "Expected benchmark DEX, package-manager DEX, resource bridge DEX and APK");
    {
      wa::interp::Vm probe;
      Check(!probe.HasClass(kBench), "DEX class absent before loading");
      probe.LoadDexFile(argv[1]);
      Check(probe.HasClass(kBench) && probe.HasClass("[[I") &&
                probe.HasClass("[Lorg/wasmandroid/m8/PerfBench;") && !probe.HasClass("[V") &&
                probe.ExceptionSummary().empty(), "HasClass descriptor lookup");
    }

    Check(wa_system_art_create(16384) == WA_OK, "Create ART");
    Check(wa_system_art_register_resource_natives() == WA_ERR_INVALID &&
              Error().find("Resource DEX missing") != std::string::npos, "Resource registration before DEX loading");
    Load(argv[1]);
    Load(argv[2]);
    Load(argv[3]);
    const auto apk = File(argv[4]);
    std::array<char, 64> path{};
    Check(wa_system_art_mount_apk(apk.data(), apk.size(), path.data(), path.size()) == WA_OK, "Mount APK through JNI");
    Check(std::filesystem::file_size(path.data()) == apk.size(), "Mounted APK byte count");
    Check(Stat("apkAssetsNativeCalls") >= 3, "AOSP ApkAssets JNI calls");

    CallVoid("verify");
    std::array<char, 96> summary{};
    Check(wa_system_art_call_static_string(kBench, "summary", "()Ljava/lang/String;", summary.data(), summary.size())
              == WA_OK, "Read Java result");
    Check(CallInt("callsPerFrame") >= 10000, "Minimum view method calls per frame");
    const Run cached = Measure(true);
    const Run uncached = Measure(false);
    Check(cached.hash == uncached.hash && cached.hash == kExpectedHash, "Render checksums");

    Check(wa_system_art_set_gc_paused(1) == WA_OK, "Pause GC");
    const auto before = Stat("collections");
    CallVoid("installWeak");
    CallVoid("allocateDuringFrame");
    Check(Stat("collections") == before, "No collection in frame");
    Check(CallInt("weakCleared") == 0, "Weak reference survives until collection");
    CheckScene(cached.color);
    Check(wa_system_art_set_gc_paused(0) == WA_OK && wa_system_art_gc_collect() == WA_OK, "Collect after commit");
    Check(Stat("collections") == before + 1 && Stat("reclaimed") > 0 && CallInt("weakCleared") == 1,
          "Collector reclaimed weak referent");
    CallVoid("allocateDuringFrame");
    Check(Stat("collections") > before + 1, "Collector runs at the allocation watermark outside frames");
    wa_system_art_destroy();
    Check(!std::filesystem::exists(path.data()), "Mount released");

    std::printf("Native %d frames, %u checksum, %.4f ms/frame cached (p95 %.4f), "
                "%.4f ms/frame uncached (%.2fx). Verified %s, GC and SurfaceFlinger.\n",
                kFrames, cached.hash, cached.mean_ms, cached.p95_ms, uncached.mean_ms,
                uncached.mean_ms / cached.mean_ms, summary.data());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    wa_system_sf_destroy();
    wa_system_art_destroy();
    return 1;
  }
}
