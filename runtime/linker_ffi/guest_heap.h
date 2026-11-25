#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

struct wa_dbt;

namespace wa::dbt {

inline constexpr uint64_t kHeapBase = 0x40000000;
inline constexpr uint64_t kHeapLimit = 0xe0000000;

struct FastHostCall;

class GuestHeap {
  public:
    GuestHeap();
    ~GuestHeap();

    uint64_t Allocate(wa_dbt& dbt, uint64_t size, uint64_t alignment, bool zero = false);
    bool Free(wa_dbt& dbt, uint64_t address, bool heap_only);
    uint64_t UsableSize(const wa_dbt& dbt, uint64_t address);
    int32_t Reallocate(wa_dbt& dbt, uint64_t address, uint64_t size, uint64_t& result);

    uint64_t AllocatePages(wa_dbt& dbt, uint64_t size, uint64_t alignment, uint32_t prot);
    int32_t MapPagesAt(wa_dbt& dbt, uint64_t address, uint64_t size, uint32_t prot);

#ifdef __EMSCRIPTEN__
    bool FastAllocate(FastHostCall& call, uint64_t size, bool zero);
    bool FastFree(FastHostCall& call);
    bool FastUsableSize(FastHostCall& call);
    bool FastReallocate(FastHostCall& call);
#endif

  private:
    struct Arena;
    struct Slot;
    static constexpr std::array<uint16_t, 14> kSizes{
        16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
    static size_t Class(uint64_t size);

    Slot FindSlot(uint64_t address);
    Slot NextSlot(uint64_t size);
    Arena* AddArena(wa_dbt& dbt, size_t index);
    void TakeSlot(Slot slot, uint64_t size);
    void ReturnSlot(Slot slot);

    void AddRange(uint64_t address, uint64_t size);
    void TakeRange(uint64_t address, uint64_t size);
    void ReturnRange(uint64_t address, uint64_t size);
    int32_t MapPages(wa_dbt& dbt, uint64_t address, uint64_t size, uint64_t mapped_size,
                     uint32_t prot, bool heap, uint64_t* conflict = nullptr);

    std::map<uint64_t, uint64_t> ranges_;
    std::set<std::pair<uint64_t, uint64_t>> ranges_by_size_;
    std::array<Arena*, kSizes.size()> available_{};
    std::unordered_map<uint32_t, std::unique_ptr<Arena>> arenas_;
};

} // namespace wa::dbt
