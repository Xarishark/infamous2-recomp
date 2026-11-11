/*
 * texpack - texture pack authoring for the ps3recomp texture dump / packs
 * (libs/video/rsx_texpack*.h). C, Windows, WIC only: no third-party code.
 *
 *   texpack list     <dump> [--fmt F] [--min N] [--class C] [--csv out.csv]
 *   texpack upscale  <dump> <pack> [--scale 1|2|4] [--method lanczos|external]
 *                    [--exe <realesrgan-ncnn-vulkan.exe> --model <name>]
 *                    [--sharpen 0..1] [--only h1,h2,..] [--fmt F] [--min N]
 *                    [--class C] [--limit N] [--tint r,g,b]
 *   texpack compress <pack> [--bc auto|bc1|bc2|bc3] [--mips gamma|linear]
 *   texpack tint     <in.png> <out.png> r,g,b     (multiply: a visible test)
 *   texpack selftest                               (synthetic checks of the upscaler)
 *
 * upscale --scale 1 copies the dump's texels unchanged (a PNG identity pack)
 * or, with --tint, multiplies the colour textures (shows a pack's coverage).
 *
 * <dump> is gamedata\textures\dump\<title> (the TEXDUMP=1 output with its
 * index.jsonl); <pack> is gamedata\textures\packs\<title>\<pack name>. The
 * file names stay the dump's: the name is the texture's identity.
 *
 * What a texture is, is guessed from how the game samples it (the index's
 * usage records): gamma bits on its colour channels -> colour (resampled in
 * linear light); the unsigned remap BIASED on them -> a normal map (vectors
 * resampled and renormalised); alpha test -> a cutout (alpha keeps its
 * coverage at the alpha reference); NEAREST filtering, 16-bit / float formats
 * or a side of 8 or less -> data, never upscaled; the address modes say which
 * edges wrap (the resampler reads across a WRAP edge, so a tiling texture
 * stays seamless).
 *
 * Built-in upscaler: DXT seam deblocking (DXT sources), separable Lanczos-3
 * in linear light with anti-ringing (wrap / mirror / clamp aware), then an
 * edge-aware unsharp mask in gamma space sized to the scale and clamped to the
 * local source range (no halos). --sharpen 0..1 (0.6), --deblock 0|1 (1).
 * External: Real-ESRGAN ncnn-vulkan (user-installed, BSD-3): the colour goes
 * through it with a wrap-aware border that is cropped afterwards; alpha always
 * goes through our own resampler; normal maps and data never go to it.
 *
 * Build: tools\texpack\build.cmd (writes build\texpack.exe).
 */
/* intermediate images (a 4x network output of a 2048 texture plus its border) are
 * larger than the runtime's caps on pack files */
#define TP_MAX_FILE_BYTES (2048ull << 20)
#define TP_MAX_REP_DIM    16384u
#define TP_MAX_REP_BYTES  (1536ull << 20)
#include "rsx_texpack_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * The dump index
 * -----------------------------------------------------------------------*/
typedef struct {
    uint64_t hash, h0;
    uint32_t w, h, fmt, levels; int ln, cube;
    char file[260], fmtname[32];
    char run[48], ctx[128]; long long frame; double sec;
    /* usage, merged over every record */
    int uses, gamma, expand, sgn, txl, at, at_ref, nearest, wrap_s, wrap_t, mirror_s, mirror_t;
    uint32_t ctrl1;   /* the first crossbar seen */
    char line[1024];  /* the texture record as written by the dump */
} tx_rec;

static tx_rec* g_rec; static int g_nrec, g_caprec;
static uint64_t* g_key; static int* g_val; static uint32_t g_mcap;

static int tx_find(uint64_t h)
{
    if (!g_mcap) return -1;
    for (uint32_t i = (uint32_t)tp_hset_mix(h ? h : 1) & (g_mcap - 1); g_key[i]; i = (i + 1) & (g_mcap - 1))
        if (g_key[i] == (h ? h : 1)) return g_val[i];
    return -1;
}
static void tx_map_add(uint64_t h, int v)
{
    if ((uint32_t)(g_nrec + 1) * 2u >= g_mcap) {
        uint32_t nc = g_mcap ? g_mcap * 2 : 4096;
        uint64_t* nk = (uint64_t*)calloc(nc, 8); int* nv = (int*)calloc(nc, sizeof(int));
        for (uint32_t j = 0; j < g_mcap; j++) if (g_key[j])
            for (uint32_t i = (uint32_t)tp_hset_mix(g_key[j]) & (nc - 1); ; i = (i + 1) & (nc - 1))
                if (!nk[i]) { nk[i] = g_key[j]; nv[i] = g_val[j]; break; }
        free(g_key); free(g_val); g_key = nk; g_val = nv; g_mcap = nc;
    }
    if (!h) h = 1;
    for (uint32_t i = (uint32_t)tp_hset_mix(h) & (g_mcap - 1); ; i = (i + 1) & (g_mcap - 1))
        if (!g_key[i]) { g_key[i] = h; g_val[i] = v; return; }
}

static int load_index(const char* dump)
{
    char p[1024]; snprintf(p, sizeof p, "%s\\index.jsonl", dump);
    wchar_t wp[1024]; tp_widen(p, wp, 1024);
    size_t n = 0; const char* err = NULL;
    uint8_t* d = tp_read_file(wp, 512ull << 20, &n, &err);
    if (!d) { fprintf(stderr, "cannot read %s: %s\n", p, err); return -1; }
    char line[4096];
    /* two passes: the dump writes a texture's usage records before the texture
     * itself (a texture is written once it has settled), so records, then usages */
    for (int pass = 0; pass < 2; pass++)
    for (size_t at = 0; at < n; ) {
        size_t e = at; while (e < n && d[e] != '\n') e++;
        if (e < n && e - at < sizeof line) {
            memcpy(line, d + at, e - at); line[e - at] = 0;
            uint64_t h;
            if (pass == 0 && !tp_json_hex64(line, "hash", &h)) {
                if (tx_find(h) < 0) {
                    tx_rec r; memset(&r, 0, sizeof r); long long v;
                    r.hash = h; tp_json_hex64(line, "h0", &r.h0);
                    if (!tp_json_int(line, "w", &v)) r.w = (uint32_t)v;
                    if (!tp_json_int(line, "h", &v)) r.h = (uint32_t)v;
                    if (!tp_json_int(line, "fmtb", &v)) r.fmt = (uint32_t)v;
                    if (!tp_json_int(line, "levels", &v)) r.levels = (uint32_t)v;
                    if (!tp_json_int(line, "ln", &v)) r.ln = (int)v;
                    if (!tp_json_int(line, "cube", &v)) r.cube = (int)v;
                    tp_json_str(line, "file", r.file, sizeof r.file);
                    tp_json_str(line, "fmt", r.fmtname, sizeof r.fmtname);
                    tp_json_str(line, "run", r.run, sizeof r.run);
                    tp_json_str(line, "ctx", r.ctx, sizeof r.ctx);
                    if (!tp_json_int(line, "frame", &v)) r.frame = v;
                    { const char* s = tp_json_find(line, "sec"); if (s) r.sec = atof(s); }
                    r.at_ref = -1;
                    snprintf(r.line, sizeof r.line, "%s", line);
                    if (g_nrec == g_caprec) { g_caprec = g_caprec ? g_caprec * 2 : 1024; g_rec = (tx_rec*)realloc(g_rec, (size_t)g_caprec * sizeof *g_rec); }
                    g_rec[g_nrec] = r; tx_map_add(h, g_nrec); g_nrec++;
                }
            } else if (pass == 1 && !tp_json_hex64(line, "use", &h)) {
                const int i = tx_find(h);
                if (i >= 0) {
                    tx_rec* r = &g_rec[i]; long long v; char s[16];
                    r->uses++;
                    if (!tp_json_int(line, "gamma", &v)) r->gamma |= (int)v;
                    if (!tp_json_int(line, "expand", &v)) r->expand |= (int)v;
                    if (!tp_json_int(line, "signed", &v)) r->sgn |= (int)v;
                    if (!tp_json_int(line, "txl", &v)) r->txl |= (int)v;
                    if (!tp_json_int(line, "alpha_test", &v) && v) {
                        r->at = 1; long long ar; if (!tp_json_int(line, "alpha_ref", &ar)) r->at_ref = (int)ar; }
                    if (!tp_json_int(line, "min", &v) && (v == 1 || v == 3)) r->nearest = 1;   /* NEAREST, NEAREST_MIPMAP_NEAREST */
                    if (!tp_json_int(line, "wrap_s", &v)) { if (v == 1 || v == 0) r->wrap_s = 1; if (v == 2) r->mirror_s = 1; }
                    if (!tp_json_int(line, "wrap_t", &v)) { if (v == 1 || v == 0) r->wrap_t = 1; if (v == 2) r->mirror_t = 1; }
                    if (!r->ctrl1 && !tp_json_str(line, "ctrl1", s, sizeof s)) r->ctrl1 = (uint32_t)strtoul(s, NULL, 16);
                }
            }
        }
        at = e + 1;
    }
    free(d);
    return 0;
}

