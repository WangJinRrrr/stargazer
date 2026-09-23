#include "model/bgra.h"

namespace sg {

void premultiply_bgra(uint8_t* px, size_t count) {
    if (!px) return;
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = px + i * 4;
        const unsigned a = p[3];
        if (a == 255) continue;
        if (a == 0) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        p[0] = static_cast<uint8_t>(p[0] * a / 255);
        p[1] = static_cast<uint8_t>(p[1] * a / 255);
        p[2] = static_cast<uint8_t>(p[2] * a / 255);
    }
}

}  // namespace sg
