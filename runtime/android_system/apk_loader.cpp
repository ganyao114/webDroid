#include "apk_loader.h"

#include "apk_dex.h"
#include "apk_manifest.h"
#include "apk_entry_reader.h"
#include "elf_image.h"
#include "system_internal.h"
#include "wasmandroid/libkern.h"

#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/Util.h>
#include <ziparchive/zip_archive.h>
#include <ziparchive/zip_error.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {

using android::Asset;
using android::AssetManager2;
using android::Res_value;
using android::ResXMLTree;

constexpr char kArchiveName[] = "APK in memory";
constexpr char kAndroid[] = "http://schemas.android.com/apk/res/android";
constexpr std::array kAbis{"armeabi-v7a", "arm64-v8a", "x86", "x86_64", "riscv64", "armeabi"};
constexpr auto kRunnableAbis = std::span(kAbis).first<2>();

struct Archive {
    Archive(std::span<const uint8_t> input, bool copy_input)
        : backing(copy_input ? std::vector<uint8_t>(input.begin(), input.end()) : std::vector<uint8_t>()),
          bytes(copy_input ? std::span<const uint8_t>(backing) : input) {}

    std::span<const uint8_t> Stored(const ZipEntry64& entry) const {
        if (entry.method != kCompressStored || entry.compressed_length != entry.uncompressed_length ||
            !std::in_range<size_t>(entry.offset) || !std::in_range<size_t>(entry.uncompressed_length))
            return {};
        const auto offset = static_cast<size_t>(entry.offset);
        const auto length = static_cast<size_t>(entry.uncompressed_length);
        if (offset > bytes.size() || length > bytes.size() - offset)
            return {};
        return bytes.subspan(offset, length);
    }

    std::unique_ptr<uint8_t[]> Extract(const ZipEntry64& entry) const {
        if (!std::in_range<ptrdiff_t>(entry.uncompressed_length))
            return nullptr;
        const auto length = static_cast<size_t>(entry.uncompressed_length);
        auto output = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[length]);
        if (!output || ExtractToMemory(zip.get(), &entry, output.get(), length) != kSuccess)
            return nullptr;
        return output;
    }

    const std::vector<uint8_t> backing;
    const std::span<const uint8_t> bytes;
    std::unique_ptr<ZipArchive, decltype(&CloseArchive)> zip{nullptr, CloseArchive};
};

// ApkAssets accepts custom providers. The memory ZIP API sits beneath ZipFileRO,
// whose public open functions take file paths or descriptors.
class MemoryAsset final : public Asset {
  public:
    MemoryAsset(std::unique_ptr<uint8_t[]> bytes, size_t length)
        : bytes_(std::move(bytes)), length_(length) {
        mTakeData = [this] { return std::move(bytes_); };
        registerAsset(this);
    }

    ~MemoryAsset() override { unregisterAsset(this); }

    ssize_t read(void* out, size_t size) override {
        const size_t count = std::min(size, length_ - position_);
        if (count) {
            std::memcpy(out, bytes_.get() + position_, count);
            position_ += count;
        }
        return count;
    }

    off64_t seek(off64_t offset, int whence) override {
        const auto position = handleSeek(offset, whence, position_, length_);
        if (position >= 0)
            position_ = position;
        return position;
    }

