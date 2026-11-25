#include "bionic_internal.h"
#include "boot_profile.h"
#include "elf_image.h"

#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace wa::dbt {
namespace {

static_assert(std::endian::native == std::endian::little);

constexpr uint16_t ET_DYN = 3, EM_ARM = 40, EM_AARCH64 = 183;
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_ARM_EXIDX = 0x70000001;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr uint8_t STB_GLOBAL = 1, STB_WEAK = 2;
constexpr uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_TLS = 6;
constexpr uint8_t STV_DEFAULT = 0, STV_PROTECTED = 3;
constexpr uint16_t SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_LORESERVE = 0xff00;
constexpr uint64_t kPageMask = WA_PAGE_SIZE - 1;

enum DynamicTag : uint64_t {
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_INIT = 12,
    DT_SONAME = 14,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELENT = 19,
    DT_PLTREL = 20,
    DT_JMPREL = 23,
    DT_INIT_ARRAY = 25,
    DT_INIT_ARRAYSZ = 27,
    DT_RELRSZ = 35,
    DT_RELR = 36,
    DT_RELRENT = 37,
    DT_ANDROID_REL = 0x6000000f,
    DT_ANDROID_RELSZ = 0x60000010,
    DT_ANDROID_RELA = 0x60000011,
    DT_ANDROID_RELASZ = 0x60000012,
    DT_GNU_HASH = 0x6ffffef5,
    DT_ANDROID_RELR = 0x6fffe000,
    DT_ANDROID_RELRSZ = 0x6fffe001,
    DT_ANDROID_RELRENT = 0x6fffe003,
};

constexpr uint32_t R_ARM_ABS32 = 2, R_ARM_TLS_DTPMOD32 = 17, R_ARM_TLS_DTPOFF32 = 18,
                   R_ARM_TLS_TPOFF32 = 19, R_ARM_COPY = 20, R_ARM_GLOB_DAT = 21,
                   R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23;
constexpr uint32_t R_AARCH64_ABS64 = 257, R_AARCH64_COPY = 1024, R_AARCH64_GLOB_DAT = 1025,
                   R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027,
                   R_AARCH64_TLS_DTPMOD = 1028, R_AARCH64_TLS_DTPREL = 1029,
                   R_AARCH64_TLS_TPREL = 1030, R_AARCH64_TLSDESC = 1031;

template <typename Word> struct ElfHeader {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    Word entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Program32 {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Program64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Symbol32 {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t section;
};
struct Symbol64 {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};

template <typename Word> struct Dynamic {
    Word tag, value;
};
template <typename Word> struct Rel {
    Word offset, info;
};
template <typename Word> struct Rela {
    Word offset, info;
    std::make_signed_t<Word> addend;
};

static_assert(sizeof(ElfHeader<uint32_t>) == 52 && sizeof(ElfHeader<uint64_t>) == 64);

template<class Word, class Program, class Symbol>
std::vector<uint8_t> MakeVirtualLibraryElf(std::string_view soname) {
    struct Metadata {
        ElfHeader<Word> header;
        Program programs[2];
        Dynamic<Word> dynamic[7];
        Symbol null_symbol;
        uint32_t hash[4];
    } metadata{};
    constexpr bool is32 = sizeof(Word) == 4;
    const uint8_t ident[]{0x7f, 'E', 'L', 'F', uint8_t(is32 ? 1 : 2), 1, 1};
    std::ranges::copy(ident, metadata.header.ident);
    auto& header = metadata.header;
    header.type = 3; // ET_DYN.
    header.machine = is32 ? 40 : 183; // EM_ARM or EM_AARCH64.
    header.version = 1;
    header.phoff = offsetof(Metadata, programs);
    header.flags = is32 ? 0x05000000 : 0; // ARM EABI version 5.
    header.ehsize = sizeof(header);
    header.phentsize = sizeof(Program);
    header.phnum = std::size(metadata.programs);

    const Word string_size = soname.size() + 2;
    const Word size = sizeof(metadata) + string_size;
    auto& load = metadata.programs[0];
    load.type = PT_LOAD;
    load.flags = PF_R;
    load.filesz = load.memsz = size;
    load.align = WA_PAGE_SIZE;
    auto& dynamic = metadata.programs[1];
    dynamic.type = PT_DYNAMIC;
    dynamic.flags = PF_R;
    dynamic.offset = dynamic.vaddr = dynamic.paddr = offsetof(Metadata, dynamic);
    dynamic.filesz = dynamic.memsz = sizeof(metadata.dynamic);
    dynamic.align = alignof(Word);

    metadata.dynamic[0] = {DT_STRTAB, sizeof(metadata)};
    metadata.dynamic[1] = {DT_STRSZ, string_size};
    metadata.dynamic[2] = {DT_SONAME, 1};
    metadata.dynamic[3] = {DT_SYMTAB, offsetof(Metadata, null_symbol)};
    metadata.dynamic[4] = {DT_SYMENT, sizeof(Symbol)};
    metadata.dynamic[5] = {DT_HASH, offsetof(Metadata, hash)};
    metadata.hash[0] = metadata.hash[1] = 1; // One bucket and one null symbol.
    std::vector<uint8_t> file(size);
    std::memcpy(file.data(), &metadata, sizeof(metadata));
    std::memcpy(file.data() + sizeof(metadata) + 1, soname.data(), soname.size());
    return file;
}
static_assert(sizeof(Program32) == 32 && sizeof(Program64) == 56);
static_assert(sizeof(Symbol32) == 16 && sizeof(Symbol64) == 24);
static_assert(sizeof(Rela<uint32_t>) == 12 && sizeof(Rela<uint64_t>) == 24);

using Bytes = std::span<const uint8_t>;

int64_t ReadSleb(Bytes& bytes) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64 && !bytes.empty(); shift += 7) {
        const auto byte = bytes.front();
        bytes = bytes.subspan(1);
        const auto payload = byte & 0x7f;
        if (shift == 63 && payload != 0 && payload != 0x7f) break;
        value |= static_cast<uint64_t>(payload) << shift;
        if (!(byte & 0x80)) {
            if ((byte & 0x40) && shift + 7 < 64) value |= UINT64_MAX << (shift + 7);
            return std::bit_cast<int64_t>(value);
        }
    }
    throw GuestFault{WA_ERR_INVALID};
}

bool Contains(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

template <typename T> T Read(Bytes bytes, uint64_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + static_cast<size_t>(offset), sizeof(value));
    return value;
}

struct Segment {
    uint64_t vaddr, offset, filesz, memsz;
    uint32_t flags;

