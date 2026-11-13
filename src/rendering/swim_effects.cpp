#include "rendering/swim_effects.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace wowee {
namespace rendering {

// Half the target's height, for the drops' size in the world (swim_ripple.vert).
static constexpr VkPushConstantRange kDropSizeRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};

static std::mt19937& rng() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return gen;
}

static float randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng());
}

SwimEffects::SwimEffects() = default;

bool SwimEffects::createSoftDepthResources() {
    if (softLayout_) return true;
    VkDevice device = vkCtx->getDevice();
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &softLayout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSoftSets};
    VkDescriptorPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSoftSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &softPool_) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[kSoftSets];
    for (auto& l : layouts) l = softLayout_;
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = softPool_;
    ai.descriptorSetCount = kSoftSets;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &ai, softSets_) != VK_SUCCESS) return false;
    const uint8_t white[4] = {255, 255, 255, 255};
    softWhite_ = std::make_unique<VkTexture>();
    if (!softWhite_->upload(*vkCtx, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM)) return false;
    softWhite_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VkDescriptorImageInfo img = softWhite_->descriptorInfo();
    VkWriteDescriptorSet writes[kSoftSets]{};
    for (uint32_t i = 0; i < kSoftSets; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = softSets_[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &img;
    }
    vkUpdateDescriptorSets(device, kSoftSets, writes, 0, nullptr);
    return true;
}

void SwimEffects::setSoftDepth(bool on, uint32_t frame, VkImageView depthView) {
    softOn_ = on && depthView != VK_NULL_HANDLE && softLayout_;
    softFrame_ = frame % kSoftFrames;
    if (!softOn_ || softViews_[softFrame_] == depthView) return;
    VkSamplerCreateInfo si{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorImageInfo img{};
    img.sampler = vkCtx->getOrCreateSampler(si);
    img.imageView = depthView;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = softSets_[softFrame_];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    softViews_[softFrame_] = depthView;
}
SwimEffects::~SwimEffects() { shutdown(); }

/// The ripple pipeline: the rings that spread where a swimmer breaks the
/// surface.
///
/// initialize() and recreatePipelines() described these three identically,
/// so each was stated twice in one file.
void SwimEffects::buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    ripplePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(ripplePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

/// The bubble pipeline: what rises from a swimmer under the surface.
void SwimEffects::buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    bubblePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(bubblePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

/// The insect pipeline: the midges that hover over still water.
void SwimEffects::buildInsectPipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    insectPipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        // Tested against the scene: untested, the midges over a pond were
        // seen through the walls of every house near it.
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(insectPipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

bool SwimEffects::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing swim effects");

    vkCtx = ctx;
    VkDevice device = vkCtx->getDevice();

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // ---- Vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats, stride = 20 bytes ----
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Soft edges (TASK_12): set 1 of the ripples and bubbles, the scene's
    // depth or a white stand-in that changes nothing ----
    if (!createSoftDepthResources()) return false;

    // ---- Ripple pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_ripple.vert.spv", "assets/shaders/swim_ripple.frag.spv", "swim_ripple");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        ripplePipelineLayout = createPipelineLayout(device, {perFrameLayout, softLayout_}, {kDropSizeRange});
        if (ripplePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline layout");
            return false;
        }

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);


        if (ripplePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline");
            return false;
        }
    }

    // ---- Bubble pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_bubble.vert.spv", "assets/shaders/swim_bubble.frag.spv", "swim_bubble");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        bubblePipelineLayout = createPipelineLayout(device, {perFrameLayout, softLayout_}, {kDropSizeRange});
        if (bubblePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline layout");
            return false;
        }

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);


        if (bubblePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline");
            return false;
        }
    }

    // ---- Insect pipeline (dark point sprites) ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_ripple.vert.spv", "assets/shaders/swim_insect.frag.spv", "swim_ripple");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        insectPipelineLayout = createPipelineLayout(device, {perFrameLayout}, {kDropSizeRange});
        if (insectPipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect pipeline layout");
            return false;
        }

        // Depth test disabled - insects are screen-space sprites that must always
        // render above the water surface regardless of scene geometry.
        buildInsectPipeline(device, vertStage, fragStage, binding, attrs);


        if (insectPipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect pipeline");
            return false;
        }
    }

    // ---- Create dynamic mapped vertex buffers ----
    rippleDynamicVBSize = (MAX_RIPPLE_PARTICLES + MAX_SPRAY_PARTICLES) * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), rippleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        rippleDynamicVB = buf.buffer;
        rippleDynamicVBAlloc = buf.allocation;
        rippleDynamicVBAllocInfo = buf.info;
        if (rippleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple dynamic vertex buffer");
            return false;
        }
    }

    bubbleDynamicVBSize = MAX_BUBBLE_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), bubbleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        bubbleDynamicVB = buf.buffer;
        bubbleDynamicVBAlloc = buf.allocation;
        bubbleDynamicVBAllocInfo = buf.info;
        if (bubbleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble dynamic vertex buffer");
            return false;
        }
    }

    insectDynamicVBSize = MAX_INSECT_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), insectDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        insectDynamicVB = buf.buffer;
        insectDynamicVBAlloc = buf.allocation;
        insectDynamicVBAllocInfo = buf.info;
        if (insectDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect dynamic vertex buffer");
            return false;
        }
    }

    ripples.reserve(MAX_RIPPLE_PARTICLES);
    bubbles.reserve(MAX_BUBBLE_PARTICLES);
    insects.reserve(MAX_INSECT_PARTICLES);
    rippleVertexData.reserve(MAX_RIPPLE_PARTICLES * 5);
    bubbleVertexData.reserve(MAX_BUBBLE_PARTICLES * 5);
    insectVertexData.reserve(MAX_INSECT_PARTICLES * 5);

    LOG_INFO("Swim effects initialized");
    return true;
}

