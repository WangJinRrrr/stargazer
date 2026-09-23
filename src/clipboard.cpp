#include "clipboard.h"

#include <windows.h>
#include <ole2.h>      // DROPEFFECT_*
#include <shellapi.h>  // HDROP / DragQueryFileW

#include "dragdrop.h"  // make_hdrop（与拖出共用同一个构造函数）

namespace sg {
namespace {

const wchar_t* kDropEffectFormat = L"Preferred DropEffect";

// CF_UNICODETEXT 的全局内存块。成功后所有权归剪贴板，调用方不得再释放
HGLOBAL make_text_hglobal(const std::wstring& text) {
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return nullptr;
    void* p = ::GlobalLock(h);
    if (!p) {
        ::GlobalFree(h);
        return nullptr;
    }
    ::memcpy(p, text.c_str(), bytes);
    ::GlobalUnlock(h);
    return h;
}

UINT drop_effect_format() {
    static UINT fmt = ::RegisterClipboardFormatW(kDropEffectFormat);
    return fmt;
}

}  // namespace

void clipboard_set_paths(const std::vector<std::wstring>& paths, bool move) {
    if (paths.empty()) return;
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();

    if (HGLOBAL h = make_hdrop(paths)) {
        if (!::SetClipboardData(CF_HDROP, h)) ::GlobalFree(h);
    }
    if (HGLOBAL h = make_text_hglobal(paths.front())) {
        if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
    }
    // 剪贴板与拖动共用这个格式：资源管理器据此决定粘贴是复制还是移动
    if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
        if (void* p = ::GlobalLock(h)) {
            *static_cast<DWORD*>(p) = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            ::GlobalUnlock(h);
            if (!::SetClipboardData(drop_effect_format(), h)) ::GlobalFree(h);
        } else {
            ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
}

std::vector<std::wstring> clipboard_get_paths(bool& move) {
    move = false;
    std::vector<std::wstring> out;
    if (!::IsClipboardFormatAvailable(CF_HDROP)) return out;
    if (!::OpenClipboard(nullptr)) return out;

    if (HANDLE h = ::GetClipboardData(CF_HDROP)) {
        if (HDROP drop = static_cast<HDROP>(h)) {
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(len + 1, L'\0');
                ::DragQueryFileW(drop, i, path.data(), len + 1);
                path.resize(len);
                if (!path.empty()) out.push_back(path);
            }
        }
    }
    if (const UINT fmt = drop_effect_format(); fmt != 0 && ::IsClipboardFormatAvailable(fmt)) {
        if (HANDLE h = ::GetClipboardData(fmt)) {
            if (const void* p = ::GlobalLock(h)) {
                move = (*static_cast<const DWORD*>(p) & DROPEFFECT_MOVE) != 0;
                ::GlobalUnlock(h);
            }
        }
    }
    ::CloseClipboard();
    return out;
}

}  // namespace sg
