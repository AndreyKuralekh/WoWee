// The renderer's half of a stereo frame: the two eye-sized targets, the
// per-frame data of each eye, and the pass that draws the world into them.
//
// These are members of rendering::Renderer, because drawing the world means
// calling Renderer::renderWorld with the renderer's own state pointed
// elsewhere. They live here so that renderer.cpp carries only the calls.

#include "rendering/renderer.hpp"

#include "core/logger.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/volumetric_fog.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/overlay_system.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"
#include "xr/xr_glow.hpp"
#include "xr/xr_hands.hpp"
#include "xr/xr_system.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
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

    // The same, once for both eyes: what the world is recorded against when
    // one recording serves the two of them. It holds the first eye's matrices
    // when the frame is submitted, and the second eye's are copied over them
    // on the GPU between the two passes - see vrRenderEyes.
    VkBuffer sharedUbo[MAX_FRAMES] = {};
    VmaAllocation sharedUboAllocation[MAX_FRAMES] = {};
    void* sharedUboMapped[MAX_FRAMES] = {};
    VkDescriptorSet sharedSet[MAX_FRAMES] = {};

    // The worker threads' command buffers, once more for each eye. The
    // renderer has one set per frame slot, which is enough to record the world
    // once; these are put in their place while an eye is recorded, so the
    // world goes down the same threaded path twice without knowing.
    VkCommandBuffer secondary[kEyeCount][NUM_SECONDARIES][MAX_FRAMES] = {};
    VkCommandPool secondaryPoolSeen = VK_NULL_HANDLE;  // which pools they came from

    // The glow added onto each eye before it goes to the headset.
    xr::EyeGlow glow;
    // The player's hands, drawn at the end of each eye's pass.
    xr::HandRenderer hands;
    bool handsThisFrame = false;

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
        vrEyes_->glow.destroy();
        vrEyes_->hands.destroy();
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
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            if (vrEyes_->sharedUbo[i]) {
                vmaDestroyBuffer(allocator, vrEyes_->sharedUbo[i], vrEyes_->sharedUboAllocation[i]);
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
    // One set for each eye and one that both share, for each frame slot.
    constexpr uint32_t kSets = (VrEyes::kEyeCount + 1) * MAX_FRAMES;
    sizes[0].descriptorCount = kSets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = kSets * 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kSets;
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
            // Copied from as well: the second eye's goes over the shared one.
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(GPUPerFrameData);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &vrEyes_->sharedUbo[i],
                            &vrEyes_->sharedUboAllocation[i], &mapInfo) != VK_SUCCESS) {
            vrDestroyEyes();
            return false;
        }
        vrEyes_->sharedUboMapped[i] = mapInfo.pMappedData;

        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = vrEyes_->pool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &perFrameSetLayout;
        if (vkAllocateDescriptorSets(device, &setInfo, &vrEyes_->sharedSet[i]) != VK_SUCCESS) {
            vrDestroyEyes();
            return false;
        }
    }
    // Without it the eyes are drawn as before, so a failure is only said.
    const VkImageView eyeViews[VrEyes::kEyeCount] = {vrEyes_->color[0].imageView, vrEyes_->color[1].imageView};
    vrEyes_->glow.create(vkCtx, wanted, vkCtx->getSwapchainFormat(), eyeViews);
    // Nor without these: the eyes are drawn without hands.
    vrEyes_->hands.create(vkCtx, vkCtx->getImGuiRenderPass());
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
    // Until this frame turns out to be seen from the character's eyes.
    if (cameraController) {
        cameraController->setPlayerHidden(false);
        cameraController->setPlayerShown(false);
    }
    if (characterRenderer && characterInstanceId) {
        characterRenderer->setInstanceHeadless(characterInstanceId, false);
        if (vrBodyShown_) {
            characterRenderer->setInstanceArmReach(characterInstanceId, 0, false);
            characterRenderer->setInstanceArmReach(characterInstanceId, 1, false);
        }
    }
    vrBodyShown_ = false;
    if (camera) camera->clearPickRay();
    if (!vrGameHandler_) vrInWorld_ = false;
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

    // Straight ahead is wherever the head faces as the world first appears,
    // and again whenever Home is pressed - unless it is being typed somewhere.
    const bool homeDown = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_HOME] &&
                          !(ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput);
    if (!vrInWorld_ || (homeDown && !vrHomeDown_)) xr::system().recenter();
    vrInWorld_ = true;
    vrHomeDown_ = homeDown;

    // Where the head is stood: at the character's eyes, or where the game put
    // its camera. Either way it faces as that camera does round the vertical,
    // so the mouse and the keys turn the view as they turn the character.
    glm::vec3 reference = camera->getPosition();
    const glm::vec3* followed = cameraController ? cameraController->getFollowTarget() : nullptr;
    if (followed && cameraController->isThirdPerson()) {
        const float eyeZ = followed->z + cameraController->followedEyeHeight();
        if (xr::system().firstPerson()) {
            reference = glm::vec3(followed->x, followed->y, eyeZ);
            // The body below the eyes, without the head they are in; vrPlaceBody
            // stands it under them.
            if (xr::system().showBody() && characterRenderer && characterInstanceId) {
                cameraController->setPlayerShown(true);
                characterRenderer->setInstanceHeadless(characterInstanceId, true);
                vrBodyShown_ = true;
            } else {
                cameraController->setPlayerHidden(true);
            }
        } else {
            // The game's camera looks down at the character from above it, and
            // a head does not take its tilt: left at that height it looks out
            // level over a character somewhere below. So behind the character
            // where the camera is, but only a little over its eyes.
            constexpr float kLiftOverEyes = 0.5f;
            reference.z = std::min(reference.z, eyeZ + kLiftOverEyes);
        }
        // Left to itself the camera starts circling a character that stands
        // still, and here that would turn the world round the player's head.
        if (cameraController->isIdleOrbitEnabled()) cameraController->setIdleOrbitEnabled(false);
    }
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
    if (vrBodyShown_) {
        vrHeadOffset_ = glm::vec2(center.position - reference);
        const glm::vec3 looks = -glm::vec3(glm::inverse(center.view)[2]);
        const float flat = std::sqrt(looks.x * looks.x + looks.y * looks.y);
        if (flat > 1e-3f) vrHeadForward_ = glm::vec2(looks.x, looks.y) / flat;
        vrSettleBody();
        // The wrists, for the arms to reach to. Without a headset they are
        // held in front of the chest, the elbows bent.
        const xr::HandPoses& hands = xr::system().handsInWorld();
        const glm::mat3 trackingTurn(glm::normalize(glm::vec3(hands.trackingToWorld[0])),
                                     glm::normalize(glm::vec3(hands.trackingToWorld[1])),
                                     glm::normalize(glm::vec3(hands.trackingToWorld[2])));
        for (int hand = 0; hand < 2; ++hand) {
            glm::mat4 wrist(1.0f);
            constexpr uint32_t kWristJoint = 1;
            if (hands.tracked[hand]) {
                wrist = hands.joint[hand][kWristJoint];
            } else if (hands.fake) {
                // Palms facing, as round a controller: the backs of the hands out.
                wrist = glm::rotate(glm::mat4(1.0f), glm::radians(hand == 0 ? 90.0f : -90.0f), glm::vec3(0, 0, 1));
                wrist[3] = glm::vec4(hand == 0 ? -0.2f : 0.2f, -0.3f, -0.2f, 1.0f);
            } else {
                vrWristSeen_[hand] = false;
                continue;
            }
            vrWristSeen_[hand] = true;
            vrWristOffset_[hand] = glm::vec3(hands.trackingToWorld * wrist[3]) - reference;
            vrWristTurn_[hand] = trackingTurn * glm::mat3(wrist);
            // How far each finger bends towards the palm, as the stock hands
            // are drawn: a hand round a controller at least curled as at rest.
            // Without a headset, about so.
            float* bend = vrFingerBend_[hand];
            if (!hands.tracked[hand]) {
                for (int f = 0; f < 5; ++f) {
                    bend[f * 2] = f == 0 ? 0.3f : 0.6f;
                    bend[f * 2 + 1] = f == 0 ? 0.3f : 0.7f;
                }
                continue;
            }
            std::array<glm::mat4, xr::kHandJointCount> joints = hands.joint[hand];
            if (hands.onController[hand]) xr::relaxHand(joints);
            // The turn of a joint's bone from its parent's, towards the palm:
            // -Z ahead along each bone, -Y the palm side.
            const auto bendAt = [&](uint32_t parent, uint32_t joint) {
                const glm::vec3 ahead = glm::transpose(glm::mat3(joints[parent])) * -glm::vec3(joints[joint][2]);
                return std::clamp(std::atan2(-ahead.y, -ahead.z), 0.0f, 1.8f);
            };
            bend[0] = bendAt(2, 3);  // thumb: metacarpal 2, proximal 3, distal 4
            bend[1] = bendAt(3, 4);
            constexpr uint32_t kMetacarpal[4] = {6, 11, 16, 21};  // index, middle, ring, little
            for (int f = 0; f < 4; ++f) {
                const uint32_t m = kMetacarpal[f];
                bend[2 + f * 2] = bendAt(m, m + 1);
                bend[3 + f * 2] = bendAt(m + 1, m + 2) + 0.5f * bendAt(m + 2, m + 3);
            }
        }
    }

    // What the controller points at is what a click is about.
    Ray pointer{};
    if (xr::system().pickRay(pointer.origin, pointer.direction)) camera->setPickRay(pointer);
}

