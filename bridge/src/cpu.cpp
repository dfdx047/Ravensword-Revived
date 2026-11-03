#include <algorithm>
// Guest threads on top of dynarmic: one Jit per host thread and call depth, thunk dispatch,
// kuser helpers and emulated POSIX signals.
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdarg>
#include <map>
#include <memory>
#include <unordered_map>
#include "common.h"

using namespace Dynarmic;

void bridge_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_FATAL, "RavenBridge", fmt, ap);
    va_end(ap);
    abort();
}

namespace {
constexpr u32 kSvcBase = 0x1000;
constexpr int kMaxThunks = 4096;
constexpr int kSvcReturn = 1;      // guest function returned to host
constexpr int kSvcSigReturn = 2;   // guest signal handler returned
constexpr int kSvcCmpxchg = 3;     // kuser helpers
constexpr int kSvcGetTls = 4;
constexpr int kSvcBarrier = 5;
constexpr int kNumSig = 65;

ThunkFn g_thunks[kMaxThunks];
const char* g_thunk_names[kMaxThunks];
std::atomic<int> g_thunk_count{16};
u32 g_return_stub, g_sigreturn_stub;

ExclusiveMonitor* g_monitor;
std::mutex g_cpu_list_lock;
std::atomic<int> g_next_cpu_id{0};

std::vector<Cpu*> g_all_cpus;

struct GuestSigaction {
    u32 handler, mask, flags, restorer;
};
GuestSigaction g_sigactions[kNumSig];
std::mutex g_sig_lock;

class Cp15 final : public A32::Coprocessor {
public:
    explicit Cp15(u32* tls) : tls_(tls) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, A32::CoprocReg, A32::CoprocReg, unsigned) override { return Callback{&Nop, nullptr}; }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 3) return tls_;
        return &zero_;
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<u8>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<u8>) override { return std::nullopt; }

private:
    static std::uint64_t Nop(void*, std::uint32_t, std::uint32_t) { return 0; }
    u32* tls_;
    u32 zero_ = 0;
};
}  // namespace

struct Cpu final : public A32::UserCallbacks {
    std::unique_ptr<A32::Jit> jit;
    GuestThread* thread = nullptr;
    bool finished = false;
    ~Cpu();

