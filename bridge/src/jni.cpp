// JNI in both directions. Guests see 32-bit handles for objects and IDs; Java sees host
// trampolines for natives registered by the guest.
#include <unordered_map>
#include <map>
#include <tuple>
#include "common.h"

extern "C" u8 bridge_tramp_table[];
constexpr int kTrampCount = 512;
constexpr int kTrampSize = 8;

JavaVM* g_host_vm = nullptr;

namespace {
u32 g_guest_env_ptr, g_guest_vm_ptr;
jobject g_app_loader = nullptr;
jmethodID g_load_class = nullptr;

thread_local JNIEnv* tl_env = nullptr;
thread_local bool tl_attached = false;

JNIEnv* env() {
    if (tl_env) return tl_env;
    if (g_host_vm->GetEnv(reinterpret_cast<void**>(&tl_env), JNI_VERSION_1_6) != JNI_OK) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "RavenGuest", nullptr};
        if (g_host_vm->AttachCurrentThreadAsDaemon(&tl_env, &args) != JNI_OK) bridge_fatal("cannot attach thread to the JVM");
        tl_attached = true;
    }
    return tl_env;
}

// ---- object handles
struct RefTable {
    std::mutex lock;
    std::vector<jobject> items{nullptr};
    std::vector<u32> free_list;
};
RefTable g_refs;
thread_local std::vector<u32> tl_locals;
thread_local std::vector<size_t> tl_frames;

u32 ref_add(jobject global) {
    std::lock_guard lk(g_refs.lock);
    u32 i;
    if (!g_refs.free_list.empty()) {
        i = g_refs.free_list.back();
        g_refs.free_list.pop_back();
        g_refs.items[i] = global;
    } else {
        i = static_cast<u32>(g_refs.items.size());
        g_refs.items.push_back(global);
    }
    return (i << 2) | 1;
}
jobject ref_get(u32 h) {
    if (!h) return nullptr;
    std::lock_guard lk(g_refs.lock);
    return g_refs.items.at(h >> 2);
}
void ref_del(u32 h) {
    if (!h) return;
    jobject g;
    {
        std::lock_guard lk(g_refs.lock);
        g = g_refs.items.at(h >> 2);
        g_refs.items[h >> 2] = nullptr;
        g_refs.free_list.push_back(h >> 2);
    }
    if (g) env()->DeleteGlobalRef(g);
}
// Takes ownership of a host local reference and returns a handle valid for this frame.
u32 wrap_local(jobject lo) {
    if (!lo) return 0;
    JNIEnv* e = env();
    jobject g = e->NewGlobalRef(lo);
    e->DeleteLocalRef(lo);
    u32 h = ref_add(g);
    tl_locals.push_back(h);
    return h;
}
void pop_locals(size_t mark) {
    while (tl_locals.size() > mark) {
        ref_del(tl_locals.back());
        tl_locals.pop_back();
    }
}

// ---- method and field IDs
struct IdRec {
    void* id;
    std::string args;  // one char per argument; L for any reference
    char ret;
};
std::mutex g_id_lock;
std::vector<IdRec*> g_ids{nullptr};
std::map<void*, u32> g_id_map;

void parse_sig(const char* sig, std::string& args, char& ret) {
    const char* p = sig;
    if (*p == '(') {
        p++;
        while (*p && *p != ')') {
            char c = *p;
            if (c == '[') {
                while (*p == '[') p++;
                if (*p == 'L') while (*p != ';') p++;
                p++;
                args.push_back('L');
            } else if (c == 'L') {
                while (*p != ';') p++;
                p++;
                args.push_back('L');
            } else {
                args.push_back(c);
                p++;
            }
        }
        if (*p == ')') p++;
    }
    ret = (*p == '[' || *p == 'L') ? 'L' : *p;
}

u32 id_intern(void* id, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(g_id_lock);
    auto it = g_id_map.find(id);
    if (it != g_id_map.end()) return it->second;
    auto* r = new IdRec{id, {}, 0};
    parse_sig(sig, r->args, r->ret);
    u32 h = static_cast<u32>(g_ids.size());
    g_ids.push_back(r);
    g_id_map[id] = h;
    return h;
}
IdRec* id_get(u32 h) {
    std::lock_guard lk(g_id_lock);
    if (!h || h >= g_ids.size()) bridge_fatal("bad jni id %u", h);
    return g_ids[h];
}

// ---- direct buffers: high host addresses get a low shadow, written back when the native returns
struct Shadow { u8* host; u32 guest; u32 cap; };
std::mutex g_shadow_lock;
std::unordered_map<void*, Shadow> g_shadows;
thread_local std::vector<Shadow> tl_used_shadows;

void flush_shadows(size_t mark) {
    while (tl_used_shadows.size() > mark) {
        auto& s = tl_used_shadows.back();
        memcpy(s.host, gptr(s.guest), s.cap);
        tl_used_shadows.pop_back();
    }
}

