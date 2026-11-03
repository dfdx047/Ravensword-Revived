// Shared declarations for the ARM32-on-ARM64 bridge.
// Guest memory is identity mapped below 4 GB, so a guest pointer is a valid host pointer.
#pragma once
#include <android/log.h>
#include <jni.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <array>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

#define BLOG(...) __android_log_print(ANDROID_LOG_INFO, "RavenBridge", __VA_ARGS__)
#define BERR(...) __android_log_print(ANDROID_LOG_ERROR, "RavenBridge", __VA_ARGS__)
[[noreturn]] void bridge_fatal(const char* fmt, ...);

template <class T = void>
inline T* gptr(u32 a) { return reinterpret_cast<T*>(static_cast<uintptr_t>(a)); }
inline u32 g32(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    if (v >> 32) bridge_fatal("host pointer %p escaped to guest", p);
    return static_cast<u32>(v);
}

// ---- mem.cpp: low address space and guest heap
void mem_init();
u32 low_mmap(u32 hint, u32 len, int prot, int flags, int fd, s64 off, bool fixed);
int low_munmap(u32 addr, u32 len);
u32 gmalloc(u32 size);
void gfree(u32 addr);
u32 grealloc(u32 addr, u32 size);
u32 gcalloc(u32 n, u32 size);
u32 gmemalign(u32 align, u32 size);
u32 gstrdup(const char* s);
u32 alloc_stub_page_slot(u32 bytes);  // permanent small allocations for stubs/data

// ---- cpu.cpp: guest threads
struct GuestThread;
struct Cpu;

using ThunkFn = void (*)();

// Register access for the thunk currently running.
u32& R(int i);
u32& guest_sp();
u32 guest_lr();  // 0 outside guest code
u32 guest_tid();
GuestThread* cur_thread();

// Calls a guest function from host code (nested runs are allowed on any thread).
u32 guest_call(u32 fn, std::initializer_list<u32> args, u32* r1_out = nullptr);
u32 guest_call_vec(u32 fn, const std::vector<u32>& args, u32* r1_out = nullptr);

u32 thunk_address(const char* name, ThunkFn fn);  // guest-callable stub for a host function
void cpu_init();
void cpu_start_watchdog();
void cache_invalidate_all(u32 start, u32 len);

// Marks a region where the thread may block, so guest signals can be serviced by a proxy.
struct BlockScope {
    BlockScope();
    ~BlockScope();
};

// ---- signals (cpu.cpp)
int sig_kill_thread(GuestThread* t, int sig);
GuestThread* thread_from_guest(u32 pthread_t_value);
GuestThread* thread_from_tid(int tid);
u32 thread_guest_id(GuestThread* t);
void sig_poll();
void guest_raise_self(int sig);
int sig_action(int sig, u32 new_act, u32 old_act);
int sig_procmask(int how, u32 set, u32 old);
int sig_suspend(u32 mask_ptr);
void sig_return();
void thread_exit_current(u32 retval);

struct GuestThreadInfo {
    u32 pthread_id;
    u32 stack_base, stack_size;
    u32 tls_block;
    u32 errno_slot;
    int last_errno;
    u32 keys[256];
    u32 retval;
};
GuestThreadInfo* thread_info();
int thread_spawn(u32* out, u32 stack_base, u32 stack_size, u32 start, u32 arg, bool detached);
int thread_join(u32 id, u32* ret);
int thread_detach(u32 id);
void thread_stack_of(GuestThread* t, u32* base, u32* size);

// ---- elf.cpp
void fx_register();
void fx_begin_frame();
void fx_end_frame();
struct GuestLib {
    std::string name;
    u32 base = 0, size = 0;
    u32 exidx = 0, exidx_count = 0;
    u32 symtab = 0, strtab = 0, hash = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    u32 init = 0, init_array = 0, init_array_sz = 0;
    u32 lookup(const char* name) const;
};
GuestLib* elf_load(const std::string& path, const std::string& name);
void elf_run_init(GuestLib* lib);
u32 elf_find_any(const char* name);
u32 elf_find_exidx(u32 pc, int* count);
bool elf_describe(u32 addr, char* out, size_t n);

