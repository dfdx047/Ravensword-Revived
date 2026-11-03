// Low 4 GB address space manager and the guest heap.
#include <sys/mman.h>
#include <cstdio>
#include <map>
#include "common.h"

namespace {
std::mutex g_vm_lock;
u32 g_arena_start = 0, g_arena_end = 0;
std::map<u32, u32> g_free;  // start -> length, page aligned

constexpr u32 kPage = 4096;
inline u32 page_up(u32 v) { return (v + kPage - 1) & ~(kPage - 1); }

// Finds the largest hole below 4 GB, leaving the top megabyte for the kuser page.
void find_arena(u64& start, u64& end) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) bridge_fatal("cannot read maps");
    u64 prev_end = 0x10000000;
    const u64 limit = 0xFFF00000ull;
    u64 best_s = 0, best_e = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e;
        if (sscanf(line, "%llx-%llx", &s, &e) != 2) continue;
        if (s >= limit) break;
        if (s > prev_end && s - prev_end > best_e - best_s) {
            best_s = prev_end;
            best_e = s;
        }
        if (e > prev_end) prev_end = e;
    }
    fclose(f);
    if (prev_end < limit && limit - prev_end > best_e - best_s) {
        best_s = prev_end;
        best_e = limit;
    }
    start = best_s;
    end = best_e;
}

u32 vm_alloc_locked(u32 len, u32 align) {
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        u32 s = it->first, l = it->second;
        u32 a = (s + align - 1) & ~(align - 1);
        if (a - s + len > l) continue;
        g_free.erase(it);
        if (a > s) g_free[s] = a - s;
        u32 tail = l - (a - s) - len;
        if (tail) g_free[a + len] = tail;
        return a;
    }
    return 0;
}

void vm_free_locked(u32 addr, u32 len) {
    auto it = g_free.lower_bound(addr);
    if (it != g_free.end() && addr + len == it->first) {
        len += it->second;
        it = g_free.erase(it);
    }
    if (it != g_free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == addr) {
            prev->second += len;
            return;
        }
    }
    g_free[addr] = len;
}

void vm_carve_locked(u32 addr, u32 len) {
    u32 end = addr + len;
    for (auto it = g_free.begin(); it != g_free.end();) {
        u32 s = it->first, e = s + it->second;
        if (e <= addr || s >= end) {
            ++it;
            continue;
        }
        it = g_free.erase(it);
        if (s < addr) g_free[s] = addr - s;
        if (e > end) g_free[end] = e - end;
    }
}

// ---- heap: segregated free lists with a size header in front of every block.
struct BlockHeader {
    u32 size;    // payload size class, or the mapping length for large blocks
    u32 offset;  // bytes from mapping/slot start to payload; bit 31 set for large blocks
};
constexpr u32 kHdr = sizeof(BlockHeader);
constexpr u32 kLargeFlag = 0x80000000u;
constexpr u32 kLarge = 1u << 20;
// Blocks below kLarge round up to at most 1 << 20, whose class is (20 - 4) * 4 = 64.
constexpr int kClasses = 65;

std::mutex g_heap_lock;
u32 g_free_list[kClasses];
u32 g_chunk_cur = 0, g_chunk_end = 0;

int class_of(u32 size, u32* rounded) {
    u32 s = size < 16 ? 16 : size;
    int shift = 31 - __builtin_clz(s);
    u32 step = (1u << shift) / 4;
    if (step < 8) step = 8;  // malloc must keep every block 8-byte aligned
    u32 r = (s + step - 1) & ~(step - 1);
    shift = 31 - __builtin_clz(r);
    step = (1u << shift) / 4;
    *rounded = r;
    return (shift - 4) * 4 + (r - (1u << shift)) / step;
}

u32 heap_alloc(u32 size) {
    if (size + kHdr + 16 >= kLarge) {
        u32 len = page_up(size + kPage);
        u32 m = low_mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, false);
        if (m == 0xFFFFFFFFu) return 0;
        u32 p = m + 16;
        auto* h = gptr<BlockHeader>(p - kHdr);
        h->size = len;
        h->offset = 16 | kLargeFlag;
        return p;
    }
    u32 rounded;
    int c = class_of(size, &rounded);
    std::lock_guard lk(g_heap_lock);
    u32 p = g_free_list[c];
    if (p) {
        g_free_list[c] = *gptr<u32>(p);
        return p;
    }
    u32 total = rounded + 16;  // keeps payloads 8-byte aligned after the header
    if (g_chunk_cur + total > g_chunk_end) {
        u32 chunk = 8u << 20;
        u32 m = low_mmap(0, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, false);
        if (m == 0xFFFFFFFFu) return 0;
        g_chunk_cur = m;
        g_chunk_end = m + chunk;
    }
    u32 slot = g_chunk_cur;
    g_chunk_cur += total;
    p = slot + 16;
    auto* h = gptr<BlockHeader>(p - kHdr);
    h->size = rounded;
    h->offset = 16;
    return p;
}

u32 block_usable(u32 p) {
    auto* h = gptr<BlockHeader>(p - kHdr);
    if (h->offset & kLargeFlag) return h->size - (h->offset & ~kLargeFlag);
    return h->size - (h->offset - 16);  // aligned blocks start later inside their slot
}
}  // namespace

