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
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <thread>
#include <utility>

namespace wowee::xr {

namespace {

constexpr uint32_t kEyeCount = 2;

// The vignette's picture: clear inside kVignetteClear of the way from the
// middle to the edge, black beyond kVignetteBlack, and a soft step between.
constexpr uint32_t kVignetteSize = 256;
constexpr float kVignetteClear = 0.15f;
constexpr float kVignetteBlack = 0.30f;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t imageIndex = 0;
    bool acquired = false;
};

// A colour swapchain and its images. False leaves `out` without a handle.
// More than one mip level is filled by blits from the first, which reads it.
bool createColorSwapchain(XrSession session, VkFormat format, uint32_t width, uint32_t height,
                          EyeSwapchain& out, uint32_t mipCount = 1) {
    XrSwapchainCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    if (mipCount > 1) createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = mipCount;
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

void transitionLevels(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t firstLevel,
                      uint32_t levels) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = firstLevel,
                                .levelCount = levels, .baseArrayLayer = 0, .layerCount = 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    transitionLevels(cmd, image, from, to, 0, 1);
}

// What of `whole` lies outside every one of `holes`, as rectangles that do not
// overlap.
std::vector<XrRect2Di> outside(const XrRect2Di& whole, const std::vector<XrRect2Di>& holes) {
    std::vector<XrRect2Di> pieces{whole};
    for (const XrRect2Di& hole : holes) {
        std::vector<XrRect2Di> left;
        for (const XrRect2Di& p : pieces) {
            const int32_t x0 = std::max(p.offset.x, hole.offset.x);
            const int32_t y0 = std::max(p.offset.y, hole.offset.y);
            const int32_t x1 = std::min(p.offset.x + p.extent.width, hole.offset.x + hole.extent.width);
            const int32_t y1 = std::min(p.offset.y + p.extent.height, hole.offset.y + hole.extent.height);
            if (x0 >= x1 || y0 >= y1) {
                left.push_back(p);
                continue;
            }
            // Above, below, then either side of the hole between them.
            if (y0 > p.offset.y) left.push_back({p.offset, {p.extent.width, y0 - p.offset.y}});
            if (y1 < p.offset.y + p.extent.height) {
                left.push_back({{p.offset.x, y1}, {p.extent.width, p.offset.y + p.extent.height - y1}});
            }
            if (x0 > p.offset.x) left.push_back({{p.offset.x, y0}, {x0 - p.offset.x, y1 - y0}});
            if (x1 < p.offset.x + p.extent.width) {
                left.push_back({{x1, y0}, {p.offset.x + p.extent.width - x1, y1 - y0}});
            }
        }
        pieces = std::move(left);
    }
    return pieces;
}

VkImageCopy copyOf(const XrRect2Di& r) {
    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    VkImageCopy copy{};
    copy.srcSubresource = layers;
    copy.dstSubresource = layers;
    copy.srcOffset = {r.offset.x, r.offset.y, 0};
    copy.dstOffset = copy.srcOffset;
    copy.extent = {static_cast<uint32_t>(r.extent.width), static_cast<uint32_t>(r.extent.height), 1};
    return copy;
}

// Each mip level below the first a filtered half of the one above, down to
// `levels`. The first is in TRANSFER_DST and all of them are left in
// COLOR_ATTACHMENT_OPTIMAL, as the runtime wants a swapchain image back.
void fillMipLevels(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, uint32_t levels) {
    int32_t width = static_cast<int32_t>(extent.width);
    int32_t height = static_cast<int32_t>(extent.height);
    for (uint32_t level = 1; level < levels; ++level) {
        transitionLevels(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         level - 1, 1);
        transitionLevels(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level, 1);
        const int32_t halfWidth = std::max(1, width / 2);
        const int32_t halfHeight = std::max(1, height / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1, .baseArrayLayer = 0,
                               .layerCount = 1};
        blit.srcOffsets[1] = {width, height, 1};
        blit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0,
                               .layerCount = 1};
        blit.dstOffsets[1] = {halfWidth, halfHeight, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        width = halfWidth;
        height = halfHeight;
    }
    if (levels > 1) {
        transitionLevels(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                         levels - 1);
    }
    transitionLevels(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     levels - 1, 1);
}

bool sameExtent(const VkExtent2D& a, const VkExtent2D& b) {
    return a.width == b.width && a.height == b.height;
}

// One whole image stretched over another, filtered. `from` in TRANSFER_SRC and
// `to` in TRANSFER_DST, as they are left.
void blitWhole(VkCommandBuffer cmd, VkImage from, VkExtent2D fromExtent, VkImage to, VkExtent2D toExtent) {
    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    VkImageBlit blit{};
    blit.srcSubresource = layers;
    blit.srcOffsets[1] = {static_cast<int32_t>(fromExtent.width), static_cast<int32_t>(fromExtent.height), 1};
    blit.dstSubresource = layers;
    blit.dstOffsets[1] = {static_cast<int32_t>(toExtent.width), static_cast<int32_t>(toExtent.height), 1};
    vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
}

// The window's picture stretched or shrunk into another whole, overwriting it.
// Both are in PRESENT_SRC before and after.
void stretchInto(VkCommandBuffer cmd, VkImage from, VkExtent2D fromExtent, VkImage to, VkExtent2D toExtent) {
    transition(cmd, from, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, to, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blitWhole(cmd, from, fromExtent, to, toExtent);
    transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

}  // namespace

struct XrSystem::FrameData {
    std::array<EyeSwapchain, kEyeCount> eyes;
    VkFormat eyeFormat = VK_FORMAT_UNDEFINED;
    // The hands alone, over the panel: each eye again with its alpha saying
    // where a hand is. Of their own, because the eyes' alpha has to be opaque.
    std::array<EyeSwapchain, kEyeCount> handEyes;
    uint32_t handsCopied = 0;

    // The panel: the window's image, its own size, shown as a flat sheet in
    // front of the player. It is the whole interface - and before the world is
    // entered, the whole picture.
    EyeSwapchain panel;
    VkExtent2D panelExtent{};
    // Smaller copies of it under the first, for where the sheet is seen
    // smaller than its pixels: without them its fine text shimmers.
    uint32_t panelMips = 1;
    bool panelCopied = false;
    // The spell book and the bags, lifted off the panel: copied here, the
    // panel's size, with a hole left where they were, so that each is seen
    // once, standing on its own. Bit i: window i went here this frame.
    EyeSwapchain windowSheet;
    uint32_t windowsLifted = 0;
    int tooltipHost = -1;  // the window the tooltip went with, lifted as well
    // The interface drawn larger than the window (setInterfaceResolution): a
    // picture of its own in the window's format, the overlay pass's
    // framebuffer on it, and whether this frame's interface went into it. Kept
    // in PRESENT_SRC between uses, as the window's images are.
    VkImage interfaceImage = VK_NULL_HANDLE;
    VmaAllocation interfaceAllocation = VK_NULL_HANDLE;
    VkImageView interfaceView = VK_NULL_HANDLE;
    VkFramebuffer interfaceFramebuffer = VK_NULL_HANDLE;
    VkExtent2D interfaceExtent{};
    VkFormat interfaceFormat = VK_FORMAT_UNDEFINED;
    bool interfaceDrawn = false;
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

    // The comfort vignette: black with a clear hole in the middle, drawn into
    // its swapchain once, as the ray is. How much of the view it takes is a
    // matter of how big the sheet is shown, so the picture never changes.
    EyeSwapchain vignette;
    bool vignetteFilling = false;
    bool vignetteFilled = false;
    VkBuffer vignetteStaging = VK_NULL_HANDLE;
    VmaAllocation vignetteStagingAllocation = VK_NULL_HANDLE;

    // WOWEE_VR_DUMP_FRAMES: pictures still to take, the ones this frame is
    // taking - copied into buffers by the frame's own commands and read back
    // once it has been submitted - and the threads writing earlier ones out.
    struct DumpShot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        const char* what = "";
        bool opaque = false;
    };
    int dumpsLeft = 0;
    int dumpIndex = 0;
    std::string dumpDir;
    std::chrono::steady_clock::time_point lastDump{};
    std::vector<DumpShot> dumpShots;
    std::vector<std::thread> dumpWriters;

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

    frame_->dumpsLeft = dumpFrames_;
    if (dumpFrames_ > 0) {
        std::filesystem::path dir = ".";
        if (const char* asked = std::getenv("WOWEE_VR_DUMP_DIR"); asked && *asked) {
            dir = asked;
        } else if (const char* log = std::getenv("WOWEE_LOG_FILE"); log && *log) {
            dir = std::filesystem::path(log).parent_path();
        }
        if (dir.empty()) dir = ".";
        std::error_code ignored;
        std::filesystem::create_directories(dir, ignored);
        frame_->dumpDir = dir.string();
        LOG_WARNING("VR: writing ", dumpFrames_, " frames, one a second, to ", frame_->dumpDir);
    }

    // Nor this: without them the panel covers the hands.
    for (EyeSwapchain& hands : frame_->handEyes) {
        if (!createColorSwapchain(session_, frame_->eyeFormat, eyeWidth_, eyeHeight_, hands)) {
            LOG_WARNING("VR: could not create the hands' swapchains - the panel covers the hands");
            for (EyeSwapchain& made : frame_->handEyes) {
                if (made.handle != XR_NULL_HANDLE) xrDestroySwapchain(made.handle);
                made = {};
            }
            break;
        }
    }

    // Not worth refusing VR over: without it the pointer has no ray, only the
    // cursor on the panel.
    if (!createColorSwapchain(session_, frame_->eyeFormat, 4, 4, frame_->beam)) {
        LOG_WARNING("VR: could not create the pointer ray's swapchain");
    }
    // Nor over this: without it there is no vignette.
    if (!createColorSwapchain(session_, frame_->eyeFormat, kVignetteSize, kVignetteSize, frame_->vignette)) {
        LOG_WARNING("VR: could not create the vignette's swapchain");
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
    for (EyeSwapchain& hands : frame_->handEyes) {
        if (hands.handle != XR_NULL_HANDLE) xrDestroySwapchain(hands.handle);
    }
    if (frame_->panel.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->panel.handle);
    if (frame_->windowSheet.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->windowSheet.handle);
    destroyInterfaceTarget();
    if (frame_->beam.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->beam.handle);
    if (frame_->vignette.handle != XR_NULL_HANDLE) xrDestroySwapchain(frame_->vignette.handle);
    if (frame_->vignetteStaging != VK_NULL_HANDLE && ctx_) {
        vmaDestroyBuffer(ctx_->getAllocator(), frame_->vignetteStaging, frame_->vignetteStagingAllocation);
    }
    finishDump(false);
    for (std::thread& writer : frame_->dumpWriters) writer.join();
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

bool XrSystem::stereoViews(const glm::vec3& characterEyes, float referenceYawDegrees,
                           float nearPlane, float farPlane, StereoViews& out) {
    if (!stereoFrame()) return false;
    const FrameData& f = *frame_;

    // The tracking space, stood in the world. The world has Z up; the headset
    // has Y up, X to the right and looks down -Z.
    const float kYardsPerMetre = yardsPerMetre();
    // Raised or lowered as the player asked, in their own metres.
    const glm::vec3 referencePosition = characterEyes + glm::vec3(0.0f, 0.0f, eyeHeightOffset_ / 0.9144f);
    const float yaw = glm::radians(referenceYawDegrees);
    const glm::vec3 forward(std::cos(yaw), std::sin(yaw), 0.0f);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::cross(forward, up);
    const glm::mat3 basis(right, up, -forward);
    worldPlaced_ = true;
    worldReference_ = referencePosition;
    worldBasis_ = basis;
    updateVignette(referencePosition);

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
        // The same without the world in between: recentred, scaled, and seen
        // from the eye - every number in it small.
        view.fromTracking = glm::mat4(glm::transpose(glm::mat3_cast(q))) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(kYardsPerMetre)) *
                            glm::translate(glm::mat4(1.0f), -p) * glm::mat4_cast(recentre) *
                            glm::translate(glm::mat4(1.0f), -headOrigin_);
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

    // The same placing as a matrix, for the hands: a pose in the tracking
    // space, in metres, to the world in yards.
    glm::mat4 trackingToWorld(basis);
    trackingToWorld[3] = glm::vec4(referencePosition, 1.0f);
    trackingToWorld = trackingToWorld * glm::scale(glm::mat4(1.0f), glm::vec3(kYardsPerMetre)) *
                      glm::mat4_cast(recentre) * glm::translate(glm::mat4(1.0f), -headOrigin_);
    placeHands(trackingToWorld);
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

void XrSystem::setHandPanelRects(const glm::vec4& actionBar, const glm::vec4& target, float windowWidth,
                                 float windowHeight) {
    handPanelRect_[0] = actionBar;
    handPanelRect_[1] = target;
    handPanelWindow_ = glm::vec2(windowWidth, windowHeight);
    if (debug_) {
        static auto lastSaid = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSaid > std::chrono::seconds(10)) {
            lastSaid = now;
            LOG_WARNING("VR: hand panels from the window ", windowWidth, "x", windowHeight, ": action bar ",
                        actionBar.x, ",", actionBar.y, " ", actionBar.z, "x", actionBar.w, ", target ", target.x,
                        ",", target.y, " ", target.z, "x", target.w);
        }
    }
}

// A frame's rectangle in the panel image, which is the window's own pixels and
// need not be the size the interface laid itself out in.
bool XrSystem::panelCrop(const glm::vec4& r, const glm::vec2& window, XrRect2Di& crop) const {
    if (!frame_ || r.z <= 1.0f || r.w <= 1.0f || window.x <= 0.0f || window.y <= 0.0f) return false;
    const VkExtent2D& extent = frame_->panelExtent;
    if (extent.width == 0 || extent.height == 0) return false;
    const float sx = static_cast<float>(extent.width) / window.x;
    const float sy = static_cast<float>(extent.height) / window.y;
    const int x0 = std::clamp(static_cast<int>(std::floor(r.x * sx)), 0, static_cast<int>(extent.width) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(r.y * sy)), 0, static_cast<int>(extent.height) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((r.x + r.z) * sx)), x0 + 1, static_cast<int>(extent.width));
    const int y1 = std::clamp(static_cast<int>(std::ceil((r.y + r.w) * sy)), y0 + 1, static_cast<int>(extent.height));
    crop.offset = {x0, y0};
    crop.extent = {x1 - x0, y1 - y0};
    return true;
}

