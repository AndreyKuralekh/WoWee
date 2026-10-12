// The renderer's half of a stereo frame: the two eye-sized targets, the
// per-frame data of each eye, and the pass that draws the world into them.
//
// These are members of rendering::Renderer, because drawing the world means
// calling Renderer::renderWorld with the renderer's own state pointed
// elsewhere. They live here so that renderer.cpp carries only the calls.

#include "rendering/renderer.hpp"

#include "core/logger.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"
#include "xr/xr_system.hpp"

#include <cmath>
#include <cstring>
#include <utility>

namespace wowee::rendering {

struct Renderer::VrEyes {
    static constexpr uint32_t kEyeCount = 2;

    VkExtent2D extent{};
    AllocatedImage color[kEyeCount];
    AllocatedImage depth[kEyeCount];
    VkFramebuffer framebuffer[kEyeCount] = {};

    // What set 0 is for an eye: the frame's own per-frame data with that eye's
    // matrices. The buffer is read when the draw executes, not when it is
    // recorded, so each eye of each frame slot needs one of its own.
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkBuffer ubo[kEyeCount][MAX_FRAMES] = {};
    VmaAllocation uboAllocation[kEyeCount][MAX_FRAMES] = {};
    void* uboMapped[kEyeCount][MAX_FRAMES] = {};
    VkDescriptorSet set[kEyeCount][MAX_FRAMES] = {};

    // The worker threads' command buffers, once more for each eye. The
    // renderer has one set per frame slot, which is enough to record the world
    // once; these are put in their place while an eye is recorded, so the
    // world goes down the same threaded path twice without knowing.
    VkCommandBuffer secondary[kEyeCount][NUM_SECONDARIES][MAX_FRAMES] = {};
    VkCommandPool secondaryPoolSeen = VK_NULL_HANDLE;  // which pools they came from

    xr::StereoViews views;
    bool viewsValid = false;
    bool refusedLogged = false;
};

void Renderer::vrDestroyEyes() {
    if (!vrEyes_) return;
    if (vkCtx) {
        const VkDevice device = vkCtx->getDevice();
        const VmaAllocator allocator = vkCtx->getAllocator();
        vkDeviceWaitIdle(device);
        for (uint32_t eye = 0; eye < VrEyes::kEyeCount; ++eye) {
            if (vrEyes_->framebuffer[eye]) vkDestroyFramebuffer(device, vrEyes_->framebuffer[eye], nullptr);
            destroyImage(device, allocator, vrEyes_->color[eye]);
            destroyImage(device, allocator, vrEyes_->depth[eye]);
            for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
                if (vrEyes_->ubo[eye][i]) {
                    vmaDestroyBuffer(allocator, vrEyes_->ubo[eye][i], vrEyes_->uboAllocation[eye][i]);
                }
            }
        }
        if (vrEyes_->pool) vkDestroyDescriptorPool(device, vrEyes_->pool, nullptr);
    }
    delete vrEyes_;
    vrEyes_ = nullptr;
}

