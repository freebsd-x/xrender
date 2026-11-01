#include "xrender/typography_backend.h"
#include "xrender/text_shaper.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace fbx::xrender {

// ── Factory ───────────────────────────────────────────────────────────────────

std::expected<std::unique_ptr<TypographyBackend>, VulkanError>
TypographyBackend::create(const VulkanContext& ctx,
                           const std::string&   shader_dir) noexcept {
    auto backend = std::unique_ptr<TypographyBackend>(new TypographyBackend(ctx));
    if (auto r = backend->init(shader_dir); !r) return std::unexpected(r.error());
    return backend;
}

TypographyBackend::TypographyBackend(const VulkanContext& ctx) noexcept
    : ctx_(ctx) {}

TypographyBackend::~TypographyBackend() noexcept {
    VkDevice dev = ctx_.getDevice();
    if (vtx_buf_)        vkDestroyBuffer(dev, vtx_buf_, nullptr);
    if (vtx_mem_)        vkFreeMemory(dev, vtx_mem_, nullptr);
    if (pipeline_)       vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipeline_layout_)vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
    if (desc_pool_)      vkDestroyDescriptorPool(dev, desc_pool_, nullptr);
    if (desc_layout_)    vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
    if (sampler_)        vkDestroySampler(dev, sampler_, nullptr);
    page_textures_.clear();
    cache_.reset();
}

// ── init ──────────────────────────────────────────────────────────────────────