// The character's body stood under the eyes the headset last saw from: turned
// as the view is round the vertical - the game turns its model only while it
// walks or a mouse button is down - and moved so its neck is behind those eyes
// wherever the head has gone, a lean or a step in the room, or the run's own
// sway. Its own place is unchanged: this is where it is drawn. Called as the
// model is posed for the frame, before its bones; they are the last frame's.
// Not on a mount or a flight, which pose the rider themselves.
void Renderer::vrPlaceBody(float torsoYawDeg) {
    if (!characterRenderer || !characterInstanceId) return;
    // Set here, as the frame is updated, and kept while it is drawn.
    characterRenderer->setInstanceArmsOnly(characterInstanceId, vrBodyShown_ && xr::system().bodyArmsOnly());
    if (!vrBodyShown_ || !cameraController) return;
    if (animationController_ && (animationController_->isMounted() || animationController_->isTaxiFlight())) return;
    // Strafing, the legs go the way it walks and the chest is turned back by
    // the torso's own twist: the model takes the view's yaw less that.
    characterRenderer->setInstanceRotation(characterInstanceId,
                                           glm::vec3(0.0f, 0.0f, glm::radians(cameraController->getYaw() - torsoYawDeg)));
    // The instance was stood on the character for this frame just before.
    glm::vec2 by(0.0f);
    vrBodyOffset_ = glm::vec2(0.0f);
    if (!vrBodyShift(by)) return;
    vrBodyOffset_ = vrBodyOffsetWithin(by);
    characterRenderer->setInstancePosition(characterInstanceId, characterPosition + glm::vec3(vrBodyOffset_, 0.0f));

    // The arms reach for the player's own hands.
    const glm::vec3 eyes = characterPosition + glm::vec3(0.0f, 0.0f, cameraController->followedEyeHeight());
    for (int hand = 0; hand < 2; ++hand) {
        characterRenderer->setInstanceArmReach(characterInstanceId, hand, vrWristSeen_[hand],
                                               eyes + vrWristOffset_[hand], vrWristTurn_[hand], vrFingerBend_[hand]);
    }
}