// ---- arguments
template <class Rd>
void read_args(Rd& rd, const IdRec* m, jvalue* out) {
    for (size_t i = 0; i < m->args.size(); i++) {
        switch (m->args[i]) {
            case 'Z': out[i].z = static_cast<jboolean>(rd.next32()); break;
            case 'B': out[i].b = static_cast<jbyte>(rd.next32()); break;
            case 'C': out[i].c = static_cast<jchar>(rd.next32()); break;
            case 'S': out[i].s = static_cast<jshort>(rd.next32()); break;
            case 'I': out[i].i = static_cast<jint>(rd.next32()); break;
            case 'J': out[i].j = static_cast<jlong>(rd.next64()); break;
            case 'F': {  // varargs promote float to double
                u64 v = rd.next64();
                double d;
                memcpy(&d, &v, 8);
                out[i].f = static_cast<jfloat>(d);
                break;
            }
            case 'D': {
                u64 v = rd.next64();
                memcpy(&out[i].d, &v, 8);
                break;
            }
            default: out[i].l = ref_get(rd.next32()); break;
        }
    }
}
void read_args_a(u32 arr, const IdRec* m, jvalue* out) {
    for (size_t i = 0; i < m->args.size(); i++) {
        u8* e = gptr<u8>(arr + static_cast<u32>(i) * 8);
        switch (m->args[i]) {
            case 'Z': out[i].z = *e; break;
            case 'B': out[i].b = static_cast<jbyte>(*e); break;
            case 'C': memcpy(&out[i].c, e, 2); break;
            case 'S': memcpy(&out[i].s, e, 2); break;
            case 'I': memcpy(&out[i].i, e, 4); break;
            case 'J': memcpy(&out[i].j, e, 8); break;
            case 'F': memcpy(&out[i].f, e, 4); break;
            case 'D': memcpy(&out[i].d, e, 8); break;
            default: { u32 h; memcpy(&h, e, 4); out[i].l = ref_get(h); break; }
        }
    }
}

// The 2013 game and plugins often ignore Java exceptions. Android 5+ throws in places that used
// to succeed, so they are logged and dropped instead of taking the whole process down.
void drop_stale_exception(JNIEnv* e, const char* where) {
    if (!e->ExceptionCheck()) return;
    BERR("ignored Java exception (%s):", where);
    e->ExceptionDescribe();  // also clears it
    e->ExceptionClear();
}

enum CallKind { kVirtual, kNonvirtual, kStatic };
void do_call(CallKind kind, char ret, int form) {
    u32 obj_h = R(1);
    u32 cls_h = kind == kNonvirtual ? R(2) : 0;
    u32 mid_h = kind == kNonvirtual ? R(3) : R(2);
    IdRec* m = id_get(mid_h);
    jvalue a[64];
    if (m->args.size() > 64) bridge_fatal("too many JNI args");
    int first = kind == kNonvirtual ? 4 : 3;
    if (form == 0) {
        ArgReader rd;
        rd.reg = first;
        read_args(rd, m, a);
    } else {
        u32 p = first < 4 ? R(first) : *gptr<u32>(guest_sp());
        if (form == 1) {
            VaReader rd(p);
            read_args(rd, m, a);
        } else {
            read_args_a(p, m, a);
        }
    }
    JNIEnv* e = env();
    drop_stale_exception(e, "before call");
    jobject o = ref_get(obj_h);
    auto mid = static_cast<jmethodID>(m->id);
    jvalue r{};
    {
        BlockScope block;
#define DISPATCH(RET, F)                                                                        \
    if (kind == kVirtual) F = e->Call##RET##MethodA(o, mid, a);                                 \
    else if (kind == kStatic) F = e->CallStatic##RET##MethodA(static_cast<jclass>(o), mid, a);  \
    else F = e->CallNonvirtual##RET##MethodA(o, static_cast<jclass>(ref_get(cls_h)), mid, a);
        switch (ret) {
            case 'L': { jobject v; DISPATCH(Object, v); r.l = v; break; }
            case 'Z': DISPATCH(Boolean, r.z); break;
            case 'B': DISPATCH(Byte, r.b); break;
            case 'C': DISPATCH(Char, r.c); break;
            case 'S': DISPATCH(Short, r.s); break;
            case 'I': DISPATCH(Int, r.i); break;
            case 'J': DISPATCH(Long, r.j); break;
            case 'F': DISPATCH(Float, r.f); break;
            case 'D': DISPATCH(Double, r.d); break;
            default:
                if (kind == kVirtual) e->CallVoidMethodA(o, mid, a);
                else if (kind == kStatic) e->CallStaticVoidMethodA(static_cast<jclass>(o), mid, a);
                else e->CallNonvirtualVoidMethodA(o, static_cast<jclass>(ref_get(cls_h)), mid, a);
        }
#undef DISPATCH
    }
    switch (ret) {
        case 'L': R(0) = wrap_local(r.l); break;
        case 'Z': R(0) = r.z; break;
        case 'B': R(0) = static_cast<u32>(static_cast<s32>(r.b)); break;
        case 'C': R(0) = r.c; break;
        case 'S': R(0) = static_cast<u32>(static_cast<s32>(r.s)); break;
        case 'I': R(0) = static_cast<u32>(r.i); break;
        case 'J': write_ret<long long>(r.j); break;
        case 'F': write_ret<float>(r.f); break;
        case 'D': write_ret<double>(r.d); break;
        default: break;
    }
}

