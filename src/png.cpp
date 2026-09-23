#include "png.h"

#include <windows.h>
#include <wincodec.h>

#include <string>

namespace sg {

namespace {

std::wstring hr_text(HRESULT hr) {
    wchar_t buf[64] = {};
    ::swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// 把 DIB 里的像素整理成“自上而下、32bpp BGRA”，用 WICWritePixels 一次写出去
bool to_bgra_top_down(const BITMAPINFOHEADER& bi, const uint8_t* pixels, size_t avail,
                      std::vector<uint8_t>& out, int& w, int& h, int& stride) {
    if (bi.biWidth <= 0 || bi.biHeight == 0) return false;
    if (bi.biBitCount != 24 && bi.biBitCount != 32) return false;
    w = bi.biWidth;
    h = bi.biHeight < 0 ? -bi.biHeight : bi.biHeight;
    const bool top_down = bi.biHeight < 0;
    const int src_stride = ((w * bi.biBitCount + 31) / 32) * 4;
    if (avail < static_cast<size_t>(src_stride) * h) return false;

    stride = w * 4;
    // 有些程序复制出来的 32bpp DIB 里 alpha 全是 0（它们的本意是“不写 alpha”）。
    // 照抄就得到一张全透明 PNG（贴出来是白块）→ 全 0 时统一当不透明，
    // 与 images.cpp 取系统缩略图时的做法一致。
    bool alpha_all_zero = false;
    if (bi.biBitCount == 32) {
        alpha_all_zero = true;
        for (int y = 0; y < h && alpha_all_zero; ++y) {
            const uint8_t* s = pixels + static_cast<size_t>(y) * src_stride;
            for (int x = 0; x < w; ++x) {
                if (s[x * 4 + 3] != 0) {
                    alpha_all_zero = false;
                    break;
                }
            }
        }
    }
    out.assign(static_cast<size_t>(stride) * h, 0);
    for (int y = 0; y < h; ++y) {
        // 目标第 y 行对应源里的哪一行：自下而上时要翻过来
        const int src_y = top_down ? y : (h - 1 - y);
        const uint8_t* s = pixels + static_cast<size_t>(src_y) * src_stride;
        uint8_t* d = out.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            d[x * 4 + 0] = s[x * (bi.biBitCount / 8) + 0];  // B
            d[x * 4 + 1] = s[x * (bi.biBitCount / 8) + 1];  // G
            d[x * 4 + 2] = s[x * (bi.biBitCount / 8) + 2];  // R
            d[x * 4 + 3] = (bi.biBitCount == 32 && !alpha_all_zero) ? s[x * 4 + 3] : 255;
        }
    }
    return true;
}

}  // namespace

bool png_encode_dib(const std::wstring& path, const std::vector<uint8_t>& dib,
                    std::wstring& error) {
    error.clear();
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
        error = L"位图数据不完整";
        return false;
    }
    BITMAPINFOHEADER bi{};
    ::memcpy(&bi, dib.data(), sizeof(bi));
    if (bi.biSize < sizeof(BITMAPINFOHEADER) || bi.biCompression != BI_RGB) {
        error = L"不支持的位图格式（只支持未压缩的 BI_RGB）";
        return false;
    }
    // BITMAPV5HEADER 等更大的头：像素从头里 biSize 之后开始
    const size_t pix_off = bi.biSize;
    if (dib.size() <= pix_off) {
        error = L"位图没有像素数据";
        return false;
    }

    std::vector<uint8_t> bgra;
    int w = 0, h = 0, stride = 0;
    if (!to_bgra_top_down(bi, dib.data() + pix_off, dib.size() - pix_off, bgra, w, h, stride)) {
        error = L"位图尺寸或位深不支持（需要 24/32bpp）";
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        error = L"无法初始化图片编码器 " + hr_text(hr);
        return false;
    }

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    do {
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) break;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) break;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        if (FAILED(hr)) break;
        hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) break;
        hr = enc->CreateNewFrame(&frame, nullptr);
        if (FAILED(hr)) break;
        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) break;
        hr = frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
        if (FAILED(hr)) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) break;
        hr = frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(stride),
                                static_cast<UINT>(bgra.size()), bgra.data());
        if (FAILED(hr)) break;
        hr = frame->Commit();
        if (FAILED(hr)) break;
        hr = enc->Commit();
        if (FAILED(hr)) break;
        ok = true;
    } while (false);

    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    factory->Release();

    if (!ok) {
        error = L"图片编码失败 " + hr_text(hr);
        ::DeleteFileW(path.c_str());  // 别留下半个文件
    }
    return ok;
}

}  // namespace sg
