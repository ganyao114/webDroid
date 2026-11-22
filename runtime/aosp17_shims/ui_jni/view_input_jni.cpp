// Browser input peers for the upstream android.view event classes.
#include "native_graphics.h"

#include <atomic>
#include <bit>
#include <deque>
#include <numbers>
#include <string_view>

namespace wa::ui_jni::detail {
namespace {

constexpr jint kDown = 0, kUp = 1, kMove = 2, kCancel = 3;
constexpr jint kPointerDown = 5, kPointerUp = 6;
constexpr jint kHoverMove = 7, kScroll = 8, kHoverEnter = 9;
constexpr jint kActionMask = 0xff, kPointerIndexShift = 8;
constexpr jint kSourceClassPointer = 2;
constexpr jint kCanceled = 0x20, kOrientationFlags = 0x180;
constexpr jint kHistoryCurrent = std::numeric_limits<jint>::min();
constexpr size_t kAxisCount = 64;
constexpr jint kMaxPointers = 32;
constexpr jint kOrientation = 8, kScrollAxis = 26, kRelativeX = 27, kRelativeY = 28;

jint NextInputId(JNIEnv*, jclass) {
    static std::atomic<uint32_t> sequence{0};
    // Match AOSP's id namespace for events created outside InputReader and InputDispatcher.
    return std::bit_cast<jint>((sequence.fetch_add(1, std::memory_order_relaxed) & 0x3fffffff) |
                               0xc0000000u);
}

// Symbolic labels from AOSP KeyEvent.
constexpr std::array<const char*, 341> kKeyLabels{
    "UNKNOWN",
    "SOFT_LEFT",
    "SOFT_RIGHT",
    "HOME",
    "BACK",
    "CALL",
    "ENDCALL",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "STAR",
    "POUND",
    "DPAD_UP",
    "DPAD_DOWN",
    "DPAD_LEFT",
    "DPAD_RIGHT",
    "DPAD_CENTER",
    "VOLUME_UP",
    "VOLUME_DOWN",
    "POWER",
    "CAMERA",
    "CLEAR",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "COMMA",
    "PERIOD",
    "ALT_LEFT",
    "ALT_RIGHT",
    "SHIFT_LEFT",
    "SHIFT_RIGHT",
    "TAB",
    "SPACE",
    "SYM",
    "EXPLORER",
    "ENVELOPE",
    "ENTER",
    "DEL",
    "GRAVE",
    "MINUS",
    "EQUALS",
    "LEFT_BRACKET",
    "RIGHT_BRACKET",
    "BACKSLASH",
    "SEMICOLON",
    "APOSTROPHE",
    "SLASH",
    "AT",
    "NUM",
    "HEADSETHOOK",
    "FOCUS",
    "PLUS",
    "MENU",
    "NOTIFICATION",
    "SEARCH",
    "MEDIA_PLAY_PAUSE",
    "MEDIA_STOP",
    "MEDIA_NEXT",
    "MEDIA_PREVIOUS",
    "MEDIA_REWIND",
    "MEDIA_FAST_FORWARD",
    "MUTE",
    "PAGE_UP",
    "PAGE_DOWN",
    "PICTSYMBOLS",
    "SWITCH_CHARSET",
    "BUTTON_A",
    "BUTTON_B",
    "BUTTON_C",
    "BUTTON_X",
    "BUTTON_Y",
    "BUTTON_Z",
    "BUTTON_L1",
    "BUTTON_R1",
    "BUTTON_L2",
    "BUTTON_R2",
    "BUTTON_THUMBL",
    "BUTTON_THUMBR",
    "BUTTON_START",
    "BUTTON_SELECT",
    "BUTTON_MODE",
    "ESCAPE",
    "FORWARD_DEL",
    "CTRL_LEFT",
    "CTRL_RIGHT",
    "CAPS_LOCK",
    "SCROLL_LOCK",
    "META_LEFT",
    "META_RIGHT",
    "FUNCTION",
    "SYSRQ",
    "BREAK",
    "MOVE_HOME",
    "MOVE_END",
    "INSERT",
    "FORWARD",
    "MEDIA_PLAY",
    "MEDIA_PAUSE",
    "MEDIA_CLOSE",
    "MEDIA_EJECT",
    "MEDIA_RECORD",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
    "NUM_LOCK",
    "NUMPAD_0",
    "NUMPAD_1",
    "NUMPAD_2",
    "NUMPAD_3",
    "NUMPAD_4",
    "NUMPAD_5",
    "NUMPAD_6",
    "NUMPAD_7",
    "NUMPAD_8",
    "NUMPAD_9",
    "NUMPAD_DIVIDE",
    "NUMPAD_MULTIPLY",
    "NUMPAD_SUBTRACT",
    "NUMPAD_ADD",
    "NUMPAD_DOT",
    "NUMPAD_COMMA",
    "NUMPAD_ENTER",
    "NUMPAD_EQUALS",
    "NUMPAD_LEFT_PAREN",
    "NUMPAD_RIGHT_PAREN",
    "VOLUME_MUTE",
    "INFO",
    "CHANNEL_UP",
    "CHANNEL_DOWN",
    "ZOOM_IN",
    "ZOOM_OUT",
    "TV",
    "WINDOW",
    "GUIDE",
    "DVR",
    "BOOKMARK",
    "CAPTIONS",
    "SETTINGS",
    "TV_POWER",
    "TV_INPUT",
    "STB_POWER",
    "STB_INPUT",
    "AVR_POWER",
    "AVR_INPUT",
    "PROG_RED",
    "PROG_GREEN",
    "PROG_YELLOW",
    "PROG_BLUE",
    "APP_SWITCH",
    "BUTTON_1",
    "BUTTON_2",
    "BUTTON_3",
    "BUTTON_4",
    "BUTTON_5",
    "BUTTON_6",
    "BUTTON_7",
    "BUTTON_8",
    "BUTTON_9",
    "BUTTON_10",
    "BUTTON_11",
    "BUTTON_12",
    "BUTTON_13",
    "BUTTON_14",
    "BUTTON_15",
    "BUTTON_16",
    "LANGUAGE_SWITCH",
    "MANNER_MODE",
    "3D_MODE",
    "CONTACTS",
    "CALENDAR",
    "MUSIC",
    "CALCULATOR",
    "ZENKAKU_HANKAKU",
    "EISU",
    "MUHENKAN",
    "HENKAN",
    "KATAKANA_HIRAGANA",
    "YEN",
    "RO",
    "KANA",
    "ASSIST",
    "BRIGHTNESS_DOWN",
    "BRIGHTNESS_UP",
    "MEDIA_AUDIO_TRACK",
    "SLEEP",
    "WAKEUP",
    "PAIRING",
    "MEDIA_TOP_MENU",
    "11",
    "12",
    "LAST_CHANNEL",
    "TV_DATA_SERVICE",
    "VOICE_ASSIST",
    "TV_RADIO_SERVICE",
    "TV_TELETEXT",
    "TV_NUMBER_ENTRY",
    "TV_TERRESTRIAL_ANALOG",
    "TV_TERRESTRIAL_DIGITAL",
    "TV_SATELLITE",
    "TV_SATELLITE_BS",
    "TV_SATELLITE_CS",
    "TV_SATELLITE_SERVICE",
    "TV_NETWORK",
    "TV_ANTENNA_CABLE",
    "TV_INPUT_HDMI_1",
    "TV_INPUT_HDMI_2",
    "TV_INPUT_HDMI_3",
    "TV_INPUT_HDMI_4",
    "TV_INPUT_COMPOSITE_1",
    "TV_INPUT_COMPOSITE_2",
    "TV_INPUT_COMPONENT_1",
    "TV_INPUT_COMPONENT_2",
    "TV_INPUT_VGA_1",
    "TV_AUDIO_DESCRIPTION",
    "TV_AUDIO_DESCRIPTION_MIX_UP",
    "TV_AUDIO_DESCRIPTION_MIX_DOWN",
    "TV_ZOOM_MODE",
    "TV_CONTENTS_MENU",
    "TV_MEDIA_CONTEXT_MENU",
    "TV_TIMER_PROGRAMMING",
    "HELP",
    "NAVIGATE_PREVIOUS",
    "NAVIGATE_NEXT",
    "NAVIGATE_IN",
    "NAVIGATE_OUT",
    "STEM_PRIMARY",
    "STEM_1",
    "STEM_2",
    "STEM_3",
    "DPAD_UP_LEFT",
    "DPAD_DOWN_LEFT",
    "DPAD_UP_RIGHT",
    "DPAD_DOWN_RIGHT",
    "MEDIA_SKIP_FORWARD",
    "MEDIA_SKIP_BACKWARD",
    "MEDIA_STEP_FORWARD",
    "MEDIA_STEP_BACKWARD",
    "SOFT_SLEEP",
    "CUT",
    "COPY",
    "PASTE",
    "SYSTEM_NAVIGATION_UP",
    "SYSTEM_NAVIGATION_DOWN",
    "SYSTEM_NAVIGATION_LEFT",
    "SYSTEM_NAVIGATION_RIGHT",
    "ALL_APPS",
    "REFRESH",
    "THUMBS_UP",
    "THUMBS_DOWN",
    "PROFILE_SWITCH",
    "VIDEO_APP_1",
    "VIDEO_APP_2",
    "VIDEO_APP_3",
    "VIDEO_APP_4",
    "VIDEO_APP_5",
    "VIDEO_APP_6",
    "VIDEO_APP_7",
    "VIDEO_APP_8",
    "FEATURED_APP_1",
    "FEATURED_APP_2",
    "FEATURED_APP_3",
    "FEATURED_APP_4",
    "DEMO_APP_1",
    "DEMO_APP_2",
    "DEMO_APP_3",
    "DEMO_APP_4",
    "KEYBOARD_BACKLIGHT_DOWN",
    "KEYBOARD_BACKLIGHT_UP",
    "KEYBOARD_BACKLIGHT_TOGGLE",
    "STYLUS_BUTTON_PRIMARY",
    "STYLUS_BUTTON_SECONDARY",
    "STYLUS_BUTTON_TERTIARY",
    "STYLUS_BUTTON_TAIL",
    "RECENT_APPS",
    "MACRO_1",
    "MACRO_2",
    "MACRO_3",
    "MACRO_4",
    "EMOJI_PICKER",
    "SCREENSHOT",
    "DICTATE",
    "NEW",
    "CLOSE",
    "DO_NOT_DISTURB",
    "PRINT",
    "LOCK",
    "FULLSCREEN",
    "F13",
    "F14",
    "F15",
    "F16",
    "F17",
    "F18",
    "F19",
    "F20",
    "F21",
    "F22",
    "F23",
    "F24",
    "ACCESSIBILITY",
    "CONTEXTUAL_SEARCH",
    "CONTEXTUAL_INSERT",
};

// Symbolic labels from AOSP MotionEvent.
constexpr std::array<const char*, 54> kAxisLabels{
    "X",
    "Y",
    "PRESSURE",
    "SIZE",
    "TOUCH_MAJOR",
    "TOUCH_MINOR",
    "TOOL_MAJOR",
    "TOOL_MINOR",
    "ORIENTATION",
    "VSCROLL",
    "HSCROLL",
    "Z",
    "RX",
    "RY",
    "RZ",
    "HAT_X",
    "HAT_Y",
    "LTRIGGER",
    "RTRIGGER",
    "THROTTLE",
    "RUDDER",
    "WHEEL",
    "GAS",
    "BRAKE",
    "DISTANCE",
    "TILT",
    "SCROLL",
    "RELATIVE_X",
    "RELATIVE_Y",
    nullptr,
    nullptr,
    nullptr,
    "GENERIC_1",
    "GENERIC_2",
    "GENERIC_3",
    "GENERIC_4",
    "GENERIC_5",
    "GENERIC_6",
    "GENERIC_7",
    "GENERIC_8",
    "GENERIC_9",
    "GENERIC_10",
    "GENERIC_11",
    "GENERIC_12",
    "GENERIC_13",
    "GENERIC_14",
    "GENERIC_15",
    "GENERIC_16",
    "GESTURE_X_OFFSET",
    "GESTURE_Y_OFFSET",
    "GESTURE_SCROLL_X_DISTANCE",
    "GESTURE_SCROLL_Y_DISTANCE",
    "GESTURE_PINCH_SCALE_FACTOR",
    "GESTURE_SWIPE_FINGER_COUNT",
};

template <const auto& Labels> jstring InputLabel(JNIEnv* env, jclass, jint value) {
    if (value < 0 || static_cast<size_t>(value) >= Labels.size() || !Labels[value]) {
        return nullptr;
    }
    return env->NewStringUTF(Labels[value]);
}

template <const auto& Labels, jint Unknown> jint InputValue(JNIEnv* env, jclass, jstring label) {
    return CallSafely(env, [&] {
        const std::string text = Utf8(env, label);
        for (size_t i = 0; i < Labels.size(); ++i) {
            if (Labels[i] && text == Labels[i])
                return static_cast<jint>(i);
        }
        return Unknown;
    });
}

struct PointerProperties {
    jint id, tool;
};

struct PointerCoordinates {
    std::array<float, kAxisCount> axes{};
    uint64_t packed = 0;
    bool resampled = false;
};

struct MotionSample {
    jlong time = 0;
    std::vector<PointerCoordinates> pointers;
};

struct MotionPeer {
    jint id = 0, device = 0, source = 0, display = 0, action = 0, flags = 0, edges = 0;
    jint meta = 0, buttons = 0, classification = 0, action_button = 0;
    jfloat x_precision = 1, y_precision = 1;
    jlong down_time = 0;
    std::array<float, 2> cursor{NAN, NAN};
    Matrix9 transform = kIdentity;
    std::vector<PointerProperties> properties;
    std::vector<MotionSample> samples;

