#include "xrender/entity.h"
#include <iostream>
#include <span>

namespace fbx::xrender {

RectEntity::RectEntity(float x, float y, float w, float h, float r, float g, float b, float a) noexcept
    : data_{x, y, w, h, r, g, b, a} 
{}

std::expected<void, std::error_code> RectEntity::record(DisplayList& list) const noexcept {
    std::span<const uint8_t> dataSpan(reinterpret_cast<const uint8_t*>(&data_), sizeof(fbx::shared::RectData));
    auto res = list.addCommand(fbx::shared::CommandType::DrawRect, dataSpan);
    if (res) {
        std::cout << "[xrender] RectEntity recorded its commands.\n";
    }
    return res;
}

} // namespace fbx::xrender