    void close() override {
        bytes_.reset();
        length_ = position_ = 0;
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

class MemoryApkProvider final : public android::AssetsProvider {
  public:
    explicit MemoryApkProvider(std::shared_ptr<Archive> archive) : archive_(std::move(archive)) {}

    bool ForEachFile(const std::string& root,
                     android::base::function_ref<void(android::StringPiece, android::FileType)>
                         visit) const override {
        const auto prefix = root.empty() || root.back() == '/' ? root : root + '/';
        void* cookie = nullptr;
        if (StartIteration(archive_->zip.get(), &cookie, prefix) != kSuccess)
            return false;
        const std::unique_ptr<void, decltype(&EndIteration)> iteration(cookie, EndIteration);
        std::map<std::string, android::FileType> children;
        ZipEntry64 entry{};
        std::string_view path;
        int32_t status;
        while ((status = Next(cookie, &entry, &path)) == kSuccess) {
            const auto relative = path.substr(prefix.size());
            const auto slash = relative.find('/');
            if (!relative.empty()) {
                children.emplace(relative.substr(0, slash), slash == std::string_view::npos
                    ? android::kFileTypeRegular : android::kFileTypeDirectory);
            }
        }
        if (status != kIterationEnd)
            return false;
        for (const auto& [name, type] : children)
            visit(name, type);
        return true;
    }

    std::optional<std::string_view> GetPath() const override { return std::nullopt; }
    const std::string& GetDebugName() const override { return name_; }
    android::UpToDate IsUpToDate() const override { return android::UpToDate::Always; }

  private:
    std::unique_ptr<Asset> OpenInternal(const std::string& path, Asset::AccessMode,
                                        bool* file_exists, off64_t max_size) const override {
        ZipEntry64 entry{};
        const auto status = FindEntry(archive_->zip.get(), path, &entry);
        if (file_exists)
            *file_exists = status != kEntryNotFound;
        if (status != kSuccess || (max_size >= 0 && std::cmp_greater(entry.uncompressed_length, max_size)))
            return nullptr;
        auto bytes = archive_->Extract(entry);
        return bytes ? std::make_unique<MemoryAsset>(std::move(bytes), entry.uncompressed_length)
                     : nullptr;
    }

    std::shared_ptr<Archive> archive_;
    const std::string name_ = kArchiveName;
};

android::ApkAssetsPtr OpenAssets(std::span<const uint8_t> bytes,
                                std::shared_ptr<Archive>& archive, bool copy_input = true) {
    archive = std::make_shared<Archive>(bytes, copy_input);
    ZipArchiveHandle zip = nullptr;
    const auto status =
        OpenArchiveFromMemory(archive->bytes.data(), archive->bytes.size(), kArchiveName, &zip);
    archive->zip.reset(zip);
    if (status != kSuccess)
        return nullptr;
    return android::ApkAssets::Load(std::make_unique<MemoryApkProvider>(archive));
}

int32_t EmbeddedAbis(ZipArchiveHandle zip, int32_t& mask) {
    void* cookie = nullptr;
    if (StartIteration(zip, &cookie, "lib/", ".so") != kSuccess)
        return WA_ERR_INVALID;
    const std::unique_ptr<void, decltype(&EndIteration)> iteration(cookie, EndIteration);
    ZipEntry64 entry{};
    std::string_view path;
    int32_t status;
    while ((status = Next(cookie, &entry, &path)) == kSuccess) {
        path.remove_prefix(4);
        const auto slash = path.find('/');
        if (slash == path.npos || path.find('/', slash + 1) != path.npos ||
            !entry.uncompressed_length)
            continue;
        const auto abi = std::ranges::find(kAbis, path.substr(0, slash));
        if (abi != kAbis.end())
            mask |= 1 << (abi - kAbis.begin());
    }
    return status == kIterationEnd ? WA_OK : WA_ERR_INVALID;
}

std::string ResourceString(const AssetManager2& manager, uint32_t id) {
    auto value = manager.GetResource(id);
    if (!value || !manager.ResolveReference(*value) || value->type != Res_value::TYPE_STRING)
        return {};
    const auto* pool = manager.GetStringPoolForCookie(value->cookie);
    if (!pool)
        return {};
    if (const auto utf8 = pool->string8At(value->data); utf8)
        return std::string(*utf8);
    if (const auto utf16 = pool->stringAt(value->data); utf16)
        return android::util::Utf16ToUtf8(*utf16);
    return {};
}

std::string AttributeString(const ResXMLTree& xml, const AssetManager2& manager,
                            const char* ns, const char* name) {
    const auto index = xml.indexOfAttribute(ns, name);
    if (index < 0)
        return {};
    const auto type = xml.getAttributeDataType(index);
    if (type == Res_value::TYPE_REFERENCE)
        return ResourceString(manager, xml.getAttributeData(index));
    if (type != Res_value::TYPE_STRING)
        return {};
    size_t length = 0;
    const auto* str = xml.getAttributeStringValue(index, &length);
    return str ? android::util::Utf16ToUtf8({str, length}) : std::string{};
}

struct Manifest {
    std::string package;
    std::string version;
    uint32_t version_code = 0;
    uint32_t version_major = 0;
    std::string launcher;
    std::string home;
    std::vector<std::string> activities;
    std::string library;
    std::string english;
    std::string chinese;
};

bool IsActivity(std::u16string_view tag) {
    return tag == u"activity" || tag == u"activity-alias";
}

bool ParseManifest(AssetManager2& manager, Manifest& manifest,
                   wa::system::LauncherPackage* indexed = nullptr) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = manager.OpenNonAsset("AndroidManifest.xml", Asset::ACCESS_BUFFER, &cookie);
    if (!asset || cookie == android::kInvalidCookie)
        return false;
    const auto xml = ResXMLTree::fromAsset(std::move(asset), manager.GetDynamicRefTableForCookie(cookie));
    if (!xml || xml->getError() != android::NO_ERROR)
        return false;

    std::vector<std::u16string_view> parents;
    std::string activity, activity_library, application_library, literal_label;
    uint32_t label_id = 0;
    bool main = false, launcher_category = false, home_category = false;
    bool launchable = false, home = false;
    wa::system::ApkComponent component;
    std::optional<bool> exported;
    for (auto event = xml->next(); event != ResXMLTree::END_DOCUMENT; event = xml->next()) {
        if (event == ResXMLTree::BAD_DOCUMENT)
            return false;
        if (event == ResXMLTree::END_TAG) {
            if (parents.empty())
                return false;
            if (parents.size() == 4 && parents[1] == u"application" && IsActivity(parents[2]) &&
                parents.back() == u"intent-filter") {
                launchable |= main && launcher_category;
                home |= main && home_category;
            } else if (parents.size() == 3 && parents[1] == u"application" &&
                       IsActivity(parents.back()) && !activity.empty()) {
                const auto name = wa::system::ActivityClassName(manifest.package, activity);
                manifest.activities.push_back(name);
                if (indexed) {
                    component.name = name;
                    component.exported = exported.value_or(!component.filters.empty());
                    component.enabled &= indexed->enabled;
                    indexed->activities.push_back(std::move(component));
                }
                if (launchable && manifest.launcher.empty()) {
                    manifest.launcher = activity;
                    manifest.library = std::move(activity_library);
                }
                if (home && manifest.home.empty())
                    manifest.home = activity;
            }
            parents.pop_back();
            continue;
        }
        if (event != ResXMLTree::START_TAG)
            continue;
        size_t length = 0;
        const char16_t* name = xml->getElementName(&length);
        if (!name)
            return false;
        const std::u16string_view tag(name, length);
        const auto attr = [&](const char* key) { return AttributeString(*xml, manager, kAndroid, key); };
        const auto typed = [&](const char* key, uint8_t type, uint32_t fallback) -> uint32_t {
            const auto index = xml->indexOfAttribute(kAndroid, key);
            return index >= 0 && xml->getAttributeDataType(index) == type
                ? xml->getAttributeData(index) : fallback;
        };
        const auto depth = parents.size();
        if (depth == 0) {
            if (tag != u"manifest" || !manifest.package.empty())
                return false;
            manifest.package = AttributeString(*xml, manager, nullptr, "package");
            manifest.version = attr("versionName");
            const auto versionCode = [&](const char* key) -> uint32_t {
                const auto code = xml->indexOfAttribute(kAndroid, key);
                if (code < 0)
                    return 0;
                const auto type = xml->getAttributeDataType(code);
                return (type == Res_value::TYPE_INT_DEC || type == Res_value::TYPE_INT_HEX)
                    ? xml->getAttributeData(code) : 0;
            };
            manifest.version_code = versionCode("versionCode");
            manifest.version_major = versionCode("versionCodeMajor");
        } else if (indexed && depth == 1 && tag == u"uses-sdk") {
            const auto target = xml->indexOfAttribute(kAndroid, "targetSdkVersion");
            if (target >= 0) {
                const auto type = xml->getAttributeDataType(target);
                if (type == Res_value::TYPE_INT_DEC || type == Res_value::TYPE_INT_HEX) {
                    indexed->target_sdk = xml->getAttributeData(target);
                }
            }
        } else if (depth == 1 && tag == u"application") {
            const auto label = xml->indexOfAttribute(kAndroid, "label");
            if (label >= 0 && xml->getAttributeDataType(label) == Res_value::TYPE_REFERENCE)
                label_id = xml->getAttributeData(label);
            else
                literal_label = attr("label");
            if (indexed) {
                indexed->application = wa::system::ActivityClassName(manifest.package, attr("name"));
                indexed->process = attr("process");
                if (indexed->process.empty()) indexed->process = manifest.package;
                else if (indexed->process.starts_with(':')) indexed->process.insert(0, manifest.package);
                indexed->icon = typed("icon", Res_value::TYPE_REFERENCE, 0);
                indexed->enabled = typed("enabled", Res_value::TYPE_INT_BOOLEAN, 1) != 0;
            }
        } else if (depth >= 2 && parents[1] == u"application") {
            if (depth == 2 && IsActivity(tag)) {
                activity = attr("name");
                activity_library.clear();
                launchable = home = false;
                if (indexed) {
                    component = {};
                    component.icon = typed("icon", Res_value::TYPE_REFERENCE, indexed->icon);
                    component.launch_mode = typed("launchMode", Res_value::TYPE_INT_DEC, 0);
                    component.label = attr("label");
                    component.enabled = typed("enabled", Res_value::TYPE_INT_BOOLEAN, 1) != 0;
                    const auto value = typed("exported", Res_value::TYPE_INT_BOOLEAN, 2);
                    exported = value == 2 ? std::nullopt : std::optional(value != 0);
                }
            } else if (tag == u"meta-data" &&
                       (depth == 2 || (depth == 3 && IsActivity(parents[2]))) &&
                       attr("name") == "android.app.lib_name") {
                const auto library = attr("value");
                (depth == 2 ? application_library : activity_library) = library;
            } else if (depth == 3 && IsActivity(parents[2]) && tag == u"intent-filter") {
                main = launcher_category = home_category = false;
                if (indexed)
                    component.filters.emplace_back();
            } else if (depth == 4 && IsActivity(parents[2]) && parents[3] == u"intent-filter") {
                if (tag == u"action") {
                    const auto action = attr("name");
                    main |= action == "android.intent.action.MAIN";
                    if (indexed && !action.empty())
                        component.filters.back().actions.push_back(action);
                }
                if (tag == u"category") {
                    const auto category = attr("name");
                    launcher_category |= category == "android.intent.category.LAUNCHER";
                    home_category |= category == "android.intent.category.HOME";
                    if (indexed && !category.empty())
                        component.filters.back().categories.push_back(category);
                }
                if (tag == u"data" && indexed) {
                    auto& filter = component.filters.back();
                    auto scheme = attr("scheme");
                    auto mime = attr("mimeType");
                    if (!scheme.empty())
                        filter.schemes.push_back(std::move(scheme));
                    if (!mime.empty())
                        filter.mime_types.push_back(std::move(mime));
                    for (const char* key : {"host", "port", "path", "pathPrefix", "pathPattern",
                                            "pathAdvancedPattern", "pathSuffix", "ssp", "sspPrefix",
                                            "sspPattern", "sspAdvancedPattern", "sspSuffix"}) {
                        filter.restricted_uri |= xml->indexOfAttribute(kAndroid, key) >= 0;
                    }
                }
            }
        }
        parents.push_back(tag);
    }
    if (!parents.empty() || manifest.package.empty() || manifest.package.contains('\0'))
        return false;
    if (manifest.library.empty())
        manifest.library = std::move(application_library);
    if (indexed) {
        indexed->package = manifest.package;
        indexed->label = label_id ? ResourceString(manager, label_id) : literal_label;
        if (indexed->label.empty())
            indexed->label = indexed->package;
        for (auto& entry : indexed->activities) {
            if (entry.label.empty())
                entry.label = indexed->label;
        }
        return true;
    }

    const auto resolve = [&](const char* locale) {
        android::ResTable_config configuration{};
        configuration.size = sizeof(configuration);
        configuration.setBcp47Locale(locale);
        manager.SetConfigurations({configuration});
        return label_id ? ResourceString(manager, label_id) : literal_label;
    };
    manifest.english = resolve("en-US");
    if (manifest.english.empty())
        manifest.english = resolve("");
    manifest.chinese = resolve("zh-CN");
    return true;
}

std::string LauncherDescriptor(const Manifest& manifest) {
    if (manifest.launcher.empty())
        return {};
    auto name = wa::system::ActivityClassName(manifest.package, manifest.launcher);
    std::replace(name.begin(), name.end(), '.', '/');
    return 'L' + name + ';';
}

bool LibraryNameValid(std::string_view name) {
    return !name.empty() && name.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.+-") == name.npos;
}

struct NativeBacking {
    std::shared_ptr<Archive> archive;
    std::unique_ptr<uint8_t[]> extracted;
    std::span<const uint8_t> bytes;
    int32_t fd = -1;
    off64_t zip_offset = 0;