    bool IsPointer() const { return (source & 0xff) == kSourceClassPointer; }
};

template <class T> T& InputPeer(jlong handle) {
    return *reinterpret_cast<T*>(static_cast<uintptr_t>(handle));
}

template <class T> jlong StoreInputPeer(JNIEnv* env, jobject owner, jlong handle, T value) {
    if (handle) {
        InputPeer<T>(handle) = std::move(value);
        return handle;
    }
    auto allocation = std::make_shared<T>(std::move(value));
    const auto result = reinterpret_cast<uintptr_t>(allocation.get());
    interp::JniBridge::From(env).RetainNativePeer(owner, std::move(allocation));
    return static_cast<jlong>(result);
}

constexpr std::array<std::pair<jint, const char*>, 11> kCoordinateFields{{
    {0, "x"},
    {1, "y"},
    {2, "pressure"},
    {3, "size"},
    {4, "touchMajor"},
    {5, "touchMinor"},
    {6, "toolMajor"},
    {7, "toolMinor"},
    {8, "orientation"},
    {27, "relativeX"},
    {28, "relativeY"},
}};

bool ReadCoordinates(JNIEnv* env, jobject object, PointerCoordinates& coordinates) {
    if (!object) {
        Throw(env, "java/lang/NullPointerException", "pointerCoords");
        return false;
    }
    const auto type = env->GetObjectClass(object);
    for (const auto& [axis, field] : kCoordinateFields) {
        coordinates.axes[axis] = env->GetFloatField(object, env->GetFieldID(type, field, "F"));
    }
    coordinates.resampled = env->GetBooleanField(object, env->GetFieldID(type, "isResampled", "Z"));
    coordinates.packed = std::bit_cast<uint64_t>(
        env->GetLongField(object, env->GetFieldID(type, "mPackedAxisBits", "J")));
    if (coordinates.packed) {
        auto array = static_cast<jfloatArray>(
            env->GetObjectField(object, env->GetFieldID(type, "mPackedAxisValues", "[F")));
        if (!array) {
            Throw(env, "java/lang/IllegalArgumentException", "Missing packed pointer axes");
            return false;
        }
        std::array<float, kAxisCount> values{};
        env->GetFloatArrayRegion(array, 0, std::popcount(coordinates.packed), values.data());
        env->DeleteLocalRef(array);
        unsigned i = 0;
        auto bits = coordinates.packed;
        while (bits) {
            const unsigned axis = std::countl_zero(bits);
            bits &= ~(uint64_t{1} << (63 - axis));
            coordinates.axes[axis] = values[i++];
        }
    }
    env->DeleteLocalRef(type);
    return !env->ExceptionCheck();
}

void WriteCoordinates(JNIEnv* env, jobject object, const PointerCoordinates& coordinates) {
    const auto type = env->GetObjectClass(object);
    for (const auto& [axis, field] : kCoordinateFields) {
        env->SetFloatField(object, env->GetFieldID(type, field, "F"), coordinates.axes[axis]);
    }
    env->SetBooleanField(object, env->GetFieldID(type, "isResampled", "Z"), coordinates.resampled);
    env->SetLongField(object, env->GetFieldID(type, "mPackedAxisBits", "J"),
                      std::bit_cast<jlong>(coordinates.packed));
    if (!coordinates.packed)
        return;

    const auto field = env->GetFieldID(type, "mPackedAxisValues", "[F");
    auto array = static_cast<jfloatArray>(env->GetObjectField(object, field));
    const jint count = std::popcount(coordinates.packed);
    if (!array || env->GetArrayLength(array) < count) {
        array = env->NewFloatArray(std::max(8, count));
        if (!array)
            return;
        env->SetObjectField(object, field, array);
    }
    std::array<float, kAxisCount> values{};
    unsigned i = 0;
    auto bits = coordinates.packed;
    while (bits) {
        const unsigned axis = std::countl_zero(bits);
        bits &= ~(uint64_t{1} << (63 - axis));
        values[i++] = coordinates.axes[axis];
    }
    env->SetFloatArrayRegion(array, 0, count, values.data());
}

bool ReadSample(JNIEnv* env, jobjectArray array, jint count, jlong time, MotionSample& sample) {
    if (!array || env->GetArrayLength(array) < count) {
        Throw(env, "java/lang/IllegalArgumentException", "Insufficient pointer coordinates");
        return false;
    }
    sample.time = time;
    sample.pointers.resize(count);
    for (jint i = 0; i < count; ++i) {
        const auto object = env->GetObjectArrayElement(array, i);
        const bool valid = ReadCoordinates(env, object, sample.pointers[i]);
        env->DeleteLocalRef(object);
        if (!valid)
            return false;
    }
    return true;
}

const MotionSample* GetSample(JNIEnv* env, const MotionPeer& event, jint position) {
    if (position == kHistoryCurrent)
        return &event.samples.back();
    if (position < 0 || static_cast<size_t>(position) >= event.samples.size() - 1) {
        Throw(env, "java/lang/IllegalArgumentException", "Invalid motion history index");
        return nullptr;
    }
    return &event.samples[position];
}

bool ValidPointer(JNIEnv* env, const MotionPeer& event, jint index) {
    if (index >= 0 && static_cast<size_t>(index) < event.properties.size())
        return true;
    Throw(env, "java/lang/IllegalArgumentException", "Invalid motion pointer index");
    return false;
}

void TransformCoordinates(PointerCoordinates& coords, const Matrix9& matrix, jint flags) {
    const auto origin = MapPoint(matrix, coords.axes[0], coords.axes[1]);
    const auto relative = MapPoint(matrix, coords.axes[0] + coords.axes[kRelativeX],
                                   coords.axes[1] + coords.axes[kRelativeY]);
    coords.axes[0] = origin[0];
    coords.axes[1] = origin[1];
    coords.axes[kRelativeX] = relative[0] - origin[0];
    coords.axes[kRelativeY] = relative[1] - origin[1];
    if (flags & 0x80) {
        const float angle = coords.axes[kOrientation];
        const float sine = std::sin(angle), cosine = std::cos(angle);
        float orientation = std::atan2(matrix[0] * sine - matrix[1] * cosine,
                                       matrix[4] * cosine - matrix[3] * sine);
        if (!(flags & 0x100)) {
            orientation = std::remainder(orientation, std::numbers::pi_v<float>);
        }
        coords.axes[kOrientation] = orientation;
    }
}

jlong InitializeMotion(JNIEnv* env, jobject owner, jlong handle, jint device, jint source,
                       jint display, jint action, jint flags, jint edges, jint meta, jint buttons,
                       jint classification, jfloat dx, jfloat dy, jfloat x_precision,
                       jfloat y_precision, jlong down_time, jlong time, jint count,
                       jobjectArray properties, jobjectArray coordinates) {
    return CallSafely(env, [&]() -> jlong {
        if (count < 1 || count > kMaxPointers || !properties ||
            env->GetArrayLength(properties) < count) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid motion pointer properties");
            return 0;
        }
        MotionPeer event;
        event.id = NextInputId(env, nullptr);
        event.device = device;
        event.source = source;
        event.display = display;
        event.action = action;
        event.flags = flags;
        event.edges = edges;
        event.meta = meta;
        event.buttons = buttons;
        event.classification = classification;
        event.x_precision = x_precision;
        event.y_precision = y_precision;
        event.down_time = down_time;
        event.transform = Translation(dx, dy);
        event.properties.reserve(count);
        uint32_t used_ids = 0;
        for (jint i = 0; i < count; ++i) {
            const auto property = env->GetObjectArrayElement(properties, i);
            if (!property) {
                Throw(env, "java/lang/NullPointerException", "pointerProperties");
                return 0;
            }
            const auto type = env->GetObjectClass(property);
            const auto id = env->GetIntField(property, env->GetFieldID(type, "id", "I"));
            const auto tool = env->GetIntField(property, env->GetFieldID(type, "toolType", "I"));
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(property);
            if (id < 0 || id >= kMaxPointers || (used_ids & (1u << id)) != 0) {
                Throw(env, "java/lang/IllegalArgumentException", "Invalid motion pointer ID");
                return 0;
            }
            used_ids |= 1u << id;
            event.properties.push_back({id, tool});
        }
        MotionSample sample;
        if (!ReadSample(env, coordinates, count, time, sample))
            return 0;
        for (auto& coords : sample.pointers) {
            if (coords.axes[kOrientation] != 0.f)
                event.flags |= kOrientationFlags;
            if (event.IsPointer() && (dx != 0 || dy != 0)) {
                coords.axes[0] -= dx;
                coords.axes[1] -= dy;
            }
        }
        event.samples.emplace_back(std::move(sample));
        return StoreInputPeer(env, owner, handle, std::move(event));
    });
}

void AddMotionBatch(JNIEnv* env, jclass, jlong handle, jlong time, jobjectArray coordinates,
                    jint meta) {
    CallSafely(env, [&] {
        auto& event = InputPeer<MotionPeer>(handle);
        MotionSample sample;
        if (!ReadSample(env, coordinates, event.properties.size(), time, sample))
            return;
        if (event.IsPointer() && event.transform != kIdentity) {
            if (const auto inverse = Inverse(event.transform)) {
                for (auto& coords : sample.pointers)
                    TransformCoordinates(coords, *inverse, event.flags);
            }
        }
        event.samples.emplace_back(std::move(sample));
        event.meta |= meta;
    });
}

jlong CopyMotion(JNIEnv* env, jobject owner, jlong handle, jlong source, jboolean history) {
    return CallSafely(env, [&] {
        auto event = InputPeer<MotionPeer>(source);
        if (!history)
            event.samples.erase(event.samples.begin(), event.samples.end() - 1);
        return StoreInputPeer(env, owner, handle, std::move(event));
    });
}

jlong SplitMotion(JNIEnv* env, jobject owner, jlong handle, jlong source, jint id_bits) {
    return CallSafely(env, [&]() -> jlong {
        auto event = InputPeer<MotionPeer>(source);
        const jint action = event.action & kActionMask;
        const size_t action_index = static_cast<uint32_t>(event.action) >> kPointerIndexShift;
        std::vector<size_t> keep;
        jint new_index = -1;
        for (size_t i = 0; i < event.properties.size(); ++i) {
            const jint id = event.properties[i].id;
            if (id < 0 || id >= kMaxPointers || !(static_cast<uint32_t>(id_bits) & (1u << id))) {
                continue;
            }
            if (i == action_index)
                new_index = keep.size();
            event.properties[keep.size()] = event.properties[i];
            keep.push_back(i);
        }
        if (keep.empty()) {
            Throw(env, "java/lang/IllegalArgumentException",
                  "Motion event contains no requested pointers");
            return 0;
        }
        event.properties.resize(keep.size());
        for (auto& sample : event.samples) {
            for (size_t i = 0; i < keep.size(); ++i)
                sample.pointers[i] = sample.pointers[keep[i]];
            sample.pointers.resize(keep.size());
        }
        if (action == kPointerDown || action == kPointerUp) {
            if (new_index < 0) {
                event.action = kMove;
            } else if (keep.size() == 1) {
                event.action = action == kPointerDown      ? kDown
                               : (event.flags & kCanceled) ? kCancel
                                                           : kUp;
            } else {
                event.action = action | (new_index << kPointerIndexShift);
            }
        }
        event.id = NextInputId(env, nullptr);
        return StoreInputPeer(env, owner, handle, std::move(event));
    });
}

template <auto Field> auto GetMotionField(JNIEnv*, jclass, jlong handle) {
    return InputPeer<MotionPeer>(handle).*Field;
}

template <auto Field, class T> void PutMotionField(JNIEnv*, jclass, jlong handle, T value) {
    InputPeer<MotionPeer>(handle).*Field = value;
}

template <auto Field> jint GetPointerField(JNIEnv* env, jclass, jlong handle, jint index) {
    const auto& event = InputPeer<MotionPeer>(handle);
    return ValidPointer(env, event, index) ? event.properties[index].*Field : 0;
}

void GetPointerProperties(JNIEnv* env, jclass, jlong handle, jint index, jobject output) {
    const auto& event = InputPeer<MotionPeer>(handle);
    if (!ValidPointer(env, event, index))
        return;
    const auto& pointer = event.properties[index];
    const auto type = env->GetObjectClass(output);
    env->SetIntField(output, env->GetFieldID(type, "id", "I"), pointer.id);
    env->SetIntField(output, env->GetFieldID(type, "toolType", "I"), pointer.tool);
}

void GetPointerCoordinates(JNIEnv* env, jclass, jlong handle, jint index, jint history,
                           jobject output) {
    const auto& event = InputPeer<MotionPeer>(handle);
    const auto* sample = GetSample(env, event, history);
    if (!sample || !ValidPointer(env, event, index))
        return;
    auto coords = sample->pointers[index];
    if (event.IsPointer() && event.transform != kIdentity) {
        TransformCoordinates(coords, event.transform, event.flags);
    }
    WriteCoordinates(env, output, coords);
}

template <bool Raw>
jfloat MotionAxis(JNIEnv* env, jclass, jlong handle, jint axis, jint index, jint history) {
    const auto& event = InputPeer<MotionPeer>(handle);
    const auto* sample = GetSample(env, event, history);
    if (!sample || !ValidPointer(env, event, index) || axis < 0 ||
        static_cast<size_t>(axis) >= kAxisCount)
        return 0;
    const auto& coords = sample->pointers[index];
    if (!Raw && event.IsPointer() && event.transform != kIdentity) {
        if (axis <= 1)
            return MapPoint(event.transform, coords.axes[0], coords.axes[1])[axis];
        if (axis == kOrientation || axis == kRelativeX || axis == kRelativeY) {
            auto transformed = coords;
            TransformCoordinates(transformed, event.transform, event.flags);
            return transformed.axes[axis];
        }
    }
    return coords.axes[axis];
}

jlong MotionTime(JNIEnv* env, jclass, jlong handle, jint history) {
    const auto* sample = GetSample(env, InputPeer<MotionPeer>(handle), history);
    return sample ? sample->time : 0;
}

jint PointerCount(JNIEnv*, jclass, jlong handle) {
    return InputPeer<MotionPeer>(handle).properties.size();
}

jint FindPointer(JNIEnv*, jclass, jlong handle, jint id) {
    const auto& properties = InputPeer<MotionPeer>(handle).properties;
    const auto it = std::ranges::find(properties, id, &PointerProperties::id);
    return it == properties.end() ? -1 : static_cast<jint>(it - properties.begin());
}

jint MotionHistorySize(JNIEnv*, jclass, jlong handle) {
    return InputPeer<MotionPeer>(handle).samples.size() - 1;
}

jboolean IsTouchEvent(JNIEnv*, jclass, jlong handle) {
    const auto& event = InputPeer<MotionPeer>(handle);
    return event.IsPointer() && (event.action & kActionMask) <= kPointerUp;
}

jint GetMotionFlags(JNIEnv*, jclass, jlong handle) {
    return InputPeer<MotionPeer>(handle).flags & ~kOrientationFlags;
}

void SetMotionFlags(JNIEnv*, jclass, jlong handle, jint flags) {
    auto& event = InputPeer<MotionPeer>(handle);
    event.flags = (flags & ~kOrientationFlags) | (event.flags & kOrientationFlags);
}

void OffsetMotion(JNIEnv*, jclass, jlong handle, jfloat dx, jfloat dy) {
    auto& event = InputPeer<MotionPeer>(handle);
    event.transform = Multiply(Translation(dx, dy), event.transform);
}

template <size_t Axis> jfloat MotionOffset(JNIEnv*, jclass, jlong handle) {
    const auto& matrix = InputPeer<MotionPeer>(handle).transform;
    return MapPoint(matrix, 0, 0)[Axis];
}

template <size_t Axis> jfloat MotionCursor(JNIEnv*, jclass, jlong handle) {
    const auto& event = InputPeer<MotionPeer>(handle);
    return MapPoint(event.transform, event.cursor[0], event.cursor[1])[Axis];
}

void SetMotionCursor(JNIEnv*, jclass, jlong handle, jfloat x, jfloat y) {
    auto& event = InputPeer<MotionPeer>(handle);
    if (event.transform == kIdentity) {
        event.cursor = {x, y};
    } else if (const auto inverse = Inverse(event.transform)) {
        event.cursor = MapPoint(*inverse, x, y);
    }
}

template <bool Apply> void TransformMotion(JNIEnv* env, jclass, jlong handle, jobject matrix) {
    const auto type = env->GetObjectClass(matrix);
    const auto peer = env->GetLongField(matrix, env->GetFieldID(type, "native_instance", "J"));
    const auto* values = MatrixValues(env, peer);
    if (!values)
        return;
    auto& event = InputPeer<MotionPeer>(handle);
    if constexpr (Apply) {
        for (auto& sample : event.samples) {
            for (auto& coords : sample.pointers)
                TransformCoordinates(coords, *values, event.flags);
        }
        event.cursor = MapPoint(*values, event.cursor[0], event.cursor[1]);
    } else {
        event.transform = Multiply(*values, event.transform);
    }
}

void ScaleMotion(JNIEnv*, jclass, jlong handle, jfloat scale) {
    auto& event = InputPeer<MotionPeer>(handle);
    event.x_precision *= scale;
    event.y_precision *= scale;
    event.transform[2] *= scale;
    event.transform[5] *= scale;
    event.cursor[0] *= scale;
    event.cursor[1] *= scale;
    for (auto& sample : event.samples) {
        for (auto& coords : sample.pointers) {
            for (const auto axis : {0, 1, 4, 5, 6, 7, kRelativeX, kRelativeY}) {
                coords.axes[axis] *= scale;
            }
        }
    }
}

jint MotionRotation(JNIEnv*, jclass, jlong handle) {
    const auto& transform = InputPeer<MotionPeer>(handle).transform;
    if (transform[1] == 0 && transform[3] == 0) {
        if (transform[0] > 0 && transform[4] > 0)
            return 0;
        if (transform[0] < 0 && transform[4] < 0)
            return 2;
    }
    if (transform[0] == 0 && transform[4] == 0) {
        if (transform[1] > 0 && transform[3] < 0)
            return 1;
        if (transform[1] < 0 && transform[3] > 0)
            return 3;
    }
    return -1;
}

constexpr std::array<jint MotionPeer::*, 11> kParcelFields{
    &MotionPeer::id,
    &MotionPeer::device,
    &MotionPeer::source,
    &MotionPeer::display,
    &MotionPeer::action,
    &MotionPeer::flags,
    &MotionPeer::edges,
    &MotionPeer::meta,
    &MotionPeer::buttons,
    &MotionPeer::classification,
    &MotionPeer::action_button,
};

bool ParcelArraySize(JNIEnv* env, jarray array, size_t size) {
    if (env->ExceptionCheck())
        return false;
    if (array && static_cast<size_t>(env->GetArrayLength(array)) == size)
        return true;
    Throw(env, "android/os/BadParcelableException", "Invalid motion event data");
    return false;
}

void WriteMotionParcel(JNIEnv* env, jclass, jlong handle, jobject parcel) {
    const auto& event = InputPeer<MotionPeer>(handle);
    const auto type = env->GetObjectClass(parcel);
    const auto write_ints = env->GetMethodID(type, "writeIntArray", "([I)V");
    const auto write_floats = env->GetMethodID(type, "writeFloatArray", "([F)V");
    const auto write_long = env->GetMethodID(type, "writeLong", "(J)V");
    const jint count = event.properties.size();
    std::array<jint, kParcelFields.size() + 2> fields{};
    for (size_t i = 0; i < kParcelFields.size(); ++i)
        fields[i] = event.*kParcelFields[i];
    fields[kParcelFields.size()] = count;
    fields.back() = event.samples.size();
    auto header = env->NewIntArray(fields.size());
    env->SetIntArrayRegion(header, 0, fields.size(), fields.data());
    env->CallVoidMethod(parcel, write_ints, header);

    std::array<jint, kMaxPointers * 2> properties{};
    for (jint i = 0; i < count; ++i) {
        properties[2 * i] = event.properties[i].id;
        properties[2 * i + 1] = event.properties[i].tool;
    }
    auto ids = env->NewIntArray(count * 2);
    env->SetIntArrayRegion(ids, 0, count * 2, properties.data());
    env->CallVoidMethod(parcel, write_ints, ids);

    std::array<float, kIdentity.size() + 4> transform{};
    std::ranges::copy(event.transform, transform.begin());
    transform[9] = event.cursor[0];
    transform[10] = event.cursor[1];
    transform[11] = event.x_precision;
    transform[12] = event.y_precision;
    auto mapping = env->NewFloatArray(transform.size());
    env->SetFloatArrayRegion(mapping, 0, transform.size(), transform.data());
    env->CallVoidMethod(parcel, write_floats, mapping);
    env->CallVoidMethod(parcel, write_long, event.down_time);

    for (const auto& sample : event.samples) {
        if (env->ExceptionCheck())
            return;
        env->CallVoidMethod(parcel, write_long, sample.time);
        for (const auto& coords : sample.pointers) {
            env->CallVoidMethod(parcel, write_long, std::bit_cast<jlong>(coords.packed));
            env->CallVoidMethod(parcel, write_long, jlong(coords.resampled));
            const auto axes = env->NewFloatArray(coords.axes.size());
            env->SetFloatArrayRegion(axes, 0, coords.axes.size(), coords.axes.data());
            env->CallVoidMethod(parcel, write_floats, axes);
            env->DeleteLocalRef(axes);
        }
    }
}

jlong ReadMotionParcel(JNIEnv* env, jobject owner, jlong handle, jobject parcel) {
    return CallSafely(env, [&]() -> jlong {
        const auto type = env->GetObjectClass(parcel);
        const auto read_ints = env->GetMethodID(type, "createIntArray", "()[I");
        const auto read_floats = env->GetMethodID(type, "createFloatArray", "()[F");
        const auto read_long = env->GetMethodID(type, "readLong", "()J");
        const auto size = env->GetMethodID(type, "dataSize", "()I");
        const auto position = env->GetMethodID(type, "dataPosition", "()I");
        MotionPeer event;
        std::array<jint, kParcelFields.size() + 2> fields{};
        const auto header = static_cast<jintArray>(env->CallObjectMethod(parcel, read_ints));
        if (!ParcelArraySize(env, header, fields.size()))
            return 0;
        env->GetIntArrayRegion(header, 0, fields.size(), fields.data());
        for (size_t i = 0; i < kParcelFields.size(); ++i)
            event.*kParcelFields[i] = fields[i];
        const jint count = fields[kParcelFields.size()], samples = fields.back();
        const jint total = env->CallIntMethod(parcel, size);
        const jint consumed = env->CallIntMethod(parcel, position);
        if (count < 1 || count > kMaxPointers || samples < 1 || total < consumed ||
            samples > (total - consumed) / (4 + 12 * count)) {
            Throw(env, "android/os/BadParcelableException", "Invalid motion event count");
            return 0;
        }

        std::array<jint, kMaxPointers * 2> properties{};
        const auto ids = static_cast<jintArray>(env->CallObjectMethod(parcel, read_ints));
        if (!ParcelArraySize(env, ids, count * 2))
            return 0;
        env->GetIntArrayRegion(ids, 0, count * 2, properties.data());
        for (jint i = 0; i < count; ++i) {
            event.properties.push_back({properties[2 * i], properties[2 * i + 1]});
        }
        std::array<float, kIdentity.size() + 4> transform{};
        const auto mapping = static_cast<jfloatArray>(env->CallObjectMethod(parcel, read_floats));
        if (!ParcelArraySize(env, mapping, transform.size()))
            return 0;
        env->GetFloatArrayRegion(mapping, 0, transform.size(), transform.data());
        std::copy_n(transform.begin(), event.transform.size(), event.transform.begin());
        event.cursor = {transform[9], transform[10]};
        event.x_precision = transform[11];
        event.y_precision = transform[12];
        event.down_time = env->CallLongMethod(parcel, read_long);
        event.samples.reserve(samples);
        for (jint i = 0; i < samples; ++i) {
            auto& sample = event.samples.emplace_back();
            sample.time = env->CallLongMethod(parcel, read_long);
            sample.pointers.resize(count);
            for (auto& coords : sample.pointers) {
                coords.packed = std::bit_cast<uint64_t>(env->CallLongMethod(parcel, read_long));
                coords.resampled = env->CallLongMethod(parcel, read_long) != 0;
                const auto axes =
                    static_cast<jfloatArray>(env->CallObjectMethod(parcel, read_floats));
                if (!ParcelArraySize(env, axes, coords.axes.size()))
                    return 0;
                env->GetFloatArrayRegion(axes, 0, coords.axes.size(), coords.axes.data());
                env->DeleteLocalRef(axes);
            }
        }
        return env->ExceptionCheck() ? 0 : StoreInputPeer(env, owner, handle, std::move(event));
    });
}

constexpr jlong kVelocityHorizon = 100'000'000, kPointerStop = 40'000'000;
constexpr jlong kLegacyVelocityHorizon = 200'000'000;
constexpr size_t kVelocitySamples = 20;
constexpr std::array<jint, 3> kVelocityAxes{0, 1, kScrollAxis};

struct TimedPosition {
    jlong time;
    float position;
};

using VelocityHistory = std::array<std::deque<TimedPosition>, kVelocityAxes.size()>;
using VelocityValues = std::array<jfloat, kVelocityAxes.size()>;

struct VelocityPeer {
    explicit VelocityPeer(jint strategy) : strategy(strategy) {}
    void Clear() {
        history.clear();
        velocities.clear();
        active = -1;
        last = 0;
    }
    void ClearPointer(jint id) {
        history.erase(id);
        velocities.erase(id);
        if (active == id) {
            active = -1;
            for (const auto& [pointer, _] : history) {
                if (active == -1 || pointer < active)
                    active = pointer;
            }
        }
    }