std::expected<VkShaderModule, VulkanError>
TypographyBackend::loadShader(const std::string& path) noexcept {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) return std::unexpected(VulkanError::ShaderCompilationError);
    auto sz = static_cast<std::size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(buf.data());

    VkShaderModule mod;
    if (vkCreateShaderModule(ctx_.getDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        return std::unexpected(VulkanError::ShaderCompilationError);
    return mod;
}

std::expected<void, VulkanError>
TypographyBackend::init(const std::string& shader_dir) noexcept {
    VkDevice dev = ctx_.getDevice();

    // 1. Glyph cache (CPU).
    cache_ = GlyphCache::create();
    std::cout << "[xrender] TypographyBackend: GlyphCache created (2048×2048 per page).\n";

    // 2. Sampler for atlas pages.
    VkSamplerCreateInfo sci{};
    sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter     = VK_FILTER_LINEAR;
    sci.minFilter     = VK_FILTER_LINEAR;
    sci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler_) != VK_SUCCESS)
        return std::unexpected(VulkanError::LogicalDeviceCreationError);

    // 3. Descriptor set layout (one combined image sampler at binding 0).
    VkDescriptorSetLayoutBinding bind{};
    bind.binding         = 0;
    bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings    = &bind;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &desc_layout_) != VK_SUCCESS)
        return std::unexpected(VulkanError::LogicalDeviceCreationError);

    // 4. Descriptor pool — start with capacity for 8 pages.
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 8;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &desc_pool_) != VK_SUCCESS)
        return std::unexpected(VulkanError::LogicalDeviceCreationError);

    // 5. Pipeline layout (no push constants — colour is per-vertex).
    VkPipelineLayoutCreateInfo plci{};
    plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &desc_layout_;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return std::unexpected(VulkanError::LogicalDeviceCreationError);

    // 6. SDF text graphics pipeline.
    auto vert_res = loadShader(shader_dir + "/sdf_text.vert.spv");
    auto frag_res = loadShader(shader_dir + "/sdf_text.frag.spv");
    if (!vert_res || !frag_res)
        return std::unexpected(VulkanError::ShaderCompilationError);

    VkShaderModule vert_mod = vert_res.value();
    VkShaderModule frag_mod = frag_res.value();

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName  = "main";

    // GlyphVertex = {x,y, u,v, r,g,b,a} = 8 floats = 32 bytes
    VkVertexInputBindingDescription vbd{};
    vbd.binding   = 0;
    vbd.stride    = sizeof(GlyphVertex);
    vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vads[3] = {};
    vads[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GlyphVertex, x) };
    vads[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GlyphVertex, u) };
    vads[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlyphVertex, r) };

    VkPipelineVertexInputStateCreateInfo visci{};
    visci.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    visci.vertexBindingDescriptionCount   = 1;
    visci.pVertexBindingDescriptions      = &vbd;
    visci.vertexAttributeDescriptionCount = 3;
    visci.pVertexAttributeDescriptions    = vads;

    VkPipelineInputAssemblyStateCreateInfo iasci{};
    iasci.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport / scissor so the pipeline is screen-size agnostic.
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynci{};
    dynci.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynci.dynamicStateCount = 2;
    dynci.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo vpci{};
    vpci.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpci.viewportCount = 1;
    vpci.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rci{};
    rci.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rci.polygonMode = VK_POLYGON_MODE_FILL;
    rci.lineWidth   = 1.0f;
    rci.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo msci{};
    msci.sType               = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Alpha blending: outColor = srcAlpha * src + (1-srcAlpha) * dst
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;   // "over": keep the surface opaque
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbci{};
    cbci.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbci.attachmentCount = 1;
    cbci.pAttachments    = &cba;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState      = &vpci;
    gpci.pRasterizationState = &rci;
    gpci.pMultisampleState   = &msci;
    gpci.pColorBlendState    = &cbci;
    gpci.pDynamicState       = &dynci;
    gpci.layout              = pipeline_layout_;
    // Create a minimal render pass compatible with B8G8R8A8_UNORM output (the
    // standard offscreen surface format used throughout the xrender pipeline).
    VkRenderPass compat_rp = VK_NULL_HANDLE;
    {
        VkAttachmentDescription ad{};
        ad.format         = VK_FORMAT_B8G8R8A8_UNORM;
        ad.samples        = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        ad.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        ad.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ad.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription  sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &ar;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            nullptr, 0, 1, &ad, 1, &sp, 0, nullptr};
        vkCreateRenderPass(dev, &rpci, nullptr, &compat_rp);
    }
    gpci.renderPass = compat_rp;

    VkResult pres = vkCreateGraphicsPipelines(
        dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_);

    vkDestroyShaderModule(dev, vert_mod, nullptr);
    vkDestroyShaderModule(dev, frag_mod, nullptr);
    if (compat_rp) vkDestroyRenderPass(dev, compat_rp, nullptr); // only needed for pipeline creation

    if (pres != VK_SUCCESS)
        return std::unexpected(VulkanError::LogicalDeviceCreationError);

    // 7. Warm up the default UI face so measureText works immediately.
    FontManager::init();
    FontFace* def_face = FontManager::getFace();
    if (def_face)
        std::cout << "[xrender] TypographyBackend: default UI face loaded (face_id="
                  << def_face->face_id_ << ").\n";

    std::cout << "[xrender] TypographyBackend: SDF text pipeline ready.\n";
    return {};
}

// ── Atlas page sync ───────────────────────────────────────────────────────────

void TypographyBackend::syncAtlasTextures() noexcept {
    if (!cache_->isDirty()) return;

    VkDevice dev = ctx_.getDevice();
    uint32_t np  = cache_->getPageCount();

    // Extend page_textures_ and desc_sets_ for any new pages.
    while (page_textures_.size() < np) {
        uint32_t pi = static_cast<uint32_t>(page_textures_.size());

        auto tex_res = Texture::create(
            ctx_,
            GlyphCache::kPageSize, GlyphCache::kPageSize,
            VK_FORMAT_R8_UNORM);
        if (!tex_res) {
            std::cerr << "[xrender] TypographyBackend: failed to create page texture "
                      << pi << "\n";
            return;
        }
        page_textures_.push_back(std::move(tex_res.value()));

        // Allocate a descriptor set for this page.
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &desc_layout_;

        VkDescriptorSet ds = VK_NULL_HANDLE;
        vkAllocateDescriptorSets(dev, &ai, &ds);
        desc_sets_.push_back(ds);
    }

    // Upload each page that changed.
    for (uint32_t pi = 0; pi < np; ++pi) {
        auto& tex = page_textures_[pi];
        tex->upload(cache_->getPageBitmap(pi));

        // Bind to the descriptor set for this page.
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView   = tex->getImageView();
        ii.sampler     = sampler_;

        VkWriteDescriptorSet wds{};
        wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds.dstSet          = desc_sets_[pi];
        wds.dstBinding      = 0;
        wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.descriptorCount = 1;
        wds.pImageInfo      = &ii;
        vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);
    }

    cache_->clearDirty();
}