/* the file name part of an index path ('/' or '\\' separated) */
static const char* tx_base(const char* p)
{
    const char* b = p;
    for (const char* q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* colour | normal | alpha (cutout colour) | data */
static const char* tx_class(const tx_rec* r)
{
    const int cls = tp_class_of(r->fmt);
    if (!tp_class_is_color(cls) || r->cube) return "data";
    if (r->nearest || r->w <= 8 || r->h <= 8) return "data";
    if ((r->expand & 7) && !(r->gamma & 7)) return "normal";
    if (cls == TP_CLS_R8G8 && !(r->gamma & 7)) return "data";
    if (r->at) return "alpha";
    if (cls == TP_CLS_R8 && !(r->gamma & 7)) return "mask";
    return "color";
}

/* ---------------------------------------------------------------------------
 * Float images and the resampler
 * -----------------------------------------------------------------------*/
typedef struct { int w, h; float* p; } fimg;   /* 4 channels */
static fimg fimg_new(int w, int h) { fimg f; f.w = w; f.h = h; f.p = (float*)calloc((size_t)w * h * 4, sizeof(float)); return f; }
static float s2l(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float l2s(float c) { if (c <= 0.0f) return 0.0f; if (c >= 1.0f) return 1.0f; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

/* address modes: 0 clamp, 1 wrap, 2 mirror */
static int addr(int i, int n, int mode)
{
    if (mode == 1) { i %= n; return i < 0 ? i + n : i; }
    if (mode == 2) { const int p = 2 * n; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}
static double lanczos3(double x)
{
    x = fabs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = 3.14159265358979323846 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}
/* separable Lanczos-3 to (ow, oh), reading across edges per the address modes.
 * When magnifying, each pass clamps its result to the two source texels it
 * lies between (anti-ringing): Lanczos' overshoot at a hard edge would
 * otherwise become a halo once sharpened. */
static fimg resample(const fimg* s, int ow, int oh, int mx, int my)
{
    fimg t = fimg_new(ow, s->h), o = fimg_new(ow, oh);
    const double sx = (double)s->w / ow, sy = (double)s->h / oh;
    const double fx = sx > 1.0 ? sx : 1.0, fy = sy > 1.0 ? sy : 1.0;    /* widen when minifying */
    for (int x = 0; x < ow; x++) {
        const double u = (x + 0.5) * sx - 0.5;
        const int i0 = (int)floor(u - 3.0 * fx) + 1, i1 = (int)floor(u + 3.0 * fx);
        double wsum = 0; double wt[64]; int nt = 0;
        for (int i = i0; i <= i1 && nt < 64; i++) { wt[nt] = lanczos3((u - i) / fx); wsum += wt[nt++]; }
        const int a0 = addr((int)floor(u), s->w, mx), a1 = addr((int)floor(u) + 1, s->w, mx);
        for (int y = 0; y < s->h; y++) {
            double acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < nt; k++) {
                const float* p = s->p + ((size_t)y * s->w + addr(i0 + k, s->w, mx)) * 4;
                for (int c = 0; c < 4; c++) acc[c] += wt[k] * p[c];
            }
            const float* q0 = s->p + ((size_t)y * s->w + a0) * 4; const float* q1 = s->p + ((size_t)y * s->w + a1) * 4;
            for (int c = 0; c < 4; c++) {
                float v = (float)(acc[c] / wsum);
                if (sx < 1.0) { const float lo = q0[c] < q1[c] ? q0[c] : q1[c], hi = q0[c] < q1[c] ? q1[c] : q0[c];
                                v = v < lo ? lo : v > hi ? hi : v; }
                t.p[((size_t)y * ow + x) * 4 + c] = v;
            }
        }
    }
    for (int y = 0; y < oh; y++) {
        const double v = (y + 0.5) * sy - 0.5;
        const int j0 = (int)floor(v - 3.0 * fy) + 1, j1 = (int)floor(v + 3.0 * fy);
        double wsum = 0; double wt[64]; int nt = 0;
        for (int j = j0; j <= j1 && nt < 64; j++) { wt[nt] = lanczos3((v - j) / fy); wsum += wt[nt++]; }
        const int b0 = addr((int)floor(v), s->h, my), b1 = addr((int)floor(v) + 1, s->h, my);
        for (int x = 0; x < ow; x++) {
            double acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < nt; k++) {
                const float* p = t.p + ((size_t)addr(j0 + k, s->h, my) * ow + x) * 4;
                for (int c = 0; c < 4; c++) acc[c] += wt[k] * p[c];
            }
            const float* q0 = t.p + ((size_t)b0 * ow + x) * 4; const float* q1 = t.p + ((size_t)b1 * ow + x) * 4;
            for (int c = 0; c < 4; c++) {
                float r = (float)(acc[c] / wsum);
                if (sy < 1.0) { const float lo = q0[c] < q1[c] ? q0[c] : q1[c], hi = q0[c] < q1[c] ? q1[c] : q0[c];
                                r = r < lo ? lo : r > hi ? hi : r; }
                o.p[((size_t)y * ow + x) * 4 + c] = r;
            }
        }
    }
    free(t.p);
    return o;
}
/* Edge-aware sharpen of channels [0, nch), on gamma-space values: an unsharp
 * mask whose blur matches the upscale (sigma 0.5 source texels, so it lifts
 * the detail band the resampler softened, not the new pixel grid), weakened
 * where the local contrast is already high (edges do not get harsh), and the
 * result kept inside the min / max of the surrounding source-texel window
 * (radius = scale output pixels): no halo can form. Wrap-aware. */
static void sharpen(fimg* f, float strength, int nch, int mx, int my, int scale)
{
    if (strength <= 0.0f || nch <= 0) return;
    const int W = f->w, H = f->h, R = scale > 1 ? scale : 1;
    const double sigma = 0.5 * (scale > 1 ? scale : 1);
    const int kr = (int)ceil(3.0 * sigma);
    double kw[64]; double ks = 0;
    for (int i = -kr; i <= kr && kr < 32; i++) { kw[i + kr] = exp(-(double)i * i / (2.0 * sigma * sigma)); ks += kw[i + kr]; }
    for (int i = 0; i <= 2 * kr; i++) kw[i] /= ks;
    const size_t np = (size_t)W * H;
    float* tmp = (float*)malloc(np * 4 * sizeof(float)); float* blur = (float*)malloc(np * 4 * sizeof(float));
    float* mn = (float*)malloc(np * 4 * sizeof(float)); float* mxv = (float*)malloc(np * 4 * sizeof(float));
    float* t2 = (float*)malloc(np * 4 * sizeof(float));
    if (!tmp || !blur || !mn || !mxv || !t2) { free(tmp); free(blur); free(mn); free(mxv); free(t2); return; }
    /* separable Gaussian, and separable min / max over (2R+1)^2 */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < nch; c++) {
        double a = 0; float lo = 1e9f, hi = -1e9f;
        for (int i = -kr; i <= kr; i++) a += kw[i + kr] * f->p[((size_t)y * W + addr(x + i, W, mx)) * 4 + c];
        for (int i = -R; i <= R; i++) { const float v = f->p[((size_t)y * W + addr(x + i, W, mx)) * 4 + c]; if (v < lo) lo = v; if (v > hi) hi = v; }
        tmp[((size_t)y * W + x) * 4 + c] = (float)a; mn[((size_t)y * W + x) * 4 + c] = lo; t2[((size_t)y * W + x) * 4 + c] = hi;
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < nch; c++) {
        double a = 0; float lo = 1e9f, hi = -1e9f;
        for (int i = -kr; i <= kr; i++) a += kw[i + kr] * tmp[((size_t)addr(y + i, H, my) * W + x) * 4 + c];
        for (int i = -R; i <= R; i++) { const size_t k = ((size_t)addr(y + i, H, my) * W + x) * 4 + c;
                                        if (mn[k] < lo) lo = mn[k]; if (t2[k] > hi) hi = t2[k]; }
        blur[((size_t)y * W + x) * 4 + c] = (float)a; mxv[((size_t)y * W + x) * 4 + c] = lo; tmp[((size_t)y * W + x) * 4 + c] = hi;
    }
    /* mxv holds the window minimum, tmp the maximum */
    for (size_t i = 0; i < np; i++) for (int c = 0; c < nch; c++) {
        const size_t k = i * 4 + c;
        const float v = f->p[k], lo = mxv[k], hi = tmp[k], range = hi - lo;
        const float amt = strength * (1.0f - 0.75f * (range > 1.0f ? 1.0f : range));
        float o = v + amt * (v - blur[k]);
        f->p[k] = o < lo ? lo : o > hi ? hi : o;
    }
    free(tmp); free(blur); free(mn); free(mxv); free(t2);
}

/* alpha that keeps its coverage at the alpha reference (cutouts) */
static float coverage(const fimg* f, float ref)
{
    size_t n = 0;
    for (size_t i = 0; i < (size_t)f->w * f->h; i++) n += f->p[i * 4 + 3] >= ref;
    return (float)n / ((float)f->w * f->h);
}
static void keep_coverage(fimg* f, float ref, float want)
{
    float lo = 0.25f, hi = 4.0f, best = 1.0f;
    for (int it = 0; it < 24; it++) {
        const float k = 0.5f * (lo + hi);
        size_t n = 0;
        for (size_t i = 0; i < (size_t)f->w * f->h; i++) n += f->p[i * 4 + 3] * k >= ref;
        const float c = (float)n / ((float)f->w * f->h);
        best = k;
        if (c < want) lo = k; else hi = k;
    }
    for (size_t i = 0; i < (size_t)f->w * f->h; i++) { float a = f->p[i * 4 + 3] * best; f->p[i * 4 + 3] = a > 1.0f ? 1.0f : a; }
}

/* ---------------------------------------------------------------------------
 * PNG <-> float
 * -----------------------------------------------------------------------*/
static int load_png(const char* path, fimg* f, int* is_gray)
{
    wchar_t wp[1024]; tp_widen(path, wp, 1024);
    size_t n = 0; const char* err = NULL;
    uint8_t* d = tp_read_file(wp, 512ull << 20, &n, &err);
    if (!d) { fprintf(stderr, "%s: %s\n", path, err); return -1; }
    uint32_t w, h, iw, ih; int dep = 8, ct = 6;
    if (!tp_png_ihdr(d, n, &iw, &ih, &dep, &ct)) *is_gray = (ct == 0 || ct == 4);
    uint8_t* px = tp_img_decode(d, n, &GUID_WICPixelFormat32bppRGBA, 4, &w, &h, &err);
    free(d);
    if (!px) { fprintf(stderr, "%s: %s\n", path, err); return -1; }
    *f = fimg_new((int)w, (int)h);
    for (size_t i = 0; i < (size_t)w * h * 4; i++) f->p[i] = px[i] / 255.0f;
    free(px);
    return 0;
}
static int save_png(const char* path, const fimg* f, int gray)
{
    const size_t np = (size_t)f->w * f->h;
    uint8_t* b = (uint8_t*)malloc(np * 4);
    for (size_t i = 0; i < np; i++) {
        uint8_t q[4];
        for (int c = 0; c < 4; c++) { float v = f->p[i * 4 + c]; v = v < 0 ? 0 : v > 1 ? 1 : v; q[c] = (uint8_t)(v * 255.0f + 0.5f); }
        if (gray) b[i] = q[0];
        else { b[i * 4] = q[2]; b[i * 4 + 1] = q[1]; b[i * 4 + 2] = q[0]; b[i * 4 + 3] = q[3]; }
    }
    size_t n = 0;
    uint8_t* png = gray ? tp_png_encode(&GUID_WICPixelFormat8bppGray, (uint32_t)f->w, (uint32_t)f->h, (uint32_t)f->w, b, &n)
                        : tp_png_encode(&GUID_WICPixelFormat32bppBGRA, (uint32_t)f->w, (uint32_t)f->h, (uint32_t)f->w * 4, b, &n);
    free(b);
    if (!png) return -1;
    wchar_t wp[1024]; tp_widen(path, wp, 1024);
    const int r = tp_write_atomic(wp, png, n, 1);
    free(png);
    return r < 0 ? -1 : 0;
}

/* A tangent-space normal map by its texels (the title expands most of them in
 * the shader, not with the texture unit's BIASED remap, so the usage records
 * do not always say so): mean (R, G) about 0.5, B high, and nearly every
 * texel's expanded vector close to unit length. */
static int looks_normal(const fimg* f)
{
    double m[3] = {0, 0, 0}, unit = 0; const size_t np0 = (size_t)f->w * f->h;
    for (size_t i = 0; i < np0; i++) {
        const float* q = f->p + i * 4; double l = 0;
        for (int c = 0; c < 3; c++) { m[c] += q[c]; const double v = q[c] * 2.0 - 1.0; l += v * v; }
        unit += fabs(sqrt(l) - 1.0) < 0.12;
    }
    for (int c = 0; c < 3; c++) m[c] /= (double)np0;
    return fabs(m[0] - 0.5) < 0.1 && fabs(m[1] - 0.5) < 0.1 && m[2] > 0.7 && unit > 0.9 * (double)np0;
}

/* ---------------------------------------------------------------------------
 * External upscaler (Real-ESRGAN ncnn-vulkan): run with full paths, from a
 * working directory of our own, never from inside its folder.
 * -----------------------------------------------------------------------*/
static int run_external(const char* exe_in, const char* model, const char* in_rel, const char* out_rel, const char* wd_rel)
{
    /* every path absolute: the program runs in our own working directory */
    char exe[1024], in[1024], out[1024], workdir[1024];
    if (!_fullpath(exe, exe_in, sizeof exe) || !_fullpath(in, in_rel, sizeof in) || !_fullpath(out, out_rel, sizeof out) ||
        !_fullpath(workdir, wd_rel, sizeof workdir)) return -1;
    char models[1024]; snprintf(models, sizeof models, "%s", exe);
    char* sl = strrchr(models, '\\'); if (!sl) sl = strrchr(models, '/');
    if (sl) *sl = 0; else snprintf(models, sizeof models, ".");
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" -i \"%s\" -o \"%s\" -n %s -s 4 -m \"%s\\models\" -f png", exe, in, out, model, models);
    /* its progress and errors go to a log next to the images */
    char logp[1100]; snprintf(logp, sizeof logp, "%s\\external.log", workdir);
    wchar_t wlog[1100]; tp_widen(logp, wlog, 1100);
    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa); sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE lg = CreateFileW(wlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    wchar_t wcmd[4096], wexe[1024], wdir[1024];
    tp_widen(cmd, wcmd, 4096); tp_widen(exe, wexe, 1024); tp_widen(workdir, wdir, 1024);
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | (lg != INVALID_HANDLE_VALUE ? STARTF_USESTDHANDLES : 0); si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = lg; si.hStdInput = NULL;
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, lg != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, wdir, &si, &pi)) {
        fprintf(stderr, "cannot start %s (%lu)\n", exe, GetLastError());
        if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
        return -1;
    }
    if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
    if (WaitForSingleObject(pi.hProcess, 600000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1); fprintf(stderr, "%s timed out\n", exe); }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * upscale one texture
 * -----------------------------------------------------------------------*/
