// The frame half of XrSystem: the runtime's frame loop, the eye swapchains a
// stereo frame is copied into, and the panel the window is shown on.

#include "xr/xr_system.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace wowee::xr {

namespace {

constexpr uint32_t kEyeCount = 2;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t imageIndex = 0;
    bool acquired = false;
};

// A colour swapchain and its images. False leaves `out` without a handle.
bool createColorSwapchain(XrSession session, VkFormat format, uint32_t width, uint32_t height,
                          EyeSwapchain& out) {
    XrSwapchainCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    out = {};
    if (XR_FAILED(xrCreateSwapchain(session, &createInfo, &out.handle))) {
        out.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(out.handle, 0, &imageCount, nullptr);
    XrSwapchainImageVulkanKHR blank{};
    blank.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    std::vector<XrSwapchainImageVulkanKHR> images(imageCount, blank);
    if (imageCount == 0 ||
        XR_FAILED(xrEnumerateSwapchainImages(out.handle, imageCount, &imageCount,
                                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
        xrDestroySwapchain(out.handle);
        out = {};
        return false;
    }
    for (const XrSwapchainImageVulkanKHR& image : images) out.images.push_back(image.image);
    return true;
}

bool acquireImage(EyeSwapchain& chain) {
    XrSwapchainImageWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(chain.handle, nullptr, &chain.imageIndex))) return false;
    chain.acquired = true;
    return XR_SUCCEEDED(xrWaitSwapchainImage(chain.handle, &waitInfo));
}

// The smaller way round from one angle to another.
float angleBetween(float from, float to) {
    float d = std::fmod(to - from, glm::two_pi<float>());
    if (d > glm::pi<float>()) d -= glm::two_pi<float>();
    if (d < -glm::pi<float>()) d += glm::two_pi<float>();
    return d;
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace

struct XrSystem::FrameData {
    std::array<EyeSwapchain, kEyeCount> eyes;
    VkFormat eyeFormat = VK_FORMAT_UNDEFINED;

    // The panel: the window's image, its own size, shown as a flat sheet in
    // front of the player. It is the whole interface - and before the world is
    // entered, the whole picture.
    EyeSwapchain panel;
    VkExtent2D panelExtent{};
    bool panelCopied = false;
    // Whether the last frame had a world under the panel. Before there is one
    // - the login screen, the character list, a loading screen - the panel is
    // the whole picture, and it stands still to be looked at whatever way of
    // following the player has chosen for the game.
    bool worldShown = false;
    // Where the sheet stands, in the tracking space: turned by `panelYaw`
    // round the vertical about `panelAnchor`, the head it was last placed for.
    // The targets are where it is heading; it follows them over a moment
    // rather than jumping.
    bool panelPlaced = false;
    float panelYaw = 0.0f;
    float panelTargetYaw = 0.0f;
    glm::vec3 panelAnchor{0.0f};
    glm::vec3 panelTargetAnchor{0.0f};
    std::chrono::steady_clock::time_point panelMoved = std::chrono::steady_clock::now();

    // The controller's ray: a few pixels of one colour, filled once and then
    // shown as a long thin sheet wherever the controller points.
    EyeSwapchain beam;
    bool beamFilling = false;
    bool beamFilled = false;

    // The frame in progress.
    bool begun = false;
    bool shouldRender = false;
    // The finished eye images of a stereo frame, as the renderer handed them over.
    std::array<VkImage, kEyeCount> eyeSource{};
    uint32_t eyesCopied = 0;
    XrTime displayTime = 0;
    std::array<XrView, kEyeCount> views{};

    // Once-a-second line under WOWEE_VR_DEBUG.
    std::chrono::steady_clock::time_point statsStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point cpuStart{};
    XrTime lastDisplayTime = 0;
    uint32_t frames = 0;
    uint32_t notRendered = 0;
    uint32_t missed = 0;
    double cpuMs = 0.0;
    double waitMs = 0.0;
    std::array<double, static_cast<size_t>(XrSystem::Timing::Count)> timing{};
};

namespace {

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

void XrSystem::addTiming(Timing which, double milliseconds) {
    if (frame_ && running_) frame_->timing[static_cast<size_t>(which)] += milliseconds;
}

XrSystem::XrSystem() = default;
XrSystem::~XrSystem() = default;

bool XrSystem::createEyeSwapchains() {
    frame_ = std::make_unique<FrameData>();

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    if (formatCount == 0 ||
        XR_FAILED(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()))) {
        LOG_WARNING("VR: the runtime lists no swapchain formats");
        return false;
    }
    // sRGB first: the client's colours are display encoded, and an sRGB
    // swapchain is how the compositor is told so.
    for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                  VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(wanted)) != formats.end()) {
            frame_->eyeFormat = wanted;
            break;
        }
    }
    if (frame_->eyeFormat == VK_FORMAT_UNDEFINED) {
        LOG_WARNING("VR: the runtime offers no 8-bit RGBA swapchain format");
        return false;
    }

    for (EyeSwapchain& eye : frame_->eyes) {
        if (!createColorSwapchain(session_, frame_->eyeFormat, eyeWidth_, eyeHeight_, eye)) {
            LOG_WARNING("VR: could not create an eye swapchain");
            return false;
        }
    }

    // Not worth refusing VR over: without it the pointer has no ray, only the
    // cursor on the panel.
    if (!createColorSwapchain(session_, frame_->eyeFormat, 4, 4, frame_->beam)) {
        LOG_WARNING("VR: could not create the pointer ray's swapchain");
    }

    if (debug_) {
        LOG_WARNING("VR: eye swapchains ", eyeWidth_, "x", eyeHeight_, " format ",
                    static_cast<int>(frame_->eyeFormat), ", ", frame_->eyes[0].images.size(), " images each");
    }
    return true;
}

