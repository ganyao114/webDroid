#include "art_interp.h"
#include "jni_bridge.h"
#include "ui_bridge.h"
#include "../../runtime/hal_surfaceflinger/font_advances.h"

#include <dex/class_accessor-inl.h>
#include <ziparchive/zip_archive.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using wa::interp::Value;
using wa::interp::Vm;
using wa::ui_jni::IntentRequest;
using wa::ui_jni::PackageActivity;

constexpr char kActivity[] = "Lorg/wasmandroid/uismoke/SmokeActivity;";
// The fixture's footer must fit below the card after the window applies its system insets.
constexpr uint32_t kLayer = 42, kWidth = 400, kHeight = 432, kDpi = 240;

void Require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void AuditFrameworkNatives(Vm& vm, std::string_view upstream, std::string_view fallback) {
    auto& linker = wa::interp::JniBridge::From(vm.JniEnv()).Vm().linker;
    const auto archive = [](std::string_view name) { return name.substr(0, name.rfind('!')); };
    std::unordered_set<std::string_view> classes;
    uint32_t primary_methods = 0;
    size_t declarations = 0;
    std::string missing;
    for (const auto& dex : linker.LoadedDexFiles()) {
        const std::string_view location = dex->GetLocation();
        const auto owner = archive(location);
        if (owner != upstream && owner != fallback) continue;
        if (owner == upstream && (location == owner || location.ends_with("!classes.dex"))) {
            primary_methods = dex->NumMethodIds();
        }
        for (uint32_t i = 0; i < dex->NumClassDefs(); ++i) {
            const auto& definition = dex->GetClassDef(i);
            const auto name = dex->GetTypeDescriptorView(definition.class_idx_);
            Require(classes.insert(name).second, "Duplicate framework DEX class " + std::string(name));
            if (owner != upstream) continue;
            const bool art_boot = name == "Ljava/io/FileInputStream;" || name == "Ljava/io/FileOutputStream;" ||
                name == "Ljava/io/RandomAccessFile;" || name == "Ljava/util/regex/Pattern;" ||
                name == "Ljava/lang/ref/ReferenceQueue;" ||
                name == "Landroid/os/Debug;" || name == "Landroid/system/Os;" ||
                name == "Ljava/lang/reflect/Field;" || name == "Ljava/lang/reflect/Executable;" ||
                name == "Ldalvik/system/VMRuntime;" || name == "Ljava/security/MessageDigest$EmbeddedDigest;" ||
                name == "Lsun/misc/Unsafe;" || name == "Ljava/lang/ThreadGroup;" ||
                name == "Ljava/util/zip/CRC32;" || name == "Ljava/util/zip/Deflater;" ||
                name == "Landroid/net/LocalSocketImpl;";
            const bool package_manager = name == "Landroid/content/pm/PackageManager;";
            const bool apache = name.starts_with("Lorg/apache/");
            if (!art_boot && !package_manager && !apache && !name.starts_with("Landroid/graphics/") &&
                !name.starts_with("Lcom/android/internal/util/") &&
                name != "Landroid/text/AndroidCharacter;" && name != "Landroid/text/Hyphenator;" &&
                name != "Landroid/view/MotionEvent;" && name != "Landroid/view/KeyEvent;")
                continue;
            const auto* type = art_boot ? &linker.Resolve(name) : linker.FindLinkedClass(name);
            for (const auto& method : art::ClassAccessor(*dex, definition).GetMethods()) {
                if (!(method.GetAccessFlags() & art::kAccNative)) continue;
                ++declarations;
                const auto& id = dex->GetMethodId(method.GetIndex());
                const auto method_name = dex->GetMethodNameView(id);
                const auto signature = dex->GetMethodSignature(id).ToString();
                if (type) {
                    const auto found = type->methods.find(wa::interp::MemberKey(method_name, signature));
                    if (found != type->methods.end() && (found->second.native || found->second.builtin))
                        continue;
                }
                missing += std::string(name) + "->" + std::string(method_name) + signature + '\n';
            }
        }
    }
    Require(primary_methods > 0 && primary_methods <= 65'535,
            "Primary framework DEX method IDs " + std::to_string(primary_methods));
    Require(declarations != 0, "The upstream DEX declares native methods");
    Require(missing.empty(), "Unregistered upstream JNI declarations\n" + missing);
    std::cout << "Audited " << declarations << " upstream framework JNI declarations, 0 unbound native methods, "
              << "0 duplicate DEX classes, " << primary_methods << " primary DEX method IDs\n";
}

void VerifyMediaStoreImages(Vm& vm, std::string_view upstream) {
    constexpr auto images = "Landroid/provider/MediaStore$Images$Media;";
    constexpr auto uri_type = "Landroid/net/Uri;";
    constexpr auto string_type = "Ljava/lang/String;";
    Require(vm.DexLocation(images).starts_with(upstream), "MediaStore images are in the upstream framework archive");
    wa::interp::ScopedHandleScope roots(vm);
    const auto uri_string = [&](Value value) {
        auto* uri = roots.Hold(value.ref());
        return vm.StringUtf8(vm.CallVirtual(uri, "toString", "()Ljava/lang/String;").ref());
    };
    Require(uri_string(vm.ReadStaticField(images, "EXTERNAL_CONTENT_URI", uri_type)) ==
                "content://media/external/images/media" &&
            uri_string(vm.ReadStaticField(images, "INTERNAL_CONTENT_URI", uri_type)) ==
                "content://media/internal/images/media", "MediaStore's image collection URIs");
    auto* volume = roots.Hold(vm.NewString("external_primary"));
    Require(uri_string(vm.CallStatic(images, "getContentUri", "(Ljava/lang/String;J)Landroid/net/Uri;",
                std::array{Value::Ref(volume), Value::Long(42)})) ==
                "content://media/external_primary/images/media/42", "MediaStore's image row URI");
    const auto column = [&](std::string_view name) {
        return vm.StringUtf8(vm.ReadStaticField(images, name, string_type).ref());
    };
    Require(column("_ID") == "_id" && column("DISPLAY_NAME") == "_display_name" &&
            column("BUCKET_DISPLAY_NAME") == "bucket_display_name", "MediaStore's inherited image columns");
    std::cout << "Verified upstream MediaStore image URIs and columns\n";
}