// ---- symbol registry
struct Hle {
    const char* name;
    ThunkFn fn;
};
void hle_register(const Hle* table, size_t count);
void hle_register_data(const char* name, u32 addr);
u32 hle_resolve(const char* name, bool weak);  // 0 only for weak symbols
void libc_init();
void pthread_register();
void gl_register();
void guest_env_init();

// ---- jni.cpp
u32 guest_javavm();
extern JavaVM* g_host_vm;
void jni_bridge_init(JavaVM* vm);
void jni_register_exported(JNIEnv* env, GuestLib* lib);
void jni_end_guest_thread();
extern std::string g_app_lib_dir;

// ---- thunk templates
struct ArgReader {
    int reg = 0;
    u32 sp;
    ArgReader() : sp(guest_sp()) {}
    u32 next32() {
        if (reg < 4) return R(reg++);
        reg = 4;
        u32 v = *gptr<u32>(sp);
        sp += 4;
        return v;
    }
    u64 next64() {
        if (reg < 4) {
            reg = (reg + 1) & ~1;
            if (reg <= 2) {
                u64 v = R(reg) | (u64(R(reg + 1)) << 32);
                reg += 2;
                return v;
            }
            reg = 4;
        }
        sp = (sp + 7) & ~7u;
        u64 v;
        memcpy(&v, gptr(sp), 8);
        sp += 8;
        return v;
    }
};

// Reader for an AAPCS va_list: a pointer into the guest stack.
struct VaReader {
    u32 p;
    explicit VaReader(u32 ptr) : p(ptr) {}
    u32 next32() {
        u32 v = *gptr<u32>(p);
        p += 4;
        return v;
    }
    u64 next64() {
        p = (p + 7) & ~7u;
        u64 v;
        memcpy(&v, gptr(p), 8);
        p += 8;
        return v;
    }
};

template <class T>
inline T read_arg(ArgReader& a) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(a.next32()));
    } else if constexpr (std::is_same_v<T, float>) {
        u32 v = a.next32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    } else if constexpr (std::is_same_v<T, double>) {
        u64 v = a.next64();
        double d;
        memcpy(&d, &v, 8);
        return d;
    } else if constexpr (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>) {
        return static_cast<T>(a.next64());
    } else if constexpr (std::is_signed_v<T>) {
        return static_cast<T>(static_cast<s32>(a.next32()));  // host long is 64 bits, guest long is 32
    } else {
        return static_cast<T>(a.next32());
    }
}

template <class T>
inline void write_ret(T v) {
    if constexpr (std::is_pointer_v<T>) {
        R(0) = g32(reinterpret_cast<const void*>(v));
    } else if constexpr (std::is_same_v<T, float>) {
        u32 b;
        memcpy(&b, &v, 4);
        R(0) = b;
    } else if constexpr (std::is_same_v<T, double>) {
        u64 b;
        memcpy(&b, &v, 8);
        R(0) = static_cast<u32>(b);
        R(1) = static_cast<u32>(b >> 32);
    } else if constexpr (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>) {
        R(0) = static_cast<u32>(v);
        R(1) = static_cast<u32>(static_cast<u64>(v) >> 32);
    } else {
        R(0) = static_cast<u32>(v);
    }
}

template <auto F>
struct ThunkOf;
template <class Ret, class... Args, Ret (*F)(Args...)>
struct ThunkOf<F> {
    static void call() {
        ArgReader a;
        // Braced init guarantees left-to-right argument evaluation.
        struct Pack {
            std::tuple<Args...> t;
        } pack{std::tuple<Args...>{read_arg<Args>(a)...}};
        if constexpr (std::is_void_v<Ret>) {
            std::apply(F, pack.t);
        } else {
            write_ret<Ret>(std::apply(F, pack.t));
        }
    }
};
template <auto F>
inline void thunk() {
    ThunkOf<F>::call();
}