    uint32_t Protection() const {
        return ((flags & PF_R) ? WA_PROT_READ : 0) | ((flags & PF_W) ? WA_PROT_WRITE : 0) |
               ((flags & PF_X) ? WA_PROT_EXEC : 0);
    }
};

struct PageMapping {
    uint64_t vaddr, offset;
    uint32_t prot;
    uint32_t valid;
    uint32_t first_bss_byte = WA_PAGE_SIZE;
};

struct LoadMappings {
    wa_softmmu_t* mmu;
    std::vector<ElfMapping> ranges;
    size_t mapped = 0;
    bool committed = false;

    ~LoadMappings() {
        if (!committed) {
            for (size_t i = 0; i < mapped; ++i)
                softmmu::Unmap(mmu, ranges[i].address, ranges[i].length);
        }
    }

    void Append(const PageMapping& page, uint64_t bias) {
        const auto address = bias + page.vaddr;
        if (!ranges.empty()) {
            auto& last = ranges.back();
            const bool same_file_run =
                last.valid == last.length && last.offset + last.length == page.offset;
            if (last.address + last.length == address && last.prot == page.prot &&
                (!page.valid || same_file_run)) {
                last.length += WA_PAGE_SIZE;
                last.valid += page.valid;
                return;
            }
        }
        ranges.push_back({address, page.offset, WA_PAGE_SIZE, page.valid, page.prot});
    }

    int32_t Map(uint32_t file_id) {
        for (const auto& range : ranges) {
            const auto status = wa_softmmu_map_file_lazy(
                mmu, range.address, range.length, range.prot, file_id, range.offset, range.valid,
                (range.prot & WA_PROT_WRITE) != 0);
            if (status != WA_OK)
                return status;
            ++mapped;
        }
        return WA_OK;
    }

