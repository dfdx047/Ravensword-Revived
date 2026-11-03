// Remaster pipeline. The game believes it draws to the window; while the 3D world is drawn, the
// window is replaced by a supersampled HDR target. When the managed mod reports that the world
// camera has finished, that image goes through ambient occlusion, screen-space sun shadows, bloom,
// light shafts and filmic tone mapping and lands in the real window, where the HUD is then drawn
// untouched at native resolution. All of this runs on the GPU: the guest only pays a few calls.
#include <EGL/egl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include "common.h"

namespace {
struct Params {
    bool enabled = true;
    int height = 0, msaa = 4, aniso = 2;  // height 0: the screen's own
    float bloom = 0.35f, ao = 1.f, shadow = 1.f, rays = 0.35f, tonemap = 1.f, saturation = 1.12f, sharpen = 0.3f, vignette = 0.f, exposure = 1.f, contrast = 0.15f, range = 150.f;
    float znear = 0.3f, zfar = 2000.f, px = 1.f, py = 1.7f;
    float lx = 0, ly = 0, lz = 0, light_on = 0, sunx = 0.5f, suny = 0.5f, sun_vis = 0;
} P;

struct Prog {
    GLuint id = 0;
    GLint u(const char* n) const { return glGetUniformLocation(id, n); }
};

const int kBloom = 5;
struct Ctx {
    EGLContext ctx = EGL_NO_CONTEXT;
    int w = 0, h = 0, sw = 0, sh = 0, samples = 0, wanted_samples = -1;
    bool hdr = true, ok = false, broken = false;
    GLuint vao = 0, fbo = 0;
    GLuint scene_fbo = 0, msaa_fbo = 0, msaa_color = 0, msaa_depth = 0, color = 0, depth = 0;
    GLuint down[kBloom]{}, up[kBloom]{};
    int bw[kBloom]{}, bh[kBloom]{};
    GLuint occ[2]{}, ray = 0;
    int ow = 0, oh = 0;
    Prog bright, down_p, up_p, occ_p, blur_p, ray_p, final_p;
} C;

// What the guest asked for, in its own (window) coordinates.
GLuint g_guest_fb = 0;
GLint g_vp[4] = {0, 0, 0, 0}, g_sc[4] = {0, 0, 0, 0};
float g_applied = 1.f;
int g_phase = 0;  // 0 outside a frame or disabled, 1 drawing the world, 2 drawing the HUD
bool g_marked_ok = false;

const char kVs[] = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

#define FS_HEAD "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\nin vec2 uv;\n"

const char kBright[] = FS_HEAD R"(
uniform sampler2D uTex;
out vec4 o;
void main() {
    vec3 c = texture(uTex, uv).rgb;
    // A single invalid pixel would grow into a dark square through the blur chain.
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    c = min(max(c, vec3(0.0)), vec3(4.0));
    float l = max(c.r, max(c.g, c.b));
    o = vec4(c * smoothstep(0.84, 1.15, l), 1.0);
}
)";

// 13 taps: keeps small highlights from flickering while moving.
const char kDown[] = FS_HEAD R"(
uniform sampler2D uTex;
uniform vec2 uPx;
out vec4 o;
void main() {
    vec2 p = uPx;
    vec3 a = texture(uTex, uv + p * vec2(-2, 2)).rgb + texture(uTex, uv + p * vec2(2, 2)).rgb +
             texture(uTex, uv + p * vec2(-2, -2)).rgb + texture(uTex, uv + p * vec2(2, -2)).rgb;
    vec3 b = texture(uTex, uv + p * vec2(-2, 0)).rgb + texture(uTex, uv + p * vec2(2, 0)).rgb +
             texture(uTex, uv + p * vec2(0, 2)).rgb + texture(uTex, uv + p * vec2(0, -2)).rgb;
    vec3 c = texture(uTex, uv + p * vec2(-1, 1)).rgb + texture(uTex, uv + p * vec2(1, 1)).rgb +
             texture(uTex, uv + p * vec2(-1, -1)).rgb + texture(uTex, uv + p * vec2(1, -1)).rgb;
    vec3 e = texture(uTex, uv).rgb;
    o = vec4(e * 0.125 + a * 0.03125 + b * 0.0625 + c * 0.125, 1.0);
}
)";

