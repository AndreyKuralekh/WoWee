#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

#include "rendering/vk_utils.hpp"

namespace wowee::rendering {
class VkContext;
}

namespace wowee::xr {

/// The joints of a hand as OpenXR numbers them (XR_HAND_JOINT_SET_DEFAULT_EXT):
/// the palm, the wrist, then four joints and a tip for each finger from the
/// thumb out.
constexpr uint32_t kHandJointCount = 26;

/// Where the player's hands are this frame. Index 0 is the left hand. The
/// joints are in the tracking space, in metres, and are drawn through each
/// eye's EyeView::fromTracking - never through world coordinates, which are
/// too large for a float to hold a fingertip steady in.
struct HandPoses {
    bool tracked[2] = {false, false};
    std::array<glm::mat4, kHandJointCount> joint[2]{};
    /// The tracking space in the world. Only its turn is used: it takes the
    /// sun's direction into the tracking space.
    glm::mat4 trackingToWorld{1.0f};
    /// No headset (WOWEE_VR_FAKE): the hands are held still in front of the
    /// eyes, open, in the pose the model was made in.
    bool fake = false;
};

/// The player's hands, drawn into each eye after the world and against its
/// depth, so that a wall in front of a hand hides it.
///
/// The model is the WebXR "generic-hand" (assets/vr/hands, MIT): one skinned
/// mesh per hand on the same 25 joints OpenXR reports, less the palm. Each
/// joint the headset reports places the part of the mesh bound to it.
class HandRenderer {
public:
    static constexpr uint32_t kFrames = 2;
    static constexpr uint32_t kEyeCount = 2;

    /// For the eye pass: `pass` is the render pass the eyes are drawn in, which
    /// the pipeline is built against.
    bool create(rendering::VkContext* ctx, VkRenderPass pass);
    void destroy();
    [[nodiscard]] bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

    /// What both eyes of this frame slot draw: the hands' joints and the light
    /// they are seen in. False when there is nothing to draw.
    bool prepare(uint32_t frame, const HandPoses& poses, const glm::vec3& lightDir, const glm::vec3& lightColor,
                 const glm::vec3& ambientColor, float tone);

    /// Draws into `cmd`, inside the eye's pass, its viewport set: the eye's
    /// alpha cleared, then the hands, which leave alpha 1 where they are seen.
    /// `projection` and `fromTracking` are the eye's (see EyeView).
    void record(VkCommandBuffer cmd, uint32_t frame, const glm::mat4& projection,
                const glm::mat4& fromTracking) const;

    /// The same into a secondary buffer of its own, for a pass that takes only
    /// secondaries. Null when there is nothing to draw.
    VkCommandBuffer recordSecondary(uint32_t frame, uint32_t eye, VkRenderPass pass, VkFramebuffer framebuffer,
                                    VkExtent2D extent, const glm::mat4& projection,
                                    const glm::mat4& fromTracking);

private:
    static constexpr uint32_t kModelJoints = 25;

    struct Mesh {
        rendering::AllocatedBuffer vertices;
        rendering::AllocatedBuffer indices;
        uint32_t indexCount = 0;
        // For each of the model's joints: which OpenXR joint moves it, the
        // inverse of where it sat when the mesh was bound, and that place.
        std::array<uint32_t, kModelJoints> xrJoint{};
        std::array<glm::mat4, kModelJoints> inverseBind{};
        std::array<glm::mat4, kModelJoints> bindPose{};
    };
    bool loadMesh(uint32_t hand, const char* path);

    rendering::VkContext* ctx_ = nullptr;
    Mesh mesh_[2];
    bool draw_[kFrames][2] = {};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline maskPipeline_ = VK_NULL_HANDLE;  // clears the eye's alpha
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    rendering::AllocatedBuffer ubo_[kFrames];
    VkDescriptorSet set_[kFrames] = {};
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer secondary_[kEyeCount][kFrames] = {};
};

}  // namespace wowee::xr
