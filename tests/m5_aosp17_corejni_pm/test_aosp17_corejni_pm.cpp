#include "jni_dispatch.h"

#include <android-base/unique_fd.h>
#include <androidfw/Asset.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/ZipFileRO.h>

#include <array>
#include <fcntl.h>
#include <limits>

namespace android {

int register_android_content_res_ApkAssets(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_AssetManager(JNIEnv* env);

} // namespace android

namespace {

using android::Res_value;
using namespace wa::jni_test;

constexpr char kPackage[] = "org.wasmandroid.cube";
constexpr char kAndroidNamespace[] = "http://schemas.android.com/apk/res/android";
constexpr char kManifest[] = "AndroidManifest.xml";
constexpr char kDex[] = "classes.dex";
constexpr char kCubeLib[] = "lib/arm64-v8a/libndk_gles3_cube.so";
constexpr jint kApkFormat = 0;
constexpr jint kAssetCookie = 1;

// Event values from XmlBlock.Parser and the upstream nativeNext implementation.
constexpr jint kStartDocument = 0;
constexpr jint kEndDocument = 1;
constexpr jint kStartTag = 2;
constexpr jint kEndTag = 3;

std::string PoolString(const NativeClass& strings, jlong pool, jint index) {
    Check(index >= 0 && index < strings.Call<jint, jobject>("nativeGetSize", "(J)I", pool),
          "Invalid string pool index");
    return JniDispatch::Utf8(
        strings.Call<jstring, jobject>("nativeGetString", "(JI)Ljava/lang/String;", pool, index));
}

class BinaryXml {
  public:
    BinaryXml(const NativeClass& xml, const NativeClass& strings, jlong tree)
        : xml_(xml), strings_(strings), tree_(tree) {
        Check(tree_ != 0, "Could not open the binary XML tree");
        pool_ = xml_.Call<jlong, jobject>("nativeGetStringBlock", "(J)J", tree_);
        state_ = xml_.Call<jlong, jobject>("nativeCreateParseState", "(JI)J", tree_, jint{0});
        Check(pool_ != 0 && state_ != 0, "Could not initialize the binary XML parser");
    }

    BinaryXml(const BinaryXml&) = delete;
    BinaryXml& operator=(const BinaryXml&) = delete;

    ~BinaryXml() {
        xml_.Call<void, jobject>("nativeDestroyParseState", "(J)V", state_);
        xml_.Call<void, jobject>("nativeDestroy", "(J)V", tree_);
    }

    jint Next() const { return xml_.Call<jint>("nativeNext", "(J)I", state_); }

    std::string Tag() const {
        Check(xml_.Call<jint>("nativeGetNamespace", "(J)I", state_) == -1,
              "Expected an unqualified XML element");
        return PoolString(strings_, pool_, xml_.Call<jint>("nativeGetName", "(J)I", state_));
    }

    std::string StringAttribute(const char* name, std::string_view ns = kAndroidNamespace) const {
        const jint index = AttributeIndex(ns, name);
        return PoolString(strings_, pool_,
                          xml_.Call<jint>("nativeGetAttributeStringValue", "(JI)I", state_, index));
    }

    jint TypedAttribute(const char* name, jint type) const {
        const jint index = AttributeIndex(kAndroidNamespace, name);
        const jint attr_id = xml_.Call<jint>("nativeGetAttributeResource", "(JI)I", state_, index);
        Check((attr_id >> 24) == 0x01, "Expected an Android framework attribute resource ID");
        Check(xml_.Call<jint>("nativeGetAttributeDataType", "(JI)I", state_, index) == type,
              "Unexpected binary XML attribute type");
        return xml_.Call<jint>("nativeGetAttributeData", "(JI)I", state_, index);
    }

