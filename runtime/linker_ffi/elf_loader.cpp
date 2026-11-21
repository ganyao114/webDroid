#include "dbt_internal.h"

#include "softmmu_internal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <optional>
#include <type_traits>

namespace wa::dbt {
namespace {

static_assert(std::endian::native == std::endian::little);

constexpr uint16_t ET_DYN = 3, EM_ARM = 40, EM_AARCH64 = 183;
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr uint8_t STB_GLOBAL = 1, STB_WEAK = 2;
constexpr uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2;
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
    DT_ANDROID_RELSZ = 0x60000010,
    DT_ANDROID_RELASZ = 0x60000012,
    DT_GNU_HASH = 0x6ffffef5,
    DT_ANDROID_RELRSZ = 0x6fffe001,
};

constexpr uint32_t R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23;
constexpr uint32_t R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026,
                   R_AARCH64_RELATIVE = 1027;

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
static_assert(sizeof(Program32) == 32 && sizeof(Program64) == 56);
static_assert(sizeof(Symbol32) == 16 && sizeof(Symbol64) == 24);
static_assert(sizeof(Rela<uint32_t>) == 12 && sizeof(Rela<uint64_t>) == 24);

using Bytes = std::span<const uint8_t>;

bool Contains(Bytes bytes, uint64_t offset, uint64_t length) {
    return offset <= bytes.size() && length <= bytes.size() - offset;
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

struct Mapping {
    uint64_t address, offset;
    size_t length, valid;
    uint32_t prot;
};

struct LoadMappings {
    wa_softmmu_t* mmu;
    std::vector<Mapping> ranges;
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
    Loader(wa_dbt& dbt, Bytes file, uint64_t bias) : dbt(dbt), file(file), bias(bias) {}

    int32_t Load(uint32_t file_id, uint64_t& base) {
        auto status = ParseProgramHeaders();
        if (status != WA_OK)
            return status;
        status = ParseDynamic();
        if (status != WA_OK)
            return status;
        status = ParseSymbols();
        if (status != WA_OK)
            return status;

        LoadMappings mappings{.mmu = dbt.mmu, .ranges = {}};
        status = PlanMappings(mappings);
        if (status != WA_OK)
            return status;
        dbt.images.reserve(dbt.images.size() + 1);
        status = mappings.Map(file_id);
        if (status == WA_OK)
            status = Relocate();
        if (status == WA_OK)
            status = RunInitializers();
        if (status != WA_OK)
            return status;

        dbt.images.push_back(std::move(image));
        mappings.Publish();
        base = bias + (segments.front().vaddr & ~kPageMask);
        return WA_OK;
    }

  private:
    int32_t ParseProgramHeaders() {
        if (file.size() < sizeof(ElfHeader<Word>))
            return WA_ERR_INVALID;
        const auto header = Read<ElfHeader<Word>>(file, 0);
        if (std::memcmp(header.ident,
                        "\x7f"
                        "ELF",
                        4) ||
            header.ident[4] != (Is32 ? 1 : 2) || header.ident[5] != 1 || header.ident[6] != 1 ||
            header.version != 1 || header.type != ET_DYN ||
            header.machine != (Is32 ? EM_ARM : EM_AARCH64) || header.ehsize != sizeof(header) ||
            header.phentsize != sizeof(Program) ||
            !Contains(file, header.phoff, uint64_t{header.phnum} * sizeof(Program)))
            return WA_ERR_INVALID;

        for (uint64_t i = 0; i < header.phnum; ++i) {
            const auto ph = Read<Program>(file, header.phoff + i * sizeof(Program));
            if (ph.type != PT_LOAD && ph.type != PT_DYNAMIC)
                continue;
            if (ph.filesz > ph.memsz || !Contains(file, ph.offset, ph.filesz) ||
                ph.vaddr >= WA_HOST_STUB_BASE - bias ||
                ph.memsz > WA_HOST_STUB_BASE - bias - ph.vaddr)
                return WA_ERR_INVALID;
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
        const auto bytes = FileAt(dynamic->vaddr);
        return bytes.size() >= dynamic->filesz && bytes.data() == file.data() + dynamic->offset
                   ? WA_OK
                   : WA_ERR_INVALID;
    }

    Bytes FileAt(uint64_t vaddr) const {
        for (const auto& segment : segments) {
            if (vaddr >= segment.vaddr && vaddr - segment.vaddr < segment.filesz) {
                const auto delta = vaddr - segment.vaddr;
                return file.subspan(static_cast<size_t>(segment.offset + delta),
                                    static_cast<size_t>(segment.filesz - delta));
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
        std::vector<uint64_t> needed_offsets;
        bool terminated = false;
        for (uint64_t i = 0; i < dynamic->filesz; i += sizeof(Dynamic<Word>)) {
            const auto entry = Read<Dynamic<Word>>(file, dynamic->offset + i);
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
        if (Tag(DT_ANDROID_RELSZ) || Tag(DT_ANDROID_RELASZ) || Tag(DT_RELRSZ) ||
            Tag(DT_ANDROID_RELRSZ))
            return WA_ERR_NOTSUP;

        const auto max_strings = FileAt(Tag(DT_STRTAB));
        if (Tag(DT_STRSZ) > max_strings.size() || (Tag(DT_STRSZ) && !tags.contains(DT_STRTAB)))
            return WA_ERR_INVALID;
        strings = max_strings.first(static_cast<size_t>(Tag(DT_STRSZ)));
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
            if (!Contains(hash, 0, 8))
                return std::nullopt;
            const auto buckets = Read<uint32_t>(hash, 0);
            const auto chains = Read<uint32_t>(hash, 4);
            if (!Contains(hash, 8, (uint64_t{buckets} + chains) * sizeof(uint32_t)))
                return std::nullopt;
            return chains;
        }
        if (!tags.contains(DT_GNU_HASH))
            return std::nullopt;
        const auto hash = FileAt(Tag(DT_GNU_HASH));
        if (!Contains(hash, 0, 16))
            return std::nullopt;
        const auto count = Read<uint32_t>(hash, 0);
        const auto first = Read<uint32_t>(hash, 4);
        const auto bloom_count = Read<uint32_t>(hash, 8);
        const auto buckets = 16 + uint64_t{bloom_count} * sizeof(Word);
        if (!Contains(hash, buckets, uint64_t{count} * 4))
            return std::nullopt;
        uint32_t last_bucket = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const auto index = Read<uint32_t>(hash, buckets + i * 4);
            if (index && index < first)
                return std::nullopt;
            last_bucket = std::max(last_bucket, index);
        }
        if (!last_bucket)
            return first;
        // GNU hash lays out each bucket's symbols consecutively in dynsym.
        const auto chains = buckets + uint64_t{count} * 4;
        auto cursor = chains + uint64_t{last_bucket - first} * 4;
        for (uint64_t index = last_bucket; Contains(hash, cursor, 4); ++index, cursor += 4) {
            if (Read<uint32_t>(hash, cursor) & 1)
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
            *count > bytes.size() / sizeof(Symbol) || strings.empty())
            return WA_ERR_INVALID;
        symbols = bytes.first(static_cast<size_t>(*count) * sizeof(Symbol));
        for (uint64_t i = 1; i < *count; ++i) {
            const auto symbol = Read<Symbol>(symbols, i * sizeof(Symbol));
            const auto binding = symbol.info >> 4;
            const auto type = symbol.info & 15;
            const auto visibility = symbol.other & 3;
            if (symbol.section == SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK) ||
                (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
                (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE))
                continue;
            const auto name = StringAt(symbol.name);
            if (!name)
                return WA_ERR_INVALID;
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
        if (!index)
            return WA_OK;
        if (index >= symbols.size() / sizeof(Symbol))
            return WA_ERR_INVALID;
        const auto symbol = Read<Symbol>(symbols, index * sizeof(Symbol));
        if (symbol.section != SHN_UNDEF)
            return DefinedSymbol(symbol, address);
        const auto name = StringAt(symbol.name);
        if (!name)
            return WA_ERR_INVALID;
        if (ResolveImport(dbt, needed, *name, address) || (symbol.info >> 4) == STB_WEAK)
            return WA_OK;
        return WA_ERR_NOENT;
    }

    template <typename Relocation> int32_t ApplyTable(uint64_t vaddr, uint64_t length) const {
        if (length == 0)
            return WA_OK;
        const auto table = FileAt(vaddr);
        if (length % sizeof(Relocation) || length > table.size())
            return WA_ERR_INVALID;
        for (uint64_t offset = 0; offset < length; offset += sizeof(Relocation)) {
            const auto reloc = Read<Relocation>(table, offset);
            const auto type = Is32 ? reloc.info & 0xff : reloc.info & UINT32_MAX;
            if (!type)
                continue;
            const bool relative = type == (Is32 ? R_ARM_RELATIVE : R_AARCH64_RELATIVE);
            const bool absolute = type == (Is32 ? R_ARM_ABS32 : R_AARCH64_ABS64);
            if (!relative && !absolute && type != (Is32 ? R_ARM_JUMP_SLOT : R_AARCH64_JUMP_SLOT) &&
                type != (Is32 ? R_ARM_GLOB_DAT : R_AARCH64_GLOB_DAT))
                return WA_ERR_NOTSUP;
            if (!InMemory(reloc.offset, sizeof(Word), PF_W))
                return WA_ERR_FAULT;
            const auto target = bias + reloc.offset;
            const auto symbol = uint64_t{reloc.info} >> (Is32 ? 8 : 32);
            uint64_t address;
            auto status = ResolveSymbol(symbol, address);
            if (status != WA_OK)
                return status;
            if (relative)
                address = bias;

            Word addend = 0;
            if constexpr (std::is_same_v<Relocation, Rela<Word>>) {
                addend = static_cast<Word>(reloc.addend);
            } else if (relative || absolute) {
                // Android's REL GOT and PLT relocations have no implicit addend.
                status = wa_softmmu_read(dbt.mmu, target, &addend, sizeof(addend));
                if (status != WA_OK)
                    return status;
            }
            const auto value = static_cast<Word>(address + addend);
            status = wa_softmmu_write(dbt.mmu, target, &value, sizeof(value));
            if (status != WA_OK)
                return status;
        }
        return WA_OK;
    }

    int32_t Relocate() const {
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

    int32_t RunInitializers() const {
        const auto init = Tag(DT_INIT);
        const auto array = Tag(DT_INIT_ARRAY);
        const auto length = Tag(DT_INIT_ARRAYSZ);
        if ((init && !InMemory(Is32 ? init & ~uint64_t{1} : init, 1, PF_X)) ||
            (length && (!tags.contains(DT_INIT_ARRAY) || length % sizeof(Word) ||
                        !InMemory(array, length, PF_R))))
            return WA_ERR_INVALID;

        uint64_t result;
        if (init) {
            const auto status = wa_dbt_call(&dbt, bias + init, nullptr, 0, &result);
            if (status != WA_OK)
                return status;
        }
        for (uint64_t offset = 0; offset < length; offset += sizeof(Word)) {
            Word address;
            auto status =
                wa_softmmu_read(dbt.mmu, bias + array + offset, &address, sizeof(address));
            if (status != WA_OK)
                return status;
            if (!address || address == static_cast<Word>(-1))
                continue;
            status = wa_dbt_call(&dbt, address, nullptr, 0, &result);
            if (status != WA_OK)
                return status;
        }
        return WA_OK;
    }

    wa_dbt& dbt;
    Bytes file;
    uint64_t bias;
    std::vector<Segment> segments;
    std::optional<Segment> dynamic;
    std::map<uint64_t, uint64_t> tags;
    Bytes strings, symbols;
    std::vector<std::string_view> needed;
    LoadedImage image;
};

} // namespace
} // namespace wa::dbt

int32_t wa_dbt_load_elf(wa_dbt_t* dbt, uint32_t file_id, const uint8_t* elf_bytes, size_t elf_len,
                        uint64_t load_bias, uint64_t* out_base_vaddr) {
    if (!dbt || !elf_bytes || !elf_len || !out_base_vaddr || load_bias >= WA_HOST_STUB_BASE ||
        (load_bias & (WA_PAGE_SIZE - 1)))
        return WA_ERR_INVALID;
    try {
        std::lock_guard lock(dbt->mutex);
        if (dbt->loading || dbt->call_depth)
            return WA_ERR_BUSY;
        struct Loading {
            bool& active;
            ~Loading() { active = false; }
        } loading{dbt->loading};
        dbt->loading = true;
        const wa::dbt::Bytes file{elf_bytes, elf_len};
        if (dbt->arch == WA_ARCH_ARM32)
            return wa::dbt::Loader<uint32_t>{*dbt, file, load_bias}.Load(file_id, *out_base_vaddr);
        return wa::dbt::Loader<uint64_t>{*dbt, file, load_bias}.Load(file_id, *out_base_vaddr);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}