void SwimEffects::shutdown() {
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        VmaAllocator allocator = vkCtx->getAllocator();
        if (softPool_) { vkDestroyDescriptorPool(device, softPool_, nullptr); softPool_ = VK_NULL_HANDLE; }
        if (softLayout_) { vkDestroyDescriptorSetLayout(device, softLayout_, nullptr); softLayout_ = VK_NULL_HANDLE; }
        for (auto& s : softSets_) s = VK_NULL_HANDLE;
        for (auto& v : softViews_) v = VK_NULL_HANDLE;
        softWhite_.reset();

        destroy(device, ripplePipeline);
        destroy(device, ripplePipelineLayout);
        destroy(allocator, rippleDynamicVB, rippleDynamicVBAlloc);

        destroy(device, bubblePipeline);
        destroy(device, bubblePipelineLayout);
        destroy(allocator, bubbleDynamicVB, bubbleDynamicVBAlloc);

        destroy(device, insectPipeline);
        destroy(device, insectPipelineLayout);
        destroy(allocator, insectDynamicVB, insectDynamicVBAlloc);
    }

    vkCtx = nullptr;
    ripples.clear();
    bubbles.clear();
    insects.clear();
}

void SwimEffects::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // Destroy old pipelines (NOT layouts)
    destroy(device, ripplePipeline);
    destroy(device, bubblePipeline);
    destroy(device, insectPipeline);

    // Shared vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Rebuild ripple pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_ripple.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_ripple.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);

    }

    // ---- Rebuild bubble pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_bubble.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_bubble.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);

    }

    // ---- Rebuild insect pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_ripple.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_insect.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildInsectPipeline(device, vertStage, fragStage, binding, attrs);

    }
}