// On the wrist of the hand that does not point, as a watch is: a little above
// the back of the hand and towards the elbow, tilted up to face the eyes
// when the forearm is raised in front of them. The target frame stands over
// the action bar. In the hand's aim space: -Z where it points, +Y its top.
bool XrSystem::handPanelQuad(uint32_t which, XrSpace& space, XrPosef& pose, XrExtent2Df& size,
                             XrRect2Di& crop) const {
    if (!handPanels_ || !frame_ || which > 1 || handPanelWindow_.x <= 0.0f || handPanelWindow_.y <= 0.0f) return false;
    const FrameData& f = *frame_;
    space = offHandSpace();
    if (space == XR_NULL_HANDLE || f.panelExtent.width == 0 || f.panelExtent.height == 0) return false;

    XrRect2Di barCrop{};
    XrRect2Di targetCrop{};
    const bool bar = panelCrop(handPanelRect_[0], handPanelWindow_, barCrop);
    const bool target = panelCrop(handPanelRect_[1], handPanelWindow_, targetCrop);
    if (which == 0 ? !bar : !target) return false;

    constexpr float kBarWidth = 0.30f;     // metres, at size 1
    constexpr float kTargetWidth = 0.14f;
    constexpr float kGap = 0.01f;
    const glm::vec3 base(0.0f, 0.07f, 0.10f);
    const glm::quat tilt = glm::angleAxis(glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 up = tilt * glm::vec3(0.0f, 1.0f, 0.0f);
    const auto heightOf = [](const XrRect2Di& c, float width) {
        return width * static_cast<float>(c.extent.height) / static_cast<float>(c.extent.width);
    };
    const float barWidth = kBarWidth * handPanelSize_;
    const float barHeight = bar ? heightOf(barCrop, barWidth) : 0.0f;
    glm::vec3 centre = base;
    if (which == 0) {
        crop = barCrop;
        size = {barWidth, barHeight};
    } else {
        crop = targetCrop;
        const float width = kTargetWidth * handPanelSize_;
        size = {width, heightOf(targetCrop, width)};
        if (bar) centre += up * (0.5f * barHeight + kGap + 0.5f * size.height);
    }
    pose.orientation = {tilt.x, tilt.y, tilt.z, tilt.w};
    pose.position = {centre.x, centre.y, centre.z};

    // With the hand tracked, a watch face instead: flat on the back of the
    // wrist, the long side along the forearm. The wrist joint has -Z towards
    // the fingers and +Y out of the back of the hand. Raised across the chest
    // to be looked at, the forearm runs left to right for the left hand and
    // right to left for the right, so the text reads along it from the elbow
    // on the left and from the hand on the right; either way its top faces
    // away from the player.
    const uint32_t hand = leftHanded_ ? 1 : 0;
    if (handsInWorld_.tracked[hand] && localSpace_ != XR_NULL_HANDLE) {
        const glm::mat4& wrist = handsInWorld_.joint[hand][1];
        const glm::vec3 at(wrist[3]);
        const glm::vec3 outOfBack = glm::normalize(glm::vec3(wrist[1]));
        const glm::vec3 towardElbow = glm::normalize(glm::vec3(wrist[2]));
        const glm::vec3 across = hand == 0 ? -towardElbow : towardElbow;  // the quad's +X
        const glm::vec3 top = glm::cross(outOfBack, across);               // the quad's +Y
        constexpr float kAboveSkin = 0.035f;  // metres off the joint, which is inside the wrist
        constexpr float kAlongArm = 0.09f;    // from the wrist joint towards the elbow
        glm::vec3 face = at + outOfBack * kAboveSkin + towardElbow * kAlongArm;
        if (which == 1 && bar) face += top * (0.5f * barHeight + kGap + 0.5f * size.height);
        // Only while the back of the wrist faces the eyes: turned away, the
        // panel would be behind the hand and is drawn over it all the same.
        const glm::vec3 head = 0.5f * (glm::vec3(f.views[0].pose.position.x, f.views[0].pose.position.y,
                                                 f.views[0].pose.position.z) +
                                       glm::vec3(f.views[1].pose.position.x, f.views[1].pose.position.y,
                                                 f.views[1].pose.position.z));
        constexpr float kMinFacing = 0.15f;  // cosine: hidden when seen within ~9 degrees of edge-on or from below
        if (glm::dot(outOfBack, glm::normalize(head - face)) < kMinFacing) return false;
        const glm::quat turn = glm::quat_cast(glm::mat3(across, top, outOfBack));
        space = localSpace_;
        pose.orientation = {turn.x, turn.y, turn.z, turn.w};
        pose.position = {face.x, face.y, face.z};
    }
    return true;
}

void XrSystem::recordHandsCopy(VkCommandBuffer cmd, uint32_t eye, VkImage image) {
    if (!stereoFrame() || fake_ || eye >= kEyeCount) return;
    FrameData& f = *frame_;
    EyeSwapchain& hands = f.handEyes[eye];
    if (hands.handle == XR_NULL_HANDLE || !hands.acquired) return;
    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                          .baseArrayLayer = 0, .layerCount = 1};
    VkImageCopy copy{};
    copy.srcSubresource = layers;
    copy.dstSubresource = layers;
    copy.extent = {eyeWidth_, eyeHeight_, 1};
    const VkImage target = hands.images[hands.imageIndex];
    transition(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    f.handsCopied |= 1u << eye;
}

void XrSystem::beginFrame() {
    if (fake_) {
        FrameData& f = *frame_;
        f.begun = true;
        f.shouldRender = true;
        f.eyesCopied = 0;
        f.handsCopied = 0;
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
    f.handsCopied = 0;
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
    locateHands(f.displayTime);

    // The pointer is the system's, and the system's is not in the picture;
    // drawPointer draws one that is.

    const auto imagesStart = std::chrono::steady_clock::now();
    for (EyeSwapchain& eye : f.eyes) {
        if (!acquireImage(eye)) {
            f.shouldRender = false;
            break;
        }
    }

    // The panel is the interface's size: the window's, whatever that is this
    // frame, times the resolution asked for.
    VkExtent2D window = ctx_ ? ctx_->getSwapchainExtent() : VkExtent2D{};
    if (window.width > 0 && window.height > 0) {
        if (interfaceResolutionNow() > 1.0f && ensureInterfaceTarget(interfaceExtentFor(window))) {
            window = f.interfaceExtent;
        } else {
            destroyInterfaceTarget();
        }
    }
    if (f.shouldRender && window.width > 0 && window.height > 0) {
        if (f.panel.handle == XR_NULL_HANDLE || f.panelExtent.width != window.width ||
            f.panelExtent.height != window.height) {
            if (f.panel.handle != XR_NULL_HANDLE) {
                vkDeviceWaitIdle(ctx_->getDevice());
                xrDestroySwapchain(f.panel.handle);
                f.panel = {};
            }
            if (f.windowSheet.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(f.windowSheet.handle);
                f.windowSheet = {};
            }
            // Down to an eighth, as far as the sheet is ever shrunk; only where
            // the format can be blitted with a filter. A runtime that refuses
            // levels gets a panel without them.
            uint32_t mips = 4;
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), f.eyeFormat, &properties);
            constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((properties.optimalTilingFeatures & kBlit) != kBlit) mips = 1;
            bool made = createColorSwapchain(session_, f.eyeFormat, window.width, window.height, f.panel, mips);
            if (!made && mips > 1) {
                mips = 1;
                made = createColorSwapchain(session_, f.eyeFormat, window.width, window.height, f.panel);
            }
            if (made) {
                f.panelExtent = window;
                f.panelMips = mips;
                // Without it the windows stay on the panel as well.
                if (!createColorSwapchain(session_, f.eyeFormat, window.width, window.height, f.windowSheet)) {
                    LOG_WARNING("VR: could not create the windows' swapchain - they stay on the panel too");
                }
                if (debug_) LOG_WARNING("VR: panel ", window.width, "x", window.height, ", ", mips, " mip levels");
            } else {
                LOG_WARNING("VR: could not create the panel swapchain");
            }
        }
        if (f.panel.handle != XR_NULL_HANDLE && !acquireImage(f.panel)) f.shouldRender = false;
        if (f.shouldRender && f.windowSheet.handle != XR_NULL_HANDLE) acquireImage(f.windowSheet);
    }
    for (EyeSwapchain& hands : f.handEyes) {
        if (f.shouldRender && hands.handle != XR_NULL_HANDLE) acquireImage(hands);
    }
    if (f.shouldRender && f.beam.handle != XR_NULL_HANDLE && !f.beamFilled) acquireImage(f.beam);
    if (f.shouldRender && f.vignette.handle != XR_NULL_HANDLE && !f.vignetteFilled) acquireImage(f.vignette);
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

    placeWindows(head, headYaw);
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

bool XrSystem::liftedWindowAt(const glm::vec2& at, const glm::vec2& window) const {
    if (!frame_ || window.x <= 0.0f || window.y <= 0.0f) return false;
    const glm::vec2 p = at * (windowPixels_ / window);
    for (uint32_t i = 0; i < 2; ++i) {
        const glm::vec4& r = windowRect_[i];
        if ((frame_->windowsLifted >> i & 1u) != 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.z &&
            p.y < r.y + r.w) {
            return true;
        }
    }
    const glm::vec4& t = tooltipRect_;
    return frame_->tooltipHost >= 0 && p.x >= t.x && p.y >= t.y && p.x < t.x + t.z && p.y < t.y + t.w;
}

float XrSystem::interfaceResolutionNow() const {
    return frame_ && (running_ || fake_) ? interfaceResolution_ : 1.0f;
}

VkExtent2D XrSystem::interfaceExtentFor(VkExtent2D window) const {
    // Within what any device takes for a framebuffer.
    constexpr float kLargest = 8192.0f;
    const float scale = interfaceResolutionNow();
    return {static_cast<uint32_t>(std::min(kLargest, std::round(static_cast<float>(window.width) * scale))),
            static_cast<uint32_t>(std::min(kLargest, std::round(static_cast<float>(window.height) * scale)))};
}

bool XrSystem::ensureInterfaceTarget(VkExtent2D extent) {
    if (!frame_ || !ctx_ || extent.width == 0 || extent.height == 0) return false;
    FrameData& f = *frame_;
    const VkFormat format = ctx_->getSwapchainFormat();
    if (f.interfaceImage != VK_NULL_HANDLE && sameExtent(f.interfaceExtent, extent) && f.interfaceFormat == format) {
        return true;
    }
    destroyInterfaceTarget();
    const VkRenderPass pass = ctx_->getOverlayRenderPass();
    if (pass == VK_NULL_HANDLE) return false;
    // Stretched into and out of with a filter, which not every format allows.
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &properties);
    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
        static bool said = false;
        if (!said) LOG_WARNING("VR: the window's format cannot be scaled - the interface is drawn at its size");
        said = true;
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx_->getAllocator(), &imageInfo, &allocInfo, &f.interfaceImage, &f.interfaceAllocation,
                       nullptr) != VK_SUCCESS) {
        f.interfaceImage = VK_NULL_HANDLE;
        LOG_WARNING("VR: could not make a ", extent.width, "x", extent.height, " picture for the interface");
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = f.interfaceImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                                 .baseArrayLayer = 0, .layerCount = 1};
    bool made = vkCreateImageView(ctx_->getDevice(), &viewInfo, nullptr, &f.interfaceView) == VK_SUCCESS;
    if (made) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &f.interfaceView;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        made = vkCreateFramebuffer(ctx_->getDevice(), &framebufferInfo, nullptr, &f.interfaceFramebuffer) ==
               VK_SUCCESS;
    }
    if (!made) {
        LOG_WARNING("VR: could not make the interface's framebuffer");
        destroyInterfaceTarget();
        return false;
    }
    f.interfaceExtent = extent;
    f.interfaceFormat = format;
    LOG_WARNING("VR: interface drawn at ", extent.width, "x", extent.height);
    return true;
}