    static int64_t Read(void* user, uint32_t, void* out, size_t size, uint64_t offset) {
        const auto& backing = *static_cast<NativeBacking*>(user);
        if (offset > backing.bytes.size() || size > backing.bytes.size() - offset)
            return WA_ERR_INVALID;
        if (backing.fd >= 0)
            return wa_vfs_pread(backing.fd, out, size, backing.zip_offset + static_cast<int64_t>(offset));
        std::memcpy(out, backing.bytes.data() + offset, size);
        return size;
    }
};

} // namespace

std::string wa::system::ActivityClassName(std::string_view package, std::string_view name) {
    if (name.empty())
        return {};
    if (name.front() == '.')
        return std::string(package).append(name);
    if (!name.contains('.'))
        return std::string(package).append(".").append(name);
    return std::string(name);
}

int32_t wa::system::ReadApkManifest(std::span<const uint8_t> bytes, ApkManifest& out) {
    if (bytes.empty() || !std::in_range<ptrdiff_t>(bytes.size()))
        return WA_ERR_INVALID;
    try {
        std::shared_ptr<Archive> archive;
        const auto assets = OpenAssets(bytes, archive);
        AssetManager2 manager;
        Manifest manifest;
        int32_t abi_mask = 0;
        if (!assets || !manager.SetApkAssets({assets}) || !ParseManifest(manager, manifest) ||
            EmbeddedAbis(archive->zip.get(), abi_mask) != WA_OK)
            return WA_ERR_INVALID;
        const auto version = std::bit_cast<int64_t>(
            uint64_t{manifest.version_major} << 32 | manifest.version_code);
        out = {manifest.package, manifest.english.empty() ? manifest.package : manifest.english,
               version, ActivityClassName(manifest.package, manifest.launcher),
               ActivityClassName(manifest.package, manifest.home), std::move(manifest.activities),
               abi_mask};
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_INVALID;
    }
}

