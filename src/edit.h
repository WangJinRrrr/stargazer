#pragma once

#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <string>

namespace sg {

struct Renderer;

// 深色主题下的 EDIT 背景刷（父窗口在 WM_CTLCOLOREDIT 里返回它）
HBRUSH edit_bg_brush();

// 一次编辑用到的“实色”配色。EDIT 只能给实色，所以视图先把 Theme 里的半透明层色
// 叠成实色再传进来（见 render.h 的 blend）。
struct EditPaint {
    COLORREF text = CLR_INVALID;
    COLORREF bg = CLR_INVALID;
    HBRUSH brush = nullptr;  // 与 bg 同色；由父窗口的 WM_CTLCOLOREDIT 返回
};

// 按窗口句柄取回该 EDIT 的配色（供父窗口的 WM_CTLCOLOREDIT 用）。找不到返回 false。
bool edit_paint_for(HWND hwnd, EditPaint& out);

// 编辑框文字与位置的附加参数。默认 = “输入框”那套（可可框 + 10px 内边距）。
struct EditStyle {
    float pad_x = 10.f;  // 左右内边距（逻辑 DIP）；矩形已经就是那段文字时传 0
    bool center = false;  // 居中的标签（盒子标签）：不居中就会一改名就“跳”到左边
    // 就地编辑（改名）：底色 = 该行当前实际的填充色、字色 = 最终文字色。
    // 这样打字时看到的就是这段文字最终的样子（而不是一个输入框方块）。
    // 两者都留 CLR_INVALID 时用输入框默认配色。
    EditPaint paint;
};

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
    // 最近一次用的配色（父窗口的 WM_CTLCOLOREDIT 取用）
    EditPaint paint;
    HBRUSH owned_brush = nullptr;  // 就地编辑自己的底色刷；close 时释放

    // rc 是**物理像素**（子窗口坐标），调用方负责用 Renderer::to_physical 换算。
    void open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
              std::function<void(const std::wstring&)> commit,
              std::function<void()> cancel, const EditStyle& style = EditStyle());
    void set_rect(const RECT& rc);
    void close();
    // 改写输入框内容（路径栏跟随当前目录用）
    void set_text(const std::wstring& text);
    bool is_open() const { return hwnd != nullptr; }
    std::wstring text() const;
    void focus();
};

// 就地编辑框的实际矩形：在目标文字区域内摆一个 20px 高的框。
// 实测（WM_PRINTCLIENT 量像素）：原生 EDIT 的文字是**贴顶**的，且自带约 5px 行距，
// 直接铺满整行会与 DWrite 居中的原名差好几像素；收到 20px 并居中后就对齐了。
// area 高于 28 时（多行文字行）铺满整行 —— 既对齐第一行，又能盖掉旧文字的后几行。
D2D1_RECT_F edit_box_rect(const D2D1_RECT_F& area);

}  // namespace sg