void SwimEffects::spawnRipple(const glm::vec3& pos, const glm::vec3& moveDir, float waterH) {
    if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) return;

    Particle p;
    // Scatter splash droplets around the character at the water surface
    float ox = randFloat(-1.5f, 1.5f);
    float oy = randFloat(-1.5f, 1.5f);
    p.position = glm::vec3(pos.x + ox, pos.y + oy, waterH + 0.3f);

    // Spray outward + upward from movement direction
    float spread = randFloat(-1.0f, 1.0f);
    glm::vec3 perp(-moveDir.y, moveDir.x, 0.0f);
    glm::vec3 outDir = -moveDir + perp * spread;
    float speed = randFloat(1.5f, 4.0f);
    p.velocity = glm::vec3(outDir.x * speed, outDir.y * speed, randFloat(1.0f, 3.0f));

    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(0.5f, 1.0f);
    p.size = randFloat(3.0f, 7.0f);
    p.alpha = randFloat(0.5f, 0.8f);

    ripples.push_back(p);
}

void SwimEffects::spawnFootSplash(const glm::vec3& footPos, float waterH) {
    // Small burst of splash droplets at foot position (for wading)
    constexpr int splashCount = 12;
    for (int i = 0; i < splashCount; ++i) {
        if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
        Particle p;
        float ox = randFloat(-0.4f, 0.4f);
        float oy = randFloat(-0.4f, 0.4f);
        p.position = glm::vec3(footPos.x + ox, footPos.y + oy, waterH + 0.1f);
        // Small upward spray in random horizontal direction
        float angle = randFloat(0.0f, 6.2832f);
        float speed = randFloat(0.8f, 2.0f);
        p.velocity = glm::vec3(std::cos(angle) * speed, std::sin(angle) * speed, randFloat(1.0f, 2.5f));
        p.lifetime = 0.0f;
        p.maxLifetime = randFloat(0.3f, 0.6f);
        p.size = randFloat(2.0f, 4.0f);
        p.alpha = randFloat(0.4f, 0.7f);
        ripples.push_back(p);
    }
}

void SwimEffects::spawnBubble(const glm::vec3& pos, float /*waterH*/) {
    if (static_cast<int>(bubbles.size()) >= MAX_BUBBLE_PARTICLES) return;

    Particle p;
    float ox = randFloat(-3.0f, 3.0f);
    float oy = randFloat(-3.0f, 3.0f);
    float oz = randFloat(-2.0f, 0.0f);
    p.position = glm::vec3(pos.x + ox, pos.y + oy, pos.z + oz);

    p.velocity = glm::vec3(randFloat(-0.3f, 0.3f), randFloat(-0.3f, 0.3f), randFloat(4.0f, 8.0f));
    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(2.0f, 3.5f);
    p.size = randFloat(6.0f, 12.0f);
    p.alpha = 0.6f;

    bubbles.push_back(p);
}

void SwimEffects::spawnInsect(const glm::vec3& vegPos) {
    if (static_cast<int>(insects.size()) >= MAX_INSECT_PARTICLES) return;

    InsectParticle p;
    p.orbitCenter = vegPos;
    p.phase = randFloat(0.0f, 6.2832f);
    p.orbitRadius = randFloat(0.5f, 2.0f);
    p.orbitSpeed = randFloat(1.5f, 4.0f);
    p.heightOffset = randFloat(0.5f, 3.0f);
    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(3.0f, 8.0f);
    p.size = randFloat(2.0f, 3.0f);
    p.alpha = randFloat(0.6f, 0.9f);

    // Start at orbit position
    float angle = p.phase;
    p.position = vegPos + glm::vec3(
        std::cos(angle) * p.orbitRadius,
        std::sin(angle) * p.orbitRadius,
        p.heightOffset
    );

    insects.push_back(p);
}

