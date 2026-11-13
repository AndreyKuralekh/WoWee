#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace rendering {

class Camera;
class CameraController;
class WaterRenderer;
class M2Renderer;
class VkContext;

class SwimEffects {
public:
    SwimEffects();
    ~SwimEffects();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The three pipelines both initialize() and recreatePipelines() need.
    void buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildInsectPipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);

    /// Select the render pass these particles will be recorded into. Water now
    /// draws after them in a pass of its own, so the spray has to move into that
    /// pass too or the water sheet is painted straight over it. Call before
    /// initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }
    void update(const Camera& camera, const CameraController& cc,
                const WaterRenderer& water, float deltaTime);
    /// targetHeight: the height in pixels of the image drawn into; the drops are
    /// sized in the world and turned into pixels with it.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, float targetHeight);
    /// Soft edges for the ripples and bubbles (TASK_12): the scene's depth
    /// copy for frame-in-flight `frame`, set before render; off, they draw as
    /// they always have.
    void setSoftDepth(bool on, uint32_t frame = 0, VkImageView depthView = VK_NULL_HANDLE);
    /// Where near waterfalls meet the water (TASK_12): xyz the foot on the
    /// water's surface, w how wide the fall is. Spray is thrown up there each
    /// frame; an empty list stops it.
    void setWaterfallFeet(const std::vector<glm::vec4>& feet) { waterfallFeet_ = feet; }
    void spawnFootSplash(const glm::vec3& footPos, float waterH);
    void setM2Renderer(M2Renderer* renderer) { m2Renderer = renderer; }

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
    };

    struct InsectParticle {
        glm::vec3 position;
        glm::vec3 orbitCenter;  // vegetation position to orbit around
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
        float phase;       // random phase offset for erratic motion
        float orbitRadius;
        float orbitSpeed;
        float heightOffset; // height above plant
    };

    static constexpr int MAX_RIPPLE_PARTICLES = 600;
    static constexpr int MAX_SPRAY_PARTICLES = 4800;   // at the feet of waterfalls, drawn with the ripples
    static constexpr int MAX_BUBBLE_PARTICLES = 150;
    static constexpr int MAX_INSECT_PARTICLES = 50;

    std::vector<Particle> ripples;
    struct SprayParticle {
        Particle p;
        float surface = 0.0f;   // the water it falls back into
    };
    std::vector<SprayParticle> spray_;
    std::vector<glm::vec4> waterfallFeet_;
    float sprayAccum_ = 0.0f;
    std::vector<Particle> bubbles;
    std::vector<InsectParticle> insects;

    // Vulkan objects
    VkContext* vkCtx = nullptr;
    M2Renderer* m2Renderer = nullptr;

    // Ripple pipeline + dynamic buffer
    VkPipeline ripplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout ripplePipelineLayout = VK_NULL_HANDLE;
    // Soft edges (TASK_12): set 1 of the ripple and bubble pipelines.
    static constexpr uint32_t kSoftFrames = 2;
    static constexpr uint32_t kSoftSets = kSoftFrames + 1;   // one a frame in flight, then the white stand-in
    VkDescriptorSetLayout softLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool softPool_ = VK_NULL_HANDLE;
    VkDescriptorSet softSets_[kSoftSets] = {};
    VkImageView softViews_[kSoftFrames] = {};
    std::unique_ptr<VkTexture> softWhite_;
    bool softOn_ = false;
    uint32_t softFrame_ = 0;
    bool createSoftDepthResources();
    [[nodiscard]] VkDescriptorSet softSet() const { return softOn_ ? softSets_[softFrame_] : softSets_[kSoftFrames]; }
    ::VkBuffer rippleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation rippleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo rippleDynamicVBAllocInfo{};
    VkDeviceSize rippleDynamicVBSize = 0;

    // Bubble pipeline + dynamic buffer
    VkPipeline bubblePipeline = VK_NULL_HANDLE;
    VkPipelineLayout bubblePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer bubbleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation bubbleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo bubbleDynamicVBAllocInfo{};
    VkDeviceSize bubbleDynamicVBSize = 0;

    // Insect pipeline + dynamic buffer
    VkPipeline insectPipeline = VK_NULL_HANDLE;
    VkPipelineLayout insectPipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer insectDynamicVB = VK_NULL_HANDLE;
    VmaAllocation insectDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo insectDynamicVBAllocInfo{};
    VkDeviceSize insectDynamicVBSize = 0;

    std::vector<float> rippleVertexData;
    std::vector<float> bubbleVertexData;
    std::vector<float> insectVertexData;

    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;

    float wadeSpawnAccum = 0.0f;
    float rippleSpawnAccum = 0.0f;
    float bubbleSpawnAccum = 0.0f;
    float insectSpawnAccum = 0.0f;

    void spawnRipple(const glm::vec3& pos, const glm::vec3& moveDir, float waterH);
    void spawnBubble(const glm::vec3& pos, float waterH);
    void spawnInsect(const glm::vec3& vegPos);
};

} // namespace rendering
} // namespace wowee
