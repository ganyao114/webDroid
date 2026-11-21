#include <android-base/file.h>
#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/Util.h>
#include <ziparchive/zip_archive.h>
#include <ziparchive/zip_error.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using android::ApkAssets;
using android::Asset;
using android::AssetManager2;
using android::AssetsProvider;
using android::Res_value;
using android::ResTable_config;
using android::ResXMLParser;
using android::ResXMLTree;

constexpr char kPackage[] = "com.example.aosp17fixture";
constexpr char kAndroidNamespace[] = "http://schemas.android.com/apk/res/android";
constexpr uint32_t kAccentColor = 0xff336699;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "androidfw fixture: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

class MemoryAsset final : public Asset {
public:
    MemoryAsset(std::unique_ptr<uint8_t[]> bytes, size_t length)
        : bytes_(std::move(bytes)), length_(length) {
        mTakeData = [this] { return std::move(bytes_); };
        registerAsset(this);
    }

    ~MemoryAsset() override { unregisterAsset(this); }

    ssize_t read(void* buffer, size_t count) override {
        const size_t size = std::min(count, length_ - position_);
        if (size != 0) {
            std::memcpy(buffer, bytes_.get() + position_, size);
            position_ += size;
        }
        return size;
    }

    off64_t seek(off64_t offset, int whence) override {
        const auto position = handleSeek(offset, whence, position_, length_);
        if (position >= 0) {
            position_ = position;
        }
        return position;
    }

    void close() override {
        bytes_.reset();
        length_ = 0;
        position_ = 0;
    }

    const void* getBuffer(bool) override { return bytes_.get(); }
    android::incfs::map_ptr<void> getIncFsBuffer(bool) override { return bytes_.get(); }
    off64_t getLength() const override { return length_; }
    off64_t getRemainingLength() const override { return length_ - position_; }
    int openFileDescriptor(off64_t*, off64_t*) const override { return -1; }
    bool isAllocated() const override { return bytes_ != nullptr; }

private:
    std::unique_ptr<uint8_t[]> bytes_;
    size_t length_;
    size_t position_ = 0;
};

// Feed the actual APK bytes to the public AssetsProvider interface via upstream libziparchive.
class MemoryApkProvider final : public AssetsProvider {
public:
    explicit MemoryApkProvider(std::string bytes) : bytes_(std::move(bytes)) {
        ZipArchiveHandle handle = nullptr;
        const int32_t status = OpenArchiveFromMemory(bytes_.data(), bytes_.size(), name_.c_str(),
                                                     &handle);
        archive_.reset(handle);
        Check(status == kSuccess, "OpenArchiveFromMemory failed");
    }

    bool ForEachFile(const std::string& root,
                     android::base::function_ref<void(android::StringPiece, android::FileType)>
                         visit) const override {
        std::string prefix = root;
        if (!prefix.empty() && prefix.back() != '/') {
            prefix += '/';
        }
        void* raw_cookie = nullptr;
        if (StartIteration(archive_.get(), &raw_cookie, prefix) != kSuccess) {
            return false;
        }
        const std::unique_ptr<void, decltype(&EndIteration)> cookie(raw_cookie, EndIteration);
        std::map<std::string_view, android::FileType> children;
        ZipEntry64 entry{};
        std::string_view path;
        int32_t status;
        while ((status = Next(cookie.get(), &entry, &path)) == kSuccess) {
            const auto relative = path.substr(prefix.size());
            if (!relative.empty()) {
                const auto slash = relative.find('/');
                const auto type = slash == std::string_view::npos ? android::kFileTypeRegular
                                                                 : android::kFileTypeDirectory;
                children.emplace(relative.substr(0, slash), type);
            }
        }
        if (status != kIterationEnd) {
            return false;
        }
        for (const auto& [name, type] : children) {
            visit(name, type);
        }
        return true;
    }

