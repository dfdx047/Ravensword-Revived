// 32-bit bionic pthreads, implemented on the guest's own 4-byte words with futexes.
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <mutex>
#include <tuple>
#include "common.h"

namespace {
long futex(u32 addr, int op, u32 val, const struct timespec* ts) {
    return syscall(SYS_futex, gptr<int>(addr), op, val, ts, nullptr, 0);
}
inline std::atomic<u32>* atom(u32 a) { return reinterpret_cast<std::atomic<u32>*>(gptr(a)); }

// Mutex word: bits 0-1 state (0 free, 1 locked, 2 contended), 2-12 count, 14-15 type, 16-31 owner.
constexpr u32 kStateMask = 3, kTypeMask = 0xC000, kRecursive = 0x4000, kErrorCheck = 0x8000, kCountMask = 0x1FFC, kCountOne = 4;

// A futex wait that wakes periodically so pending guest signals are serviced by the proxy path.
int wait_word(u32 addr, u32 expected, const struct timespec* abs_rt) {
    for (;;) {
        struct timespec slice = {0, 50 * 1000000};
        if (abs_rt) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            s64 left = (abs_rt->tv_sec - now.tv_sec) * 1000000000ll + (abs_rt->tv_nsec - now.tv_nsec);
            if (left <= 0) return ETIMEDOUT;
            if (left < slice.tv_nsec) slice.tv_nsec = left;
        }
        long r = futex(addr, FUTEX_WAIT_PRIVATE, expected, &slice);
        if (r == 0) return 0;
        if (errno == EAGAIN) return 0;
        if (errno == ETIMEDOUT) {
            if (!abs_rt) {
                if (atom(addr)->load() != expected) return 0;
                continue;
            }
            continue;
        }
        if (errno != EINTR) return 0;
    }
}

int mutex_lock(u32 m, const struct timespec* abs_rt = nullptr, bool try_only = false) {
    auto* w = atom(m);
    u32 v = w->load(std::memory_order_relaxed);
    u32 type = v & kTypeMask;
    u32 tid = guest_tid() & 0xFFFF;
    if (type) {
        if ((v >> 16) == tid && (v & kStateMask)) {
            if (type == kErrorCheck) return EDEADLK;
            if ((v & kCountMask) == kCountMask) return EAGAIN;
            w->fetch_add(kCountOne);
            return 0;
        }
    }
    u32 unlocked = type;
    u32 locked = type | 1 | (type ? tid << 16 : 0);
    u32 expected = unlocked;
    if (w->compare_exchange_strong(expected, locked, std::memory_order_acquire)) return 0;
    if (try_only) return EBUSY;

    BlockScope block;
    u32 contended = type | 2 | (type ? tid << 16 : 0);
    for (;;) {
        u32 cur = w->load();
        if ((cur & kStateMask) == 0) {
            expected = cur;
            if (w->compare_exchange_strong(expected, contended, std::memory_order_acquire)) return 0;
            continue;
        }
        if ((cur & kStateMask) == 1) {
            u32 want = (cur & ~kStateMask) | 2;
            expected = cur;
            if (!w->compare_exchange_strong(expected, want)) continue;
            cur = want;
        }
        int rc = wait_word(m, cur, abs_rt);
        if (rc) return rc;
    }
}

int mutex_unlock(u32 m) {
    auto* w = atom(m);
    u32 v = w->load(std::memory_order_relaxed);
    u32 type = v & kTypeMask;
    if (type) {
        u32 tid = guest_tid() & 0xFFFF;
        if ((v >> 16) != tid || !(v & kStateMask)) return EPERM;
        if (v & kCountMask) {
            w->fetch_sub(kCountOne);
            return 0;
        }
    }
    u32 old = w->exchange(type, std::memory_order_release);
    if ((old & kStateMask) == 2) futex(m, FUTEX_WAKE_PRIVATE, 1, nullptr);
    return 0;
}

void t_mutex_init() {
    u32 m = R(0), a = R(1);
    u32 type = 0;
    if (a) {
        u32 k = *gptr<u32>(a);
        type = k == 1 ? kRecursive : k == 2 ? kErrorCheck : 0;
    }
    *gptr<u32>(m) = type;
    R(0) = 0;
}
void t_mutex_lock() { R(0) = mutex_lock(R(0)); }
void t_mutex_trylock() { R(0) = mutex_lock(R(0), nullptr, true); }
void t_mutex_unlock() { R(0) = mutex_unlock(R(0)); }
void t_mutex_destroy() { *gptr<u32>(R(0)) = 0xDEADu; *gptr<u32>(R(0)) = 0; R(0) = 0; }
void t_mutexattr_init() { *gptr<u32>(R(0)) = 0; R(0) = 0; }
void t_mutexattr_settype() { *gptr<u32>(R(0)) = R(1); R(0) = 0; }
void t_zero() { R(0) = 0; }