void XrSystem::destroyEyeSwapchains() {
    if (!frame_) return;
    // The last frame's copies may still be on the queue.
    if (ctx_) vkDeviceWaitIdle(ctx_->getDevice());
    for (EyeSwapchain& eye : frame_->eyes) {
        if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
    }
    if (frame_->panel.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->panel.handle);
    if (frame_->beam.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->beam.handle);
    frame_.reset();
}

void XrSystem::beginSession() {
    XrSessionBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const XrResult result = xrBeginSession(session_, &beginInfo);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrBeginSession failed: ", resultName(result));
        return;
    }
    running_ = true;
    // The window stops waiting for its own display; see VkContext::createSwapchain.
    if (ctx_) ctx_->markSwapchainDirty();
    LOG_WARNING("VR: session running");
}

void XrSystem::endSession() {
    if (!running_) return;
    if (frame_ && frame_->begun) endFrame(false);
    xrEndSession(session_);
    running_ = false;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = false;
    if (ctx_) ctx_->markSwapchainDirty();
    LOG_WARNING("VR: session stopped");
}

// A headset that is not there: two eyes 64 mm apart looking straight ahead,
// with the lopsided fields of view a real one has. Enough to draw a stereo
// frame and look at both halves of it in the window.
void XrSystem::startFake() {
    fake_ = true;
    frame_ = std::make_unique<FrameData>();
    eyeWidth_ = 1344;
    eyeHeight_ = 1440;
    if (!leanRefused_) leanForced_ = true;
    LOG_WARNING("VR: fake headset - stereo is drawn side by side into the window");
}

bool XrSystem::stereoFrame() const {
    return frame_ && frame_->begun && frame_->shouldRender && (fake_ || running_);
}