bool Renderer::vrEnsureEyes() {
    const VkExtent2D wanted{xr::system().recommendedEyeWidth(), xr::system().recommendedEyeHeight()};
    if (vrEyes_ && vrEyes_->extent.width == wanted.width && vrEyes_->extent.height == wanted.height) return true;
    vrDestroyEyes();
    if (wanted.width == 0 || wanted.height == 0) return false;

    const VkDevice device = vkCtx->getDevice();
    const VmaAllocator allocator = vkCtx->getAllocator();
    vrEyes_ = new VrEyes();
    vrEyes_->extent = wanted;

    for (uint32_t eye = 0; eye < VrEyes::kEyeCount; ++eye) {
        // The window's own formats on the scene's own pass, which is what
        // every pipeline that draws the world was built against.
        vrEyes_->color[eye] = createImage(device, allocator, wanted.width, wanted.height,
                                          vkCtx->getSwapchainFormat(),
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT);
        vrEyes_->depth[eye] = createImage(device, allocator, wanted.width, wanted.height, vkCtx->getDepthFormat(),
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!vrEyes_->color[eye].imageView || !vrEyes_->depth[eye].imageView) {
            vrDestroyEyes();
            return false;
        }
        const VkImageView attachments[2] = {vrEyes_->color[eye].imageView, vrEyes_->depth[eye].imageView};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vkCtx->getImGuiRenderPass();
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = wanted.width;
        fbInfo.height = wanted.height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &vrEyes_->framebuffer[eye]) != VK_SUCCESS) {
            vrDestroyEyes();
            return false;
        }
    }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = VrEyes::kEyeCount * MAX_FRAMES;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = VrEyes::kEyeCount * MAX_FRAMES * 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = VrEyes::kEyeCount * MAX_FRAMES;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &vrEyes_->pool) != VK_SUCCESS) {
        vrDestroyEyes();
        return false;
    }

    for (uint32_t eye = 0; eye < VrEyes::kEyeCount; ++eye) {
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = sizeof(GPUPerFrameData);
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapInfo{};
            if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &vrEyes_->ubo[eye][i],
                                &vrEyes_->uboAllocation[eye][i], &mapInfo) != VK_SUCCESS) {
                vrDestroyEyes();
                return false;
            }
            vrEyes_->uboMapped[eye][i] = mapInfo.pMappedData;

            VkDescriptorSetAllocateInfo setInfo{};
            setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            setInfo.descriptorPool = vrEyes_->pool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts = &perFrameSetLayout;
            if (vkAllocateDescriptorSets(device, &setInfo, &vrEyes_->set[eye][i]) != VK_SUCCESS) {
                vrDestroyEyes();
                return false;
            }
        }
    }
    LOG_WARNING("VR: eye targets ", wanted.width, "x", wanted.height);
    return true;
}

// A set of secondary command buffers for each eye, from the pools the
// renderer's own come from and in the same arrangement: a pool is recorded
// from one thread at a time, and that is still true, because the eyes are
// recorded one after the other. False leaves the eyes to be recorded inline.
bool Renderer::vrEnsureEyeSecondaries() {
    // The renderer can rebuild its pools; buffers from pools that are gone
    // went with them.
    if (vrEyes_->secondaryPoolSeen == workerCmdPools_[0] && vrEyes_->secondaryPoolSeen != VK_NULL_HANDLE) return true;
    std::memset(vrEyes_->secondary, 0, sizeof(vrEyes_->secondary));
    vrEyes_->secondaryPoolSeen = VK_NULL_HANDLE;
    if (workerCmdPools_[0] == VK_NULL_HANDLE || mainSecondaryCmdPool_ == VK_NULL_HANDLE) return false;

    const VkDevice device = vkCtx->getDevice();
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    // As in Renderer::createSecondaryCommandResources.
    const uint32_t workerSecondaries[NUM_WORKERS] = {SEC_TERRAIN, SEC_WMO, SEC_CHARS, SEC_M2, SEC_POST};
    const uint32_t mainSecondaries[] = {SEC_SKY, SEC_SELECTION, SEC_IMGUI};
    for (uint32_t eye = 0; eye < VrEyes::kEyeCount; ++eye) {
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
                allocInfo.commandPool = workerCmdPools_[w];
                if (vkAllocateCommandBuffers(device, &allocInfo, &vrEyes_->secondary[eye][workerSecondaries[w]][f]) !=
                    VK_SUCCESS) {
                    return false;
                }
            }
            for (const uint32_t index : mainSecondaries) {
                allocInfo.commandPool = mainSecondaryCmdPool_;
                if (vkAllocateCommandBuffers(device, &allocInfo, &vrEyes_->secondary[eye][index][f]) != VK_SUCCESS) {
                    return false;
                }
            }
        }
    }
    vrEyes_->secondaryPoolSeen = workerCmdPools_[0];
    LOG_WARNING("VR: the eyes are recorded on the worker threads");
    return true;
}