void SwimEffects::update(const Camera& camera, const CameraController& cc,
                         const WaterRenderer& water, float deltaTime) {
    glm::vec3 camPos = camera.getPosition();

    // Use character position for ripples in third-person mode
    glm::vec3 charPos = camPos;
    const glm::vec3* followTarget = cc.getFollowTarget();
    if (cc.isThirdPerson() && followTarget) {
        charPos = *followTarget;
    }

    // Check water at character position (for ripples) and camera position (for bubbles)
    auto charWaterH = water.getWaterHeightAt(charPos.x, charPos.y);
    auto camWaterH = water.getWaterHeightAt(camPos.x, camPos.y);

    bool swimming = cc.isSwimming();
    bool moving = cc.isMoving();

    // WOWEE_SWIM_DIAG=1: spray thrown round the character all the time, as if
    // swimming on the spot, so the drops can be looked at without a swim.
    static const bool diag = [] {
        const char* v = std::getenv("WOWEE_SWIM_DIAG");
        return v && *v == '1';
    }();
    if (diag && !swimming) {
        charWaterH = charPos.z;  // at the feet, wherever they are: on a bank the water is under the ground
        swimming = true;
        moving = true;
    }
    if (diag) {
        static float sinceSaid = 0.0f;
        sinceSaid += deltaTime;
        if (sinceSaid > 2.0f) {
            sinceSaid = 0.0f;
            LOG_WARNING("Swim diag: ", ripples.size(), " drops, ", bubbles.size(), " bubbles at (", charPos.x, ", ",
                        charPos.y, ", ", charPos.z, ") surface ", *charWaterH, " dt ", deltaTime);
        }
    }

    // --- Ripple/splash spawning ---
    if (swimming && charWaterH) {
        float wh = *charWaterH;
        // Three times what it was: seen from the eyes in a headset, a yard off,
        // forty drops a second read as a stray few rather than spray.
        float spawnRate = moving ? 120.0f : 24.0f;
        rippleSpawnAccum += spawnRate * deltaTime;

        // Compute movement direction from camera yaw
        float yawRad = glm::radians(cc.getYaw());
        glm::vec3 moveDir(std::cos(yawRad), std::sin(yawRad), 0.0f);
        if (moveDir.x * moveDir.x + moveDir.y * moveDir.y > 1e-6f) {
            moveDir = glm::normalize(moveDir);
        }

        while (rippleSpawnAccum >= 1.0f) {
            spawnRipple(charPos, moveDir, wh);
            rippleSpawnAccum -= 1.0f;
        }
    } else {
        rippleSpawnAccum = 0.0f;
        // Don't clear ripples - foot splash particles are added while wading
        // (not swimming) and need to live out their lifetime.
    }

    // --- Wading spray ---
    // Feet in the water but not swimming: the shoreline transition. Ripples
    // above only spawn while swimming, so walking or riding through the
    // shallows threw nothing but the per-footstep burst, which reads as dry.
    // Rate rises with depth, since deeper water is displaced harder, and dies
    // off as the surface approaches the character's waist, by which point
    // swimming takes over.
    if (!swimming && moving && charWaterH) {
        const float depth = *charWaterH - charPos.z;
        constexpr float kWadeMinDepth = 0.03f;
        constexpr float kWadeMaxDepth = 1.60f;
        if (depth > kWadeMinDepth && depth < kWadeMaxDepth) {
            const float depthScale = glm::clamp(depth / 0.7f, 0.30f, 1.0f);
            // Its own accumulator: the swim branch above zeroes rippleSpawnAccum
            // on every frame it is not swimming, so a wading rate of 30/s could
            // only ever reach 0.5 in a frame and never crossed the threshold.
            wadeSpawnAccum += 90.0f * depthScale * deltaTime;

            // Forward from yaw is (cos, sin) - the same derivation the camera
            // controller uses to move the character.
            const float yawRad = glm::radians(cc.getYaw());
            const glm::vec2 travel(std::cos(yawRad), std::sin(yawRad));

            while (wadeSpawnAccum >= 1.0f) {
                wadeSpawnAccum -= 1.0f;
                if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
                Particle p;
                // Around the legs, biased behind so the spray trails the stride.
                const float side = randFloat(-0.45f, 0.45f);
                const float back = randFloat(-0.55f, 0.15f);
                p.position = glm::vec3(charPos.x + travel.x * back - travel.y * side,
                                       charPos.y + travel.y * back + travel.x * side,
                                       *charWaterH + 0.05f);
                const float angle = randFloat(0.0f, 6.2832f);
                const float outward = randFloat(0.5f, 1.6f) * depthScale;
                p.velocity = glm::vec3(std::cos(angle) * outward - travel.x * 0.8f,
                                       std::sin(angle) * outward - travel.y * 0.8f,
                                       randFloat(0.9f, 2.2f) * depthScale);
                p.lifetime = 0.0f;
                p.maxLifetime = randFloat(0.25f, 0.55f);
                p.size = randFloat(1.6f, 3.4f) * depthScale;
                p.alpha = randFloat(0.35f, 0.65f) * depthScale;
                ripples.push_back(p);
            }
        } else {
            wadeSpawnAccum = 0.0f;
        }
    } else {
        wadeSpawnAccum = 0.0f;
    }

    // --- Bubble spawning ---
    // Require swimming state to prevent spurious bubbles on login/teleport
    // when camera may briefly appear below a water surface before grounding.
    bool underwater = swimming && camWaterH && camPos.z < *camWaterH;
    if (underwater) {
        float bubbleRate = 20.0f;
        bubbleSpawnAccum += bubbleRate * deltaTime;
        while (bubbleSpawnAccum >= 1.0f) {
            spawnBubble(camPos, *camWaterH);
            bubbleSpawnAccum -= 1.0f;
        }
    } else {
        bubbleSpawnAccum = 0.0f;
        bubbles.clear();
    }

    // --- Insect spawning near water vegetation ---
    if (m2Renderer) {
        auto vegPositions = m2Renderer->getWaterVegetationPositions(camPos, 60.0f);
        if (!vegPositions.empty()) {
            // Spawn rate: ~4/sec per nearby vegetation cluster (capped by MAX_INSECT_PARTICLES)
            float spawnRate = std::min(static_cast<float>(vegPositions.size()) * 4.0f, 20.0f);
            insectSpawnAccum += spawnRate * deltaTime;
            while (insectSpawnAccum >= 1.0f && static_cast<int>(insects.size()) < MAX_INSECT_PARTICLES) {
                // Pick a random vegetation position to spawn near
                int idx = static_cast<int>(randFloat(0.0f, static_cast<float>(vegPositions.size()) - 0.01f));
                spawnInsect(vegPositions[idx]);
                insectSpawnAccum -= 1.0f;
            }
            if (insectSpawnAccum > 2.0f) insectSpawnAccum = 0.0f;
        }
    }

    // --- Update ripples (splash droplets with gravity) ---
    for (int i = static_cast<int>(ripples.size()) - 1; i >= 0; --i) {
        auto& p = ripples[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }
        // Apply gravity to splash droplets
        p.velocity.z -= 9.8f * deltaTime;
        p.position += p.velocity * deltaTime;

        // Kill if fallen back below water
        float surfaceZ = charWaterH ? *charWaterH : 0.0f;
        if (p.position.z < surfaceZ && p.lifetime > 0.1f) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }

        float t = p.lifetime / p.maxLifetime;
        p.alpha = glm::mix(0.7f, 0.0f, t);
        p.size = glm::mix(5.0f, 2.0f, t);
    }

    // --- Spray at the feet of near waterfalls (TASK_12) ---
    // The falling water strikes the pool and throws drops up and out along
    // the fall's width: about 160 a second a fall (at forty there were too few
    // to read as spray), each up to a couple of
    // yards high, falling back into the water it came from.
    if (!waterfallFeet_.empty()) {
        sprayAccum_ += deltaTime * 160.0f * static_cast<float>(waterfallFeet_.size());
        while (sprayAccum_ >= 1.0f && static_cast<int>(spray_.size()) < MAX_SPRAY_PARTICLES) {
            sprayAccum_ -= 1.0f;
            const glm::vec4& foot = waterfallFeet_[static_cast<size_t>(randFloat(0.0f, 0.999f) *
                                                                       static_cast<float>(waterfallFeet_.size()))];
            const float half = std::max(foot.w * 0.5f, 1.0f);
            SprayParticle s;
            s.surface = foot.z;
            s.p.position = glm::vec3(foot.x + randFloat(-half, half), foot.y + randFloat(-half, half), foot.z + 0.1f);
            const float a = randFloat(0.0f, 6.2831853f);
            const float out = randFloat(0.5f, 2.5f);
            s.p.velocity = glm::vec3(std::cos(a) * out, std::sin(a) * out, randFloat(2.5f, 6.0f));
            s.p.lifetime = 0.0f;
            s.p.maxLifetime = randFloat(0.7f, 1.4f);
            s.p.size = randFloat(8.0f, 16.0f);
            s.p.alpha = randFloat(0.35f, 0.6f);
            spray_.push_back(s);
        }
        if (sprayAccum_ > 4.0f) sprayAccum_ = 0.0f;
    }
    for (int i = static_cast<int>(spray_.size()) - 1; i >= 0; --i) {
        auto& s = spray_[static_cast<size_t>(i)];
        s.p.lifetime += deltaTime;
        s.p.velocity.z -= 9.8f * deltaTime;
        s.p.position += s.p.velocity * deltaTime;
        if (s.p.lifetime >= s.p.maxLifetime || (s.p.position.z < s.surface && s.p.lifetime > 0.1f)) {
            spray_[static_cast<size_t>(i)] = spray_.back();
            spray_.pop_back();
            continue;
        }
        const float t = s.p.lifetime / s.p.maxLifetime;
        s.p.alpha = glm::mix(0.55f, 0.0f, t);
        s.p.size = glm::mix(16.0f, 6.0f, t);
    }

    // --- Update bubbles ---
    float bubbleCeilH = camWaterH ? *camWaterH : 0.0f;
    for (int i = static_cast<int>(bubbles.size()) - 1; i >= 0; --i) {
        auto& p = bubbles[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime || p.position.z >= bubbleCeilH) {
            bubbles[i] = bubbles.back();
            bubbles.pop_back();
            continue;
        }
        // Wobble
        float wobbleX = std::sin(p.lifetime * 3.0f) * 0.5f;
        float wobbleY = std::cos(p.lifetime * 2.5f) * 0.5f;
        p.position += (p.velocity + glm::vec3(wobbleX, wobbleY, 0.0f)) * deltaTime;

        float t = p.lifetime / p.maxLifetime;
        if (t > 0.8f) {
            p.alpha = 0.6f * (1.0f - (t - 0.8f) / 0.2f);
        } else {
            p.alpha = 0.6f;
        }
    }

    // --- Update insects (erratic orbiting flight) ---
    for (int i = static_cast<int>(insects.size()) - 1; i >= 0; --i) {
        auto& p = insects[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime) {
            insects[i] = insects.back();
            insects.pop_back();
            continue;
        }

        float t = p.lifetime / p.maxLifetime;
        float time = p.lifetime * p.orbitSpeed + p.phase;

        // Erratic looping: primary orbit + secondary wobble
        float primaryAngle = time;
        float wobbleAngle = std::sin(time * 2.3f) * 0.8f;
        float radius = p.orbitRadius + std::sin(time * 1.7f) * 0.3f;

        float heightWobble = std::sin(time * 1.1f + p.phase * 0.5f) * 0.5f;

        p.position = p.orbitCenter + glm::vec3(
            std::cos(primaryAngle + wobbleAngle) * radius,
            std::sin(primaryAngle + wobbleAngle) * radius,
            p.heightOffset + heightWobble
        );

        // Fade in/out
        if (t < 0.1f) {
            p.alpha = glm::mix(0.0f, 0.8f, t / 0.1f);
        } else if (t > 0.85f) {
            p.alpha = glm::mix(0.8f, 0.0f, (t - 0.85f) / 0.15f);
        } else {
            p.alpha = 0.8f;
        }
    }

    // --- Build vertex data ---
    rippleVertexData.clear();
    rippleVertexData.reserve((ripples.size() + spray_.size()) * 5);
    for (const auto& p : ripples) {
        rippleVertexData.push_back(p.position.x);
        rippleVertexData.push_back(p.position.y);
        rippleVertexData.push_back(p.position.z);
        rippleVertexData.push_back(p.size);
        rippleVertexData.push_back(p.alpha);
    }
    for (const auto& s : spray_) {
        rippleVertexData.push_back(s.p.position.x);
        rippleVertexData.push_back(s.p.position.y);
        rippleVertexData.push_back(s.p.position.z);
        rippleVertexData.push_back(s.p.size);
        rippleVertexData.push_back(s.p.alpha);
    }

    bubbleVertexData.clear();
    bubbleVertexData.reserve(bubbles.size() * 5);
    for (const auto& p : bubbles) {
        bubbleVertexData.push_back(p.position.x);
        bubbleVertexData.push_back(p.position.y);
        bubbleVertexData.push_back(p.position.z);
        bubbleVertexData.push_back(p.size);
        bubbleVertexData.push_back(p.alpha);
    }

    insectVertexData.clear();
    insectVertexData.reserve(insects.size() * 5);
    for (const auto& p : insects) {
        insectVertexData.push_back(p.position.x);
        insectVertexData.push_back(p.position.y);
        insectVertexData.push_back(p.position.z);
        insectVertexData.push_back(p.size);
        insectVertexData.push_back(p.alpha);
    }
}