    jint strategy, active = -1;
    jlong last = 0;
    std::unordered_map<jint, VelocityHistory> history;
    std::unordered_map<jint, VelocityValues> velocities;
};

jlong InitializeVelocity(JNIEnv* env, jobject owner, jint strategy) {
    return CallSafely(env, [&] { return StoreInputPeer(env, owner, 0, VelocityPeer(strategy)); });
}

void ClearVelocity(JNIEnv*, jclass, jlong handle) {
    InputPeer<VelocityPeer>(handle).Clear();
}

void AddVelocitySample(std::deque<TimedPosition>& history, jlong time, float value, jlong horizon) {
    if (!history.empty()) {
        if (history.back().time > time)
            return;
        if (history.back().time == time)
            history.pop_back();
    }
    history.push_back({time, value});
    while (history.size() > kVelocitySamples || time - history.front().time > horizon) {
        history.pop_front();
    }
}

void AddVelocityMovement(JNIEnv* env, jclass, jlong handle, jobject input) {
    CallSafely(env, [&] {
        auto& tracker = InputPeer<VelocityPeer>(handle);
        const auto type = env->GetObjectClass(input);
        const auto ptr = env->GetLongField(input, env->GetFieldID(type, "mNativePtr", "J"));
        if (env->ExceptionCheck() || !ptr)
            return;
        const auto& motion = InputPeer<MotionPeer>(ptr);
        const auto action = motion.action & kActionMask;
        const auto index =
            static_cast<size_t>(static_cast<uint32_t>(motion.action) >> kPointerIndexShift);
        if (action == kDown || action == kHoverEnter || action == kCancel)
            tracker.Clear();
        if (action == kCancel)
            return;
        if (action == kPointerDown && index < motion.properties.size()) {
            tracker.ClearPointer(motion.properties[index].id);
        }
        if (action == kPointerUp || action == kUp) {
            if ((motion.flags & kCanceled) && index < motion.properties.size()) {
                tracker.ClearPointer(motion.properties[index].id);
            } else if (motion.samples.back().time - tracker.last > kPointerStop) {
                for (auto& [_, traces] : tracker.history) {
                    traces[0].clear();
                    traces[1].clear();
                }
            }
            return;
        }
        const bool scroll = action == kScroll;
        if (!scroll && action != kDown && action != kMove && action != kPointerDown &&
            action != kHoverEnter && action != kHoverMove)
            return;
        const jlong horizon = tracker.strategy == 9 ? kLegacyVelocityHorizon : kVelocityHorizon;
        for (const auto& sample : motion.samples) {
            if (tracker.last && sample.time - tracker.last > kPointerStop) {
                tracker.history.clear();
                tracker.velocities.clear();
            }
            tracker.last = sample.time;
            for (size_t i = 0; i < sample.pointers.size(); ++i) {
                const auto& coords = sample.pointers[i];
                if (coords.resampled)
                    continue;
                const jint pointer = motion.properties[i].id;
                if (tracker.active == -1)
                    tracker.active = pointer;
                auto& traces = tracker.history[pointer];
                if (scroll) {
                    AddVelocitySample(traces[2], sample.time, coords.axes[kScrollAxis],
                                      kVelocityHorizon);
                } else {
                    const auto position =
                        motion.IsPointer()
                            ? MapPoint(motion.transform, coords.axes[0], coords.axes[1])
                            : std::array{coords.axes[0], coords.axes[1]};
                    AddVelocitySample(traces[0], sample.time, position[0], horizon);
                    AddVelocitySample(traces[1], sample.time, position[1], horizon);
                }
            }
        }
    });
}

double SampleWeight(const std::deque<TimedPosition>& history, size_t index, jint strategy) {
    const double age = (history.back().time - history[index].time) * 1e-6;
    switch (strategy) {
    case 4: {
        if (index + 1 == history.size())
            return 1;
        const double delta = (history[index + 1].time - history[index].time) * 1e-6;
        return std::clamp(0.5 + delta * 0.05, 0.5, 1.0);
    }
    case 5:
        if (age < 10)
            return 0.5 + age / 20;
        if (age < 50)
            return 1;
        return std::clamp(1.0 - (age - 50) / 20, 0.5, 1.0);
    case 6:
        return std::clamp(1.0 - (age - 50) / 100, 0.5, 1.0);
    default:
        return 1;
    }
}

double LeastSquaresVelocity(const std::deque<TimedPosition>& history, size_t degree,
                            jint strategy) {
    // Fit position against timestamps relative to the latest sample. The linear coefficient
    // is the curve's derivative at that sample. Time is normalized to the history horizon.
    const size_t terms = std::min(degree + 1, history.size());
    if (terms < 2)
        return 0;
    double system[4][5]{};
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& sample = history[i];
        const double time =
            static_cast<double>(sample.time - history.back().time) / kVelocityHorizon;
        const double weight = SampleWeight(history, i, strategy);
        std::array<double, 7> powers{1};
        for (size_t j = 1; j < powers.size(); ++j)
            powers[j] = powers[j - 1] * time;
        for (size_t row = 0; row < terms; ++row) {
            for (size_t col = 0; col < terms; ++col) {
                system[row][col] += powers[row + col] * weight * weight;
            }
            system[row][terms] +=
                powers[row] * weight * weight * (sample.position - history.back().position);
        }
    }
    for (size_t row = 0; row < terms; ++row) {
        size_t pivot = row;
        for (size_t next = row + 1; next < terms; ++next) {
            if (std::abs(system[next][row]) > std::abs(system[pivot][row]))
                pivot = next;
        }
        if (std::abs(system[pivot][row]) < 1e-12)
            return 0;
        std::swap(system[pivot], system[row]);
        const double divisor = system[row][row];
        for (size_t col = row; col <= terms; ++col)
            system[row][col] /= divisor;
        for (size_t other = 0; other < terms; ++other) {
            if (other == row)
                continue;
            const double factor = system[other][row];
            for (size_t col = row; col <= terms; ++col) {
                system[other][col] -= factor * system[row][col];
            }
        }
    }
    return system[1][terms] * (1e9 / kVelocityHorizon);
}

