#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <unordered_map>

namespace sg {

struct Theme {
    D2D1_COLOR_F bg       = D2D1::ColorF(0.106f, 0.110f, 0.125f, 1.f);
    D2D1_COLOR_F panel    = D2D1::ColorF(0.145f, 0.153f, 0.176f, 1.f);
    D2D1_COLOR_F card     = D2D1::ColorF(0.180f, 0.192f, 0.220f, 1.f);
    D2D1_COLOR_F hover    = D2D1::ColorF(0.235f, 0.251f, 0.290f, 1.f);
    D2D1_COLOR_F border   = D2D1::ColorF(0.250f, 0.266f, 0.306f, 1.f);
    D2D1_COLOR_F text     = D2D1::ColorF(0.850f, 0.870f, 0.900f, 1.f);
    D2D1_COLOR_F text_dim = D2D1::ColorF(0.500f, 0.530f, 0.580f, 1.f);
    D2D1_COLOR_F accent   = D2D1::ColorF(0.290f, 0.580f, 0.900f, 1.f);
};

struct Renderer {
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    HWND hwnd = nullptr;
    float dpi = 96.f;  // 物理 DPI；DIP 坐标下所有尺寸都用 96 DPI 逻辑像素
    Theme theme;
    std::unordered_map<std::wstring, IDWriteTextFormat*> formats;

    bool init(HWND hwnd);
    void shutdown();

    bool begin();  // 取到 rt 并 BeginDraw；false 表示跳过这一帧
    void end();    // EndDraw；D2DERR_RECREATE_TARGET 时重建设备资源

    IDWriteTextFormat* format(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    // 坐标系统一在这三个函数里换算，视图层不得再自己乘除 DPI
    float scale() const { return dpi / 96.f; }
    // 客户区尺寸（逻辑 DIP）。D2D 因为 SetDpi 用逻辑坐标，GetClientRect 给的是物理像素，
    // 两者不能混用，否则高 DPI 下卡片会超出窗口。
    D2D1_SIZE_F client_logical() const;
    // 鼠标消息给的是物理客户区坐标 -> 逻辑 DIP
    D2D1_POINT_2F to_logical(POINT physical) const;
    // 逻辑 DIP 矩形 -> 物理像素矩形（给子 HWND 用，如 EDIT）
    RECT to_physical(const D2D1_RECT_F& logic) const;

    void clear(D2D1_COLOR_F c);
    void fill_rect(const D2D1_RECT_F& r, D2D1_COLOR_F c);
    void fill_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c);
    void stroke_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1.f);
    void text(const D2D1_RECT_F& r, const std::wstring& s, IDWriteTextFormat* fmt, D2D1_COLOR_F c);

private:
    bool create_device_resources();
    void discard_device_resources();
    void discard_formats();
};

ID2D1Bitmap* make_bitmap(Renderer& r, const void* pixels, int w, int h);

}  // namespace sg
