#pragma once

#include <cstddef>
#include <cstdint>

namespace sg {

// 原地把直通 alpha 的 BGRA 转成预乘（D2D 的 PREMULTIPLIED 需要）
// px 为 BGRA 顺序，每像素 4 字节；count 为像素数
void premultiply_bgra(uint8_t* px, size_t count);

}  // namespace sg