const char kUp[] = FS_HEAD R"(
uniform sampler2D uTex;
uniform sampler2D uAdd;
uniform vec2 uPx;
out vec4 o;
void main() {
    vec2 p = uPx;
    vec3 c = texture(uTex, uv).rgb * 4.0;
    c += (texture(uTex, uv + p * vec2(-1, 0)).rgb + texture(uTex, uv + p * vec2(1, 0)).rgb +
          texture(uTex, uv + p * vec2(0, 1)).rgb + texture(uTex, uv + p * vec2(0, -1)).rgb) * 2.0;
    c += texture(uTex, uv + p * vec2(-1, -1)).rgb + texture(uTex, uv + p * vec2(1, -1)).rgb +
         texture(uTex, uv + p * vec2(-1, 1)).rgb + texture(uTex, uv + p * vec2(1, 1)).rgb;
    o = vec4(c / 16.0 * 0.85 + texture(uAdd, uv).rgb, 1.0);
}
)";

#define DEPTH_FUNCS R"(
uniform sampler2D uDepth;
uniform vec2 uNF;
uniform vec2 uProj;
float lz(vec2 t) {
    float d = texture(uDepth, t).r * 2.0 - 1.0;
    return 2.0 * uNF.x * uNF.y / (uNF.y + uNF.x - d * (uNF.y - uNF.x));
}
vec3 vp(vec2 t, float z) { return vec3((t * 2.0 - 1.0) / uProj * z, z); }
)"

// Red: ambient occlusion. Green: sun visibility, by marching the depth buffer towards the sun.
const char kOcc[] = FS_HEAD DEPTH_FUNCS R"(
uniform vec2 uPx;
uniform vec3 uSun;
uniform float uHasSun;
uniform float uRange;
out vec4 o;
void main() {
    float z = lz(uv);
    if (z >= uNF.y * 0.98) { o = vec4(1.0); return; }
    vec3 P = vp(uv, z);
    vec3 r = vp(uv + vec2(uPx.x, 0.0), lz(uv + vec2(uPx.x, 0.0))) - P;
    vec3 l = P - vp(uv - vec2(uPx.x, 0.0), lz(uv - vec2(uPx.x, 0.0)));
    vec3 t = vp(uv + vec2(0.0, uPx.y), lz(uv + vec2(0.0, uPx.y))) - P;
    vec3 b = P - vp(uv - vec2(0.0, uPx.y), lz(uv - vec2(0.0, uPx.y)));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(t.z) < abs(b.z) ? t : b;
    vec3 N = normalize(cross(dy, dx));
    if (N.z > 0.0) N = -N;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    // Surfaces seen edge-on have unreliable normals: far ground would shade itself in bands.
    float facing = smoothstep(0.03, 0.14, abs(dot(N, normalize(P))));

    float R = mix(0.5, 2.0, clamp(z / 80.0, 0.0, 1.0));
    float sr = R * uProj.y / z * 0.5;
    float occ = 0.0;
    float aspect = uProj.y / uProj.x;
    for (int i = 0; i < 12; i++) {
        float a = (float(i) + noise) * 2.39996;
        float rr = sqrt((float(i) + 0.5) / 12.0) * sr;
        vec2 s = uv + vec2(cos(a) / aspect, sin(a)) * rr;
        vec3 v = vp(s, lz(s)) - P;
        float d2 = dot(v, v);
        float h = dot(v, N) / sqrt(d2 + 1e-5);
        occ += max(h - 0.1 - 0.001 * z, 0.0) * smoothstep(R * R * 2.25, R * R * 0.25, d2);
    }
    float ao = clamp(1.0 - occ / 12.0 * 2.6, 0.0, 1.0);
    ao = mix(1.0, ao, facing * (1.0 - smoothstep(uRange * 0.8, uRange * 1.6, z)));

    // Short rays only: they add contact shadows without contradicting the game's baked light.
    float sun = 1.0;
    if (uHasSun > 0.5 && z < uRange && dot(N, uSun) > 0.08) {
        float maxD = clamp(z * 0.12, 1.5, 10.0);
        vec3 start = P + N * (0.02 + 0.002 * z);
        for (int i = 0; i < 24; i++) {
            float f = (float(i) + noise) / 24.0;
            float td = f * f * maxD + 0.02;
            vec3 p = start + uSun * td;
            if (p.z <= uNF.x) break;
            vec2 q = p.xy / p.z * uProj * 0.5 + 0.5;
            if (q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0) break;
            float dz = p.z - lz(q);
            if (dz > 0.02 + 0.002 * p.z + 0.03 * td && dz < 0.5 + td * 0.35) {
                float edge = min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y));
                sun = 1.0 - smoothstep(0.0, 0.06, edge) * (1.0 - f * 0.55);
                break;
            }
        }
        sun = mix(1.0, sun, facing * (1.0 - smoothstep(uRange * 0.5, uRange, z)));
    }
    o = vec4(ao, sun, 0.0, 1.0);
}
)";

