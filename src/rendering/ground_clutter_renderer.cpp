#include "rendering/ground_clutter_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include <glm/gtc/packing.hpp>

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

namespace {

struct ClutterVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};
static_assert(sizeof(ClutterVertex) == 32, "vertex stride");

// Matches Instance in ground_clutter.vert.glsl and ground_clutter_cull.comp.glsl (std430).
struct InstanceGPU {
    glm::vec4 positionScale;  // xyz, w = scale
    float yaw;
    uint32_t model;
    uint32_t groundNormalXY;  // packHalf2x16; z is positive and derived
    float shade;
};
static_assert(sizeof(InstanceGPU) == 32, "instance stride");

// Matches ModelInfo in ground_clutter_cull.comp.glsl.
struct ModelInfoGPU {
    uint32_t base;       // first slot of the model's range
    uint32_t firstPart;  // its first draw command
    uint32_t partCount;
    float radius;        // bounding sphere at scale 1
};

// Matches CullUniforms in ground_clutter_cull.comp.glsl (std140).
struct CullUniformsGPU {
    glm::vec4 frustumPlanes[6];
    glm::vec4 center;  // xyz = range centre, w = range squared
    uint32_t instanceCount;
    uint32_t pad[3];
};

bool createGpuBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                     VkBuffer& outBuffer, VmaAllocation& outAlloc) {
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    return vmaCreateBuffer(allocator, &bci, &aci, &outBuffer, &outAlloc, nullptr) == VK_SUCCESS;
}

bool createMappedBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer& outBuffer, VmaAllocation& outAlloc, void*& outMapped) {
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ai{};
    if (vmaCreateBuffer(allocator, &bci, &aci, &outBuffer, &outAlloc, &ai) != VK_SUCCESS) return false;
    outMapped = ai.pMappedData;
    return true;
}

std::string normalizedPath(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p;
}

}  // namespace

GroundClutterRenderer::~GroundClutterRenderer() { shutdown(); }

bool GroundClutterRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                       pipeline::AssetManager* assets) {
    if (!ctx || !assets) return false;
    vkCtx_ = ctx;
    assets_ = assets;
    if (!createBuffers() || !createCullPipeline() || !createDrawResources(perFrameLayout)) {
        LOG_ERROR("GroundClutterRenderer: initialization failed");
        shutdown();
        return false;
    }
    LOG_INFO("GroundClutterRenderer initialized (capacity ", kMaxInstances, " doodads)");
    return true;
}

bool GroundClutterRenderer::createBuffers() {
    VmaAllocator allocator = vkCtx_->getAllocator();
    if (!createGpuBuffer(allocator, sizeof(ClutterVertex) * kMaxVertices,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         vertexBuffer_, vertexAlloc_)) return false;
    if (!createGpuBuffer(allocator, sizeof(uint16_t) * kMaxIndices,
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         indexBuffer_, indexAlloc_)) return false;
    if (!createGpuBuffer(allocator, sizeof(InstanceGPU) * kMaxInstances,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         instanceBuffer_, instanceAlloc_)) return false;
    if (!createGpuBuffer(allocator, sizeof(ModelInfoGPU) * kMaxParts,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         modelInfoBuffer_, modelInfoAlloc_)) return false;
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (!createMappedBuffer(allocator, sizeof(CullUniformsGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                cullUniform_[i], cullUniformAlloc_[i], cullUniformMapped_[i])) return false;
        if (!createGpuBuffer(allocator, sizeof(uint32_t) * kMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             visibleBuffer_[i], visibleAlloc_[i])) return false;
        if (!createGpuBuffer(allocator, sizeof(VkDrawIndexedIndirectCommand) * kMaxParts,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             commandBuffer_[i], commandAlloc_[i])) return false;
    }
    // Bound by parts that have no texture of their own.
    const uint8_t white[4] = {255, 255, 255, 255};
    whiteTexture_ = std::make_unique<VkTexture>();
    whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    whiteTexture_->createSampler(vkCtx_->getDevice());
    return true;
}

