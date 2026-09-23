#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>
#include <vector>

#include "viewapi.h"
#include "views/grid.h"

namespace sg {

struct App;

// 布局常量（逻辑 DIP）
constexpr float kBrowseRowH = 32.f;   // 列表行高（Win11 标准控件高）
constexpr float kBrowseBarH = 32.f;   // 路径栏高度
constexpr float kBrowseIcon = 20.f;   // 行内图标边长

// 路径栏 / 列表区矩形（列表上沿要把“正在读取/错误/提示”那一行让出来，
// 否则提示文字会压在第一条上 —— 命中与渲染共用这一个上沿）
D2D1_RECT_F browse_path_rect(D2D1_SIZE_F client);
D2D1_RECT_F browse_list_rect(const AppState& s, D2D1_SIZE_F client);
// 返回可见行下标；未命中返回 -1
int browse_row_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt);
// 列表最多能显示多少行
int browse_rows_visible(const AppState& s, D2D1_SIZE_F client);

// 当前选中项的完整路径；没有选中返回空
std::wstring browse_sel_path(const AppState& s);

void browse_render(App& app);
bool browse_keydown(App& app, UINT vk);
void browse_context_menu(App& app, POINT screen_pt, POINT client_pt);

// 导航
void browse_go(App& app, const std::wstring& path);   // 进入（推历史）
void browse_up(App& app);
void browse_back(App& app);
void browse_forward(App& app);
void browse_refresh(App& app);
// 配置里的根目录（config.txt 的 browse_root）；为空则用 exe 所在目录兜底
std::wstring browse_root(const AppState& s, const std::wstring& exe_dir);

// 视图激活/离开：激活时确保路径就位并开始枚举；离开时收起路径输入框
void browse_activate(App& app);
void browse_leave(App& app);

// 文件操作（都在工作线程执行，完成后刷新列表）
void browse_open_selected(App& app);        // 目录=进入；文件=用默认程序打开
void browse_reveal_selected(App& app);      // 在资源管理器里定位
void browse_copy_selected(App& app);        // Ctrl+C：路径（两种剪贴板格式）
void browse_paste(App& app);                // Ctrl+V：把剪贴板里的文件复制/移动到当前目录
void browse_rename_selected(App& app);      // F2
void browse_delete_selected(App& app, bool recycle);
void browse_new_folder(App& app);           // F7
void browse_add_to_box(App& app);           // 添加到当前收纳盒
// 路径栏输入框开关（点路径栏或 Ctrl+L 打开；回车跳转）
void browse_edit_path(App& app);
void browse_sync_path_edit(App& app);

// 工作线程完成通知（由 app 层在 WM_APP_DIR_LOADED / WM_APP_FS_OP_DONE 里调用）
void browse_on_dir_loaded(App& app);
void browse_on_op_done(App& app);

}  // namespace sg