// Condition variable word: a sequence counter.
int cond_wait(u32 c, u32 m, const struct timespec* abs_rt) {
    auto* w = atom(c);
    u32 seq = w->load();
    mutex_unlock(m);
    int rc;
    {
        BlockScope block;
        rc = wait_word(c, seq, abs_rt);
    }
    int lr = mutex_lock(m);
    return rc ? rc : lr;
}
void t_cond_init() { *gptr<u32>(R(0)) = 0; R(0) = 0; }
void t_cond_wait() { R(0) = cond_wait(R(0), R(1), nullptr); }
void t_cond_timedwait() {
    const s32* t = gptr<s32>(R(2));
    struct timespec ts = {t[0], t[1]};
    R(0) = cond_wait(R(0), R(1), &ts);
}
void t_cond_signal() { atom(R(0))->fetch_add(1); futex(R(0), FUTEX_WAKE_PRIVATE, 1, nullptr); R(0) = 0; }
void t_cond_broadcast() { atom(R(0))->fetch_add(1); futex(R(0), FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr); R(0) = 0; }

// Semaphores
void t_sem_init() { *gptr<u32>(R(0)) = R(2); R(0) = 0; }
void t_sem_post() {
    atom(R(0))->fetch_add(1);
    futex(R(0), FUTEX_WAKE_PRIVATE, 1, nullptr);
    R(0) = 0;
}
int sem_wait_impl(u32 s, const struct timespec* abs_rt, bool try_only) {
    auto* w = atom(s);
    for (;;) {
        u32 v = w->load();
        if (v > 0) {
            if (w->compare_exchange_weak(v, v - 1, std::memory_order_acquire)) return 0;
            continue;
        }
        if (try_only) return EAGAIN;
        BlockScope block;
        int rc = wait_word(s, 0, abs_rt);
        if (rc) return rc;
    }
}
void t_sem_wait() {
    int rc = sem_wait_impl(R(0), nullptr, false);
    if (rc) errno = rc;
    R(0) = rc ? 0xFFFFFFFFu : 0;
}
void t_sem_trywait() {
    int rc = sem_wait_impl(R(0), nullptr, true);
    if (rc) errno = rc;
    R(0) = rc ? 0xFFFFFFFFu : 0;
}
void t_sem_timedwait() {
    const s32* t = gptr<s32>(R(1));
    struct timespec ts = {t[0], t[1]};
    int rc = sem_wait_impl(R(0), &ts, false);
    if (rc) errno = rc;
    R(0) = rc ? 0xFFFFFFFFu : 0;
}
void t_sem_getvalue() { *gptr<u32>(R(1)) = atom(R(0))->load(); R(0) = 0; }

// pthread_once: 0 = not run, 1 = running, 2 = done
void t_once() {
    u32 ctl = R(0), fn = R(1);
    auto* w = atom(ctl);
    for (;;) {
        u32 v = w->load();
        if (v == 2) break;
        if (v == 0) {
            if (w->compare_exchange_strong(v, 1)) {
                guest_call(fn, {});
                w->store(2);
                futex(ctl, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr);
                break;
            }
            continue;
        }
        BlockScope block;
        wait_word(ctl, 1, nullptr);
    }
    R(0) = 0;
}

// Thread attributes: flags, stack_base, stack_size, guard, policy, priority.
struct GuestAttr { u32 flags, stack_base, stack_size, guard, policy, priority; };
constexpr u32 kDetached = 1, kDefaultStackSize = 1u << 20;
void t_attr_init() {
    auto* a = gptr<GuestAttr>(R(0));
    *a = {0, 0, kDefaultStackSize, 4096, 0, 0};
    R(0) = 0;
}
void t_attr_setdetachstate() {
    auto* a = gptr<GuestAttr>(R(0));
    if (R(1)) a->flags |= kDetached;
    else a->flags &= ~kDetached;
    R(0) = 0;
}
void t_attr_getdetachstate() { *gptr<u32>(R(1)) = (gptr<GuestAttr>(R(0))->flags & kDetached) ? 1 : 0; R(0) = 0; }
void t_attr_setstacksize() { gptr<GuestAttr>(R(0))->stack_size = R(1); R(0) = 0; }
void t_attr_setschedparam() { gptr<GuestAttr>(R(0))->priority = *gptr<u32>(R(1)); R(0) = 0; }
void t_attr_setschedpolicy() { gptr<GuestAttr>(R(0))->policy = R(1); R(0) = 0; }
void t_attr_getstack() {
    auto* a = gptr<GuestAttr>(R(0));
    *gptr<u32>(R(1)) = a->stack_base;
    *gptr<u32>(R(2)) = a->stack_size;
    R(0) = 0;
}
void t_getattr_np() {
    GuestThread* t = thread_from_guest(R(0));
    auto* a = gptr<GuestAttr>(R(1));
    *a = {0, 0, 0, 4096, 0, 0};
    if (t) {
        thread_stack_of(t, &a->stack_base, &a->stack_size);
    } else {
        a->stack_base = thread_info()->stack_base;
        a->stack_size = thread_info()->stack_size;
    }
    R(0) = 0;
}
void t_create() {
    u32 out = R(0), attr = R(1), start = R(2), arg = R(3);
    u32 stack_base = 0, stack_size = 0;
    bool detached = false;
    if (attr) {
        auto* a = gptr<GuestAttr>(attr);
        stack_base = a->stack_base;
        stack_size = a->stack_size;
        detached = a->flags & kDetached;
    }
    if (!stack_base) stack_size = 0;  // the bridge allocates a roomy stack
    R(0) = thread_spawn(gptr<u32>(out), stack_base, stack_size, start, arg, detached);
}
void t_join() {
    u32 ret = 0;
    u32 out = R(1);
    int rc = thread_join(R(0), &ret);
    if (out) *gptr<u32>(out) = ret;
    R(0) = rc;
}
void t_detach() { R(0) = thread_detach(R(0)); }
void t_self() { R(0) = thread_info()->pthread_id; }
void t_equal() { R(0) = R(0) == R(1); }
void t_exit() { thread_exit_current(R(0)); }
void t_kill() { R(0) = sig_kill_thread(thread_from_guest(R(0)), static_cast<int>(R(1))); }

