// The player's hands in the eyes. See xr/xr_hands.hpp.

#include "xr/xr_hands.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::xr {

namespace {

struct HandVertex {
    float position[3];
    float normal[3];
    uint8_t joints[4];
    float weights[4];
};

// Set 0, binding 0: what both eyes of a frame draw.
struct HandUniforms {
    glm::mat4 skin[2 * 25];  // each hand's joints, the left hand's first
    glm::vec4 lightDir;      // towards the ground, as the world's is
    glm::vec4 lightColor;
    glm::vec4 ambientColor;
    glm::vec4 skinColor;
};

struct HandPush {
    glm::mat4 viewProjection;
    glm::vec4 eye;  // xyz the eye, w the first skin matrix of the hand drawn
};

// The model's joints are named as WebXR names them; OpenXR numbers the same
// joints in the same order, after the palm.
const std::unordered_map<std::string, uint32_t>& openXrJointByName() {
    static const std::unordered_map<std::string, uint32_t> names = [] {
        std::unordered_map<std::string, uint32_t> m;
        m["wrist"] = 1;
        const char* fingers[] = {"index-finger", "middle-finger", "ring-finger", "pinky-finger"};
        m["thumb-metacarpal"] = 2;
        m["thumb-phalanx-proximal"] = 3;
        m["thumb-phalanx-distal"] = 4;
        m["thumb-tip"] = 5;
        for (uint32_t f = 0; f < 4; ++f) {
            const std::string base = fingers[f];
            const uint32_t first = 6 + f * 5;
            m[base + "-metacarpal"] = first;
            m[base + "-phalanx-proximal"] = first + 1;
            m[base + "-phalanx-intermediate"] = first + 2;
            m[base + "-phalanx-distal"] = first + 3;
            m[base + "-tip"] = first + 4;
        }
        return m;
    }();
    return names;
}

// Just enough glTF binary to read one skinned mesh: the JSON chunk, the
// buffer chunk, and accessors into the buffer.
class Glb {
public:
    bool load(const char* path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        data_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (data_.size() < 28 || std::memcmp(data_.data(), "glTF", 4) != 0) return false;
        const uint32_t jsonLength = u32(12);
        if (u32(16) != 0x4E4F534Au || 20 + jsonLength + 8 > data_.size()) return false;  // "JSON"
        json_ = nlohmann::json::parse(data_.begin() + 20, data_.begin() + 20 + jsonLength, nullptr, false);
        if (json_.is_discarded()) return false;
        const size_t binHeader = 20 + jsonLength;
        const uint32_t binLength = u32(binHeader);
        if (u32(binHeader + 4) != 0x004E4942u || binHeader + 8 + binLength > data_.size()) return false;  // "BIN"
        bin_ = data_.data() + binHeader + 8;
        binLength_ = binLength;
        return true;
    }

    const nlohmann::json& json() const { return json_; }

    /// Element `i` of an accessor as floats or integers; false when it runs
    /// outside the buffer or has a type not read here.
    template <typename T>
    bool read(uint32_t accessor, std::vector<T>& out, uint32_t& components) const {
        const auto& a = json_["accessors"].at(accessor);
        const std::string type = a.value("type", "");
        components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4
                   : type == "MAT4"   ? 16 : 0;
        const uint32_t componentType = a.value("componentType", 0u);
        const uint32_t componentSize = componentType == 5126 || componentType == 5125 ? 4
                                     : componentType == 5123 || componentType == 5122 ? 2
                                     : componentType == 5121 || componentType == 5120 ? 1 : 0;
        const uint32_t count = a.value("count", 0u);
        if (!components || !componentSize || !a.contains("bufferView")) return false;
        const auto& view = json_["bufferViews"].at(a["bufferView"].get<uint32_t>());
        const size_t base = view.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
        const size_t stride = view.value("byteStride", size_t{components * componentSize});
        if (count == 0 || base + stride * (count - 1) + components * componentSize > binLength_) return false;
        out.resize(static_cast<size_t>(count) * components);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* element = bin_ + base + stride * i;
            for (uint32_t c = 0; c < components; ++c) {
                const uint8_t* p = element + c * componentSize;
                T value{};
                switch (componentType) {
                    case 5126: { float f; std::memcpy(&f, p, 4); value = static_cast<T>(f); break; }
                    case 5125: { uint32_t u; std::memcpy(&u, p, 4); value = static_cast<T>(u); break; }
                    case 5123: { uint16_t u; std::memcpy(&u, p, 2); value = static_cast<T>(u); break; }
                    case 5121: value = static_cast<T>(*p); break;
                    default: return false;
                }
                out[static_cast<size_t>(i) * components + c] = value;
            }
        }
        return true;
    }

private:
    uint32_t u32(size_t at) const {
        uint32_t v;
        std::memcpy(&v, data_.data() + at, 4);
        return v;
    }