double ImpulseVelocity(const std::deque<TimedPosition>& history, bool differential) {
    const auto kinetic_velocity = [](double work) {
        return std::copysign(std::sqrt(2 * std::abs(work)), work);
    };
    double work = 0;
    for (size_t i = 1; i < history.size(); ++i) {
        const auto& sample = history[i];
        const auto& previous = history[i - 1];
        if (sample.time == previous.time)
            continue;
        const double distance =
            differential ? sample.position : sample.position - previous.position;
        const double velocity = distance * 1e9 / (sample.time - previous.time);
        work += (velocity - kinetic_velocity(work)) * std::abs(velocity);
        if (i == 1)
            work *= 0.5;
    }
    return kinetic_velocity(work);
}

double FilteredVelocity(const std::deque<TimedPosition>& history, bool acceleration) {
    double velocity = 0, estimated_acceleration = 0;
    unsigned degree = 0;
    auto previous = history.front();
    for (size_t i = 1; i < history.size(); ++i) {
        const auto& sample = history[i];
        if (sample.time - previous.time <= 2'000'000)
            continue;
        const double dt = (sample.time - previous.time) * 1e-9;
        const double measured = (sample.position - previous.position) / dt;
        previous = sample;
        if (degree == 0) {
            velocity = measured;
            degree = 1;
            continue;
        }
        const double alpha = dt / (dt + 0.010);
        if (acceleration) {
            const double measured_acceleration = (measured - velocity) / dt;
            if (degree == 1) {
                estimated_acceleration = measured_acceleration;
                degree = 2;
            } else {
                estimated_acceleration += alpha * (measured_acceleration - estimated_acceleration);
            }
            velocity += estimated_acceleration * dt * alpha;
        } else {
            velocity += alpha * (measured - velocity);
        }
    }
    return velocity;
}

