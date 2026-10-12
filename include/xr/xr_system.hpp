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
 *   WOWEE_VR_EYE_SCALE=P    eye image size in percent of what the runtime recommends (25-200)
 *   WOWEE_VR_FAKE=1         no runtime and no headset: a fixed pair of eyes, drawn side by side
 *                           into the window. For looking at the stereo render on a desk.
 *   WOWEE_VR_PANEL_DISTANCE=cm, WOWEE_VR_PANEL_WIDTH=cm
 *                           where the interface panel stands and how wide it is (150, 200)
 *   WOWEE_VR_PANEL_FOLLOW=lazy|yaw|head
 *                           how the panel keeps up with the head: yaw (the default) keeps it in
 *                           front at once but upright and level; lazy leaves it standing until
 *                           the head has turned well away; head fixes it to the view
 *   WOWEE_VR_PROFILE=0|1    the lean frame (see leanFrame): 0 keeps the flat client's frame in the
 *                           headset, 1 forces the lean one without a headset, for measuring it
 *
 * Nothing here is ever fatal. No runtime, no headset connected, a runtime
 * without the Vulkan extension: each is logged once and the client carries on
 * as the flat client it was going to be anyway.
 */

#include <glm/glm.hpp>
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

/// One view of the world: an eye, or the point between the eyes.
struct EyeView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};  // Vulkan clip space: Y down, depth 0..1
    glm::vec3 position{0.0f};
};

/// What a stereo frame is drawn from. `center` sees everything either eye
/// does, and is what the frame is culled by.
struct StereoViews {
    EyeView eye[2];
    EyeView center;
};

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

    /// Whether the renderer should leave out what a headset frame cannot
    /// afford: the shadow pass and the water reflection, each of which draws
    /// the scene a second time. True while the session runs.
    [[nodiscard]] bool leanFrame() const { return leanForced_ || (running_ && !leanRefused_); }

    // --- Frame ---
    //
    // Called by VkContext around every frame it records, the loading screen's
    // included, so whatever reaches the window reaches the headset.

    /// Waits for the runtime's frame slot and begins the frame. Call once the
    /// frame's command buffer is certain to be submitted.
    void beginFrame();

    /// Records the copy of the finished window image into the panel: the flat
    /// sheet the interface is shown on in the headset. `cmd` is outside any
    /// render pass and `windowImage` is in PRESENT_SRC layout.
    void recordPanel(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    /// Fake headset only: the two eyes side by side into the window, before the
    /// interface is drawn over it. Same requirements as recordPanel.
    void recordFakePreview(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    /// Hands the frame to the runtime. `submitted` is whether the command
    /// buffer made it to the queue; if not the frame goes out with no layer.
    void endFrame(bool submitted);

    /// Whether this is the stand-in for a headset (WOWEE_VR_FAKE).
    [[nodiscard]] bool fakeHeadset() const { return fake_; }

    /// Whether this frame wants the world drawn once per eye.
    [[nodiscard]] bool stereoFrame() const;

    /// The eyes of this frame, placed in the world.
    ///
    /// The headset reports where the head is in its own tracking space. That
    /// space is stood on `referencePosition`, facing `referenceYawDegrees`
    /// around the vertical, which is where the game put its camera. One yard
    /// of the world is 0.9144 metres.
    [[nodiscard]] bool stereoViews(const glm::vec3& referencePosition, float referenceYawDegrees,
                                   float nearPlane, float farPlane, StereoViews& out) const;

    /// Records the copy of a finished eye image into the headset. `image` is
    /// eye sized, in the window's format and in PRESENT_SRC layout; it is left
    /// in TRANSFER_SRC.
    void recordEyeCopy(VkCommandBuffer cmd, uint32_t eye, VkImage image);

    /// Where a frame's wall-clock time went, for the once-a-second debug line.
    /// The window's own waits are reported by VkContext; the rest is measured here.
    enum class Timing { SlotWait, WindowAcquire, WindowPresent, XrBegin, XrImages, XrEnd, Count };
    void addTiming(Timing which, double milliseconds);

    [[nodiscard]] XrSession session() const { return session_; }
    [[nodiscard]] XrSessionState sessionState() const { return sessionState_; }
    [[nodiscard]] XrSpace localSpace() const { return localSpace_; }
    /// XR_NULL_HANDLE when the runtime has no stage space.
    [[nodiscard]] XrSpace stageSpace() const { return stageSpace_; }

    /// How the interface panel keeps up with the head, once in the world; before
    /// that it always stands still, as Lazy. In the order the setting lists
    /// them, which is the order they are saved by.
    enum class PanelFollow { Yaw, Lazy, Head };

    /// Where the panel stands, from the settings page. A value the environment
    /// set at start is left alone: that is how one is tried without saving it.
    void setPanelPlacement(PanelFollow follow, float distanceMetres, float widthMetres);

private:
    bool createInstance();
    bool findHeadset();
    bool createReferenceSpaces();
    bool createEyeSwapchains();
    void destroyEyeSwapchains();
    void startFake();
    void placePanel();
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
    XrSpace viewSpace_ = XR_NULL_HANDLE;  // the head itself; what a head-fixed panel is placed in
    PanelFollow panelFollow_ = PanelFollow::Yaw;
    bool panelFollowFromEnv_ = false;
    bool panelDistanceFromEnv_ = false;
    bool panelWidthFromEnv_ = false;
    rendering::VkContext* ctx_ = nullptr;
    bool running_ = false;
    bool fake_ = false;
    float panelDistance_ = 1.5f;  // metres
    float panelWidth_ = 2.0f;
    bool leanForced_ = false;
    bool leanRefused_ = false;

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