// Thread-specific data
std::mutex g_key_lock;
u32 g_key_used[8];
u32 g_key_dtor[256];
void t_key_create() {
    std::lock_guard lk(g_key_lock);
    for (u32 k = 0; k < 256; k++) {
        if (!(g_key_used[k / 32] & (1u << (k % 32)))) {
            g_key_used[k / 32] |= 1u << (k % 32);
            g_key_dtor[k] = R(1);
            *gptr<u32>(R(0)) = k;
            R(0) = 0;
            return;
        }
    }
    R(0) = EAGAIN;
}
void t_key_delete() {
    std::lock_guard lk(g_key_lock);
    u32 k = R(0);
    if (k < 256) g_key_used[k / 32] &= ~(1u << (k % 32));
    R(0) = 0;
}
void t_getspecific() { u32 k = R(0); R(0) = k < 256 ? thread_info()->keys[k] : 0; }
void t_setspecific() { u32 k = R(0); if (k < 256) thread_info()->keys[k] = R(1); R(0) = 0; }

#define C(name, fn) {name, fn}
const Hle kTable[] = {
    C("pthread_mutex_init", t_mutex_init), C("pthread_mutex_lock", t_mutex_lock), C("pthread_mutex_trylock", t_mutex_trylock),
    C("pthread_mutex_unlock", t_mutex_unlock), C("pthread_mutex_destroy", t_mutex_destroy),
    C("pthread_mutexattr_init", t_mutexattr_init), C("pthread_mutexattr_destroy", t_zero), C("pthread_mutexattr_settype", t_mutexattr_settype),
    C("pthread_cond_init", t_cond_init), C("pthread_cond_destroy", t_zero), C("pthread_cond_wait", t_cond_wait), C("pthread_cond_timedwait", t_cond_timedwait),
    C("pthread_cond_signal", t_cond_signal), C("pthread_cond_broadcast", t_cond_broadcast),
    C("sem_init", t_sem_init), C("sem_destroy", t_zero), C("sem_post", t_sem_post), C("sem_wait", t_sem_wait), C("sem_trywait", t_sem_trywait),
    C("sem_timedwait", t_sem_timedwait), C("sem_getvalue", t_sem_getvalue),
    C("pthread_once", t_once),
    C("pthread_attr_init", t_attr_init), C("pthread_attr_destroy", t_zero), C("pthread_attr_setdetachstate", t_attr_setdetachstate),
    C("pthread_attr_getdetachstate", t_attr_getdetachstate), C("pthread_attr_setstacksize", t_attr_setstacksize), C("pthread_attr_setschedparam", t_attr_setschedparam),
    C("pthread_attr_setschedpolicy", t_attr_setschedpolicy), C("pthread_attr_getstack", t_attr_getstack), C("pthread_getattr_np", t_getattr_np),
    C("pthread_getschedparam", t_zero), C("pthread_setschedparam", t_zero),
    C("pthread_create", t_create), C("pthread_join", t_join), C("pthread_detach", t_detach), C("pthread_self", t_self), C("pthread_equal", t_equal), C("pthread_exit", t_exit), C("pthread_kill", t_kill),
    C("pthread_key_create", t_key_create), C("pthread_key_delete", t_key_delete), C("pthread_getspecific", t_getspecific), C("pthread_setspecific", t_setspecific),
};
}  // namespace

void pthread_register() { hle_register(kTable, sizeof kTable / sizeof kTable[0]); }
