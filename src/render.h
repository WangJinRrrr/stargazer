#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <unordered_map>

namespace sg {

// 浅色、留白多的纸面配色。层色用实色而不是叠在深底上的半透明白：
// 原生 EDIT 只能吃实色，浅色上也更容易看出卡片和阴影。
struct Theme {
    D2D1_COLOR_F bg             = D2D1::ColorF(0xF7F6F3);  // 暖纸
    D2D1_COLOR_F panel          = D2D1::ColorF(0xEFEEE9);  // 侧栏
    D2D1_COLOR_F card           = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F control        = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F hover          = D2D1::ColorF(0xF3F1EC);
    D2D1_COLOR_F pressed        = D2D1::ColorF(0xE7E3DB);
    // 描边/分隔
    D2D1_COLOR_F border         = D2D1::ColorF(0xE6E2DA);
    D2D1_COLOR_F border_strong  = D2D1::ColorF(0xD5D0C7);
    D2D1_COLOR_F stroke_strong  = D2D1::ColorF(0xA8A29E);
    D2D1_COLOR_F divider        = D2D1::ColorF(0xE6E2DA);
    // 文字
    D2D1_COLOR_F text           = D2D1::ColorF(0x1C1917);
    D2D1_COLOR_F text_dim       = D2D1::ColorF(0x57534E);
    D2D1_COLOR_F text_faint     = D2D1::ColorF(0xA8A29E);
    // 强调：沉一点的灰蓝，压在暖纸上不刺眼
    D2D1_COLOR_F accent         = D2D1::ColorF(0x3E6A82);
    D2D1_COLOR_F on_accent      = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F danger         = D2D1::ColorF(0x9B4040);
    D2D1_COLOR_F sel_fill       = D2D1::ColorF(0.243f, 0.416f, 0.510f, 0.12f);
    D2D1_COLOR_F sel_stroke     = D2D1::ColorF(0x3E6A82);
    D2D1_COLOR_F accent_press   = D2D1::ColorF(0x32586C);
    D2D1_COLOR_F shadow         = D2D1::ColorF(0.15f, 0.12f, 0.08f, 0.10f);
    D2D1_COLOR_F shadow_soft    = D2D1::ColorF(0.15f, 0.12f, 0.08f, 0.06f);
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

// 半透明主题色 -> 实色。原生 EDIT 子控件只能给实色（WM_CTLCOLOREDIT），
// 而 Theme 里的层色是半透明白；不换算就会与周围差一截。
COLORREF to_solid(D2D1_COLOR_F c);               // c 必须不透明（如 theme.bg）
COLORREF blend(D2D1_COLOR_F fg, COLORREF under);  // fg 叠在实色 under 上

}  // namespace sg