    std::vector<uint8_t> data_;
    nlohmann::json json_;
    const uint8_t* bin_ = nullptr;
    size_t binLength_ = 0;
};

glm::mat4 nodeLocal(const nlohmann::json& node) {
    if (node.contains("matrix")) {
        std::vector<float> m = node["matrix"].get<std::vector<float>>();
        if (m.size() == 16) return glm::make_mat4(m.data());
    }
    glm::mat4 result(1.0f);
    if (node.contains("translation")) {
        const auto t = node["translation"].get<std::vector<float>>();
        if (t.size() == 3) result = glm::translate(result, glm::vec3(t[0], t[1], t[2]));
    }
    if (node.contains("rotation")) {
        const auto r = node["rotation"].get<std::vector<float>>();  // x, y, z, w
        if (r.size() == 4) result *= glm::mat4_cast(glm::quat(r[3], r[0], r[1], r[2]));
    }
    if (node.contains("scale")) {
        const auto s = node["scale"].get<std::vector<float>>();
        if (s.size() == 3) result = glm::scale(result, glm::vec3(s[0], s[1], s[2]));
    }
    return result;
}

// Skin, not the world's: tones from pale to dark, in the display encoded
// colours the world is drawn in.
glm::vec3 skinColor(float tone) {
    const glm::vec3 pale(0.93f, 0.78f, 0.68f);
    const glm::vec3 dark(0.36f, 0.24f, 0.17f);
    return glm::mix(pale, dark, tone);
}

// Without a headset the hands are held up in front of the eyes, fingers
// forward: the model hangs them down its -Y, and a quarter turn round X takes
// that to the tracking space's forward, -Z.
glm::mat4 fakePlacement(uint32_t hand) {
    const float side = hand == 0 ? -1.0f : 1.0f;
    return glm::translate(glm::mat4(1.0f), glm::vec3(side * 0.14f, -0.22f, -0.32f)) *
           glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
}

}  // namespace