bool GroundClutterRenderer::createCullPipeline() {
    VkDevice device = vkCtx_->getDevice();

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrames * 8};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = kFrames * 2 + kMaxTextures;
    poolCi.poolSizeCount = 3;
    poolCi.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolCi, nullptr, &descPool_) != VK_SUCCESS) return false;

    // 0 uniforms, 1 instances, 2 visible list, 3 draw commands, 4 model table.
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCi.bindingCount = 5;
    layoutCi.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &cullSetLayout_) != VK_SUCCESS) return false;
    cullPipelineLayout_ = createPipelineLayout(device, {cullSetLayout_});
    if (cullPipelineLayout_ == VK_NULL_HANDLE) return false;

    VkShaderModule comp;
    if (!comp.loadFromFile(device, "assets/shaders/ground_clutter_cull.comp.spv")) {
        LOG_ERROR("GroundClutterRenderer: failed to load ground_clutter_cull.comp.spv");
        return false;
    }
    VkComputePipelineCreateInfo cpCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCi.stage = comp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    cpCi.layout = cullPipelineLayout_;
    const VkResult r = vkCreateComputePipelines(device, vkCtx_->getPipelineCache(), 1, &cpCi, nullptr, &cullPipeline_);
    comp.destroy();
    if (r != VK_SUCCESS) { cullPipeline_ = VK_NULL_HANDLE; return false; }

    for (uint32_t i = 0; i < kFrames; ++i) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &cullSetLayout_;
        if (vkAllocateDescriptorSets(device, &ai, &cullSet_[i]) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo infos[5] = {
            {cullUniform_[i], 0, sizeof(CullUniformsGPU)},
            {instanceBuffer_, 0, VK_WHOLE_SIZE},
            {visibleBuffer_[i], 0, VK_WHOLE_SIZE},
            {commandBuffer_[i], 0, VK_WHOLE_SIZE},
            {modelInfoBuffer_, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t b = 0; b < 5; ++b) {
            writes[b] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = cullSet_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }
    return true;
}

bool GroundClutterRenderer::createDrawResources(VkDescriptorSetLayout perFrameLayout) {
    VkDevice device = vkCtx_->getDevice();
    // Set 1: instances and this frame's visible list.
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCi.bindingCount = 2;
    layoutCi.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &drawSetLayout_) != VK_SUCCESS) return false;

    // Set 2: the part's texture.
    VkDescriptorSetLayoutBinding texBinding{};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    layoutCi.bindingCount = 1;
    layoutCi.pBindings = &texBinding;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &textureSetLayout_) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < kFrames; ++i) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &drawSetLayout_;
        if (vkAllocateDescriptorSets(device, &ai, &drawSet_[i]) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo infos[2] = {{instanceBuffer_, 0, VK_WHOLE_SIZE}, {visibleBuffer_[i], 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[b] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = drawSet_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    perFrameLayout_ = perFrameLayout;
    return buildDrawPipeline();
}

bool GroundClutterRenderer::buildDrawPipeline() {
    VkDevice device = vkCtx_->getDevice();
    destroyPipeline(device, pipeline_, pipelineLayout_);
    auto shaders = loadShaderPair(device, "assets/shaders/ground_clutter.vert.spv",
                                  "assets/shaders/ground_clutter.frag.spv", "ground clutter");
    if (!shaders) return false;

    VkPushConstantRange fade{};
    fade.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    fade.offset = 0;
    fade.size = sizeof(float) * 2;
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, drawSetLayout_, textureSetLayout_}, {fade});
    if (pipelineLayout_ == VK_NULL_HANDLE) return false;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ClutterVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ClutterVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ClutterVertex, normal))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ClutterVertex, uv))},
    };
    const bool msaa = vkCtx_->getMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
    pipeline_ = PipelineBuilder()
                    .setShaders(shaders.vertStage, shaders.fragStage)
                    .setVertexInput({binding}, attrs)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                    // Cards, seen from both sides.
                    .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                    .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setColorBlendAttachment(PipelineBuilder::blendDisabled())
                    .setMultisample(vkCtx_->getMsaaSamples())
                    .setAlphaToCoverage(msaa)
                    .setLayout(pipelineLayout_)
                    .setRenderPass(vkCtx_->getImGuiRenderPass())
                    .setDynamicStates(viewportAndScissorDynamic())
                    .build(device, vkCtx_->getPipelineCache());
    return pipeline_ != VK_NULL_HANDLE;
}

