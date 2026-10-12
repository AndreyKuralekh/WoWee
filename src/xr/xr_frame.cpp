// The frame half of XrSystem: the eye swapchains, the runtime's frame loop,
// and - for now - a flat copy of the window into both eyes.

#include "xr/xr_system.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace wowee::xr {

namespace {

constexpr uint32_t kEyeCount = 2;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t imageIndex = 0;
    bool acquired = false;
};

// The same bytes read as linear instead of as sRGB.
VkFormat unormSibling(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        default:                      return format;
    }
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

    // The window image scaled to an eye, in the linear twin of the eye format.
    // A blit converts between a linear and an sRGB format, and the window's
    // bytes are already display encoded, so blitting straight into an sRGB eye
    // image would encode them a second time. Scaling into this and then
    // copying the bytes across does not.
    rendering::AllocatedImage mirror;

    // The frame in progress.
    bool begun = false;
    bool shouldRender = false;
    bool mirrored = false;
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

    XrSwapchainCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = frame_->eyeFormat;
    createInfo.sampleCount = 1;
    createInfo.width = eyeWidth_;
    createInfo.height = eyeHeight_;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;

    for (EyeSwapchain& eye : frame_->eyes) {
        XrResult result = xrCreateSwapchain(session_, &createInfo, &eye.handle);
        if (XR_FAILED(result)) {
            eye.handle = XR_NULL_HANDLE;
            LOG_WARNING("VR: xrCreateSwapchain failed: ", resultName(result));
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(eye.handle, 0, &imageCount, nullptr);
        XrSwapchainImageVulkanKHR blank{};
        blank.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        std::vector<XrSwapchainImageVulkanKHR> images(imageCount, blank);
        result = xrEnumerateSwapchainImages(eye.handle, imageCount, &imageCount,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || imageCount == 0) {
            LOG_WARNING("VR: xrEnumerateSwapchainImages failed: ", resultName(result));
            return false;
        }
        for (const XrSwapchainImageVulkanKHR& image : images) eye.images.push_back(image.image);
    }

    frame_->mirror = rendering::createImage(
        ctx_->getDevice(), ctx_->getAllocator(), eyeWidth_, eyeHeight_, unormSibling(frame_->eyeFormat),
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (frame_->mirror.image == VK_NULL_HANDLE) {
        LOG_WARNING("VR: could not create the mirror image");
        return false;
    }

    if (debug_) {
        LOG_WARNING("VR: eye swapchains ", eyeWidth_, "x", eyeHeight_, " format ",
                    static_cast<int>(frame_->eyeFormat), ", ", frame_->eyes[0].images.size(), " images each");
    }
    return true;
}

void XrSystem::destroyEyeSwapchains() {
    if (!frame_) return;
    if (ctx_ && frame_->mirror.image != VK_NULL_HANDLE) {
        // The last frame's copy may still be on the queue.
        vkDeviceWaitIdle(ctx_->getDevice());
        rendering::destroyImage(ctx_->getDevice(), ctx_->getAllocator(), frame_->mirror);
    }
    for (EyeSwapchain& eye : frame_->eyes) {
        if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
    }
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
    const auto place = [&](const XrPosef& pose, EyeView& view) {
        const glm::vec3 p(pose.position.x, pose.position.y, pose.position.z);
        const glm::quat q(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
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
        f.mirrored = false;
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
    f.mirrored = false;
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

    const auto imagesStart = std::chrono::steady_clock::now();
    for (EyeSwapchain& eye : f.eyes) {
        XrSwapchainImageWaitInfo waitInfo{};
        waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(eye.handle, nullptr, &eye.imageIndex))) {
            f.shouldRender = false;
            break;
        }
        eye.acquired = true;
        if (XR_FAILED(xrWaitSwapchainImage(eye.handle, &waitInfo))) {
            f.shouldRender = false;
            break;
        }
    }
    addTiming(Timing::XrImages, millisecondsSince(imagesStart));
}

void XrSystem::recordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    if (!frame_ || !frame_->begun || !frame_->shouldRender) return;
    if (windowExtent.width == 0 || windowExtent.height == 0) return;
    FrameData& f = *frame_;
    constexpr uint32_t kBothEyes = (1u << kEyeCount) - 1u;
    if (fake_) {
        // No headset to show the eyes in: put them next to each other in the
        // window, over whatever it held.
        if (f.eyesCopied != kBothEyes) return;
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
        return;
    }
    // The eyes were drawn for themselves this frame; the window is not what
    // the headset shows.
    if (f.eyesCopied == kBothEyes) return;
    const VkImage mirror = f.mirror.image;

    // The window, whole, as large as fits an eye, on black.
    const float scale = std::min(static_cast<float>(eyeWidth_) / static_cast<float>(windowExtent.width),
                                 static_cast<float>(eyeHeight_) / static_cast<float>(windowExtent.height));
    const auto width = static_cast<int32_t>(static_cast<float>(windowExtent.width) * scale);
    const auto height = static_cast<int32_t>(static_cast<float>(windowExtent.height) * scale);
    const int32_t left = (static_cast<int32_t>(eyeWidth_) - width) / 2;
    const int32_t top = (static_cast<int32_t>(eyeHeight_) - height) / 2;

    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                        .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};

    transition(cmd, windowImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, mirror, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(cmd, mirror, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    VkImageBlit blit{};
    blit.srcSubresource = layers;
    blit.srcOffsets[1] = {static_cast<int32_t>(windowExtent.width), static_cast<int32_t>(windowExtent.height), 1};
    blit.dstSubresource = layers;
    blit.dstOffsets[0] = {left, top, 0};
    blit.dstOffsets[1] = {left + width, top + height, 1};
    vkCmdBlitImage(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mirror,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    transition(cmd, mirror, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VkImageCopy copy{};
    copy.srcSubresource = layers;
    copy.dstSubresource = layers;
    copy.extent = {eyeWidth_, eyeHeight_, 1};
    for (const EyeSwapchain& eye : f.eyes) {
        const VkImage target = eye.images[eye.imageIndex];
        // Overwritten whole, so what the image held does not matter. The
        // runtime wants it back in COLOR_ATTACHMENT_OPTIMAL.
        transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cmd, mirror, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    f.mirrored = true;
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
    for (EyeSwapchain& eye : f.eyes) {
        if (!eye.acquired) continue;
        if (XR_FAILED(xrReleaseSwapchainImage(eye.handle, nullptr))) released = false;
        eye.acquired = false;
    }

    std::array<XrCompositionLayerProjectionView, kEyeCount> projectionViews{};
    XrCompositionLayerProjection layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    const bool show = submitted && released && f.shouldRender &&
                      (f.mirrored || f.eyesCopied == (1u << kEyeCount) - 1u);
    if (show) {
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
    }

    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = f.displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = show ? 1 : 0;
    endInfo.layers = show ? layers : nullptr;
    const XrResult result = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(result)) LOG_WARNING("VR: xrEndFrame failed: ", resultName(result));
    f.begun = false;
    addTiming(Timing::XrEnd, millisecondsSince(endStart));

    const auto now = std::chrono::steady_clock::now();
    f.cpuMs += std::chrono::duration<double, std::milli>(now - f.cpuStart).count();
    ++f.frames;
    if (!show) ++f.notRendered;
    if (now - f.statsStart >= std::chrono::seconds(1)) {
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
