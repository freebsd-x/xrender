#include "xrender/vulkan_context.h"
#include "xrender/memory_allocator.h"
#include "xrender/display_list.h"
#include "xrender/entity.h"
#include "xrender/typography_backend.h"
#include "xserver/ipc_socket.h"
#include "xserver/event_loop.h"
#include "shared/protocol.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <csignal>
#include <functional>

using namespace fbx::xrender;
using namespace fbx::xserver;
using namespace fbx::shared;

namespace {
    EventLoop* g_loop = nullptr;
    void handle_signal(int sig) {
        if (g_loop) {
            std::cout << "\n[xrender] Signal " << sig << " received, stopping...\n";
            g_loop->stop();
        }
    }

    std::expected<VkShaderModule, VulkanError> createShaderModule(VkDevice device, const std::string& filename) noexcept {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) return std::unexpected(VulkanError::ShaderCompilationError);
        
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = buffer.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            return std::unexpected(VulkanError::ShaderCompilationError);
        }
        return shaderModule;
    }
}

int main() {
    try {
        std::cout << "Starting xrender (Phase 10: Grand Unification) [C++23 Strict Mode]...\n";
        
        auto loopRes = EventLoop::create();
        if (!loopRes) {
            std::cerr << "Fatal Error: Failed to create EventLoop\n";
            return 1;
        }
        auto loop = std::move(loopRes.value());
        g_loop = loop.get();

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        auto contextRes = VulkanContext::create();
        if (!contextRes.has_value()) {
            std::cerr << "Fatal Error: Failed to initialize Vulkan Context.\n";
            return 1;
        }
        auto vkContext = std::move(contextRes.value());
        VkDevice device = vkContext->getDevice();
        uint32_t width = 1920;
        uint32_t height = 1080;

        std::cout << "\nInitializing Typography Backend (SDF Font Atlas)...\n";
        auto typoRes = TypographyBackend::create(*vkContext, "/usr/local/share/fonts/dejavu/DejaVuSans.ttf");
        if (!typoRes) {
            std::cerr << "Warning: Failed to load font for typography backend.\n";
        }
        std::unique_ptr<TypographyBackend> typography = typoRes ? std::move(typoRes.value()) : nullptr;

        // Create Exportable Image
        VkExternalMemoryImageCreateInfo extImageInfo{};
        extImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext = &extImageInfo;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VkImage image;
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            std::cerr << "Failed to create image\n"; return 1;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, image, &memReqs);

        auto memType = vkContext->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memType.has_value()) {
            std::cerr << "Failed to find memory type\n"; return 1;
        }

        VkExportMemoryAllocateInfo exportAllocInfo{};
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &exportAllocInfo;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memType.value();

        VkDeviceMemory imageMemory;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate image memory\n"; return 1;
        }
        vkBindImageMemory(device, image, imageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imageView;
        if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
            std::cerr << "Failed to create image view\n"; return 1;
        }

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        VkRenderPass renderPass;
        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) return 1;

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &imageView;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        VkFramebuffer framebuffer;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) return 1;

        auto vertModuleRes = createShaderModule(device, "/home/wheel/freebsd-x/xrender/build/xrender.p/triangle.vert.spv");
        auto fragModuleRes = createShaderModule(device, "/home/wheel/freebsd-x/xrender/build/xrender.p/triangle.frag.spv");
        if (!vertModuleRes || !fragModuleRes) {
            std::cerr << "[xrender] Failed to load shaders! Check file paths.\n";
            return 1;
        }
        VkShaderModule vertModule = vertModuleRes.value();
        VkShaderModule fragModule = fragModuleRes.value();

        VkPipelineShaderStageCreateInfo vertStageInfo{};
        vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStageInfo.module = vertModule;
        vertStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragStageInfo{};
        fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStageInfo.module = fragModule;
        fragStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertStageInfo, fragStageInfo};
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {width, height}};
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkDescriptorSetLayout layouts[] = {typography ? typography->getDescriptorSetLayout() : VK_NULL_HANDLE};
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(float) * 4;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = typography ? 1 : 0;
        pipelineLayoutInfo.pSetLayouts = typography ? layouts : nullptr;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        VkPipelineLayout pipelineLayout;
        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) return 1;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;

        VkPipeline graphicsPipeline;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) return 1;

        VkExportSemaphoreCreateInfo exportSemaphoreInfo{};
        exportSemaphoreInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportSemaphoreInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphoreInfo.pNext = &exportSemaphoreInfo;

        VkSemaphore renderFinishedSemaphore;
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphore);

        // Setup IPC Server Socket to receive DisplayLists from xkit
        UniqueFd sFd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!sFd) {
            std::cerr << "Fatal Error: Failed to create Render IPC socket\n";
            return 1;
        }
        
        struct sockaddr_un serverAddr{};
        serverAddr.sun_family = AF_UNIX;
        strncpy(serverAddr.sun_path, "/tmp/freebsd-x-render.sock", sizeof(serverAddr.sun_path) - 1);
        unlink("/tmp/freebsd-x-render.sock");

        if (bind(sFd.get(), (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("[xrender] bind failed");
            return 1;
        }
        if (listen(sFd.get(), 5) < 0) {
            perror("[xrender] listen failed");
            return 1;
        }

        std::cout << "[xrender] Listening for UI clients on Render Routing socket: /tmp/freebsd-x-render.sock\n";

        // Create IPC Client to send buffers to xserver
        int clientSocket = socket(AF_UNIX, SOCK_STREAM, 0);
        UniqueFd clientFd(clientSocket);
        IpcSocket xserverClient(std::move(clientFd));

        struct sockaddr_un xserverAddr{};
        xserverAddr.sun_family = AF_UNIX;
        strncpy(xserverAddr.sun_path, "/tmp/freebsd-x.sock", sizeof(xserverAddr.sun_path) - 1);

        if (connect(clientSocket, (struct sockaddr*)&xserverAddr, sizeof(xserverAddr)) != 0) {
            std::cerr << "[xrender] Failed to connect to xserver socket. Make sure it's running.\n";
            return 1;
        }
        std::cout << "[xrender] Connected to xserver.\n";

        // Accept UI Clients
        std::shared_ptr<IpcSocket> activeClient = nullptr;
        (void)loop->addFd(sFd.get(), [sFd = sFd.get(), &activeClient, &loop, device, width, height,
                                      &vkContext, framebuffer, renderPass, pipelineLayout, graphicsPipeline,
                                      &typography, renderFinishedSemaphore, imageMemory, &xserverClient]() {
            struct sockaddr_un clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientRawFd = accept4(sFd, (struct sockaddr*)&clientAddr, &clientLen, SOCK_CLOEXEC);
            
            if (clientRawFd >= 0) {
                std::cout << "[xrender] Accepted xkit client connection.\n";
                activeClient = std::make_shared<IpcSocket>(UniqueFd(clientRawFd));
                
                (void)loop->addFd(clientRawFd, [activeClient, &vkContext, device, width, height, framebuffer, 
                                                renderPass, pipelineLayout, graphicsPipeline, &typography,
                                                renderFinishedSemaphore, imageMemory, &xserverClient, &loop]() {
                    auto msgRes = activeClient->receiveMessage();
                    if (!msgRes) {
                        std::cerr << "[xrender] Client disconnected.\n";
                        return;
                    }

                    auto& data = msgRes.value().first;
                    std::cout << "[xrender] Received " << data.size() << " bytes of DisplayList from xkit.\n";

                    if (data.empty()) return;

                    // Parse the DisplayList bytes and record commands
                    // For brevity in the prototype, we simply assume the payload indicates a frame ready to render.
                    // A full implementation would parse `DrawCommand` array from the `data` span.
                    
                    VkCommandBufferAllocateInfo cmdAllocInfo{};
                    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    cmdAllocInfo.commandPool = vkContext->getCommandPool();
                    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cmdAllocInfo.commandBufferCount = 1;
                    VkCommandBuffer cmdBuffer;
                    if (vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuffer) != VK_SUCCESS) return;

                    VkCommandBufferBeginInfo beginInfo{};
                    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

                    VkRenderPassBeginInfo renderPassBeginInfo{};
                    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    renderPassBeginInfo.renderPass = renderPass;
                    renderPassBeginInfo.framebuffer = framebuffer;
                    renderPassBeginInfo.renderArea.extent = {width, height};
                    VkClearValue clearColor = {{{0.1f, 0.2f, 0.3f, 1.0f}}};
                    renderPassBeginInfo.clearValueCount = 1;
                    renderPassBeginInfo.pClearValues = &clearColor;

                    vkCmdBeginRenderPass(cmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
                    
                    if (typography) {
                        VkDescriptorSet descriptorSet = typography->getDescriptorSet();
                        vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
                    }

                    // Mock drawing the DisplayList
                    std::cout << "[xrender] Translating DisplayList and recording draw calls...\n";
                    vkCmdDraw(cmdBuffer, 3, 1, 0, 0); 
                    
                    vkCmdEndRenderPass(cmdBuffer);
                    vkEndCommandBuffer(cmdBuffer);

                    // Submit to GPU
                    VkSubmitInfo submitInfo{};
                    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    submitInfo.commandBufferCount = 1;
                    submitInfo.pCommandBuffers = &cmdBuffer;
                    
                    // Hardware fallback for Semaphore extension missing
                    auto syncFdRes = vkContext->exportSemaphoreFd(renderFinishedSemaphore);
                    int syncFdVal = -1;

                    if (!syncFdRes) {
                        vkQueueWaitIdle(vkContext->getGraphicsQueue());
                    } else {
                        submitInfo.signalSemaphoreCount = 1;
                        submitInfo.pSignalSemaphores = &renderFinishedSemaphore;
                        syncFdVal = syncFdRes.value().release();
                    }

                    vkQueueSubmit(vkContext->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);

                    // Export memory
                    auto dmaFdRes = vkContext->exportMemoryFd(imageMemory);
                    if (dmaFdRes) {
                        int dmaFdVal = dmaFdRes.value().release();
                        
                        RenderMessage renderMsg{
                            .surface_id_ = 1,
                            .status_ = RenderMessageStatus::FrameReady,
                            .width_ = 1920,
                            .height_ = 1080,
                            .damage_x_ = 0.0f,
                            .damage_y_ = 0.0f,
                            .damage_width_ = 1920.0f, // Full screen for first frame
                            .damage_height_ = 1080.0f
                        };
                        
                        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&renderMsg), sizeof(RenderMessage));
                        
                        std::cout << "[xrender] Sending Rendered FDs to xserver (Surface ID 1)...\n";
                        if (syncFdVal >= 0) {
                            xserverClient.sendMessage(payload, dmaFdVal);
                            xserverClient.sendMessage(payload, syncFdVal);
                            close(syncFdVal);
                        } else {
                            xserverClient.sendMessage(payload, dmaFdVal);
                            xserverClient.sendMessage(payload, -1);
                        }
                        close(dmaFdVal);
                    }
                    
                    // Note: Memory leak of cmdBuffer for simplicity of prototype lambda
                });
            }
        });

        loop->run();

        // Cleanup
        unlink("/tmp/freebsd-x-render.sock");
        vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