bool XrSystem::stereoViews(const glm::vec3& referencePosition, float referenceYawDegrees,
                           float nearPlane, float farPlane, StereoViews& out) const {
    if (!stereoFrame()) return false;
    const FrameData& f = *frame_;

    // The tracking space, stood in the world. The world has Z up; the headset
    // has Y up, X to the right and looks down -Z.
    constexpr float kYardsPerMetre = 1.0f / 0.9144f;
    const float yaw = glm::radians(referenceYawDegrees);
    const glm::vec3 forward(std::cos(yaw), std::sin(yaw), 0.0f);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::cross(forward, up);
    const glm::mat3 basis(right, up, -forward);

    // A view matrix for a pose given in tracking space. Rigid: the scale goes
    // into the position only, so depth stays in yards.
    // What the game's camera has already been turned by is in the reference
    // yaw, and comes off here with what recentring took.
    const glm::quat recentre = glm::angleAxis(-(headOriginYaw_ + bodyYaw_), glm::vec3(0.0f, 1.0f, 0.0f));
    const auto place = [&](const XrPosef& pose, EyeView& view) {
        const glm::vec3 p = recentre * (glm::vec3(pose.position.x, pose.position.y, pose.position.z) - headOrigin_);
        const glm::quat q = recentre * glm::quat(pose.orientation.w, pose.orientation.x, pose.orientation.y,
                                                 pose.orientation.z);
        view.position = referencePosition + basis * (p * kYardsPerMetre);
        glm::mat4 eyeToWorld(basis * glm::mat3_cast(q));
        eyeToWorld[3] = glm::vec4(view.position, 1.0f);
        view.view = glm::inverse(eyeToWorld);
    };
    // Depth 0..1, and Y turned over for Vulkan, as Camera does for its own.
    const auto project = [&](const XrFovf& fov) {
        glm::mat4 p = glm::frustumRH_ZO(nearPlane * std::tan(fov.angleLeft), nearPlane * std::tan(fov.angleRight),
                                        nearPlane * std::tan(fov.angleDown), nearPlane * std::tan(fov.angleUp),
                                        nearPlane, farPlane);
        p[1][1] *= -1.0f;
        p[2][1] *= -1.0f;
        return p;
    };

    for (uint32_t i = 0; i < kEyeCount; ++i) {
        place(f.views[i].pose, out.eye[i]);
        out.eye[i].projection = project(f.views[i].fov);
    }

    // Between the eyes, turned as the left one is, and as wide as both
    // together with a few degrees to spare for the distance between them.
    constexpr float kMargin = 0.09f;
    XrPosef centre = f.views[0].pose;
    centre.position.x = 0.5f * (f.views[0].pose.position.x + f.views[1].pose.position.x);
    centre.position.y = 0.5f * (f.views[0].pose.position.y + f.views[1].pose.position.y);
    centre.position.z = 0.5f * (f.views[0].pose.position.z + f.views[1].pose.position.z);
    XrFovf wide{};
    wide.angleLeft = std::min(f.views[0].fov.angleLeft, f.views[1].fov.angleLeft) - kMargin;
    wide.angleRight = std::max(f.views[0].fov.angleRight, f.views[1].fov.angleRight) + kMargin;
    wide.angleDown = std::min(f.views[0].fov.angleDown, f.views[1].fov.angleDown) - kMargin;
    wide.angleUp = std::max(f.views[0].fov.angleUp, f.views[1].fov.angleUp) + kMargin;
    place(centre, out.center);
    out.center.projection = project(wide);
    return true;
}