typedef struct { int scale; int external; const char* exe; const char* model; float sharp; float tint[3]; int tinted; int deblock; } up_opts;

/* DXT deblocking (a texture that was DXT1/3/5 on the disc): across each 4x4
 * block edge, a step that is small in absolute terms (under ~16/255) but
 * clearly larger than the gradient on either side of it is a quantisation
 * seam, not a feature; it is halved by moving the two texels at the seam
 * toward each other. Real edges (large steps, or steps that continue a
 * gradient) are left alone. Gamma-space values, colour channels only,
 * wrap-aware like the resampler. */
static void deblock(fimg* f, int mx, int my)
{
    const float T = 16.0f / 255.0f;
    for (int pass = 0; pass < 2; pass++) {
        const int n = pass ? f->h : f->w, lines = pass ? f->w : f->h, mode = pass ? my : mx;
        if (n < 8) continue;
        for (int l = 0; l < lines; l++)
            for (int e = 0; e < n; e += 4) {                     /* the seam between e-1 and e */
                if (e == 0 && mode != 1) continue;
                const int ia = addr(e - 2, n, mode), ib = addr(e - 1, n, mode), ic = e, id = addr(e + 1, n, mode);
                float* P[4];
                const int idx[4] = { ia, ib, ic, id };
                for (int k = 0; k < 4; k++)
                    P[k] = pass ? f->p + ((size_t)idx[k] * f->w + l) * 4 : f->p + ((size_t)l * f->w + idx[k]) * 4;
                for (int c = 0; c < 3; c++) {
                    const float step = P[2][c] - P[1][c], g1 = P[1][c] - P[0][c], g2 = P[3][c] - P[2][c];
                    const float as = fabsf(step), ag = fabsf(g1) > fabsf(g2) ? fabsf(g1) : fabsf(g2);
                    if (as < 1.0f / 255.0f || as > T || as < 1.5f * ag) continue;
                    P[1][c] += 0.25f * step; P[2][c] -= 0.25f * step;
                }
            }
    }
}

