#pragma once

/**
 * xr_system.hpp - the OpenXR instance and the headset it found.
 *
 * Built only with WOWEE_VR=ON, and even then it does nothing unless the
 * client is started with WOWEE_VR=1 in the environment. The client takes no
 * command line arguments, so the environment is where a launch option goes -
 * and it is also the one channel that reaches the Android build, where the
 * activity sets it before SDL_main runs.
 *
 *   WOWEE_VR=1              ask for VR
 *   WOWEE_VR_DEBUG=1        verbose OpenXR log, and the once-a-second frame line
 *   WOWEE_VR_DUMP_FRAMES=N  write N frames of each eye and of the interface as PNG
 *
 * Nothing here is ever fatal. No runtime, no headset connected, a runtime
 * without the Vulkan extension: each is logged once and the client carries on
 * as the flat client it was going to be anyway.
 */

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wowee::rendering {
class VkContext;
}

namespace wowee::xr {

class XrSystem {
public:
    XrSystem();
    ~XrSystem();

    /// Creates the instance and looks for a head mounted display.
    ///
    /// Call before the Vulkan context exists: the runtime has a say in which
    /// instance and device extensions that context is created with. Returns
    /// whether VR is available; false leaves everything released.
    bool initialize();

    void shutdown();

    /// Whether the launch asked for VR at all.
    [[nodiscard]] static bool requested();

    /// Whether an instance exists and a headset answered.
    [[nodiscard]] bool available() const { return systemId_ != XR_NULL_SYSTEM_ID; }

    [[nodiscard]] XrInstance instance() const { return instance_; }
    [[nodiscard]] XrSystemId systemId() const { return systemId_; }

    /// The size the runtime recommends for one eye's image.
    [[nodiscard]] uint32_t recommendedEyeWidth() const { return eyeWidth_; }
    [[nodiscard]] uint32_t recommendedEyeHeight() const { return eyeHeight_; }

    [[nodiscard]] bool debugEnabled() const { return debug_; }
    [[nodiscard]] int dumpFrameCount() const { return dumpFrames_; }

    /// Whether the runtime offers an extension, enabled here or not.
    [[nodiscard]] bool runtimeOffers(const char* extensionName) const;

    /// An XrResult by name, for the log.
    [[nodiscard]] std::string resultName(XrResult result) const;

    // --- What the runtime asks of the Vulkan context (XR_KHR_vulkan_enable) ---
    //
    // All three answer with nothing when VR is not available, so the places in
    // VkContext that ask need no test of their own.

    /// Instance extensions the runtime needs enabled.
    [[nodiscard]] std::vector<std::string> vulkanInstanceExtensions() const;

    /// Device extensions the runtime needs enabled.
    [[nodiscard]] std::vector<std::string> vulkanDeviceExtensions() const;

    /// The GPU the headset is driven from. With two cards in the machine the
    /// session can only be created on this one.
    [[nodiscard]] VkPhysicalDevice vulkanGraphicsDevice(VkInstance vkInstance) const;

    // --- Session ---

    /// Creates the session on the client's Vulkan device, and its reference
    /// spaces. On failure VR is shut down and the client stays flat.
    bool createSession(rendering::VkContext& ctx);

    /// Has to happen before the Vulkan device is destroyed.
    void destroySession();

    /// Drains the runtime's event queue. beginFrame does it every frame.
    void pollEvents();

    /// Whether the session has been begun and frames go to the headset. While
    /// it is, the runtime paces the frame and the client's own limiter and the
    /// window's vsync stand down.
    [[nodiscard]] bool sessionRunning() const { return running_; }

    // --- Frame ---
    //
    // Called by VkContext around every frame it records, the loading screen's
    // included, so whatever reaches the window reaches the headset.

    /// Waits for the runtime's frame slot and begins the frame. Call once the
    /// frame's command buffer is certain to be submitted.
    void beginFrame();

    /// Records the copy of the finished window image into both eyes. `cmd` is
    /// outside any render pass and `windowImage` is in PRESENT_SRC layout.
    void recordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    /// Hands the frame to the runtime. `submitted` is whether the command
    /// buffer made it to the queue; if not the frame goes out with no layer.
    void endFrame(bool submitted);

    [[nodiscard]] XrSession session() const { return session_; }
    [[nodiscard]] XrSessionState sessionState() const { return sessionState_; }
    [[nodiscard]] XrSpace localSpace() const { return localSpace_; }
    /// XR_NULL_HANDLE when the runtime has no stage space.
    [[nodiscard]] XrSpace stageSpace() const { return stageSpace_; }

private:
    bool createInstance();
    bool findHeadset();
    bool createReferenceSpaces();
    bool createEyeSwapchains();
    void destroyEyeSwapchains();
    void beginSession();
    void endSession();
    void onSessionStateChanged(XrSessionState newState);
    [[nodiscard]] std::vector<std::string> vulkanExtensionList(const char* functionName) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace stageSpace_ = XR_NULL_HANDLE;
    rendering::VkContext* ctx_ = nullptr;
    bool running_ = false;

    // Swapchains, the frame in progress and its statistics; xr_frame.cpp.
    struct FrameData;
    std::unique_ptr<FrameData> frame_;
    std::vector<std::string> runtimeExtensions_;
    uint32_t eyeWidth_ = 0;
    uint32_t eyeHeight_ = 0;
    bool debug_ = false;
    int dumpFrames_ = 0;
};

/// The one XrSystem. The application starts it and shuts it down.
XrSystem& system();

}  // namespace wowee::xr