  private:
    jint AttributeIndex(std::string_view ns, const char* name) const {
        const jint count = xml_.Call<jint>("nativeGetAttributeCount", "(J)I", state_);
        for (jint i = 0; i < count; ++i) {
            const jint name_index = xml_.Call<jint>("nativeGetAttributeName", "(JI)I", state_, i);
            if (PoolString(strings_, pool_, name_index) != name) {
                continue;
            }
            const jint ns_index =
                xml_.Call<jint>("nativeGetAttributeNamespace", "(JI)I", state_, i);
            if (ns == (ns_index < 0 ? std::string{} : PoolString(strings_, pool_, ns_index))) {
                return i;
            }
        }
        Fail(std::string("Missing XML attribute ") + name);
    }

    const NativeClass& xml_;
    const NativeClass& strings_;
    jlong tree_;
    jlong pool_ = 0;
    jlong state_ = 0;
};

struct Manifest {
    std::string package;
    std::string version_name;
    std::string activity;
    std::string metadata_name;
    std::string metadata_value;
    jint version_code = 0;
    jint label_id = 0;

    bool operator==(const Manifest&) const = default;
};

Manifest ReadManifest(const NativeClass& xml, const NativeClass& strings, jlong tree) {
    BinaryXml parser(xml, strings, tree);
    Manifest result;
    std::vector<std::string> parents;
    for (jint event = parser.Next(); event != kEndDocument; event = parser.Next()) {
        if (event == kStartDocument) {
            continue;
        }
        Check(event == kStartTag || event == kEndTag, "Unexpected binary manifest event");
        std::string tag = parser.Tag();
        if (event == kEndTag) {
            Check(!parents.empty() && parents.back() == tag, "Unbalanced binary manifest tags");
            parents.pop_back();
            continue;
        }

        if (parents.empty() && tag == "manifest") {
            result.package = parser.StringAttribute("package", "");
            result.version_code = parser.TypedAttribute("versionCode", Res_value::TYPE_INT_DEC);
            result.version_name = parser.StringAttribute("versionName");
        } else if (parents.size() == 1 && parents.back() == "manifest" && tag == "application") {
            result.label_id = parser.TypedAttribute("label", Res_value::TYPE_REFERENCE);
        } else if (parents.size() == 2 && parents.back() == "application") {
            if (tag == "activity") {
                result.activity = parser.StringAttribute("name");
            } else if (tag == "meta-data") {
                result.metadata_name = parser.StringAttribute("name");
                result.metadata_value = parser.StringAttribute("value");
            }
        }
        parents.push_back(std::move(tag));
    }
    Check(parents.empty(), "Binary manifest ended before its closing tag");
    return result;
}

jint ResourceId(JNIEnv& env, const NativeClass& manager, jlong handle, const char* name) {
    const jint id = manager.Call<jint>(
        "nativeGetResourceIdentifier", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
        handle, env.NewStringUTF(name), env.NewStringUTF("string"), env.NewStringUTF(kPackage));
    Check(id != 0, std::string("Missing string resource ") + name);
    return id;
}

void SetLocale(JNIEnv& env, const NativeClass& manager, jlong handle, const char* locale) {
    const jobjectArray locales =
        env.NewObjectArray(1, env.FindClass("java/lang/String"), env.NewStringUTF(locale));
    manager.Call<void>("nativeSetConfiguration",
                       "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIIZ)V", handle, 0,
                       0, env.NewStringUTF("en-US"), locales, 0, 0, 0, 0, 0,
                       0,             // Input and orientation.
                       0, 0, 0, 0, 0, // Screen sizes.
                       0, 0, 0, 0,    // Layout, modes and grammatical gender.
                       36, 0, jboolean{JNI_FALSE});
}

jint StringIndex(JniDispatch& env, const NativeClass& manager, jlong handle, jint id) {
    auto* value = env.MakeObject<TypedValueRef>("android/util/TypedValue");
    const jint cookie =
        manager.Call<jint>("nativeGetResourceValue", "(JISLandroid/util/TypedValue;Z)I", handle, id,
                           jshort{0}, jobject{value}, jboolean{JNI_TRUE});
    env.CheckNoException();
    Check(cookie == kAssetCookie && value->cookie == cookie && value->id == id &&
              value->type == Res_value::TYPE_STRING,
          "Unexpected TypedValue for a string resource");
    return value->data;
}

void CheckLayout(const BinaryXml& parser, jint label_id) {
    bool root_seen = false;
    bool text_seen = false;
    for (jint event = parser.Next(); event != kEndDocument; event = parser.Next()) {
        Check(event == kStartDocument || event == kStartTag || event == kEndTag,
              "Unexpected compiled layout event");
        if (event != kStartTag) {
            continue;
        }
        if (!root_seen) {
            Check(parser.Tag() == "LinearLayout", "Unexpected layout root");
            root_seen = true;
        } else if (parser.Tag() == "TextView") {
            Check(parser.TypedAttribute("text", Res_value::TYPE_REFERENCE) == label_id,
                  "Layout text did not reference app_name");
            text_seen = true;
        }
    }
    Check(root_seen && text_seen, "Missing layout elements");
}

jbyteArray ReadEntry(JniDispatch& env, const NativeClass& manager, jlong handle, const char* name) {
    const jlong asset = manager.Call<jlong>("nativeOpenNonAsset", "(JILjava/lang/String;I)J",
                                            handle, kAssetCookie, env.NewStringUTF(name),
                                            static_cast<jint>(android::Asset::ACCESS_BUFFER));
    env.CheckNoException();
    Check(asset != 0, "Could not open non-asset entry");
    const jlong length = manager.Call<jlong>("nativeAssetGetLength", "(J)J", asset);
    Check(length > 0 && length <= std::numeric_limits<jsize>::max(), "Invalid APK entry length");
    const jsize size = static_cast<jsize>(length);
    const jbyteArray bytes = env.NewByteArray(size);
    const jint got = manager.Call<jint>("nativeAssetRead", "(J[BII)I", asset, bytes, jint{0}, size);
    manager.Call<void>("nativeAssetDestroy", "(J)V", asset);
    env.CheckNoException();
    Check(got == size, "Incomplete read of APK entry");
    return bytes;
}

void CheckZipEntry(const android::ZipFileRO& zip, const char* name,
                   const std::vector<jbyte>& jni_bytes) {
    const auto release = [&zip](void* entry) { zip.releaseEntry(entry); };
    const std::unique_ptr<void, decltype(release)> entry(zip.findEntryByName(name), release);
    Check(entry != nullptr, "ZipFileRO did not find the APK entry");
    std::vector<jbyte> zip_bytes(jni_bytes.size());
    Check(zip.uncompressEntry(entry.get(), zip_bytes.data(), zip_bytes.size()),
          "ZipFileRO could not read the APK entry");
    Check(jni_bytes == zip_bytes, "AssetManager and ZipFileRO returned different bytes");
}

void CheckPayloads(JniDispatch& env, const NativeClass& manager, jlong handle, const char* path) {
    const auto& dex = static_cast<ByteArray*>(ReadEntry(env, manager, handle, kDex))->values;
    const auto& elf = static_cast<ByteArray*>(ReadEntry(env, manager, handle, kCubeLib))->values;
    constexpr std::array<jbyte, 8> dex_magic{'d', 'e', 'x', '\n', '0', '3', '5', '\0'};
    constexpr std::array<jbyte, 7> elf_magic{0x7f, 'E', 'L', 'F', 2, 1, 1};
    Check(dex.size() >= 0x70 && std::equal(dex_magic.begin(), dex_magic.end(), dex.begin()),
          "classes.dex has an invalid DEX 035 header");
    Check(elf.size() >= 64 && std::equal(elf_magic.begin(), elf_magic.end(), elf.begin()) &&
              elf[16] == 3 && elf[17] == 0 && static_cast<uint8_t>(elf[18]) == 183 && elf[19] == 0,
          "The NDK cube is not an AArch64 ELF64 shared object");

    const std::unique_ptr<android::ZipFileRO> zip(android::ZipFileRO::open(path));
    Check(zip != nullptr, "ZipFileRO could not open wa_cube_app.apk");
    CheckZipEntry(*zip, kDex, dex);
    CheckZipEntry(*zip, kCubeLib, elf);
    std::printf("Verified DEX 035 (%zu bytes) and AArch64 ELF64 (%zu bytes)\n", dex.size(),
                elf.size());
}

} // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "Usage: test_aosp17_corejni_pm PATH_TO_WA_CUBE_APP_APK");
    JniDispatch env;
    Check(android::register_android_content_res_ApkAssets(&env) == JNI_OK, "Registering ApkAssets");
    Check(android::register_android_content_XmlBlock(&env) == JNI_OK, "Registering XmlBlock");
    Check(android::register_android_content_StringBlock(&env) == JNI_OK, "Registering StringBlock");
    Check(android::register_android_content_AssetManager(&env) == JNI_OK,
          "Registering AssetManager");
    env.CheckNoException();