const char kBlur[] = FS_HEAD DEPTH_FUNCS R"(
uniform sampler2D uTex;
uniform vec2 uDir;
out vec4 o;
void main() {
    float z = lz(uv);
    vec4 sum = texture(uTex, uv) * 2.0;
    float w = 2.0;
    for (int i = -3; i <= 3; i++) {
        if (i == 0) continue;
        vec2 t = uv + uDir * float(i);
        float k = exp(-abs(lz(t) - z) * 6.0 / (0.05 * z + 0.15)) * (1.0 - abs(float(i)) * 0.2);
        sum += texture(uTex, t) * k;
        w += k;
    }
    o = sum / w;
}
)";

const char kRay[] = FS_HEAD DEPTH_FUNCS R"(
uniform sampler2D uTex;
uniform vec2 uSunUv;
out vec4 o;
void main() {
    vec2 d = (uSunUv - uv) / 28.0;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec2 t = uv + d * noise;
    float sum = 0.0, w = 1.0;
    for (int i = 0; i < 28; i++) {
        if (lz(t) >= uNF.y * 0.98) {
            float l = dot(min(texture(uTex, t).rgb, vec3(4.0)), vec3(0.3, 0.6, 0.1));
            sum += smoothstep(0.7, 1.25, l) * w;
        }
        w *= 0.96;
        t += d;
    }
    o = vec4(sum / 28.0);
}
)";

const char kFinal[] = FS_HEAD DEPTH_FUNCS R"(
uniform vec2 uOccPx;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uOcc;
uniform sampler2D uRay;
uniform vec2 uOut;
uniform float uBloomK, uAoK, uSunK, uRayK, uTone, uSat, uSharp, uVig, uWorld, uExpo, uContrast;
out vec4 o;
// The half-resolution result is picked per pixel by depth, so it never bleeds across silhouettes.
vec2 occlusion() {
    vec2 p = uv / uOccPx - 0.5;
    vec2 base = floor(p), fr = p - base;
    float z0 = lz(uv);
    vec2 sum = vec2(0.0);
    float w = 0.0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            vec2 t = (base + vec2(float(i), float(j)) + 0.5) * uOccPx;
            vec2 b = vec2(i == 0 ? 1.0 - fr.x : fr.x, j == 0 ? 1.0 - fr.y : fr.y);
            float k = (b.x * b.y + 0.001) / (1.0 + abs(lz(t) - z0) * 20.0 / (0.02 * z0 + 0.05));
            sum += texture(uOcc, t).rg * k;
            w += k;
        }
    }
    return sum / w;
}
// The game's art is already made for the screen, so a film curve meant for scene light would only
// lift its middle tones and dull its whites. This keeps the middle where it is: only what goes past
// white rolls off, the blacks settle, and shadows lean cool against warm lights.
vec3 grade(vec3 c) {
    vec3 over = max(c - 0.82, vec3(0.0));
    c = min(c, vec3(0.82)) + 0.18 * (1.0 - exp(-over / 0.18));
    c = max(c - 0.012, vec3(0.0)) / 0.988;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return c * mix(vec3(0.965, 1.0, 1.045), vec3(1.03, 1.0, 0.955), smoothstep(0.12, 0.72, l));
}
// Contrast around a pivot: darks go down and lights go up, the pivot itself does not move.
vec3 curve(vec3 c, float g) {
    const float p = 0.42;
    vec3 x = clamp(c, 0.0, 1.0);
    vec3 lo = p * pow(x / p, vec3(g));
    vec3 hi = 1.0 - (1.0 - p) * pow((1.0 - x) / (1.0 - p), vec3(g));
    return mix(lo, hi, step(p, x));
}
void main() {
    vec3 c = texture(uScene, uv).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    c = min(max(c, vec3(0.0)), vec3(32.0));
    if (uWorld > 0.5) {
        if (uSharp > 0.0) {
            vec3 n = texture(uScene, uv + vec2(0.0, uOut.y)).rgb, s = texture(uScene, uv - vec2(0.0, uOut.y)).rgb;
            vec3 e = texture(uScene, uv + vec2(uOut.x, 0.0)).rgb, w = texture(uScene, uv - vec2(uOut.x, 0.0)).rgb;
            vec3 mn = min(c, min(n, min(s, min(e, w)))), mx = max(c, max(n, max(s, max(e, w))));
            vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(0.001)), 0.0, 1.0));
            vec3 k = -amp * mix(0.125, 0.2, uSharp);
            c = clamp((c + (n + s + e + w) * k) / (1.0 + 4.0 * k), mn * 0.8, mx * 1.2);
        }
        vec2 oc = uAoK > 0.0 || uSunK > 0.0 ? occlusion() : vec2(1.0);
        float ao = mix(1.0, oc.r, uAoK);
        float sun = mix(1.0, oc.g, uSunK * 0.62);
        // Shadows keep a cool tint from the sky instead of going to plain grey.
        vec3 shade = mix(vec3(0.86, 0.93, 1.10), vec3(1.0), sun);
        c *= ao * sun * shade;
        c += texture(uBloom, uv).rgb * uBloomK;
        c += texture(uRay, uv).r * uRayK * vec3(1.0, 0.86, 0.62);
        c *= uExpo;
        c = mix(min(c, vec3(1.0)), grade(c), uTone);
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        float vib = uSat + (uSat - 1.0) * (1.0 - clamp(length(c - l) * 2.0, 0.0, 1.0));
        c = clamp(mix(vec3(l), c, vib), 0.0, 1.0);
        c = curve(c, 1.0 + uContrast * 1.2);
        vec2 d = uv - 0.5;
        c *= 1.0 - uVig * pow(dot(d, d) * 2.0, 1.5);
    } else {
        c = min(c, vec3(1.0));
    }
    // Keeps 8-bit gradients in skies and fog free of bands.
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c += (n - 0.5) / 255.0;
    o = vec4(c, 1.0);
}
)";