void mem_init() {
    u64 s, e;
    find_arena(s, e);
    s = (s + 0xFFFFF) & ~0xFFFFFull;
    e &= ~0xFFFFFull;
    if (e - s < (512ull << 20)) bridge_fatal("low arena too small: %llx-%llx", (unsigned long long)s, (unsigned long long)e);
    void* m = mmap(reinterpret_cast<void*>(s), e - s, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != reinterpret_cast<void*>(s)) bridge_fatal("cannot reserve low arena %llx: got %p", (unsigned long long)s, m);
    g_arena_start = static_cast<u32>(s);
    g_arena_end = static_cast<u32>(e);
    g_free[g_arena_start] = g_arena_end - g_arena_start;
    BLOG("guest arena %08x-%08x (%u MB)", g_arena_start, g_arena_end, (g_arena_end - g_arena_start) >> 20);
}

u32 low_mmap(u32 hint, u32 len, int prot, int flags, int fd, s64 off, bool fixed) {
    len = page_up(len);
    u32 addr;
    {
        std::lock_guard lk(g_vm_lock);
        if (fixed) {
            vm_carve_locked(hint, len);
            addr = hint;
        } else {
            addr = vm_alloc_locked(len, kPage);
            if (!addr) return 0xFFFFFFFFu;
        }
    }
    void* r = mmap(gptr(addr), len, prot, (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED, fd, off);
    if (r == MAP_FAILED) {
        if (!fixed) {
            std::lock_guard lk(g_vm_lock);
            vm_free_locked(addr, len);
        }
        return 0xFFFFFFFFu;
    }
    return addr;
}

int low_munmap(u32 addr, u32 len) {
    len = page_up(len);
    if (addr < g_arena_start || addr + len > g_arena_end) return munmap(gptr(addr), len);
    void* r = mmap(gptr(addr), len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (r == MAP_FAILED) return -1;
    std::lock_guard lk(g_vm_lock);
    vm_free_locked(addr, len);
    return 0;
}

u32 gmalloc(u32 size) { return heap_alloc(size); }

void gfree(u32 p) {
    if (!p) return;
    auto* h = gptr<BlockHeader>(p - kHdr);
    if (h->offset & kLargeFlag) {
        u32 off = h->offset & ~kLargeFlag;
        // An aligned allocation of this block keeps the mapping start in the header's distance.
        u32 start = p - off;
        if (start < g_arena_start || start + h->size > g_arena_end || (start & (kPage - 1)) || (h->size & (kPage - 1))) {
            BERR("free of a bad large block %08x (size %x, offset %x), leaked", p, h->size, off);
            return;
        }
        low_munmap(start, h->size);
        return;
    }
    u32 rounded;
    if (h->size >= kLarge + 1 || h->size < 16) {
        char from[96] = "host";
        if (u32 lr = guest_lr()) elf_describe(lr, from, sizeof from);
        BERR("free of a bad block %08x (size %x, offset %x) from %s, leaked", p, h->size, h->offset, from);
        return;
    }
    int c = class_of(h->size, &rounded);
    if (h->offset != 16) p -= h->offset - 16;  // aligned allocation inside a small slot
    std::lock_guard lk(g_heap_lock);
    *gptr<u32>(p) = g_free_list[c];
    g_free_list[c] = p;
}

u32 grealloc(u32 p, u32 size) {
    if (!p) return gmalloc(size);
    if (!size) {
        gfree(p);
        return 0;
    }
    u32 usable = block_usable(p);
    if (size <= usable) return p;
    u32 n = gmalloc(size + size / 2);
    if (!n) return 0;
    memcpy(gptr(n), gptr(p), usable);
    gfree(p);
    return n;
}

u32 gcalloc(u32 n, u32 size) {
    u64 total = u64(n) * size;
    if (total >> 31) return 0;
    u32 p = gmalloc(static_cast<u32>(total));
    if (p) memset(gptr(p), 0, static_cast<size_t>(total));
    return p;
}

u32 gmemalign(u32 align, u32 size) {
    if (align <= 8) return gmalloc(size);
    u32 raw = gmalloc(size + align + kHdr + 16);
    if (!raw) return 0;
    u32 aligned = (raw + kHdr + align - 1) & ~(align - 1);
    if (aligned == raw) return raw;
    auto* rh = gptr<BlockHeader>(raw - kHdr);
    auto* ah = gptr<BlockHeader>(aligned - kHdr);
    u32 delta = aligned - raw;
    if (rh->offset & kLargeFlag) {
        ah->size = rh->size;
        ah->offset = ((rh->offset & ~kLargeFlag) + delta) | kLargeFlag;
    } else {
        ah->size = rh->size;
        ah->offset = rh->offset + delta;
    }
    return aligned;
}

u32 gstrdup(const char* s) {
    size_t n = strlen(s) + 1;
    u32 p = gmalloc(static_cast<u32>(n));
    memcpy(gptr(p), s, n);
    return p;
}

u32 alloc_stub_page_slot(u32 bytes) {
    static std::mutex lock;
    static u32 cur = 0, end = 0;
    std::lock_guard lk(lock);
    bytes = (bytes + 15) & ~15u;
    if (cur + bytes > end) {
        u32 len = page_up(bytes > (1u << 20) ? bytes : (1u << 20));
        cur = low_mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, false);
        if (cur == 0xFFFFFFFFu) bridge_fatal("stub allocation failed");
        end = cur + len;
    }
    u32 r = cur;
    cur += bytes;
    return r;
}
