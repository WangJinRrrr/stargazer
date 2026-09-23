#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sg {

// 把一段 DIB 编码成 PNG 文件。
// dib = BITMAPINFOHEADER / BITMAPV5HEADER 起，后跟像素数据；支持 24bpp 与 32bpp，
// 行序由头里的 biHeight 正负决定（负 = 自上而下）。
// 失败返回 false 并填 error（中文，可直接进托盘气泡）。
bool png_encode_dib(const std::wstring& path, const std::vector<uint8_t>& dib,
                    std::wstring& error);

}  // namespace sg