constexpr char kTypes[] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'V'};
constexpr int kElemSize[] = {0, 1, 1, 2, 2, 4, 8, 4, 8};

void field_get(bool is_static, char t) {
    JNIEnv* e = env();
    jobject o = ref_get(R(1));
    auto id = static_cast<jfieldID>(id_get(R(2))->id);
#define FG(RET, FIELD) (is_static ? e->GetStatic##RET##Field(static_cast<jclass>(o), id) : e->Get##RET##Field(o, id))
    switch (t) {
        case 'L': R(0) = wrap_local(FG(Object, l)); break;
        case 'Z': R(0) = FG(Boolean, z); break;
        case 'B': R(0) = static_cast<u32>(static_cast<s32>(FG(Byte, b))); break;
        case 'C': R(0) = FG(Char, c); break;
        case 'S': R(0) = static_cast<u32>(static_cast<s32>(FG(Short, s))); break;
        case 'I': R(0) = static_cast<u32>(FG(Int, i)); break;
        case 'J': write_ret<long long>(FG(Long, j)); break;
        case 'F': write_ret<float>(FG(Float, f)); break;
        case 'D': write_ret<double>(FG(Double, d)); break;
    }
#undef FG
}
void field_set(bool is_static, char t) {
    JNIEnv* e = env();
    jobject o = ref_get(R(1));
    auto id = static_cast<jfieldID>(id_get(R(2))->id);
    ArgReader a;
    a.reg = 3;
#define FS(RET, V) { if (is_static) e->SetStatic##RET##Field(static_cast<jclass>(o), id, V); else e->Set##RET##Field(o, id, V); }
    switch (t) {
        case 'L': FS(Object, ref_get(a.next32())); break;
        case 'Z': FS(Boolean, static_cast<jboolean>(a.next32())); break;
        case 'B': FS(Byte, static_cast<jbyte>(a.next32())); break;
        case 'C': FS(Char, static_cast<jchar>(a.next32())); break;
        case 'S': FS(Short, static_cast<jshort>(a.next32())); break;
        case 'I': FS(Int, static_cast<jint>(a.next32())); break;
        case 'J': FS(Long, static_cast<jlong>(a.next64())); break;
        case 'F': { u32 b = a.next32(); float f; memcpy(&f, &b, 4); FS(Float, f); break; }
        case 'D': { u64 b = a.next64(); double d; memcpy(&d, &b, 8); FS(Double, d); break; }
    }
#undef FS
}

void array_region(int type, bool set) {
    JNIEnv* e = env();
    auto arr = ref_get(R(1));
    jsize start = static_cast<jsize>(R(2)), len = static_cast<jsize>(R(3));
    void* buf = gptr(*gptr<u32>(guest_sp()));
#define AR(RET, JT, ARR) \
    if (set) e->Set##RET##ArrayRegion(static_cast<ARR>(arr), start, len, static_cast<const JT*>(buf)); \
    else e->Get##RET##ArrayRegion(static_cast<ARR>(arr), start, len, static_cast<JT*>(buf));
    switch (type) {
        case 1: AR(Boolean, jboolean, jbooleanArray); break;
        case 2: AR(Byte, jbyte, jbyteArray); break;
        case 3: AR(Char, jchar, jcharArray); break;
        case 4: AR(Short, jshort, jshortArray); break;
        case 5: AR(Int, jint, jintArray); break;
        case 6: AR(Long, jlong, jlongArray); break;
        case 7: AR(Float, jfloat, jfloatArray); break;
        case 8: AR(Double, jdouble, jdoubleArray); break;
    }
#undef AR
}
void array_get_elements(int type) {
    JNIEnv* e = env();
    auto arr = ref_get(R(1));
    u32 is_copy = R(2);
    jsize len = e->GetArrayLength(static_cast<jarray>(arr));
    u32 p = gmalloc(static_cast<u32>(len) * kElemSize[type] + 16);
#define AG(RET, JT, ARR) e->Get##RET##ArrayRegion(static_cast<ARR>(arr), 0, len, gptr<JT>(p));
    switch (type) {
        case 1: AG(Boolean, jboolean, jbooleanArray); break;
        case 2: AG(Byte, jbyte, jbyteArray); break;
        case 3: AG(Char, jchar, jcharArray); break;
        case 4: AG(Short, jshort, jshortArray); break;
        case 5: AG(Int, jint, jintArray); break;
        case 6: AG(Long, jlong, jlongArray); break;
        case 7: AG(Float, jfloat, jfloatArray); break;
        case 8: AG(Double, jdouble, jdoubleArray); break;
    }
#undef AG
    if (is_copy) *gptr<u8>(is_copy) = 1;
    R(0) = p;
}
void array_release_elements(int type) {
    JNIEnv* e = env();
    auto arr = ref_get(R(1));
    u32 p = R(2), mode = R(3);
    if (mode != JNI_ABORT) {
        jsize len = e->GetArrayLength(static_cast<jarray>(arr));
#define AS(RET, JT, ARR) e->Set##RET##ArrayRegion(static_cast<ARR>(arr), 0, len, gptr<JT>(p));
        switch (type) {
            case 1: AS(Boolean, jboolean, jbooleanArray); break;
            case 2: AS(Byte, jbyte, jbyteArray); break;
            case 3: AS(Char, jchar, jcharArray); break;
            case 4: AS(Short, jshort, jshortArray); break;
            case 5: AS(Int, jint, jintArray); break;
            case 6: AS(Long, jlong, jlongArray); break;
            case 7: AS(Float, jfloat, jfloatArray); break;
            case 8: AS(Double, jdouble, jdoubleArray); break;
        }
#undef AS
    }
    if (mode != JNI_COMMIT) gfree(p);
}
int array_type_of(jobject arr) {
    JNIEnv* e = env();
    static jclass cls[9];
    static const char* names[9] = {nullptr, "[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    if (!cls[1]) {
        for (int i = 1; i < 9; i++) {
            jclass c = e->FindClass(names[i]);
            cls[i] = static_cast<jclass>(e->NewGlobalRef(c));
            e->DeleteLocalRef(c);
        }
    }
    for (int i = 1; i < 9; i++) if (e->IsInstanceOf(arr, cls[i])) return i;
    bridge_fatal("unknown primitive array type");
}