// Called once the frame's command buffer exists and the headset has said where
// the eyes are. From here until the eyes are drawn the camera is the point
// between them, so the frame's culling and its per-frame data are those of the
// head rather than of where the game left its camera.
void Renderer::vrBeginViews() {
    vrWorldInEyes_ = false;
    if (vrEyes_) vrEyes_->viewsValid = false;
    if (!vrGameHandler_ || !camera || currentCmd == VK_NULL_HANDLE || !xr::system().stereoFrame()) return;

    if (vkCtx->getMsaaSamples() != VK_SAMPLE_COUNT_1_BIT) {
        // The eye targets are single sampled, and so must the pass be.
        static bool said = false;
        if (!said) LOG_WARNING("VR: stereo needs antialiasing off - showing the window instead");
        said = true;
        return;
    }
    if (!vrEnsureEyes()) {
        if (!vrEyes_ || !vrEyes_->refusedLogged) LOG_WARNING("VR: could not create the eye targets");
        if (vrEyes_) vrEyes_->refusedLogged = true;
        return;
    }

    // Where the game put the camera, and which way it faces round the vertical.
    const glm::vec3 reference = camera->getPosition();
    const glm::vec3 forward = camera->getForward();
    const float yawDegrees = (std::abs(forward.x) + std::abs(forward.y) > 1e-4f)
                                 ? glm::degrees(std::atan2(forward.y, forward.x))
                                 : 0.0f;
    // Nearer than the flat client's half a yard: a head gets closer to things.
    constexpr float kNearPlane = 0.1f;
    if (!xr::system().stereoViews(reference, yawDegrees, kNearPlane, camera->getFarPlane(), vrEyes_->views)) return;
    vrEyes_->viewsValid = true;

    const xr::EyeView& center = vrEyes_->views.center;
    camera->setExternalView(center.view, center.projection, center.position);
}

