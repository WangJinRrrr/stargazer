#pragma once

#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <string>

namespace sg {

struct Renderer;

// 深色主题下的 EDIT 背景刷（父窗口在 WM_CTLCOLOREDIT 里返回它）
HBRUSH edit_bg_brush();

// 所有文本编辑都用原生 EDIT 子控件，绝不自绘：
// 中文 IME 候选窗、光标、选区、剪贴板快捷键、右键菜单全部免费拿到。
struct InlineEdit {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    HFONT font = nullptr;
    std::function<void(const std::wstring&)> on_commit;
    std::function<void()> on_cancel;
    // 视图可能想先处理某些按键（例如 ↓ 从搜索框进网格）。返回 true = 已被消费。
    std::function<bool(UINT vk)> on_key;
    // 搜索框用：移焦到网格时提交内容但保留输入框（否则内容会跟输入框一起消失）
    bool keep_open_on_blur = false;
    // 多行模式（待办的常驻输入框）：粘多行文本要原样收下。
    // 回车仍然由子类拦下来当“提交”，不会插换行。
    bool multiline = false;
    bool committing = false;  // 防重入：失焦提交时不再触发取消
    // 最近一次的**物理像素**矩形（视图要在它周围画聚焦框，见 edit_draw_focus_ring）
    RECT last_rc{};

    // rc 是**物理像素**（子窗口坐标），调用方负责用 Renderer::to_physical 换算。
    // pad_x 是左右内边距（逻辑 DIP）：改名框要对齐被改写的那段文字时传 0（矩形已经就是那段文字的位置）。
    // center 用于居中的标签（盒子标签）：文字居中才不会一改名就“跳”到左边。
    void open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
              std::function<void(const std::wstring&)> commit,
              std::function<void()> cancel, float pad_x = 10.f, bool center = false);
    void set_rect(const RECT& rc);
    void close();
    // 改写输入框内容（路径栏跟随当前目录用）
    void set_text(const std::wstring& text);
    bool is_open() const { return hwnd != nullptr; }
    std::wstring text() const;
    void focus();
};

// 给正在编辑的子控件画一圈聚焦框（Win11 文本框的观感）。
// 视图在 render 里调；框画在 EDIT 矩形外面一圈 ——
// 画在里面会被 EDIT 自己的方底盖掉。
void edit_draw_focus_ring(Renderer& r, const InlineEdit& e);

// 改名框的实际矩形：在目标文字区域内摆一个 20px 高的框。
// 实测（WM_PRINTCLIENT 量像素）：原生 EDIT 的文字是**贴顶**的，且自带约 5px 行距，
// 直接铺满整行会与 DWrite 居中的原名差好几像素；收到 20px 并居中后就对齐了。
// area 高 > 28 时（多行文字行）改为贴顶 —— 多行文本本来就是从第一行开始排的。
D2D1_RECT_F edit_box_rect(const D2D1_RECT_F& area);

}  // namespace sg
