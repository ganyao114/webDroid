#include "apk_loader.h"

#include "apk_dex.h"
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
constexpr std::array kAbis{"armeabi-v7a", "arm64-v8a"};

struct Archive {
    explicit Archive(std::span<const uint8_t> input) : bytes(input.begin(), input.end()) {}

    std::span<const uint8_t> Stored(const ZipEntry64& entry) const {
        if (entry.method != kCompressStored || entry.compressed_length != entry.uncompressed_length ||
            !std::in_range<size_t>(entry.offset) || !std::in_range<size_t>(entry.uncompressed_length))
            return {};
        const auto offset = static_cast<size_t>(entry.offset);
        const auto length = static_cast<size_t>(entry.uncompressed_length);
        if (offset > bytes.size() || length > bytes.size() - offset)
            return {};
        return std::span(bytes).subspan(offset, length);
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

    const std::vector<uint8_t> bytes;
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
    std::string launcher;
    std::string library;
    std::string english;
    std::string chinese;
};

bool ParseManifest(AssetManager2& manager, Manifest& manifest) {
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
    bool main = false, launcher_category = false, launchable = false;
    for (auto event = xml->next(); event != ResXMLTree::END_DOCUMENT; event = xml->next()) {
        if (event == ResXMLTree::BAD_DOCUMENT)
            return false;
        if (event == ResXMLTree::END_TAG) {
            if (parents.empty())
                return false;
            if (parents.size() == 4 && parents.back() == u"intent-filter") {
                launchable |= main && launcher_category;
            } else if (parents.size() == 3 && parents.back() == u"activity" &&
                       launchable && manifest.launcher.empty() && !activity.empty()) {
                manifest.launcher = std::move(activity);
                manifest.library = std::move(activity_library);
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
        const auto depth = parents.size();
        if (depth == 0) {
            if (tag != u"manifest" || !manifest.package.empty())
                return false;
            manifest.package = AttributeString(*xml, manager, nullptr, "package");
            manifest.version = attr("versionName");
            const auto code = xml->indexOfAttribute(kAndroid, "versionCode");
            if (code >= 0) {
                const auto type = xml->getAttributeDataType(code);
                if (type == Res_value::TYPE_INT_DEC || type == Res_value::TYPE_INT_HEX)
                    manifest.version_code = xml->getAttributeData(code);
            }
        } else if (depth == 1 && tag == u"application") {
            const auto label = xml->indexOfAttribute(kAndroid, "label");
            if (label >= 0 && xml->getAttributeDataType(label) == Res_value::TYPE_REFERENCE)
                label_id = xml->getAttributeData(label);
            else
                literal_label = attr("label");
        } else if (depth >= 2 && parents[1] == u"application") {
            if (depth == 2 && tag == u"activity") {
                activity = attr("name");
                activity_library.clear();
                launchable = false;
            } else if (tag == u"meta-data" &&
                       (depth == 2 || (depth == 3 && parents[2] == u"activity")) &&
                       attr("name") == "android.app.lib_name") {
                const auto library = attr("value");
                (depth == 2 ? application_library : activity_library) = library;
            } else if (depth == 3 && parents[2] == u"activity" && tag == u"intent-filter") {
                main = launcher_category = false;
            } else if (depth == 4 && parents[2] == u"activity" && parents[3] == u"intent-filter") {
                if (tag == u"action")
                    main |= attr("name") == "android.intent.action.MAIN";
                if (tag == u"category")
                    launcher_category |= attr("name") == "android.intent.category.LAUNCHER";
            }
        }
        parents.push_back(tag);
    }
    if (!parents.empty() || manifest.package.empty() || manifest.package.contains('\0'))
        return false;
    if (manifest.library.empty())
        manifest.library = std::move(application_library);

    const auto resolve = [&](const char* locale) {
        android::ResTable_config configuration{};
        configuration.size = sizeof(configuration);
        configuration.setBcp47Locale(locale);
        manager.SetConfigurations({configuration});
        return label_id ? ResourceString(manager, label_id) : literal_label;
    };
    manifest.english = resolve("en-US");
    manifest.chinese = resolve("zh-CN");
    return true;
}

std::string LauncherDescriptor(const Manifest& manifest) {
    if (manifest.launcher.empty())
        return {};
    std::string name = manifest.launcher;
    if (name.front() == '.')
        name.insert(0, manifest.package);
    else if (!name.contains('.'))
        name.insert(0, manifest.package + '.');
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

struct wa_system_apk {
    std::shared_ptr<Archive> archive;
    std::unique_ptr<Asset> dex;
    Manifest manifest;
    wa_system_apk_info_t info{};
    std::array<std::optional<ZipEntry64>, kAbis.size()> native;
    std::vector<uint8_t> wasm;
    wa_system_apk_dex_stats_t dex_stats{};
    int32_t backing_fd = -1;
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
        apk->archive = std::make_shared<Archive>(std::span(apk_bytes, apk_len));
        ZipArchiveHandle zip = nullptr;
        const auto status = OpenArchiveFromMemory(apk->archive->bytes.data(), apk_len, kArchiveName, &zip);
        apk->archive->zip.reset(zip);
        if (status != kSuccess)
            return nullptr;
        const auto assets = android::ApkAssets::Load(std::make_unique<MemoryApkProvider>(apk->archive));
        AssetManager2 manager;
        if (!assets || !manager.SetApkAssets({assets}) || !ParseManifest(manager, apk->manifest))
            return nullptr;
        apk->dex = manager.OpenNonAsset("classes.dex", Asset::ACCESS_BUFFER);

        const auto& m = apk->manifest;
        if (LibraryNameValid(m.library)) {
            for (size_t i = 0; i < kAbis.size(); ++i) {
                const auto path = std::string("lib/") + kAbis[i] + "/lib" + m.library + ".so";
                ZipEntry64 entry{};
                if (FindEntry(zip, path, &entry) == kSuccess && entry.uncompressed_length &&
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
    const auto it = std::ranges::find(kAbis, std::string_view(abi));
    if (it == kAbis.end())
        return nullptr;
    const auto index = static_cast<size_t>(it - kAbis.begin());
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
