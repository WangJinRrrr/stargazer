#include "dragdrop.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>  // DROPFILES 与 SHCreateStdEnumFmtEtc（都在 shlobj_core.h）

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

// 拖出的数据源：只提供 CF_HDROP（与资源管理器互通靠的正是这个格式）。
// 手写而不是用 SHCreateDataObject：后者对“来自不同目录的多个路径”还要给父目录 PIDL，
// 反而比这几十行更绕。
class HdropDataObject : public IDataObject {
public:
    explicit HdropDataObject(HGLOBAL hdrop) : hdrop_(hdrop) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) {
            if (hdrop_) ::GlobalFree(hdrop_);
            delete this;
        }
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* stg) override {
        if (!fe || !stg) return E_POINTER;
        if (fe->cfFormat != CF_HDROP || (fe->tymed & TYMED_HGLOBAL) == 0) return DV_E_FORMATETC;
        if (!hdrop_) return E_UNEXPECTED;
        // 所有权转移给调用方
        stg->tymed = TYMED_HGLOBAL;
        stg->hGlobal = hdrop_;
        stg->pUnkForRelease = nullptr;
        hdrop_ = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (!fe) return E_POINTER;
        return (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL) != 0) ? S_OK
                                                                             : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;  // 只有一种格式，谈不上“等价格式”
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return ::SHCreateStdEnumFmtEtc(1, &fe, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ULONG refs_ = 1;
    HGLOBAL hdrop_ = nullptr;
};

// 拖出的源：只关心左键是否松开与 Esc 是否按下（其余交给系统默认光标反馈）
class DropSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
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

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD key_state) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if ((key_state & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ULONG refs_ = 1;
};

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

void dragdrop_test_invoke(const std::vector<std::wstring>& paths) {
    if (g_hook) g_hook(paths);
}

bool dragdrop_begin_drag(HWND owner, const std::vector<std::wstring>& paths) {
    (void)owner;  // DoDragDrop 不需要 owner 窗口
    if (paths.empty()) return false;

    HGLOBAL h = make_hdrop(paths);
    if (!h) return false;

    auto* obj = new HdropDataObject(h);  // 接管内存块所有权，Release 时负责释放
    auto* src = new DropSource();
    DWORD effect = 0;
    // DoDragDrop 跑嵌套消息循环，期间会重入 WM_PAINT：
    // 调用方（app 层）用 in_drag 抑制悬停更新，并在返回后复位
    const HRESULT hr = ::DoDragDrop(obj, src, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    obj->Release();
    src->Release();
    return SUCCEEDED(hr) && hr != DRAGDROP_S_CANCEL;
}

}  // namespace sg