void VerifyLegacyHttp(Vm& vm, std::string_view upstream) {
    constexpr auto client_type = "Lorg/apache/http/impl/client/DefaultHttpClient;";
    Require(vm.DexLocation(client_type).starts_with(upstream), "Apache HTTP Legacy is in the primary framework archive");
    wa::interp::ScopedHandleScope roots(vm);
    auto* client = roots.Hold(vm.NewObject(client_type));
    const auto url = roots.Hold(Value::Ref(vm.NewString("https://example.invalid/hello%20%E4%BD%A0")));
    auto* post = roots.Hold(vm.NewObject("Lorg/apache/http/client/methods/HttpPost;",
        "(Ljava/lang/String;)V", std::array{url}));
    auto* uri = roots.Hold(vm.CallVirtual(post, "getURI", "()Ljava/net/URI;").ref());
    Require(vm.StringUtf8(vm.CallVirtual(uri, "getPath", "()Ljava/lang/String;").ref()) == "/hello 你",
            "Apache HttpPost decodes percent-encoded UTF-8 using libcore URI");
    try {
        vm.CallVirtual(client, "execute",
            "(Lorg/apache/http/client/methods/HttpUriRequest;)Lorg/apache/http/HttpResponse;",
            std::array{Value::Ref(post)});
    } catch (const wa::interp::ManagedException&) {
        const auto failure = vm.ExceptionSummary();
        vm.ClearException();
        Require(failure == "Ljava/net/UnknownHostException;: example.invalid", failure);
        return;
    }
    throw std::runtime_error("Apache HTTP must report unavailable guest DNS through IOException");
}

std::vector<uint8_t> ExtractDex(const char* apk) {
    ZipArchiveHandle archive = nullptr;
    Require(OpenArchive(apk, &archive) == 0, "Open smoke APK");
    std::unique_ptr<ZipArchive, decltype(&CloseArchive)> owner(archive, CloseArchive);
    ZipEntry64 entry{};
    Require(FindEntry(archive, "classes.dex", &entry) == 0, "APK contains compiled app DEX");
    std::vector<uint8_t> dex(entry.uncompressed_length);
    Require(ExtractToMemory(archive, &entry, dex.data(), dex.size()) == 0,
            "Extract APK classes.dex");
    return dex;
}

class Packet {
  public:
    explicit Packet(wa_sf_t* sf) : sf_(sf), frame_(wa_sf_frame_acquire(sf)) {
        Require(frame_ != nullptr, "SurfaceFlinger published a frame");
    }
    ~Packet() { Release(); }
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
    const wa_sf_frame_packet_t& operator*() const { return *frame_; }
    const wa_sf_frame_packet_t* operator->() const { return frame_; }
    void Release() {
        if (frame_) {
            wa_sf_frame_release(sf_, frame_);
            frame_ = nullptr;
        }
    }

  private:
    wa_sf_t* sf_;
    const wa_sf_frame_packet_t* frame_;
};

struct DisplayList {
    std::array<uint32_t, WA_CANVAS_DRAW_TEXT + 1> counts{};
    std::array<uint32_t, WA_PATH_CLOSE + 1> verbs{};
    uint32_t bytes = 0;
    uint32_t commands = 0;
    uint32_t sequence = 0;
    uint32_t layers = 0;
    float min_translation_x = 0;
    bool rounded_card = false;
    bool window_background = false;
    bool adaptive_mask = false;
    bool stroke_path = false;
    bool even_odd = false;
    bool authored_bitmap = false;
    bool resource_bitmap = false;
    unsigned shader_bitmaps = 0;
    std::vector<std::string> text;

    bool HasText(std::string_view value) const {
        return std::ranges::find(text, value) != text.end();
    }
};

template <class Record> Record Read(const uint8_t* data) {
    Record result;
    std::memcpy(&result, data, sizeof(result));
    return result;
}