    void Publish() {
        for (const auto& range : ranges)
            softmmu::PublishFileTemplates(mmu, range.address, range.length);
        committed = true;
    }
};

template <typename Word> class Loader {
    static constexpr bool Is32 = sizeof(Word) == 4;
    using Program = std::conditional_t<Is32, Program32, Program64>;
    using Symbol = std::conditional_t<Is32, Symbol32, Symbol64>;

  public:
    Loader(wa_dbt* dbt, const ElfFile& file, uint64_t bias) : dbt(dbt), file(file), bias(bias) {}

    int32_t Inspect(ElfInfo& info, bool include_symbols) {
        auto status = ParseProgramHeaders();
        if (status == WA_OK) status = ParseDynamic();
        if (status == WA_OK && include_symbols) status = ParseSymbols();
        if (status != WA_OK) return status;
        info = {};
        info.soname = image.soname;
        info.needed.assign(needed.begin(), needed.end());
        info.alignment = alignment;
        info.end = (segments.back().vaddr + segments.back().memsz + kPageMask) & ~kPageMask;
        for (const auto& [name, address] : image.exports) info.exports.push_back(name);
        for (const auto& [name, offset] : image.tls_exports) info.exports.push_back(name);
        std::ranges::sort(info.exports);
        for (size_t i = 1; i < symbols.size() / sizeof(Symbol); ++i) {
            const auto symbol = Read<Symbol>(symbols, i * sizeof(Symbol));
            if (symbol.section != SHN_UNDEF) continue;
            const auto name = StringAt(symbol.name);
            if (!name) return WA_ERR_INVALID;
            if (!name->empty()) info.imports.push_back({std::string(*name), (symbol.info >> 4) == STB_WEAK});
        }
        return WA_OK;
    }

    int32_t Load(uint32_t file_id, std::string_view name, uint64_t& base, std::string* error) {
        this->error = error;
        auto status = ParseProgramHeaders();
        if (status != WA_OK)
            return status;
        status = ParseDynamic();
        if (status != WA_OK)
            return status;
        status = ParseSymbols();
        if (status != WA_OK)
            return status;

        if (bias % alignment) return WA_ERR_INVALID;
        if (image.soname.empty()) image.soname = name;
        image.filename = name;
        image.needed.assign(needed.begin(), needed.end());
        image.base = bias + (segments.front().vaddr & ~kPageMask);
        image.bias = bias;
        for (const auto& segment : segments)
            image.segments.emplace_back(bias + segment.vaddr, bias + segment.vaddr + segment.memsz);
        if (tls && tls->memsz) {
            if (tls_alignment > kTlsCapacity) return WA_ERR_NOTSUP;
            image.tls_offset = AlignUp(dbt->next_tls_offset, tls_alignment);
            if (image.tls_offset > kTlsCapacity || tls->memsz > kTlsCapacity - image.tls_offset)
                return WA_ERR_NOMEM;
            image.tls_size = tls->memsz;
            image.tls_id = dbt->next_tls_id++;
            dbt->next_tls_offset = image.tls_offset + image.tls_size;
            image.tls_initial.resize(tls->filesz);
        }
        LoadMappings mappings{.mmu = dbt->mmu, .ranges = {}};
        status = PlanMappings(mappings);
        if (status != WA_OK)
            return status;
        const auto mapped_base = image.base;
        dbt->images.reserve(dbt->images.size() + 1);
        status = mappings.Map(file_id);
        if (status == WA_OK) status = Relocate();
        if (status == WA_OK && !image.tls_initial.empty())
            status = wa_softmmu_read(dbt->mmu, bias + tls->vaddr, image.tls_initial.data(),
                                    image.tls_initial.size());
        if (status != WA_OK) return status;
        image.mappings = mappings.ranges;
        GuestBuffer name_storage(*dbt, image.soname.size() + 1);
        image.name_address = name_storage.address;
        CheckGuest(wa_softmmu_write(dbt->mmu, image.name_address, image.soname.c_str(), image.soname.size() + 1));
        std::optional<GuestBuffer> phdr_storage;
        if (!image.phdr) {
            phdr_storage.emplace(*dbt, image.phnum * sizeof(Program));
            image.phdr = phdr_storage->address;
            const auto headers = FileBytes(phoff, image.phnum * sizeof(Program), program_headers);
            CheckGuest(wa_softmmu_write(dbt->mmu, image.phdr, headers.data(), headers.size()));
        }
        dbt->images.push_back(std::move(image));
        ++dbt->image_loads;
        struct Rollback {
            wa_dbt& dbt;
            uint64_t base, before, end;
            bool committed = false;
            ~Rollback() {
                if (committed) return;
                HideElf(dbt, base);
                std::erase_if(dbt.exit_handlers, [&](const auto& handler) {
                    return (handler.function >= before && handler.function < end) ||
                        (handler.dso >= before && handler.dso < end);
                });
            }
        } rollback{*dbt, mapped_base, bias, bias + segments.back().vaddr + segments.back().memsz};
        InitializeThreadTls(*dbt, dbt->bionic->main, dbt->images.back());
        for (auto& [id, thread] : dbt->bionic->threads)
            InitializeThreadTls(*dbt, thread, dbt->images.back());
        status = RunInitializers(name);
        if (status != WA_OK) return status;
        rollback.committed = true;
        name_storage.address = 0;
        if (phdr_storage) phdr_storage->address = 0;
        mappings.Publish();
        base = mapped_base;
        return WA_OK;
    }

  private:
    struct FileRange {
        uint64_t offset, size;
    };

    Bytes FileBytes(uint64_t offset, uint64_t size, std::vector<uint8_t>& storage) const {
        if (!size) return {};
        if (!Contains(file.size, offset, size) || !std::in_range<size_t>(size))
            throw GuestFault{WA_ERR_INVALID};
        if (!file.read) return file.bytes.subspan(offset, size);
        storage.resize(size);
        if (file.read(file.user, file.file_id, storage.data(), storage.size(), offset) !=
                static_cast<int64_t>(size))
            throw GuestFault{WA_ERR_FAULT};
        return storage;
    }

    template <typename T> T FileValue(uint64_t offset) const {
        if (!file.read) return Read<T>(file.bytes, offset);
        T value;
        if (file.read(file.user, file.file_id, &value, sizeof(value), offset) != sizeof(value))
            throw GuestFault{WA_ERR_FAULT};
        return value;
    }

    int32_t ParseProgramHeaders() {
        if (file.size < sizeof(ElfHeader<Word>))
            return WA_ERR_INVALID;
        const auto header = FileValue<ElfHeader<Word>>(0);
        if (std::memcmp(header.ident,
                        "\x7f"
                        "ELF",
                        4) ||
            header.ident[4] != (Is32 ? 1 : 2) || header.ident[5] != 1 || header.ident[6] != 1 ||
            header.version != 1 || header.type != ET_DYN ||
            header.machine != (Is32 ? EM_ARM : EM_AARCH64) || header.ehsize != sizeof(header) ||
            header.phentsize != sizeof(Program) ||
            !Contains(file.size, header.phoff, uint64_t{header.phnum} * sizeof(Program)))
            return WA_ERR_INVALID;

        phoff = header.phoff;
        image.phnum = header.phnum;
        const auto headers = FileBytes(phoff, header.phnum * sizeof(Program), program_headers);
        for (uint64_t i = 0; i < header.phnum; ++i) {
            const auto ph = Read<Program>(headers, i * sizeof(Program));
            if (ph.type != PT_LOAD && ph.type != PT_DYNAMIC && ph.type != PT_TLS && ph.type != PT_ARM_EXIDX)
                continue;
            if (ph.filesz > ph.memsz || !Contains(file.size, ph.offset, ph.filesz) ||
                ph.vaddr >= WA_HOST_STUB_BASE - bias ||
                ph.memsz > WA_HOST_STUB_BASE - bias - ph.vaddr)
                return WA_ERR_INVALID;
            if (ph.type == PT_TLS) {
                if (tls || (ph.align && !std::has_single_bit(ph.align))) return WA_ERR_INVALID;
                tls = Segment{ph.vaddr, ph.offset, ph.filesz, ph.memsz, ph.flags};
                tls_alignment = std::max<uint64_t>(1, ph.align);
                continue;
            }
            if (ph.type == PT_ARM_EXIDX) {
                if (ph.filesz % 8) return WA_ERR_INVALID;
                image.exidx = bias + ph.vaddr;
                image.exidx_count = ph.filesz / 8;
                continue;
            }
            if (ph.type == PT_DYNAMIC) {
                if (dynamic || !ph.filesz)
                    return WA_ERR_INVALID;
                dynamic = Segment{ph.vaddr, ph.offset, ph.filesz, ph.memsz, ph.flags};
                continue;
            }
            if (!ph.memsz)
                continue;
            if (((ph.vaddr ^ ph.offset) & kPageMask) ||
                (ph.align > 1 &&
                 (!std::has_single_bit(ph.align) || ((ph.vaddr ^ ph.offset) & (ph.align - 1)))))
                return WA_ERR_INVALID;
            alignment = std::max<uint64_t>(alignment, ph.align);
            segments.push_back({ph.vaddr, ph.offset, ph.filesz, ph.memsz, ph.flags});
        }
        if (segments.empty() || !dynamic)
            return WA_ERR_INVALID;
        std::sort(segments.begin(), segments.end(),
                  [](auto& a, auto& b) { return a.vaddr < b.vaddr; });
        for (size_t i = 1; i < segments.size(); ++i) {
            if (segments[i - 1].vaddr + segments[i - 1].memsz > segments[i].vaddr)
                return WA_ERR_NOTSUP;
        }
        for (const auto& segment : segments) {
            if (phoff >= segment.offset && phoff - segment.offset <= segment.filesz &&
                image.phnum * sizeof(Program) <= segment.filesz - (phoff - segment.offset)) {
                image.phdr = bias + segment.vaddr + (phoff - segment.offset);
                break;
            }
        }
        if (image.exidx_count && !InMemory(image.exidx - bias, uint64_t{image.exidx_count} * 8, PF_R))
            return WA_ERR_INVALID;
        const auto range = FileAt(dynamic->vaddr);
        return range.size >= dynamic->filesz && range.offset == dynamic->offset
                   ? WA_OK
                   : WA_ERR_INVALID;
    }

    FileRange FileAt(uint64_t vaddr) const {
        for (const auto& segment : segments) {
            if (vaddr >= segment.vaddr && vaddr - segment.vaddr < segment.filesz) {
                const auto delta = vaddr - segment.vaddr;
                return {segment.offset + delta, segment.filesz - delta};
            }
        }
        return {};
    }

    bool InMemory(uint64_t vaddr, uint64_t length, uint32_t flags = 0) const {
        for (const auto& segment : segments) {
            if (vaddr >= segment.vaddr && vaddr - segment.vaddr <= segment.memsz &&
                length <= segment.memsz - (vaddr - segment.vaddr) &&
                (segment.flags & flags) == flags)
                return true;
        }
        return false;
    }

    uint64_t Tag(uint64_t tag) const {
        const auto found = tags.find(tag);
        return found == tags.end() ? 0 : found->second;
    }

    std::optional<std::string_view> StringAt(uint64_t index) const {
        if (index >= strings.size())
            return std::nullopt;
        const auto* first = reinterpret_cast<const char*>(strings.data() + index);
        const auto* last = static_cast<const char*>(std::memchr(first, 0, strings.size() - index));
        if (!last)
            return std::nullopt;
        return std::string_view{first, static_cast<size_t>(last - first)};
    }

    int32_t ParseDynamic() {
        if (dynamic->filesz % sizeof(Dynamic<Word>))
            return WA_ERR_INVALID;
        std::vector<uint8_t> storage;
        const auto entries = FileBytes(dynamic->offset, dynamic->filesz, storage);
        std::vector<uint64_t> needed_offsets;
        bool terminated = false;
        for (uint64_t i = 0; i < dynamic->filesz; i += sizeof(Dynamic<Word>)) {
            const auto entry = Read<Dynamic<Word>>(entries, i);
            if (entry.tag == DT_NULL) {
                terminated = true;
                break;
            }
            if (entry.tag == DT_NEEDED)
                needed_offsets.push_back(entry.value);
            else
                tags.try_emplace(entry.tag, entry.value);
        }
        if (!terminated)
            return WA_ERR_INVALID;

        const auto max_strings = FileAt(Tag(DT_STRTAB));
        if (Tag(DT_STRSZ) > max_strings.size || (Tag(DT_STRSZ) && !tags.contains(DT_STRTAB)))
            return WA_ERR_INVALID;
        strings = FileBytes(max_strings.offset, Tag(DT_STRSZ), string_storage);
        for (const auto offset : needed_offsets) {
            const auto name = StringAt(offset);
            if (!name || name->empty())
                return WA_ERR_INVALID;
            needed.push_back(*name);
        }
        if (tags.contains(DT_SONAME)) {
            const auto soname = StringAt(Tag(DT_SONAME));
            if (!soname)
                return WA_ERR_INVALID;
            image.soname = *soname;
        }
        return WA_OK;
    }

    std::optional<uint64_t> SymbolCount() const {
        if (tags.contains(DT_HASH)) {
            const auto hash = FileAt(Tag(DT_HASH));
            if (!Contains(hash.size, 0, 8))
                return std::nullopt;
            const auto [buckets, chains] = FileValue<std::array<uint32_t, 2>>(hash.offset);
            if (!Contains(hash.size, 8, (uint64_t{buckets} + chains) * sizeof(uint32_t)))
                return std::nullopt;
            return chains;
        }
        if (!tags.contains(DT_GNU_HASH))
            return std::nullopt;
        const auto hash = FileAt(Tag(DT_GNU_HASH));
        if (!Contains(hash.size, 0, 16))
            return std::nullopt;
        const auto [count, first, bloom_count, shift] = FileValue<std::array<uint32_t, 4>>(hash.offset);
        const auto buckets = 16 + uint64_t{bloom_count} * sizeof(Word);
        if (!Contains(hash.size, buckets, uint64_t{count} * 4))
            return std::nullopt;
        std::vector<uint8_t> storage;
        const auto bucket_bytes = FileBytes(hash.offset + buckets, uint64_t{count} * 4, storage);
        uint32_t last_bucket = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const auto index = Read<uint32_t>(bucket_bytes, i * 4);
            if (index && index < first)
                return std::nullopt;
            last_bucket = std::max(last_bucket, index);
        }
        if (!last_bucket)
            return first;
        // GNU hash lays out each bucket's symbols consecutively in dynsym.
        const auto chains = buckets + uint64_t{count} * 4;
        auto cursor = chains + uint64_t{last_bucket - first} * 4;
        for (uint64_t index = last_bucket; Contains(hash.size, cursor, 4); ++index, cursor += 4) {
            if (FileValue<uint32_t>(hash.offset + cursor) & 1)
                return index + 1;
        }
        return std::nullopt;
    }