void XrSystem::destroyInterfaceTarget() {
    if (!frame_ || !ctx_) return;
    FrameData& f = *frame_;
    if (f.interfaceImage == VK_NULL_HANDLE && f.interfaceView == VK_NULL_HANDLE &&
        f.interfaceFramebuffer == VK_NULL_HANDLE) {
        return;
    }
    // Earlier frames may still be drawing into it.
    vkDeviceWaitIdle(ctx_->getDevice());
    if (f.interfaceFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(ctx_->getDevice(), f.interfaceFramebuffer, nullptr);
    if (f.interfaceView != VK_NULL_HANDLE) vkDestroyImageView(ctx_->getDevice(), f.interfaceView, nullptr);
    if (f.interfaceImage != VK_NULL_HANDLE) {
        vmaDestroyImage(ctx_->getAllocator(), f.interfaceImage, f.interfaceAllocation);
    }
    f.interfaceFramebuffer = VK_NULL_HANDLE;
    f.interfaceView = VK_NULL_HANDLE;
    f.interfaceImage = VK_NULL_HANDLE;
    f.interfaceAllocation = VK_NULL_HANDLE;
    f.interfaceExtent = {};
    f.interfaceDrawn = false;
}

VkExtent2D XrSystem::beginInterfacePass(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent,
                                        VkFramebuffer& framebuffer) {
    if (!frame_ || interfaceResolutionNow() <= 1.0f) return {};
    const VkExtent2D extent = interfaceExtentFor(windowExtent);
    if (!ensureInterfaceTarget(extent)) return {};
    FrameData& f = *frame_;
    stretchInto(cmd, windowImage, windowExtent, f.interfaceImage, extent);
    framebuffer = f.interfaceFramebuffer;
    f.interfaceDrawn = true;
    return extent;
}

void XrSystem::endInterfacePass(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    if (!frame_ || !frame_->interfaceDrawn) return;
    stretchInto(cmd, frame_->interfaceImage, frame_->interfaceExtent, windowImage, windowExtent);
}

void XrSystem::drawPointer() {
    if (!running_ || !ImGui::GetCurrentContext()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 at = io.MousePos;
    if (at.x < 0.0f || at.y < 0.0f || at.x > io.DisplaySize.x || at.y > io.DisplaySize.y) return;

    // The interface asks for a hand over what can be clicked in the world and
    // over a link; the point says the same by its colour and its size.
    const bool hand = ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand;
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f) * (hand ? 1.3f : 1.0f);
    const int r = hand ? 255 : 120;
    const int g = hand ? 215 : 215;
    const int b = hand ? 110 : 255;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    // A glow: rings fainter the wider they are, and a white middle.
    constexpr float kRadii[] = {13.0f, 9.5f, 6.5f, 4.5f};
    constexpr int kAlpha[] = {26, 46, 90, 200};
    for (size_t i = 0; i < std::size(kRadii); ++i) {
        draw->AddCircleFilled(at, kRadii[i] * scale, IM_COL32(r, g, b, kAlpha[i]), 32);
    }
    draw->AddCircleFilled(at, 2.2f * scale, IM_COL32(255, 255, 255, 255), 16);
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
    const bool drawnLarge = frame_ && std::exchange(frame_->interfaceDrawn, false);
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
    if (f.vignette.acquired && !f.vignetteFilling && ctx_) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(kVignetteSize) * kVignetteSize * 4;
        if (f.vignetteStaging == VK_NULL_HANDLE) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            void* mapped = nullptr;
            if (vmaCreateBuffer(ctx_->getAllocator(), &bufferInfo, &allocInfo, &f.vignetteStaging,
                                &f.vignetteStagingAllocation, nullptr) == VK_SUCCESS &&
                vmaMapMemory(ctx_->getAllocator(), f.vignetteStagingAllocation, &mapped) == VK_SUCCESS) {
                // Black throughout, so the order of the colour bytes does not
                // matter and nor does whose alpha they were multiplied by.
                auto* pixel = static_cast<uint8_t*>(mapped);
                const float middle = 0.5f * static_cast<float>(kVignetteSize - 1);
                for (uint32_t y = 0; y < kVignetteSize; ++y) {
                    for (uint32_t x = 0; x < kVignetteSize; ++x, pixel += 4) {
                        const float dx = (static_cast<float>(x) - middle) / middle;
                        const float dy = (static_cast<float>(y) - middle) / middle;
                        const float t = std::clamp((std::sqrt(dx * dx + dy * dy) - kVignetteClear) /
                                                       (kVignetteBlack - kVignetteClear), 0.0f, 1.0f);
                        pixel[0] = pixel[1] = pixel[2] = 0;
                        pixel[3] = static_cast<uint8_t>(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f);
                    }
                }
                vmaUnmapMemory(ctx_->getAllocator(), f.vignetteStagingAllocation);
            } else if (f.vignetteStaging != VK_NULL_HANDLE) {
                vmaDestroyBuffer(ctx_->getAllocator(), f.vignetteStaging, f.vignetteStagingAllocation);
                f.vignetteStaging = VK_NULL_HANDLE;
            }
        }
        if (f.vignetteStaging != VK_NULL_HANDLE) {
            const VkImage image = f.vignette.images[f.vignette.imageIndex];
            VkBufferImageCopy region{};
            region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                       .baseArrayLayer = 0, .layerCount = 1};
            region.imageExtent = {kVignetteSize, kVignetteSize, 1};
            transition(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, f.vignetteStaging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            f.vignetteFilling = true;
        }
    }
    // Drawn larger, the panel is a copy of that picture instead. A frame whose
    // interface went to the window all the same - a loading screen draws its
    // own - is stretched into it first.
    VkImage source = windowImage;
    VkExtent2D sourceExtent = windowExtent;
    if (f.interfaceImage != VK_NULL_HANDLE && sameExtent(f.interfaceExtent, f.panelExtent)) {
        if (!drawnLarge) stretchInto(cmd, windowImage, windowExtent, f.interfaceImage, f.interfaceExtent);
        source = f.interfaceImage;
        sourceExtent = f.interfaceExtent;
    }
    if (!f.panel.acquired || !sameExtent(sourceExtent, f.panelExtent)) return;

    // The windows standing on their own this frame, lifted off the panel.
    std::vector<XrRect2Di> lifted;
    f.windowsLifted = 0;
    if (f.windowSheet.acquired && f.worldShown) {
        for (uint32_t i = 0; i < 2; ++i) {
            XrRect2Di crop{};
            if (windows_ && windowPlace_[i].open && panelCrop(windowRect_[i], windowPixels_, crop)) {
                lifted.push_back(crop);
                f.windowsLifted |= 1u << i;
            }
        }
    }
    f.tooltipHost = -1;
    if (const int host = lifted.empty() ? -1 : tooltipHost(); host >= 0 && (f.windowsLifted >> host & 1u) != 0) {
        XrRect2Di crop{};
        if (panelCrop(tooltipRect_, windowPixels_, crop)) {
            lifted.push_back(crop);
            f.tooltipHost = host;
        }
    }

    const VkImage target = f.panel.images[f.panel.imageIndex];
    transition(cmd, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // Overwritten whole, so what the image held does not matter. The runtime
    // wants it back in COLOR_ATTACHMENT_OPTIMAL.
    transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const XrRect2Di whole{{0, 0}, {static_cast<int32_t>(sourceExtent.width), static_cast<int32_t>(sourceExtent.height)}};
    if (lifted.empty()) {
        const VkImageCopy copy = copyOf(whole);
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        // Clear where the windows were, the rest copied round them.
        const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange first{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                            .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &first);
        transition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::vector<VkImageCopy> copies;
        for (const XrRect2Di& piece : outside(whole, lifted)) copies.push_back(copyOf(piece));
        if (!copies.empty()) {
            vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
        }
        // The windows themselves, where they are in the picture.
        const VkImage sheet = f.windowSheet.images[f.windowSheet.imageIndex];
        copies.clear();
        // Each where the ones before it are not: they may overlap.
        for (size_t k = 0; k < lifted.size(); ++k) {
            const std::vector<XrRect2Di> before(lifted.begin(), lifted.begin() + static_cast<std::ptrdiff_t>(k));
            for (const XrRect2Di& piece : outside(lifted[k], before)) copies.push_back(copyOf(piece));
        }
        transition(cmd, sheet, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sheet, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       static_cast<uint32_t>(copies.size()), copies.data());
        transition(cmd, sheet, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    fillMipLevels(cmd, target, f.panelExtent, f.panelMips);
    if (f.dumpsLeft > 0) recordDump(cmd, source, sourceExtent);
    transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    f.panelCopied = true;
}

// WOWEE_VR_DUMP_FRAMES: what this frame sends to the headset, copied out so
// that it can be looked at by someone who is not wearing one. One frame a
// second - consecutive frames are the same picture. `windowImage` and the eye
// images are in TRANSFER_SRC here.
void XrSystem::recordDump(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent) {
    FrameData& f = *frame_;
    const auto now = std::chrono::steady_clock::now();
    if (!ctx_ || !f.dumpShots.empty() || now - f.lastDump < std::chrono::seconds(1)) return;
    // The frames asked for are frames of the world. Before there is one - the
    // login screen, the loading screens - a single picture of the panel is
    // taken and not counted, or the count would be spent before the world came.
    const bool world = f.eyesCopied == (1u << kEyeCount) - 1u;
    if (!world && f.dumpIndex > 0) return;
    f.lastDump = now;

    const auto take = [&](VkImage image, uint32_t width, uint32_t height, const char* what, bool opaque) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = static_cast<VkDeviceSize>(width) * height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        FrameData::DumpShot shot;
        shot.width = width;
        shot.height = height;
        shot.what = what;
        shot.opaque = opaque;
        if (vmaCreateBuffer(ctx_->getAllocator(), &bufferInfo, &allocInfo, &shot.buffer, &shot.allocation,
                            nullptr) != VK_SUCCESS) {
            return;
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                   .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &region);
        f.dumpShots.push_back(shot);
    };
    // The panel keeps its alpha: where it is clear the world shows through.
    take(windowImage, windowExtent.width, windowExtent.height, "panel", false);
    if (world) {
        take(f.eyeSource[0], eyeWidth_, eyeHeight_, "left", true);
        take(f.eyeSource[1], eyeWidth_, eyeHeight_, "right", true);
    }
}

// The other half, once the frame's commands are on the queue: wait for them,
// take the pixels, and leave the slow part - the PNG - to a thread.
void XrSystem::finishDump(bool submitted) {
    if (!frame_ || frame_->dumpShots.empty() || !ctx_) return;
    FrameData& f = *frame_;
    const VmaAllocator allocator = ctx_->getAllocator();
    if (submitted) vkDeviceWaitIdle(ctx_->getDevice());
    const VkFormat format = ctx_->getSwapchainFormat();
    const bool blueFirst = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;

    for (const FrameData::DumpShot& shot : f.dumpShots) {
        void* mapped = nullptr;
        if (submitted && vmaMapMemory(allocator, shot.allocation, &mapped) == VK_SUCCESS) {
            const size_t bytes = static_cast<size_t>(shot.width) * shot.height * 4;
            std::vector<uint8_t> pixels(static_cast<const uint8_t*>(mapped),
                                        static_cast<const uint8_t*>(mapped) + bytes);
            vmaUnmapMemory(allocator, shot.allocation);
            char name[64];
            std::snprintf(name, sizeof(name), "vr-dump-%03d-%s.png", f.dumpIndex, shot.what);
            const std::string path = (std::filesystem::path(f.dumpDir) / name).string();
            f.dumpWriters.emplace_back([pixels = std::move(pixels), path, width = shot.width,
                                        height = shot.height, opaque = shot.opaque, blueFirst]() mutable {
                for (size_t i = 0; i < pixels.size(); i += 4) {
                    if (blueFirst) std::swap(pixels[i], pixels[i + 2]);
                    if (opaque) pixels[i + 3] = 255;
                }
                if (!stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                    pixels.data(), static_cast<int>(width * 4))) {
                    LOG_WARNING("VR: could not write ", path);
                }
            });
        }
        vmaDestroyBuffer(allocator, shot.buffer, shot.allocation);
    }
    const bool world = f.dumpShots.size() > 1;
    f.dumpShots.clear();
    if (!submitted) return;
    if (debug_) LOG_WARNING("VR: frame ", f.dumpIndex, " written to ", f.dumpDir);
    ++f.dumpIndex;
    if (world && --f.dumpsLeft == 0) LOG_WARNING("VR: all ", dumpFrames_, " frames written");
}