void SwimEffects::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, float targetHeight) {
    const float halfHeight = 0.5f * targetHeight;
    if (rippleVertexData.empty() && bubbleVertexData.empty() && insectVertexData.empty()) return;

    VkDeviceSize offset = 0;

    // --- Render ripples (splash droplets above water surface) ---
    if (!rippleVertexData.empty() && ripplePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = rippleVertexData.size() * sizeof(float);
        if (rippleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(rippleDynamicVBAllocInfo.pMappedData, rippleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdPushConstants(cmd, ripplePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &halfHeight);
        {
            const VkDescriptorSet soft = softSet();
            if (soft) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipelineLayout, 1, 1, &soft, 0, nullptr);
        }
        vkCmdBindVertexBuffers(cmd, 0, 1, &rippleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(rippleVertexData.size() / 5), 1, 0, 0);
    }

    // --- Render bubbles ---
    if (!bubbleVertexData.empty() && bubblePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = bubbleVertexData.size() * sizeof(float);
        if (bubbleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(bubbleDynamicVBAllocInfo.pMappedData, bubbleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdPushConstants(cmd, bubblePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &halfHeight);
        {
            const VkDescriptorSet soft = softSet();
            if (soft) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipelineLayout, 1, 1, &soft, 0, nullptr);
        }
        vkCmdBindVertexBuffers(cmd, 0, 1, &bubbleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(bubbleVertexData.size() / 5), 1, 0, 0);
    }

    // --- Render insects ---
    if (!insectVertexData.empty() && insectPipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = insectVertexData.size() * sizeof(float);
        if (insectDynamicVBAllocInfo.pMappedData) {
            std::memcpy(insectDynamicVBAllocInfo.pMappedData, insectVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, insectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, insectPipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdPushConstants(cmd, insectPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &halfHeight);
        vkCmdBindVertexBuffers(cmd, 0, 1, &insectDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(insectVertexData.size() / 5), 1, 0, 0);
    }
}

} // namespace rendering
} // namespace wowee
