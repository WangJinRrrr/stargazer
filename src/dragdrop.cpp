#include "dragdrop.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>  // DROPFILES（定义在 shlobj_core.h）

namespace sg {
namespace {

std::function<void(const std::vector<std::wstring>&)> g_hook;
bool* g_in_drag = nullptr;

bool read_hdrop(IDataObject* obj, std::vector<std::wstring>& out) {
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (FAILED(obj->GetData(&fe, &stg))) return false;

    bool ok = false;
    if (HDROP drop = static_cast<HDROP>(::GlobalLock(stg.hGlobal))) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len + 1, L'\0');
            ::DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            if (!path.empty()) out.push_back(path);
        }
        ::GlobalUnlock(stg.hGlobal);
        ok = !out.empty();
    }
    ::ReleaseStgMedium(&stg);
    return ok;
}

class DropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* obj, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        const bool ok = obj->QueryGetData(&fe) == S_OK;
        if (g_in_drag) *g_in_drag = ok;
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = (g_in_drag && *g_in_drag) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (g_in_drag) *g_in_drag = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* obj, DWORD, POINTL, DWORD* effect) override {
        std::vector<std::wstring> paths;
        const bool ok = read_hdrop(obj, paths);
        if (ok && g_hook) g_hook(paths);
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (g_in_drag) *g_in_drag = false;
        return S_OK;
    }

private:
    ULONG refs_ = 1;
};

DropTarget* g_target = nullptr;

}  // namespace

HGLOBAL make_hdrop(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;

    // 布局：DROPFILES 头 + 每条路径（各自以 \0 结尾）+ 末尾再一个 \0（双 NUL 收尾）
    size_t chars = 1;  // 末尾的额外 NUL
    for (const auto& p : paths) chars += p.size() + 1;
    const SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL h = ::GlobalAlloc(GHND, bytes);  // GHND = 固定内存 + 自动清零
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
    if (!df) {
        ::GlobalFree(h);
        return nullptr;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;  // 宽字符：中文路径全靠它

    auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        ::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size();
        *dst++ = L'\0';
    }
    *dst = L'\0';

    ::GlobalUnlock(h);
    return h;
}

bool dragdrop_init(HWND hwnd) {
    if (g_target) return true;
    g_target = new DropTarget();
    if (FAILED(::RegisterDragDrop(hwnd, g_target))) {
        g_target->Release();
        g_target = nullptr;
        return false;
    }
    return true;
}

void dragdrop_shutdown(HWND hwnd) {
    if (!g_target) return;
    ::RevokeDragDrop(hwnd);
    g_target->Release();
    g_target = nullptr;
    g_hook = nullptr;
}

void dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)> hook) {
    g_hook = std::move(hook);
}

void dragdrop_set_drag_flag(bool* flag) { g_in_drag = flag; }

}  // namespace sg