static int upscale_one(const tx_rec* r, const char* dump, const char* pack, const up_opts* o, char* why, size_t whyn)
{
    const char* cls = tx_class(r);   /* may become "normal" below */
    if (!strcmp(cls, "data")) { snprintf(why, whyn, "data (not upscaled)"); return 1; }
    /* the index is a file other people may hand you: its paths stay inside the dump, and the
     * output is always the bare file name inside the pack */
    if (!r->file[0] || strstr(r->file, "..") || strchr(r->file, ':') || r->file[0] == '/' || r->file[0] == '\\') {
        snprintf(why, whyn, "bad file path in the index"); return -1; }
    { tp_name_info ni; const char* b = tx_base(r->file);
      if (tp_parse_name(b, &ni) || ni.hash != r->hash) { snprintf(why, whyn, "file name does not match the record"); return -1; } }
    char in[1024]; snprintf(in, sizeof in, "%s\\%s", dump, r->file);
    for (char* p = in; *p; p++) if (*p == '/') *p = '\\';
    const char* ext = strrchr(in, '.');
    if (!ext || _stricmp(ext, ".png")) { snprintf(why, whyn, "not a PNG dump"); return 1; }
    if (o->scale == 1) {
        /* --scale 1: the dump's own texels (an identity pack), or with --tint
         * each colour channel multiplied as stored (a coverage / test pack) */
        char outp[1024];
        snprintf(outp, sizeof outp, "%s\\%s", pack, tx_base(r->file));
        { wchar_t w[1024]; tp_widen(pack, w, 1024); tp_mkdirs(w); }
        if (!o->tinted || (strcmp(cls, "color") && strcmp(cls, "alpha"))) {
            wchar_t wi[1024], wo[1024]; tp_widen(in, wi, 1024); tp_widen(outp, wo, 1024);
            if (!CopyFileW(wi, wo, FALSE)) { snprintf(why, whyn, "cannot copy"); return -1; }
            snprintf(why, whyn, "%s %ux%u copied", cls, r->w, r->h);
            return 0;
        }
        fimg f; int g = 0;
        if (load_png(in, &f, &g)) { snprintf(why, whyn, "cannot read"); return -1; }
        if (!g && looks_normal(&f)) {                 /* a normal map is never tinted */
            free(f.p);
            wchar_t wi[1024], wo[1024]; tp_widen(in, wi, 1024); tp_widen(outp, wo, 1024);
            if (!CopyFileW(wi, wo, FALSE)) { snprintf(why, whyn, "cannot copy"); return -1; }
            snprintf(why, whyn, "normal %ux%u copied", r->w, r->h);
            return 0;
        }
        for (size_t i = 0; i < (size_t)f.w * f.h; i++)
            for (int c = 0; c < 3; c++) { const float v = f.p[i * 4 + c] * o->tint[g ? 0 : c]; f.p[i * 4 + c] = v > 1.0f ? 1.0f : v; }
        const int rc = save_png(outp, &f, g);
        free(f.p);
        if (rc) { snprintf(why, whyn, "cannot write"); return -1; }
        snprintf(why, whyn, "%s %ux%u tinted", cls, r->w, r->h);
        return 0;
    }
    fimg src; int gray = 0;
    if (load_png(in, &src, &gray)) { snprintf(why, whyn, "cannot read"); return -1; }
    const int mx = r->wrap_s ? 1 : r->mirror_s ? 2 : 0, my = r->wrap_t ? 1 : r->mirror_t ? 2 : 0;
    int is_normal = !strcmp(cls, "normal");
    if (!is_normal && !gray) {
        if (looks_normal(&src)) { is_normal = 1; cls = "normal"; }
    }
    const int color = !is_normal && (!strcmp(cls, "color") || !strcmp(cls, "alpha"));
    const int normal = is_normal;
    const size_t np = (size_t)src.w * src.h;
    /* the resource channels the game expands to a vector (through its crossbar) */
    int vec[4] = {0, 0, 0, 0}, nvec = 0;
    if (normal) {
        u8 sel[4]; rsx_texture_component_remap(r->ctrl1 ? r->ctrl1 : 0xAAE4u, r->fmt, sel);
        const int outc[4] = { sel[1], sel[2], sel[3], sel[0] };          /* outputs R,G,B,A -> resource channel */
        for (int c = 0; c < 4; c++) if (((r->expand >> c) & 1) && outc[c] < 4) vec[outc[c]] = 1;
        for (int c = 0; c < 4; c++) nvec += vec[c];
        if (!nvec) { vec[0] = vec[1] = vec[2] = 1; nvec = 3; }          /* found by its texels: RGB */
    }
    const float ref = r->at && r->at_ref >= 0 ? (float)r->at_ref / 255.0f : 0.5f;
    const float cov0 = r->at ? coverage(&src, ref) : 0.0f;
    { const uint32_t bf = r->fmt & 0x9Fu;
      if (o->deblock && color && (bf == 0x86u || bf == 0x87u || bf == 0x88u)) deblock(&src, mx, my); }
    /* to linear light (colour), premultiplied by alpha (no colour bleeds in from clear texels) */
    if (color)
        for (size_t i = 0; i < np; i++) {
            const float a = src.p[i * 4 + 3];
            for (int c = 0; c < 3; c++) src.p[i * 4 + c] = s2l(src.p[i * 4 + c]) * (r->at ? 1.0f : a);
        }
    if (normal) for (size_t i = 0; i < np; i++) for (int c = 0; c < 4; c++) if (vec[c]) src.p[i * 4 + c] = src.p[i * 4 + c] * 2.0f - 1.0f;
    const int ow = src.w * o->scale, oh = src.h * o->scale;
    fimg up;
    if (o->external && color && !gray) {
        /* colour through the network on a wrap-aware bordered copy (RGB, gamma space) */
        const int B = src.w >= 64 ? 16 : 8;
        fimg pad = fimg_new(src.w + 2 * B, src.h + 2 * B);
        for (int y = 0; y < pad.h; y++) for (int x = 0; x < pad.w; x++) {
            const float* s = src.p + ((size_t)addr(y - B, src.h, my) * src.w + addr(x - B, src.w, mx)) * 4;
            float* d = pad.p + ((size_t)y * pad.w + x) * 4;
            const float a = r->at ? 1.0f : (s[3] > 1e-6f ? s[3] : 1.0f);
            for (int c = 0; c < 3; c++) d[c] = l2s(s[c] / a);
            d[3] = 1.0f;
        }
        char tin[1024], tout[1024], wd[1024];
        snprintf(wd, sizeof wd, "%s\\_texpack_tmp", pack);
        { wchar_t w[1024]; tp_widen(wd, w, 1024); tp_mkdirs(w); }
        snprintf(tin, sizeof tin, "%s\\%016llx_in.png", wd, (unsigned long long)r->hash);
        snprintf(tout, sizeof tout, "%s\\%016llx_out.png", wd, (unsigned long long)r->hash);
        if (save_png(tin, &pad, 0) || run_external(o->exe, o->model, tin, tout, wd)) {
            free(pad.p); free(src.p); snprintf(why, whyn, "external upscaler failed"); return -1;
        }
        fimg big; int g2 = 0;
        if (load_png(tout, &big, &g2) || big.w != pad.w * 4 || big.h != pad.h * 4) {
            free(pad.p); free(src.p); snprintf(why, whyn, "external output missing or the wrong size"); return -1;
        }
        { wchar_t w1[1024], w2[1024]; tp_widen(tin, w1, 1024); tp_widen(tout, w2, 1024); DeleteFileW(w1); DeleteFileW(w2); }
        /* crop the border, then (for 2x) resample down in linear light */
        fimg crop = fimg_new(src.w * 4, src.h * 4);
        for (int y = 0; y < crop.h; y++) for (int x = 0; x < crop.w; x++)
            for (int c = 0; c < 3; c++) crop.p[((size_t)y * crop.w + x) * 4 + c] = s2l(big.p[((size_t)(y + 4 * B) * big.w + x + 4 * B) * 4 + c]);
        free(big.p); free(pad.p);
        if (o->scale != 4) { fimg t = resample(&crop, ow, oh, mx, my); free(crop.p); crop = t; }
        /* alpha: our own resampler, always */
        fimg al = resample(&src, ow, oh, mx, my);
        up = crop;
        for (size_t i = 0; i < (size_t)ow * oh; i++) {
            const float a = al.p[i * 4 + 3];
            up.p[i * 4 + 3] = a;
            if (!r->at) for (int c = 0; c < 3; c++) up.p[i * 4 + c] *= a;     /* back to premultiplied, as below */
        }
        free(al.p);
    } else {
        up = resample(&src, ow, oh, mx, my);
    }
    if (r->at) keep_coverage(&up, ref, cov0);
    const size_t no = (size_t)ow * oh;
    if (normal) {
        for (size_t i = 0; i < no; i++) {
            float* p = up.p + i * 4; double l = 0;
            for (int c = 0; c < 4; c++) if (vec[c]) l += (double)p[c] * p[c];
            l = sqrt(l);
            /* three components: unit length; two (z derived in the shader): at most 1 */
            if (l > 1e-6 && (nvec >= 3 || l > 1.0)) for (int c = 0; c < 4; c++) if (vec[c]) p[c] = (float)(p[c] / l);
            for (int c = 0; c < 4; c++) if (vec[c]) p[c] = p[c] * 0.5f + 0.5f;
        }
    }
    if (color)
        for (size_t i = 0; i < no; i++) {
            float* p = up.p + i * 4;
            const float a = p[3] < 0 ? 0 : p[3] > 1 ? 1 : p[3];
            for (int c = 0; c < 3; c++) {
                float v = r->at ? p[c] : (a > 1e-6f ? p[c] / a : 0.0f);
                if (o->tinted) v *= o->tint[c];
                p[c] = l2s(v);
            }
            p[3] = a;
        }
    /* the built-in path is sharpened in gamma space (the external one is sharp already) */
    if (!normal && !(o->external && color && !gray)) sharpen(&up, o->sharp, color ? 3 : (gray ? 1 : 4), mx, my, o->scale);
    char outp[1024], dir[1024];
    snprintf(outp, sizeof outp, "%s\\%s", pack, tx_base(r->file));
    snprintf(dir, sizeof dir, "%s", pack);
    { wchar_t w[1024]; tp_widen(dir, w, 1024); tp_mkdirs(w); }
    const int rc = save_png(outp, &up, gray);
    free(up.p); free(src.p);
    if (rc) { snprintf(why, whyn, "cannot write"); return -1; }
    snprintf(why, whyn, "%s %dx%d -> %dx%d%s%s", cls, r->w, r->h, ow, oh, mx == 1 || my == 1 ? " (wraps)" : "",
             o->external && color && !gray ? " [external]" : "");
    return 0;
}