    int32_t DefinedSymbol(const Symbol& symbol, uint64_t& address) const {
        if (symbol.section == SHN_ABS) {
            address = symbol.value;
            return WA_OK;
        }
        if (symbol.section >= SHN_LORESERVE || (symbol.info & 15) > STT_FUNC)
            return WA_ERR_NOTSUP;
        const bool function = (symbol.info & 15) == STT_FUNC;
        const auto vaddr = Is32 && function ? symbol.value & ~uint64_t{1} : symbol.value;
        if (!InMemory(vaddr, function ? std::max<uint64_t>(1, symbol.size) : symbol.size,
                      function ? PF_X : 0))
            return WA_ERR_INVALID;
        address = bias + symbol.value;
        return WA_OK;
    }

    int32_t ParseSymbols() {
        if (!tags.contains(DT_SYMTAB))
            return WA_OK;
        const auto count = SymbolCount();
        const auto bytes = FileAt(Tag(DT_SYMTAB));
        if (Tag(DT_SYMENT) != sizeof(Symbol) || !count || !*count ||
            *count > bytes.size / sizeof(Symbol) || strings.empty())
            return WA_ERR_INVALID;
        symbols = FileBytes(bytes.offset, *count * sizeof(Symbol), symbol_storage);
        for (uint64_t i = 1; i < *count; ++i) {
            const auto symbol = Read<Symbol>(symbols, i * sizeof(Symbol));
            const auto binding = symbol.info >> 4;
            const auto type = symbol.info & 15;
            const auto visibility = symbol.other & 3;
            if (symbol.section == SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK) ||
                (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
                (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE && type != STT_TLS))
                continue;
            const auto name = StringAt(symbol.name);
            if (!name)
                return WA_ERR_INVALID;
            if (type == STT_TLS) {
                if (!tls || symbol.value > tls->memsz || symbol.size > tls->memsz - symbol.value)
                    return WA_ERR_INVALID;
                if (!name->empty()) image.tls_exports.try_emplace(std::string(*name), symbol.value);
                continue;
            }
            uint64_t address = 0;
            const auto status = DefinedSymbol(symbol, address);
            if (status != WA_OK)
                return status;
            if (!name->empty())
                image.exports.try_emplace(std::string(*name), address);
        }
        return WA_OK;
    }