DisplayList Inspect(wa_sf_t* sf, const Packet& packet) {
    Require(packet->magic == WA_SF_FRAME_MAGIC && packet->abi_version == WA_SF_ABI_VERSION,
            "Frame ABI is valid");
    Require(packet->viewport_w == kWidth && packet->viewport_h == kHeight, "Frame viewport");
    Require(packet->layers_offset >= sizeof(*packet) && packet->layer_count <= WA_SF_MAX_LAYERS &&
                packet->payload_offset >=
                    packet->layers_offset + packet->layer_count * sizeof(wa_sf_frame_layer_t) &&
                packet->byte_size == packet->payload_offset + packet->payload_size,
            "Frame header and payload bounds");
    const auto* layers = wa_sf_frame_layers(&*packet);
    const wa_sf_frame_layer_t* layer = nullptr;
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        if (layers[i].desc.layer_id == kLayer)
            layer = &layers[i];
    }
    Require(layer && layer->desc.width == kWidth && layer->desc.height == kHeight &&
                layer->desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST,
            "Java window uses the bound display-list layer");
    Require(layer->list_size > 0 && layer->list_offset >= packet->payload_offset &&
                layer->list_offset <= packet->byte_size &&
                layer->list_size <= packet->byte_size - layer->list_offset &&
                layer->list_generation > 0,
            "Display-list packet bounds");

    DisplayList stats;
    stats.bytes = layer->list_size;
    stats.sequence = packet->sequence;
    stats.layers = packet->layer_count;
    const uint8_t* bytes = wa_sf_frame_list(&*packet, layer);
    uint32_t save_depth = 0;
    for (size_t at = 0; at < layer->list_size;) {
        Require(layer->list_size - at >= sizeof(wa_canvas_record_t), "Complete record header");
        auto record = Read<wa_canvas_record_t>(bytes + at);
        Require(record.opcode > 0 && record.opcode < stats.counts.size() &&
                    record.byte_size >= sizeof(record) && record.byte_size % 4 == 0 &&
                    record.byte_size <= layer->list_size - at,
                "Record has a known opcode and fits the packet");
        stats.counts[record.opcode]++;
        stats.commands++;
        const uint8_t* body = bytes + at + sizeof(record);
        const size_t body_size = record.byte_size - sizeof(record);

        switch (record.opcode) {
        case WA_CANVAS_SAVE:
            ++save_depth;
            break;
        case WA_CANVAS_RESTORE:
            Require(save_depth != 0, "Canvas restore is balanced");
            --save_depth;
            break;
        case WA_CANVAS_TRANSLATE: {
            Require(body_size == sizeof(wa_canvas_xy_t), "Translate payload");
            auto translate = Read<wa_canvas_xy_t>(body);
            stats.min_translation_x = std::min(stats.min_translation_x, translate.x);
            break;
        }
        case WA_CANVAS_DRAW_RECT: {
            Require(body_size == sizeof(wa_canvas_rect_draw_t), "Rectangle payload");
            auto draw = Read<wa_canvas_rect_draw_t>(body);
            stats.window_background |= draw.paint.argb == 0xff10223c &&
                    draw.paint.style == WA_PAINT_FILL && draw.rect.l == 0 && draw.rect.t == 0 &&
                    draw.rect.r == kWidth && draw.rect.b == kHeight;
            break;
        }
        case WA_CANVAS_DRAW_ROUND_RECT: {
            Require(body_size == sizeof(wa_canvas_round_draw_t), "Rounded shape payload");
            auto shape = Read<wa_canvas_round_draw_t>(body);
            stats.rounded_card |= shape.paint.argb == 0xff29466b && shape.shape.rx == 21 &&
                                  shape.shape.rect.r > shape.shape.rect.l &&
                                  shape.shape.rect.b > shape.shape.rect.t;
            break;
        }
        case WA_CANVAS_CLIP_ROUND_RECT: {
            Require(body_size == sizeof(wa_round_rect_t), "Rounded clip payload");
            break;
        }
        case WA_CANVAS_DRAW_PATH: {
            Require(body_size >= sizeof(wa_canvas_path_t), "Path header payload");
            auto path = Read<wa_canvas_path_t>(body);
            stats.stroke_path |= path.paint.style == WA_PAINT_STROKE && path.paint.stroke_width > 0;
            stats.even_odd |= path.fill_rule == WA_PATH_EVEN_ODD;
            size_t position = sizeof(path);
            constexpr std::array<size_t, 6> coords{0, 2, 2, 4, 6, 0};
            for (uint32_t i = 0; i < path.verb_count; ++i) {
                Require(body_size - position >= sizeof(uint32_t), "Path verb fits");
                uint32_t verb = Read<uint32_t>(body + position);
                position += sizeof(verb);
                Require(verb > 0 && verb < coords.size() && (i != 0 || verb == WA_PATH_MOVE),
                        "Path verb type");
                Require(coords[verb] * sizeof(float) <= body_size - position,
                        "Path coordinates fit");
                stats.verbs[verb]++;
                for (size_t point = 0; point < coords[verb]; ++point) {
                    Require(std::isfinite(Read<float>(body + position)), "Finite path coordinates");
                    position += sizeof(float);
                }
            }
            Require(position == body_size, "Complete serialized path");
            break;
        }
        case WA_CANVAS_DRAW_BITMAP: {
            Require(body_size == sizeof(wa_canvas_bitmap_t), "Bitmap payload");
            auto draw = Read<wa_canvas_bitmap_t>(body);
            auto* store = wa_sf_bitmaps(sf);
            auto* view = wa_bitmap_acquire(store, draw.bitmap_id);
            Require(view != nullptr, "Published bitmap is available to frame consumers");
            const uint8_t* pixels = wa_bitmap_pixels(view);
            if (view->width == 2 && view->height == 2) {
                constexpr std::array<uint8_t, 8> expected{0x26, 0x71, 0xc4, 0xff,
                                                          0xe9, 0x49, 0x50, 0xff};
                stats.authored_bitmap = std::ranges::equal(expected, std::span(pixels, 8));
            } else if (view->width == 3 && view->height == 2) {
                constexpr std::array<uint8_t, 4> left{0x66, 0xcc, 0x33, 0xff};
                constexpr std::array<uint8_t, 4> right{0x33, 0x66, 0x99, 0x80};
                stats.resource_bitmap = true;
                for (uint32_t y = 0; y < view->height; ++y) {
                    const auto* row = pixels + y * view->stride;
                    stats.resource_bitmap &= std::ranges::equal(left, std::span(row, 4)) &&
                            std::ranges::equal(right, std::span(row + 8, 4));
                }
            } else if (view->width == view->height && view->width > 2) {
                const auto center = view->height / 2 * view->stride + view->width / 2 * 4;
                stats.adaptive_mask |= pixels[3] < 255 && pixels[center + 3] == 255;
            }
            if (view->width >= 16 && view->width <= 26 && view->height >= 16 && view->height <= 26) {
                bool blue = false, orange = false, transparent = false;
                for (uint32_t y = 0; y < view->height; ++y) {
                    for (uint32_t x = 0; x < view->width; ++x) {
                        const auto* p = pixels + y * view->stride + x * 4;
                        blue |= p[0] == 0x21 && p[1] == 0x63 && p[2] == 0xce && p[3] == 128;
                        orange |= p[0] == 0xdf && p[1] == 0x97 && p[2] == 0x31 && p[3] == 128;
                        transparent |= p[3] == 0;
                    }
                }
                stats.shader_bitmaps += blue && orange && transparent;
            }
            wa_bitmap_release(store, view);
            break;
        }
        case WA_CANVAS_DRAW_TEXT: {
            Require(body_size >= sizeof(wa_canvas_text_t), "Text payload");
            auto text = Read<wa_canvas_text_t>(body);
            Require(text.utf8_offset == sizeof(record) + sizeof(text) &&
                        text.utf8_len <= record.byte_size - text.utf8_offset &&
                        text.font_size > 0 && std::isfinite(text.y) && text.y > 0,
                    "Text bytes and measured baseline fit the record");
            stats.text.emplace_back(reinterpret_cast<const char*>(bytes + at + text.utf8_offset),
                                    text.utf8_len);
            break;
        }
        }
        at += record.byte_size;
    }
    Require(save_depth == 0, "Canvas stack is balanced at commit");
    return stats;
}