static int parse_hexlist(const char* s, uint64_t* out, int max)
{
    int n = 0;
    while (*s && n < max) {
        char* e = NULL; out[n++] = _strtoui64(s, &e, 16);
        if (e == s) break;
        s = *e ? e + 1 : e;
    }
    return n;
}

/* pack index: the dump's records for the files a pack holds, so the runtime
 * knows their level-0 hashes */
static void write_pack_index(const char* pack, const int* sel, int nsel)
{
    char p[1024]; snprintf(p, sizeof p, "%s\\index.jsonl", pack);
    FILE* f = fopen(p, "ab");
    if (!f) return;
    for (int i = 0; i < nsel; i++) fprintf(f, "%s\n", g_rec[sel[i]].line);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * compress: PNG -> DDS BC1 (opaque) / BC3 through WIC's DDS encoder, full mips
 * -----------------------------------------------------------------------*/
static int compress_one(const char* png, int force, int linear)
{
    wchar_t wp[1024]; tp_widen(png, wp, 1024);
    size_t n = 0; const char* err = NULL;
    uint8_t* d = tp_read_file(wp, 512ull << 20, &n, &err);
    if (!d) return -1;
    uint32_t w, h;
    uint8_t* px = tp_img_decode(d, n, &GUID_WICPixelFormat32bppRGBA, 4, &w, &h, &err);
    free(d);
    if (!px) return -1;
    if ((w & 3) || (h & 3)) { free(px); fprintf(stderr, "%s: not a multiple of 4\n", png); return 1; }
    int opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) if (px[i * 4 + 3] != 255) { opaque = 0; break; }
    const int bc = force ? force : (opaque ? 1 : 3);
    const uint32_t levels = tp_full_levels(w, h);
    char out[1024]; snprintf(out, sizeof out, "%s", png);
    char* dot = strrchr(out, '.'); if (dot) strcpy(dot, ".dds");
    wchar_t wo[1024]; tp_widen(out, wo, 1024);
    IWICImagingFactory* F = tp_wic();
    IWICStream* st = NULL; IWICBitmapEncoder* enc = NULL; IWICDdsEncoder* dds = NULL;
    HRESULT hr = IWICImagingFactory_CreateStream(F, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wo, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(F, &GUID_ContainerFormatDds, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream*)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_QueryInterface(enc, &IID_IWICDdsEncoder, (void**)&dds);
    if (SUCCEEDED(hr)) {
        WICDdsParameters p; memset(&p, 0, sizeof p);
        p.Width = w; p.Height = h; p.Depth = 1; p.MipLevels = levels; p.ArraySize = 1;
        p.DxgiFormat = bc == 1 ? DXGI_FORMAT_BC1_UNORM : bc == 2 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        p.Dimension = WICDdsTexture2D; p.AlphaMode = WICDdsAlphaModeStraight;
        hr = IWICDdsEncoder_SetParameters(dds, &p);
    }
    uint8_t* lv = px; uint32_t lw = w, lh = h;
    for (uint32_t m = 0; SUCCEEDED(hr) && m < levels; m++) {
        IWICBitmapFrameEncode* fr = NULL; UINT ai, mi, si;
        hr = IWICDdsEncoder_CreateNewFrame(dds, &fr, &ai, &mi, &si);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, NULL);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, lw, lh);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
        uint8_t* wr = lv;
        if (SUCCEEDED(hr) && !memcmp(&pf, &GUID_WICPixelFormat32bppBGRA, sizeof pf)) {
            wr = (uint8_t*)malloc((size_t)lw * lh * 4);
            for (size_t i = 0; i < (size_t)lw * lh; i++) { wr[i * 4] = lv[i * 4 + 2]; wr[i * 4 + 1] = lv[i * 4 + 1]; wr[i * 4 + 2] = lv[i * 4]; wr[i * 4 + 3] = lv[i * 4 + 3]; }
        } else if (SUCCEEDED(hr) && memcmp(&pf, &GUID_WICPixelFormat32bppRGBA, sizeof pf)) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, lh, lw * 4, lw * lh * 4, wr);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
        if (wr != lv) free(wr);
        if (fr) IWICBitmapFrameEncode_Release(fr);
        if (m + 1 < levels) {
            const uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
            uint8_t* nl = (uint8_t*)malloc((size_t)nw * nh * 4);
            tp_mip_down8(lv, lw, lh, 4, nl, linear);
            if (lv != px) free(lv);
            lv = nl; lw = nw; lh = nh;
        }
    }
    if (lv != px) free(lv);
    free(px);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (dds) IWICDdsEncoder_Release(dds);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (FAILED(hr)) { DeleteFileW(wo); fprintf(stderr, "%s: WIC DDS encode failed (0x%08lX)\n", png, (unsigned long)hr); return -1; }
    printf("%s -> BC%d, %u levels\n", out, bc, levels);
    return 0;
}