    const NativeClass apk(env, "android/content/res/ApkAssets");
    const NativeClass xml(env, "android/content/res/XmlBlock");
    const NativeClass strings(env, "android/content/res/StringBlock");
    const NativeClass manager(env, "android/content/res/AssetManager");
    const jlong apk_handle = apk.Call<jlong>(
        "nativeLoad", "(ILjava/lang/String;ILandroid/content/res/loader/AssetsProvider;)J",
        kApkFormat, env.NewStringUTF(argv[1]), jint{0}, jobject{nullptr});
    env.CheckNoException();
    Check(apk_handle != 0, "ApkAssets.nativeLoad failed");
    Check(JniDispatch::Utf8(apk.Call<jstring>("nativeGetAssetPath", "(J)Ljava/lang/String;",
                                              apk_handle)) == argv[1],
          "Unexpected ApkAssets path");
    Check(apk.Call<jboolean>("nativeDefinesOverlayable", "(J)Z", apk_handle) == JNI_FALSE,
          "The fixture has no overlayable declarations");

    const jstring manifest_name = env.NewStringUTF(kManifest);
    constexpr char open_xml[] = "(JLjava/lang/String;)J";
    const auto manifest = ReadManifest(
        xml, strings, apk.Call<jlong>("nativeOpenXml", open_xml, apk_handle, manifest_name));
    Check(manifest.package == kPackage && manifest.version_code == 17 &&
              manifest.version_name == "17.0-cube" && manifest.label_id != 0,
          "Unexpected manifest package, version or label");
    Check(manifest.activity == "org.wasmandroid.cube.CubeActivity" &&
              manifest.metadata_name == "org.wasmandroid.cube.renderer" &&
              manifest.metadata_value == "gles3",
          "Unexpected manifest activity or metadata");
    std::printf("Manifest %s versionCode=%d versionName=%s activity=%s metadata=%s=%s\n",
                manifest.package.c_str(), manifest.version_code, manifest.version_name.c_str(),
                manifest.activity.c_str(), manifest.metadata_name.c_str(),
                manifest.metadata_value.c_str());

