#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <unordered_map>

namespace sg {

// Windows 11 Fluent 2 深色令牌（数值取自 WinUI 的 dark themeresources）。
// 半透明白的“叠加色”是有意的：D2D 会与清屏色按 alpha 混合，
// 所以 card/control/hover 叠在 bg 上得到的就是 Win11 那一套层色，
// 换底色时不用重新算每层的实色值。
struct Theme {
    // 底色
    D2D1_COLOR_F bg             = D2D1::ColorF(0x202020);  // SolidBackgroundFillColorBase
    D2D1_COLOR_F panel          = D2D1::ColorF(0x272727);
    // 层/控件底
    D2D1_COLOR_F card           = D2D1::ColorF(1.f, 1.f, 1.f, 0.0512f);  // CardBackground
    D2D1_COLOR_F control        = D2D1::ColorF(1.f, 1.f, 1.f, 0.0605f);  // ControlFillDefault
    D2D1_COLOR_F hover          = D2D1::ColorF(1.f, 1.f, 1.f, 0.0837f);  // ControlFillSecondary
    D2D1_COLOR_F pressed        = D2D1::ColorF(1.f, 1.f, 1.f, 0.0326f);  // ControlFillTertiary
    // 描边/分隔
    D2D1_COLOR_F border         = D2D1::ColorF(1.f, 1.f, 1.f, 0.0698f);  // ControlStrokeDefault
    D2D1_COLOR_F border_strong  = D2D1::ColorF(1.f, 1.f, 1.f, 0.0930f);
    D2D1_COLOR_F stroke_strong  = D2D1::ColorF(1.f, 1.f, 1.f, 0.5451f);  // 复选框等强描边
    D2D1_COLOR_F divider        = D2D1::ColorF(1.f, 1.f, 1.f, 0.0837f);  // DividerStrokeDefault
    // 文字
    D2D1_COLOR_F text           = D2D1::ColorF(1.f, 1.f, 1.f, 0.8941f);  // TextFillPrimary
    D2D1_COLOR_F text_dim       = D2D1::ColorF(1.f, 1.f, 1.f, 0.6196f);  // TextFillSecondary
    D2D1_COLOR_F text_faint     = D2D1::ColorF(1.f, 1.f, 1.f, 0.4784f);  // TextFillTertiary
    // 强调
    D2D1_COLOR_F accent         = D2D1::ColorF(0x60CDFF);                // AccentFillDefault（深色）
    D2D1_COLOR_F on_accent      = D2D1::ColorF(0.f, 0.f, 0.f, 0.8941f);  // 强调色上的字：深色
    D2D1_COLOR_F danger         = D2D1::ColorF(0xFF99A4);                // SystemErrorTextColor
    // 选中底（列表/网格）：强调色降透明度 + 描边，文字仍用 primary
    D2D1_COLOR_F sel_fill       = D2D1::ColorF(0.f, 0.5f, 0.75f, 0.34f);
    D2D1_COLOR_F sel_stroke     = D2D1::ColorF(0.376f, 0.804f, 1.f, 0.65f);
    D2D1_COLOR_F accent_press   = D2D1::ColorF(0.376f, 0.804f, 1.f, 1.f);  // 强调色 hover
};

// 全局 UI 字体：Win11 的 Segoe UI Variable Text → Segoe UI → Microsoft YaHei UI。
// Renderer::format 与原生 EDIT 子控件（edit.cpp）都用它，保证自绘文字与输入框同脸。
const wchar_t* ui_font_family();

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

    // 呼出/跨显示器移动后重新取一次窗口所在显示器的 DPI。
    // 窗口未显示时 GetDpiForWindow 取不准（懒加载后这一点就会咬人），
    // 所以创建时的值只能当初始猜测，这里校正；变了就释放表面让下一帧按新缩放重建。
    bool sync_dpi();

    bool begin();  // 取到 rt 并 BeginDraw；false 表示跳过这一帧
    void end();    // EndDraw；D2DERR_RECREATE_TARGET 时重建设备资源

    // 释放绘制表面（隐藏态复用）。保留像素缓冲，下次绘制时按需重建。
    void release_surfaces();

    IDWriteTextFormat* format(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    // 量一行文字的宽度（标签下划线、链接下划线用）。失败返回 0 宽。
    D2D1_SIZE_F measure_text(const std::wstring& s, IDWriteTextFormat* fmt);

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
