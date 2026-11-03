// HLE libc/libm/liblog/libz for 2013-era 32-bit bionic guests. Anything whose in-memory layout
// differs between 32 and 64 bits is marshalled by hand; the rest uses the thunk<> template.
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <map>
#include <atomic>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include "common.h"

u32& ExtR(int i);
void sig_unwind_to(u32 sp);
const char* current_thunk_name();
u32 guest_pc();

std::string g_app_lib_dir;

namespace {
// ---- symbol registry
std::unordered_map<std::string, ThunkFn> g_hle;
std::unordered_map<std::string, u32> g_data;
std::unordered_map<std::string, u32> g_stubs;
std::mutex g_reg_lock;

void unimplemented() {
    char where[96];
    elf_describe(R(14), where, sizeof where);
    BERR("UNIMPLEMENTED %s called from %s", current_thunk_name(), where);
    R(0) = 0;
}

// ---- guest FILE objects: 84 bytes each (32-bit bionic), host FILE* stored at offset 0.
u32 g_sF;
constexpr u32 kFileSize = 84;
inline FILE* F(u32 g) {
    FILE* f;
    memcpy(&f, gptr(g), 8);
    return f;
}
u32 wrap_file(FILE* f) {
    if (!f) return 0;
    u32 g = gcalloc(1, kFileSize);
    memcpy(gptr(g), &f, 8);
    return g;
}
bool is_log_stream(u32 g) { return g == g_sF + kFileSize || g == g_sF + 2 * kFileSize; }

void log_stream(u32 g, const char* text, size_t n) {
    __android_log_print(g == g_sF + 2 * kFileSize ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "RavenStdout", "%.*s", static_cast<int>(n), text);
}

// ---- printf
template <class Rd>
std::string gformat(const char* fmt, Rd& rd) {
    std::string out;
    char spec[64], buf[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            const char* q = strchr(p, '%');
            if (!q) q = p + strlen(p);
            out.append(p, q - p);
            p = q;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out.push_back('%');
            p++;
            continue;
        }
        std::string s(1, '%');
        while (*p && strchr("-+ #0", *p)) s.push_back(*p++);
        auto star = [&]() {
            if (*p == '*') {
                s += std::to_string(static_cast<s32>(rd.next32()));
                p++;
            } else {
                while (isdigit(static_cast<u8>(*p))) s.push_back(*p++);
            }
        };
        star();
        if (*p == '.') {
            s.push_back(*p++);
            star();
        }
        int lng = 0;  // 2: 64-bit integer
        bool wide = false;
        for (;;) {
            if (*p == 'l' && p[1] == 'l') { lng = 2; p += 2; }
            else if (*p == 'l') { wide = true; p++; }
            else if (*p == 'j' || *p == 'q') { lng = 2; p++; }
            else if (*p == 'L') { p++; }
            else if (*p == 'h' || *p == 'z' || *p == 't') {
                if (*p == 'h') s.push_back(*p);
                p++;
            }
            else break;
        }
        char conv = *p ? *p++ : 0;
        snprintf(spec, sizeof spec, "%s", s.c_str());
        size_t sl = strlen(spec);
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                if (lng == 2) {
                    spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = conv; spec[sl + 3] = 0;
                    snprintf(buf, sizeof buf, spec, static_cast<long long>(rd.next64()));
                } else {
                    spec[sl] = conv; spec[sl + 1] = 0;
                    snprintf(buf, sizeof buf, spec, static_cast<int>(rd.next32()));
                }
                out += buf;
                break;
            }
            case 'c': {
                spec[sl] = 'c'; spec[sl + 1] = 0;
                snprintf(buf, sizeof buf, spec, static_cast<int>(rd.next32()));
                out += buf;
                break;
            }
            case 'p': {
                snprintf(buf, sizeof buf, "0x%x", rd.next32());
                out += buf;
                break;
            }
            case 's': {
                u32 a = rd.next32();
                const char* str = a ? gptr<const char>(a) : "(null)";
                if (wide) {
                    str = "(wstr)";
                }
                spec[sl] = 's'; spec[sl + 1] = 0;
                int n = snprintf(nullptr, 0, spec, str);
                std::string tmp(static_cast<size_t>(n) + 1, 0);
                snprintf(tmp.data(), tmp.size(), spec, str);
                out.append(tmp.data(), static_cast<size_t>(n));
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                u64 v = rd.next64();
                double d;
                memcpy(&d, &v, 8);
                spec[sl] = conv; spec[sl + 1] = 0;
                snprintf(buf, sizeof buf, spec, d);
                out += buf;
                break;
            }
            case 'n':
                rd.next32();
                break;
            case 0:
                return out;
            default:
                out.append(start, p - start);
        }
    }
    return out;
}

int put_bounded(u32 dst, s64 cap, const std::string& s) {
    if (cap > 0) {
        size_t n = std::min<size_t>(s.size(), static_cast<size_t>(cap - 1));
        memcpy(gptr(dst), s.data(), n);
        gptr<char>(dst)[n] = 0;
    }
    return static_cast<int>(s.size());
}

ArgReader args_from(int reg) {
    ArgReader a;
    a.reg = reg;
    return a;
}

void t_snprintf() { auto a = args_from(3); R(0) = put_bounded(R(0), R(1), gformat(gptr<const char>(R(2)), a)); }
void t_vsnprintf() { VaReader v(R(3)); R(0) = put_bounded(R(0), R(1), gformat(gptr<const char>(R(2)), v)); }
void t_sprintf() { auto a = args_from(2); R(0) = put_bounded(R(0), 1ll << 30, gformat(gptr<const char>(R(1)), a)); }
void t_vsprintf() { VaReader v(R(2)); R(0) = put_bounded(R(0), 1ll << 30, gformat(gptr<const char>(R(1)), v)); }
void t_vasprintf() {
    VaReader v(R(2));
    auto s = gformat(gptr<const char>(R(1)), v);
    u32 p = gmalloc(static_cast<u32>(s.size() + 1));
    memcpy(gptr(p), s.c_str(), s.size() + 1);
    *gptr<u32>(R(0)) = p;
    R(0) = static_cast<u32>(s.size());
}
void file_out(u32 g, const std::string& s) {
    if (is_log_stream(g)) log_stream(g, s.data(), s.size());
    else fwrite(s.data(), 1, s.size(), F(g));
}
void t_fprintf() { auto a = args_from(2); auto s = gformat(gptr<const char>(R(1)), a); file_out(R(0), s); R(0) = static_cast<u32>(s.size()); }
void t_vfprintf() { VaReader v(R(2)); auto s = gformat(gptr<const char>(R(1)), v); file_out(R(0), s); R(0) = static_cast<u32>(s.size()); }
void t_printf() { auto a = args_from(1); auto s = gformat(gptr<const char>(R(0)), a); log_stream(g_sF + kFileSize, s.data(), s.size()); R(0) = static_cast<u32>(s.size()); }
void t_vprintf() { VaReader v(R(1)); auto s = gformat(gptr<const char>(R(0)), v); log_stream(g_sF + kFileSize, s.data(), s.size()); R(0) = static_cast<u32>(s.size()); }
void t_log_print() { auto a = args_from(3); auto s = gformat(gptr<const char>(R(2)), a); R(0) = __android_log_write(R(0), gptr<const char>(R(1)), s.c_str()); }
void t_log_vprint() { VaReader v(R(3)); auto s = gformat(gptr<const char>(R(2)), v); R(0) = __android_log_write(R(0), gptr<const char>(R(1)), s.c_str()); }

