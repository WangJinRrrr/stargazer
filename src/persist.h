#pragma once

#include <string>

namespace sg {

struct Paths {
    std::wstring exe_dir;   // 末尾无分隔符
    std::wstring data_dir;  // exe_dir + "\\data"，末尾无分隔符
    bool writable = false;
};

// 目录存在且能创建并删除临时文件
bool dir_writable(const std::wstring& dir);

// 定位 exe 目录、创建 data 子目录、探测可写性
bool init_paths(Paths& out);

std::wstring data_file(const Paths& p, const wchar_t* name);
bool save_text(const Paths& p, const wchar_t* name, const std::wstring& text);
bool load_text(const Paths& p, const wchar_t* name, std::wstring& out);

// 当前进程 exe 的绝对路径；失败返回空
std::wstring exe_path();

// HKCU\...\Run 的 stargazer 值
bool autostart_enabled();
bool autostart_set(bool enabled, const std::wstring& exe_path);
// 已启用但记录的路径与当前 exe 不一致时改写；未启用时不动
bool autostart_heal(const std::wstring& exe_path);

}  // namespace sg
