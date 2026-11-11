/*
 * inFamous 2 -- PNG through WIC (Windows Imaging Component: an OS component).
 *
 * The one PNG writer of the port, shared by the checkpoint harness's
 * screenshots (src/if2_checkpoint.cpp) and the save-state thumbnails (the
 * slot files' THMB section and slotNN.png beside them, through
 * ss_slots_host.png_encode in src/main.cpp).
 *
 *   if2_png_write(path, w, h, bgr24, stride)    24-bit BGR rows (a DIB's) -> a .png file
 *   if2_png_encode_rgba(rgba, w, h, stride, &png, &n)
 *                                               RGBA rows -> PNG bytes (malloc'd; free())
 * Any thread: each call initialises COM for itself (multithreaded) and
 * releases it.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

/* Encode w x h 24-bit BGR rows into the stream. */
bool encode_bgr(IWICImagingFactory* fac, IWICStream* st, int w, int h, const void* bits, UINT stride)
{
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    if (SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
        SUCCEEDED(fr->Initialize(nullptr)) &&
        SUCCEEDED(fr->SetSize((UINT)w, (UINT)h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(fr->WritePixels((UINT)h, stride, stride * (UINT)h, (BYTE*)bits)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    return ok;
}

}  // namespace

extern "C" int if2_png_write(const char* path, int w, int h, const void* bits, unsigned stride)
{
    if (!path || !bits || w <= 0 || h <= 0) return 0;
    bool ok = false;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    IWICStream* st = nullptr;
    const std::string p(path);
    std::wstring wp(p.begin(), p.end());
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)))
        ok = encode_bgr(fac, st, w, h, bits, stride);
    if (st) st->Release();
    if (fac) fac->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
    if (!ok) DeleteFileA(path);
    return ok ? 1 : 0;
}

extern "C" int if2_png_encode_rgba(const uint8_t* rgba, int w, int h, int stride, void** png, size_t* n)
{
    if (!rgba || w <= 0 || h <= 0 || !png || !n) return 0;
    *png = nullptr;
    *n = 0;
    const UINT bstride = ((UINT)w * 3 + 3) & ~3u;
    uint8_t* bgr = (uint8_t*)calloc((size_t)bstride * (size_t)h, 1);
    if (!bgr) return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t* s = rgba + (size_t)y * (size_t)stride;
        uint8_t* d = bgr + (size_t)y * bstride;
        for (int x = 0; x < w; x++) { d[x * 3] = s[x * 4 + 2]; d[x * 3 + 1] = s[x * 4 + 1]; d[x * 3 + 2] = s[x * 4]; }
    }
    bool ok = false;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    IWICStream* st = nullptr;
    IStream* mem = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromIStream(mem)) &&
        encode_bgr(fac, st, w, h, bgr, bstride)) {
        LARGE_INTEGER z;
        z.QuadPart = 0;
        ULARGE_INTEGER end;
        HGLOBAL hg = nullptr;
        if (SUCCEEDED(mem->Seek(z, STREAM_SEEK_CUR, &end)) && end.QuadPart > 0 &&
            SUCCEEDED(GetHGlobalFromStream(mem, &hg)) && hg) {
            const void* src = GlobalLock(hg);
            void* out = src ? malloc((size_t)end.QuadPart) : nullptr;
            if (out) {
                memcpy(out, src, (size_t)end.QuadPart);
                *png = out;
                *n = (size_t)end.QuadPart;
                ok = true;
            }
            if (src) GlobalUnlock(hg);
        }
    }
    if (st) st->Release();
    if (fac) fac->Release();
    if (mem) mem->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
    free(bgr);
    return ok ? 1 : 0;
}