// Closes while the character is carried along and opens when it stops, over
// a moment either way: a vignette that snapped would be seen more than the
// motion it is there to hide.
void XrSystem::updateVignette(const glm::vec3& referencePosition) {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float dt = vignetteHasReference_ ? static_cast<float>(now - vignetteReferenceTime_) : 0.0f;
    bool moving = false;
    if (dt > 0.0f && dt < 0.5f) {
        // Yards a second. A step of the real feet is far under this, and a
        // teleport - a hearthstone, a zone change - far over.
        constexpr float kWalking = 1.0f;
        constexpr float kTeleport = 150.0f;
        const float speed = glm::length(referencePosition - vignetteReference_) / dt;
        moving = speed > kWalking && speed < kTeleport;
    }
    vignetteHasReference_ = true;
    vignetteReference_ = referencePosition;
    vignetteReferenceTime_ = now;

    constexpr float kCloseSeconds = 0.25f;
    constexpr float kOpenSeconds = 0.5f;
    const bool wanted = vignetteLevel_ > 0 && (moving || turningSmoothly_);
    const float step = dt > 0.0f && dt < 0.5f ? dt : 0.0f;
    vignetteStrength_ = wanted ? std::min(1.0f, vignetteStrength_ + step / kCloseSeconds)
                               : std::max(0.0f, vignetteStrength_ - step / kOpenSeconds);
}