double LegacyVelocity(const std::deque<TimedPosition>& history) {
    if (history.size() < 2)
        return 0;
    const auto& first = history.front();
    double velocity = 0, previous_duration = 0;
    for (size_t i = 1; i < history.size(); ++i) {
        const double dt = (history[i].time - first.time) * 1e-9;
        if (dt < 0.010)
            continue;
        const double measured = (history[i].position - first.position) / dt;
        velocity = (velocity * previous_duration + measured * dt) / (previous_duration + dt);
        previous_duration = dt;
    }
    return velocity;
}

double EstimateVelocity(const std::deque<TimedPosition>& history, jint strategy, bool scroll) {
    if (history.size() < 2)
        return 0;
    if (strategy == 0 || scroll)
        return ImpulseVelocity(history, scroll);
    if (strategy == 7 || strategy == 8)
        return FilteredVelocity(history, strategy == 8);
    if (strategy == 9)
        return LegacyVelocity(history);
    const size_t degree = strategy == 1 ? 1 : strategy == 3 ? 3 : 2;
    return LeastSquaresVelocity(history, degree, strategy);
}

void ComputeVelocity(JNIEnv* env, jclass, jlong handle, jint units, jfloat maximum) {
    CallSafely(env, [&] {
        if (units <= 0 || !(maximum >= 0)) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid velocity scaling");
            return;
        }
        auto& tracker = InputPeer<VelocityPeer>(handle);
        tracker.velocities.clear();
        for (const auto& [pointer, traces] : tracker.history) {
            auto& speeds = tracker.velocities[pointer];
            for (size_t axis = 0; axis < speeds.size(); ++axis) {
                const double speed = EstimateVelocity(traces[axis], tracker.strategy, axis == 2);
                speeds[axis] =
                    std::clamp(static_cast<float>(speed * units / 1000), -maximum, maximum);
            }
        }
    });
}