// ── Per-frame API ─────────────────────────────────────────────────────────────

void TypographyBackend::beginFrame() noexcept {
    batches_.clear();
}

void TypographyBackend::addText(const fbx::shared::TextData& td,
                                 const char*                  utf8_bytes,
                                 uint32_t                     screen_w,
                                 uint32_t                     screen_h) noexcept {
    if (!cache_ || td.string_length_ == 0) return;

    // Select the right FontFace.
    std::string family(td.font_family_); // may be empty → default
    FontWeight  weight = (td.font_weight_ != 0)
                         ? static_cast<FontWeight>(td.font_weight_)
                         : FontWeight::Regular;
    FontStyle   style  = static_cast<FontStyle>(td.font_style_);

    FontFace* face  = FontManager::getFace(family, weight, style);
    FontFace* emoji = FontManager::getEmojiFace();
    if (!face) return;

    // Shape the text.
    std::string text(utf8_bytes, td.string_length_);
    auto& shaper = TextShaper::getInstance();
    auto  glyphs = shaper.shapeText(text,
                                     face->hb_font_,  face->face_id_,
                                     emoji ? emoji->hb_font_ : nullptr,
                                     emoji ? emoji->face_id_ : 0u);

    // Ensure every glyph is in the cache (rasterizes if missing).
    for (const auto& sg : glyphs) {
        const GlyphMetric* m = cache_->lookup(sg.face_id_, sg.glyph_index_);
        if (!m) {
            FontFace* from = FontManager::faceForId(sg.face_id_, face);   // primary / emoji / last resort
            FT_Face ft = from ? from->ft_face_ : nullptr;
            if (ft) cache_->addGlyph(ft, sg.face_id_, sg.glyph_index_);
        }
    }

    // Convert screen pixel positions to NDC.
    float inv_w = 2.0f / static_cast<float>(screen_w);
    float inv_h = 2.0f / static_cast<float>(screen_h);
    float scale = td.font_size_ / GlyphCache::kRefSize;

    float pen_x = td.x_;
    float pen_y = td.y_;

    for (const auto& sg : glyphs) {
        const GlyphMetric* m = cache_->lookup(sg.face_id_, sg.glyph_index_);
        if (!m || (m->width_ == 0.0f && m->height_ == 0.0f)) {
            // Space or missing glyph — advance the pen only.
            pen_x += sg.x_advance_ * scale;
            continue;
        }

        float gx = pen_x + (m->bearing_x_ + sg.x_offset_) * scale;
        float gy = pen_y - (m->bearing_y_ + sg.y_offset_) * scale;
        float gw = m->width_  * scale;
        float gh = m->height_ * scale;

        // NDC coordinates (Y flipped: +Y is up in Vulkan clip space before flip).
        float x0 = gx            * inv_w - 1.0f;
        float x1 = (gx + gw)    * inv_w - 1.0f;
        float y0 = gy            * inv_h - 1.0f;
        float y1 = (gy + gh)    * inv_h - 1.0f;

        float u0 = m->atlas_u_,  u1 = m->atlas_u2_;
        float v0 = m->atlas_v_,  v1 = m->atlas_v2_;
        float r = td.r_, g = td.g_, b = td.b_, a = td.a_;

        // Find or create the page batch.
        PageBatch* batch = nullptr;
        for (auto& pb : batches_) {
            if (pb.page == m->page_) { batch = &pb; break; }
        }
        if (!batch) {
            batches_.push_back({m->page_, {}});
            batch = &batches_.back();
        }

        // Two triangles (6 vertices) for the glyph quad.
        batch->verts.insert(batch->verts.end(), {
            {x0,y0, u0,v0, r,g,b,a},
            {x1,y0, u1,v0, r,g,b,a},
            {x0,y1, u0,v1, r,g,b,a},
            {x1,y0, u1,v0, r,g,b,a},
            {x1,y1, u1,v1, r,g,b,a},
            {x0,y1, u0,v1, r,g,b,a},
        });

        pen_x += sg.x_advance_ * scale;
        pen_y += sg.y_advance_ * scale;
    }
}