VkDescriptorSet GroundClutterRenderer::textureSetFor(const std::string& rawPath) {
    const std::string path = normalizedPath(rawPath);
    if (auto it = textureSets_.find(path); it != textureSets_.end()) return it->second;
    if (textureSets_.size() >= kMaxTextures - 1) return VK_NULL_HANDLE;

    VkTexture* tex = whiteTexture_.get();
    if (!path.empty()) {
        pipeline::BLPImage image = assets_->loadTexture(path);
        if (image.isValid()) {
            auto owned = std::make_unique<VkTexture>();
            if (owned->uploadBLP(*vkCtx_, image)) {
                owned->createSampler(vkCtx_->getDevice());
                tex = owned.get();
                textures_.push_back(std::move(owned));
            }
        }
    }
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &textureSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    const VkDescriptorImageInfo info = tex->descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
    textureSets_[path] = set;
    return set;
}

int GroundClutterRenderer::modelFor(uint32_t doodadId, const std::string& m2Path) {
    if (auto it = modelByDoodad_.find(doodadId); it != modelByDoodad_.end()) return it->second;
    int result = -1;
    // Remembered either way: a model that will not load is not tried again
    // on every rebuild.
    struct Remember {
        GroundClutterRenderer* self; uint32_t id; int* r;
        ~Remember() { self->modelByDoodad_[id] = *r; }
    } remember{this, doodadId, &result};
    if (!isReady() || m2Path.empty()) return result;

    std::vector<uint8_t> data = assets_->readFile(m2Path);
    if (data.empty()) return result;
    pipeline::M2Model m2 = pipeline::M2Loader::load(data);
    if (m2.version >= 264) {
        std::vector<uint8_t> skin = assets_->readFileOptional(pipeline::skinPathForM2(m2Path));
        if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, m2);
    }
    if (m2.vertices.empty() || m2.indices.empty() || m2.batches.empty()) return result;

    const size_t vertexCount = vertexData_.size() / sizeof(ClutterVertex);
    if (vertexCount + m2.vertices.size() > kMaxVertices ||
        indexData_.size() + m2.indices.size() > kMaxIndices ||
        parts_.size() + m2.batches.size() > kMaxParts) {
        LOG_WARNING("GroundClutterRenderer: out of room for ", m2Path);
        return result;
    }

    Model model;
    model.vertexOffset = static_cast<uint32_t>(vertexCount);
    model.firstPart = static_cast<uint32_t>(parts_.size());
    float radiusSq = 0.0f;
    for (const auto& v : m2.vertices) {
        ClutterVertex cv{v.position, v.normal, v.texCoords[0]};
        const size_t at = vertexData_.size();
        vertexData_.resize(at + sizeof(cv));
        std::memcpy(vertexData_.data() + at, &cv, sizeof(cv));
        radiusSq = std::max(radiusSq, glm::dot(v.position, v.position));
    }
    model.radius = std::max(0.25f, std::sqrt(radiusSq));
    const uint32_t indexBase = static_cast<uint32_t>(indexData_.size());
    indexData_.insert(indexData_.end(), m2.indices.begin(), m2.indices.end());

    for (const auto& batch : m2.batches) {
        if (batch.submeshLevel != 0 || batch.indexCount == 0) continue;
        std::string texPath;
        if (batch.textureIndex < m2.textureLookup.size()) {
            const uint16_t t = m2.textureLookup[batch.textureIndex];
            if (t < m2.textures.size()) texPath = m2.textures[t].filename;
        }
        Part part;
        part.firstIndex = indexBase + batch.indexStart;
        part.indexCount = batch.indexCount;
        part.textureSet = textureSetFor(texPath);
        if (part.textureSet == VK_NULL_HANDLE) continue;
        parts_.push_back(part);
        model.partCount++;
    }
    if (model.partCount == 0) return result;

    result = static_cast<int>(models_.size());
    models_.push_back(model);
    geometryDirty_ = true;
    modelTableDirty_ = true;
    return result;
}