// ---- natives registered by the guest
struct Native { u32 fn; std::string args; char ret; bool render; };
std::mutex g_native_lock;
std::vector<Native> g_natives;

void register_natives(jclass cls, u32 table, int count) {
    JNIEnv* e = env();
    std::vector<JNINativeMethod> ms;
    for (int i = 0; i < count; i++) {
        u32* m = gptr<u32>(table + static_cast<u32>(i) * 12);
        Native n{m[2], {}, 0, strcmp(gptr<const char>(m[0]), "nativeRender") == 0};
        parse_sig(gptr<const char>(m[1]), n.args, n.ret);
        int slot;
        {
            std::lock_guard lk(g_native_lock);
            slot = static_cast<int>(g_natives.size());
            if (slot >= kTrampCount) bridge_fatal("out of native trampolines");
            g_natives.push_back(n);
        }
        ms.push_back({gptr<const char>(m[0]), gptr<const char>(m[1]), bridge_tramp_table + slot * kTrampSize});
    }
    if (e->RegisterNatives(cls, ms.data(), count) != JNI_OK) {
        BERR("RegisterNatives failed (%d methods)", count);
        if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); }
    }
}

// JNI descriptor of a Class, through Class.getName().
std::string class_descriptor(JNIEnv* e, jobject cls) {
    jclass cc = e->GetObjectClass(cls);
    jmethodID get_name = e->GetMethodID(cc, "getName", "()Ljava/lang/String;");
    auto js = static_cast<jstring>(e->CallObjectMethod(cls, get_name));
    const char* c = e->GetStringUTFChars(js, nullptr);
    std::string n = c;
    e->ReleaseStringUTFChars(js, c);
    e->DeleteLocalRef(js);
    e->DeleteLocalRef(cc);
    for (auto& ch : n) if (ch == '.') ch = '/';
    if (n[0] == '[') return n;
    static const std::map<std::string, std::string> prim = {
        {"int", "I"}, {"long", "J"}, {"float", "F"}, {"double", "D"}, {"boolean", "Z"},
        {"byte", "B"}, {"char", "C"}, {"short", "S"}, {"void", "V"}};
    auto it = prim.find(n);
    return it != prim.end() ? it->second : "L" + n + ";";
}