// sscanf: host long is 64 bits, so integer "l" and "z" lengths are removed from the format.
std::string scanf_format(const char* f) {
    std::string out;
    for (; *f; f++) {
        out.push_back(*f);
        if (*f != '%') continue;
        f++;
        if (!*f) break;
        if (*f == '%') { out.push_back('%'); continue; }
        while (*f && isdigit(static_cast<u8>(*f))) out.push_back(*f++);
        if (*f == '*') { out.push_back(*f++); while (*f && isdigit(static_cast<u8>(*f))) out.push_back(*f++); }
        if (*f == 'z' || *f == 't') f++;
        else if (*f == 'l' && f[1] != 'l') {
            char c = f[1];
            if (strchr("diuxXo", c)) f++;
        }
        if (!*f) break;
        out.push_back(*f);
    }
    return out;
}
template <class Rd>
void collect_ptrs(Rd& rd, void* (&p)[16]) {
    for (auto& e : p) e = gptr(rd.next32());
}
void t_sscanf() {
    auto a = args_from(2);
    void* p[16];
    // Reads past the real arguments only touch the guest stack.
    collect_ptrs(a, p);
    auto f = scanf_format(gptr<const char>(R(1)));
    R(0) = sscanf(gptr<const char>(R(0)), f.c_str(), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}
void t_fscanf() {
    auto a = args_from(2);
    void* p[16];
    collect_ptrs(a, p);
    auto f = scanf_format(gptr<const char>(R(1)));
    R(0) = fscanf(F(R(0)), f.c_str(), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}

// ---- files
int fake_cpuinfo_fd() {
    static const char text[] =
        "Processor\t: ARMv7 Processor rev 1 (v7l)\n"
        "processor\t: 0\nBogoMIPS\t: 2400.00\n\nprocessor\t: 1\nBogoMIPS\t: 2400.00\n\n"
        "processor\t: 2\nBogoMIPS\t: 2400.00\n\nprocessor\t: 3\nBogoMIPS\t: 2400.00\n\n"
        "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x3\nCPU part\t: 0xc0f\nCPU revision\t: 1\n\n"
        "Hardware\t: Ravensword Bridge\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    int fd = static_cast<int>(syscall(SYS_memfd_create, "cpuinfo", 0u));
    if (fd >= 0) {
        (void)write(fd, text, sizeof text - 1);
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}
bool is_cpuinfo(const char* p) { return p && strcmp(p, "/proc/cpuinfo") == 0; }

void t_open() {
    const char* p = gptr<const char>(R(0));
    if (is_cpuinfo(p)) { R(0) = fake_cpuinfo_fd(); return; }
    R(0) = open(p, static_cast<int>(R(1)), R(2));
}
void t_fopen() {
    const char* p = gptr<const char>(R(0));
    FILE* f = is_cpuinfo(p) ? fdopen(fake_cpuinfo_fd(), "r") : fopen(p, gptr<const char>(R(1)));
    R(0) = wrap_file(f);
}
void t_fdopen() { R(0) = wrap_file(fdopen(static_cast<int>(R(0)), gptr<const char>(R(1)))); }
void t_fclose() {
    u32 g = R(0);
    int rc = fclose(F(g));
    if (g < g_sF || g >= g_sF + 3 * kFileSize) gfree(g);
    R(0) = rc;
}
void t_fread() { BlockScope b; R(0) = static_cast<u32>(fread(gptr(R(0)), R(1), R(2), F(R(3)))); }
void t_fwrite() {
    u32 g = R(3);
    u64 total = u64(R(1)) * R(2);
    if (is_log_stream(g)) { log_stream(g, gptr<const char>(R(0)), total); R(0) = R(2); return; }
    R(0) = static_cast<u32>(fwrite(gptr(R(0)), R(1), R(2), F(g)));
}
void t_fflush() { R(0) = (R(0) == 0 || is_log_stream(R(0))) ? 0 : fflush(F(R(0))); }
void t_fseek() { R(0) = fseek(F(R(0)), static_cast<s32>(R(1)), static_cast<int>(R(2))); }
void t_ftell() { R(0) = static_cast<u32>(ftell(F(R(0)))); }
void t_fgets() { char* r = fgets(gptr<char>(R(0)), static_cast<int>(R(1)), F(R(2))); R(0) = g32(r); }
void t_fputs() {
    u32 g = R(1);
    const char* s = gptr<const char>(R(0));
    if (is_log_stream(g)) { log_stream(g, s, strlen(s)); R(0) = 1; return; }
    R(0) = fputs(s, F(g));
}
void t_fputc() {
    u32 g = R(1);
    char c = static_cast<char>(R(0));
    if (is_log_stream(g)) { log_stream(g, &c, 1); return; }
    R(0) = fputc(static_cast<u8>(c), F(g));
}
void t_getc() { R(0) = getc(F(R(0))); }
void t_ungetc() { R(0) = ungetc(static_cast<int>(R(0)), F(R(1))); }
void t_clearerr() { clearerr(F(R(0))); }
void t_setvbuf() { R(0) = 0; }
void t_puts() { const char* s = gptr<const char>(R(0)); log_stream(g_sF + kFileSize, s, strlen(s)); R(0) = 1; }
void t_putchar() {}
void t_perror() {
    BERR("guest perror: %s: %s", R(0) ? gptr<const char>(R(0)) : "", strerror(errno));
}
void t_getwc() { R(0) = static_cast<u32>(getwc(F(R(0)))); }
void t_putwc() { R(0) = static_cast<u32>(putwc(static_cast<wchar_t>(R(0)), F(R(1)))); }
void t_ungetwc() { R(0) = static_cast<u32>(ungetwc(R(0), F(R(1)))); }

void put_stat(u32 g, const struct stat& s) {
    // 32-bit ARM struct stat / stat64: 104 bytes.
    u8* p = gptr<u8>(g);
    memset(p, 0, 104);
    u64 dev = s.st_dev, rdev = s.st_rdev, ino = s.st_ino;
    s64 size = s.st_size;
    u64 blocks = static_cast<u64>(s.st_blocks);
    u32 w;
    memcpy(p + 0, &dev, 8);
    w = static_cast<u32>(ino); memcpy(p + 12, &w, 4);
    w = s.st_mode; memcpy(p + 16, &w, 4);
    w = static_cast<u32>(s.st_nlink); memcpy(p + 20, &w, 4);
    w = s.st_uid; memcpy(p + 24, &w, 4);
    w = s.st_gid; memcpy(p + 28, &w, 4);
    memcpy(p + 32, &rdev, 8);
    memcpy(p + 48, &size, 8);
    w = static_cast<u32>(s.st_blksize); memcpy(p + 56, &w, 4);
    memcpy(p + 64, &blocks, 8);
    w = static_cast<u32>(s.st_atime); memcpy(p + 72, &w, 4);
    w = static_cast<u32>(s.st_atim.tv_nsec); memcpy(p + 76, &w, 4);
    w = static_cast<u32>(s.st_mtime); memcpy(p + 80, &w, 4);
    w = static_cast<u32>(s.st_mtim.tv_nsec); memcpy(p + 84, &w, 4);
    w = static_cast<u32>(s.st_ctime); memcpy(p + 88, &w, 4);
    w = static_cast<u32>(s.st_ctim.tv_nsec); memcpy(p + 92, &w, 4);
    memcpy(p + 96, &ino, 8);
}
void t_stat() { struct stat s; int r = stat(gptr<const char>(R(0)), &s); if (!r) put_stat(R(1), s); R(0) = r; }
void t_lstat() { struct stat s; int r = lstat(gptr<const char>(R(0)), &s); if (!r) put_stat(R(1), s); R(0) = r; }
void t_fstat() { struct stat s; int r = fstat(static_cast<int>(R(0)), &s); if (!r) put_stat(R(1), s); R(0) = r; }

struct GuestDir { DIR* d; u32 ent; };
std::map<u32, GuestDir> g_dirs;
std::mutex g_dir_lock;
void t_opendir() {
    DIR* d = opendir(gptr<const char>(R(0)));
    if (!d) { R(0) = 0; return; }
    u32 h = gcalloc(1, 16);
    std::lock_guard lk(g_dir_lock);
    g_dirs[h] = {d, gcalloc(1, 300)};
    R(0) = h;
}
void t_readdir() {
    GuestDir gd;
    { std::lock_guard lk(g_dir_lock); gd = g_dirs.at(R(0)); }
    struct dirent* e = readdir(gd.d);
    if (!e) { R(0) = 0; return; }
    // Same layout as 32-bit bionic: u64 ino, s64 off, u16 reclen, u8 type, char name[256].
    memcpy(gptr(gd.ent), e, std::min<size_t>(e->d_reclen, 280));
    R(0) = gd.ent;
}
void t_closedir() {
    GuestDir gd;
    u32 h = R(0);
    { std::lock_guard lk(g_dir_lock); gd = g_dirs.at(h); g_dirs.erase(h); }
    int rc = closedir(gd.d);
    gfree(gd.ent);
    gfree(h);
    R(0) = rc;
}

// ---- memory
void t_mmap() {
    ArgReader a;
    u32 addr = a.next32(), len = a.next32();
    int prot = static_cast<int>(a.next32()), flags = static_cast<int>(a.next32()), fd = static_cast<int>(a.next32());
    s32 off = static_cast<s32>(a.next32());
    prot &= ~PROT_EXEC;  // guest code never executes natively
    if (prot & PROT_WRITE) prot |= PROT_READ;
    bool fixed = flags & MAP_FIXED;
    u32 r = low_mmap(addr, len, prot, flags, fd, off, fixed);
    if (r == 0xFFFFFFFFu) errno = ENOMEM;
    R(0) = r;
}
void t_munmap() { R(0) = low_munmap(R(0), R(1)); }
void t_mprotect() {
    int prot = static_cast<int>(R(2));
    if (prot & PROT_EXEC) prot |= PROT_READ;
    prot &= ~PROT_EXEC;
    R(0) = mprotect(gptr(R(0)), R(1), prot);
}
void t_mremap() { errno = ENOMEM; R(0) = 0xFFFFFFFFu; }
void t_malloc() { R(0) = gmalloc(R(0)); }
void t_free() { gfree(R(0)); }
void t_calloc() { R(0) = gcalloc(R(0), R(1)); }
void t_realloc() { R(0) = grealloc(R(0), R(1)); }
void t_memalign() { R(0) = gmemalign(R(0), R(1)); }
void t_strdup() { R(0) = gstrdup(gptr<const char>(R(0))); }
void t_strndup() {
    const char* s = gptr<const char>(R(0));
    size_t n = strnlen(s, R(1));
    u32 p = gmalloc(static_cast<u32>(n + 1));
    memcpy(gptr(p), s, n);
    gptr<char>(p)[n] = 0;
    R(0) = p;
}

// ---- errors and env
void t_errno() {
    auto* ti = thread_info();
    int* slot = gptr<int>(ti->errno_slot);
    if (*slot != ti->last_errno) errno = *slot;  // the guest wrote it since the last sync
    else *slot = errno;
    ti->last_errno = *slot;
    R(0) = ti->errno_slot;
}
void t_strerror() {
    static thread_local u32 buf = 0;
    if (!buf) buf = gmalloc(128);
    snprintf(gptr<char>(buf), 128, "%s", strerror(static_cast<int>(R(0))));
    R(0) = buf;
}
std::map<std::string, u32> g_env;
std::mutex g_env_lock;
void t_getenv() {
    std::string k = gptr<const char>(R(0));
    std::lock_guard lk(g_env_lock);
    auto it = g_env.find(k);
    R(0) = it == g_env.end() ? 0 : it->second;
}
void t_setenv() {
    std::lock_guard lk(g_env_lock);
    std::string k = gptr<const char>(R(0));
    if (g_env.count(k) && !R(2)) { R(0) = 0; return; }
    g_env[k] = gstrdup(gptr<const char>(R(1)));
    R(0) = 0;
}
void t_unsetenv() {
    std::lock_guard lk(g_env_lock);
    g_env.erase(gptr<const char>(R(0)));
    R(0) = 0;
}
void t_exit() { BLOG("guest exit(%d)", static_cast<int>(R(0))); _exit(static_cast<int>(R(0))); }
void t_abort() {
    char where[96];
    elf_describe(R(14), where, sizeof where);
    bridge_fatal("guest abort called from %s", where);
}
void t_stack_chk_fail() {
    char where[96];
    elf_describe(R(14), where, sizeof where);
    bridge_fatal("guest stack smash detected near %s", where);
}
void t_pure_virtual() { bridge_fatal("guest pure virtual call"); }
void t_return0() { R(0) = 0; }
void t_return_minus1() { errno = ENOSYS; R(0) = 0xFFFFFFFFu; }
void t_noop() {}

// ---- cxa
std::recursive_mutex g_guard_lock;
void t_cxa_guard_acquire() {
    u8* g = gptr<u8>(R(0));
    if (__atomic_load_n(g, __ATOMIC_ACQUIRE)) { R(0) = 0; return; }
    g_guard_lock.lock();
    if (*g) { g_guard_lock.unlock(); R(0) = 0; return; }
    R(0) = 1;
}
void t_cxa_guard_release() { __atomic_store_n(gptr<u8>(R(0)), 1, __ATOMIC_RELEASE); g_guard_lock.unlock(); }
void t_find_exidx() {
    int count = 0;
    u32 r = elf_find_exidx(R(0), &count);
    *gptr<int>(R(1)) = count;
    R(0) = r;
}

// ---- strings with pointer-to-pointer or overloaded prototypes
void t_strtol() {
    char* e = nullptr;
    errno = 0;
    long v = strtol(gptr<const char>(R(0)), R(1) ? &e : nullptr, static_cast<int>(R(2)));
    if (R(1)) *gptr<u32>(R(1)) = g32(e);
    if (v > INT32_MAX) { v = INT32_MAX; errno = ERANGE; }
    if (v < INT32_MIN) { v = INT32_MIN; errno = ERANGE; }
    R(0) = static_cast<u32>(v);
}
void t_strtoul() {
    char* e = nullptr;
    unsigned long v = strtoul(gptr<const char>(R(0)), R(1) ? &e : nullptr, static_cast<int>(R(2)));
    if (R(1)) *gptr<u32>(R(1)) = g32(e);
    R(0) = v > UINT32_MAX ? UINT32_MAX : static_cast<u32>(v);
}
void t_strtoull() {
    char* e = nullptr;
    unsigned long long v = strtoull(gptr<const char>(R(0)), R(1) ? &e : nullptr, static_cast<int>(R(2)));
    if (R(1)) *gptr<u32>(R(1)) = g32(e);
    write_ret<unsigned long long>(v);
}
void t_strtod() {
    char* e = nullptr;
    double v = strtod(gptr<const char>(R(0)), R(1) ? &e : nullptr);
    if (R(1)) *gptr<u32>(R(1)) = g32(e);
    write_ret<double>(v);
}
void t_strtok_r() {
    char* save = gptr<char>(*gptr<u32>(R(2)));
    char* r = strtok_r(gptr<char>(R(0)), gptr<const char>(R(1)), &save);
    *gptr<u32>(R(2)) = g32(save);
    R(0) = g32(r);
}
char* x_strchr(const char* s, int c) { return const_cast<char*>(strchr(s, c)); }
char* x_strrchr(const char* s, int c) { return const_cast<char*>(strrchr(s, c)); }
char* x_strstr(const char* a, const char* b) { return const_cast<char*>(strstr(a, b)); }
char* x_strpbrk(const char* a, const char* b) { return const_cast<char*>(strpbrk(a, b)); }
char* x_strcasestr(const char* a, const char* b) { return const_cast<char*>(strcasestr(a, b)); }
void* x_memchr(const void* a, int c, size_t n) { return const_cast<void*>(memchr(a, c, n)); }
void* x_memmem(const void* a, size_t al, const void* b, size_t bl) { return const_cast<void*>(memmem(a, al, b, bl)); }
wchar_t* x_wmemchr(const wchar_t* a, wchar_t c, size_t n) { return const_cast<wchar_t*>(wmemchr(a, c, n)); }
void t_mbrtowc() { wchar_t wc; size_t r = mbrtowc(R(0) ? &wc : nullptr, gptr<const char>(R(1)), R(2), nullptr); if (R(0) && r < (size_t)-2) *gptr<u32>(R(0)) = static_cast<u32>(wc); R(0) = static_cast<u32>(r); }
void t_wcrtomb() { R(0) = static_cast<u32>(wcrtomb(gptr<char>(R(0)), static_cast<wchar_t>(R(1)), nullptr)); }
void t_setlocale() {
    static u32 c_name = gstrdup("C");
    R(0) = c_name;
}

thread_local u32 tl_compare_fn = 0;
int compare_tramp(const void* a, const void* b) { return static_cast<int>(guest_call(tl_compare_fn, {g32(a), g32(b)})); }
void t_qsort() {
    u32 saved = tl_compare_fn;
    tl_compare_fn = R(3);
    qsort(gptr(R(0)), R(1), R(2), compare_tramp);
    tl_compare_fn = saved;
}
void t_bsearch() {
    ArgReader a;
    u32 key = a.next32(), base = a.next32(), n = a.next32(), sz = a.next32(), fn = a.next32();
    u32 saved = tl_compare_fn;
    tl_compare_fn = fn;
    void* r = bsearch(gptr(key), gptr(base), n, sz, compare_tramp);
    tl_compare_fn = saved;
    R(0) = g32(r);
}
void t_div() {  // the struct result is returned through a hidden pointer
    u32 out = R(0);
    s32 n = static_cast<s32>(R(1)), d = static_cast<s32>(R(2));
    gptr<s32>(out)[0] = d ? n / d : 0;
    gptr<s32>(out)[1] = d ? n % d : 0;
}

// ---- setjmp family: r4-r14 then d8-d15
void jb_save(u32 env) {
    u32* j = gptr<u32>(env);
    for (int i = 0; i < 11; i++) j[i] = R(4 + i);
    for (int i = 0; i < 16; i++) j[11 + i] = ExtR(16 + i);
}
void jb_load(u32 env, u32 val) {
    u32* j = gptr<u32>(env);
    sig_unwind_to(j[9]);
    for (int i = 0; i < 11; i++) R(4 + i) = j[i];
    for (int i = 0; i < 16; i++) ExtR(16 + i) = j[11 + i];
    R(0) = val ? val : 1;
}
void t_setjmp() { jb_save(R(0)); R(0) = 0; }
void t_longjmp() { jb_load(R(0), R(1)); }

// ---- time: time_t and long are 32 bits in the guest
void put_tm(u32 g, const struct tm& t) {
    s32* p = gptr<s32>(g);
    p[0] = t.tm_sec; p[1] = t.tm_min; p[2] = t.tm_hour; p[3] = t.tm_mday; p[4] = t.tm_mon;
    p[5] = t.tm_year; p[6] = t.tm_wday; p[7] = t.tm_yday; p[8] = t.tm_isdst;
    p[9] = static_cast<s32>(t.tm_gmtoff);
    p[10] = 0;
}
struct tm get_tm(u32 g) {
    const s32* p = gptr<s32>(g);
    struct tm t{};
    t.tm_sec = p[0]; t.tm_min = p[1]; t.tm_hour = p[2]; t.tm_mday = p[3]; t.tm_mon = p[4];
    t.tm_year = p[5]; t.tm_wday = p[6]; t.tm_yday = p[7]; t.tm_isdst = p[8];
    t.tm_gmtoff = p[9];
    return t;
}
u32 tm_slot() {
    static thread_local u32 s = 0;
    if (!s) s = gcalloc(1, 64);
    return s;
}
void t_time() {
    time_t v = time(nullptr);
    if (R(0)) *gptr<s32>(R(0)) = static_cast<s32>(v);
    R(0) = static_cast<u32>(v);
}
void t_gettimeofday() {
    struct timeval tv;
    int r = gettimeofday(&tv, nullptr);
    s32* p = gptr<s32>(R(0));
    if (p) { p[0] = static_cast<s32>(tv.tv_sec); p[1] = static_cast<s32>(tv.tv_usec); }
    R(0) = r;
}
void t_clock_gettime() {
    struct timespec ts;
    int r = clock_gettime(static_cast<clockid_t>(R(0)), &ts);
    s32* p = gptr<s32>(R(1));
    p[0] = static_cast<s32>(ts.tv_sec);
    p[1] = static_cast<s32>(ts.tv_nsec);
    R(0) = r;
}
void t_clock_getres() {
    struct timespec ts;
    int r = clock_getres(static_cast<clockid_t>(R(0)), &ts);
    if (R(1)) { s32* p = gptr<s32>(R(1)); p[0] = static_cast<s32>(ts.tv_sec); p[1] = static_cast<s32>(ts.tv_nsec); }
    R(0) = r;
}
void t_nanosleep() {
    const s32* p = gptr<s32>(R(0));
    struct timespec ts = {p[0], p[1]};
    BlockScope b;
    R(0) = nanosleep(&ts, nullptr);
}
void t_usleep() { BlockScope b; R(0) = usleep(R(0)); }
void t_sleep() { BlockScope b; R(0) = sleep(R(0)); }
void t_gmtime() { time_t v = *gptr<s32>(R(0)); struct tm t; gmtime_r(&v, &t); u32 s = tm_slot(); put_tm(s, t); R(0) = s; }
void t_localtime() { time_t v = *gptr<s32>(R(0)); struct tm t; localtime_r(&v, &t); u32 s = tm_slot(); put_tm(s, t); R(0) = s; }
void t_mktime() { struct tm t = get_tm(R(0)); time_t v = mktime(&t); put_tm(R(0), t); R(0) = static_cast<u32>(v); }
void t_strftime() { struct tm t = get_tm(R(3)); R(0) = static_cast<u32>(strftime(gptr<char>(R(0)), R(1), gptr<const char>(R(2)), &t)); }
void t_select() {
    ArgReader a;
    int n = static_cast<int>(a.next32());
    u32 rd = a.next32(), wr = a.next32(), ex = a.next32(), tv = a.next32();
    struct timeval t{};
    if (tv) { t.tv_sec = gptr<s32>(tv)[0]; t.tv_usec = gptr<s32>(tv)[1]; }
    BlockScope b;
    R(0) = select(n, gptr<fd_set>(rd), gptr<fd_set>(wr), gptr<fd_set>(ex), tv ? &t : nullptr);
}
void t_poll() { BlockScope b; R(0) = poll(gptr<struct pollfd>(R(0)), R(1), static_cast<int>(R(2))); }
void t_read() { BlockScope b; R(0) = static_cast<u32>(read(static_cast<int>(R(0)), gptr(R(1)), R(2))); }
void t_fcntl() { R(0) = fcntl(static_cast<int>(R(0)), static_cast<int>(R(1)), R(2)); }
void t_ioctl() { R(0) = ioctl(static_cast<int>(R(0)), static_cast<int>(R(1)), gptr(R(2))); }

// ---- dynamic loading
u32 g_dlerror_str;
void t_dlopen() {
    const char* n = gptr<const char>(R(0));
    static const char* known[] = {"libravenfx", "libGLES", "libEGL", "libc.", "libm.", "libdl", "liblog", "libz", "libmono", "libunity", "libandroid", "libOpenSLES"};
    u32 h = 0;
    if (!n) h = 1;
    else for (const char* k : known) if (strstr(n, k)) h = 1;
    if (!h) BLOG("guest dlopen refused: %s", n);
    R(0) = h;
}
void t_dlsym() {
    const char* n = gptr<const char>(R(1));
    u32 v = elf_find_any(n);
    if (!v) v = hle_resolve(n, true);
    R(0) = v;
}
void t_dlerror() { R(0) = g_dlerror_str; }

// ---- signals
void t_sigaction() { R(0) = sig_action(static_cast<int>(R(0)), R(1), R(2)); }
void t_bsd_signal() {
    u32 tmp = gcalloc(1, 32), old = gcalloc(1, 32);
    gptr<u32>(tmp)[0] = R(1);
    sig_action(static_cast<int>(R(0)), tmp, old);
    R(0) = gptr<u32>(old)[0];
    gfree(tmp);
    gfree(old);
}
void t_sigprocmask() { R(0) = sig_procmask(static_cast<int>(R(0)), R(1), R(2)); }
void t_sigsuspend() { R(0) = sig_suspend(R(0)); errno = EINTR; }
void t_raise() { guest_raise_self(static_cast<int>(R(0))); R(0) = 0; }
void t_kill() {
    if (static_cast<int>(R(0)) == getpid()) { guest_raise_self(static_cast<int>(R(1))); R(0) = 0; return; }
    R(0) = 0xFFFFFFFFu;
    errno = EPERM;
}
void t_tgkill() { R(0) = sig_kill_thread(thread_from_tid(static_cast<int>(R(1))), static_cast<int>(R(2))) ? 0xFFFFFFFFu : 0; }
void t_tkill() { R(0) = sig_kill_thread(thread_from_tid(static_cast<int>(R(0))), static_cast<int>(R(1))) ? 0xFFFFFFFFu : 0; }
void t_syscall() {
    u32 nr = R(0);
    if (nr == 224) { R(0) = guest_tid(); return; }
    if (nr == 158) { sched_yield(); R(0) = 0; return; }
    if (nr == 238) { R(0) = sig_kill_thread(thread_from_tid(static_cast<int>(R(1))), static_cast<int>(R(2))) ? 0xFFFFFFFFu : 0; return; }
    BERR("guest libc syscall(%u) not handled", nr);
    errno = ENOSYS;
    R(0) = 0xFFFFFFFFu;
}
void t_h_errno() { static u32 s = gcalloc(1, 4); R(0) = s; }

// ---- zlib: 32-bit z_stream is 14 words; a host stream is kept per guest stream
struct GZ { u32 next_in, avail_in, total_in, next_out, avail_out, total_out, msg, state, zalloc, zfree, opaque, data_type, adler, reserved; };
std::unordered_map<u32, z_stream*> g_z;
std::mutex g_z_lock;
z_stream* z_get(u32 g, bool create = false) {
    std::lock_guard lk(g_z_lock);
    auto it = g_z.find(g);
    if (it != g_z.end()) return it->second;
    if (!create) return nullptr;
    auto* s = static_cast<z_stream*>(calloc(1, sizeof(z_stream)));
    g_z[g] = s;
    return s;
}
void z_in(z_stream* h, u32 g) {
    auto* s = gptr<GZ>(g);
    h->next_in = gptr<Bytef>(s->next_in);
    h->avail_in = s->avail_in;
    h->next_out = gptr<Bytef>(s->next_out);
    h->avail_out = s->avail_out;
}
void z_out(z_stream* h, u32 g) {
    auto* s = gptr<GZ>(g);
    s->next_in = g32(h->next_in);
    s->avail_in = h->avail_in;
    s->total_in = static_cast<u32>(h->total_in);
    s->next_out = g32(h->next_out);
    s->avail_out = h->avail_out;
    s->total_out = static_cast<u32>(h->total_out);
    s->adler = static_cast<u32>(h->adler);
    s->data_type = static_cast<u32>(h->data_type);
    s->msg = 0;
    s->state = g;  // only checked for non-null by callers
}
void t_inflateInit2() { u32 g = R(0); auto* h = z_get(g, true); int r = inflateInit2_(h, static_cast<int>(R(1)), ZLIB_VERSION, sizeof(z_stream)); z_out(h, g); gptr<GZ>(g)->next_in = 0; R(0) = r; }
void t_inflateInit() { u32 g = R(0); auto* h = z_get(g, true); z_in(h, g); int r = inflateInit_(h, ZLIB_VERSION, sizeof(z_stream)); z_out(h, g); R(0) = r; }
void t_inflate() {
    u32 g = R(0);
    auto* h = z_get(g);
    z_in(h, g);
    int r = inflate(h, static_cast<int>(R(1)));
    if (r < 0 && r != Z_BUF_ERROR) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 8)
            BERR("inflate error %d (%s): avail_in %u avail_out %u total_in %lu total_out %lu", r, h->msg ? h->msg : "no message", h->avail_in, h->avail_out, h->total_in, h->total_out);
    }
    z_out(h, g);
    R(0) = r;
}
void t_inflateEnd() { u32 g = R(0); auto* h = z_get(g); int r = h ? inflateEnd(h) : 0; if (h) { std::lock_guard lk(g_z_lock); g_z.erase(g); free(h); } R(0) = r; }
// A reset leaves the caller's buffers alone, so they have to be read before writing them back.
void t_inflateReset() { u32 g = R(0); auto* h = z_get(g); z_in(h, g); int r = inflateReset(h); z_out(h, g); R(0) = r; }
void t_deflateInit2() { u32 g = R(0); ArgReader a; a.next32(); int level = a.next32(), method = a.next32(), wb = a.next32(), mem = a.next32(), strat = a.next32(); auto* h = z_get(g, true); int r = deflateInit2_(h, level, method, wb, mem, strat, ZLIB_VERSION, sizeof(z_stream)); z_out(h, g); R(0) = r; }
void t_deflate() { u32 g = R(0); auto* h = z_get(g); z_in(h, g); int r = deflate(h, static_cast<int>(R(1))); z_out(h, g); R(0) = r; }
void t_deflateEnd() { u32 g = R(0); auto* h = z_get(g); int r = h ? deflateEnd(h) : 0; if (h) { std::lock_guard lk(g_z_lock); g_z.erase(g); free(h); } R(0) = r; }
void t_deflateReset() { u32 g = R(0); auto* h = z_get(g); z_in(h, g); int r = deflateReset(h); z_out(h, g); R(0) = r; }
unsigned int x_crc32(unsigned int c, const Bytef* p, unsigned int n) { return static_cast<unsigned int>(crc32(c, p, n)); }

// ---- SHA-1 (bionic exported it)
struct Sha1 { u32 h[5]; u32 cnt[2]; u8 buf[64]; };
inline u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
void sha1_block(u32* s, const u8* b) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (b[i * 4] << 24) | (b[i * 4 + 1] << 16) | (b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s[0], bb = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e;
}
void t_sha1init() {
    auto* c = gptr<Sha1>(R(0));
    static const u32 iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->h, iv, sizeof iv);
    c->cnt[0] = c->cnt[1] = 0;
}
void sha1_update(Sha1* c, const u8* d, u32 n) {
    u32 used = (c->cnt[0] >> 3) & 63;
    u64 bits = ((u64(c->cnt[1]) << 32) | c->cnt[0]) + (u64(n) << 3);
    c->cnt[0] = static_cast<u32>(bits);
    c->cnt[1] = static_cast<u32>(bits >> 32);
    while (n) {
        u32 take = std::min(64 - used, n);
        memcpy(c->buf + used, d, take);
        used += take; d += take; n -= take;
        if (used == 64) { sha1_block(c->h, c->buf); used = 0; }
    }
}
void t_sha1update() { sha1_update(gptr<Sha1>(R(0)), gptr<u8>(R(1)), R(2)); }
void t_sha1final() {
    u8* out = gptr<u8>(R(0));
    auto* c = gptr<Sha1>(R(1));
    u64 bits = (u64(c->cnt[1]) << 32) | c->cnt[0];
    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    u8 zero = 0;
    while (((c->cnt[0] >> 3) & 63) != 56) sha1_update(c, &zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = static_cast<u8>(bits >> (56 - 8 * i));
    sha1_update(c, len, 8);
    for (int i = 0; i < 20; i++) out[i] = static_cast<u8>(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void t_fork() { errno = ENOSYS; R(0) = 0xFFFFFFFFu; }
int x_fpclassifyf(float f) {
    // Old bionic: FP_INFINITE 0x01, FP_NAN 0x02, FP_NORMAL 0x04, FP_SUBNORMAL 0x08, FP_ZERO 0x10.
    switch (fpclassify(f)) {
        case FP_INFINITE: return 0x01;
        case FP_NAN: return 0x02;
        case FP_NORMAL: return 0x04;
        case FP_SUBNORMAL: return 0x08;
        default: return 0x10;
    }
}
int x_getdtablesize() { return 1024; }
int x_prctl(int o, unsigned a, unsigned b, unsigned c, unsigned d) { return prctl(o, a, b, c, d); }
long x_sysconf(int n) { return sysconf(n); }
int x_sched_get_priority_max(int p) { return sched_get_priority_max(p); }
int x_sched_get_priority_min(int p) { return sched_get_priority_min(p); }

template <double (*F)(double)> void d1() { thunk<F>(); }

#define T(name) {#name, thunk<static_cast<decltype(&::name)>(&::name)>}
#define C(name, fn) {name, fn}
#define MD1(n) {#n, thunk<static_cast<double (*)(double)>(::n)>}
#define MD2(n) {#n, thunk<static_cast<double (*)(double, double)>(::n)>}
#define MF1(n) {#n, thunk<static_cast<float (*)(float)>(::n)>}
#define MF2(n) {#n, thunk<static_cast<float (*)(float, float)>(::n)>}

const Hle kTable[] = {
    // memory and strings
    C("malloc", t_malloc), C("free", t_free), C("calloc", t_calloc), C("realloc", t_realloc), C("memalign", t_memalign),
    C("strdup", t_strdup), C("strndup", t_strndup),
    T(memcpy), T(memmove), T(memset), T(memcmp), T(strlen), T(strcmp), T(strncmp), T(strcpy), T(strncpy), T(strcat), T(strncat),
    T(strcasecmp), T(strncasecmp), T(strcoll), T(strxfrm), T(strlcpy),
    {"strchr", thunk<x_strchr>}, {"strrchr", thunk<x_strrchr>}, {"strstr", thunk<x_strstr>}, {"strpbrk", thunk<x_strpbrk>},
    {"strcasestr", thunk<x_strcasestr>}, {"memchr", thunk<x_memchr>}, {"memmem", thunk<x_memmem>},
    T(strtok), C("strtok_r", t_strtok_r), C("strtol", t_strtol), C("strtoul", t_strtoul), C("strtoull", t_strtoull), C("strtod", t_strtod),
    T(atoi), T(atol),
    C("strerror", t_strerror), C("__errno", t_errno),
    T(tolower), T(isalnum), T(isalpha), T(isspace), T(isxdigit),
    // wide characters
    T(btowc), T(wctob), T(towlower), T(towupper), T(wcslen), T(wcscoll), T(wcsxfrm), T(wmemcmp), T(wmemcpy), T(wmemmove), T(wmemset), T(iswctype), T(wctype),
    {"wmemchr", thunk<x_wmemchr>}, C("mbrtowc", t_mbrtowc), C("wcrtomb", t_wcrtomb), C("getwc", t_getwc), C("putwc", t_putwc), C("ungetwc", t_ungetwc),
    C("setlocale", t_setlocale),
    // math
    MD1(sin), MD1(cos), MD1(tan), MD1(asin), MD1(acos), MD1(atan), MD1(sinh), MD1(cosh), MD1(tanh), MD1(exp), MD1(log), MD1(log10),
    MD1(sqrt), MD1(floor), MD1(ceil), MD1(trunc), MD1(round), MD1(rint), MD2(pow), MD2(fmod), MD2(atan2),
    MF1(sinf), MF1(cosf), MF1(tanf), MF1(asinf), MF1(acosf), MF1(atanf), MF1(expf), MF1(logf), MF1(sqrtf), MF1(floorf), MF1(ceilf),
    MF2(powf), MF2(fmodf), MF2(atan2f),
    {"ldexp", thunk<static_cast<double (*)(double, int)>(::ldexp)>},
    {"modff", thunk<static_cast<float (*)(float, float*)>(::modff)>},
    {"__fpclassifyf", thunk<x_fpclassifyf>},
    C("div", t_div), T(lrand48), T(srand48),
    // stdio
    C("snprintf", t_snprintf), C("vsnprintf", t_vsnprintf), C("sprintf", t_sprintf), C("vsprintf", t_vsprintf), C("vasprintf", t_vasprintf),
    C("fprintf", t_fprintf), C("vfprintf", t_vfprintf), C("printf", t_printf), C("vprintf", t_vprintf), C("sscanf", t_sscanf), C("fscanf", t_fscanf),
    C("fopen", t_fopen), C("fdopen", t_fdopen), C("fclose", t_fclose), C("fread", t_fread), C("fwrite", t_fwrite), C("fflush", t_fflush),
    C("fseek", t_fseek), C("ftell", t_ftell), C("fgets", t_fgets), C("fputs", t_fputs), C("fputc", t_fputc), C("putc", t_fputc),
    C("getc", t_getc), C("ungetc", t_ungetc), C("clearerr", t_clearerr), C("setvbuf", t_setvbuf), C("puts", t_puts), C("putchar", t_putchar), C("perror", t_perror),
    // files
    C("open", t_open), C("read", t_read), T(write), T(close), T(lseek), C("stat", t_stat), C("lstat", t_lstat), C("fstat", t_fstat), C("fcntl", t_fcntl), C("ioctl", t_ioctl),
    T(mkdir), T(rmdir), T(unlink), T(rename), T(access), T(chdir), T(chmod), T(truncate), T(ftruncate), T(fsync), T(dup2), T(pipe), T(isatty), T(flock), T(readlink), T(getcwd),
    T(mkstemp), {"getdtablesize", thunk<x_getdtablesize>}, T(uname), T(gethostname), C("utime", t_return_minus1),
    C("opendir", t_opendir), C("readdir", t_readdir), C("closedir", t_closedir),
    // memory mapping
    C("mmap", t_mmap), C("munmap", t_munmap), C("mprotect", t_mprotect), C("mremap", t_mremap), T(madvise),
    // time
    C("time", t_time), C("gettimeofday", t_gettimeofday), C("clock_gettime", t_clock_gettime), C("clock_getres", t_clock_getres), T(clock),
    C("nanosleep", t_nanosleep), C("usleep", t_usleep), C("sleep", t_sleep), C("gmtime", t_gmtime), C("localtime", t_localtime), C("mktime", t_mktime), C("strftime", t_strftime),
    C("select", t_select), C("poll", t_poll),
    // process
    T(getpid), T(gettid), T(getuid), T(geteuid), T(getgid), T(getegid), T(getpriority), T(setpriority), T(sched_yield),
    {"prctl", thunk<x_prctl>}, {"sysconf", thunk<x_sysconf>},
    {"sched_get_priority_max", thunk<x_sched_get_priority_max>}, {"sched_get_priority_min", thunk<x_sched_get_priority_min>},
    C("exit", t_exit), C("_exit", t_exit), C("abort", t_abort), C("__stack_chk_fail", t_stack_chk_fail), C("__cxa_pure_virtual", t_pure_virtual),
    C("getenv", t_getenv), C("setenv", t_setenv), C("unsetenv", t_unsetenv),
    C("__cxa_atexit", t_return0), C("__aeabi_atexit", t_return0), C("atexit", t_return0), C("__cxa_finalize", t_noop),
    C("__cxa_guard_acquire", t_cxa_guard_acquire), C("__cxa_guard_release", t_cxa_guard_release),
    C("__gnu_Unwind_Find_exidx", t_find_exidx),
    C("qsort", t_qsort), C("bsearch", t_bsearch),
    C("setjmp", t_setjmp), C("longjmp", t_longjmp), C("sigsetjmp", t_setjmp), C("siglongjmp", t_longjmp),
    // dl
    C("dlopen", t_dlopen), C("dlsym", t_dlsym), C("dlclose", t_return0), C("dlerror", t_dlerror),
    // signals
    C("sigaction", t_sigaction), C("bsd_signal", t_bsd_signal), C("sigprocmask", t_sigprocmask), C("pthread_sigmask", t_sigprocmask), C("sigsuspend", t_sigsuspend),
    C("raise", t_raise), C("kill", t_kill), C("tgkill", t_tgkill), C("tkill", t_tkill), C("syscall", t_syscall),
    // log and properties
    C("__android_log_print", t_log_print), C("__android_log_vprint", t_log_vprint), T(__android_log_write), T(__system_property_get),
    // sockets
    T(socket), T(bind), T(listen), T(accept), T(connect), T(send), T(recv), T(sendto), T(recvfrom), T(shutdown), T(setsockopt), T(getsockopt), T(getsockname), T(getpeername),
    T(inet_addr), T(inet_aton), T(inet_pton), T(inet_ntop),
    T(epoll_create), T(epoll_ctl), T(epoll_wait),
    C("gethostbyname", t_return0), C("gethostbyaddr", t_return0), C("getaddrinfo", t_return_minus1), C("freeaddrinfo", t_noop), C("gai_strerror", t_return0), C("__get_h_errno", t_h_errno),
    // never needed by the game: children, users, timers
    C("fork", t_fork), C("execl", t_return_minus1), C("execv", t_return_minus1), C("execve", t_return_minus1), C("system", t_return_minus1), C("popen", t_return0), C("pclose", t_return_minus1), C("waitpid", t_return_minus1), C("ptrace", t_return_minus1),
    C("getpwuid", t_return0), C("getpwnam", t_return0), C("getgrgid", t_return0), C("getgrnam", t_return0), C("getresuid", t_return_minus1), C("setresuid", t_return_minus1),
    C("setitimer", t_return_minus1), C("getrusage", t_return_minus1), C("sendfile", t_return_minus1), C("sendmsg", t_return_minus1), C("recvmsg", t_return_minus1),
    C("__pthread_cleanup_push", t_noop), C("__pthread_cleanup_pop", t_noop),
    // zlib
    C("inflateInit2_", t_inflateInit2), C("inflateInit_", t_inflateInit), C("inflate", t_inflate), C("inflateEnd", t_inflateEnd), C("inflateReset", t_inflateReset),
    C("deflateInit2_", t_deflateInit2), C("deflate", t_deflate), C("deflateEnd", t_deflateEnd), C("deflateReset", t_deflateReset),
    {"crc32", thunk<x_crc32>},
    C("SHA1Init", t_sha1init), C("SHA1Update", t_sha1update), C("SHA1Final", t_sha1final),
};
}  // namespace

void hle_register(const Hle* table, size_t count) {
    std::lock_guard lk(g_reg_lock);
    for (size_t i = 0; i < count; i++) g_hle[table[i].name] = table[i].fn;
}
void hle_register_data(const char* name, u32 addr) {
    std::lock_guard lk(g_reg_lock);
    g_data[name] = addr;
}

u32 hle_resolve(const char* name, bool weak) {
    std::lock_guard lk(g_reg_lock);
    if (auto d = g_data.find(name); d != g_data.end()) return d->second;
    if (auto s = g_stubs.find(name); s != g_stubs.end()) return s->second;
    auto it = g_hle.find(name);
    if (it == g_hle.end()) {
        if (weak) return 0;
        BLOG("no implementation for %s, adding a logging stub", name);
        char* keep = strdup(name);
        u32 a = thunk_address(keep, unimplemented);
        g_stubs[name] = a;
        return a;
    }
    u32 a = thunk_address(it->first.c_str(), it->second);
    g_stubs[name] = a;
    return a;
}

void libc_init() {
    hle_register(kTable, sizeof kTable / sizeof kTable[0]);

    g_sF = gcalloc(3, kFileSize);
    FILE* hs[3] = {stdin, stdout, stderr};
    for (int i = 0; i < 3; i++) memcpy(gptr(g_sF + i * kFileSize), &hs[i], 8);
    hle_register_data("__sF", g_sF);

    // Old bionic exposes the classification tables through pointer variables.
    u32 ctype_tab = gcalloc(1, 257 + 3);
    for (int c = 0; c < 256; c++) {
        u8 f = 0;
        if (c < 128) {
            if (isupper(c)) f |= 0x01;
            if (islower(c)) f |= 0x02;
            if (isdigit(c)) f |= 0x04;
            if (isspace(c)) f |= 0x08;
            if (ispunct(c)) f |= 0x10;
            if (iscntrl(c)) f |= 0x20;
            if (isxdigit(c) && !isdigit(c)) f |= 0x40;
            if (c == ' ') f |= 0x80;
        }
        gptr<u8>(ctype_tab)[1 + c] = f;
    }
    u32 ctype_ptr = gcalloc(1, 4);
    *gptr<u32>(ctype_ptr) = ctype_tab;
    hle_register_data("_ctype_", ctype_ptr);

    u32 upper = gcalloc(257, 2), lower = gcalloc(257, 2);
    gptr<int16_t>(upper)[0] = gptr<int16_t>(lower)[0] = -1;
    for (int c = 0; c < 256; c++) {
        gptr<int16_t>(upper)[1 + c] = static_cast<int16_t>(c < 128 ? toupper(c) : c);
        gptr<int16_t>(lower)[1 + c] = static_cast<int16_t>(c < 128 ? tolower(c) : c);
    }
    u32 up = gcalloc(1, 4), lp = gcalloc(1, 4);
    *gptr<u32>(up) = upper;
    *gptr<u32>(lp) = lower;
    hle_register_data("_toupper_tab_", up);
    hle_register_data("_tolower_tab_", lp);

    u32 guard = gcalloc(1, 4);
    *gptr<u32>(guard) = 0xB7A2D091u;
    hle_register_data("__stack_chk_guard", guard);
    u32 page = gcalloc(1, 4);
    *gptr<u32>(page) = 4096;
    hle_register_data("__page_size", page);
    u32 env_ptr = gcalloc(1, 4), env_arr = gcalloc(4, 4);
    *gptr<u32>(env_ptr) = env_arr;
    hle_register_data("environ", env_ptr);

    g_dlerror_str = gstrdup("dlopen failed");

    std::lock_guard lk(g_env_lock);
    // Null checks must not depend on delivering SIGSEGV through the emulator.
    g_env["MONO_DEBUG"] = gstrdup("explicit-null-checks");
    g_env["HOME"] = gstrdup("/data/local/tmp");
}
