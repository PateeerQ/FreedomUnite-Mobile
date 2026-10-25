// FreedomUniteRecomp launcher -- see image.h.
#include "image.h"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>

bool image_load(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h) {
    static bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)) || true;
    (void)com;
    std::wstring wp(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), (int)wp.size());

    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(wp.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        rgba.resize((size_t)uw * uh * 4);
        ok = uw && uh && SUCCEEDED(conv->CopyPixels(NULL, uw * 4, (UINT)rgba.size(), rgba.data()));
        w = (int)uw;
        h = (int)uh;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

// The PNG encoder may take BGRA only (it says so through SetPixelFormat).
static const uint8_t *bgra(const uint8_t *rgba, int w, int h) {
    static std::vector<uint8_t> out;
    out.assign(rgba, rgba + (size_t)w * h * 4);
    for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
    return out.data();
}

bool image_save_png(const std::string &path, const uint8_t *rgba, int w, int h) {
    std::wstring wp(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), (int)wp.size());
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    bool ok = false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) &&
        SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize((UINT)w, (UINT)h)) &&
        SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
        SUCCEEDED(fr->WritePixels((UINT)h, (UINT)w * 4, (UINT)(w * h * 4),
                                  (BYTE *)(fmt == GUID_WICPixelFormat32bppRGBA ? rgba : bgra(rgba, w, h)))) &&
        SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
        ok = true;
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

#else

bool image_load(const std::string &, std::vector<uint8_t> &, int &, int &) { return false; }
bool image_save_png(const std::string &, const uint8_t *, int, int) { return false; }

#endif