    constexpr char missing[] = "missing.xml";
    Check(apk.Call<jlong>("nativeOpenXml", open_xml, apk_handle, env.NewStringUTF(missing)) == 0,
          "ApkAssets opened a missing file");
    env.ExpectException("java/io/FileNotFoundException", missing);

    android::base::unique_fd fd(open(argv[1], O_RDONLY | O_CLOEXEC));
    Check(fd.ok(), "Could not open the fixture APK descriptor");
    const jlong fd_handle = apk.Call<jlong>(
        "nativeLoadFd",
        "(ILjava/io/FileDescriptor;Ljava/lang/String;ILandroid/content/res/loader/"
        "AssetsProvider;)J",
        kApkFormat, jobject{env.MakeObject<FileDescriptorRef>("java/io/FileDescriptor", fd.get())},
        env.NewStringUTF("cube via fd"), jint{0}, jobject{nullptr});
    env.CheckNoException();
    Check(fd_handle != 0, "ApkAssets.nativeLoadFd failed");
    fd.reset();
    Check(ReadManifest(xml, strings,
                       apk.Call<jlong>("nativeOpenXml", open_xml, fd_handle, manifest_name)) ==
              manifest,
          "The duplicate APK descriptor did not retain the manifest");
    apk.Call<void>("nativeDestroy", "(J)V", fd_handle);