    int32_t PlanMappings(LoadMappings& mappings) const {
        std::optional<PageMapping> pending;
        for (const auto& segment : segments) {
            const auto start = segment.vaddr & ~kPageMask;
            const auto end = segment.vaddr + segment.memsz;
            const auto file_end = segment.vaddr + segment.filesz;
            for (auto page = start; page < end; page += WA_PAGE_SIZE) {
                const auto valid = segment.filesz && file_end > page
                                       ? std::min<uint64_t>(WA_PAGE_SIZE, file_end - page)
                                       : 0;
                const auto offset =
                    valid ? segment.offset - (segment.vaddr - start) + (page - start) : 0;
                const auto bss = segment.memsz > segment.filesz && file_end < page + WA_PAGE_SIZE
                                     ? std::max(page, file_end) - page
                                     : WA_PAGE_SIZE;
                const PageMapping next{page, offset, segment.Protection(),
                                       static_cast<uint32_t>(valid), static_cast<uint32_t>(bss)};
                if (pending && pending->vaddr == page) {
                    // Segment boundaries may share a page backed by the same file page.
                    if ((pending->valid && next.valid && pending->offset != next.offset) ||
                        pending->valid > next.first_bss_byte ||
                        next.valid > pending->first_bss_byte)
                        return WA_ERR_NOTSUP;
                    if (next.valid > pending->valid) {
                        pending->offset = next.offset;
                        pending->valid = next.valid;
                    }
                    pending->first_bss_byte =
                        std::min(pending->first_bss_byte, next.first_bss_byte);
                    pending->prot |= next.prot;
                } else {
                    if (pending)
                        mappings.Append(*pending, bias);
                    pending = next;
                }
            }
        }
        mappings.Append(*pending, bias);
        return WA_OK;
    }

    int32_t ResolveSymbol(uint64_t index, uint64_t& address) const {
        address = 0;
        if (!index) return WA_OK;
        if (index >= resolved_symbols.size()) return WA_ERR_INVALID;
        auto& cached = resolved_symbols[index];
        if (cached != UINT64_MAX) {
            address = cached;
            return WA_OK;
        }
        const auto symbol = Read<Symbol>(symbols, index * sizeof(Symbol));
        if (symbol.section != SHN_UNDEF) {
            const auto status = DefinedSymbol(symbol, address);
            if (status == WA_OK) cached = address;
            return status;
        }
        const auto name = StringAt(symbol.name);
        if (!name) return WA_ERR_INVALID;
        if (ResolveImport(*dbt, imports, *name, address) || (symbol.info >> 4) == STB_WEAK) {
            cached = address;
            return WA_OK;
        }
        if (error) *error = "unresolved import " + std::string(*name);
        return WA_ERR_NOENT;
    }

    int32_t ResolveTls(uint64_t index, const LoadedImage*& module, uint64_t& offset) const {
        module = &image;
        offset = 0;
        if (!index) return image.tls_id ? WA_OK : WA_ERR_INVALID;
        if (index >= symbols.size() / sizeof(Symbol)) return WA_ERR_INVALID;
        const auto symbol = Read<Symbol>(symbols, index * sizeof(Symbol));
        if ((symbol.info & 15) != STT_TLS) return WA_ERR_INVALID;
        if (symbol.section != SHN_UNDEF) {
            offset = symbol.value;
            return offset < image.tls_size ? WA_OK : WA_ERR_INVALID;
        }
        const auto name = StringAt(symbol.name);
        if (!name) return WA_ERR_INVALID;
        for (const auto& library : imports) {
            for (const auto* candidate : library.images) {
                if (const auto it = candidate->tls_exports.find(*name); it != candidate->tls_exports.end()) {
                    module = candidate;
                    offset = it->second;
                    return WA_OK;
                }
            }
        }
        if (error) *error = "unresolved TLS import " + std::string(*name);
        return WA_ERR_NOENT;
    }