// The sheet in front of the eyes, sized so that its clear middle is as wide
// as the vignette is open. Fully open it is wider than anything the headset
// shows, which is why it can come and go without an edge appearing.
bool XrSystem::vignetteQuad(XrPosef& pose, XrExtent2Df& size) const {
    if (vignetteLevel_ <= 0 || vignetteStrength_ <= 0.01f || viewSpace_ == XR_NULL_HANDLE) return false;
    constexpr float kDistance = 4.0f;  // metres: far enough to be one picture for both eyes
    constexpr float kOpenDegrees = 70.0f;
    const float closedDegrees = vignetteLevel_ >= 2 ? 33.0f : 42.0f;
    const float s = vignetteStrength_ * vignetteStrength_ * (3.0f - 2.0f * vignetteStrength_);
    const float halfAngle = glm::radians(kOpenDegrees + (closedDegrees - kOpenDegrees) * s);
    // The half-way point of the soft edge is put at that angle.
    constexpr float kEdge = 0.5f * (kVignetteClear + kVignetteBlack);
    const float side = 2.0f * kDistance * std::tan(halfAngle) / kEdge;
    pose = {};
    pose.orientation.w = 1.0f;
    pose.position.z = -kDistance;
    size = {side, side};
    return true;
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
    finishDump(submitted);
    const auto endStart = std::chrono::steady_clock::now();

    bool released = true;
    const auto release = [&](EyeSwapchain& chain) {
        if (!chain.acquired) return;
        if (XR_FAILED(xrReleaseSwapchainImage(chain.handle, nullptr))) released = false;
        chain.acquired = false;
    };
    for (EyeSwapchain& eye : f.eyes) release(eye);
    for (EyeSwapchain& hands : f.handEyes) release(hands);
    release(f.panel);
    release(f.windowSheet);
    release(f.beam);
    if (f.beamFilling && submitted && released) f.beamFilled = true;
    f.beamFilling = false;
    release(f.vignette);
    if (f.vignetteFilling && submitted && released) f.vignetteFilled = true;
    f.vignetteFilling = false;

    // The world first, when this frame drew one, and the panel over it.
    const bool usable = submitted && released && f.shouldRender;
    const bool showWorld = usable && f.eyesCopied == (1u << kEyeCount) - 1u;
    const bool showPanel = usable && f.panelCopied;
    const bool show = showWorld || showPanel;
    const XrCompositionLayerBaseHeader* layers[12] = {};
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

    // The vignette, over the world and under the interface: it is the world
    // that moves, and the panel has to stay readable to its corners.
    XrCompositionLayerQuad vignette{};
    vignette.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    if (showWorld && f.vignetteFilled && vignetteQuad(vignette.pose, vignette.size)) {
        vignette.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        vignette.space = viewSpace_;
        vignette.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        vignette.subImage.swapchain = f.vignette.handle;
        vignette.subImage.imageRect.extent = {static_cast<int32_t>(kVignetteSize),
                                              static_cast<int32_t>(kVignetteSize)};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vignette);
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

    // The spell book and the bags, nearer than the panel and parts of its
    // image like the wrist's; each with the bar that moves it, in the ray's
    // colour. Under the hands: a hand reaching for one is in front of it.
    XrCompositionLayerQuad windows[2]{};
    XrCompositionLayerQuad windowBars[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        XrCompositionLayerQuad& quad = windows[i];
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        if (!showWorld || !showPanel || !windowQuad(i, quad.pose, quad.size, quad.subImage.imageRect)) continue;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = localSpace_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = (f.windowsLifted >> i & 1u) != 0 ? f.windowSheet.handle : f.panel.handle;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        XrCompositionLayerQuad& bar = windowBars[i];
        bar.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        if (!f.beamFilled || !windowBarQuad(i, bar.pose, bar.size)) continue;
        bar.space = localSpace_;
        bar.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        bar.subImage.swapchain = f.beam.handle;
        bar.subImage.imageRect.extent = {4, 4};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&bar);
    }
    // The tooltip of one of them, beside it rather than on the panel.
    XrCompositionLayerQuad tooltip{};
    tooltip.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    if (showWorld && showPanel && f.tooltipHost >= 0 &&
        tooltipQuad(static_cast<uint32_t>(f.tooltipHost), tooltip.pose, tooltip.size, tooltip.subImage.imageRect)) {
        tooltip.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        tooltip.space = localSpace_;
        tooltip.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        tooltip.subImage.swapchain = f.windowSheet.handle;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&tooltip);
    }

    // The hands, over the panel: the eyes once more, showing only what their
    // alpha keeps, which is where a hand was drawn and the world did not hide
    // it. A hand is always nearer than the big panel, so this is always right.
    std::array<XrCompositionLayerProjectionView, kEyeCount> handViews = projectionViews;
    XrCompositionLayerProjection hands = layer;
    if (showWorld && showPanel && f.handsCopied == (1u << kEyeCount) - 1u) {
        for (uint32_t i = 0; i < kEyeCount; ++i) handViews[i].subImage.swapchain = f.handEyes[i].handle;
        hands.views = handViews.data();
        hands.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hands);
    }

    // The action bar and the target frame again, on the wrist: parts of the
    // panel's own image, so nothing is copied for them. Over the hands, since
    // they lie on the back of the wrist and are only shown while it faces the
    // eyes; the other hand passes under them.
    XrCompositionLayerQuad handPanels[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        XrCompositionLayerQuad& quad = handPanels[i];
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        if (!showWorld || !showPanel || !handPanelQuad(i, quad.space, quad.pose, quad.size, quad.subImage.imageRect)) {
            continue;
        }
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = f.panel.handle;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
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
