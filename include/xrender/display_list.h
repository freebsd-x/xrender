#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <expected>
#include <system_error>
#include "shared/protocol.h"

namespace fbx::xrender {

// The serialized Display List
class DisplayList {
public:
    DisplayList() = default;
    ~DisplayList() = default;

    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;

    DisplayList(DisplayList&&) noexcept = default;
    DisplayList& operator=(DisplayList&&) noexcept = default;

    std::expected<void, std::error_code> addCommand(fbx::shared::CommandType type, std::span<const uint8_t> data) noexcept;
    
    [[nodiscard]] std::span<const fbx::shared::DrawCommand> getCommands() const noexcept;
    [[nodiscard]] std::span<const uint8_t> getDataBuffer() const noexcept;

    // Number of commands recorded so far — capture before painting a subtree to
    // pair with scaleAlphaFrom() for group opacity.
    [[nodiscard]] size_t commandCount() const noexcept { return commands_.size(); }

    // Multiply the alpha of every colour-bearing command at index >= from_index
    // by `factor` (clamped to [0,1] effect). Used to dim a subtree — e.g. a
    // disabled control — after it has painted itself and its children.
    void scaleAlphaFrom(size_t from_index, float factor) noexcept;

    void clear() noexcept;

private:
    std::vector<fbx::shared::DrawCommand> commands_;
    std::vector<uint8_t> data_buffer_;
};

} // namespace fbx::xrender