void XrSystem::recordEyeCopy(VkCommandBuffer cmd, uint32_t eye, VkImage image) {
    if (!stereoFrame() || eye >= kEyeCount) return;
    FrameData& f = *frame_;
    transition(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    f.eyeSource[eye] = image;
    f.eyesCopied |= 1u << eye;
    if (fake_) return;

    // Bytes across, not a blit: the image is in the window's linear format and
    // already display encoded, and the eye image is its sRGB twin.
    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    VkImageCopy copy{};
    copy.srcSubresource = layers;
    copy.dstSubresource = layers;
    copy.extent = {eyeWidth_, eyeHeight_, 1};
    const VkImage target = f.eyes[eye].images[f.eyes[eye].imageIndex];
    transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void XrSystem::beginFrame() {
    if (fake_) {
        FrameData& f = *frame_;
        f.begun = true;
        f.shouldRender = true;
        f.eyesCopied = 0;
        for (uint32_t i = 0; i < kEyeCount; ++i) {
            const float side = i == 0 ? -1.0f : 1.0f;
            f.views[i] = {};
            f.views[i].type = XR_TYPE_VIEW;
            f.views[i].pose.orientation.w = 1.0f;
            f.views[i].pose.position.x = side * 0.032f;
            // Wider outwards than towards the nose, and further down than up.
            f.views[i].fov.angleLeft = i == 0 ? -0.94f : -0.70f;
            f.views[i].fov.angleRight = i == 0 ? 0.70f : 0.94f;
            f.views[i].fov.angleUp = 0.77f;
            f.views[i].fov.angleDown = -0.96f;
        }
        return;
    }
    if (instance_ == XR_NULL_HANDLE) return;
    pollEvents();
    if (!running_ || !frame_) return;

    FrameData& f = *frame_;
    // A frame the client began and never ended: close it, or the runtime
    // refuses the next one.
    if (f.begun) endFrame(false);

    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    const auto waitStart = std::chrono::steady_clock::now();
    XrResult result = xrWaitFrame(session_, nullptr, &frameState);
    f.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrWaitFrame failed: ", resultName(result));
        return;
    }
    const auto beginStart = std::chrono::steady_clock::now();
    result = xrBeginFrame(session_, nullptr);
    addTiming(Timing::XrBegin, millisecondsSince(beginStart));
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrBeginFrame failed: ", resultName(result));
        return;
    }
    f.begun = true;
    f.panelCopied = false;
    f.eyesCopied = 0;
    f.cpuStart = std::chrono::steady_clock::now();
    f.displayTime = frameState.predictedDisplayTime;
    f.shouldRender = frameState.shouldRender == XR_TRUE;

    // A gap of more than one period between two predicted display times is a
    // frame the headset had to show twice.
    if (f.lastDisplayTime != 0 && frameState.predictedDisplayPeriod > 0) {
        const XrDuration gap = f.displayTime - f.lastDisplayTime;
        const int64_t periods = (gap + frameState.predictedDisplayPeriod / 2) / frameState.predictedDisplayPeriod;
        if (periods > 1) f.missed += static_cast<uint32_t>(periods - 1);
    }
    f.lastDisplayTime = f.displayTime;

    if (!f.shouldRender) return;

    XrViewLocateInfo locateInfo{};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = f.displayTime;
    locateInfo.space = localSpace_;
    XrViewState viewState{};
    viewState.type = XR_TYPE_VIEW_STATE;
    for (XrView& view : f.views) {
        view = {};
        view.type = XR_TYPE_VIEW;
    }
    uint32_t viewCount = 0;
    result = xrLocateViews(session_, &locateInfo, &viewState, kEyeCount, &viewCount, f.views.data());
    const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(result) || viewCount != kEyeCount || (viewState.viewStateFlags & needed) != needed) {
        f.shouldRender = false;
        return;
    }

    // The pointer is the system's, and the system's is not in the picture:
    // have the interface draw its own while the picture is in the headset.
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = true;

    const auto imagesStart = std::chrono::steady_clock::now();
    for (EyeSwapchain& eye : f.eyes) {
        if (!acquireImage(eye)) {
            f.shouldRender = false;
            break;
        }
    }

    // The panel is the window's size, whatever that is this frame.
    const VkExtent2D window = ctx_ ? ctx_->getSwapchainExtent() : VkExtent2D{};
    if (f.shouldRender && window.width > 0 && window.height > 0) {
        if (f.panel.handle == XR_NULL_HANDLE || f.panelExtent.width != window.width ||
            f.panelExtent.height != window.height) {
            if (f.panel.handle != XR_NULL_HANDLE) {
                vkDeviceWaitIdle(ctx_->getDevice());
                xrDestroySwapchain(f.panel.handle);
                f.panel = {};
            }
            if (createColorSwapchain(session_, f.eyeFormat, window.width, window.height, f.panel)) {
                f.panelExtent = window;
                if (debug_) LOG_WARNING("VR: panel ", window.width, "x", window.height);
            } else {
                LOG_WARNING("VR: could not create the panel swapchain");
            }
        }
        if (f.panel.handle != XR_NULL_HANDLE && !acquireImage(f.panel)) f.shouldRender = false;
    }
    if (f.shouldRender && f.beam.handle != XR_NULL_HANDLE && !f.beamFilled) acquireImage(f.beam);
    addTiming(Timing::XrImages, millisecondsSince(imagesStart));

    placePanel();
}