namespace {

int32_t IndexLauncherPackage(const android::ApkAssetsPtr& assets, const char* locale,
                             int density, wa::system::LauncherPackage& out) {
    AssetManager2 manager;
    if (!assets || !manager.SetApkAssets({assets}))
        return WA_ERR_INVALID;
    android::ResTable_config configuration{};
    configuration.size = sizeof(configuration);
    configuration.setBcp47Locale(locale);
    configuration.density = density;
    configuration.sdkVersion = 37;
    manager.SetConfigurations({configuration});
    Manifest manifest;
    wa::system::LauncherPackage indexed;
    if (!ParseManifest(manager, manifest, &indexed))
        return WA_ERR_INVALID;
    out = std::move(indexed);
    return WA_OK;
}

} // namespace

int32_t wa::system::ReadLauncherPackage(std::span<const uint8_t> bytes, const char* locale,
                                        int density, LauncherPackage& out) {
    if (bytes.empty() || !std::in_range<ptrdiff_t>(bytes.size()))
        return WA_ERR_INVALID;
    std::shared_ptr<Archive> archive;
    // This method copies metadata out of the resource table before returning.
    return IndexLauncherPackage(OpenAssets(bytes, archive, /*copy_input=*/false), locale, density, out);
}

int32_t wa::system::ReadLauncherPackage(const char* path, const char* locale, int density,
                                        LauncherPackage& out) {
    if (!path || !*path) return WA_ERR_INVALID;
    return IndexLauncherPackage(android::ApkAssets::Load(path), locale, density, out);
}