static int cmd_compress(const char* pack, int force, int linear)
{
    char pat[1024]; snprintf(pat, sizeof pat, "%s\\*.png", pack);
    wchar_t wpat[1024]; tp_widen(pat, wpat, 1024);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "no PNG in %s\n", pack); return 1; }
    int n = 0, bad = 0;
    do {
        char an[260], full[1024];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, an, sizeof an, NULL, NULL);
        tp_name_info ni; if (tp_parse_name(an, &ni)) continue;
        const int cls = tp_class_of(ni.fmt);
        if (cls != TP_CLS_RGBA8 && cls != TP_CLS_BC1 && cls != TP_CLS_BC2 && cls != TP_CLS_BC3) continue;
        snprintf(full, sizeof full, "%s\\%s", pack, an);
        if (compress_one(full, force, linear) < 0) bad++; else n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    printf("compressed %d file(s), %d failed\n", n, bad);
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------*/
static const char* opt(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
static int selected(const tx_rec* r, int argc, char** argv)
{
    const char* f = opt(argc, argv, "--fmt"); const char* mn = opt(argc, argv, "--min"); const char* c = opt(argc, argv, "--class");
    if (f && _stricmp(f, r->fmtname)) return 0;
    if (mn && (int)(r->w > r->h ? r->w : r->h) < atoi(mn)) return 0;
    if (c && strcmp(c, tx_class(r))) return 0;
    const char* only = opt(argc, argv, "--only");
    if (only) { uint64_t hs[256]; const int n = parse_hexlist(only, hs, 256); int hit = 0;
                for (int i = 0; i < n; i++) hit |= hs[i] == r->hash;
                if (!hit) return 0; }
    return 1;
}

/* ---------------------------------------------------------------------------
 * selftest: the upscaler's promises on synthetic images (no game data)
 * -----------------------------------------------------------------------*/
static int st_fail, st_pass;
#define ST(c, ...) do { if (c) st_pass++; else { st_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static float st_wave(int x, int y, int n) { return 0.5f + 0.35f * sinf(6.2831853f * (3.0f * x / n)) * cosf(6.2831853f * (2.0f * y / n)); }
static int cmd_selftest(void)
{
    const int N = 64;
    /* 1. wrap: a tiling texture stays seamless -- upscaling the texture rolled by half
     *    equals the upscale rolled by half (the edges see the same neighbours as the middle) */
    { fimg a = fimg_new(N, N), b = fimg_new(N, N);
      for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
          const float v = st_wave(x, y, N) + ((x * 7 + y * 13) % 5) * 0.02f;
          for (int c = 0; c < 4; c++) { a.p[((size_t)y * N + x) * 4 + c] = v; b.p[((size_t)y * N + (x + N / 2) % N) * 4 + c] = v; } }
      for (int sc = 2; sc <= 4; sc += 2) {
          fimg ua = resample(&a, N * sc, N * sc, 1, 1), ub = resample(&b, N * sc, N * sc, 1, 1);
          sharpen(&ua, 0.6f, 3, 1, 1, sc); sharpen(&ub, 0.6f, 3, 1, 1, sc);
          float md = 0;
          for (int y = 0; y < N * sc; y++) for (int x = 0; x < N * sc; x++) {
              const float d = fabsf(ua.p[((size_t)y * N * sc + x) * 4] - ub.p[((size_t)y * N * sc + (x + N * sc / 2) % (N * sc)) * 4]);
              if (d > md) md = d; }
          ST(md < 1e-4f, "wrap %dx: rolled upscale differs by %g", sc, md);
          /* and the seam is no rougher than the interior */
          double seam = 0, mid = 0;
          for (int y = 0; y < N * sc; y++) {
              seam += fabsf(ua.p[((size_t)y * N * sc + N * sc - 1) * 4] - ua.p[((size_t)y * N * sc) * 4]);
              mid += fabsf(ua.p[((size_t)y * N * sc + N * sc / 2 - 1) * 4] - ua.p[((size_t)y * N * sc + N * sc / 2) * 4]); }
          ST(seam < mid * 1.5 + 1e-3, "wrap %dx: seam step %.4f vs interior %.4f", sc, seam, mid);
          free(ua.p); free(ub.p); }
      free(a.p); free(b.p); }
    /* 2. clamp mode reads no texel across the edge: a texture black on the left half and
     *    white on the right keeps its pure columns at both outer edges */
    { fimg a = fimg_new(N, N);
      for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) for (int c = 0; c < 4; c++) a.p[((size_t)y * N + x) * 4 + c] = x < N / 2 ? 0.0f : 1.0f;
      fimg u = resample(&a, N * 2, N * 2, 0, 0); sharpen(&u, 0.6f, 3, 0, 0, 2);
      float lo = 1, hi = 0;
      for (size_t i = 0; i < (size_t)N * 2 * N * 2 * 4; i++) { if (u.p[i] < lo) lo = u.p[i]; if (u.p[i] > hi) hi = u.p[i]; }
      ST(u.p[0] == 0.0f && u.p[(size_t)(N * 2 - 1) * 4] == 1.0f, "clamp: edge columns %g %g", u.p[0], u.p[(size_t)(N * 2 - 1) * 4]);
      /* 3. no halo: anti-ringing + the clamped sharpen never leave the source range */
      ST(lo >= 0.0f && hi <= 1.0f, "halo: output range [%g, %g] outside the source's [0, 1]", lo, hi);
      free(u.p); free(a.p); }
    /* 4. alpha coverage at the reference is kept (a cutout leaf: a soft disc, ref 0.5) */
    { fimg a = fimg_new(N, N);
      for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
          const float r = sqrtf((float)((x - 20) * (x - 20) + (y - 30) * (y - 30))) / 18.0f;
          float al = 1.2f - 0.9f * r + 0.15f * sinf(x * 0.9f) * sinf(y * 1.3f); al = al < 0 ? 0 : al > 1 ? 1 : al;
          for (int c = 0; c < 3; c++) a.p[((size_t)y * N + x) * 4 + c] = 0.4f; a.p[((size_t)y * N + x) * 4 + 3] = al; }
      const float c0 = coverage(&a, 0.5f);
      for (int sc = 2; sc <= 4; sc += 2) {
          fimg u = resample(&a, N * sc, N * sc, 0, 0);
          keep_coverage(&u, 0.5f, c0);
          const float c1 = coverage(&u, 0.5f);
          ST(fabsf(c1 - c0) < 0.01f, "coverage %dx: %.4f -> %.4f", sc, c0, c1);
          free(u.p); }
      free(a.p); }
    /* 5. normal maps: resampled vectors renormalised to unit length, also after 8-bit storage */
    { fimg a = fimg_new(N, N);
      for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
          float n[3] = { 0.6f * sinf(x * 0.31f), 0.6f * cosf(y * 0.23f + x * 0.05f), 1.0f };
          const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          for (int c = 0; c < 3; c++) a.p[((size_t)y * N + x) * 4 + c] = n[c] / l; a.p[((size_t)y * N + x) * 4 + 3] = 1; }
      fimg u = resample(&a, N * 4, N * 4, 1, 1);
      float worst = 0, worst8 = 0;
      for (size_t i = 0; i < (size_t)N * 4 * N * 4; i++) {
          float* q = u.p + i * 4; const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
          for (int c = 0; c < 3; c++) q[c] /= l;
          const float l2 = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
          if (fabsf(l2 - 1) > worst) worst = fabsf(l2 - 1);
          float q8[3]; for (int c = 0; c < 3; c++) q8[c] = (floorf((q[c] * 0.5f + 0.5f) * 255.0f + 0.5f) / 255.0f) * 2.0f - 1.0f;
          const float l8 = sqrtf(q8[0] * q8[0] + q8[1] * q8[1] + q8[2] * q8[2]);
          if (fabsf(l8 - 1) > worst8) worst8 = fabsf(l8 - 1); }
      ST(worst < 1e-5f && worst8 < 0.012f, "normals: |len - 1| up to %g (%g after 8-bit storage)", worst, worst8);
      free(u.p); free(a.p); }
    /* 6. deblocking: a 4x4 DXT seam is halved, a real edge and a smooth ramp are untouched */
    { fimg a = fimg_new(16, 4);
      for (int y = 0; y < 4; y++) for (int x = 0; x < 16; x++) {
          float v = x < 4 ? 0.50f : x < 8 ? 0.53f : x < 12 ? 0.10f + 0.0f : 0.90f;   /* seam at 4 (3/255*~2.5), edges at 8, 12 */
          if (x >= 8 && x < 12) v = 0.10f;
          for (int c = 0; c < 4; c++) a.p[((size_t)y * 16 + x) * 4 + c] = v; }
      fimg r = fimg_new(16, 4);
      for (int y = 0; y < 4; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 4; c++) r.p[((size_t)y * 16 + x) * 4 + c] = 0.2f + 0.01f * x;
      deblock(&a, 0, 0); deblock(&r, 0, 0);
      const float s4 = a.p[4 * 4] - a.p[3 * 4];
      ST(fabsf(s4 - 0.015f) < 1e-4f, "deblock: the 0.03 seam became %g (want 0.015)", s4);
      ST(a.p[8 * 4] == 0.10f && a.p[7 * 4] == 0.53f && a.p[12 * 4] == 0.90f, "deblock: a real edge moved");
      int moved = 0; for (int x = 0; x < 16; x++) moved |= fabsf(r.p[x * 4] - (0.2f + 0.01f * x)) > 1e-6f;
      ST(!moved, "deblock: a smooth ramp moved");
      free(a.p); free(r.p); }
    /* 7. PNG round trip through our encoder/decoder is exact (8-bit BGRA) */
    { const int W = 37, H = 23; uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
      for (int i = 0; i < W * H * 4; i++) px[i] = (uint8_t)(i * 37 + (i >> 3));
      size_t n = 0; uint8_t* png = tp_png_encode(&GUID_WICPixelFormat32bppBGRA, W, H, W * 4, px, &n);
      uint32_t w2 = 0, h2 = 0; const char* err = NULL;
      uint8_t* back = png ? tp_img_decode(png, n, &GUID_WICPixelFormat32bppRGBA, 4, &w2, &h2, &err) : NULL;
      int same = back && w2 == (uint32_t)W && h2 == (uint32_t)H;
      for (int i = 0; same && i < W * H; i++)
          same = back[i * 4] == px[i * 4 + 2] && back[i * 4 + 1] == px[i * 4 + 1] && back[i * 4 + 2] == px[i * 4] && back[i * 4 + 3] == px[i * 4 + 3];
      ST(same, "PNG round trip not exact");
      free(px); free(png); free(back); }
    /* 8. compress: WIC's DDS encoder gives BC1 / BC2 / BC3 files with a full chain that our
     *    parser reads back, and the blocks decode to the image (PSNR > 25 dB: a sanity bound; ~29 dB on this busy test image) */
    { char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
      char dir[MAX_PATH + 32]; snprintf(dir, sizeof dir, "%stexpack_selftest_%lu", tmp, (unsigned long)GetCurrentProcessId());
      CreateDirectoryA(dir, NULL);
      const int W = 64, H = 32; uint8_t* bgra = (uint8_t*)malloc((size_t)W * H * 4);
      for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          uint8_t* q = bgra + ((size_t)y * W + x) * 4;
          q[2] = (uint8_t)(128 + 100 * sinf(x * 0.2f)); q[1] = (uint8_t)(128 + 90 * cosf(y * 0.3f)); q[0] = (uint8_t)(x * 3 + y);
          q[3] = (uint8_t)(x < 32 ? 255 : 255 - y * 4); }
      size_t pn = 0; uint8_t* png = tp_png_encode(&GUID_WICPixelFormat32bppBGRA, W, H, W * 4, bgra, &pn);
      for (int bc = 1; bc <= 3; bc++) {
          char pp[MAX_PATH + 96], dp[MAX_PATH + 96];
          snprintf(pp, sizeof pp, "%s\\0123456789abcdef_32x16_DXT1.png", dir);
          snprintf(dp, sizeof dp, "%s\\0123456789abcdef_32x16_DXT1.dds", dir);
          wchar_t wp[MAX_PATH + 96]; tp_widen(pp, wp, MAX_PATH + 96);
          if (!png || tp_write_atomic(wp, png, pn, 1) < 0) { ST(0, "compress: cannot write the PNG"); break; }
          const int rc = compress_one(pp, bc, 0);
          wchar_t wd[MAX_PATH + 96]; tp_widen(dp, wd, MAX_PATH + 96);
          size_t dn = 0; const char* err = NULL; uint8_t* dds = rc == 0 ? tp_read_file(wd, 64u << 20, &dn, &err) : NULL;
          tp_dds_info di; const int ok = dds && !tp_dds_parse(dds, dn, &di);
          const uint32_t want = bc == 1 ? TP_DXGI_BC1_UNORM : bc == 2 ? TP_DXGI_BC2_UNORM : TP_DXGI_BC3_UNORM;
          ST(ok && di.dxgi == want && di.w == (uint32_t)W && di.h == (uint32_t)H && di.levels == tp_full_levels(W, H),
             "compress BC%d: file %s (dxgi %u, %ux%u, %u levels)", bc, ok ? "parsed" : "unreadable", ok ? di.dxgi : 0, ok ? di.w : 0, ok ? di.h : 0, ok ? di.levels : 0);
          if (ok) {
              uint8_t* rgba = (uint8_t*)malloc((size_t)W * H * 4);
              tp_bc_decode(rgba, W * 4, dds + di.off[0], W, H, bc);
              double se = 0; int n = 0;
              for (int i = 0; i < W * H; i++) for (int c = 0; c < 3; c++) {
                  const double d = (double)rgba[i * 4 + c] - bgra[i * 4 + 2 - c]; se += d * d; n++; }
              const double psnr = 10.0 * log10(255.0 * 255.0 / (se / n + 1e-9));
              ST(psnr > 25.0, "compress BC%d: colour PSNR %.1f dB", bc, psnr);   /* a sanity bound: swapped channels or bad blocks give < 15 */
              free(rgba);
          }
          free(dds); DeleteFileA(dp); DeleteFileA(pp);
      }
      free(png); free(bgra); RemoveDirectoryA(dir); }
    printf("texpack selftest: %d passed, %d failed\n", st_pass, st_fail);
    return st_fail ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "selftest")) { if (!tp_wic()) return 1; return cmd_selftest(); }
    if (argc < 3) {
        fprintf(stderr, "usage:\n  texpack list <dump> [--fmt F] [--min N] [--class color|normal|alpha|mask|data] [--csv out.csv]\n"
                        "  texpack upscale <dump> <pack> [--scale 1|2|4] [--method lanczos|external --exe <path> --model <name>]\n"
                        "                  [--sharpen 0..1] [--deblock 0|1] [--only h1,h2] [--fmt F] [--min N] [--class C] [--limit N] [--tint r,g,b]\n"
                        "  texpack compress <pack> [--bc auto|bc1|bc2|bc3] [--mips gamma|linear]\n"
                        "  texpack tint <in.png> <out.png> r,g,b\n"
                        "  texpack selftest     (the upscaler's guarantees on synthetic images)\n");
        return 2;
    }
    if (!tp_wic()) { fprintf(stderr, "WIC is not available\n"); return 1; }
    const char* cmd = argv[1];
    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "tint")) {
        if (argc < 5) return 2;
        fimg f; int gray = 0; float t[3] = {1, 1, 1};
        sscanf(argv[4], "%f,%f,%f", &t[0], &t[1], &t[2]);
        if (load_png(argv[2], &f, &gray)) return 1;
        for (size_t i = 0; i < (size_t)f.w * f.h; i++) for (int c = 0; c < 3; c++) { float v = f.p[i * 4 + c] * t[c]; f.p[i * 4 + c] = v > 1 ? 1 : v; }
        return save_png(argv[3], &f, 0) ? 1 : 0;
    }
    if (!strcmp(cmd, "compress")) {
        const char* b = opt(argc, argv, "--bc"); const char* mp = opt(argc, argv, "--mips");
        return cmd_compress(argv[2], b && !strcmp(b, "bc1") ? 1 : b && !strcmp(b, "bc2") ? 2 : b && !strcmp(b, "bc3") ? 3 : 0, mp && !strcmp(mp, "linear"));
    }
    if (load_index(argv[2])) return 1;
    if (!strcmp(cmd, "list")) {
        const char* csv = opt(argc, argv, "--csv");
        FILE* cf = csv ? fopen(csv, "wb") : NULL;
        if (cf) fprintf(cf, "hash,w,h,fmt,levels,cube,class,uses,gamma,expand,alpha_test,wrap,first_run,first_frame,first_sec,ctx,file\n");
        int n = 0;
        printf("%-16s %9s %-10s %3s %-6s %4s %-4s %-6s  %s\n", "hash", "size", "format", "lv", "class", "uses", "wrap", "seen", "where (run frame ctx)");
        for (int i = 0; i < g_nrec; i++) {
            const tx_rec* r = &g_rec[i];
            if (!selected(r, argc, argv)) continue;
            char sz[24]; snprintf(sz, sizeof sz, "%ux%u", r->w, r->h);
            const char* wr = r->wrap_s || r->wrap_t ? "wrap" : r->mirror_s || r->mirror_t ? "mirr" : "clmp";
            printf("%016llx %9s %-10s %3u %-6s %4d %-4s %6.1fs  %s f%lld %s\n", (unsigned long long)r->hash, sz, r->fmtname, r->levels,
                   tx_class(r), r->uses, wr, r->sec, r->run, r->frame, r->ctx);
            if (cf) fprintf(cf, "%016llx,%u,%u,%s,%u,%d,%s,%d,%d,%d,%d,%s,%s,%lld,%.1f,\"%s\",%s\n", (unsigned long long)r->hash, r->w, r->h,
                            r->fmtname, r->levels, r->cube, tx_class(r), r->uses, r->gamma, r->expand, r->at, wr, r->run, r->frame, r->sec,
                            r->ctx, r->file);
            n++;
        }
        if (cf) fclose(cf);
        printf("%d of %d textures\n", n, g_nrec);
        return 0;
    }
    if (!strcmp(cmd, "upscale")) {
        if (argc < 4) return 2;
        up_opts o; memset(&o, 0, sizeof o);
        const char* s = opt(argc, argv, "--scale"); o.scale = s ? atoi(s) : 2;
        if (o.scale != 1 && o.scale != 2 && o.scale != 4) { fprintf(stderr, "--scale is 1, 2 or 4\n"); return 2; }
        const char* m = opt(argc, argv, "--method"); o.external = m && !strcmp(m, "external");
        o.exe = opt(argc, argv, "--exe"); o.model = opt(argc, argv, "--model");
        if (!o.model) o.model = "realesrgan-x4plus";
        if (o.external && !o.exe) { fprintf(stderr, "--method external needs --exe <realesrgan-ncnn-vulkan.exe>\n"); return 2; }
        const char* sh = opt(argc, argv, "--sharpen"); o.sharp = sh ? (float)atof(sh) : 0.6f;
        const char* db = opt(argc, argv, "--deblock"); o.deblock = db ? atoi(db) : 1;
        const char* ti = opt(argc, argv, "--tint");
        if (ti && sscanf(ti, "%f,%f,%f", &o.tint[0], &o.tint[1], &o.tint[2]) == 3) o.tinted = 1;
        const char* lim = opt(argc, argv, "--limit"); const int limit = lim ? atoi(lim) : 1 << 30;
        int* sel = (int*)malloc(sizeof(int) * (size_t)(g_nrec + 1)); int nsel = 0, done = 0, skipped = 0, failed = 0;
        for (int i = 0; i < g_nrec && done < limit; i++) {
            if (!selected(&g_rec[i], argc, argv)) continue;
            char why[200];
            const int r = upscale_one(&g_rec[i], argv[2], argv[3], &o, why, sizeof why);
            printf("%016llx %s\n", (unsigned long long)g_rec[i].hash, why);
            if (r == 0) { sel[nsel++] = i; done++; } else if (r > 0) skipped++; else failed++;
        }
        write_pack_index(argv[3], sel, nsel);
        free(sel);
        if (o.external && !failed) {          /* the network's scratch folder: its log, then the folder */
            char t[1024]; wchar_t w[1024];
            snprintf(t, sizeof t, "%s/_texpack_tmp/external.log", argv[3]); tp_widen(t, w, 1024); DeleteFileW(w);
            snprintf(t, sizeof t, "%s/_texpack_tmp", argv[3]); tp_widen(t, w, 1024); RemoveDirectoryW(w);
        }
        printf("upscaled %d, skipped %d, failed %d\n", done, skipped, failed);
        return failed ? 1 : 0;
    }
    fprintf(stderr, "unknown command %s\n", cmd);
    return 2;
}