    int32_t ApplyCopy(softmmu::CpuAccess& memory, uint64_t offset, uint64_t index) const {
        if (!index || index >= symbols.size() / sizeof(Symbol)) return WA_ERR_INVALID;
        const auto symbol = Read<Symbol>(symbols, index * sizeof(Symbol));
        const auto name = StringAt(symbol.name);
        if (!name || !InMemory(offset, symbol.size, PF_W)) return WA_ERR_INVALID;
        uint64_t source;
        if (!ResolveImport(*dbt, imports, *name, source)) return WA_ERR_NOENT;
        std::array<uint8_t, WA_PAGE_SIZE> buffer;
        if (!source || source >= WA_HOST_STUB_BASE || symbol.size > WA_HOST_STUB_BASE - source)
            return WA_ERR_INVALID;
        for (uint64_t done = 0; done < symbol.size;) {
            const auto chunk = std::min<uint64_t>(buffer.size(), symbol.size - done);
            const auto bytes = std::span(buffer).first(chunk);
            auto status = memory.Read(source + done, bytes, WA_PROT_READ).status;
            if (status == WA_OK) status = memory.Write(bias + offset + done, bytes).status;
            if (status != WA_OK) return status;
            done += chunk;
        }
        return WA_OK;
    }

    int32_t ApplyRelocation(softmmu::CpuAccess& memory, uint64_t offset, uint64_t info,
                            Word addend, bool implicit) const {
        if (info > std::numeric_limits<Word>::max()) return WA_ERR_INVALID;
        const auto type = Is32 ? info & 0xff : info & UINT32_MAX;
        const auto symbol = info >> (Is32 ? 8 : 32);
        if (!type) return WA_OK;
        if (type == (Is32 ? R_ARM_COPY : R_AARCH64_COPY)) return ApplyCopy(memory, offset, symbol);
        const bool relative = type == (Is32 ? R_ARM_RELATIVE : R_AARCH64_RELATIVE);
        const bool absolute = type == (Is32 ? R_ARM_ABS32 : R_AARCH64_ABS64);
        const bool tls_module = type == (Is32 ? R_ARM_TLS_DTPMOD32 : R_AARCH64_TLS_DTPMOD);
        const bool tls_offset = type == (Is32 ? R_ARM_TLS_DTPOFF32 : R_AARCH64_TLS_DTPREL);
        const bool tls_static = type == (Is32 ? R_ARM_TLS_TPOFF32 : R_AARCH64_TLS_TPREL);
        const bool tls_desc = !Is32 && type == R_AARCH64_TLSDESC;
        const bool is_tls = tls_module || tls_offset || tls_static || tls_desc;
        if (!relative && !absolute && !is_tls &&
            type != (Is32 ? R_ARM_JUMP_SLOT : R_AARCH64_JUMP_SLOT) &&
            type != (Is32 ? R_ARM_GLOB_DAT : R_AARCH64_GLOB_DAT)) {
            if (error) *error = "unsupported relocation " + std::to_string(type);
            return WA_ERR_NOTSUP;
        }
        if (!InMemory(offset, sizeof(Word) * (tls_desc ? 2 : 1), PF_W)) return WA_ERR_FAULT;
        const auto target = bias + offset;
        if (implicit && (relative || absolute || tls_offset || tls_static)) {
            const auto status = memory.Read(target, {reinterpret_cast<uint8_t*>(&addend), sizeof(addend)}, WA_PROT_READ).status;
            if (status != WA_OK) return status;
        }
        uint64_t address = bias;
        if (is_tls) {
            const LoadedImage* module = nullptr;
            auto status = ResolveTls(symbol, module, address);
            if (status != WA_OK) return status;
            if (tls_module) {
                address = module->tls_id;
                addend = 0;
            } else if (tls_static || tls_desc) {
                address += module->tls_offset;
            }
            if (tls_desc) {
                const uint64_t descriptor[] = {FindVirtualSymbol(*dbt, "libc.so", "__wa_tlsdesc"),
                                               address + addend};
                return memory.Write(target, {reinterpret_cast<const uint8_t*>(descriptor), sizeof(descriptor)}).status;
            }
        } else if (!relative) {
            const auto status = ResolveSymbol(symbol, address);
            if (status != WA_OK) return status;
        } else if (symbol) {
            return WA_ERR_INVALID;
        }
        const auto value = static_cast<Word>(address + addend);
        return memory.Write(target, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)}).status;
    }

    template <typename Relocation> int32_t ApplyTable(uint64_t vaddr, uint64_t length) const {
        if (!length) return WA_OK;
        const auto range = FileAt(vaddr);
        if (length % sizeof(Relocation) || length > range.size) return WA_ERR_INVALID;
        std::vector<uint8_t> storage;
        const auto table = FileBytes(range.offset, length, storage);
        softmmu::CpuAccess memory(dbt->mmu);
        for (uint64_t i = 0; i < length; i += sizeof(Relocation)) {
            const auto reloc = Read<Relocation>(table, i);
            int32_t status;
            if constexpr (std::is_same_v<Relocation, Rela<Word>>)
                status = ApplyRelocation(memory, reloc.offset, reloc.info, reloc.addend, false);
            else
                status = ApplyRelocation(memory, reloc.offset, reloc.info, 0, true);
            if (status != WA_OK) return status;
        }
        return WA_OK;
    }

    int32_t ApplyRelr(uint64_t tag, uint64_t size_tag, uint64_t entry_tag) const {
        const auto size = Tag(size_tag);
        if (!size) return WA_OK;
        const auto range = FileAt(Tag(tag));
        if (!tags.contains(tag) || Tag(entry_tag) != sizeof(Word) ||
            size % sizeof(Word) || size > range.size) return WA_ERR_INVALID;
        std::vector<uint8_t> storage;
        const auto bytes = FileBytes(range.offset, size, storage);
        softmmu::CpuAccess memory(dbt->mmu);

        constexpr auto relative = Is32 ? R_ARM_RELATIVE : R_AARCH64_RELATIVE;
        constexpr size_t coverage = sizeof(Word) * 8 - 1;
        uint64_t where = 0;
        bool have_base = false;
        for (uint64_t i = 0; i < size; i += sizeof(Word)) {
            auto entry = Read<Word>(bytes, i);
            if (!(entry & 1)) {
                if (entry % sizeof(Word)) return WA_ERR_INVALID;
                const auto status = ApplyRelocation(memory, entry, relative, 0, true);
                if (status != WA_OK) return status;
                have_base = true;
                where = uint64_t{entry} + sizeof(Word);
                continue;
            }
            if (!have_base || where > WA_HOST_STUB_BASE) return WA_ERR_INVALID;
            for (unsigned bit = 0; (entry >>= 1) != 0; ++bit) {
                if (!(entry & 1)) continue;
                const auto status = ApplyRelocation(memory, where + bit * sizeof(Word), relative, 0, true);
                if (status != WA_OK) return status;
            }
            where += coverage * sizeof(Word);
        }
        return WA_OK;
    }

    int32_t ApplyPacked(uint64_t tag, uint64_t size_tag, bool rela) const {
        enum { GroupInfo = 1, GroupOffset = 2, GroupAddend = 4, HasAddend = 8 };
        const auto size = Tag(size_tag);
        if (!size) return WA_OK;
        const auto range = FileAt(Tag(tag));
        if (!tags.contains(tag) || size < 4 || size > range.size) return WA_ERR_INVALID;
        std::vector<uint8_t> storage;
        auto bytes = FileBytes(range.offset, size, storage);
        if (std::memcmp(bytes.data(), "APS2", 4)) return WA_ERR_INVALID;
        bytes = bytes.subspan(4);
        softmmu::CpuAccess memory(dbt->mmu);
        try {
            auto count = ReadSleb(bytes);
            auto offset = ReadSleb(bytes);
            int64_t addend = 0;
            if (count < 0 || count > 4'000'000) return WA_ERR_INVALID;
            while (count) {
                const auto group = ReadSleb(bytes);
                const auto flags = ReadSleb(bytes);
                if (group <= 0 || group > count || flags < 0 || flags > 15 ||
                    ((flags & GroupAddend) && !(flags & HasAddend)) ||
                    (!rela && (flags & HasAddend)))
                    return WA_ERR_INVALID;
                const auto offset_delta = flags & GroupOffset ? ReadSleb(bytes) : 0;
                const auto info = flags & GroupInfo ? ReadSleb(bytes) : 0;
                const auto addend_delta = flags & GroupAddend ? ReadSleb(bytes) : 0;
                if (!(flags & HasAddend)) addend = 0;
                for (int64_t i = 0; i < group; ++i) {
                    const auto delta = flags & GroupOffset ? offset_delta : ReadSleb(bytes);
                    const auto reloc = flags & GroupInfo ? info : ReadSleb(bytes);
                    if (__builtin_add_overflow(offset, delta, &offset) || offset < 0 || reloc < 0)
                        return WA_ERR_INVALID;
                    if (flags & HasAddend) {
                        const auto delta = flags & GroupAddend ? addend_delta : ReadSleb(bytes);
                        if (__builtin_add_overflow(addend, delta, &addend)) return WA_ERR_INVALID;
                    }
                    const auto status = ApplyRelocation(memory, offset, reloc, static_cast<Word>(addend), !rela);
                    if (status != WA_OK) return status;
                }
                count -= group;
            }
            return bytes.empty() ? WA_OK : WA_ERR_INVALID;
        } catch (const GuestFault& fault) {
            return fault.status;
        }
    }

    int32_t Relocate() {
        const BootProfile::Scope phase(BootProfile::Relocations);
        imports = MakeImportScope(*dbt, needed);
        resolved_symbols.assign(symbols.size() / sizeof(Symbol), UINT64_MAX);
        auto packed = ApplyRelr(DT_RELR, DT_RELRSZ, DT_RELRENT);
        if (packed == WA_OK) packed = ApplyRelr(DT_ANDROID_RELR, DT_ANDROID_RELRSZ, DT_ANDROID_RELRENT);
        if (packed == WA_OK) packed = ApplyPacked(DT_ANDROID_REL, DT_ANDROID_RELSZ, false);
        if (packed == WA_OK) packed = ApplyPacked(DT_ANDROID_RELA, DT_ANDROID_RELASZ, true);
        if (packed != WA_OK) {
            if (error && error->empty()) *error = "invalid packed ELF relocation";
            return packed;
        }
        if ((Tag(DT_RELSZ) && (!tags.contains(DT_REL) || Tag(DT_RELENT) != sizeof(Rel<Word>))) ||
            (Tag(DT_RELASZ) && (!tags.contains(DT_RELA) || Tag(DT_RELAENT) != sizeof(Rela<Word>))))
            return WA_ERR_INVALID;
        auto status = ApplyTable<Rel<Word>>(Tag(DT_REL), Tag(DT_RELSZ));
        if (status == WA_OK)
            status = ApplyTable<Rela<Word>>(Tag(DT_RELA), Tag(DT_RELASZ));
        if (status != WA_OK || !Tag(DT_PLTRELSZ))
            return status;
        if (!tags.contains(DT_JMPREL))
            return WA_ERR_INVALID;
        switch (Tag(DT_PLTREL)) {
        case DT_REL:
            return ApplyTable<Rel<Word>>(Tag(DT_JMPREL), Tag(DT_PLTRELSZ));
        case DT_RELA:
            return ApplyTable<Rela<Word>>(Tag(DT_JMPREL), Tag(DT_PLTRELSZ));
        default:
            return WA_ERR_INVALID;
        }
    }

    int32_t RunInitializers(std::string_view name) const {
        const auto init = Tag(DT_INIT);
        const auto array = Tag(DT_INIT_ARRAY);
        const auto length = Tag(DT_INIT_ARRAYSZ);
        if ((init && !InMemory(Is32 ? init & ~uint64_t{1} : init, 1, PF_X)) ||
            (length && (!tags.contains(DT_INIT_ARRAY) || length % sizeof(Word) ||
                        !InMemory(array, length, PF_R))))
            return WA_ERR_INVALID;
        if (!init && !length) return WA_OK;
        const BootProfile::LibraryCall profile(name, &BootProfile::Library::constructors,
                                               dbt->stats.insns_executed);

        if (init) {
            const auto status = CallInitializer(*dbt, bias + init);
            if (status != WA_OK) {
                if (error) *error = "DT_INIT stopped at guest PC " + std::to_string(dbt->last_pc);
                return status;
            }
        }
        for (uint64_t offset = 0; offset < length; offset += sizeof(Word)) {
            Word address;
            auto status =
                wa_softmmu_read(dbt->mmu, bias + array + offset, &address, sizeof(address));
            if (status != WA_OK)
                return status;
            if (!address || address == static_cast<Word>(-1))
                continue;
            status = CallInitializer(*dbt, address);
            if (status != WA_OK) {
                if (error) *error = ".init_array stopped at guest PC " + std::to_string(dbt->last_pc);
                return status;
            }
        }
        return WA_OK;
    }

    wa_dbt* dbt;
    const ElfFile& file;
    uint64_t bias;
    uint64_t alignment = WA_PAGE_SIZE;
    std::string* error = nullptr;
    std::vector<Segment> segments;
    std::optional<Segment> dynamic, tls;
    uint64_t phoff = 0, tls_alignment = 1;
    std::map<uint64_t, uint64_t> tags;
    std::vector<uint8_t> program_headers, string_storage, symbol_storage;
    Bytes strings, symbols;
    std::vector<std::string_view> needed;
    ImportScope imports;
    mutable std::vector<uint64_t> resolved_symbols;
    LoadedImage image;
};

} // namespace
} // namespace wa::dbt