// The world, once per eye, before the window's own pass opens.
void Renderer::vrRenderEyes() {
    if (!vrEyes_ || !vrEyes_->viewsValid) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    const VkDevice device = vkCtx->getDevice();

    const bool wasParallel = parallelRecordingEnabled_;
    const VkRenderPass savedPass = activeRenderPass_;
    const VkFramebuffer savedFramebuffer = activeFramebuffer_;
    const VkExtent2D savedExtent = activeRenderExtent_;
    vrEyePass_ = true;

    // Threaded when the renderer is, and the eyes have buffers of their own
    // for it; otherwise straight into the frame's command buffer.
    const bool threaded = wasParallel && vrEnsureEyeSecondaries();
    parallelRecordingEnabled_ = threaded;

    for (uint32_t eye = 0; eye < VrEyes::kEyeCount; ++eye) {
        const xr::EyeView& view = vrEyes_->views.eye[eye];
        // The camera stays the point between the eyes for both of them. The
        // renderers fill buffers as they record - instances, bones, particles -
        // and those are read when the frame executes, after both eyes have been
        // recorded: an eye that culled for itself left the other drawing from
        // lists that were no longer its own. So both record the same thing, and
        // what differs is only the matrices in set 0.
        vrEyeView_ = &view.view;
        vrEyeProjection_ = &view.projection;

        GPUPerFrameData data = currentFrameData;
        data.view = view.view;
        data.projection = view.projection;
        data.viewPos = glm::vec4(view.position, 1.0f);
        // Both are built for one camera, the window's.
        data.volumetricParams = glm::vec4(0.0f);
        data.rtParams = glm::vec4(0.0f);
        std::memcpy(vrEyes_->uboMapped[eye][frame], &data, sizeof(GPUPerFrameData));

        // Everything but the buffer is the frame's own, copied each frame: the
        // renderer rewrites those bindings when a setting changes them.
        const VkDescriptorSet set = vrEyes_->set[eye][frame];
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = vrEyes_->ubo[eye][frame];
        bufferInfo.range = sizeof(GPUPerFrameData);
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        VkCopyDescriptorSet copies[4]{};
        for (uint32_t binding = 1; binding <= 4; ++binding) {
            VkCopyDescriptorSet& copy = copies[binding - 1];
            copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            copy.srcSet = perFrameDescSets[frame];
            copy.srcBinding = binding;
            copy.dstSet = set;
            copy.dstBinding = binding;
            copy.descriptorCount = 1;
        }
        vkUpdateDescriptorSets(device, 1, &write, 4, copies);

        VkRenderPassBeginInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        passInfo.renderPass = vkCtx->getImGuiRenderPass();
        passInfo.framebuffer = vrEyes_->framebuffer[eye];
        passInfo.renderArea.extent = vrEyes_->extent;
        VkClearValue clearValues[2]{};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {.depth = 1.0f, .stencil = 0};
        passInfo.clearValueCount = 2;
        passInfo.pClearValues = clearValues;

        activeRenderPass_ = passInfo.renderPass;
        activeFramebuffer_ = passInfo.framebuffer;
        activeRenderExtent_ = vrEyes_->extent;
        vrPerFrameSet_ = set;

        const auto swapSecondaries = [&] {
            for (uint32_t i = 0; i < NUM_SECONDARIES; ++i) {
                std::swap(secondaryCmds_[i][frame], vrEyes_->secondary[eye][i][frame]);
            }
        };
        if (threaded) {
            swapSecondaries();
            vkCmdBeginRenderPass(currentCmd, &passInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        } else {
            vkCmdBeginRenderPass(currentCmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{};
            viewport.width = static_cast<float>(vrEyes_->extent.width);
            viewport.height = static_cast<float>(vrEyes_->extent.height);
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.extent = vrEyes_->extent;
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);
        }

        renderWorld(nullptr, vrGameHandler_);

        vkCmdEndRenderPass(currentCmd);
        if (threaded) swapSecondaries();
        xr::system().recordEyeCopy(currentCmd, eye, vrEyes_->color[eye].image);
    }

    vrPerFrameSet_ = VK_NULL_HANDLE;
    vrEyeView_ = nullptr;
    vrEyeProjection_ = nullptr;
    vrEyePass_ = false;
    parallelRecordingEnabled_ = wasParallel;
    activeRenderPass_ = savedPass;
    activeFramebuffer_ = savedFramebuffer;
    activeRenderExtent_ = savedExtent;

    // The game's own camera again, for everything that follows: the window's
    // pass, the interface that projects into it, and the next update.
    camera->clearExternalView();
    vrWorldInEyes_ = true;
}

}  // namespace wowee::rendering

namespace wowee::rendering {

// The window's pass in a frame whose world went to the eyes: of everything
// renderWorld records, only the minimap is the interface's, and so the
// window's. It is drawn where renderWorld draws it - in the continuation of
// the scene pass when there is one, which is the pass its pipeline was built
// against - and that pass is left open, as renderWorld leaves it.
void Renderer::vrRenderWindowMinimap(game::GameHandler* gameHandler) {
    if (minimapDrawsWithWater_ && vkCtx->getSceneContinueRenderPass() != VK_NULL_HANDLE) {
        vkCmdEndRenderPass(currentCmd);

        VkRenderPassBeginInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        passInfo.renderPass = vkCtx->getSceneContinueRenderPass();
        passInfo.framebuffer = activeFramebuffer_;
        passInfo.renderArea.extent = activeRenderExtent_;
        vkCmdBeginRenderPass(currentCmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(activeRenderExtent_.width);
        viewport.height = static_cast<float>(activeRenderExtent_.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.extent = activeRenderExtent_;
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    }
    renderMinimapOverlay(currentCmd, gameHandler);
}

}  // namespace wowee::rendering