void GroundClutterRenderer::uploadModelTable() {
    std::vector<ModelInfoGPU> table(models_.size());
    commandTemplate_.assign(parts_.size(), VkDrawIndexedIndirectCommand{});
    for (size_t m = 0; m < models_.size(); ++m) {
        const Model& model = models_[m];
        const uint32_t base = m < modelBase_.size() ? modelBase_[m] : 0;
        table[m] = {base, model.firstPart, model.partCount, model.radius};
        for (uint32_t p = 0; p < model.partCount; ++p) {
            auto& c = commandTemplate_[model.firstPart + p];
            c.indexCount = parts_[model.firstPart + p].indexCount;
            c.instanceCount = 0;
            c.firstIndex = parts_[model.firstPart + p].firstIndex;
            c.vertexOffset = static_cast<int32_t>(model.vertexOffset);
            c.firstInstance = base;
        }
    }
    if (!table.empty()) queueUpload(table.data(), sizeof(ModelInfoGPU) * table.size(), modelInfoBuffer_);
    modelTableDirty_ = false;
}

bool GroundClutterRenderer::queueUpload(const void* data, VkDeviceSize size, VkBuffer dst) {
    // Staged, and copied by the frame's own command buffer in dispatchCull,
    // behind a barrier against the previous frame's reads: the buffers being
    // replaced are still being drawn from by the frame in flight, and a
    // separate submit would either race those reads or wait for them.
    PendingCopy copy;
    copy.dst = dst;
    copy.size = size;
    void* mapped = nullptr;
    if (!createMappedBuffer(vkCtx_->getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, copy.src, copy.alloc,
                            mapped) || !mapped) {
        return false;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(vkCtx_->getAllocator(), copy.alloc, 0, VK_WHOLE_SIZE);
    // A newer copy of the same buffer replaces one not yet recorded.
    for (auto& p : pendingCopies_) {
        if (p.dst == dst) {
            destroy(vkCtx_->getAllocator(), p.src, p.alloc);
            p = copy;
            return true;
        }
    }
    pendingCopies_.push_back(copy);
    return true;
}

void GroundClutterRenderer::recordUploads(VkCommandBuffer cmd, uint32_t frameIndex) {
    // The staging of the copies this slot recorded last time: its fence has
    // been waited on by the time the slot comes round again.
    for (auto& r : retired_[frameIndex]) destroy(vkCtx_->getAllocator(), r.src, r.alloc);
    retired_[frameIndex].clear();
    if (pendingCopies_.empty()) return;

    VkMemoryBarrier2 before{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    before.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    before.srcAccessMask = 0;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &before;
    cmdPipelineBarrier2(cmd, dep);

    for (const auto& p : pendingCopies_) {
        VkBufferCopy region{};
        region.size = p.size;
        vkCmdCopyBuffer(cmd, p.src, p.dst, 1, &region);
    }

    VkMemoryBarrier2 after{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    after.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    dep.pMemoryBarriers = &after;
    cmdPipelineBarrier2(cmd, dep);

    retired_[frameIndex] = std::move(pendingCopies_);
    pendingCopies_.clear();
}
void GroundClutterRenderer::setInstances(std::vector<Instance>& instances) {
    if (!isReady()) return;
    if (geometryDirty_) {
        if (!vertexData_.empty()) queueUpload(vertexData_.data(), vertexData_.size(), vertexBuffer_);
        if (!indexData_.empty()) queueUpload(indexData_.data(), sizeof(uint16_t) * indexData_.size(), indexBuffer_);
        geometryDirty_ = false;
    }

    std::erase_if(instances, [&](const Instance& i) { return i.model < 0 || i.model >= static_cast<int>(models_.size()); });
    std::sort(instances.begin(), instances.end(), [](const Instance& a, const Instance& b) { return a.model < b.model; });
    if (instances.size() > kMaxInstances) instances.resize(kMaxInstances);

    modelBase_.assign(models_.size(), 0);
    std::vector<InstanceGPU> packed(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& in = instances[i];
        packed[i].positionScale = glm::vec4(in.position, in.scale);
        packed[i].yaw = in.yaw;
        packed[i].model = static_cast<uint32_t>(in.model);
        packed[i].groundNormalXY = glm::packHalf2x16(glm::vec2(in.groundNormal.x, in.groundNormal.y));
        packed[i].shade = in.shade;
        if (i == 0 || instances[i - 1].model != in.model) modelBase_[in.model] = static_cast<uint32_t>(i);
    }
    if (!packed.empty() && !queueUpload(packed.data(), sizeof(InstanceGPU) * packed.size(), instanceBuffer_)) {
        instanceCount_ = 0;
        return;
    }
    instanceCount_ = static_cast<uint32_t>(packed.size());
    uploadModelTable();
}

void GroundClutterRenderer::dispatchCull(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera,
                                         const glm::vec3& rangeCenter) {
    if (!isReady() || frameIndex >= kFrames) return;
    recordUploads(cmd, frameIndex);
    if (instanceCount_ == 0 || commandTemplate_.empty()) return;

    CullUniformsGPU u{};
    Frustum frustum;
    frustum.extractFromMatrix(camera.getProjectionMatrix() * camera.getViewMatrix());
    for (int i = 0; i < 6; ++i) {
        const auto& plane = frustum.getPlane(static_cast<Frustum::Side>(i));
        u.frustumPlanes[i] = glm::vec4(plane.normal, plane.distance);
    }
    u.center = glm::vec4(rangeCenter, distance_ * distance_);
    u.instanceCount = instanceCount_;
    std::memcpy(cullUniformMapped_[frameIndex], &u, sizeof(u));

    // Every count back to zero, every first instance at its model's base.
    vkCmdUpdateBuffer(cmd, commandBuffer_[frameIndex], 0,
                      sizeof(VkDrawIndexedIndirectCommand) * commandTemplate_.size(), commandTemplate_.data());
    {
        VkBufferMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = commandBuffer_[frameIndex];
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0,
                            nullptr);
    vkCmdDispatch(cmd, (instanceCount_ + 63) / 64, 1, 1);
    {
        VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    }
}

void GroundClutterRenderer::render(VkCommandBuffer cmd, uint32_t frameIndex, VkDescriptorSet perFrameSet) {
    if (!isReady() || frameIndex >= kFrames || instanceCount_ == 0 || commandTemplate_.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &drawSet_[frameIndex], 0,
                            nullptr);
    // The fade: the last sixth of the range.
    const float fade[2] = {distance_ * 0.83f, distance_};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(fade), fade);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &zero);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT16);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (size_t p = 0; p < commandTemplate_.size(); ++p) {
        if (parts_[p].textureSet != bound) {
            bound = parts_[p].textureSet;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &bound, 0, nullptr);
        }
        vkCmdDrawIndexedIndirect(cmd, commandBuffer_[frameIndex], sizeof(VkDrawIndexedIndirectCommand) * p, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
}

void GroundClutterRenderer::recreatePipelines() {
    if (!vkCtx_ || pipelineLayout_ == VK_NULL_HANDLE) return;
    if (!buildDrawPipeline()) LOG_ERROR("GroundClutterRenderer: failed to rebuild the draw pipeline");
}

void GroundClutterRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    destroyPipeline(device, pipeline_, pipelineLayout_);
    destroy(device, cullPipeline_);
    destroy(device, cullPipelineLayout_);
    destroy(device, cullSetLayout_);
    destroy(device, drawSetLayout_);
    destroy(device, textureSetLayout_);
    destroy(device, descPool_);
    for (auto& t : textures_) t->destroy(device, allocator);
    textures_.clear();
    if (whiteTexture_) { whiteTexture_->destroy(device, allocator); whiteTexture_.reset(); }
    textureSets_.clear();
    for (auto& p : pendingCopies_) destroy(allocator, p.src, p.alloc);
    pendingCopies_.clear();
    for (uint32_t i = 0; i < kFrames; ++i) {
        for (auto& r : retired_[i]) destroy(allocator, r.src, r.alloc);
        retired_[i].clear();
        destroy(allocator, cullUniform_[i], cullUniformAlloc_[i]);
        cullUniformMapped_[i] = nullptr;
        destroy(allocator, visibleBuffer_[i], visibleAlloc_[i]);
        destroy(allocator, commandBuffer_[i], commandAlloc_[i]);
        cullSet_[i] = VK_NULL_HANDLE;
        drawSet_[i] = VK_NULL_HANDLE;
    }
    destroy(allocator, vertexBuffer_, vertexAlloc_);
    destroy(allocator, indexBuffer_, indexAlloc_);
    destroy(allocator, instanceBuffer_, instanceAlloc_);
    destroy(allocator, modelInfoBuffer_, modelInfoAlloc_);
    models_.clear();
    parts_.clear();
    modelByDoodad_.clear();
    vertexData_.clear();
    indexData_.clear();
    commandTemplate_.clear();
    instanceCount_ = 0;
    vkCtx_ = nullptr;
}

}  // namespace rendering
}  // namespace wowee
