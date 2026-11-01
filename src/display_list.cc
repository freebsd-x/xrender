#include "xrender/display_list.h"
#include <cstring>
#include <iostream>

namespace fbx::xrender {

std::expected<void, std::error_code> DisplayList::addCommand(fbx::shared::CommandType type, std::span<const uint8_t> data) noexcept {
    try {
        // Start every record on an 8-byte boundary: consumers read the
        // RectData/TextData/ImageData (which holds a pointer) in place, and a
        // record after a DrawText's variable-length string otherwise landed at
        // an odd offset — misaligned loads (UB; flagged by UBSan in every app).
        constexpr size_t kAlign = 8;
        uint32_t offset = static_cast<uint32_t>((data_buffer_.size() + kAlign - 1) & ~(kAlign - 1));

        if (!data.empty()) {
            data_buffer_.resize(offset + data.size());
            std::memcpy(data_buffer_.data() + offset, data.data(), data.size());
        }
        
        commands_.push_back({type, offset, static_cast<uint32_t>(data.size())});
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

std::span<const fbx::shared::DrawCommand> DisplayList::getCommands() const noexcept {
    return commands_;
}

std::span<const uint8_t> DisplayList::getDataBuffer() const noexcept {
    return data_buffer_;
}

void DisplayList::scaleAlphaFrom(size_t from_index, float factor) noexcept {
    for (size_t i = from_index; i < commands_.size(); ++i) {
        const auto& cmd = commands_[i];
        if (cmd.data_size_ == 0) continue;
        uint8_t* p = data_buffer_.data() + cmd.data_offset_;
        switch (cmd.type_) {
            case fbx::shared::CommandType::DrawRect: {
                auto* r = reinterpret_cast<fbx::shared::RectData*>(p);
                r->a_ *= factor;
                r->shadow_alpha_ *= factor;
                break;
            }
            case fbx::shared::CommandType::DrawText: {
                reinterpret_cast<fbx::shared::TextData*>(p)->a_ *= factor;
                break;
            }
            case fbx::shared::CommandType::DrawPath: {
                reinterpret_cast<fbx::shared::PathData*>(p)->a_ *= factor;
                break;
            }
            default: break;  // images / transforms / scissors carry no alpha to scale
        }
    }
}

void DisplayList::clear() noexcept {
    commands_.clear();
    data_buffer_.clear();
}

} // namespace fbx::xrender