// Stands the panel in front of the player and leaves it there: something to
// look around, not something stuck to the face. It is moved only when the head
// has turned well away from it or walked off, and then it follows over a
// moment instead of jumping.
void XrSystem::placePanel() {
    FrameData& f = *frame_;
    const glm::vec3 head = 0.5f * (glm::vec3(f.views[0].pose.position.x, f.views[0].pose.position.y,
                                             f.views[0].pose.position.z) +
                                   glm::vec3(f.views[1].pose.position.x, f.views[1].pose.position.y,
                                             f.views[1].pose.position.z));
    const XrQuaternionf& o = f.views[0].pose.orientation;
    const glm::vec3 forward = glm::quat(o.w, o.x, o.y, o.z) * glm::vec3(0.0f, 0.0f, -1.0f);
    // Looking straight up or down says nothing about which way to face.
    const bool level = std::abs(forward.x) + std::abs(forward.z) > 0.2f;
    const float headYaw = level ? std::atan2(-forward.x, -forward.z) : f.panelTargetYaw;

    if (level) headYaw_ = headYaw;

    // Here because this is where the head has just been worked out. Left
    // pending while the head looks straight up or down.
    if (recenterPending_ && level) {
        recenterPending_ = false;
        headOrigin_ = head;
        headOriginYaw_ = headYaw;
        bodyYaw_ = 0.0f;
        if (debug_) {
            LOG_WARNING("VR: recentred at ", head.x, " ", head.y, " ", head.z, ", turned ",
                        glm::degrees(headYaw), " degrees");
        }
    }

    constexpr float kFollowAngle = glm::radians(40.0f);
    constexpr float kFollowDistance = 0.75f;
    if (!f.panelPlaced) {
        f.panelPlaced = true;
        f.panelYaw = f.panelTargetYaw = headYaw;
        f.panelAnchor = f.panelTargetAnchor = head;
    } else if (f.worldShown && panelFollow_ == PanelFollow::Yaw) {
        // In front at once, but upright and at the height of the eyes: turning
        // the head brings it along, looking up or down looks past it.
        f.panelYaw = f.panelTargetYaw = headYaw;
        f.panelAnchor = f.panelTargetAnchor = head;
    } else if (std::abs(angleBetween(f.panelTargetYaw, headYaw)) > kFollowAngle ||
               glm::distance(f.panelTargetAnchor, head) > kFollowDistance) {
        f.panelTargetYaw = headYaw;
        f.panelTargetAnchor = head;
    }

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - f.panelMoved).count());
    f.panelMoved = now;
    const float k = 1.0f - std::exp(-dt * 6.0f);
    f.panelYaw += angleBetween(f.panelYaw, f.panelTargetYaw) * k;
    f.panelAnchor += (f.panelTargetAnchor - f.panelAnchor) * k;
}