Prog make_prog(const char* name, const char* fs) {
    auto comp = [&](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = "";
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            BERR("fx shader %s: %s", name, log);
            C.broken = true;
        }
        return s;
    };
    Prog p;
    GLuint v = comp(GL_VERTEX_SHADER, kVs), f = comp(GL_FRAGMENT_SHADER, fs);
    p.id = glCreateProgram();
    glAttachShader(p.id, v);
    glAttachShader(p.id, f);
    glLinkProgram(p.id);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
        BERR("fx program %s: %s", name, log);
        C.broken = true;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

GLuint make_tex(GLenum fmt, int w, int h, GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void free_targets() {
    GLuint texs[] = {C.color, C.depth, C.occ[0], C.occ[1], C.ray};
    glDeleteTextures(5, texs);
    glDeleteTextures(kBloom, C.down);
    glDeleteTextures(kBloom, C.up);
    glDeleteFramebuffers(1, &C.scene_fbo);
    glDeleteFramebuffers(1, &C.msaa_fbo);
    glDeleteRenderbuffers(1, &C.msaa_color);
    glDeleteRenderbuffers(1, &C.msaa_depth);
    C.color = C.depth = C.scene_fbo = C.msaa_fbo = C.msaa_color = C.msaa_depth = 0;
    C.ok = false;
}

bool make_targets(int w, int h, int sw, int sh, int samples) {
    GLint old_tex = 0, old_rb = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    C.w = w; C.h = h; C.sw = sw; C.sh = sh;
    C.wanted_samples = samples;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    if (samples > max_samples) samples = max_samples;
    C.samples = samples;
    GLenum color_fmt = C.hdr ? GL_RGBA16F : GL_RGBA8;
    C.color = make_tex(color_fmt, sw, sh, GL_LINEAR);
    C.depth = make_tex(GL_DEPTH24_STENCIL8, sw, sh, GL_NEAREST);
    glGenFramebuffers(1, &C.scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, C.scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, C.depth, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok && samples > 1) {
        glGenRenderbuffers(1, &C.msaa_color);
        glBindRenderbuffer(GL_RENDERBUFFER, C.msaa_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, color_fmt, sw, sh);
        glGenRenderbuffers(1, &C.msaa_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, C.msaa_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, sw, sh);
        glGenFramebuffers(1, &C.msaa_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, C.msaa_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, C.msaa_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, C.msaa_depth);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    int bw = w / 2, bh = h / 2;
    for (int i = 0; i < kBloom; i++) {
        C.bw[i] = bw > 1 ? bw : 1;
        C.bh[i] = bh > 1 ? bh : 1;
        C.down[i] = make_tex(GL_RGBA16F, C.bw[i], C.bh[i], GL_LINEAR);
        C.up[i] = make_tex(GL_RGBA16F, C.bw[i], C.bh[i], GL_LINEAR);
        bw /= 2;
        bh /= 2;
    }
    C.ow = w / 2 + (w & 1);
    C.oh = h / 2 + (h & 1);
    C.occ[0] = make_tex(GL_RG8, C.ow, C.oh, GL_LINEAR);
    C.occ[1] = make_tex(GL_RG8, C.ow, C.oh, GL_LINEAR);
    C.ray = make_tex(GL_R8, w / 4, h / 4, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_rb));
    BLOG("fx targets: window %dx%d, scene %dx%d %s, MSAA %d, complete=%d", w, h, sw, sh, C.hdr ? "RGBA16F" : "RGBA8", samples, ok);
    return ok;
}

GLuint scene_target() { return C.msaa_fbo ? C.msaa_fbo : C.scene_fbo; }
float cur_scale() { return C.ok ? static_cast<float>(C.sw) / static_cast<float>(C.w) : 1.f; }
bool on_scene() { return g_phase == 1 && g_guest_fb == 0; }

int scaled(int v, float s) { return static_cast<int>(std::lround(static_cast<double>(v) * s)); }
void apply_rects() {
    float s = on_scene() ? cur_scale() : 1.f;
    glViewport(scaled(g_vp[0], s), scaled(g_vp[1], s), scaled(g_vp[2], s), scaled(g_vp[3], s));
    glScissor(scaled(g_sc[0], s), scaled(g_sc[1], s), scaled(g_sc[2], s), scaled(g_sc[3], s));
    g_applied = s;
}
void bind_guest_default() {
    glBindFramebuffer(GL_FRAMEBUFFER, on_scene() ? scene_target() : 0);
    apply_rects();
}

void ensure_context() {
    EGLContext now = eglGetCurrentContext();
    if (now == C.ctx) return;
    C = Ctx{};  // a new context has none of the previous objects
    C.ctx = now;
    if (now == EGL_NO_CONTEXT) return;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    C.hdr = ext && (strstr(ext, "GL_EXT_color_buffer_half_float") || strstr(ext, "GL_EXT_color_buffer_float"));
    GLint old_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    C.bright = make_prog("bright", kBright);
    C.down_p = make_prog("down", kDown);
    C.up_p = make_prog("up", kUp);
    C.occ_p = make_prog("occlusion", kOcc);
    C.blur_p = make_prog("blur", kBlur);
    C.ray_p = make_prog("rays", kRay);
    C.final_p = make_prog("final", kFinal);
    glGenVertexArrays(1, &C.vao);
    glGenFramebuffers(1, &C.fbo);
    glUseProgram(static_cast<GLuint>(old_prog));
}

struct SavedState {
    GLint prog, active, array_buffer, tex[5], cm[4], depth_mask, stencil_mask;
    GLboolean blend, depth, cull, scissor, stencil, dither, a2c;
    void save() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        for (int i = 0; i < 5; i++) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
        }
        glGetIntegerv(GL_COLOR_WRITEMASK, cm);
        glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        dither = glIsEnabled(GL_DITHER);
        a2c = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
    static void set(GLenum cap, GLboolean v) { if (v) glEnable(cap); else glDisable(cap); }
    void restore() {
        glUseProgram(static_cast<GLuint>(prog));
        for (int i = 0; i < 5; i++) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex[i]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        glBindVertexArray(0);
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        glDepthMask(static_cast<GLboolean>(depth_mask));
        glStencilMask(static_cast<GLuint>(stencil_mask));
        set(GL_BLEND, blend);
        set(GL_DEPTH_TEST, depth);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_STENCIL_TEST, stencil);
        set(GL_DITHER, dither);
        set(GL_SAMPLE_ALPHA_TO_COVERAGE, a2c);
    }
};

