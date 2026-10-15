// The headset's glow pass. See xr/xr_glow.hpp.

#include "xr/xr_glow.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>

namespace wowee::xr {

namespace {

struct GlowPush {
    float texelX, texelY;
    float dirX, dirY;
    int32_t mode;
    float threshold;
    float strength;
    float pad;
};

// Where the glow starts. The day here is bright - sunlit stone sits near 0.8 -
// and a lower threshold glows the whole of it, which reads as fog rather than
// light.
constexpr float kThreshold = 0.6f;

VkRenderPass makePass(VkDevice device, VkFormat format, VkAttachmentLoadOp load,
                      VkImageLayout initial, VkImageLayout final) {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = load;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = initial;
    color.finalLayout = final;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    // What came before wrote or read the image; what comes after samples or
    // copies it. Both are covered by the barriers record() places, and these
    // order the pass against them.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

VkFramebuffer makeFramebuffer(VkDevice device, VkRenderPass pass, VkImageView view, VkExtent2D extent) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device, &info, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

void layoutBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace

bool EyeGlow::create(rendering::VkContext* ctx, VkExtent2D eyeExtent, VkFormat format,
                     const VkImageView eyeViews[kEyeCount]) {
    destroy();
    if (!ctx || eyeExtent.width == 0 || eyeExtent.height == 0) return false;
    ctx_ = ctx;
    eyeExtent_ = eyeExtent;
    glowExtent_ = {std::max(1u, eyeExtent.width / 4), std::max(1u, eyeExtent.height / 4)};
    const VkDevice device = ctx->getDevice();
    const VmaAllocator allocator = ctx->getAllocator();

    toTexture_ = makePass(device, format, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ontoEye_ = makePass(device, format, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!toTexture_ || !ontoEye_) {
        LOG_WARNING("VR glow: render passes not created - no glow");
        destroy();
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    setLayout_ = rendering::createDescriptorSetLayout(device, {binding});
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GlowPush)};
    layout_ = rendering::createPipelineLayout(device, {setLayout_}, {push});

    rendering::VkShaderModule vert;
    rendering::VkShaderModule frag;
    if (!setLayout_ || !layout_ || !vert.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/vr_glow.frag.spv")) {
        LOG_WARNING("VR glow: shaders or layouts not created - no glow");
        destroy();
        return false;
    }
    auto build = [&](VkRenderPass pass, VkPipelineColorBlendAttachmentState blend) {
        return rendering::PipelineBuilder()
            .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({}, {})
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(blend)
            .setMultisample(VK_SAMPLE_COUNT_1_BIT)
            .setLayout(layout_)
            .setRenderPass(pass)
            .setDynamicStates(rendering::viewportAndScissorDynamic())
            .build(device, ctx->getPipelineCache());
    };
    toTexturePipeline_ = build(toTexture_, rendering::PipelineBuilder::blendDisabled());
    // Alpha is added too, from the shader's 1: what comes out is opaque, which
    // is what puts the eye's alpha back after the hands used it as a mask.
    composite_ = build(ontoEye_, rendering::PipelineBuilder::blendAdditive());
    vert.destroy();
    frag.destroy();
    if (!toTexturePipeline_ || !composite_) {
        LOG_WARNING("VR glow: pipelines not created - no glow");
        destroy();
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx->getOrCreateSampler(samplerInfo);  // the context's, not ours to free

    constexpr uint32_t kSets = kEyeCount * 3;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kSets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (!sampler_ || vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        LOG_WARNING("VR glow: sampler or descriptor pool not created - no glow");
        destroy();
        return false;
    }
    auto makeSet = [&](VkImageView view) {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS) return set;
        VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return set;
    };

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (uint32_t eye = 0; eye < kEyeCount; ++eye) {
        a_[eye] = rendering::createImage(device, allocator, glowExtent_.width, glowExtent_.height, format, usage);
        b_[eye] = rendering::createImage(device, allocator, glowExtent_.width, glowExtent_.height, format, usage);
        if (!a_[eye].imageView || !b_[eye].imageView) {
            LOG_WARNING("VR glow: images not created - no glow");
            destroy();
            return false;
        }
        aFb_[eye] = makeFramebuffer(device, toTexture_, a_[eye].imageView, glowExtent_);
        bFb_[eye] = makeFramebuffer(device, toTexture_, b_[eye].imageView, glowExtent_);
        eyeFb_[eye] = makeFramebuffer(device, ontoEye_, eyeViews[eye], eyeExtent);
        eyeSet_[eye] = makeSet(eyeViews[eye]);
        aSet_[eye] = makeSet(a_[eye].imageView);
        bSet_[eye] = makeSet(b_[eye].imageView);
        if (!aFb_[eye] || !bFb_[eye] || !eyeFb_[eye] || !eyeSet_[eye] || !aSet_[eye] || !bSet_[eye]) {
            LOG_WARNING("VR glow: framebuffers or sets not created - no glow");
            destroy();
            return false;
        }
    }
    LOG_INFO("VR glow: ", glowExtent_.width, "x", glowExtent_.height, " per eye");
    return true;
}

void EyeGlow::destroy() {
    if (!ctx_) return;
    const VkDevice device = ctx_->getDevice();
    const VmaAllocator allocator = ctx_->getAllocator();
    for (uint32_t eye = 0; eye < kEyeCount; ++eye) {
        for (VkFramebuffer* fb : {&aFb_[eye], &bFb_[eye], &eyeFb_[eye]}) {
            if (*fb) vkDestroyFramebuffer(device, *fb, nullptr);
            *fb = VK_NULL_HANDLE;
        }
        rendering::destroyImage(device, allocator, a_[eye]);
        rendering::destroyImage(device, allocator, b_[eye]);
        eyeSet_[eye] = aSet_[eye] = bSet_[eye] = VK_NULL_HANDLE;
    }
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (composite_) vkDestroyPipeline(device, composite_, nullptr);
    if (toTexturePipeline_) vkDestroyPipeline(device, toTexturePipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (ontoEye_) vkDestroyRenderPass(device, ontoEye_, nullptr);
    if (toTexture_) vkDestroyRenderPass(device, toTexture_, nullptr);
    pool_ = VK_NULL_HANDLE;
    composite_ = toTexturePipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ontoEye_ = toTexture_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void EyeGlow::pass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkExtent2D extent,
                   VkDescriptorSet source, VkExtent2D sourceExtent, int mode, float dirX, float dirY,
                   float strength) const {
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = rp;
    begin.framebuffer = fb;
    begin.renderArea.extent = extent;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mode == 2 ? composite_ : toTexturePipeline_);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &source, 0, nullptr);
    const GlowPush push{1.0f / static_cast<float>(sourceExtent.width), 1.0f / static_cast<float>(sourceExtent.height),
                        dirX, dirY, mode, kThreshold, strength, 0.0f};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void EyeGlow::record(VkCommandBuffer cmd, uint32_t eye, VkImage eyeImage, float strength, bool opaqueAlpha) const {
    if (!ready() || eye >= kEyeCount) return;
    if (strength <= 0.0f) {
        if (!opaqueAlpha) return;
        // No glow, only the alpha: the composite adds nothing to the colour.
        layoutBarrier(cmd, eyeImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        pass(cmd, ontoEye_, eyeFb_[eye], eyeExtent_, aSet_[eye], glowExtent_, 2, 0.0f, 0.0f, 0.0f);
        return;
    }

    // The world pass left the eye ready to present; it is read first.
    layoutBarrier(cmd, eyeImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // What is bright, at a quarter; then blurred across and down.
    pass(cmd, toTexture_, aFb_[eye], glowExtent_, eyeSet_[eye], eyeExtent_, 0, 0.0f, 0.0f, 0.0f);
    pass(cmd, toTexture_, bFb_[eye], glowExtent_, aSet_[eye], glowExtent_, 1, 1.0f, 0.0f, 0.0f);
    pass(cmd, toTexture_, aFb_[eye], glowExtent_, bSet_[eye], glowExtent_, 1, 0.0f, 1.0f, 0.0f);
    // And added onto the eye, which goes on to the copy as it came.
    layoutBarrier(cmd, eyeImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    pass(cmd, ontoEye_, eyeFb_[eye], eyeExtent_, aSet_[eye], glowExtent_, 2, 0.0f, 0.0f, strength);
}

}  // namespace wowee::xr
