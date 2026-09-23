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

void clipboard_set_text(const std::wstring& text) {
    if (text.empty()) return;
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();
    if (HGLOBAL h = make_text_hglobal(text)) {
        if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
    }
    ::CloseClipboard();
}

bool clipboard_get_text(std::wstring& out) {
    out.clear();
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!::OpenClipboard(nullptr)) return false;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            out.assign(p);
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return !out.empty();
}

bool clipboard_get_image_dib(std::vector<uint8_t>& out) {
    out.clear();
    // 绝大多数复制位图的程序都会给 CF_DIB；CF_BITMAP（HBITMAP）不处理：
    // HBITMAP 是设备相关位图，取像素还要过一遍 DC，得不偿失。
    UINT fmt = 0;
    if (::IsClipboardFormatAvailable(CF_DIBV5)) {
        fmt = CF_DIBV5;
    } else if (::IsClipboardFormatAvailable(CF_DIB)) {
        fmt = CF_DIB;
    } else {
        return false;
    }
    if (!::OpenClipboard(nullptr)) return false;
    if (HANDLE h = ::GetClipboardData(fmt)) {
        if (const void* p = ::GlobalLock(h)) {
            const SIZE_T n = ::GlobalSize(h);
            if (n >= sizeof(BITMAPINFOHEADER)) {
                out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
            }
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return !out.empty();
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
