#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace sg {

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

    // rc 是**物理像素**（子窗口坐标），调用方负责用 Renderer::to_physical 换算
    void open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
              std::function<void(const std::wstring&)> commit,
              std::function<void()> cancel);
    void set_rect(const RECT& rc);
    void close();
    // 改写输入框内容（路径栏跟随当前目录用）
    void set_text(const std::wstring& text);
    bool is_open() const { return hwnd != nullptr; }
    std::wstring text() const;
    void focus();
};

}  // namespace sg