int ReadInt(Vm& vm, const char* method) {
    return vm.CallStatic(kActivity, method, "()I").i();
}
bool ReadBool(Vm& vm, const char* method) {
    return vm.CallStatic(kActivity, method, "()Z").i();
}

std::string ReadString(Vm& vm, const char* method) {
    return vm.StringUtf8(vm.CallStatic(kActivity, method, "()Ljava/lang/String;").ref());
}

void Push(wa_sf_t* sf, const wa_input_event_t& event) {
    Require(wa_input_push(wa_sf_input(sf), &event) == WA_SF_OK,
            "Input queued in the compositor ring");
}

void Motion(wa_sf_t* sf, uint32_t action, double time, float x, float y) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_MOTION;
    event.action = action;
    event.pointer_id = 3;
    event.event_time_ms = time;
    event.x = x;
    event.y = y;
    Push(sf, event);
}

void Key(wa_sf_t* sf, uint32_t action, double time, uint32_t code) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_KEY;
    event.action = action;
    event.keycode = code;
    event.event_time_ms = time;
    Push(sf, event);
}

void Ime(wa_sf_t* sf, uint32_t action, uint32_t flags, double time, std::string_view text) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_IME;
    event.action = action;
    event.flags = flags;
    event.event_time_ms = time;
    Require(text.size() <= sizeof(event.text), "IME text fits a ring record");
    event.text_len = text.size();
    std::memcpy(event.text, text.data(), text.size());
    Push(sf, event);
}

void PrintList(std::string_view label, const DisplayList& frame) {
    std::cout << label << " seq=" << frame.sequence << " layers=" << frame.layers
              << " bytes=" << frame.bytes << " commands=" << frame.commands
              << " round_rect=" << frame.counts[WA_CANVAS_DRAW_ROUND_RECT]
              << " path=" << frame.counts[WA_CANVAS_DRAW_PATH]
              << " text=" << frame.counts[WA_CANVAS_DRAW_TEXT]
              << " bitmap=" << frame.counts[WA_CANVAS_DRAW_BITMAP]
              << " round_clip=" << frame.counts[WA_CANVAS_CLIP_ROUND_RECT] << '\n';
}

void VerifyVmRuntime(Vm& vm) {
    constexpr auto owner = "Ldalvik/system/VMRuntime;";
    constexpr auto runtime_type = "()Ldalvik/system/VMRuntime;";
    wa::interp::ScopedHandleScope scope(vm);
    auto* runtime = scope.Hold(vm.CallStatic(owner, "getRuntime", runtime_type).ref());
    Require(runtime && runtime == vm.CallStatic(owner, "getRuntime", runtime_type).ref(), "VMRuntime singleton");
    Require(vm.CallVirtual(runtime, "is64Bit", "()Z").i() &&
            vm.StringUtf8(vm.CallVirtual(runtime, "vmInstructionSet", "()Ljava/lang/String;").ref()) == "arm64",
            "VMRuntime reports the Android guest architecture");
    const auto previous = vm.CallVirtual(runtime, "getTargetSdkVersion", "()I");
    vm.SetAndroidTargetSdkVersion(34);
    Require(vm.CallVirtual(runtime, "getTargetSdkVersion", "()I").i() == 34, "VMRuntime sees the manifest target SDK");
    vm.CallVirtual(runtime, "setTargetSdkVersion", "(I)V", std::array{Value::Int(31)});
    Require(vm.CallVirtual(runtime, "getTargetSdkVersion", "()I").i() == 31, "VMRuntime updates the target SDK");
    vm.CallVirtual(runtime, "setTargetSdkVersion", "(I)V", std::array{previous});

    const auto component = vm.ReadStaticField("Ljava/lang/Integer;", "TYPE", "Ljava/lang/Class;");
    const auto array = scope.Hold(vm.CallVirtual(runtime, "newNonMovableArray", "(Ljava/lang/Class;I)Ljava/lang/Object;",
        std::array{component, Value::Int(2)}));
    const auto address_of = [&] {
        return vm.CallVirtual(runtime, "addressOf", "(Ljava/lang/Object;)J", std::array{array}).j();
    };
    const auto address = address_of();
    Require(address != 0, "Non-movable primitive array has an address");
    auto* env = vm.JniEnv();
    auto& jni = wa::interp::JniBridge::From(env);
    auto ref = static_cast<jintArray>(jni.Local(array.ref()));
    jint written = 0x12345678, read = 0;
    env->SetIntArrayRegion(ref, 1, 1, &written);
    auto* data = reinterpret_cast<jint*>(static_cast<uintptr_t>(address));
    Require(data[1] == written && address_of() == address, "JNI writes use stable non-movable array storage");
    data[0] = 37;
    env->GetIntArrayRegion(ref, 0, 1, &read);
    Require(read == 37, "JNI reads reflect writes to the backing storage");
    jni.DeleteLocal(ref);
    auto* prefixes = scope.Hold(jni.Vm().NewArray(jni.Vm().linker.Resolve("[Ljava/lang/String;"), 0));
    vm.CallVirtual(runtime, "setHiddenApiExemptions", "([Ljava/lang/String;)V", std::array{Value::Ref(prefixes)});
}

