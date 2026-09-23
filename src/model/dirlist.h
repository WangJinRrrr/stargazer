#pragma once

#include <string>
#include <vector>

namespace sg {

// 目录里的一项（**不存时间/大小**：这一层要保持无 Win32、可测试；
// 需要的话由视图自己从 FindFirstFileW 的结果里另外带一份）
struct DirEntry {
    std::wstring name;
    bool is_dir = false;
};

// 排序规则：目录在前，然后按自然序（大小写不敏感）比较名字。
// 与 Explore 的观感一致：1.txt < 2.txt < 10.txt，且文件夹总在上面。
void sort_dir_entries(std::vector<DirEntry>& entries);

// 名字筛选（浏览视图的筛选框）：needle 为空时原样返回，否则只留下名字里含它的项。
// 不动顺序 —— 筛选不该把排序也换了（需要的话调用方自己再排一次）。
std::vector<DirEntry> filter_dir_entries(const std::vector<DirEntry>& entries,
                                         const std::wstring& needle);

// 新建文件夹的名字：先试“新建文件夹”，被占了就依次试“新建文件夹 (2)”、“(3)”…
std::wstring new_folder_name(const std::vector<std::wstring>& taken_names);

// 被占用的名字集合（视图从当前列表构造），供重命名/新建时检查重名
bool name_taken(const std::vector<std::wstring>& taken_names, const std::wstring& name);

}  // namespace sg
