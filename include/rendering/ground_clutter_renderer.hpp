#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_context.hpp"

namespace wowee {

namespace pipeline { class AssetManager; }

namespace rendering {

class Camera;
class VkTexture;

/// Ground clutter: the tufts, flowers and pebbles of GroundEffectDoodad,
/// scattered per quad by pipeline/ground_clutter around the player.
///
/// Drawn instanced rather than as world doodads. The models are tiny - eight
/// vertices on average, a single bone, no animation - and there are tens of
/// thousands of them in view, so each one being an M2 instance the CPU walks
/// every frame is what capped them at three a chunk. Here the CPU touches an
/// instance once, when the window around the player is rebuilt; a compute
/// pass culls them against the view and the draw range, and each part of each
/// model is one indirect draw whose instance count the GPU wrote.
///
/// Instances are grouped by model. A model's instances occupy one range of
/// the source buffer, and its visible ones go to the same range of the visible
/// list, so a part's draw starts at the model's base and reads as many as the
/// cull counted for it.
class GroundClutterRenderer {
public:
    GroundClutterRenderer() = default;
    ~GroundClutterRenderer();

    GroundClutterRenderer(const GroundClutterRenderer&) = delete;
    GroundClutterRenderer& operator=(const GroundClutterRenderer&) = delete;

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assets);
    void shutdown();
    void recreatePipelines();

    [[nodiscard]] bool isReady() const { return pipeline_ != VK_NULL_HANDLE && cullPipeline_ != VK_NULL_HANDLE; }

    /// The model for a ground-effect doodad, loaded on first use; -1 when it
    /// will not load. Loading reads the M2, its skin and its textures.
    int modelFor(uint32_t doodadId, const std::string& m2Path);

    struct Instance {
        glm::vec3 position{0.0f};
        float yaw = 0.0f;
        float scale = 1.0f;
        int model = -1;
        glm::vec3 groundNormal{0.0f, 0.0f, 1.0f};
        float shade = 0.0f;  // baked shadow on the ground under it, 0..1
    };

    /// Replace the live instances. Sorted by model here; beyond kMaxInstances
    /// the rest are dropped.
    void setInstances(std::vector<Instance>& instances);

    /// Outside a render pass, before render() in the same frame. The range is
    /// measured from `rangeCenter`, the player, as the window is built around.
    void dispatchCull(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera,
                      const glm::vec3& rangeCenter);
    void render(VkCommandBuffer cmd, uint32_t frameIndex, VkDescriptorSet perFrameSet);

    /// Draw distance in yards; the last part of it fades the doodads out.
    void setDistance(float yards) { distance_ = yards; }
    [[nodiscard]] float distance() const { return distance_; }

    [[nodiscard]] uint32_t instanceCount() const { return instanceCount_; }
    [[nodiscard]] size_t modelCount() const { return models_.size(); }

    static constexpr uint32_t kMaxInstances = 600000;
    static constexpr uint32_t kMaxVertices = 262144;
    static constexpr uint32_t kMaxIndices = 1048576;
    /// Draw commands, one per model part. vkCmdUpdateBuffer resets them each
    /// frame, which caps the array at 64 KB.
    static constexpr uint32_t kMaxParts = 3000;
    static constexpr uint32_t kMaxTextures = 1024;

private:
    struct Part {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        VkDescriptorSet textureSet = VK_NULL_HANDLE;
    };
    struct Model {
        uint32_t vertexOffset = 0;
        uint32_t firstPart = 0;
        uint32_t partCount = 0;
        float radius = 1.0f;
    };

    bool createBuffers();
    bool createCullPipeline();
    bool createDrawResources(VkDescriptorSetLayout perFrameLayout);
    bool buildDrawPipeline();
    VkDescriptorSet textureSetFor(const std::string& path);
    void uploadModelTable();
    bool queueUpload(const void* data, VkDeviceSize size, VkBuffer dst);
    void recordUploads(VkCommandBuffer cmd, uint32_t frameIndex);

    VkContext* vkCtx_ = nullptr;
    pipeline::AssetManager* assets_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    float distance_ = 140.0f;
    uint32_t instanceCount_ = 0;

    std::vector<Model> models_;
    std::vector<Part> parts_;
    std::unordered_map<uint32_t, int> modelByDoodad_;
    std::vector<uint8_t> vertexData_;     // ClutterVertex, CPU copy for whole re-uploads
    std::vector<uint16_t> indexData_;
    std::unordered_map<std::string, VkDescriptorSet> textureSets_;
    std::vector<std::unique_ptr<VkTexture>> textures_;
    std::unique_ptr<VkTexture> whiteTexture_;
    bool geometryDirty_ = false;
    bool modelTableDirty_ = false;
    // Per model, where its range of the source and visible buffers starts.
    std::vector<uint32_t> modelBase_;
    // The draw commands as the cull wants them at the start of a frame:
    // every count zero, every first instance its model's base.
    std::vector<VkDrawIndexedIndirectCommand> commandTemplate_;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAlloc_ = VK_NULL_HANDLE;
    VkBuffer instanceBuffer_ = VK_NULL_HANDLE;
    VmaAllocation instanceAlloc_ = VK_NULL_HANDLE;
    VkBuffer modelInfoBuffer_ = VK_NULL_HANDLE;
    VmaAllocation modelInfoAlloc_ = VK_NULL_HANDLE;

    static constexpr uint32_t kFrames = MAX_FRAMES_IN_FLIGHT;
    struct PendingCopy {
        VkBuffer src = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkBuffer dst = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };
    std::vector<PendingCopy> pendingCopies_;
    std::vector<PendingCopy> retired_[MAX_FRAMES_IN_FLIGHT];
    VkBuffer cullUniform_[kFrames]{};
    VmaAllocation cullUniformAlloc_[kFrames]{};
    void* cullUniformMapped_[kFrames]{};
    VkBuffer visibleBuffer_[kFrames]{};
    VmaAllocation visibleAlloc_[kFrames]{};
    VkBuffer commandBuffer_[kFrames]{};
    VmaAllocation commandAlloc_[kFrames]{};

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet cullSet_[kFrames]{};
    VkPipelineLayout cullPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout drawSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet drawSet_[kFrames]{};
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