std::vector<uint8_t> wa::dbt::VirtualLibraryElf(wa_guest_arch_t arch, std::string_view soname) {
    return arch == WA_ARCH_ARM32 ? MakeVirtualLibraryElf<uint32_t, Program32, Symbol32>(soname)
                                 : MakeVirtualLibraryElf<uint64_t, Program64, Symbol64>(soname);
}

int32_t wa::dbt::InspectElf(wa_guest_arch_t arch, std::span<const uint8_t> bytes, ElfInfo& info,
                            bool include_symbols) {
    return InspectElf(arch, ElfFile(bytes), info, include_symbols);
}

int32_t wa::dbt::InspectElf(wa_guest_arch_t arch, const ElfFile& file, ElfInfo& info,
                           bool include_symbols) {
    const BootProfile::Scope phase(BootProfile::Elf);
    try {
        if (arch == WA_ARCH_ARM32) return Loader<uint32_t>{nullptr, file, 0}.Inspect(info, include_symbols);
        if (arch == WA_ARCH_ARM64) return Loader<uint64_t>{nullptr, file, 0}.Inspect(info, include_symbols);
    } catch (const GuestFault& fault) {
        return fault.status;
    }
    return WA_ERR_INVALID;
}

int32_t wa::dbt::LoadElf(wa_dbt& dbt, uint32_t file_id, std::span<const uint8_t> bytes, uint64_t bias,
                         std::string_view name, uint64_t& base, std::string* error) {
    return LoadElf(dbt, file_id, ElfFile(bytes), bias, name, base, error);
}