struct wa_system_apk {
    std::shared_ptr<Archive> archive;
    std::unique_ptr<Asset> dex;
    Manifest manifest;
    wa_system_apk_info_t info{};
    std::array<std::optional<ZipEntry64>, kRunnableAbis.size()> native;
    std::vector<uint8_t> wasm;
    wa_system_apk_dex_stats_t dex_stats{};
    int32_t backing_fd = -1;
    std::optional<bool> legacy_cube;
};

#ifdef __EMSCRIPTEN__
static_assert(sizeof(wa_system_apk_info_t) == 36 && offsetof(wa_system_apk_info_t, version_code) == 24);
static_assert(sizeof(wa_system_apk_dex_stats_t) == 24);
#endif

wa_system_apk_t* wa_system_open_apk(const uint8_t* apk_bytes, size_t apk_len) {
    if (!apk_bytes || !apk_len || !std::in_range<ptrdiff_t>(apk_len))
        return nullptr;
    try {
        auto apk = std::make_unique<wa_system_apk_t>();
        const auto assets = OpenAssets({apk_bytes, apk_len}, apk->archive);
        AssetManager2 manager;
        if (!assets || !manager.SetApkAssets({assets}) || !ParseManifest(manager, apk->manifest))
            return nullptr;
        apk->dex = manager.OpenNonAsset("classes.dex", Asset::ACCESS_BUFFER);

        const auto& m = apk->manifest;
        if (LibraryNameValid(m.library)) {
            for (size_t i = 0; i < kRunnableAbis.size(); ++i) {
                const auto path = std::string("lib/") + kRunnableAbis[i] + "/lib" + m.library + ".so";
                ZipEntry64 entry{};
                if (FindEntry(apk->archive->zip.get(), path, &entry) == kSuccess &&
                    entry.uncompressed_length &&
                    std::in_range<ptrdiff_t>(entry.uncompressed_length) &&
                    (entry.method == kCompressDeflated || !apk->archive->Stored(entry).empty()))
                    apk->native[i] = entry;
            }
        }
        const auto size = [&](size_t i) -> size_t {
            return apk->native[i] ? apk->native[i]->uncompressed_length : 0;
        };
        apk->info = {m.package.c_str(), m.version.c_str(), m.launcher.c_str(), m.library.c_str(),
                     m.english.c_str(), m.chinese.c_str(), m.version_code, size(0), size(1)};
        return apk.release();
    } catch (const std::exception&) {
        return nullptr;
    }
}