// How far the body has to move from where it stands now for its neck to be
// behind the eyes.
bool Renderer::vrBodyShift(glm::vec2& by) const {
    constexpr int32_t kKeyBoneHead = 6;
    glm::vec3 neck(0.0f);
    if (!characterRenderer->getInstanceKeyBoneWorld(characterInstanceId, kKeyBoneHead, neck)) return false;
    // Eyes are in front of the neck the head bone turns on, not over it: left
    // there, a look down saw the chest from inside the collar.
    constexpr float kEyesForward = 0.09f;  // of the eye height
    const glm::vec2 wanted = glm::vec2(characterPosition) + vrHeadOffset_ -
                             vrHeadForward_ * (kEyesForward * cameraController->followedEyeHeight());
    by = wanted - glm::vec2(neck);
    return true;
}

// The body's offset from the character, kept within a yard: a head further
// off has gone through a wall or been recentred badly, and the body stays
// with the character.
glm::vec2 Renderer::vrBodyOffsetWithin(glm::vec2 offset) {
    constexpr float kFurthest = 1.0f;
    const float away = glm::length(offset);
    return away > kFurthest ? offset * (kFurthest / away) : offset;
}

// Placed as the frame was updated, the body went by the head of the frame
// before and the neck of bones not yet posed: one frame behind both, it shook
// against a view that never is. Placed again once both are this frame's.
void Renderer::vrSettleBody() {
    if (!characterRenderer || !characterInstanceId || !cameraController) return;
    if (animationController_ && (animationController_->isMounted() || animationController_->isTaxiFlight())) return;
    glm::vec2 by(0.0f);
    if (!vrBodyShift(by)) return;
    const glm::vec2 offset = vrBodyOffsetWithin(vrBodyOffset_ + by);
    characterRenderer->nudgeInstance(characterInstanceId, glm::vec3(offset - vrBodyOffset_, 0.0f));
    vrBodyOffset_ = offset;
}