void tex_unit(int unit, GLuint t) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, t);
}
void target(GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, C.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
}
void draw() { glDrawArrays(GL_TRIANGLES, 0, 3); }

void depth_uniforms(const Prog& p) {
    glUniform1i(p.u("uDepth"), 1);
    glUniform2f(p.u("uNF"), P.znear, P.zfar);
    glUniform2f(p.u("uProj"), P.px, P.py);
}

void post(bool world) {
    SavedState st;
    st.save();
    glBindVertexArray(C.vao);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (C.msaa_fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, C.msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C.scene_fbo);
        glBlitFramebuffer(0, 0, C.sw, C.sh, 0, 0, C.sw, C.sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitFramebuffer(0, 0, C.sw, C.sh, 0, 0, C.sw, C.sh, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }

    tex_unit(1, C.depth);
    GLuint bloom_tex = C.up[0];
    bool use_bloom = world && P.bloom > 0.f;
    if (use_bloom) {
        glUseProgram(C.bright.id);
        glUniform1i(C.bright.u("uTex"), 0);
        tex_unit(0, C.color);
        target(C.down[0], C.bw[0], C.bh[0]);
        draw();
        glUseProgram(C.down_p.id);
        glUniform1i(C.down_p.u("uTex"), 0);
        GLint px = C.down_p.u("uPx");
        for (int i = 1; i < kBloom; i++) {
            tex_unit(0, C.down[i - 1]);
            glUniform2f(px, 1.f / C.bw[i - 1], 1.f / C.bh[i - 1]);
            target(C.down[i], C.bw[i], C.bh[i]);
            draw();
        }
        glUseProgram(C.up_p.id);
        glUniform1i(C.up_p.u("uTex"), 0);
        glUniform1i(C.up_p.u("uAdd"), 2);
        px = C.up_p.u("uPx");
        GLuint prev = C.down[kBloom - 1];
        for (int i = kBloom - 2; i >= 0; i--) {
            tex_unit(0, prev);
            tex_unit(2, C.down[i]);
            glUniform2f(px, 1.f / C.bw[i + 1], 1.f / C.bh[i + 1]);
            target(C.up[i], C.bw[i], C.bh[i]);
            draw();
            prev = C.up[i];
        }
    }

    bool have_cam = world && P.zfar > P.znear && P.znear > 0.f;
    bool has_sun = have_cam && P.light_on > 0.5f && P.shadow > 0.f;
    bool use_occ = have_cam && (P.ao > 0.f || has_sun);
    if (use_occ) {
        glUseProgram(C.occ_p.id);
        depth_uniforms(C.occ_p);
        glUniform2f(C.occ_p.u("uPx"), 1.f / C.ow, 1.f / C.oh);
        glUniform3f(C.occ_p.u("uSun"), P.lx, P.ly, P.lz);
        glUniform1f(C.occ_p.u("uHasSun"), has_sun ? 1.f : 0.f);
        glUniform1f(C.occ_p.u("uRange"), P.range);
        target(C.occ[0], C.ow, C.oh);
        draw();
        glUseProgram(C.blur_p.id);
        depth_uniforms(C.blur_p);
        glUniform1i(C.blur_p.u("uTex"), 0);
        GLint dir = C.blur_p.u("uDir");
        tex_unit(0, C.occ[0]);
        glUniform2f(dir, 1.f / C.ow, 0.f);
        target(C.occ[1], C.ow, C.oh);
        draw();
        tex_unit(0, C.occ[1]);
        glUniform2f(dir, 0.f, 1.f / C.oh);
        target(C.occ[0], C.ow, C.oh);
        draw();
    }
    bool use_rays = have_cam && P.rays > 0.f && P.sun_vis > 0.01f;
    if (use_rays) {
        glUseProgram(C.ray_p.id);
        depth_uniforms(C.ray_p);
        glUniform1i(C.ray_p.u("uTex"), 0);
        tex_unit(0, C.color);
        glUniform2f(C.ray_p.u("uSunUv"), P.sunx, P.suny);
        target(C.ray, C.w / 4, C.h / 4);
        draw();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, C.w, C.h);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    const Prog& f = C.final_p;
    glUseProgram(f.id);
    depth_uniforms(f);
    glUniform2f(f.u("uOccPx"), 1.f / C.ow, 1.f / C.oh);
    glUniform1i(f.u("uScene"), 0);
    glUniform1i(f.u("uBloom"), 2);
    glUniform1i(f.u("uOcc"), 3);
    glUniform1i(f.u("uRay"), 4);
    tex_unit(0, C.color);
    tex_unit(2, use_bloom ? bloom_tex : C.color);
    tex_unit(3, use_occ ? C.occ[0] : C.color);
    tex_unit(4, use_rays ? C.ray : C.color);
    glUniform2f(f.u("uOut"), 1.f / C.w, 1.f / C.h);
    glUniform1f(f.u("uBloomK"), use_bloom ? P.bloom : 0.f);
    glUniform1f(f.u("uAoK"), use_occ ? P.ao : 0.f);
    glUniform1f(f.u("uSunK"), use_occ && has_sun ? P.shadow : 0.f);
    glUniform1f(f.u("uRayK"), use_rays ? P.rays * P.sun_vis : 0.f);
    glUniform1f(f.u("uTone"), P.tonemap);
    glUniform1f(f.u("uSat"), P.saturation);
    glUniform1f(f.u("uSharp"), P.sharpen);
    glUniform1f(f.u("uVig"), P.vignette);
    glUniform1f(f.u("uExpo"), P.exposure);
    glUniform1f(f.u("uContrast"), P.contrast);
    glUniform1f(f.u("uWorld"), world ? 1.f : 0.f);
    draw();
    st.restore();

    // The next frame redraws these, so a tiler need not keep them in memory.
    static const GLenum all[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT};
    if (C.msaa_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, C.msaa_fbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, all);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, C.scene_fbo);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, all);
}

