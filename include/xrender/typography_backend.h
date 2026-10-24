#pragma once

#include <memory>
#include <string>
#include <vector>
#include <expected>

#include <vulkan/vulkan.h>

#include "xrender/font_atlas.h"     // GlyphCache / FontAtlas alias
#include "xrender/font_manager.h"   // FontFace, FontWeight, FontStyle
#include "xrender/texture.h"
#include "xrender/vulkan_context.h"
#include "shared/protocol.h"        // TextData

namespace fbx::xrender {

// Per-glyph vertex fed to sdf_text.vert.
// Layout must match the vertex input attributes in the SDF pipeline.
struct GlyphVertex {
    float x, y;       // NDC position [-1..1]
    float u, v;       // Atlas UV [0..1]
    float r, g, b, a; // RGBA colour [0..1]
};

// TypographyBackend owns:
//   • The dynamic GlyphCache (CPU-side SDF atlas)
//   • GPU atlas textures (one Texture per GlyphCache page)
//   • The Vulkan SDF-text pipeline (sdf_text.vert + sdf_text.frag)
//   • A per-frame vertex buffer for glyph quads
//
// Usage per frame:
//   backend->beginFrame();
//   for each DrawText command:
//       backend->addText(td, utf8_bytes, screen_w, screen_h);
//   backend->flushAndRecord(cmdBuffer);  // upload dirty pages + record draw calls
class TypographyBackend {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<TypographyBackend>, VulkanError>
    create(const VulkanContext& ctx,
           const std::string&   shader_dir) noexcept;

    ~TypographyBackend() noexcept;

    TypographyBackend(const TypographyBackend&) = delete;
    TypographyBackend& operator=(const TypographyBackend&) = delete;

    // ── Per-frame API ──────────────────────────────────────────────────────

    // Clear the frame's glyph quad accumulator.
    void beginFrame() noexcept;

    // Shape, rasterize, and accumulate quads for one DrawText command.
    // utf8_bytes must be td.string_length_ bytes long.
    void addText(const fbx::shared::TextData& td,
                 const char*                  utf8_bytes,
                 uint32_t                     screen_w,
                 uint32_t                     screen_h) noexcept;

    // Upload any dirty atlas pages to GPU, then record all glyph draw calls
    // into cmdBuffer.  Must be called inside an active render pass.
    void flushAndRecord(VkCommandBuffer cmdBuffer,
                        uint32_t        screen_w,
                        uint32_t        screen_h) noexcept;

    // ── Legacy / introspection ─────────────────────────────────────────────

    [[nodiscard]] const GlyphCache& getGlyphCache() const noexcept { return *cache_; }

private:
    explicit TypographyBackend(const VulkanContext& ctx) noexcept;

    std::expected<void, VulkanError>
    init(const std::string& shader_dir) noexcept;

    // Ensure page textures are created / re-uploaded when the cache is dirty.
    void syncAtlasTextures() noexcept;

    // Create (or recreate) the Vulkan vertex buffer to hold vtx_.
    bool ensureVertexBuffer() noexcept;

    // Helpers for building the SDF pipeline.
    std::expected<VkShaderModule, VulkanError>
    loadShader(const std::string& path) noexcept;

    const VulkanContext& ctx_;

    std::unique_ptr<GlyphCache> cache_;

    // One texture per atlas page.
    std::vector<std::unique_ptr<Texture>> page_textures_;

    // Per-page Vulkan descriptor resources (sampler + set per page).
    VkSampler             sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout desc_layout_{VK_NULL_HANDLE};
    VkDescriptorPool      desc_pool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> desc_sets_; // one per page

    // The SDF text Vulkan pipeline.
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline       pipeline_{VK_NULL_HANDLE};

    // Per-frame quad accumulator (split by page so each page gets one draw call).
    struct PageBatch {
        uint32_t              page;
        std::vector<GlyphVertex> verts; // 6 verts per glyph quad
    };
    std::vector<PageBatch> batches_;

    // GPU vertex buffer (re-created when it's too small).
    VkBuffer       vtx_buf_{VK_NULL_HANDLE};
    VkDeviceMemory vtx_mem_{VK_NULL_HANDLE};
    VkDeviceSize   vtx_buf_capacity_{0};
};

} // namespace fbx::xrender