int32_t wa::dbt::LoadElf(wa_dbt& dbt, uint32_t file_id, const ElfFile& file, uint64_t bias,
                        std::string_view name, uint64_t& base, std::string* error) {
    const BootProfile::Scope phase(BootProfile::Elf);
    if (!file.size || bias >= WA_HOST_STUB_BASE || (bias & kPageMask)) return WA_ERR_INVALID;
    std::lock_guard lock(dbt.mutex);
    try {
        if (dbt.arch == WA_ARCH_ARM32)
            return Loader<uint32_t>{&dbt, file, bias}.Load(file_id, name, base, error);
        return Loader<uint64_t>{&dbt, file, bias}.Load(file_id, name, base, error);
    } catch (const GuestFault& fault) {
        return fault.status;
    }
}

void wa::dbt::HideElf(wa_dbt& dbt, uint64_t base) {
    std::erase_if(dbt.images, [&](const auto& image) {
        if (image.base != base) return false;
        ++dbt.image_unloads;
        FreeGuest(dbt, image.name_address);
        FreeGuest(dbt, image.phdr);
        return true;
    });
}

uint64_t wa::dbt::FindElfSymbol(const wa_dbt& dbt, uint64_t base, std::string_view name) {
    for (const auto& image : dbt.images) {
        if (base && image.base != base) continue;
        if (const auto it = image.exports.find(name); it != image.exports.end()) return it->second;
    }
    return 0;
}

uint64_t wa::dbt::FindElfSymbolInScope(const wa_dbt& dbt, uint64_t base, std::string_view name) {
    for (const auto& image : dbt.images) {
        if (image.base != base) continue;
        if (const auto found = image.exports.find(name); found != image.exports.end()) return found->second;
        uint64_t address = 0;
        // dlsym(handle) uses the image followed by its breadth-first dependency scope.
        const std::vector<std::string_view> needed(image.needed.begin(), image.needed.end());
        return ResolveImport(dbt, MakeImportScope(dbt, needed), name, address) ? address : 0;
    }
    return 0;
}

int32_t wa_dbt_load_elf(wa_dbt_t* dbt, uint32_t file_id, const uint8_t* elf_bytes, size_t elf_len,
                        uint64_t load_bias, uint64_t* out_base_vaddr) {
    if (!dbt || !elf_bytes || !out_base_vaddr) return WA_ERR_INVALID;
    try {
        return wa::dbt::LoadElf(*dbt, file_id, {elf_bytes, elf_len}, load_bias, {}, *out_base_vaddr);
    } catch (const wa::dbt::GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}
