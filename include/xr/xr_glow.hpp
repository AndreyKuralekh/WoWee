#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "rendering/vk_utils.hpp"

namespace wowee::rendering {
class VkContext;
}

namespace wowee::xr {

/// The glow over the headset's eyes: what is bright is blurred at a quarter
/// of the eye's size and added back onto it, the way the original client's
/// full-screen glow softens and warms its picture. The window's own frame
/// has a post-processing chain; the eyes skip it, so this is theirs.
///
/// Recorded after an eye's world pass and before it is copied to the
/// headset, on the eye target itself. Costs the GPU three small passes and
/// one full-size additive one per eye; the CPU a handful of commands.
class EyeGlow {
public:
    static constexpr uint32_t kEyeCount = 2;

    /// For eye targets of this size and format, drawn in their own pass and
    /// left in PRESENT_SRC_KHR layout - which is what record() takes them in
    /// and leaves them in.
    bool create(rendering::VkContext* ctx, VkExtent2D eyeExtent, VkFormat format,
                const VkImageView eyeViews[kEyeCount]);
    void destroy();
    [[nodiscard]] bool ready() const { return composite_ != VK_NULL_HANDLE; }

    /// Strength 0 records nothing, unless `opaqueAlpha`. Either way the eye
    /// leaves with alpha 1 everywhere: the hands' pass used it as their mask,
    /// and the runtime blends even the world's layer by it.
    void record(VkCommandBuffer cmd, uint32_t eye, VkImage eyeImage, float strength, bool opaqueAlpha = false) const;

private:
    void pass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkExtent2D extent,
              VkDescriptorSet source, VkExtent2D sourceExtent, int mode, float dirX, float dirY,
              float strength) const;

    rendering::VkContext* ctx_ = nullptr;
    VkExtent2D eyeExtent_{};
    VkExtent2D glowExtent_{};

    VkRenderPass toTexture_ = VK_NULL_HANDLE;   // a glow image, for sampling next
    VkRenderPass ontoEye_ = VK_NULL_HANDLE;     // added onto the eye, kept
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline toTexturePipeline_ = VK_NULL_HANDLE;
    VkPipeline composite_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    rendering::AllocatedImage a_[kEyeCount];
    rendering::AllocatedImage b_[kEyeCount];
    VkFramebuffer aFb_[kEyeCount] = {};
    VkFramebuffer bFb_[kEyeCount] = {};
    VkFramebuffer eyeFb_[kEyeCount] = {};
    VkDescriptorSet eyeSet_[kEyeCount] = {};
    VkDescriptorSet aSet_[kEyeCount] = {};
    VkDescriptorSet bSet_[kEyeCount] = {};
};

}  // namespace wowee::xr
