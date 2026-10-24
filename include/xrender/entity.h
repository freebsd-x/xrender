#pragma once

#include "xrender/display_list.h"
#include "shared/protocol.h"
#include <memory>
#include <expected>
#include <system_error>

namespace fbx::xrender {

class Entity {
public:
    virtual ~Entity() = default;

    virtual std::expected<void, std::error_code> record(DisplayList& list) const noexcept = 0;
};

class RectEntity : public Entity {
public:
    RectEntity(float x, float y, float w, float h, float r, float g, float b, float a) noexcept;
    
    std::expected<void, std::error_code> record(DisplayList& list) const noexcept override;

private:
    fbx::shared::RectData data_;
};

} // namespace fbx::xrender