// Signature of a java.lang.reflect.Method or Constructor object.
std::string reflected_signature(JNIEnv* e, jobject m) {
    jclass mc = e->GetObjectClass(m);
    jmethodID params = e->GetMethodID(mc, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID ret = e->GetMethodID(mc, "getReturnType", "()Ljava/lang/Class;");
    if (!ret) e->ExceptionClear();  // constructors have no return type
    auto arr = static_cast<jobjectArray>(e->CallObjectMethod(m, params));
    std::string sig = "(";
    jsize n = e->GetArrayLength(arr);
    for (jsize i = 0; i < n; i++) {
        jobject c = e->GetObjectArrayElement(arr, i);
        sig += class_descriptor(e, c);
        e->DeleteLocalRef(c);
    }
    sig += ")";
    if (ret) {
        jobject r = e->CallObjectMethod(m, ret);
        sig += class_descriptor(e, r);
        e->DeleteLocalRef(r);
    } else {
        sig += "V";
    }
    e->DeleteLocalRef(arr);
    e->DeleteLocalRef(mc);
    return sig;
}
std::string reflected_field_signature(JNIEnv* e, jobject f) {
    jclass fc = e->GetObjectClass(f);
    jmethodID get_type = e->GetMethodID(fc, "getType", "()Ljava/lang/Class;");
    jobject t = e->CallObjectMethod(f, get_type);
    std::string sig = class_descriptor(e, t);
    e->DeleteLocalRef(t);
    e->DeleteLocalRef(fc);
    return sig;
}

template <int N> void jni_thunk();

void jni_dispatch(int n) {
    JNIEnv* e = env();
    if (n >= 34 && n <= 63) { int t = (n - 34) / 3; do_call(kVirtual, kTypes[t], (n - 34) % 3); return; }
    if (n >= 64 && n <= 93) { int t = (n - 64) / 3; do_call(kNonvirtual, kTypes[t], (n - 64) % 3); return; }
    if (n >= 114 && n <= 143) { int t = (n - 114) / 3; do_call(kStatic, kTypes[t], (n - 114) % 3); return; }
    if (n >= 95 && n <= 103) { field_get(false, kTypes[n - 95]); return; }
    if (n >= 104 && n <= 112) { field_set(false, kTypes[n - 104]); return; }
    if (n >= 145 && n <= 153) { field_get(true, kTypes[n - 145]); return; }
    if (n >= 154 && n <= 162) { field_set(true, kTypes[n - 154]); return; }
    if (n >= 175 && n <= 182) {
        jsize len = static_cast<jsize>(R(1));
        jobject a = nullptr;
        switch (n) {
            case 175: a = e->NewBooleanArray(len); break;
            case 176: a = e->NewByteArray(len); break;
            case 177: a = e->NewCharArray(len); break;
            case 178: a = e->NewShortArray(len); break;
            case 179: a = e->NewIntArray(len); break;
            case 180: a = e->NewLongArray(len); break;
            case 181: a = e->NewFloatArray(len); break;
            case 182: a = e->NewDoubleArray(len); break;
        }
        R(0) = wrap_local(a);
        return;
    }
    if (n >= 183 && n <= 190) { array_get_elements(n - 182); return; }
    if (n >= 191 && n <= 198) { array_release_elements(n - 190); return; }
    if (n >= 199 && n <= 206) { array_region(n - 198, false); return; }
    if (n >= 207 && n <= 214) { array_region(n - 206, true); return; }

    switch (n) {
        case 4: R(0) = JNI_VERSION_1_6; return;
        case 6: {
            const char* name = gptr<const char>(R(1));
            jclass c = e->FindClass(name);
            if (!c && g_app_loader) {
                e->ExceptionClear();
                std::string dotted = name;
                for (auto& ch : dotted) if (ch == '/') ch = '.';
                jstring js = e->NewStringUTF(dotted.c_str());
                c = static_cast<jclass>(e->CallObjectMethod(g_app_loader, g_load_class, js));
                e->DeleteLocalRef(js);
            }
            R(0) = wrap_local(c);
            return;
        }
        case 7: {
            jobject m = ref_get(R(1));
            R(0) = id_intern(e->FromReflectedMethod(m), reflected_signature(e, m).c_str());
            return;
        }
        case 8: {
            jobject f = ref_get(R(1));
            R(0) = id_intern(e->FromReflectedField(f), reflected_field_signature(e, f).c_str());
            return;
        }
        case 9: R(0) = wrap_local(e->ToReflectedMethod(static_cast<jclass>(ref_get(R(1))), static_cast<jmethodID>(id_get(R(2))->id), static_cast<jboolean>(R(3)))); return;
        case 12: R(0) = wrap_local(e->ToReflectedField(static_cast<jclass>(ref_get(R(1))), static_cast<jfieldID>(id_get(R(2))->id), static_cast<jboolean>(R(3)))); return;
        case 10: R(0) = wrap_local(e->GetSuperclass(static_cast<jclass>(ref_get(R(1))))); return;
        case 11: R(0) = e->IsAssignableFrom(static_cast<jclass>(ref_get(R(1))), static_cast<jclass>(ref_get(R(2)))); return;
        case 13: R(0) = e->Throw(static_cast<jthrowable>(ref_get(R(1)))); return;
        case 14: R(0) = e->ThrowNew(static_cast<jclass>(ref_get(R(1))), gptr<const char>(R(2))); return;
        case 15: R(0) = wrap_local(e->ExceptionOccurred()); return;
        case 16: e->ExceptionDescribe(); return;
        case 17: e->ExceptionClear(); return;
        case 18: bridge_fatal("guest JNI FatalError: %s", gptr<const char>(R(1)));
        case 19: tl_frames.push_back(tl_locals.size()); R(0) = 0; return;
        case 20: {
            jobject keep = ref_get(R(1));
            jobject lo = keep ? e->NewLocalRef(keep) : nullptr;
            if (!tl_frames.empty()) {
                pop_locals(tl_frames.back());
                tl_frames.pop_back();
            }
            R(0) = wrap_local(lo);
            return;
        }
        case 21: { jobject o = ref_get(R(1)); R(0) = o ? ref_add(e->NewGlobalRef(o)) : 0; return; }
        case 226: { jobject o = ref_get(R(1)); R(0) = o ? ref_add(e->NewGlobalRef(o)) : 0; return; }
        case 22: case 227: ref_del(R(1)); return;
        case 23: {
            u32 h = R(1);
            for (size_t i = tl_locals.size(); i-- > 0;) {
                if (tl_locals[i] == h) {
                    tl_locals.erase(tl_locals.begin() + static_cast<long>(i));
                    ref_del(h);
                    break;
                }
            }
            return;
        }
        case 24: R(0) = e->IsSameObject(ref_get(R(1)), ref_get(R(2))); return;
        case 25: { jobject o = ref_get(R(1)); R(0) = wrap_local(o ? e->NewLocalRef(o) : nullptr); return; }
        case 26: R(0) = 0; return;
        case 27: R(0) = wrap_local(e->AllocObject(static_cast<jclass>(ref_get(R(1))))); return;
        case 28: case 29: case 30: {
            IdRec* m = id_get(R(2));
            jvalue a[64];
            if (n == 28) { ArgReader rd; rd.reg = 3; read_args(rd, m, a); }
            else if (n == 29) { VaReader rd(R(3)); read_args(rd, m, a); }
            else read_args_a(R(3), m, a);
            jobject o;
            {
                BlockScope block;
                o = e->NewObjectA(static_cast<jclass>(ref_get(R(1))), static_cast<jmethodID>(m->id), a);
            }
            R(0) = wrap_local(o);
            return;
        }
        case 31: R(0) = wrap_local(e->GetObjectClass(ref_get(R(1)))); return;
        case 32: R(0) = e->IsInstanceOf(ref_get(R(1)), static_cast<jclass>(ref_get(R(2)))); return;
        case 33: R(0) = id_intern(e->GetMethodID(static_cast<jclass>(ref_get(R(1))), gptr<const char>(R(2)), gptr<const char>(R(3))), gptr<const char>(R(3))); return;
        case 113: R(0) = id_intern(e->GetStaticMethodID(static_cast<jclass>(ref_get(R(1))), gptr<const char>(R(2)), gptr<const char>(R(3))), gptr<const char>(R(3))); return;
        case 94: R(0) = id_intern(e->GetFieldID(static_cast<jclass>(ref_get(R(1))), gptr<const char>(R(2)), gptr<const char>(R(3))), gptr<const char>(R(3))); return;
        case 144: R(0) = id_intern(e->GetStaticFieldID(static_cast<jclass>(ref_get(R(1))), gptr<const char>(R(2)), gptr<const char>(R(3))), gptr<const char>(R(3))); return;
        case 163: R(0) = wrap_local(e->NewString(gptr<const jchar>(R(1)), static_cast<jsize>(R(2)))); return;
        case 164: R(0) = static_cast<u32>(e->GetStringLength(static_cast<jstring>(ref_get(R(1))))); return;
        case 165: case 224: {
            auto s = static_cast<jstring>(ref_get(R(1)));
            jsize len = e->GetStringLength(s);
            u32 p = gmalloc(static_cast<u32>(len + 1) * 2);
            e->GetStringRegion(s, 0, len, gptr<jchar>(p));
            gptr<jchar>(p)[len] = 0;
            if (R(2)) *gptr<u8>(R(2)) = 1;
            R(0) = p;
            return;
        }
        case 166: case 225: gfree(R(2)); return;
        case 167: R(0) = wrap_local(e->NewStringUTF(gptr<const char>(R(1)))); return;
        case 168: R(0) = static_cast<u32>(e->GetStringUTFLength(static_cast<jstring>(ref_get(R(1))))); return;
        case 169: {
            auto s = static_cast<jstring>(ref_get(R(1)));
            const char* c = e->GetStringUTFChars(s, nullptr);
            u32 p = gstrdup(c ? c : "");
            if (c) e->ReleaseStringUTFChars(s, c);
            if (R(2)) *gptr<u8>(R(2)) = 1;
            R(0) = p;
            return;
        }
        case 170: gfree(R(2)); return;
        case 171: R(0) = static_cast<u32>(e->GetArrayLength(static_cast<jarray>(ref_get(R(1))))); return;
        case 172: R(0) = wrap_local(e->NewObjectArray(static_cast<jsize>(R(1)), static_cast<jclass>(ref_get(R(2))), ref_get(R(3)))); return;
        case 173: R(0) = wrap_local(e->GetObjectArrayElement(static_cast<jobjectArray>(ref_get(R(1))), static_cast<jsize>(R(2)))); return;
        case 174: e->SetObjectArrayElement(static_cast<jobjectArray>(ref_get(R(1))), static_cast<jsize>(R(2)), ref_get(R(3))); return;
        case 215: register_natives(static_cast<jclass>(ref_get(R(1))), R(2), static_cast<int>(R(3))); R(0) = 0; return;
        case 216: R(0) = e->UnregisterNatives(static_cast<jclass>(ref_get(R(1)))); return;
        case 217: { BlockScope b; R(0) = e->MonitorEnter(ref_get(R(1))); return; }
        case 218: R(0) = e->MonitorExit(ref_get(R(1))); return;
        case 219: *gptr<u32>(R(1)) = g_guest_vm_ptr; R(0) = 0; return;
        case 220: e->GetStringRegion(static_cast<jstring>(ref_get(R(1))), static_cast<jsize>(R(2)), static_cast<jsize>(R(3)), gptr<jchar>(*gptr<u32>(guest_sp()))); return;
        case 221: e->GetStringUTFRegion(static_cast<jstring>(ref_get(R(1))), static_cast<jsize>(R(2)), static_cast<jsize>(R(3)), gptr<char>(*gptr<u32>(guest_sp()))); return;
        case 222: array_get_elements(array_type_of(ref_get(R(1)))); return;
        case 223: array_release_elements(array_type_of(ref_get(R(1)))); return;
        case 228: R(0) = e->ExceptionCheck(); return;
        case 229: R(0) = wrap_local(e->NewDirectByteBuffer(gptr(R(1)), static_cast<jlong>(R(2)) | (static_cast<jlong>(R(3)) << 32))); return;
        case 230: {
            jobject b = ref_get(R(1));
            auto* host = static_cast<u8*>(e->GetDirectBufferAddress(b));
            if (!host) { R(0) = 0; return; }
            if (!(reinterpret_cast<uintptr_t>(host) >> 32)) { R(0) = g32(host); return; }
            u32 cap = static_cast<u32>(e->GetDirectBufferCapacity(b));
            Shadow s;
            {
                std::lock_guard lk(g_shadow_lock);
                auto it = g_shadows.find(host);
                if (it == g_shadows.end() || it->second.cap < cap) {
                    s = {host, gmalloc(cap), cap};
                    g_shadows[host] = s;
                } else {
                    s = it->second;
                }
            }
            memcpy(gptr(s.guest), host, cap);
            tl_used_shadows.push_back(s);
            R(0) = s.guest;
            return;
        }
        case 231: write_ret<long long>(e->GetDirectBufferCapacity(ref_get(R(1)))); return;
        case 232: R(0) = e->GetObjectRefType(ref_get(R(1))); return;
        default:
            bridge_fatal("guest used unimplemented JNI function %d", n);
    }
}

template <int N>
void jni_thunk() { jni_dispatch(N); }
template <int... Ns>
constexpr std::array<ThunkFn, sizeof...(Ns)> make_thunks(std::integer_sequence<int, Ns...>) {
    return {{jni_thunk<Ns>...}};
}
const auto kJniThunks = make_thunks(std::make_integer_sequence<int, 233>{});

// ---- JavaVM
void vm_destroy() { R(0) = 0; }
void vm_attach() {
    env();
    if (R(1)) *gptr<u32>(R(1)) = g_guest_env_ptr;
    R(0) = 0;
}
void vm_detach() {
    if (tl_attached) {
        g_host_vm->DetachCurrentThread();
        tl_attached = false;
        tl_env = nullptr;
    }
    R(0) = 0;
}
void vm_getenv() {
    env();
    *gptr<u32>(R(1)) = g_guest_env_ptr;
    R(0) = JNI_OK;
}
}  // namespace