bool HandRenderer::loadMesh(uint32_t hand, const char* path) {
    Glb glb;
    if (!glb.load(path)) {
        LOG_WARNING("VR hands: ", path, " is missing or not a glTF binary");
        return false;
    }
    const auto& j = glb.json();
    if (!j.contains("meshes") || !j.contains("skins") || j["skins"].empty()) {
        LOG_WARNING("VR hands: ", path, " has no skinned mesh");
        return false;
    }
    const auto& primitive = j["meshes"][0]["primitives"][0];
    const auto& attributes = primitive["attributes"];
    std::vector<float> positions, normals, weights, inverseBind;
    std::vector<uint32_t> joints, indices;
    uint32_t pc = 0, nc = 0, jc = 0, wc = 0, ic = 0, bc = 0;
    const auto& skin = j["skins"][0];
    if (!attributes.contains("POSITION") || !attributes.contains("NORMAL") || !attributes.contains("JOINTS_0") ||
        !attributes.contains("WEIGHTS_0") || !primitive.contains("indices") ||
        !glb.read(attributes["POSITION"].get<uint32_t>(), positions, pc) || pc != 3 ||
        !glb.read(attributes["NORMAL"].get<uint32_t>(), normals, nc) || nc != 3 ||
        !glb.read(attributes["JOINTS_0"].get<uint32_t>(), joints, jc) || jc != 4 ||
        !glb.read(attributes["WEIGHTS_0"].get<uint32_t>(), weights, wc) || wc != 4 ||
        !glb.read(primitive["indices"].get<uint32_t>(), indices, ic) || ic != 1 ||
        !glb.read(skin["inverseBindMatrices"].get<uint32_t>(), inverseBind, bc) || bc != 16) {
        LOG_WARNING("VR hands: ", path, " - the mesh could not be read");
        return false;
    }

    // Each joint's name says which OpenXR joint moves it.
    const auto& skinJoints = skin["joints"];
    const auto& nodes = j["nodes"];
    if (skinJoints.size() != kModelJoints || inverseBind.size() != kModelJoints * 16) {
        LOG_WARNING("VR hands: ", path, " has ", skinJoints.size(), " joints, not ", kModelJoints);
        return false;
    }
    // Where each node sits in the model, through its parents.
    std::vector<int> parent(nodes.size(), -1);
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (!nodes[n].contains("children")) continue;
        for (const auto& child : nodes[n]["children"]) {
            const size_t c = child.get<size_t>();
            if (c < parent.size()) parent[c] = static_cast<int>(n);
        }
    }
    const auto global = [&](size_t n) {
        glm::mat4 m = nodeLocal(nodes[n]);
        for (int p = parent[n]; p >= 0; p = parent[static_cast<size_t>(p)]) m = nodeLocal(nodes[p]) * m;
        return m;
    };
    Mesh& mesh = mesh_[hand];
    for (uint32_t i = 0; i < kModelJoints; ++i) {
        const size_t node = skinJoints[i].get<size_t>();
        const std::string name = node < nodes.size() ? nodes[node].value("name", "") : "";
        const auto found = openXrJointByName().find(name);
        if (found == openXrJointByName().end()) {
            LOG_WARNING("VR hands: ", path, " - joint '", name, "' is not one OpenXR reports");
            return false;
        }
        mesh.xrJoint[i] = found->second;
        mesh.inverseBind[i] = glm::make_mat4(inverseBind.data() + i * 16);
        mesh.bindPose[i] = global(node);
    }

    const size_t vertexCount = positions.size() / 3;
    if (normals.size() != vertexCount * 3 || joints.size() != vertexCount * 4 || weights.size() != vertexCount * 4) {
        LOG_WARNING("VR hands: ", path, " - attributes of different lengths");
        return false;
    }
    std::vector<HandVertex> vertices(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v) {
        HandVertex& out = vertices[v];
        for (int c = 0; c < 3; ++c) {
            out.position[c] = positions[v * 3 + c];
            out.normal[c] = normals[v * 3 + c];
        }
        for (int c = 0; c < 4; ++c) {
            const uint32_t joint = joints[v * 4 + c];
            out.joints[c] = static_cast<uint8_t>(joint < kModelJoints ? joint : 0);
            out.weights[c] = joint < kModelJoints ? weights[v * 4 + c] : 0.0f;
        }
    }
    for (uint32_t& index : indices) {
        if (index >= vertexCount) index = 0;
    }

    // Small enough to be read from where the CPU wrote it.
    const VmaAllocator allocator = ctx_->getAllocator();
    mesh.vertices = rendering::createBuffer(allocator, vertices.size() * sizeof(HandVertex),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    mesh.indices = rendering::createBuffer(allocator, indices.size() * sizeof(uint32_t),
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    if (!mesh.vertices.info.pMappedData || !mesh.indices.info.pMappedData) {
        LOG_WARNING("VR hands: buffers not created");
        return false;
    }
    std::memcpy(mesh.vertices.info.pMappedData, vertices.data(), vertices.size() * sizeof(HandVertex));
    std::memcpy(mesh.indices.info.pMappedData, indices.data(), indices.size() * sizeof(uint32_t));
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

bool HandRenderer::create(rendering::VkContext* ctx, VkRenderPass pass) {
    destroy();
    if (!ctx || pass == VK_NULL_HANDLE) return false;
    ctx_ = ctx;
    const VkDevice device = ctx->getDevice();

    if (!loadMesh(0, "assets/vr/hands/left.glb") || !loadMesh(1, "assets/vr/hands/right.glb")) {
        LOG_WARNING("VR hands: no model - no hands");
        destroy();
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    setLayout_ = rendering::createDescriptorSetLayout(device, {binding});
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(HandPush)};
    layout_ = rendering::createPipelineLayout(device, {setLayout_}, {push});

    rendering::VkShaderModule vert;
    rendering::VkShaderModule frag;
    if (!setLayout_ || !layout_ || !vert.loadFromFile(device, "assets/shaders/vr_hand.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/vr_hand.frag.spv")) {
        LOG_WARNING("VR hands: shaders or layouts not created - no hands");
        destroy();
        return false;
    }
    const VkVertexInputBindingDescription vertexBinding{0, sizeof(HandVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::vector<VkVertexInputAttributeDescription> attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HandVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HandVertex, normal)},
        {2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(HandVertex, joints)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HandVertex, weights)},
    };
    pipeline_ = rendering::PipelineBuilder()
                    .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                                frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                    .setVertexInput({vertexBinding}, attributes)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                    // Both sides: the winding of a mirrored hand is not worth a guess.
                    .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                    .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setColorBlendAttachment(rendering::PipelineBuilder::blendDisabled())
                    .setMultisample(VK_SAMPLE_COUNT_1_BIT)
                    .setLayout(layout_)
                    .setRenderPass(pass)
                    .setDynamicStates(rendering::viewportAndScissorDynamic())
                    .build(device, ctx->getPipelineCache());
    vert.destroy();
    frag.destroy();
    if (!pipeline_) {
        LOG_WARNING("VR hands: pipeline not created - no hands");
        destroy();
        return false;
    }

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFrames;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = ctx->getGraphicsQueueFamily();
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS ||
        vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        LOG_WARNING("VR hands: pools not created - no hands");
        destroy();
        return false;
    }
    for (uint32_t f = 0; f < kFrames; ++f) {
        ubo_[f] = rendering::createBuffer(ctx->getAllocator(), sizeof(HandUniforms),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &setLayout_;
        if (!ubo_[f].info.pMappedData || vkAllocateDescriptorSets(device, &alloc, &set_[f]) != VK_SUCCESS) {
            LOG_WARNING("VR hands: buffers or sets not created - no hands");
            destroy();
            return false;
        }
        VkDescriptorBufferInfo bufferInfo{ubo_[f].buffer, 0, sizeof(HandUniforms)};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_[f];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    commandInfo.commandBufferCount = kEyeCount * kFrames;
    if (vkAllocateCommandBuffers(device, &commandInfo, &secondary_[0][0]) != VK_SUCCESS) {
        LOG_WARNING("VR hands: command buffers not created - no hands");
        destroy();
        return false;
    }
    LOG_INFO("VR hands: ", mesh_[0].indexCount / 3, " and ", mesh_[1].indexCount / 3, " triangles");
    return true;
}

void HandRenderer::destroy() {
    if (!ctx_) return;
    const VkDevice device = ctx_->getDevice();
    const VmaAllocator allocator = ctx_->getAllocator();
    if (commandPool_) vkDestroyCommandPool(device, commandPool_, nullptr);  // and its buffers
    commandPool_ = VK_NULL_HANDLE;
    std::memset(secondary_, 0, sizeof(secondary_));
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (ubo_[f].buffer) rendering::destroyBuffer(allocator, ubo_[f]);
        ubo_[f] = {};
        set_[f] = VK_NULL_HANDLE;
    }
    for (Mesh& mesh : mesh_) {
        if (mesh.vertices.buffer) rendering::destroyBuffer(allocator, mesh.vertices);
        if (mesh.indices.buffer) rendering::destroyBuffer(allocator, mesh.indices);
        mesh = {};
    }
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

bool HandRenderer::prepare(uint32_t frame, const HandPoses& poses, const glm::vec3& lightDir,
                           const glm::vec3& lightColor, const glm::vec3& ambientColor, float tone) {
    if (!ready() || frame >= kFrames) return false;
    auto* uniforms = static_cast<HandUniforms*>(ubo_[frame].info.pMappedData);
    bool any = false;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        const Mesh& mesh = mesh_[hand];
        draw_[frame][hand] = poses.tracked[hand] || poses.fake;
        if (!draw_[frame][hand]) continue;
        any = true;
        for (uint32_t i = 0; i < kModelJoints; ++i) {
            // Where the joint is now, from where it was when the mesh was bound
            // to it; held still without a headset.
            const glm::mat4 joint = poses.tracked[hand]
                                        ? poses.joint[hand][mesh.xrJoint[i]]
                                        : poses.trackingToWorld * fakePlacement(hand) * mesh.bindPose[i];
            uniforms->skin[hand * kModelJoints + i] = joint * mesh.inverseBind[i];
        }
    }
    if (!any) return false;
    uniforms->lightDir = glm::vec4(lightDir, 0.0f);
    uniforms->lightColor = glm::vec4(lightColor, 0.0f);
    uniforms->ambientColor = glm::vec4(ambientColor, 0.0f);
    uniforms->skinColor = glm::vec4(skinColor(tone), 1.0f);
    return true;
}

void HandRenderer::record(VkCommandBuffer cmd, uint32_t frame, const glm::mat4& viewProjection,
                          const glm::vec3& eyePosition) const {
    if (!ready() || frame >= kFrames) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_[frame], 0, nullptr);
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (!draw_[frame][hand]) continue;
        const Mesh& mesh = mesh_[hand];
        const HandPush push{viewProjection, glm::vec4(eyePosition, static_cast<float>(hand * kModelJoints))};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
    }
}

VkCommandBuffer HandRenderer::recordSecondary(uint32_t frame, uint32_t eye, VkRenderPass pass,
                                              VkFramebuffer framebuffer, VkExtent2D extent,
                                              const glm::mat4& viewProjection, const glm::vec3& eyePosition) {
    if (!ready() || frame >= kFrames || eye >= kEyeCount) return VK_NULL_HANDLE;
    VkCommandBuffer cmd = secondary_[eye][frame];
    VkCommandBufferInheritanceInfo inheritance{};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.renderPass = pass;
    inheritance.subpass = 0;
    inheritance.framebuffer = framebuffer;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin.pInheritanceInfo = &inheritance;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    record(cmd, frame, viewProjection, eyePosition);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    return cmd;
}

}  // namespace wowee::xr