    const jlong manager_handle = manager.Call<jlong>("nativeCreate", "()J");
    Check(manager_handle != 0, "AssetManager.nativeCreate failed");
    const jobjectArray assets = env.NewObjectArray(
        1, env.FindClass("android/content/res/ApkAssets"),
        env.MakeObject<ApkAssetsRef>("android/content/res/ApkAssets", apk_handle));
    manager.Call<void>("nativeSetApkAssets", "(J[Landroid/content/res/ApkAssets;ZZ)V",
                       manager_handle, assets, jboolean{JNI_TRUE}, jboolean{JNI_FALSE});
    env.CheckNoException();

    const jint label_id = ResourceId(env, manager, manager_handle, "app_name");
    Check(label_id == manifest.label_id, "Manifest label did not reference app_name");
    const jlong pool = apk.Call<jlong>("nativeGetStringBlock", "(J)J", apk_handle);
    Check(pool != 0, "Missing APK resource string pool");
    const auto check_locale = [&](const char* locale, const char* expected) {
        SetLocale(env, manager, manager_handle, locale);
        const auto value =
            PoolString(strings, pool, StringIndex(env, manager, manager_handle, label_id));
        Check(value == expected, std::string("Unexpected app_name for ") + locale);
        std::printf("%s app_name=%s\n", locale, value.c_str());
    };
    check_locale("en-US", "Wasm Android Cube");
    check_locale("zh-CN", "安卓立方体");
    check_locale("en-US", "Wasm Android Cube");

    const jint styled_id = ResourceId(env, manager, manager_handle, "styled_title");
    const jint styled_index = StringIndex(env, manager, manager_handle, styled_id);
    Check(PoolString(strings, pool, styled_index) == "Cube GLES3", "Unexpected styled text");
    const jintArray style =
        strings.Call<jintArray, jobject>("nativeGetStyle", "(JI)[I", pool, styled_index);
    Check(style != nullptr && env.GetArrayLength(style) == 3, "Expected one style span");
    const auto& span = static_cast<IntArray*>(style)->values;
    Check(PoolString(strings, pool, span[0]) == "b" && span[1] == 5 && span[2] == 9,
          "Unexpected bold span in styled_title");

    {
        const jlong layout = manager.Call<jlong, jobject>(
            "nativeOpenXmlAsset", "(JILjava/lang/String;)J", manager_handle, kAssetCookie,
            env.NewStringUTF("res/layout/activity_main.xml"));
        env.CheckNoException();
        CheckLayout(BinaryXml(xml, strings, layout), label_id);
    }
    const jbyteArray manifest_bytes = ReadEntry(env, manager, manager_handle, kManifest);
    const jlong xml_from_bytes = xml.Call<jlong, jobject>(
        "nativeCreate", "([BII)J", manifest_bytes, jint{0}, env.GetArrayLength(manifest_bytes));
    Check(ReadManifest(xml, strings, xml_from_bytes) == manifest,
          "XmlBlock.nativeCreate changed the manifest fields");
    CheckPayloads(env, manager, manager_handle, argv[1]);

    manager.Call<void>("nativeDestroy", "(J)V", manager_handle);
    apk.Call<void>("nativeDestroy", "(J)V", apk_handle);
    env.CheckNoException();
    return EXIT_SUCCESS;
}
