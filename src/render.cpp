#include "render.h"

#include <cstdio>

#include "icons.h"
#include "images.h"

namespace sg {

namespace {

// 字体是否存在：拿 GDI 枚举一次比 CreateTextFormat 可靠 ——
// DWrite 遇到不存在的族名会静默给一个替代字体，不会失败。
bool font_family_exists(const wchar_t* name) {
    HDC dc = ::GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    ::wcsncpy_s(lf.lfFaceName, name, _TRUNCATE);
    bool found = false;
    ::EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ::ReleaseDC(nullptr, dc);
    return found;
}

}  // namespace

const wchar_t* ui_font_family() {
    static const std::wstring family = [] {
        for (const wchar_t* candidate : { L"Segoe UI Variable Text", L"Segoe UI",
                                          L"Microsoft YaHei UI" }) {
            if (font_family_exists(candidate)) return std::wstring(candidate);
        }
        return std::wstring(L"Microsoft YaHei UI");
    }();
    return family.c_str();
}

bool Renderer::init(HWND wnd) {
    hwnd = wnd;
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) return false;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(&dwrite)))) {
        return false;
    }
    dpi = static_cast<float>(::GetDpiForWindow(hwnd));
    if (dpi <= 0.f) dpi = static_cast<float>(::GetDpiForSystem());
    if (dpi <= 0.f) dpi = 96.f;
    return create_device_resources();
}

bool Renderer::sync_dpi() {
    const float d = static_cast<float>(::GetDpiForWindow(hwnd));
    if (d <= 0.f || d == dpi) return false;
    dpi = d;
    // 图标位图也属于旧设备，必须一起丢弃（否则会把旧设备的位图画到新设备上）
    discard_device_resources();
    icons_on_device_lost();
    images_on_device_lost();
    return true;
}

bool Renderer::create_device_resources() {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right > 0 ? rc.right : 1),
        static_cast<UINT32>(rc.bottom > 0 ? rc.bottom : 1));

    // SetDpi 后所有绘制坐标都是 96 DPI 下的逻辑像素，DPI 换算全部交给 D2D
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
        D2D1::HwndRenderTargetProperties(hwnd, size, D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(factory->CreateHwndRenderTarget(props, hwnd_props, &rt))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush))) {
        // 必须连 rt 一起释放：否则留下 rt != null 而 brush == null 的状态，
        // 下一次 begin() 不会重建，后面每个 fill_/text 都会空指针崩
        discard_device_resources();
        return false;
    }
    return true;
}

void Renderer::discard_device_resources() {
    if (brush) {
        brush->Release();
        brush = nullptr;
    }
    if (rt) {
        rt->Release();
        rt = nullptr;
    }
}

void Renderer::discard_formats() {
    for (auto& kv : formats) kv.second->Release();
    formats.clear();
}

void Renderer::shutdown() {
    discard_device_resources();
    discard_formats();
    if (dwrite) {
        dwrite->Release();
        dwrite = nullptr;
    }
    if (factory) {
        factory->Release();
        factory = nullptr;
    }
}

bool Renderer::begin() {
    if (!rt) {
        if (!create_device_resources()) return false;
    }
    // 窗口尺寸变化后 rt 需要同步，否则绘制会被裁剪
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom));
    if (rt->GetPixelSize() != size) rt->Resize(size);
    rt->BeginDraw();
    return true;
}

void Renderer::end() {
    if (!rt) return;
    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // 设备丢失：丢弃设备相关资源，下一帧重建。图标位图由 icons 模块自理
        discard_device_resources();
        icons_on_device_lost();
        images_on_device_lost();
    }
}

void Renderer::release_surfaces() { discard_device_resources(); }

IDWriteTextFormat* Renderer::format(float size, DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align) {
    if (!dwrite) return nullptr;  // 设备/工厂未就绪：调用方按 nullptr 处理
    wchar_t key[64] = {};
    ::swprintf_s(key, L"%.1f|%d|%d", size, static_cast<int>(weight), static_cast<int>(align));
    auto it = formats.find(key);
    if (it != formats.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dwrite->CreateTextFormat(ui_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", &fmt))) {
        return nullptr;
    }
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 单元格内文字过长时截断，不溢到相邻单元格
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats.emplace(key, fmt);
    return fmt;
}

void Renderer::clear(D2D1_COLOR_F c) { rt->Clear(&c); }

D2D1_SIZE_F Renderer::client_logical() const {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const float s = scale();
    return D2D1::SizeF(static_cast<float>(rc.right) / s, static_cast<float>(rc.bottom) / s);
}

D2D1_POINT_2F Renderer::to_logical(POINT physical) const {
    const float s = scale();
    return D2D1::Point2F(static_cast<float>(physical.x) / s, static_cast<float>(physical.y) / s);
}

RECT Renderer::to_physical(const D2D1_RECT_F& logic) const {
    const float s = scale();
    RECT rc{};
    rc.left = static_cast<LONG>(logic.left * s);
    rc.top = static_cast<LONG>(logic.top * s);
    rc.right = static_cast<LONG>(logic.right * s);
    rc.bottom = static_cast<LONG>(logic.bottom * s);
    return rc;
}

void Renderer::fill_rect(const D2D1_RECT_F& r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    rt->FillRectangle(&r, brush);
}

D2D1_SIZE_F Renderer::measure_text(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!dwrite || !fmt || s.empty()) return D2D1::SizeF(0.f, 0.f);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 4096.f,
                                        1024.f, &layout)) ||
        !layout) {
        return D2D1::SizeF(0.f, 0.f);
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return D2D1::SizeF(m.width, m.height);
}

void Renderer::fill_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    brush->SetColor(c);
    rt->FillRoundedRectangle(&rr, brush);
}

void Renderer::stroke_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    brush->SetColor(c);
    rt->DrawRoundedRectangle(&rr, brush, width);
}

void Renderer::text(const D2D1_RECT_F& r, const std::wstring& s, IDWriteTextFormat* fmt,
                    D2D1_COLOR_F c) {
    if (!fmt || s.empty()) return;
    brush->SetColor(c);
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

COLORREF to_solid(D2D1_COLOR_F c) {
    const auto b = [](float v) {
        const float x = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        return static_cast<int>(x * 255.f + 0.5f);
    };
    return RGB(b(c.r), b(c.g), b(c.b));
}

COLORREF blend(D2D1_COLOR_F fg, COLORREF under) {
    const float a = fg.a < 0.f ? 0.f : (fg.a > 1.f ? 1.f : fg.a);
    const float ur = static_cast<float>(GetRValue(under)) / 255.f;
    const float ug = static_cast<float>(GetGValue(under)) / 255.f;
    const float ub = static_cast<float>(GetBValue(under)) / 255.f;
    return to_solid(D2D1::ColorF(fg.r * a + ur * (1.f - a), fg.g * a + ug * (1.f - a),
                                 fg.b * a + ub * (1.f - a), 1.f));
}

ID2D1Bitmap* make_bitmap(Renderer& r, const void* pixels, int w, int h) {    if (!r.rt || w <= 0 || h <= 0 || !pixels) return nullptr;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), r.dpi, r.dpi);
    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(r.rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)),
                                  pixels, static_cast<UINT32>(w * 4), props, &bmp))) {
        return nullptr;
    }
    return bmp;
}

}  // namespace sg