// ---- thunks
void t_glBindFramebuffer() {
    u32 fb = R(1);
    g_guest_fb = fb;
    if (fb == 0 && g_phase != 0) {
        bind_guest_default();
    } else {
        glBindFramebuffer(R(0), fb);
        if (g_applied != 1.f) apply_rects();
    }
}
void t_glViewport() {
    g_vp[0] = static_cast<GLint>(R(0)); g_vp[1] = static_cast<GLint>(R(1));
    g_vp[2] = static_cast<GLint>(R(2)); g_vp[3] = static_cast<GLint>(R(3));
    float s = on_scene() ? cur_scale() : 1.f;
    glViewport(scaled(g_vp[0], s), scaled(g_vp[1], s), scaled(g_vp[2], s), scaled(g_vp[3], s));
}
void t_glScissor() {
    g_sc[0] = static_cast<GLint>(R(0)); g_sc[1] = static_cast<GLint>(R(1));
    g_sc[2] = static_cast<GLint>(R(2)); g_sc[3] = static_cast<GLint>(R(3));
    float s = on_scene() ? cur_scale() : 1.f;
    glScissor(scaled(g_sc[0], s), scaled(g_sc[1], s), scaled(g_sc[2], s), scaled(g_sc[3], s));
}
void t_glGetIntegerv() {
    GLenum name = R(0);
    GLint* out = gptr<GLint>(R(1));
    if (name == GL_FRAMEBUFFER_BINDING) { *out = static_cast<GLint>(g_guest_fb); return; }
    if (name == GL_VIEWPORT && g_phase != 0) { memcpy(out, g_vp, sizeof g_vp); return; }
    if (name == GL_SCISSOR_BOX && g_phase != 0) { memcpy(out, g_sc, sizeof g_sc); return; }
    glGetIntegerv(name, out);
}
void after_filter(GLenum tex_target, GLenum pname, GLint value) {
    if (pname != GL_TEXTURE_MIN_FILTER || P.aniso < 2) return;
    if (value == GL_LINEAR_MIPMAP_LINEAR || value == GL_LINEAR_MIPMAP_NEAREST || value == GL_NEAREST_MIPMAP_LINEAR) {
        glTexParameterf(tex_target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.f);
    }
}
void t_glTexParameteri() {
    GLenum t = R(0), p = R(1);
    GLint v = static_cast<GLint>(R(2));
    glTexParameteri(t, p, v);
    after_filter(t, p, v);
}
void t_glTexParameterf() {
    GLenum t = R(0), p = R(1);
    u32 bits = R(2);
    float f;
    memcpy(&f, &bits, 4);
    glTexParameterf(t, p, f);
    after_filter(t, p, static_cast<GLint>(f));
}
// The world target is float and bigger than the window: grabs of it read the window's area.
void t_glCopyTexSubImage2D() {
    GLenum tgt = R(0);
    GLint level = static_cast<GLint>(R(1)), xo = static_cast<GLint>(R(2)), yo = static_cast<GLint>(R(3));
    u32 sp = guest_sp();
    GLint x = *gptr<GLint>(sp), y = *gptr<GLint>(sp + 4), w = *gptr<GLint>(sp + 8), h = *gptr<GLint>(sp + 12);
    static int warned = 0;
    if (on_scene() && !warned) {
        warned = 1;
        BLOG("guest copies from the world target (%dx%d)", w, h);
    }
    glCopyTexSubImage2D(tgt, level, xo, yo, x, y, w, h);
}