void wa_system_close_apk(wa_system_apk_t* apk) { delete apk; }

const wa_system_apk_info_t* wa_system_apk_info(const wa_system_apk_t* apk) {
    return apk ? &apk->info : nullptr;
}

namespace {

int32_t FindLegacyCube(ZipArchiveHandle zip, std::span<const uint8_t> bytes, std::string_view package,
                       decltype(wa_system_apk::native)& candidates, std::string& library_name) {
    const bool cube_package = package == "org.wasmandroid.cube" || package == "com.wasmandroid.cube";
    for (size_t i = 0; i < kRunnableAbis.size(); ++i) {
        const auto prefix = std::string("lib/") + kRunnableAbis[i] + "/";
        void* cookie = nullptr;
        if (StartIteration(zip, &cookie, prefix, ".so") != kSuccess)
            return WA_ERR_INVALID;
        const std::unique_ptr<void, decltype(&EndIteration)> iteration(cookie, EndIteration);
        ZipEntry64 entry{};
        std::string_view path;
        while (Next(cookie, &entry, &path) == kSuccess) {
            const auto name = path.substr(prefix.size());
            if (!name.starts_with("lib") || name.contains('/') || !entry.uncompressed_length) continue;
            bool match = cube_package && name.starts_with("libndk_gles3_cube");
            if (!match) {
                const wa::system::ApkZipEntry contents{
                    entry.offset, entry.compressed_length, entry.uncompressed_length,
                    entry.crc32, entry.method};
                wa::system::ApkEntryReader file(GetFileDescriptor(zip), contents, bytes, nullptr, path);
                wa::dbt::ElfInfo elf;
                const auto arch = i == 0 ? WA_ARCH_ARM32 : WA_ARCH_ARM64;
                match = wa::dbt::InspectElf(arch, file.Elf(), elf, true) == WA_OK &&
                    std::ranges::binary_search(elf.exports, "wa_demo_init") &&
                    std::ranges::binary_search(elf.exports, "wa_demo_render_frame");
            }
            if (!match) continue;
            candidates[i] = entry;
            if (library_name.empty()) library_name = name.substr(3, name.size() - 6);
            break;
        }
    }
    return candidates[0] || candidates[1];
}

int32_t IsLegacyCubeApkFile(const char* path, std::string_view expected_package) {
    wa::system::LauncherPackage info;
    const auto status = wa::system::ReadLauncherPackage(path, "en-US", 160, info);
    if (status != WA_OK || info.package != expected_package) return WA_ERR_INVALID;
    ZipArchiveHandle handle = nullptr;
    const auto opened = OpenArchive(path, &handle);
    const std::unique_ptr<ZipArchive, decltype(&CloseArchive)> zip(handle, CloseArchive);
    if (opened != kSuccess) return WA_ERR_INVALID;
    decltype(wa_system_apk::native) candidates;
    std::string library_name;
    return FindLegacyCube(zip.get(), {}, info.package, candidates, library_name);
}

} // namespace