// In the fake headset the two eyes go next to each other in the window, under
// the interface: what a headset would show with the panel over the world.
void XrSystem::recordFakePreview(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    if (!fake_ || !frame_ || !frame_->begun) return;
    FrameData& f = *frame_;
    if (f.eyesCopied != (1u << kEyeCount) - 1u || windowExtent.width == 0) return;
    const VkImageSubresourceLayers whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                         .baseArrayLayer = 0, .layerCount = 1};
    transition(cmd, windowImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const auto half = static_cast<int32_t>(windowExtent.width / 2);
    for (uint32_t i = 0; i < kEyeCount; ++i) {
        VkImageBlit blit{};
        blit.srcSubresource = whole;
        blit.srcOffsets[1] = {static_cast<int32_t>(eyeWidth_), static_cast<int32_t>(eyeHeight_), 1};
        blit.dstSubresource = whole;
        blit.dstOffsets[0] = {static_cast<int32_t>(i) * half, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(i + 1) * half, static_cast<int32_t>(windowExtent.height), 1};
        vkCmdBlitImage(cmd, f.eyeSource[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, windowImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    transition(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void XrSystem::drawFrameRate() {
    if (!showFrameRate_ || !running_ || !ImGui::GetCurrentContext() || lastSecondFrames_ == 0) return;
    char text[96];
    std::snprintf(text, sizeof(text), "VR %u fps  %.0f ms  missed %u", lastSecondFrames_,
                  1000.0 / static_cast<double>(lastSecondFrames_), lastSecondMissed_);

    // Top middle of the panel, on a dark plate so it reads over anything.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float x = (ImGui::GetIO().DisplaySize.x - size.x) * 0.5f;
    const float y = 6.0f;
    draw->AddRectFilled(ImVec2(x - 8.0f, y - 3.0f), ImVec2(x + size.x + 8.0f, y + size.y + 3.0f),
                        IM_COL32(0, 0, 0, 170), 4.0f);
    // Green where the headset gets a new picture for nearly every one it shows.
    const ImU32 colour = lastSecondMissed_ <= lastSecondFrames_ / 10 ? IM_COL32(120, 230, 120, 255)
                                                                     : IM_COL32(255, 210, 90, 255);
    draw->AddText(ImVec2(x, y), colour, text);
}

// The finished window image, copied whole into the panel. Bytes across, not a
// blit: the window is in a linear format and already display encoded, and the
// panel is its sRGB twin of the same size.
void XrSystem::recordPanel(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    if (fake_ || !frame_ || !frame_->begun || !frame_->shouldRender) return;
    FrameData& f = *frame_;
    if (f.beam.acquired && !f.beamFilling) {
        const VkImage beam = f.beam.images[f.beam.imageIndex];
        const VkClearColorValue colour{{0.25f, 0.75f, 1.0f, 1.0f}};
        const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                            .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        transition(cmd, beam, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, beam, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);
        transition(cmd, beam, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        f.beamFilling = true;
    }
    if (!f.panel.acquired || windowExtent.width != f.panelExtent.width ||
        windowExtent.height != f.panelExtent.height) {
        return;
    }

    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    VkImageCopy copy{};
    copy.srcSubresource = layers;
    copy.dstSubresource = layers;
    copy.extent = {windowExtent.width, windowExtent.height, 1};
    const VkImage target = f.panel.images[f.panel.imageIndex];

    transition(cmd, windowImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // Overwritten whole, so what the image held does not matter. The runtime
    // wants it back in COLOR_ATTACHMENT_OPTIMAL.
    transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    f.panelCopied = true;
}

XrTime XrSystem::frameTime() const {
    return frame_ ? frame_->displayTime : 0;
}

bool XrSystem::panelQuad(XrSpace& space, XrPosef& pose, XrExtent2Df& size) const {
    if (!frame_ || !frame_->panelPlaced || frame_->panelExtent.width == 0) return false;
    const FrameData& f = *frame_;
    // A quad faces +Z of its pose; turned by the yaw it faces the head it
    // was placed for, from panelDistance_ in front of it and a little low.
    space = localSpace_;
    pose = {};
    pose.orientation.y = std::sin(f.panelYaw * 0.5f);
    pose.orientation.w = std::cos(f.panelYaw * 0.5f);
    pose.position.x = f.panelAnchor.x - std::sin(f.panelYaw) * panelDistance_;
    pose.position.y = f.panelAnchor.y - 0.1f;
    pose.position.z = f.panelAnchor.z - std::cos(f.panelYaw) * panelDistance_;
    if (f.worldShown && panelFollow_ == PanelFollow::Head && viewSpace_ != XR_NULL_HANDLE) {
        // Fixed to the view: the compositor keeps it there itself, so it
        // does not swim the way a pose a frame old would.
        space = viewSpace_;
        pose = {};
        pose.orientation.w = 1.0f;
        pose.position.y = -0.1f;
        pose.position.z = -panelDistance_;
    }
    size.width = panelWidth_;
    size.height = panelWidth_ * static_cast<float>(f.panelExtent.height) /
                  static_cast<float>(f.panelExtent.width);
    return true;
}

void XrSystem::endFrame(bool submitted) {
    if (!frame_ || !frame_->begun) return;
    FrameData& f = *frame_;
    if (fake_) {
        f.begun = false;
        return;
    }
    const auto endStart = std::chrono::steady_clock::now();

    bool released = true;
    const auto release = [&](EyeSwapchain& chain) {
        if (!chain.acquired) return;
        if (XR_FAILED(xrReleaseSwapchainImage(chain.handle, nullptr))) released = false;
        chain.acquired = false;
    };
    for (EyeSwapchain& eye : f.eyes) release(eye);
    release(f.panel);
    release(f.beam);
    if (f.beamFilling && submitted && released) f.beamFilled = true;
    f.beamFilling = false;

    // The world first, when this frame drew one, and the panel over it.
    const bool usable = submitted && released && f.shouldRender;
    const bool showWorld = usable && f.eyesCopied == (1u << kEyeCount) - 1u;
    const bool showPanel = usable && f.panelCopied;
    const bool show = showWorld || showPanel;
    const XrCompositionLayerBaseHeader* layers[3] = {};
    uint32_t layerCount = 0;

    std::array<XrCompositionLayerProjectionView, kEyeCount> projectionViews{};
    XrCompositionLayerProjection layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    if (showWorld) {
        for (uint32_t i = 0; i < kEyeCount; ++i) {
            projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[i].pose = f.views[i].pose;
            projectionViews[i].fov = f.views[i].fov;
            projectionViews[i].subImage.swapchain = f.eyes[i].handle;
            projectionViews[i].subImage.imageRect.extent = {static_cast<int32_t>(eyeWidth_),
                                                            static_cast<int32_t>(eyeHeight_)};
        }
        layer.space = localSpace_;
        layer.viewCount = kEyeCount;
        layer.views = projectionViews.data();
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    }

    XrCompositionLayerQuad panel{};
    panel.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    if (showPanel) {
        // The interface blends as it is drawn, onto a picture that was cleared
        // to nothing: what is left in it is colour already multiplied by its
        // own alpha, which is what a layer is taken to hold unless told otherwise.
        panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        panel.space = localSpace_;
        panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        panel.subImage.swapchain = f.panel.handle;
        panel.subImage.imageRect.extent = {static_cast<int32_t>(f.panelExtent.width),
                                           static_cast<int32_t>(f.panelExtent.height)};
        if (panelQuad(panel.space, panel.pose, panel.size)) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
        }
    }

    // The controller's ray, over everything: there is no depth to hide it by.
    XrCompositionLayerQuad beam{};
    beam.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    const glm::vec3 head = 0.5f * (glm::vec3(f.views[0].pose.position.x, f.views[0].pose.position.y,
                                             f.views[0].pose.position.z) +
                                   glm::vec3(f.views[1].pose.position.x, f.views[1].pose.position.y,
                                             f.views[1].pose.position.z));
    if (usable && f.beamFilled && pointerBeam(f.displayTime, head, beam.pose, beam.size)) {
        beam.space = localSpace_;
        beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        beam.subImage.swapchain = f.beam.handle;
        beam.subImage.imageRect.extent = {4, 4};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&beam);
    }

    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = f.displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount > 0 ? layers : nullptr;
    const XrResult result = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(result)) LOG_WARNING("VR: xrEndFrame failed: ", resultName(result));
    f.begun = false;
    f.worldShown = showWorld;
    addTiming(Timing::XrEnd, millisecondsSince(endStart));

    const auto now = std::chrono::steady_clock::now();
    f.cpuMs += std::chrono::duration<double, std::milli>(now - f.cpuStart).count();
    ++f.frames;
    if (!show) ++f.notRendered;
    if (now - f.statsStart >= std::chrono::seconds(1)) {
        lastSecondFrames_ = f.frames;
        lastSecondMissed_ = f.missed;
        if (debug_) {
            // "wait" is how long xrWaitFrame held the frame back, and "cpu" what
            // the client then spent between beginning the frame and ending it.
            const double perFrame = 1.0 / std::max<uint32_t>(1, f.frames);
            LOG_WARNING("VR frames ", f.frames, " / missed ", f.missed, " / not shown ", f.notRendered,
                        " / cpu ", f.cpuMs * perFrame, " ms / wait ", f.waitMs * perFrame, " ms");
            auto t = [&](Timing which) { return f.timing[static_cast<size_t>(which)] * perFrame; };
            LOG_WARNING("VR waits per frame: slot ", t(Timing::SlotWait), " / window acquire ",
                        t(Timing::WindowAcquire), " / window present ", t(Timing::WindowPresent),
                        " / xr begin ", t(Timing::XrBegin), " / xr images ", t(Timing::XrImages),
                        " / xr end ", t(Timing::XrEnd), " ms");
        }
        f.statsStart = now;
        f.frames = 0;
        f.notRendered = 0;
        f.missed = 0;
        f.cpuMs = 0.0;
        f.waitMs = 0.0;
        f.timing.fill(0.0);
    }
}

}  // namespace wowee::xr