// A thread that sleeps between frames looks idle, and the system clocks its core down until frames
// no longer fit: this tells it the target and how long each frame's work really took.
void report_frame(long long work, long long period) {
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using Duration = int (*)(void*, int64_t);
    static void* lib = dlopen("libandroid.so", RTLD_NOW);
    static auto get_manager = lib ? reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager")) : nullptr;
    static auto create = lib ? reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession")) : nullptr;
    static auto report = lib ? reinterpret_cast<Duration>(dlsym(lib, "APerformanceHint_reportActualWorkDuration")) : nullptr;
    static auto update = lib ? reinterpret_cast<Duration>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration")) : nullptr;
    static void* session = nullptr;
    static long long target = 0;
    if (!target && get_manager && create && report && update) {
        void* manager = get_manager();
        int32_t tid = gettid();
        session = manager ? create(manager, &tid, 1, period) : nullptr;
        target = period;
        BLOG("performance hint: %s", session ? "on" : "unavailable");
    }
    if (!session) return;
    if (period != target) {
        update(session, period);
        target = period;
    }
    if (work > 0) report(session, work);
}

void t_fx_set() {
    int k = static_cast<int>(R(0));
    int raw = static_cast<int>(R(1));
    float v = static_cast<float>(raw) / 10000.f;
    switch (k) {
        case 0: P.enabled = raw != 0; break;
        case 1: P.height = raw; break;
        case 2: P.msaa = raw; break;
        case 3: P.bloom = v; break;
        case 4: P.ao = v; break;
        case 5: P.shadow = v; break;
        case 6: P.rays = v; break;
        case 7: P.tonemap = v; break;
        case 8: P.saturation = v; break;
        case 9: P.sharpen = v; break;
        case 10: P.vignette = v; break;
        case 11: P.aniso = raw; break;
        case 12: P.exposure = v; break;
        case 13: P.contrast = v; break;
        case 14: P.range = v; break;
        case 20: P.znear = v; break;
        case 21: P.zfar = v; break;
        case 22: P.px = v; break;
        case 23: P.py = v; break;
        case 24: P.lx = v; break;
        case 25: P.ly = v; break;
        case 26: P.lz = v; break;
        case 27: P.light_on = v; break;
        case 28: P.sunx = v; break;
        case 29: P.suny = v; break;
        case 30: P.sun_vis = v; break;
        case 40: {
            // Frames on a fixed timeline instead of "some time after the last one": a slow frame is
            // made up by starting the next at once, and each asks for its moment on screen, so a
            // frame a hair late or early still lands on the same refresh of the display.
            using Present = EGLBoolean (*)(EGLDisplay, EGLSurface, long long);
            static Present present = reinterpret_cast<Present>(eglGetProcAddress("eglPresentationTimeANDROID"));
            static long long start = 0, woke = 0;
            if (raw <= 0) break;
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec, period = 1000000000LL / raw;
            report_frame(woke ? now - woke : 0, period);
            start += period;
            if (start < now - period * 3 || start > now + period) start = now;  // a hitch is not made up
            if (start > now) {
                BlockScope b;
                timespec wait{0, static_cast<long>(start - now)};
                nanosleep(&wait, nullptr);
                clock_gettime(CLOCK_MONOTONIC, &ts);
                now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
            }
            woke = now;
            EGLDisplay dpy = eglGetCurrentDisplay();
            EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
            if (present && dpy != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) present(dpy, surface, start + period);
            break;
        }
        default: break;
    }
}