extern "C" int32_t wa_system_apk_is_legacy_cube_file(const char* path, const char* package) {
    if (!path || !package) return WA_ERR_INVALID;
    try {
        return IsLegacyCubeApkFile(path, package);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_INVALID;
    }
}

int32_t wa_system_apk_is_legacy_cube(wa_system_apk_t* apk) {
    if (!apk) return WA_ERR_INVALID;
    if (apk->legacy_cube) return *apk->legacy_cube;
    try {
        decltype(apk->native) candidates;
        std::string library_name;
        const auto legacy = FindLegacyCube(apk->archive->zip.get(), apk->archive->bytes,
            apk->manifest.package, candidates, library_name);
        if (legacy < 0) return legacy;
        if (legacy) {
            apk->native = std::move(candidates);
            apk->manifest.library = std::move(library_name);
            apk->info.native_library = apk->manifest.library.c_str();
            apk->info.arm32_library_size = apk->native[0] ? apk->native[0]->uncompressed_length : 0;
            apk->info.arm64_library_size = apk->native[1] ? apk->native[1]->uncompressed_length : 0;
        }
        apk->legacy_cube = legacy;
        return legacy;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_INVALID;
    }
}

int32_t wa_system_apk_compile_dex(wa_system_apk_t* apk, const uint8_t** out_wasm,
                                 size_t* out_wasm_len) {
    if (!out_wasm || !out_wasm_len)
        return WA_ERR_INVALID;
    *out_wasm = nullptr;
    *out_wasm_len = 0;
    if (!apk)
        return WA_ERR_INVALID;
    if (!apk->dex || apk->dex->getLength() <= 0)
        return WA_ERR_NOENT;
    try {
        if (apk->wasm.empty()) {
            const auto* data = static_cast<const uint8_t*>(apk->dex->getBuffer(true));
            if (!data)
                return WA_ERR_INVALID;
            const auto status = wa::system::CompileApkDex(
                {data, static_cast<size_t>(apk->dex->getLength())}, LauncherDescriptor(apk->manifest),
                apk->wasm, apk->dex_stats);
            if (status != WA_OK)
                return status;
        }
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (const std::exception&) {
        return WA_ERR_NOTSUP;
    }
    *out_wasm = apk->wasm.data();
    *out_wasm_len = apk->wasm.size();
    return WA_OK;
}

const wa_system_apk_dex_stats_t* wa_system_apk_dex_stats(const wa_system_apk_t* apk) {
    return apk && !apk->wasm.empty() ? &apk->dex_stats : nullptr;
}

int32_t wa_system_apk_set_backing_file(wa_system_apk_t* apk, int32_t fd) {
    if (!apk)
        return WA_ERR_INVALID;
    apk->backing_fd = fd;
    return WA_OK;
}

wa_system_guest_t* wa_system_apk_load_native(const wa_system_apk_t* apk, const char* abi) {
    if (!apk || !abi)
        return nullptr;
    const auto it = std::ranges::find(kRunnableAbis, std::string_view(abi));
    if (it == kRunnableAbis.end())
        return nullptr;
    const auto index = static_cast<size_t>(it - kRunnableAbis.begin());
    if (!apk->native[index])
        return nullptr;
    const auto& entry = *apk->native[index];
    try {
        auto backing = std::make_shared<NativeBacking>();
        if (entry.method == kCompressStored) {
            backing->archive = apk->archive;
            backing->bytes = backing->archive->Stored(entry);
            backing->fd = apk->backing_fd;
            backing->zip_offset = entry.offset;
        } else {
            backing->extracted = apk->archive->Extract(entry);
            if (!backing->extracted)
                return nullptr;
            backing->bytes = {backing->extracted.get(), static_cast<size_t>(entry.uncompressed_length)};
        }
        wa_system_guest_t* guest = nullptr;
        const auto arch = index == 0 ? WA_ARCH_ARM32 : WA_ARCH_ARM64;
        if (wa_system_load(arch, 0, backing->bytes.data(), backing->bytes.size(), NativeBacking::Read,
                           backing.get(), &guest) != WA_OK)
            return nullptr;
        wa::system::RetainGuestBacking(guest, std::move(backing));
        return guest;
    } catch (const std::exception&) {
        return nullptr;
    }
}
