#include "model/dirlist.h"

#include <algorithm>

#include "model/search.h"

namespace sg {

void sort_dir_entries(std::vector<DirEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;  // 目录在前
        return natural_compare(a.name, b.name) < 0;
    });
}

std::wstring new_folder_name(const std::vector<std::wstring>& taken_names) {
    std::wstring base = L"新建文件夹";
    if (!name_taken(taken_names, base)) return base;
    for (int n = 2; n < 10000; ++n) {
        std::wstring candidate = base + L" (" + std::to_wstring(n) + L")";
        if (!name_taken(taken_names, candidate)) return candidate;
    }
    return base;  // 一万个同名文件夹之后就算了，交给系统报错
}

bool name_taken(const std::vector<std::wstring>& taken_names, const std::wstring& name) {
    if (name.empty()) return false;
    for (const auto& t : taken_names) {
        if (equals_ci(t, name)) return true;
    }
    return false;
}

}  // namespace sg