    std::optional<std::string_view> GetPath() const override { return std::nullopt; }
    const std::string& GetDebugName() const override { return name_; }
    android::UpToDate IsUpToDate() const override { return android::UpToDate::Always; }

protected:
    std::unique_ptr<Asset> OpenInternal(const std::string& path, Asset::AccessMode,
                                        bool* file_exists, off64_t max_size) const override {
        ZipEntry64 entry{};
        const int32_t status = FindEntry(archive_.get(), path, &entry);
        if (file_exists != nullptr) {
            *file_exists = status != kEntryNotFound;
        }
        if (status != kSuccess || !std::in_range<size_t>(entry.uncompressed_length) ||
            (max_size >= 0 && std::cmp_greater(entry.uncompressed_length, max_size))) {
            return nullptr;
        }
        const size_t size = entry.uncompressed_length;
        auto data = std::make_unique_for_overwrite<uint8_t[]>(size);
        if (ExtractToMemory(archive_.get(), &entry, data.get(), size) != kSuccess) {
            return nullptr;
        }
        return std::make_unique<MemoryAsset>(std::move(data), size);
    }

private:
    const std::string bytes_;
    const std::string name_ = "sample APK in memory";
    std::unique_ptr<ZipArchive, decltype(&CloseArchive)> archive_{nullptr, CloseArchive};
};

android::ApkAssetsPtr LoadMemoryApk(const char* path) {
    std::string bytes;
    Check(android::base::ReadFileToString(path, &bytes), "read APK bytes for memory provider");
    return ApkAssets::Load(std::make_unique<MemoryApkProvider>(std::move(bytes)));
}

uint32_t ResourceId(const AssetManager2& manager, const char* name) {
    const auto id = manager.GetResourceId(name, {}, kPackage);
    Check(id.has_value(), name);
    return *id;
}

std::string ResourceString(const AssetManager2& manager, uint32_t id) {
    const auto value = manager.GetResource(id);
    Check(value.has_value(), "GetResource for string");
    Check(value->type == Res_value::TYPE_STRING, "resource value is a string");
    const auto* pool = manager.GetStringPoolForCookie(value->cookie);
    Check(pool != nullptr, "resource string pool exists");
    if (const auto utf8 = pool->string8At(value->data); utf8.has_value()) {
        return std::string(*utf8);
    }
    const auto utf16 = pool->stringAt(value->data);
    Check(utf16.has_value(), "read resource string");
    return android::util::Utf16ToUtf8(*utf16);
}

std::unique_ptr<ResXMLTree> OpenXml(const AssetManager2& manager, const char* path) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = manager.OpenNonAsset(path, Asset::ACCESS_BUFFER, &cookie);
    Check(asset != nullptr && cookie != android::kInvalidCookie, path);
    auto xml = ResXMLTree::fromAsset(std::move(asset), manager.GetDynamicRefTableForCookie(cookie));
    Check(xml != nullptr && xml->getError() == android::NO_ERROR, path);
    return xml;
}

void ExpectTag(ResXMLTree& xml, ResXMLParser::event_code_t event, std::u16string_view name) {
    auto next = xml.next();
    while (next == ResXMLParser::START_NAMESPACE || next == ResXMLParser::END_NAMESPACE) {
        next = xml.next();
    }
    Check(next == event, "unexpected binary XML event");
    size_t length = 0;
    const char16_t* actual = xml.getElementName(&length);
    Check(actual != nullptr && std::u16string_view(actual, length) == name, "binary XML tag name");
}

void ExpectEnd(ResXMLTree& xml) {
    auto next = xml.next();
    while (next == ResXMLParser::END_NAMESPACE) {
        next = xml.next();
    }
    Check(next == ResXMLParser::END_DOCUMENT, "end of binary XML document");
}

size_t AttributeIndex(const ResXMLTree& xml, const char* ns, const char* name) {
    const auto index = xml.indexOfAttribute(ns, name);
    Check(index >= 0, name);
    return static_cast<size_t>(index);
}

void ExpectStringAttribute(const ResXMLTree& xml, const char* ns, const char* name,
                           std::u16string_view expected) {
    const size_t index = AttributeIndex(xml, ns, name);
    size_t length = 0;
    const char16_t* actual = xml.getAttributeStringValue(index, &length);
    Check(xml.getAttributeDataType(index) == Res_value::TYPE_STRING && actual != nullptr &&
              std::u16string_view(actual, length) == expected,
          name);
}

uint32_t TypedAttribute(const ResXMLTree& xml, const char* name, uint8_t type) {
    const size_t index = AttributeIndex(xml, kAndroidNamespace, name);
    Check(xml.getAttributeDataType(index) == type, name);
    return static_cast<uint32_t>(xml.getAttributeData(index));
}