jboolean IsVelocityAxis(JNIEnv*, jclass, jint axis) {
    return std::ranges::find(kVelocityAxes, axis) != kVelocityAxes.end();
}

jfloat GetVelocity(JNIEnv*, jclass, jlong handle, jint axis, jint pointer) {
    const auto& tracker = InputPeer<VelocityPeer>(handle);
    const auto index = std::ranges::find(kVelocityAxes, axis);
    const auto speeds = tracker.velocities.find(pointer == -1 ? tracker.active : pointer);
    if (index == kVelocityAxes.end() || speeds == tracker.velocities.end())
        return 0;
    return speeds->second[index - kVelocityAxes.begin()];
}

} // namespace

void RegisterViewInput(interp::Vm& vm) {
    Register(
        vm, "android/view/KeyEvent",
        Method<InputLabel<kKeyLabels>>(vm, "nativeKeyCodeToString", "(I)Ljava/lang/String;"),
        Method<InputValue<kKeyLabels, 0>>(vm, "nativeKeyCodeFromString", "(Ljava/lang/String;)I"),
        Method<NextInputId>(vm, "nativeNextId", "()I"));
    Register(
        vm, "android/view/MotionEvent",
        Method<InitializeMotion>(vm, "nativeInitialize",
                                 "(JIIIIIIIIIFFFFJJI[Landroid/view/MotionEvent$PointerProperties;"
                                 "[Landroid/view/MotionEvent$PointerCoords;)J"),
        Method<AddMotionBatch>(vm, "nativeAddBatch",
                               "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V"),
        Method<GetPointerCoordinates>(vm, "nativeGetPointerCoords",
                                      "(JIILandroid/view/MotionEvent$PointerCoords;)V"),
        Method<GetPointerProperties>(vm, "nativeGetPointerProperties",
                                     "(JILandroid/view/MotionEvent$PointerProperties;)V"),
        Method<ReadMotionParcel>(vm, "nativeReadFromParcel", "(JLandroid/os/Parcel;)J"),
        Method<WriteMotionParcel>(vm, "nativeWriteToParcel", "(JLandroid/os/Parcel;)V"),
        Method<InputLabel<kAxisLabels>>(vm, "nativeAxisToString", "(I)Ljava/lang/String;"),
        Method<InputValue<kAxisLabels, -1>>(vm, "nativeAxisFromString", "(Ljava/lang/String;)I"),
        Method<GetPointerField<&PointerProperties::id>>(vm, "nativeGetPointerId", "(JI)I"),
        Method<GetPointerField<&PointerProperties::tool>>(vm, "nativeGetToolType", "(JI)I"),
        Method<MotionTime>(vm, "nativeGetEventTimeNanos", "(JI)J"),
        Method<MotionAxis<true>>(vm, "nativeGetRawAxisValue", "(JIII)F"),
        Method<MotionAxis<false>>(vm, "nativeGetAxisValue", "(JIII)F"),
        Method<TransformMotion<false>>(vm, "nativeTransform", "(JLandroid/graphics/Matrix;)V"),
        Method<TransformMotion<true>>(vm, "nativeApplyTransform", "(JLandroid/graphics/Matrix;)V"),
        Method<CopyMotion>(vm, "nativeCopy", "(JJZ)J"),
        Method<SplitMotion>(vm, "nativeSplit", "(JJI)J"),
        Method<GetMotionField<&MotionPeer::id>>(vm, "nativeGetId", "(J)I"),
        Method<GetMotionField<&MotionPeer::device>>(vm, "nativeGetDeviceId", "(J)I"),
        Method<GetMotionField<&MotionPeer::source>>(vm, "nativeGetSource", "(J)I"),
        Method<PutMotionField<&MotionPeer::source, jint>>(vm, "nativeSetSource", "(JI)V"),
        Method<GetMotionField<&MotionPeer::display>>(vm, "nativeGetDisplayId", "(J)I"),
        Method<PutMotionField<&MotionPeer::display, jint>>(vm, "nativeSetDisplayId", "(JI)V"),
        Method<GetMotionField<&MotionPeer::action>>(vm, "nativeGetAction", "(J)I"),
        Method<PutMotionField<&MotionPeer::action, jint>>(vm, "nativeSetAction", "(JI)V"),
        Method<IsTouchEvent>(vm, "nativeIsTouchEvent", "(J)Z"),
        Method<GetMotionFlags>(vm, "nativeGetFlags", "(J)I"),
        Method<SetMotionFlags>(vm, "nativeSetFlags", "(JI)V"),
        Method<GetMotionField<&MotionPeer::edges>>(vm, "nativeGetEdgeFlags", "(J)I"),
        Method<PutMotionField<&MotionPeer::edges, jint>>(vm, "nativeSetEdgeFlags", "(JI)V"),
        Method<GetMotionField<&MotionPeer::meta>>(vm, "nativeGetMetaState", "(J)I"),
        Method<GetMotionField<&MotionPeer::buttons>>(vm, "nativeGetButtonState", "(J)I"),
        Method<PutMotionField<&MotionPeer::buttons, jint>>(vm, "nativeSetButtonState", "(JI)V"),
        Method<GetMotionField<&MotionPeer::classification>>(vm, "nativeGetClassification", "(J)I"),
        Method<GetMotionField<&MotionPeer::action_button>>(vm, "nativeGetActionButton", "(J)I"),
        Method<PutMotionField<&MotionPeer::action_button, jint>>(vm, "nativeSetActionButton",
                                                                 "(JI)V"),
        Method<OffsetMotion>(vm, "nativeOffsetLocation", "(JFF)V"),
        Method<MotionOffset<0>>(vm, "nativeGetRawXOffset", "(J)F"),
        Method<MotionOffset<1>>(vm, "nativeGetRawYOffset", "(J)F"),
        Method<GetMotionField<&MotionPeer::x_precision>>(vm, "nativeGetXPrecision", "(J)F"),
        Method<GetMotionField<&MotionPeer::y_precision>>(vm, "nativeGetYPrecision", "(J)F"),
        Method<MotionCursor<0>>(vm, "nativeGetXCursorPosition", "(J)F"),
        Method<MotionCursor<1>>(vm, "nativeGetYCursorPosition", "(J)F"),
        Method<SetMotionCursor>(vm, "nativeSetCursorPosition", "(JFF)V"),
        Method<GetMotionField<&MotionPeer::down_time>>(vm, "nativeGetDownTimeNanos", "(J)J"),
        Method<PutMotionField<&MotionPeer::down_time, jlong>>(vm, "nativeSetDownTimeNanos",
                                                              "(JJ)V"),
        Method<PointerCount>(vm, "nativeGetPointerCount", "(J)I"),
        Method<FindPointer>(vm, "nativeFindPointerIndex", "(JI)I"),
        Method<MotionHistorySize>(vm, "nativeGetHistorySize", "(J)I"),
        Method<ScaleMotion>(vm, "nativeScale", "(JF)V"),
        Method<MotionRotation>(vm, "nativeGetSurfaceRotation", "(J)I"));
    Register(vm, "android/view/VelocityTracker",
             Method<InitializeVelocity>(vm, "nativeInitialize", "(I)J"),
             Method<ClearVelocity>(vm, "nativeClear", "(J)V"),
             Method<AddVelocityMovement>(vm, "nativeAddMovement", "(JLandroid/view/MotionEvent;)V"),
             Method<ComputeVelocity>(vm, "nativeComputeCurrentVelocity", "(JIF)V"),
             Method<GetVelocity>(vm, "nativeGetVelocity", "(JII)F"),
             Method<IsVelocityAxis>(vm, "nativeIsAxisSupported", "(I)Z"));
}

} // namespace wa::ui_jni::detail
