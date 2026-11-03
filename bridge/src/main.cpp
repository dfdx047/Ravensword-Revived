// Entry point: this library replaces libunity.so inside the APK. It starts the emulator, loads
// the game's original 32-bit libmono and libunity and forwards JNI_OnLoad to them.
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include "common.h"

static void bridge_boot(JavaVM* vm) {
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&bridge_boot), &info);
    std::string self = info.dli_fname ? info.dli_fname : "";
    g_app_lib_dir = self.substr(0, self.rfind('/'));
    BLOG("bridge starting, library dir %s", g_app_lib_dir.c_str());

    mem_init();
    cpu_init();
#ifdef RAVEN_DEV_OVERRIDE
    cpu_start_watchdog();
#endif
    libc_init();
    pthread_register();
    gl_register();
    guest_env_init();
    jni_bridge_init(vm);

    GuestLib* mono = elf_load(g_app_lib_dir + "/libravenguestmono.so", "libmono.so");
    GuestLib* unity = elf_load(g_app_lib_dir + "/libravenguestunity.so", "libunity.so");
    elf_run_init(mono);
    elf_run_init(unity);

    JNIEnv* e = nullptr;
    vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
    jni_register_exported(e, unity);

    u32 onload = unity->lookup("JNI_OnLoad");
    if (!onload) bridge_fatal("guest libunity has no JNI_OnLoad");
    u32 ver = guest_call(onload, {guest_javavm(), 0});
    BLOG("guest JNI_OnLoad returned 0x%x", ver);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
#ifdef RAVEN_DEV_OVERRIDE
    // Development builds: a newer bridge pushed with run-as replaces the installed one.
    static const char kDev[] = "/data/data/com.crescentmoongames.ravensword2/files/dev/libunity.so";
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&bridge_boot), &info);
    struct stat st{};
    if (info.dli_fname && strcmp(info.dli_fname, kDev) != 0 && stat(kDev, &st) == 0) {
        void* h = dlopen(kDev, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            auto fn = reinterpret_cast<jint (*)(JavaVM*, void*)>(dlsym(h, "JNI_OnLoad"));
            if (fn) return fn(vm, reserved);
        }
        BERR("dev override failed: %s", dlerror());
    }
#endif
    bridge_boot(vm);
    return JNI_VERSION_1_6;
}