    std::uint8_t MemoryRead8(u32 a) override { return *check(a, gptr<u8>(a)); }
    std::uint16_t MemoryRead16(u32 a) override { u16 v; memcpy(&v, check(a, gptr<u8>(a)), 2); return v; }
    std::uint32_t MemoryRead32(u32 a) override { u32 v; memcpy(&v, check(a, gptr<u8>(a)), 4); return v; }
    std::uint64_t MemoryRead64(u32 a) override { u64 v; memcpy(&v, check(a, gptr<u8>(a)), 8); return v; }
    void MemoryWrite8(u32 a, std::uint8_t v) override { *check(a, gptr<u8>(a)) = v; }
    void MemoryWrite16(u32 a, std::uint16_t v) override { memcpy(check(a, gptr<u8>(a)), &v, 2); }
    void MemoryWrite32(u32 a, std::uint32_t v) override { memcpy(check(a, gptr<u8>(a)), &v, 4); }
    void MemoryWrite64(u32 a, std::uint64_t v) override { memcpy(check(a, gptr<u8>(a)), &v, 8); }
    bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t e) override { return __atomic_compare_exchange_n(gptr<u8>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t e) override { return __atomic_compare_exchange_n(gptr<u16>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t e) override { return __atomic_compare_exchange_n(gptr<u32>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t e) override { return __atomic_compare_exchange_n(gptr<u64>(a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    std::optional<std::uint32_t> MemoryReadCode(u32 a) override {
        if (a < 0x1000) return std::nullopt;
        return *gptr<u32>(a);
    }
    void InterpreterFallback(u32 pc, size_t) override { bridge_fatal("interpreter fallback at %08x", pc); }
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(u32 pc, A32::Exception e) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 60; }

    u8* check(u32 a, u8* p);
};

struct SavedSig {
    u32 frame;
    u32 old_mask;
    std::array<u32, 64> ext;
    u32 fpscr;
};

struct GuestThread {
    GuestThreadInfo info{};
    std::vector<std::unique_ptr<Cpu>> cpus;  // one per nesting depth
    int depth = 0;
    u32 tls_value = 0;
    int tid = 0;
    pthread_t host{};
    bool joinable = false;
    bool exiting = false;
    bool owned_by_bridge = false;

    // Signals. When proxying, `identity` is the thread being stood in for.
    GuestThread* identity = nullptr;
    std::mutex state_lock;
    std::atomic<u64> pending{0};
    u32 sig_mask = 0;
    bool in_guest = false;
    int blocked = 0;
    std::atomic<int> proxy_active{0};
    std::atomic<int> self_polling{0};  // in sigsuspend: takes its own signals, no proxy
    std::vector<SavedSig> sig_stack;
    u32 saved_sp = 0;
    u32 saved_regs[16]{};
    std::atomic<u32> finished_flag{0};
    GuestThread* proxy_ctx = nullptr;  // reusable stand-in for signal handling
    const char* volatile last_thunk = nullptr;
    volatile u32 last_lr = 0;
};

namespace {
thread_local GuestThread* tl_thread = nullptr;
thread_local Cpu* tl_cpu = nullptr;
thread_local const char* tl_thunk_name = nullptr;
std::mutex g_threads_lock;
std::unordered_map<u32, GuestThread*> g_by_id;
std::unordered_map<int, GuestThread*> g_by_tid;
constexpr u32 kDefaultStack = 8u << 20;

std::unique_ptr<Cpu> make_cpu(GuestThread* t, int depth) {
    auto c = std::make_unique<Cpu>();
    c->thread = t;
    A32::UserConfig cfg;
    cfg.callbacks = c.get();
    cfg.global_monitor = g_monitor;
    cfg.processor_id = g_next_cpu_id.fetch_add(1) % 512;
    cfg.fastmem_pointer = static_cast<uintptr_t>(0);
    cfg.recompile_on_fastmem_failure = true;
    cfg.arch_version = A32::ArchVersion::v7;
    cfg.enable_cycle_counting = false;
    // Thunks and guest code are the whole budget of a frame: skip the costly corner cases of
    // floating point that a 2013 game never depends on. Exclusive monitor stays exact.
    cfg.unsafe_optimizations = true;
    cfg.optimizations = all_safe_optimizations | OptimizationFlag::Unsafe_ReducedErrorFP | OptimizationFlag::Unsafe_InaccurateNaN |
                        OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    cfg.always_little_endian = true;
    cfg.define_unpredictable_behaviour = true;
    cfg.code_cache_size = (depth == 0 ? 64 : 16) * 1024 * 1024;
    cfg.coprocessors[15] = std::make_shared<Cp15>(&t->tls_value);
    c->jit = std::make_unique<A32::Jit>(cfg);
    std::lock_guard lk(g_cpu_list_lock);
    g_all_cpus.push_back(c.get());
    return c;
}

GuestThread* new_thread_record(u32 stack_base, u32 stack_size, bool register_it) {
    auto* t = new GuestThread();
    t->identity = t;
    if (!stack_base) {
        stack_size = kDefaultStack;
        stack_base = low_mmap(0, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0, false);
        if (stack_base == 0xFFFFFFFFu) bridge_fatal("no memory for guest stack");
        // An overflow must fault here instead of silently overwriting whatever is mapped below.
        mprotect(gptr(stack_base), 64 * 1024, PROT_NONE);
        t->owned_by_bridge = true;
    }
    t->info.stack_base = stack_base;
    t->info.stack_size = stack_size;
    u32 block = gcalloc(1, 1024);
    t->info.pthread_id = block;
    t->info.tls_block = block + 512;
    t->info.errno_slot = block + 256;
    t->tls_value = t->info.tls_block;
    // Old bionic TLS layout: slot 0 points to itself, slot 1 to the thread block.
    gptr<u32>(t->info.tls_block)[0] = t->info.tls_block;
    gptr<u32>(t->info.tls_block)[1] = block;
    if (register_it) {
        std::lock_guard lk(g_threads_lock);
        g_by_id[block] = t;
    }
    return t;
}

GuestThread* ensure_thread() {
    if (!tl_thread) {
        tl_thread = new_thread_record(0, 0, true);
        tl_thread->tid = static_cast<int>(syscall(SYS_gettid));
        std::lock_guard lk(g_threads_lock);
        g_by_tid[tl_thread->tid] = tl_thread;
    }
    return tl_thread;
}

GuestThread* me_identity() { return tl_thread->identity; }

// Waits while a proxy is standing in for this thread.
void gate(GuestThread* t) {
    while (t->proxy_active.load(std::memory_order_acquire)) {
        syscall(SYS_futex, reinterpret_cast<int*>(&t->proxy_active), FUTEX_WAIT_PRIVATE, 1, nullptr, nullptr, 0);
    }
}

void deliver_pending(Cpu* c);

void run_until_return(Cpu* c) {
    c->finished = false;
    auto* id = c->thread->identity;
    while (!c->finished && !c->thread->exiting) {
        deliver_pending(c);
        c->jit->Run();
    }
    (void)id;
}

void push_signal_frame(Cpu* c, int sig) {
    auto* id = c->thread->identity;
    const GuestSigaction act = g_sigactions[sig];
    auto& regs = c->jit->Regs();
    u32 sp = (regs[13] - 1024) & ~7u;
    // siginfo at sp, ucontext at sp+128: flags, link, stack(3 words), sigcontext, sigmask.
    u32 si = sp;
    u32 uc = sp + 128;
    memset(gptr(sp), 0, 128 + 512);
    gptr<int>(si)[0] = sig;
    gptr<int>(si)[2] = -6;  // SI_TKILL
    u32* sc = gptr<u32>(uc + 20);
    sc[0] = 0;             // trap_no
    sc[1] = 0;             // error_code
    sc[2] = id->sig_mask;  // oldmask
    for (int i = 0; i < 13; i++) sc[3 + i] = regs[i];  // r0-r10, fp, ip
    sc[16] = regs[13];
    sc[17] = regs[14];
    sc[18] = regs[15];
    sc[19] = c->jit->Cpsr();
    sc[20] = 0;
    gptr<u32>(uc + 20 + 84)[0] = id->sig_mask;

    SavedSig saved;
    saved.frame = sp;
    saved.old_mask = id->sig_mask;
    saved.ext = c->jit->ExtRegs();
    saved.fpscr = c->jit->Fpscr();
    c->thread->sig_stack.push_back(saved);

    id->sig_mask |= act.mask;
    if (!(act.flags & 0x40000000u /*SA_NODEFER*/)) id->sig_mask |= 1u << (sig - 1);

    regs[0] = static_cast<u32>(sig);
    regs[1] = si;
    regs[2] = uc;
    regs[13] = sp;
    regs[14] = g_sigreturn_stub;
    regs[15] = act.handler & ~1u;
    u32 cpsr = c->jit->Cpsr();
    c->jit->SetCpsr((act.handler & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u));
    c->jit->ClearExclusiveState();
}

void deliver_pending(Cpu* c) {
    auto* id = c->thread->identity;
    u64 p = id->pending.load(std::memory_order_acquire);
    if (!p) return;
    for (int sig = 1; sig < 32; sig++) {
        u64 bit = 1ull << sig;
        if (!(p & bit) || (id->sig_mask & (1u << (sig - 1)))) continue;
        id->pending.fetch_and(~bit);
        u32 handler = g_sigactions[sig].handler;
        if (handler == 0 || handler == 1) continue;  // default or ignore: never fatal in the bridge
        push_signal_frame(c, sig);
        return;  // the next pending one is delivered after this handler's first halt
    }
}

// Runs a signal handler for a thread that is blocked in host code or outside the guest.
void* proxy_main(void* arg) {
    auto* target = static_cast<GuestThread*>(arg);
    GuestThread* p = target->proxy_ctx;
    if (!p) {
        p = new GuestThread();
        p->identity = target;
        p->info = target->info;
        p->tls_value = target->tls_value;
        p->tid = target->tid;
        p->cpus.push_back(make_cpu(p, 1));
        target->proxy_ctx = p;
    }
    tl_thread = p;
    p->depth = 1;  // nested guest calls from thunks need their own Jit
    p->exiting = false;
    Cpu* c = p->cpus[0].get();
    tl_cpu = c;
    auto& regs = c->jit->Regs();
    for (int i = 0; i < 16; i++) regs[i] = target->saved_regs[i];
    regs[13] = (target->saved_sp ? target->saved_sp : target->info.stack_base + target->info.stack_size) - 2048;
    regs[13] &= ~7u;

    for (;;) {
        u64 pend = target->pending.load();
        int sig = 0;
        for (int s = 1; s < 32; s++) {
            if ((pend & (1ull << s)) && !(target->sig_mask & (1u << (s - 1)))) {
                sig = s;
                break;
            }
        }
        if (!sig) {
            // kick() skips threads with an active proxy, so nothing may arrive unseen here.
            std::lock_guard lk(target->state_lock);
            u64 again = target->pending.load();
            bool more = false;
            for (int s = 1; s < 32; s++) {
                if ((again & (1ull << s)) && !(target->sig_mask & (1u << (s - 1)))) more = true;
            }
            if (more) continue;
            target->proxy_active.store(0, std::memory_order_release);
            break;
        }
        target->pending.fetch_and(~(1ull << sig));
        u32 handler = g_sigactions[sig].handler;
        if (handler <= 1) continue;
#ifdef RAVEN_DEV_OVERRIDE
        BLOG("proxy for tid=%d runs signal %d", target->tid, sig);
#endif
        regs[15] = g_return_stub;
        push_signal_frame(c, sig);
        run_until_return(c);
    }
    syscall(SYS_futex, reinterpret_cast<int*>(&target->proxy_active), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
    return nullptr;
}

void kick(GuestThread* t) {
    std::unique_lock lk(t->state_lock);
    if (t->proxy_active.load() || t->self_polling.load()) return;  // they poll pending themselves
    if (t->in_guest && t->blocked == 0) {
        if (t->depth > 0 && static_cast<size_t>(t->depth - 1) < t->cpus.size()) t->cpus[t->depth - 1]->jit->HaltExecution();
        return;
    }
    t->proxy_active.store(1, std::memory_order_release);
    lk.unlock();
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, proxy_main, t) != 0) bridge_fatal("cannot start signal proxy");
    pthread_attr_destroy(&attr);
}
}  // namespace

Cpu::~Cpu() {
    std::lock_guard lk(g_cpu_list_lock);
    for (size_t i = 0; i < g_all_cpus.size(); i++) {
        if (g_all_cpus[i] == this) {
            g_all_cpus.erase(g_all_cpus.begin() + static_cast<long>(i));
            break;
        }
    }
}

u8* Cpu::check(u32 a, u8* p) {
    if (a < 0x1000) {
        char where[96];
        elf_describe(jit->Regs()[15], where, sizeof where);
        BERR("guest null access addr=%08x near %s", a, where);
        static u8 sink[16];
        memset(sink, 0, sizeof sink);
        return sink;
    }
    return p;
}

void Cpu::ExceptionRaised(u32 pc, A32::Exception e) {
    switch (e) {
        case A32::Exception::SendEvent:
        case A32::Exception::SendEventLocal:
        case A32::Exception::WaitForInterrupt:
        case A32::Exception::WaitForEvent:
        case A32::Exception::Yield:
        case A32::Exception::PreloadData:
        case A32::Exception::PreloadDataWithIntentToWrite:
        case A32::Exception::PreloadInstruction:
            return;
        default:
            break;
    }
    char where[96], lr[96];
    elf_describe(pc, where, sizeof where);
    elf_describe(jit->Regs()[14], lr, sizeof lr);
    auto& r = jit->Regs();
    BERR("guest crash: tid=%d %s last_thunk=%s", thread->identity->tid, thread == thread->identity ? "own" : "proxy", thread->last_thunk ? thread->last_thunk : "-");
    BERR("r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    BERR("r8=%08x r9=%08x r10=%08x fp=%08x ip=%08x sp=%08x lr=%s", r[8], r[9], r[10], r[11], r[12], r[13], lr);
    // Words on the stack that point into the game's code approximate a backtrace.
    u32 base = thread->info.stack_base, top = base + thread->info.stack_size;
    int shown = 0;
    for (u32 a = r[13] & ~3u; a + 4 <= top && a >= base && shown < 32; a += 4) {
        char sym[96];
        u32 v = *gptr<u32>(a);
        if (v > 0x1000 && elf_describe(v & ~1u, sym, sizeof sym) && strstr(sym, ".so+")) {
            BERR("  stack %08x: %s", a, sym);
            shown++;
        }
    }
    bridge_fatal("guest exception %d at %s (lr=%s)", static_cast<int>(e), where, lr);
}

#ifdef RAVEN_PROFILE
static std::atomic<long long> g_prof_ns[kMaxThunks];
static std::atomic<long long> g_prof_n[kMaxThunks];
#endif

void Cpu::CallSVC(std::uint32_t swi) {
    Cpu* prev = tl_cpu;
    tl_cpu = this;
    if (swi >= kSvcBase) {
        u32 idx = swi - kSvcBase;
        if (idx >= static_cast<u32>(g_thunk_count.load())) bridge_fatal("bad thunk index %u", idx);
        tl_thunk_name = g_thunk_names[idx];
        thread->last_thunk = tl_thunk_name;
        thread->last_lr = jit->Regs()[14];
#ifdef RAVEN_PROFILE
        {
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            g_thunks[idx]();
            clock_gettime(CLOCK_MONOTONIC, &b);
            g_prof_ns[idx].fetch_add((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec), std::memory_order_relaxed);
            g_prof_n[idx].fetch_add(1, std::memory_order_relaxed);
        }
#else
        g_thunks[idx]();
#endif
    } else {
        auto& r = jit->Regs();
        switch (swi) {
            case kSvcReturn:
                finished = true;
                jit->HaltExecution();
                break;
            case kSvcSigReturn:
                sig_return();
                break;
            case kSvcCmpxchg: {  // __kuser_cmpxchg(old, new, ptr): r0 == 0 on success
                u32 expected = r[0];
                bool ok = __atomic_compare_exchange_n(gptr<u32>(r[2]), &expected, r[1], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                r[0] = ok ? 0 : 1;
                break;
            }
            case kSvcGetTls:
                r[0] = thread->tls_value;
                break;
            case kSvcBarrier:
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                break;
            case 0: {  // real Linux syscall convention: number in r7
                u32 nr = r[7];
                if (nr == 0xF0002) {  // cacheflush
                    u32 start = r[0], end = r[1];
                    if (end > start) cache_invalidate_all(start, end - start);
                    r[0] = 0;
                } else if (nr == 0xF0005) {  // set_tls
                    thread->tls_value = r[0];
                    r[0] = 0;
                } else if (nr == 224) {  // gettid
                    r[0] = static_cast<u32>(thread->tid);
                } else if (nr == 158) {  // sched_yield
                    sched_yield();
                    r[0] = 0;
                } else {
                    char where[96];
                    elf_describe(r[15], where, sizeof where);
                    BERR("unhandled guest syscall %u at %s", nr, where);
                    r[0] = static_cast<u32>(-38);  // -ENOSYS
                }
                break;
            }
            default:
                bridge_fatal("unknown bridge svc %u", swi);
        }
    }
    tl_cpu = prev;
    if (thread->identity->pending.load(std::memory_order_relaxed)) jit->HaltExecution();
}

u32& R(int i) { return tl_cpu->jit->Regs()[i]; }
u32& ExtR(int i) { return tl_cpu->jit->ExtRegs()[i]; }
u32& guest_sp() { return tl_cpu->jit->Regs()[13]; }
u32 guest_pc() { return tl_cpu->jit->Regs()[15]; }
u32 guest_lr() { return tl_cpu ? tl_cpu->jit->Regs()[14] : 0; }
const char* current_thunk_name() { return tl_thunk_name ? tl_thunk_name : "?"; }
void thread_stack_of(GuestThread* t, u32* base, u32* size) {
    *base = t->info.stack_base;
    *size = t->info.stack_size;
}
void sig_unwind_to(u32 sp) {
    auto* t = tl_cpu->thread;
    while (!t->sig_stack.empty() && t->sig_stack.back().frame < sp) {
        t->identity->sig_mask = t->sig_stack.back().old_mask;
        t->sig_stack.pop_back();
    }
}
GuestThread* cur_thread() { return ensure_thread(); }
GuestThreadInfo* thread_info() { return &ensure_thread()->identity->info; }
u32 guest_tid() { return static_cast<u32>(ensure_thread()->identity->tid); }

u32 guest_call_vec(u32 fn, const std::vector<u32>& args, u32* r1_out) {
    GuestThread* t = ensure_thread();
    if (t->identity == t) gate(t);  // a proxy must not wait for itself

    Cpu* parent = tl_cpu;
    int d;
    {
        std::lock_guard lk(t->state_lock);
        d = t->depth;
        if (t->cpus.size() <= static_cast<size_t>(d)) t->cpus.push_back(make_cpu(t, d));
    }
    Cpu* c = t->cpus[d].get();

    auto& regs = c->jit->Regs();
    u32 sp;
    if (parent && parent->thread == t) {
        auto& pr = parent->jit->Regs();
        for (int i = 4; i <= 12; i++) regs[i] = pr[i];
        sp = pr[13] - 256;
    } else if (d > 0 && t->saved_sp) {
        sp = t->saved_sp - 256;  // called back from host code running under guest frames
    } else {
        sp = t->info.stack_base + t->info.stack_size - 64;  // no guest frames below this call
    }
    const u32 entry_saved_sp = t->saved_sp;
    sp &= ~7u;
    size_t stack_args = args.size() > 4 ? args.size() - 4 : 0;
    sp -= static_cast<u32>(stack_args * 4);
    sp &= ~7u;
    for (size_t i = 0; i < args.size(); i++) {
        if (i < 4) regs[i] = args[i];
        else gptr<u32>(sp)[i - 4] = args[i];
    }
    regs[13] = sp;
    regs[14] = g_return_stub;
    regs[15] = fn & ~1u;
    c->jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));
    c->jit->SetFpscr(0x03C00000 & 0);  // default rounding, no flush-to-zero

    Cpu* saved_cpu = tl_cpu;
    int saved_blocked;
    {
        // A proxy started after the gate above would run its handler on the stack this thread
        // is about to use, so the check and the switch to "running guest code" are one step.
        std::unique_lock lk(t->state_lock);
        while (t->identity == t && t->proxy_active.load(std::memory_order_acquire)) {
            lk.unlock();
            gate(t);
            lk.lock();
        }
        t->depth = d + 1;
        t->in_guest = true;
        saved_blocked = t->blocked;
        t->blocked = 0;
    }
    tl_cpu = c;
    run_until_return(c);
    tl_cpu = saved_cpu;
    u32 r0 = regs[0];
    if (r1_out) *r1_out = regs[1];
    {
        std::lock_guard lk(t->state_lock);
        t->depth = d;
        t->blocked = saved_blocked;
        // Everything this call pushed is gone: keeping its deepest sp would make every later
        // call start lower, until the stack (one call per frame) ran into other mappings.
        t->saved_sp = d == 0 ? 0 : entry_saved_sp;
        if (d == 0) {
            t->in_guest = false;
            memcpy(t->saved_regs, regs.data(), sizeof t->saved_regs);
        }
    }
    return r0;
}

u32 guest_call(u32 fn, std::initializer_list<u32> args, u32* r1_out) {
    return guest_call_vec(fn, std::vector<u32>(args), r1_out);
}

BlockScope::BlockScope() {
    GuestThread* t = ensure_thread();
    if (t->identity != t) return;  // proxies poll on their own
    {
        std::lock_guard lk(t->state_lock);
        if (tl_cpu) {
            t->saved_sp = tl_cpu->jit->Regs()[13];
            memcpy(t->saved_regs, tl_cpu->jit->Regs().data(), sizeof t->saved_regs);
        }
        t->blocked++;
    }
    // A signal sent just before this point only halted the Jit, and this thread may now sleep
    // for a long time: hand it to a proxy, or the sender (the GC) would wait for it forever.
    u64 p = t->pending.load();
    for (int s = 1; s < 32; s++) {
        if ((p & (1ull << s)) && !(t->sig_mask & (1u << (s - 1)))) {
            kick(t);
            break;
        }
    }
}

BlockScope::~BlockScope() {
    GuestThread* t = ensure_thread();
    if (t->identity != t) return;
    {
        std::lock_guard lk(t->state_lock);
        t->blocked--;
    }
    gate(t);
}

void cache_invalidate_all(u32 start, u32 len) {
    std::lock_guard lk(g_cpu_list_lock);
    for (Cpu* c : g_all_cpus) c->jit->InvalidateCacheRange(start, len);
}

u32 thunk_address(const char* name, ThunkFn fn) {
    int idx = g_thunk_count.fetch_add(1);
    if (idx >= kMaxThunks) bridge_fatal("too many thunks");
    g_thunks[idx] = fn;
    g_thunk_names[idx] = name;
    u32 addr = alloc_stub_page_slot(8);
    gptr<u32>(addr)[0] = 0xEF000000u | (kSvcBase + idx);  // svc #idx
    gptr<u32>(addr)[1] = 0xE12FFF1Eu;                     // bx lr
    return addr;
}

// ---- signals
int sig_action(int sig, u32 new_act, u32 old_act) {
    if (sig <= 0 || sig >= kNumSig) return -1;
    std::lock_guard lk(g_sig_lock);
    if (old_act) *gptr<GuestSigaction>(old_act) = g_sigactions[sig];
    if (new_act) g_sigactions[sig] = *gptr<GuestSigaction>(new_act);
    return 0;
}

int sig_procmask(int how, u32 set, u32 old) {
    auto* id = me_identity();
    if (old) *gptr<u32>(old) = id->sig_mask;
    if (set) {
        u32 v = *gptr<u32>(set);
        if (how == 0) id->sig_mask |= v;
        else if (how == 1) id->sig_mask &= ~v;
        else if (how == 2) id->sig_mask = v;
    }
    if (tl_cpu && id->pending.load()) tl_cpu->jit->HaltExecution();
    return 0;
}

// Waits with the given mask until an unblocked signal is pending; the frame is pushed when
// the thunk returns to the run loop, so the guest sees sigsuspend return after the handler.
int sig_suspend(u32 mask_ptr) {
    auto* id = me_identity();
    u32 old = id->sig_mask;
    u32 mask = *gptr<u32>(mask_ptr);
    bool own = tl_thread == id;
    if (own) {
        std::lock_guard lk(id->state_lock);
        id->self_polling.store(1);
    }
    id->sig_mask = mask;
    BlockScope scope;
    for (;;) {
        u64 p = id->pending.load();
        for (int s = 1; s < 32; s++) {
            if ((p & (1ull << s)) && !(mask & (1u << (s - 1))) && g_sigactions[s].handler > 1) {
                // Run the handler nested so the old mask can be restored right after it.
                id->pending.fetch_and(~(1ull << s));
                Cpu* c = tl_cpu;
                u32 handler = g_sigactions[s].handler;
                u32 m = g_sigactions[s].mask;
                id->sig_mask = mask | m | (1u << (s - 1));
                guest_call(handler, {static_cast<u32>(s), 0, 0});
                tl_cpu = c;
                id->sig_mask = old;
                if (own) id->self_polling.store(0);
                return -1;
            }
        }
        struct timespec ts = {0, 200000};
        nanosleep(&ts, nullptr);
    }
}

void sig_return() {
    Cpu* c = tl_cpu;
    auto* t = c->thread;
    if (t->sig_stack.empty()) bridge_fatal("sigreturn without a signal frame");
    auto& regs = c->jit->Regs();
    SavedSig saved = t->sig_stack.back();
    t->sig_stack.pop_back();
    u32 uc = saved.frame + 128;
    u32* sc = gptr<u32>(uc + 20);
    for (int i = 0; i < 13; i++) regs[i] = sc[3 + i];
    regs[13] = sc[16];
    regs[14] = sc[17];
    regs[15] = sc[18] & ~1u;
    c->jit->SetCpsr(sc[19]);
    c->jit->ExtRegs() = saved.ext;
    c->jit->SetFpscr(saved.fpscr);
    t->identity->sig_mask = saved.old_mask;
    if (regs[15] == g_return_stub) {  // a proxy's handler has finished
        c->finished = true;
    }
    c->jit->HaltExecution();
}

int sig_kill_thread(GuestThread* t, int sig) {
    if (!t) return 3;  // ESRCH
    if (sig == 0) return 0;
#ifdef RAVEN_DEV_OVERRIDE
    {
        static std::atomic<int> count{0};
        if (count.fetch_add(1) < 120) {
            BLOG("kill from tid=%d (%s) to tid=%d sig=%d target: blocked=%d depth=%d proxy=%d polling=%d", me_identity()->tid,
                 tl_thread != me_identity() ? "proxy" : "own", t->tid, sig, t->blocked, t->depth, t->proxy_active.load(), t->self_polling.load());
        }
    }
#endif
    t->pending.fetch_or(1ull << sig);
    if (t == me_identity()) {
        if (tl_cpu) tl_cpu->jit->HaltExecution();
        return 0;
    }
    kick(t);
    return 0;
}

void guest_raise_self(int sig) { sig_kill_thread(me_identity(), sig); }

void sig_poll() {
    if (tl_cpu && me_identity()->pending.load()) tl_cpu->jit->HaltExecution();
}

GuestThread* thread_from_guest(u32 v) {
    std::lock_guard lk(g_threads_lock);
    auto it = g_by_id.find(v);
    return it == g_by_id.end() ? nullptr : it->second;
}

GuestThread* thread_from_tid(int tid) {
    std::lock_guard lk(g_threads_lock);
    auto it = g_by_tid.find(tid);
    return it == g_by_tid.end() ? nullptr : it->second;
}

u32 thread_guest_id(GuestThread* t) { return t->identity->info.pthread_id; }

// ---- thread lifetime
namespace {
struct SpawnArgs {
    u32 stack_base, stack_size, start, arg;
    GuestThread* t;
    std::atomic<int> ready{0};
};

void run_key_destructors(GuestThread* t);

void* host_thread_main(void* raw) {
    auto* a = static_cast<SpawnArgs*>(raw);
    GuestThread* t = a->t;
    t->tid = static_cast<int>(syscall(SYS_gettid));
    t->identity = t;
    tl_thread = t;
    {
        std::lock_guard lk(g_threads_lock);
        g_by_tid[t->tid] = t;
    }
    u32 start = a->start, arg = a->arg;
    a->ready.store(1);
    syscall(SYS_futex, reinterpret_cast<int*>(&a->ready), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    u32 ret = guest_call(start, {arg});
    if (!t->exiting) t->info.retval = ret;
    t->exiting = false;
    run_key_destructors(t);
    jni_end_guest_thread();
    {
        std::lock_guard lk(g_threads_lock);
        g_by_tid.erase(t->tid);
    }
    t->finished_flag.store(1);
    t->cpus.clear();
    tl_cpu = nullptr;
    return nullptr;
}

void run_key_destructors(GuestThread*) {}
}  // namespace

extern "C" u32 pthread_key_destructor_table(int);

int thread_spawn(u32* out, u32 stack_base, u32 stack_size, u32 start, u32 arg, bool detached) {
    auto* t = new_thread_record(stack_base, stack_size, true);
    auto* a = new SpawnArgs{stack_base, stack_size, start, arg, t};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    if (detached) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    t->joinable = !detached;
    int rc = pthread_create(&t->host, &attr, host_thread_main, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) return rc;
    *out = t->info.pthread_id;
    while (!a->ready.load()) {
        syscall(SYS_futex, reinterpret_cast<int*>(&a->ready), FUTEX_WAIT_PRIVATE, 0, nullptr, nullptr, 0);
    }
    delete a;
    return 0;
}

int thread_join(u32 id, u32* ret) {
    GuestThread* t = thread_from_guest(id);
    if (!t) return 3;
    BlockScope block;
    int rc = pthread_join(t->host, nullptr);
    if (ret) *ret = t->info.retval;
    {
        std::lock_guard lk(g_threads_lock);
        g_by_id.erase(id);
    }
    return rc;
}

int thread_detach(u32 id) {
    GuestThread* t = thread_from_guest(id);
    if (!t) return 3;
    t->joinable = false;
    return pthread_detach(t->host);
}

void thread_exit_current(u32 retval) {
    GuestThread* t = ensure_thread();
    t->info.retval = retval;
    t->exiting = true;
    if (tl_cpu) tl_cpu->jit->HaltExecution();
}

static void* watchdog_main(void*) {
    for (;;) {
        sleep(12);
#ifdef RAVEN_PROFILE
        {
            // Where guest threads spent the last period inside host code.
            std::vector<std::pair<long long, int>> top;
            int n = g_thunk_count.load();
            for (int i = 0; i < n; i++) {
                long long v = g_prof_ns[i].load();
                if (v > 0) top.push_back({v, i});
            }
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 12; i++) {
                int k = top[i].second;
                BLOG("prof %-24s %6lld ms %8lld calls", g_thunk_names[k], top[i].first / 1000000, g_prof_n[k].load());
            }
            for (int i = 0; i < n; i++) {
                g_prof_ns[i].store(0);
                g_prof_n[i].store(0);
            }
        }
#endif
        std::vector<GuestThread*> ts;
        {
            std::lock_guard lk(g_threads_lock);
            for (auto& kv : g_by_tid) ts.push_back(kv.second);
        }
        BLOG("--- guest threads ---");
        for (auto* t : ts) {
            char lr[96], pc[96];
            elf_describe(t->last_lr, lr, sizeof lr);
            elf_describe(t->saved_regs[14], pc, sizeof pc);
            BLOG("tid=%d depth=%d blocked=%d pending=%llx mask=%x proxy=%d last=%s from %s saved_lr=%s", t->tid, t->depth, t->blocked,
                 (unsigned long long)t->pending.load(), t->sig_mask, t->proxy_active.load(), t->last_thunk ? t->last_thunk : "-", lr, pc);
        }
    }
    return nullptr;
}

void cpu_start_watchdog() {
    pthread_t th;
    pthread_create(&th, nullptr, watchdog_main, nullptr);
    pthread_detach(th);
}

void cpu_init() {
    g_monitor = new ExclusiveMonitor(512);
    auto svc_stub = [](int n) {
        u32 addr = alloc_stub_page_slot(8);
        gptr<u32>(addr)[0] = 0xEF000000u | static_cast<u32>(n);
        gptr<u32>(addr)[1] = 0xE12FFF1Eu;
        return addr;
    };
    g_return_stub = svc_stub(kSvcReturn);
    g_sigreturn_stub = svc_stub(kSvcSigReturn);

    // kuser helper page used by old bionic atomics and TLS reads.
    void* page = mmap(gptr(0xFFFF0000u), 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (page != gptr(0xFFFF0000u)) bridge_fatal("cannot map kuser page");
    auto put = [](u32 addr, int n) {
        gptr<u32>(addr)[0] = 0xEF000000u | static_cast<u32>(n);
        gptr<u32>(addr)[1] = 0xE12FFF1Eu;
    };
    put(0xFFFF0FA0u, kSvcBarrier);
    put(0xFFFF0FC0u, kSvcCmpxchg);
    put(0xFFFF0FE0u, kSvcGetTls);
    *gptr<u32>(0xFFFF0FFCu) = 5;  // __kuser_helper_version
}
