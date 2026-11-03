// Minimal ELF32 ARM loader for the guest libraries. Resolution order: loaded guest
// libraries first (libunity imports mono_* from libmono), then the HLE table.
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common.h"

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif
#ifndef R_ARM_COPY
#define R_ARM_COPY 20
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif

namespace {
std::vector<GuestLib*> g_libs;

u32 elf_hash(const char* s) {
    u32 h = 0, g;
    while (*s) {
        h = (h << 4) + static_cast<u8>(*s++);
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

void relocate(GuestLib* lib, u32 table, u32 bytes) {
    auto* symtab = gptr<Elf32_Sym>(lib->symtab);
    const char* strtab = gptr<const char>(lib->strtab);
    for (u32 off = 0; off < bytes; off += sizeof(Elf32_Rel)) {
        auto* r = gptr<Elf32_Rel>(table + off);
        u32 type = ELF32_R_TYPE(r->r_info);
        u32 symi = ELF32_R_SYM(r->r_info);
        u32* where = gptr<u32>(lib->base + r->r_offset);
        if (type == R_ARM_RELATIVE) {
            *where += lib->base;
            continue;
        }
        u32 value = 0;
        if (symi) {
            const Elf32_Sym& s = symtab[symi];
            const char* name = strtab + s.st_name;
            bool weak = ELF32_ST_BIND(s.st_info) == STB_WEAK;
            if (s.st_shndx != SHN_UNDEF) {
                // Global scope wins so one definition is shared across libraries.
                value = elf_find_any(name);
                if (!value) value = lib->base + s.st_value;
            } else {
                value = elf_find_any(name);
                if (!value) value = hle_resolve(name, weak);
            }
        }
        switch (type) {
            case R_ARM_ABS32:
                *where += value;
                break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT:
                *where = value;
                break;
            case R_ARM_COPY:
                memcpy(where, gptr(value), symtab[symi].st_size);
                break;
            default:
                bridge_fatal("%s: unsupported relocation %u at %x", lib->name.c_str(), type, r->r_offset);
        }
    }
}
}  // namespace

u32 GuestLib::lookup(const char* sym) const {
    if (!hash) return 0;
    auto* h = gptr<u32>(hash);
    u32 nbucket = h[0];
    auto* bucket = h + 2;
    auto* chain = bucket + nbucket;
    auto* st = gptr<Elf32_Sym>(symtab);
    const char* str = gptr<const char>(strtab);
    for (u32 i = bucket[elf_hash(sym) % nbucket]; i; i = chain[i]) {
        const Elf32_Sym& s = st[i];
        if (s.st_shndx != SHN_UNDEF && strcmp(str + s.st_name, sym) == 0) return base + s.st_value;
    }
    return 0;
}

u32 elf_find_any(const char* name) {
    for (auto* l : g_libs) {
        if (u32 v = l->lookup(name)) return v;
    }
    return 0;
}

u32 elf_find_exidx(u32 pc, int* count) {
    for (auto* l : g_libs) {
        if (pc >= l->base && pc < l->base + l->size) {
            *count = static_cast<int>(l->exidx_count);
            return l->exidx;
        }
    }
    *count = 0;
    return 0;
}

bool elf_describe(u32 addr, char* out, size_t n) {
    for (auto* l : g_libs) {
        if (addr >= l->base && addr < l->base + l->size) {
            snprintf(out, n, "%s+0x%x", l->name.c_str(), addr - l->base);
            return true;
        }
    }
    snprintf(out, n, "0x%08x", addr);
    return false;
}

GuestLib* elf_load(const std::string& path, const std::string& name) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) bridge_fatal("cannot open %s", path.c_str());
    struct stat st{};
    fstat(fd, &st);
    std::vector<u8> file(static_cast<size_t>(st.st_size));
    if (pread(fd, file.data(), file.size(), 0) != static_cast<ssize_t>(file.size())) bridge_fatal("short read on %s", path.c_str());
    close(fd);

    auto* eh = reinterpret_cast<Elf32_Ehdr*>(file.data());
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM)
        bridge_fatal("%s is not an ARM32 ELF", path.c_str());
    auto* ph = reinterpret_cast<Elf32_Phdr*>(file.data() + eh->e_phoff);

    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        lo = std::min(lo, ph[i].p_vaddr & ~0xFFFu);
        hi = std::max(hi, (ph[i].p_vaddr + ph[i].p_memsz + 0xFFFu) & ~0xFFFu);
    }
    u32 span = hi - lo;
    u32 base = low_mmap(0, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, false);
    if (base == 0xFFFFFFFFu) bridge_fatal("no low memory for %s", path.c_str());
    base -= lo;

    auto* lib = new GuestLib();
    lib->name = name;
    lib->base = base;
    lib->size = hi;
    u32 dyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            memcpy(gptr(base + p.p_vaddr), file.data() + p.p_offset, p.p_filesz);
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = base + p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            lib->exidx = base + p.p_vaddr;
            lib->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dyn) bridge_fatal("%s has no dynamic section", path.c_str());

    for (auto* d = gptr<Elf32_Dyn>(dyn); d->d_tag != DT_NULL; ++d) {
        u32 v = d->d_un.d_val;
        switch (d->d_tag) {
            case DT_SYMTAB: lib->symtab = base + v; break;
            case DT_STRTAB: lib->strtab = base + v; break;
            case DT_HASH: lib->hash = base + v; break;
            case DT_REL: lib->rel = base + v; break;
            case DT_RELSZ: lib->relsz = v; break;
            case DT_JMPREL: lib->jmprel = base + v; break;
            case DT_PLTRELSZ: lib->pltrelsz = v; break;
            case DT_INIT: lib->init = base + v; break;
            case DT_INIT_ARRAY: lib->init_array = base + v; break;
            case DT_INIT_ARRAYSZ: lib->init_array_sz = v; break;
            default: break;
        }
    }
    g_libs.push_back(lib);
    if (lib->rel) relocate(lib, lib->rel, lib->relsz);
    if (lib->jmprel) relocate(lib, lib->jmprel, lib->pltrelsz);
    BLOG("loaded %s at %08x, size %u KB", name.c_str(), base + lo, span >> 10);
    return lib;
}

void elf_run_init(GuestLib* lib) {
    if (lib->init) guest_call(lib->init, {});
    for (u32 i = 0; i < lib->init_array_sz / 4; i++) {
        u32 fn = *gptr<u32>(lib->init_array + i * 4);
        if (fn && fn != 0xFFFFFFFFu) guest_call(fn, {});
    }
    BLOG("constructors of %s finished", lib->name.c_str());
}