uint32_t CheckManifest(const AssetManager2& manager) {
    auto xml = OpenXml(manager, "AndroidManifest.xml");
    ExpectTag(*xml, ResXMLParser::START_TAG, u"manifest");
    ExpectStringAttribute(*xml, nullptr, "package", u"com.example.aosp17fixture");
    Check(TypedAttribute(*xml, "versionCode", Res_value::TYPE_INT_DEC) == 17, "versionCode");
    ExpectStringAttribute(*xml, kAndroidNamespace, "versionName", u"17.0-fixture");

    ExpectTag(*xml, ResXMLParser::START_TAG, u"uses-sdk");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"uses-sdk");
    ExpectTag(*xml, ResXMLParser::START_TAG, u"application");
    const uint32_t label = TypedAttribute(*xml, "label", Res_value::TYPE_REFERENCE);
    Check(label == ResourceId(manager, "string/app_name"), "application label reference");
    ExpectTag(*xml, ResXMLParser::START_TAG, u"activity");
    ExpectStringAttribute(*xml, kAndroidNamespace, "name", u".MainActivity");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"activity");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"application");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"manifest");
    ExpectEnd(*xml);

    std::printf("Manifest: %s, versionCode=17, versionName=17.0-fixture, activity=.MainActivity\n",
                kPackage);
    return label;
}

void CheckResources(AssetManager2& manager, uint32_t label, uint32_t greeting, uint32_t color,
                    const char* locale, std::string_view expected_label,
                    std::string_view expected_greeting) {
    ResTable_config config{};
    config.size = sizeof(config);
    config.setBcp47Locale(locale);
    manager.SetConfigurations({config});

    const auto label_text = ResourceString(manager, label);
    const auto greeting_text = ResourceString(manager, greeting);
    Check(label_text == expected_label, "localized application label");
    Check(greeting_text == expected_greeting, "localized greeting");
    const auto accent = manager.GetResource(color);
    Check(accent.has_value() && accent->type == Res_value::TYPE_INT_COLOR_ARGB8 &&
              accent->data == kAccentColor,
          "accent color ARGB value");
    std::printf("%s: label=%s, greeting=%s, color=0x%08x\n", locale, label_text.c_str(),
                greeting_text.c_str(), accent->data);
}

void CheckLayout(const AssetManager2& manager, uint32_t greeting, uint32_t color) {
    auto xml = OpenXml(manager, "res/layout/activity_main.xml");
    ExpectTag(*xml, ResXMLParser::START_TAG, u"LinearLayout");
    Check(TypedAttribute(*xml, "background", Res_value::TYPE_REFERENCE) == color,
          "layout background references the accent color");
    ExpectTag(*xml, ResXMLParser::START_TAG, u"TextView");
    Check(TypedAttribute(*xml, "text", Res_value::TYPE_REFERENCE) == greeting,
          "TextView text references the greeting");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"TextView");
    ExpectTag(*xml, ResXMLParser::END_TAG, u"LinearLayout");
    ExpectEnd(*xml);
    std::puts("Layout: LinearLayout and TextView resource references verified");
}

}  // namespace

int main(int argc, char** argv) {
    Check(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--memory"),
          "expected the sample APK path with optional --memory");
    const bool memory = argc == 3;
    const auto apk = memory ? LoadMemoryApk(argv[1]) : ApkAssets::Load(argv[1]);
    Check(apk != nullptr, "ApkAssets::Load failed");
    AssetManager2 manager;
    Check(manager.SetApkAssets({apk}), "AssetManager2::SetApkAssets failed");
    if (memory) {
        Check(!apk->GetPath().has_value() && manager.ContainsAllocatedTable(),
              "resources.arsc is backed by the in-memory provider");
    }

    const uint32_t label = CheckManifest(manager);
    const uint32_t greeting = ResourceId(manager, "string/greeting");
    const uint32_t color = ResourceId(manager, "color/accent");
    CheckResources(manager, label, greeting, color, "en-US", "AOSP 17 Fixture",
                   "Hello from Android 17!");
    CheckResources(manager, label, greeting, color, "zh-CN", "AOSP 17 测试应用", "你好，安卓 17！");
    CheckLayout(manager, greeting, color);
    return EXIT_SUCCESS;
}
