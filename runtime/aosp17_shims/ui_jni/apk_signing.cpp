#include "apk_signing.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>

namespace wa::ui_jni::detail {
namespace {

using Bytes = std::span<const uint8_t>;
constexpr size_t kZipEndSize = 22;
constexpr uint32_t kZipEndMagic = 0x06054b50;
constexpr uint32_t kV2BlockId = 0x7109871a;
constexpr uint32_t kV3BlockId = 0xf05368c0;
constexpr uint32_t kV31BlockId = 0x1b93ad61;
constexpr char kSigningMagic[] = "APK Sig Block 42";

uint64_t LittleEndian(Bytes bytes) {
    uint64_t result = 0;
    for (size_t i = 0; i < bytes.size(); ++i) result |= uint64_t{bytes[i]} << (8 * i);
    return result;
}

bool Read(std::ifstream& file, uint64_t offset, std::span<uint8_t> output) {
    return bool(file.seekg(static_cast<std::streamoff>(offset)) &&
                file.read(reinterpret_cast<char*>(output.data()), output.size()));
}

std::optional<uint64_t> CentralDirectory(std::ifstream& file, uint64_t size) {
    if (size < kZipEndSize) return std::nullopt;
    std::vector<uint8_t> tail(std::min<uint64_t>(size, kZipEndSize + UINT16_MAX));
    if (!Read(file, size - tail.size(), tail)) return std::nullopt;
    for (size_t comment = 0; comment <= tail.size() - kZipEndSize; ++comment) {
        const auto end = Bytes(tail).subspan(tail.size() - kZipEndSize - comment, kZipEndSize);
        if (LittleEndian(end.first<4>()) != kZipEndMagic ||
            LittleEndian(end.subspan<20, 2>()) != comment) continue;
        const auto directory_size = LittleEndian(end.subspan<12, 4>());
        const auto directory_offset = LittleEndian(end.subspan<16, 4>());
        if (LittleEndian(end.subspan<4, 4>()) != 0 ||
            directory_offset + directory_size != size - comment - kZipEndSize) return std::nullopt;
        return directory_offset;
    }
    return std::nullopt;
}

std::optional<Bytes> TakeBlock(Bytes& bytes) {
    if (bytes.size() < 4) return std::nullopt;
    const auto size = LittleEndian(bytes.first<4>());
    bytes = bytes.subspan(4);
    if (size > bytes.size()) return std::nullopt;
    const auto block = bytes.first(static_cast<size_t>(size));
    bytes = bytes.subspan(block.size());
    return block;
}

std::vector<std::vector<uint8_t>> SignerCertificates(Bytes entry) {
    auto signers = TakeBlock(entry);
    if (!signers || !entry.empty()) return {};
    std::vector<std::vector<uint8_t>> result;
    while (!signers->empty()) {
        auto signer = TakeBlock(*signers);
        if (!signer) return {};
        auto signed_data = TakeBlock(*signer);
        const auto signatures = TakeBlock(*signer);
        const auto key = TakeBlock(*signer);
        if (!signed_data || !signatures || signatures->empty() || !key || key->empty() ||
            !signer->empty()) return {};
        const auto digests = TakeBlock(*signed_data);
        auto certificates = TakeBlock(*signed_data);
        if (!digests || digests->empty() || !certificates) return {};
        const auto leaf = TakeBlock(*certificates);
        if (!leaf || leaf->empty()) return {};
        while (!certificates->empty()) {
            const auto issuer = TakeBlock(*certificates);
            if (!issuer || issuer->empty()) return {};
        }
        result.emplace_back(leaf->begin(), leaf->end());
    }
    return result;
}

} // namespace

std::vector<std::vector<uint8_t>> ReadApkV2Signers(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size < 0) return {};
    const auto directory = CentralDirectory(file, static_cast<uint64_t>(size));
    std::array<uint8_t, 24> footer{};
    if (!directory || *directory < footer.size() + 8) return {};
    const auto end = *directory - footer.size();
    if (!Read(file, end, footer) ||
        std::memcmp(footer.data() + 8, kSigningMagic, sizeof(kSigningMagic) - 1) != 0) return {};
    const auto block_size = LittleEndian(Bytes(footer).first<8>());
    if (block_size < footer.size() || block_size > *directory - 8) return {};
    auto offset = *directory - block_size;
    std::array<uint8_t, 8> header{};
    if (!Read(file, offset - header.size(), header) || LittleEndian(header) != block_size) return {};

    std::optional<std::pair<uint64_t, uint64_t>> v2;
    while (offset < end) {
        std::array<uint8_t, 12> entry{};
        if (entry.size() > end - offset || !Read(file, offset, entry)) return {};
        const auto length = LittleEndian(Bytes(entry).first<8>());
        if (length < 4 || length > end - offset - 8) return {};
        switch (LittleEndian(Bytes(entry).last<4>())) {
        case kV3BlockId:
        case kV31BlockId:
            // Later schemes can rotate the signing identity.
            return {};
        case kV2BlockId:
            if (v2) return {};
            v2.emplace(offset + entry.size(), length - 4);
            break;
        }
        offset += 8 + length;
    }
    if (!v2 || v2->second > SIZE_MAX) return {};
    std::vector<uint8_t> entry(static_cast<size_t>(v2->second));
    return Read(file, v2->first, entry) ? SignerCertificates(entry) : std::vector<std::vector<uint8_t>>{};
}

} // namespace wa::ui_jni::detail