// Where the head looked in the last stereo frame: the point between the eyes
// and its view-projection, as wide as both eyes. The M2 animation update culls
// against this in a headset - against the game's camera, which in the
// character's eyes stands behind them looking its own way, a spell effect the
// player was looking straight at could sit outside it, keep its bones frozen
// at their first pose and never be seen: Blizzard's shards stayed in the sky.
// False when the last frame was not a stereo one.
bool Renderer::vrLastHeadView(glm::vec3& position, glm::mat4& viewProjection) const {
    if (!vrEyes_ || !vrEyes_->viewsValid || !vrWorldInEyes_) return false;
    const xr::EyeView& center = vrEyes_->views.center;
    position = center.position;
    viewProjection = center.projection * center.view;
    return true;
}

// The second eye of a frame whose world was recorded once: the first eye's
// secondaries again, in its own pass, after its own matrices have been put
// where those secondaries read them.
void Renderer::vrReplayWorldForEye(uint32_t eye) {
    const uint32_t frame = vkCtx->getCurrentFrame();
    const xr::EyeView& view = vrEyes_->views.eye[eye];

    // The buffer was read by the pass before and is read by the one after;
    // the copy is told so on both sides.
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vrEyes_->sharedUbo[frame];
    barrier.size = VK_WHOLE_SIZE;
    constexpr VkPipelineStageFlags kShading =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(currentCmd, kShading, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0,
                         nullptr);
    VkBufferCopy region{};
    region.size = sizeof(GPUPerFrameData);
    vkCmdCopyBuffer(currentCmd, vrEyes_->ubo[eye][frame], vrEyes_->sharedUbo[frame], 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kShading, 0, 0, nullptr, 1, &barrier, 0,
                         nullptr);

    activeFramebuffer_ = vrEyes_->framebuffer[eye];

    // The selection circle is the one thing in the world drawn with matrices
    // handed to it rather than read from set 0, so it is recorded again.
    VkCommandBuffer selection = VK_NULL_HANDLE;
    if (vrWorldSelectionAt_ < vrWorldCmdCount_) {
        std::swap(secondaryCmds_[SEC_SELECTION][frame], vrEyes_->secondary[eye][SEC_SELECTION][frame]);
        selection = beginSecondary(SEC_SELECTION);
        setSecondaryViewportScissor(selection);
        if (overlaySystem_) {
            overlaySystem_->renderSelectionCircle(
                view.view, view.projection, selection,
                terrainManager ? OverlaySystem::HeightQuery2D(
                                     [&](float x, float y) { return terrainManager->getHeightAt(x, y); })
                               : OverlaySystem::HeightQuery2D{},
                wmoRenderer ? OverlaySystem::HeightQuery3D(
                                  [&](float x, float y, float z) { return wmoRenderer->getFloorHeight(x, y, z); })
                            : OverlaySystem::HeightQuery3D{},
                m2Renderer ? OverlaySystem::HeightQuery3D(
                                 [&](float x, float y, float z) { return m2Renderer->getFloorHeight(x, y, z); })
                           : OverlaySystem::HeightQuery3D{});
        }
        vkEndCommandBuffer(selection);
        std::swap(secondaryCmds_[SEC_SELECTION][frame], vrEyes_->secondary[eye][SEC_SELECTION][frame]);
    }

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
    vkCmdBeginRenderPass(currentCmd, &passInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    for (uint32_t i = 0; i < vrWorldCmdCount_; ++i) {
        const VkCommandBuffer buffer = (i == vrWorldSelectionAt_ && selection) ? selection : vrWorldCmds_[i];
        vkCmdExecuteCommands(currentCmd, 1, &buffer);
    }
    vrRecordHands(eye, true);
    vkCmdEndRenderPass(currentCmd);
    if (vrEyes_->handsThisFrame) xr::system().recordHandsCopy(currentCmd, eye, vrEyes_->color[eye].image);
    vrEyes_->glow.record(currentCmd, eye, vrEyes_->color[eye].image, xr::system().glow(), vrEyes_->handsThisFrame);
    xr::system().recordEyeCopy(currentCmd, eye, vrEyes_->color[eye].image);
}

// The fog volume is a grid over one camera's frustum, and every world shader
// reads it through the matrices of set 0 - an eye's, in its pass. So it is
// built for each eye in turn, into the same volume, just before that eye's
// pass: the first eye's pass has read it by the time the second build
// overwrites it, and the second eye then reads its own. The window draws no
// world meanwhile and builds none. True when the eyes will.
bool Renderer::vrFogForEyes(float density, float layerBase) {
    if (!vrEyes_ || !vrEyes_->viewsValid) return false;
    vrFogWanted_ = true;
    vrFogDensity_ = density;
    vrFogLayerBase_ = layerBase;
    return true;
}

void Renderer::vrBuildFogForEye(uint32_t eye) {
    if (!volumetricFog_) return;
    const xr::EyeView& view = vrEyes_->views.eye[eye];
    VolumetricFog::FrameInputs in;
    in.view = view.view;
    in.projection = view.projection;
    in.cameraPos = view.position;
    in.time = globalTime;
    in.density = vrFogDensity_;
    in.layerBase = vrFogLayerBase_;
    // More of the frame before: what is left of the jitter shimmers in a
    // headset, and an eye's history is the other eye's air.
    in.historyWeight = 0.95f;
    const uint32_t frame = vkCtx->getCurrentFrame();
    volumetricFog_->record(currentCmd, frame, perFrameDescSets[frame], in);
}

// The hands, last in an eye's pass, against the depth the world left: into a
// buffer of their own where the pass takes only secondaries, or straight in.
void Renderer::vrRecordHands(uint32_t eye, bool secondaryOnly) {
    if (!vrEyes_->handsThisFrame) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    const xr::EyeView& view = vrEyes_->views.eye[eye];
    if (secondaryOnly) {
        const VkCommandBuffer hands =
            vrEyes_->hands.recordSecondary(frame, eye, vkCtx->getImGuiRenderPass(), vrEyes_->framebuffer[eye],
                                           vrEyes_->extent, view.projection, view.fromTracking);
        if (hands != VK_NULL_HANDLE) vkCmdExecuteCommands(currentCmd, 1, &hands);
        return;
    }
    VkViewport viewport{};
    viewport.width = static_cast<float>(vrEyes_->extent.width);
    viewport.height = static_cast<float>(vrEyes_->extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(currentCmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = vrEyes_->extent;
    vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    vrEyes_->hands.record(currentCmd, frame, view.projection, view.fromTracking);
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

    // Both eyes record the same draws and differ only in the matrices of set
    // 0, so the world is recorded once, for the first eye, and the second eye
    // executes the same buffers. They are bound to one set whose buffer holds
    // the first eye's matrices; the second eye's are copied into it between
    // the passes, on the GPU, where the order of the two is kept.
    // WOWEE_VR_EYE_REPLAY=0 records each eye for itself, as before.
    static const bool replayWanted = [] {
        const char* v = std::getenv("WOWEE_VR_EYE_REPLAY");
        return !(v && *v == '0');
    }();
    const bool shared = threaded && replayWanted;
    vrWorldShared_ = shared;
    const auto eyesStart = std::chrono::steady_clock::now();

    // The hands are the same for both eyes; only the matrix they are seen by
    // differs, and that is handed to each eye's draw.
    vrEyes_->handsThisFrame =
        xr::system().showHands() && !vrBodyShown_ &&
        vrEyes_->hands.prepare(frame, xr::system().handsInWorld(), glm::vec3(currentFrameData.lightDir),
                               glm::vec3(currentFrameData.lightColor), glm::vec3(currentFrameData.ambientColor),
                               xr::system().handTone());

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
        // The fog is built for this eye just before its pass; the ray traced
        // light is built for one camera, the window's.
        data.volumetricParams = vrFogWanted_ ? volumetricFog_->frameParams() : glm::vec4(0.0f);
        data.rtParams = glm::vec4(0.0f);
        std::memcpy(vrEyes_->uboMapped[eye][frame], &data, sizeof(GPUPerFrameData));
        if (shared && eye == 0) std::memcpy(vrEyes_->sharedUboMapped[frame], &data, sizeof(GPUPerFrameData));

        if (vrFogWanted_) vrBuildFogForEye(eye);
        if (shared && eye == 1) {
            vrReplayWorldForEye(eye);
            continue;
        }

        // Everything but the buffer is the frame's own, copied each frame: the
        // renderer rewrites those bindings when a setting changes them.
        const VkDescriptorSet set = shared ? vrEyes_->sharedSet[frame] : vrEyes_->set[eye][frame];
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = shared ? vrEyes_->sharedUbo[frame] : vrEyes_->ubo[eye][frame];
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
        vrRecordHands(eye, threaded);

        vkCmdEndRenderPass(currentCmd);
        if (threaded) swapSecondaries();
        if (vrEyes_->handsThisFrame) xr::system().recordHandsCopy(currentCmd, eye, vrEyes_->color[eye].image);
        vrEyes_->glow.record(currentCmd, eye, vrEyes_->color[eye].image, xr::system().glow(),
                             vrEyes_->handsThisFrame);
        xr::system().recordEyeCopy(currentCmd, eye, vrEyes_->color[eye].image);
    }

    // WOWEE_FRAME_PROFILE: what the two eyes cost the main thread together.
    static const bool profile = [] {
        const char* v = std::getenv("WOWEE_FRAME_PROFILE");
        return v && *v && *v != '0';
    }();
    if (profile) {
        static auto lastSaid = std::chrono::steady_clock::now();
        static double total = 0.0;
        static uint32_t frames = 0;
        const auto now = std::chrono::steady_clock::now();
        total += std::chrono::duration<double, std::milli>(now - eyesStart).count();
        ++frames;
        if (now - lastSaid > std::chrono::seconds(10)) {
            LOG_WARNING("  VR eyes: ", total / frames, "ms a frame over ", frames, " frames, ",
                        shared ? "one recording for both" : "each recorded for itself");
            lastSaid = now;
            total = 0.0;
            frames = 0;
        }
    }

    vrWorldShared_ = false;
    vrFogWanted_ = false;
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
