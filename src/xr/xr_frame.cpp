// The frame half of XrSystem: the eye swapchains, the runtime's frame loop,
// and - for now - a flat copy of the window into both eyes.

#include "xr/xr_system.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>

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
};

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

void XrSystem::beginFrame() {
    if (instance_ == XR_NULL_HANDLE) return;
    pollEvents();
    if (!running_ || !frame_) return;

    FrameData& f = *frame_;
    // A frame the client began and never ended: close it, or the runtime
    // refuses the next one.
    if (f.begun) endFrame(false);

    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    XrResult result = xrWaitFrame(session_, nullptr, &frameState);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrWaitFrame failed: ", resultName(result));
        return;
    }
    result = xrBeginFrame(session_, nullptr);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrBeginFrame failed: ", resultName(result));
        return;
    }
    f.begun = true;
    f.mirrored = false;
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
}

void XrSystem::recordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    if (!frame_ || !frame_->begun || !frame_->shouldRender) return;
    if (windowExtent.width == 0 || windowExtent.height == 0) return;
    FrameData& f = *frame_;
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
    const bool show = submitted && released && f.shouldRender && f.mirrored;
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

    const auto now = std::chrono::steady_clock::now();
    f.cpuMs += std::chrono::duration<double, std::milli>(now - f.cpuStart).count();
    ++f.frames;
    if (!show) ++f.notRendered;
    if (now - f.statsStart >= std::chrono::seconds(1)) {
        if (debug_) {
            LOG_WARNING("VR frames ", f.frames, " / missed ", f.missed, " / not shown ", f.notRendered,
                        " / cpu ", f.cpuMs / std::max<uint32_t>(1, f.frames), " ms");
        }
        f.statsStart = now;
        f.frames = 0;
        f.notRendered = 0;
        f.missed = 0;
        f.cpuMs = 0.0;
    }
}

}  // namespace wowee::xr