u32 guest_javavm() { return g_guest_vm_ptr; }

void jni_end_guest_thread() {
    if (tl_attached) {
        g_host_vm->DetachCurrentThread();
        tl_attached = false;
        tl_env = nullptr;
    }
}

void guest_env_init() {
    u32 table = gcalloc(233, 4);
    for (int i = 4; i < 233; i++) gptr<u32>(table)[i] = thunk_address("jni", kJniThunks[static_cast<size_t>(i)]);
    g_guest_env_ptr = gcalloc(1, 8);
    *gptr<u32>(g_guest_env_ptr) = table;

    u32 vmt = gcalloc(8, 4);
    gptr<u32>(vmt)[3] = thunk_address("vm_destroy", vm_destroy);
    gptr<u32>(vmt)[4] = thunk_address("vm_attach", vm_attach);
    gptr<u32>(vmt)[5] = thunk_address("vm_detach", vm_detach);
    gptr<u32>(vmt)[6] = thunk_address("vm_getenv", vm_getenv);
    gptr<u32>(vmt)[7] = thunk_address("vm_attach", vm_attach);
    g_guest_vm_ptr = gcalloc(1, 8);
    *gptr<u32>(g_guest_vm_ptr) = vmt;
}

void jni_bridge_init(JavaVM* vm) {
    g_host_vm = vm;
    JNIEnv* e = env();
    jclass player = e->FindClass("com/unity3d/player/UnityPlayer");
    if (player) {
        jclass cc = e->GetObjectClass(player);
        jmethodID gl = e->GetMethodID(cc, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jobject loader = e->CallObjectMethod(player, gl);
        g_app_loader = e->NewGlobalRef(loader);
        jclass lc = e->GetObjectClass(loader);
        g_load_class = e->GetMethodID(lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    } else {
        e->ExceptionClear();
    }
}

// Java_* natives exported by libunity (FMOD) are looked up by name in the host library,
// so they are registered explicitly.
void jni_register_exported(JNIEnv* e, GuestLib* lib) {
    static const struct { const char* jname; const char* sig; const char* sym; } fm[] = {
        {"fmodGetInfo", "(I)I", "Java_org_fmod_FMODAudioDevice_fmodGetInfo"},
        {"fmodProcess", "(Ljava/nio/ByteBuffer;)I", "Java_org_fmod_FMODAudioDevice_fmodProcess"},
        {"fmodBlockStreaming", "()I", "Java_org_fmod_FMODAudioDevice_fmodBlockStreaming"},
        {"fmodUnblockStreaming", "()I", "Java_org_fmod_FMODAudioDevice_fmodUnblockStreaming"},
        {"fmodInitJni", "()I", "Java_org_fmod_FMODAudioDevice_fmodInitJni"},
        {"fmodProcessMicData", "(Ljava/nio/ByteBuffer;I)I", "Java_org_fmod_FMODAudioDevice_fmodProcessMicData"},
    };
    jclass c = e->FindClass("org/fmod/FMODAudioDevice");
    if (!c) {
        e->ExceptionClear();
        BERR("org.fmod.FMODAudioDevice not found");
        return;
    }
    u32 table = gcalloc(6, 12);
    for (int i = 0; i < 6; i++) {
        u32* m = gptr<u32>(table + static_cast<u32>(i) * 12);
        m[0] = gstrdup(fm[i].jname);
        m[1] = gstrdup(fm[i].sig);
        m[2] = lib->lookup(fm[i].sym);
        if (!m[2]) bridge_fatal("guest export %s missing", fm[i].sym);
    }
    register_natives(c, table, 6);
    gfree(table);
}

// Entered from the assembly trampoline with the arm64 argument registers saved.
extern "C" u64 bridge_native_dispatch(int slot, u64* gpr, double* fpr, u64* stack) {
    Native n;
    {
        std::lock_guard lk(g_native_lock);
        n = g_natives[static_cast<size_t>(slot)];
    }
    tl_env = reinterpret_cast<JNIEnv*>(gpr[0]);
    size_t mark = tl_locals.size();
    size_t shadow_mark = tl_used_shadows.size();

    int gi = 2, fi = 0;
    auto next_g = [&]() -> u64 { return gi < 8 ? gpr[gi++] : *stack++; };
    auto next_f = [&]() -> u64 {
        if (fi < 8) { u64 v; memcpy(&v, &fpr[fi++], 8); return v; }
        return *stack++;
    };
    std::vector<u32> args;
    args.push_back(g_guest_env_ptr);
    // This object/class reference stays local; wrap_local must not delete the caller's ref.
    jobject self = reinterpret_cast<jobject>(gpr[1]);
    args.push_back(wrap_local(self ? tl_env->NewLocalRef(self) : nullptr));
    for (char t : n.args) {
        switch (t) {
            case 'J': {
                if (args.size() % 2) args.push_back(0);
                u64 v = next_g();
                args.push_back(static_cast<u32>(v));
                args.push_back(static_cast<u32>(v >> 32));
                break;
            }
            case 'D': {
                if (args.size() % 2) args.push_back(0);
                u64 v = next_f();
                args.push_back(static_cast<u32>(v));
                args.push_back(static_cast<u32>(v >> 32));
                break;
            }
            case 'F': args.push_back(static_cast<u32>(next_f())); break;
            case 'L': {
                auto o = reinterpret_cast<jobject>(next_g());
                args.push_back(wrap_local(o ? tl_env->NewLocalRef(o) : nullptr));
                break;
            }
            default: args.push_back(static_cast<u32>(next_g())); break;
        }
    }
    u32 r1 = 0;
    if (n.render) fx_begin_frame();
    u32 r0 = guest_call_vec(n.fn, args, &r1);
    if (n.render) fx_end_frame();
    flush_shadows(shadow_mark);
    drop_stale_exception(tl_env, "native returning to Java");

    u64 ret = 0;
    switch (n.ret) {
        case 'L': {
            jobject o = ref_get(r0);
            ret = reinterpret_cast<u64>(o ? tl_env->NewLocalRef(o) : nullptr);
            break;
        }
        case 'J':
        case 'D': ret = r0 | (u64(r1) << 32); break;
        case 'V': break;
        default: ret = r0; break;
    }
    pop_locals(mark);
    return ret;
}