// The world camera has finished: everything drawn from here on is HUD.
void t_fx_mark() {
    if (g_phase != 1 || g_guest_fb != 0 || !C.ok) return;
    post(true);
    g_phase = 2;
    bind_guest_default();
}

const Hle kFxTable[] = {
    {"glBindFramebuffer", t_glBindFramebuffer}, {"glBindFramebufferOES", t_glBindFramebuffer},
    {"glViewport", t_glViewport}, {"glScissor", t_glScissor}, {"glGetIntegerv", t_glGetIntegerv},
    {"glTexParameteri", t_glTexParameteri}, {"glTexParameterf", t_glTexParameterf},
    {"glCopyTexSubImage2D", t_glCopyTexSubImage2D},
    {"raven_fx_set", t_fx_set}, {"raven_fx_mark", t_fx_mark},
};
}  // namespace

void fx_register() { hle_register(kFxTable, sizeof kFxTable / sizeof kFxTable[0]); }

// Called with the render thread's context current, around each nativeRender.
void fx_begin_frame() {
    ensure_context();
    if (!P.enabled || C.broken || C.ctx == EGL_NO_CONTEXT) {
        if (C.ok && !C.broken) free_targets();
        g_phase = 0;
        return;
    }
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) { g_phase = 0; return; }
    int sh = P.height > 0 ? P.height : h;
    int sw = ((w * sh / h) + 1) & ~1;
    if (!C.ok || C.w != w || C.h != h || C.sw != sw || C.sh != sh || C.wanted_samples != P.msaa) {
        if (C.scene_fbo) free_targets();
        C.ok = make_targets(w, h, sw, sh, P.msaa);
        if (!C.ok && C.hdr) {
            free_targets();
            C.hdr = false;
            C.ok = make_targets(w, h, sw, sh, P.msaa);
        }
        if (!C.ok) {
            free_targets();
            C.broken = true;
            BERR("fx disabled: the render targets could not be created");
            g_phase = 0;
            return;
        }
        if (g_vp[2] == 0) { g_vp[2] = w; g_vp[3] = h; g_sc[2] = w; g_sc[3] = h; }
    }
    g_phase = 1;
    if (g_guest_fb == 0) {
        bind_guest_default();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, g_guest_fb);
    }
}

void fx_end_frame() {
    if (g_phase == 0) return;
    if (g_phase == 1 && C.ok) post(false);  // no world camera this frame (loading screens)
    g_phase = 0;
    if (g_guest_fb == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        apply_rects();
    }
}