void Run(Vm& vm, const char* upstream, const char* framework, const char* apk, const char* system_resources) {
    vm.LoadDexFile(upstream);
    vm.LoadFallbackDexFile(framework);
    vm.LoadDexFile(std::string(apk) + "!classes.dex", ExtractDex(apk));
    vm.SetSystemProperty("org.wasmandroid.foreground", "true");
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf(wa_sf_create(), wa_sf_destroy);
    Require(sf != nullptr, "SurfaceFlinger allocation");
    wa::ui_jni::UiBridge bridge;
    bridge.BindSurfaceFlinger(sf.get(), kLayer);
    bridge.RegisterNatives(vm);
    AuditFrameworkNatives(vm, upstream, framework);
    VerifyMediaStoreImages(vm, upstream);
    VerifyLegacyHttp(vm, upstream);
    VerifyVmRuntime(vm);
    {
        wa::interp::ScopedHandleScope scope(vm);
        auto* local = scope.Hold(vm.NewObject("Ljava/lang/ThreadLocal;", "()V"));
        Require(!vm.CallVirtual(local, "get", "()Ljava/lang/Object;").ref(),
                "ThreadLocal initializes the current thread before its first use");
    }
    const std::array<uint8_t, 4> mounted{0x10, 0x23, 0x45, 0x67};
    vm.MountReadOnlyFile("/data/mounted.bin", mounted);
    Require(ReadInt(vm, "verifyFileStreams") == 1, "Guest stream and read-only file operations");
    Require(ReadInt(vm, "verifyReadWriteLock") == 1, "Cooperative locks preserve hold counts and park permits");
    Require(ReadInt(vm, "verifyThreadStack") == 1, "Thread stack traces report the active guest frames");
    Require(ReadInt(vm, "verifyHandlerThread") == 1, "HandlerThread signals a waiting caller and resumes its callback");
    Require(ReadInt(vm, "verifyReferenceQueue") == 1, "ReferenceQueue wakes its removal consumer and preserves FIFO state");
    Require(ReadInt(vm, "verifyForkJoin") == 1, "ForkJoin workers join, complete, and compensate for blocking");
    Require(ReadInt(vm, "verifyLocalSockets") == 1, "Guest local sockets bind, transfer streams, and release addresses");
    Require(ReadInt(vm, "verifyCrc32") == 1, "Upstream CRC32 handles single bytes and read-only buffers");
    Require(ReadInt(vm, "verifyDeflater") == 1, "Upstream Deflater completes a zlib stream in partial writes");
    Require(ReadInt(vm, "verifyRegex") == 1, "Upstream regex captures and Unicode classes use the guest backend");
    Require(ReadInt(vm, "verifyLegacyTime") == 1, "Android Time round-trips a fixed-offset timestamp");
    Require(ReadInt(vm, "verifyGenericSuperclass") == 1, "Class reflection parses DEX generic superclass signatures");
    Require(ReadInt(vm, "verifyConcurrentKeySet") == 1, "ConcurrentHashMap exposes a mutable backed key set");
    Require(ReadInt(vm, "verifyCopiesList") == 1, "Collections.nCopies keeps an immutable repeated-value view");
    Require(ReadInt(vm, "verifyPriorityQueue") == 1, "PriorityQueue grows while preserving comparator order");
    Require(ReadInt(vm, "verifyUtf32") == 1, "UTF-32 preserves supplementary characters in both byte orders");
    Require(ReadInt(vm, "verifyTlsProviderUnavailable") == 1, "Java TLS reports an unavailable trust provider");
    Require(ReadInt(vm, "verifyWebp") == 1, "BitmapFactory decodes WebP bounds, color and transparency");
    Require(ReadInt(vm, "verifyNioPaths") == 1, "NIO paths and directory operations share the guest file system");
    Require(ReadInt(vm, "verifyOsStat") == 1, "Android stat reports guest file metadata and typed path errors");
    Require(ReadInt(vm, "verifyPicture") == 1, "Picture records, copies, and scales real canvas output");
    Require(ReadInt(vm, "verifyVirtualKeyboard") == 1, "The virtual keyboard exposes its AOSP layout and produces key events");
    Require(ReadInt(vm, "verifyFieldReflection") == 1, "Field reflection reads DEX metadata and respects final fields");
    vm.SetAndroidTargetSdkVersion(34);
    Require(ReadInt(vm, "verifyLegacyFieldReflection") == 1, "Legacy Android apps can reflectively update static final fields");
    vm.SetAndroidTargetSdkVersion(0);
    Require(ReadInt(vm, "verifyBigInteger") == 1, "BigInteger performs pure Java multiplication, division and modular exponentiation");
    Require(ReadInt(vm, "verifyDecimalRounding") == 1, "BigDecimal rounds compact decimal values");
    Require(ReadInt(vm, "verifyArrayListHierarchy") == 1, "ArrayList dispatches through the libcore AbstractCollection hierarchy");
    Require(ReadInt(vm, "verifySlashWrapping") == 1, "Text wraps at the slash in compound labels");
    Require(ReadInt(vm, "verifyEmbeddedDom") == 1, "Java DOM parses XML streams and reports malformed input with a location");
    Require(ReadInt(vm, "verifyDigest") == 1, "Guest digest API matches SHA-1, MD5, and SHA-256 test vectors");
    Require(ReadInt(vm, "verifyJson") == 1, "Upstream JSON serializes and reads nested values");
    Require(ReadInt(vm, "verifyEmbeddedUrl") == 1, "URL resolves paths and reports unavailable transports as I/O errors");

    const auto icon = static_cast<uint32_t>(ReadInt(vm, "iconResource"));
    unsigned queried = 0, applications = 0, launches = 0;
    bool user_forwarded = false;
    PackageActivity notes{"org.wasmandroid.notes", "org.wasmandroid.notes.MainActivity", "Notes",
                          apk, icon};
    bridge.SetPackageCallbacks({
        .query_activities =
            [&](const IntentRequest& request) {
                ++queried;
                user_forwarded |= request.user_id == 10;
                Require(
                    request.action == "android.intent.action.MAIN" &&
                        std::ranges::find(request.categories, "android.intent.category.LAUNCHER") !=
                            request.categories.end(),
                    "Package query forwards Android action and categories");
                return std::vector{notes};
            },
        .get_application = [&](std::string_view name, uint64_t flags,
                               int32_t user) -> std::optional<PackageActivity> {
            ++applications;
            Require(name == notes.package_name && flags == 0 && (user == 0 || user == 10),
                    "Application info lookup");
            return notes;
        },
        .start_activity =
            [&](const IntentRequest& request) {
                ++launches;
                Require(request.package_name == notes.package_name &&
                            request.activity_name == notes.activity_name &&
                            (request.intent_flags & 0x10000000) != 0,
                        "Activity launch reaches the host with explicit component and flags");
                return true;
            },
    });

    uint32_t frame_number = 0;
    auto frame = [&](uint64_t time_ms, uint32_t dpi = kDpi) {
        wa_sf_vsync_state_t tick{time_ms * 1'000'000, ++frame_number, kWidth, kHeight, dpi};
        wa_sf_vsync_write(wa_sf_vsync(sf.get()), &tick);
        return bridge.DoFrame(vm);
    };

    std::cout << "Launching compiled SmokeActivity\n";
    bridge.LaunchActivity(vm, {.apk_path = apk,
                               .locale = "en-US",
                               .density_dpi = kDpi,
                               .width = kWidth,
                               .height = kHeight,
                               .framework_resource_apk = system_resources});
    Require(ReadInt(vm, "lifecycle") == 123, "Launch ran onCreate/onStart/onResume");
    Require(ReadInt(vm, "verifyResultReceiver") == 1, "Parcel ResultReceiver delivers results on its Handler");
    Require(ReadInt(vm, "verifyRunningTask") == 1, "ActivityManager reports the current embedded task");
    Require(ReadInt(vm, "verifyProviderQuery") == 1, "ContentResolver queries installed providers in its user");
    Require(ReadInt(vm, "verifyManifestServiceProcess") == 1, "Only services assigned to the current process start locally");
    Require(ReadBool(vm, "resourcesVerified"),
            "Compiled resources, styles, included layout, and merge inflated");
    Require(ReadBool(vm, "packagesVerified") && user_forwarded && queried == 2 && applications >= 1,
            "PackageManager and LauncherApps query the host and load icon resources");
    vm.CallStatic("Lorg/wasmandroid/uismoke/NativeGraphicsProbe;", "verify", "()V");

    wa_font_metric_t& font =
        wa_sf_font_metrics(sf.get())[WA_FONT_ROBOTO * WA_FONT_WEIGHT_COUNT + 6];
    __atomic_store_n(&font.sequence, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&font.latin_q16, 49152u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.wide_q16, 65536u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.ascent_q16, 49152u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.descent_q16, 16384u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.sequence, 2u, __ATOMIC_RELEASE);
    const float heading_width = vm.CallStatic(kActivity, "headingAdvance", "()F").f();
    Require(heading_width >= 40.5f && heading_width <= 41.f &&
                vm.CallStatic(kActivity, "headingAscent", "()F").f() == -20.25f,
            "Paint measures text with SurfaceFlinger-published font metrics");

    auto& widths = wa_sf_font_advances(sf.get())[WA_FONT_ROBOTO * WA_FONT_WEIGHT_COUNT + 6];
    std::ranges::fill(widths.ascii_q16, 32768u);
    widths.ascii_q16['M' - WA_FONT_ASCII_FIRST] = 58983;
    widths.ascii_q16['W' - WA_FONT_ASCII_FIRST] = 62260;
    widths.ascii_q16['m' - WA_FONT_ASCII_FIRST] = 62260;
    widths.ascii_q16['l' - WA_FONT_ASCII_FIRST] = 19661;
    widths.ascii_q16[' ' - WA_FONT_ASCII_FIRST] = 16384;
    widths.wide_q16 = 65536;
    __atomic_store_n(&widths.sequence, 2u, __ATOMIC_RELEASE);
    struct Title { const char* text; float width_at_16px; };
    for (const auto& title : {Title{"Apps", 32}, Title{"Modes", 46.4f}, Title{"Communal", 75.2f},
                             Title{"WasmAndroid 3D Cube", 158.4f}, Title{"系统设置", 64}}) {
        const Value text[] = {Value::Ref(vm.NewString(title.text))};
        const float measured = vm.CallStatic(kActivity, "titleAdvance", "(Ljava/lang/String;)F", text).f();
        const float actual = title.width_at_16px * 27 / 16;
        Require(measured >= actual && measured <= actual + 2, std::string("Calibrated Paint width for ") + title.text);
    }
    __atomic_store_n(&widths.sequence, 0u, __ATOMIC_RELEASE);

    Require(frame(990) == WA_SF_OK, "Startup screen commits before browser calibration");
    const auto text_measures = [&] {
        return vm.GetInstanceMethodStats("Landroid/widget/TextView;", "onMeasure", "(II)V").invocations;
    };
    const auto startup_measures = text_measures();
    auto* advances = wa_sf_font_advances(sf.get());
    __atomic_store_n(&widths.sequence, 2u, __ATOMIC_RELEASE);
    __atomic_store_n(&advances[WA_FONT_FACE_COUNT * WA_FONT_WEIGHT_COUNT - 1].sequence,
                     2u, __ATOMIC_RELEASE);
    Require(frame(1000) == WA_SF_OK && text_measures() > startup_measures,
            "The first browser frame remeasures the startup screen after font calibration");
    Packet first(sf.get());
    auto initial = Inspect(sf.get(), first);
    PrintList("initial", initial);
    Require(first->density_dpi == kDpi && ReadInt(vm, "tileWidth") > 150 &&
                ReadInt(vm, "tileHeight") == 162,
            "Measured custom view bounds respect XML margins, padding and density");
    Require(ReadInt(vm, "handlerValue") == 7, "Handler dispatched delayed Message");
    Require(static_cast<uint32_t>(ReadInt(vm, "swatchPixel")) == 0xffe94950u,
            "Reading a presented bitmap through the SurfaceFlinger store preserves ARGB");
    Require(initial.rounded_card, "The rounded card uses its authored paint and radius");
    Require(initial.adaptive_mask, "The adaptive icon has transparent corners and an opaque center");
    Require(initial.stroke_path && initial.even_odd &&
                initial.verbs[WA_PATH_CUBIC] > 0 && initial.verbs[WA_PATH_QUAD] > 0 &&
                initial.verbs[WA_PATH_CLOSE] > 0,
            "Vector paths preserve stroke, fill rule and curve verbs");
    Require(initial.authored_bitmap, "The programmatic bitmap preserves its authored pixels");
    Require(initial.shader_bitmaps == 3, "Recorded round rect, circle and path preserve shader pixels and masks");
    Require(initial.resource_bitmap, "The density-scaled PNG preserves its edge colors and alpha");
    Require(initial.HasText("Ready to launch") && initial.HasText("Launcher tile"),
            "The display list contains both fixture labels");
    Require(initial.window_background, "The window records its full-size themed background");

    const float x = ReadInt(vm, "tileX"), y = ReadInt(vm, "tileY") + 25;
    Motion(sf.get(), WA_ACTION_DOWN, 1005, x + 45, y);
    Motion(sf.get(), WA_ACTION_UP, 1010, x + 45, y);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 2 && ReadInt(vm, "clickCount") == 1 &&
                launches == 1,
            "MotionEvent tap invoked OnClickListener");
    Require(frame(1016) == WA_SF_OK, "Tap publishes the next display list");
    {
        Packet second(sf.get());
        auto clicked = Inspect(sf.get(), second);
        PrintList("tap", clicked);
        Require(clicked.HasText("Tile clicked") && clicked.sequence == initial.sequence + 1,
                "Tap changes TextView on the next frame");
    }
    Require(frame(1032) == WA_SF_BUSY, "A reader holding the previous packet delays atomic commit");
    {
        Packet published(sf.get());
        Require(published->sequence == initial.sequence + 1,
                "An incomplete transaction is never exposed");
    }
    first.Release();
    Require(frame(1032) == WA_SF_OK, "Busy commit resumes without replaying Java traversal");
    Require(ReadInt(vm, "animationUpdates") >= 3,
            "ValueAnimator drove updates on Choreographer ticks");

    Key(sf.get(), WA_ACTION_DOWN, 1033, 22);
    Key(sf.get(), WA_ACTION_UP, 1034, 22);
    Ime(sf.get(), WA_IME_COMPOSING, 0, 1035, "输入");
    Ime(sf.get(), WA_IME_COMMIT, WA_IME_MORE, 1036, "输入");
    Ime(sf.get(), WA_IME_COMMIT, WA_IME_CONTINUATION, 1037, " ✓");
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 5 && ReadInt(vm, "keyCount") == 1 &&
                ReadString(vm, "composed") == "输入" && ReadString(vm, "committed") == "输入 ✓",
            "Key and multipart IME events reach the focused Java View");

    Motion(sf.get(), WA_ACTION_DOWN, 1050, x + 170, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1060, x + 137, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1070, x + 104, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1080, x + 71, y);
    Motion(sf.get(), WA_ACTION_UP, 1090, x + 38, y);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 5 && ReadInt(vm, "motionEvents") == 7 &&
                ReadBool(vm, "flinging") && ReadInt(vm, "clickCount") == 1,
            "MotionEvent drag releases into an upstream OverScroller fling");
    const int released = ReadInt(vm, "scroll");
    std::vector<int> offsets{released};
    DisplayList scrolled;
    for (uint64_t millis : {1106, 1122, 1138, 1154, 1170, 1186}) {
        Require(frame(millis) == WA_SF_OK, "Fling frame commits");
        offsets.push_back(ReadInt(vm, "scroll"));
        Packet published(sf.get());
        scrolled = Inspect(sf.get(), published);
    }
    Require(std::ranges::adjacent_find(offsets, std::greater_equal{}) == offsets.end() &&
                scrolled.min_translation_x == -offsets.back(),
            "Multiple Choreographer ticks advance the actual display-list scroll");
    PrintList("fling", scrolled);
    std::cout << "fling scroll px";
    for (int px : offsets)
        std::cout << ' ' << px;
    std::cout << '\n';

    const auto before = vm.HeapStats();
    const auto collected = vm.Collect();
    Require(collected.collections == before.collections + 1 && collected.live > 0 &&
                collected.reclaimed > 0,
            "Exact GC reclaimed objects while the Activity remained rooted");
    Require(frame(1202) == WA_SF_OK && ReadInt(vm, "clickCount") == 1,
            "Views survive exact GC and render again");
    {
        Packet after_gc(sf.get());
        Require(Inspect(sf.get(), after_gc).HasText("Tile clicked"),
                "Text and drawable references survive GC");
    }
    std::cout << "exact GC collections=" << collected.collections << " live=" << collected.live
              << " reclaimed=" << collected.reclaimed << '\n';

    bridge.PauseActivity(vm);
    bridge.StopActivity(vm);
    Require(ReadInt(vm, "lifecycle") == 12345, "Activity paused and stopped");
    bridge.ResumeActivity(vm);
    Require(ReadInt(vm, "lifecycle") == 12345723, "Activity restarted and resumed");
    Key(sf.get(), WA_ACTION_DOWN, 1205, 4);
    Key(sf.get(), WA_ACTION_UP, 1206, 4);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 2 && ReadInt(vm, "lifecycle") == 345723456,
            "Back dispatch finishes the Activity and runs lifecycle cleanup");
    Require(frame(1208) == WA_SF_OK, "Finished Activity clears its display layer");
    {
        Packet removed(sf.get());
        Require(removed->layer_count == 0 && removed->payload_size == 0,
                "Layer removal is published atomically");
    }

    bridge.LaunchActivity(vm, {.apk_path = apk,
                               .class_name = ".SmokeActivity",
                               .locale = "zh-rCN",
                               .density_dpi = 160,
                               .width = kWidth,
                               .height = kHeight,
                               .framework_resource_apk = system_resources});
    Require(ReadBool(vm, "resourcesVerified"), "Compiled resources verified at 160 dpi");
    Require(ReadBool(vm, "platformResources"),
            "Package 0x01 resolves through supplied platform assets");
    Require(frame(1220, 160) == WA_SF_OK, "Localized screen commits");
    {
        Packet chinese(sf.get());
        const auto localized = Inspect(sf.get(), chinese);
        PrintList("zh-CN", localized);
        Require(chinese->density_dpi == 160 && localized.HasText("准备启动") &&
                    localized.HasText("启动磁贴"),
                "AOSP resource selection resolves zh-rCN and records UTF-8 text");
    }
    Require(ReadInt(vm, "verifyScheduledExecutor") == 1, "Scheduled work runs through the guest Looper");
    Require(ReadInt(vm, "verifyScheduledWorkerWait") == 1, "A scheduled worker can wait for the main Looper");
    Require(ReadInt(vm, "verifyNestedLooper") == 1, "Nested main Looper resumes into its original exception handlers");
    Require(ReadInt(vm, "verifyEmbeddedPackage") == 1, "Embedded package exposes manifest version and mount source");
    Require(ReadInt(vm, "verifyExecutorAndRegisterShutdownHook") == 1, "Executor honors its rejection policy");
    bridge.DestroyActivity(vm);
    vm.RunShutdownHooks();
    Require(ReadBool(vm, "shutdownHookRan"), "Guest shutdown hooks run with their registered thread");
    vm.Collect();

    const uint64_t apk_calls = vm.NativeCalls("Landroid/content/res/ApkAssets;");
    const uint64_t xml_calls = vm.NativeCalls("Landroid/content/res/XmlBlock;");
    const uint64_t asset_calls = vm.NativeCalls("Landroid/content/res/AssetManager;");
    Require(apk_calls > 0 && xml_calls > 0 && asset_calls > 0, "Original AOSP resource JNI ran");
    std::cout << "AOSP native calls apk=" << apk_calls << " xml=" << xml_calls
              << " assets=" << asset_calls
              << " canvas=" << vm.NativeCalls("Landroid/graphics/Canvas;") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr
            << "Usage: test_aosp17_ui_framework UPSTREAM.dex CORE.dex APP.apk PLATFORM_RESOURCES.apk\n";
        return 2;
    }
    Vm vm({.gc_interval = 137, .capture_exception_stack = true});
    try {
        Run(vm, argv[1], argv[2], argv[3], argv[4]);
        return 0;
    } catch (const wa::interp::ManagedException&) {
        std::cerr << vm.ExceptionSummary() << '\n';
        for (const auto& frame : vm.ExceptionStackTrace()) std::cerr << frame << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