bool TypographyBackend::ensureVertexBuffer() noexcept {
    // Count total vertices across all batches.
    VkDeviceSize needed = 0;
    for (const auto& b : batches_)
        needed += b.verts.size() * sizeof(GlyphVertex);
    if (needed == 0) return true;

    if (needed <= vtx_buf_capacity_) return true;

    VkDevice dev = ctx_.getDevice();

    // Destroy old buffer.
    if (vtx_buf_) { vkDestroyBuffer(dev, vtx_buf_, nullptr); vtx_buf_ = VK_NULL_HANDLE; }
    if (vtx_mem_) { vkFreeMemory(dev, vtx_mem_, nullptr);    vtx_mem_ = VK_NULL_HANDLE; }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = needed;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &vtx_buf_) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, vtx_buf_, &mr);

    auto mt = ctx_.findMemoryType(
        mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = mt.value();
    if (vkAllocateMemory(dev, &mai, nullptr, &vtx_mem_) != VK_SUCCESS) return false;

    vkBindBufferMemory(dev, vtx_buf_, vtx_mem_, 0);
    vtx_buf_capacity_ = needed;
    return true;
}

void TypographyBackend::flushAndRecord(VkCommandBuffer cmdBuffer,
                                        uint32_t        screen_w,
                                        uint32_t        screen_h) noexcept {
    // Sync any new atlas pages to GPU.
    syncAtlasTextures();

    if (batches_.empty() || pipeline_ == VK_NULL_HANDLE) return;

    // Upload vertices to GPU.
    if (!ensureVertexBuffer()) return;

    VkDevice dev = ctx_.getDevice();
    void* mapped = nullptr;
    vkMapMemory(dev, vtx_mem_, 0, vtx_buf_capacity_, 0, &mapped);
    char* dst = static_cast<char*>(mapped);
    for (const auto& b : batches_) {
        VkDeviceSize sz = b.verts.size() * sizeof(GlyphVertex);
        std::memcpy(dst, b.verts.data(), sz);
        dst += sz;
    }
    vkUnmapMemory(dev, vtx_mem_);

    // Set dynamic viewport & scissor.
    VkViewport vp{0.0f, 0.0f,
                  static_cast<float>(screen_w), static_cast<float>(screen_h),
                  0.0f, 1.0f};
    VkRect2D   sc{{0,0}, {screen_w, screen_h}};
    vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
    vkCmdSetScissor(cmdBuffer,  0, 1, &sc);

    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkDeviceSize vtx_offset = 0;
    for (const auto& b : batches_) {
        if (b.verts.empty()) continue;
        if (b.page >= desc_sets_.size()) continue; // page not yet synced

        vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pipeline_layout_, 0, 1,
                                 &desc_sets_[b.page], 0, nullptr);
        vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vtx_buf_, &vtx_offset);
        vkCmdDraw(cmdBuffer,
                  static_cast<uint32_t>(b.verts.size()), 1, 0, 0);

        vtx_offset += b.verts.size() * sizeof(GlyphVertex);
    }
}

} // namespace fbx::xrender
