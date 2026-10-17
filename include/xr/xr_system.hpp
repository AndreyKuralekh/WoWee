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
 *   WOWEE_VR_DUMP_FRAMES=N  write N frames of each eye and of the interface as PNG, one a second,
 *                           beside the log (WOWEE_LOG_FILE) or into WOWEE_VR_DUMP_DIR
 *   WOWEE_VR_EYE_SCALE=P    eye image size in percent of what the runtime recommends (25-200, 130 if unset)
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

#include "xr/xr_controls.hpp"
#include "xr/xr_hands.hpp"
#include <glm/glm.hpp>
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace wowee::rendering {
class CameraController;
class VkContext;
}

namespace wowee::xr {

/// One view of the world: an eye, or the point between the eyes.
struct EyeView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};  // Vulkan clip space: Y down, depth 0..1
    glm::vec3 position{0.0f};
    /// The eye's view of the tracking space itself: a point in metres there
    /// to this eye's view space, in yards. What is drawn within reach - the
    /// hands - goes through this rather than through the world, where a
    /// coordinate in the thousands leaves floats a millimetre to work with
    /// and a hand a foot away shakes by pixels as the head moves.
    glm::mat4 fromTracking{1.0f};
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

    /// How many times the window's size the interface is drawn at in the
    /// headset, 1 to 2. The panel, the windows and the wrist are all cut from
    /// that picture, so more of its pixels land on each degree of the view and
    /// text is sharper; the layout is the same, being in the interface's own
    /// units. The window gets the picture scaled back down.
    void setInterfaceResolution(float scale) {
        interfaceResolution_ = scale < 1.0f ? 1.0f : (scale > 2.0f ? 2.0f : scale);
    }
    /// The scale this frame's interface is drawn at: the setting while there is
    /// a headset or the stand-in for one, 1 otherwise. ImGui is told it before
    /// the frame, so that it rasterizes text for that size.
    [[nodiscard]] float interfaceResolutionNow() const;
    /// Moves the interface's pass onto the larger picture: what the window
    /// holds so far (the minimap, or the world before it is entered) is
    /// stretched into it, and `framebuffer` becomes its framebuffer for the
    /// overlay render pass. Returns its size, or 0 x 0 when the interface is
    /// drawn at the window's size this frame. Outside any render pass, with
    /// `windowImage` in PRESENT_SRC; both are left in PRESENT_SRC.
    [[nodiscard]] VkExtent2D beginInterfacePass(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent,
                                                VkFramebuffer& framebuffer);
    /// After that pass: the larger picture scaled down into the window, which
    /// shows it and records it. Same requirements.
    void endInterfacePass(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    /// Hands the frame to the runtime. `submitted` is whether the command
    /// buffer made it to the queue; if not the frame goes out with no layer.
    void endFrame(bool submitted);

    /// Whether this is the stand-in for a headset (WOWEE_VR_FAKE).
    [[nodiscard]] bool fakeHeadset() const { return fake_; }
    /// The stand-in's way to the window's Vulkan context, which a real headset
    /// is given by createSession: the interface is drawn larger with it too.
    void useFakeContext(rendering::VkContext& ctx) {
        if (fake_) ctx_ = &ctx;
    }

    /// Whether this frame wants the world drawn once per eye.
    [[nodiscard]] bool stereoFrame() const;

    /// The eyes of this frame, placed in the world.
    ///
    /// The headset reports where the head is in its own tracking space. That
    /// space is stood on `referencePosition`, facing `referenceYawDegrees`
    /// around the vertical, which is where the game put its camera. One yard
    /// of the world is 0.9144 metres.
    [[nodiscard]] bool stereoViews(const glm::vec3& referencePosition, float referenceYawDegrees,
                                   float nearPlane, float farPlane, StereoViews& out);

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

    /// Whether the panel carries a line saying how fast the headset is being
    /// fed: frames a second, the time one takes, and how many the headset had
    /// to show twice.
    void setShowFrameRate(bool show) { showFrameRate_ = show; }

    /// Draws that line. Called while the interface frame is open, last, so
    /// that it lies over everything on the panel.
    void drawFrameRate();

    /// Draws the pointer on the panel: a point of light where the mouse is,
    /// in place of the arrow, which belongs on a desk. Brighter and warmer
    /// where the interface would have shown a hand. Same place in the frame
    /// as drawFrameRate.
    void drawPointer();

    /// Whether the world is seen from the character's own eyes, with the
    /// character itself left out, rather than from where the game put its
    /// camera behind it.
    void setFirstPerson(bool on) { firstPerson_ = on; }
    [[nodiscard]] bool firstPerson() const { return firstPerson_; }

    /// Takes where the head is at the next frame as the place the character's
    /// head is, and the way it faces as straight ahead: the body is wherever
    /// the player sat down, and this is how the game is told. Height is taken
    /// too, so it works seated. The panel is not moved by it.
    void recenter() { recenterPending_ = true; }

    // --- Controllers ---

    /// Reads the controllers and passes on what they say as the input the
    /// client already takes: held keys for the left stick and for jumping, a
    /// turn of the camera for the right stick. Call once a frame before the
    /// keyboard is sampled. Outside the world everything held is let go.
    ///
    /// The right controller is also a pointer, in the world or out of it: where
    /// its ray meets the interface panel is where the mouse is put, the trigger
    /// is the left button and the grip the right one.
    void updateControllers(rendering::CameraController* camera, bool inWorld, float deltaTime,
                           SDL_Window* window);

    /// The right controller's ray in the world, as the last stereoViews stood
    /// the tracking space in it. False unless the controller is what is
    /// pointing just now: while it moves the cursor over the panel, or holds a
    /// button it pressed beside it. Otherwise the mouse picks, as it always did.
    [[nodiscard]] bool pickRay(glm::vec3& origin, glm::vec3& direction) const;

    /// Whether the character turns as the head does, so that it faces - and
    /// walks - the way the player is looking. What is seen does not change by
    /// it: the turn is given to the game and taken off the head in one step.
    void setBodyFollowsHead(bool follows) { bodyFollowsHead_ = follows; }

    /// What each button does, as the settings page reads and writes it; see
    /// isControlSettingKey. A change is in force at once and is written to
    /// vr_controls.cfg. The setter is false for a key that is not a binding.
    [[nodiscard]] std::string controlSetting(const std::string& key);
    bool setControlSetting(const std::string& key, int value);

    /// Whether the right stick turns by a fixed angle at a push, or smoothly.
    void setSnapTurn(bool snap) { snapTurn_ = snap; }

    /// The comfort vignette: while the character moves, or is turned smoothly,
    /// the edge of the view is darkened, which is where the motion the body
    /// does not feel is seen most. 0 off, 1 light, 2 strong.
    void setVignette(int level) { vignetteLevel_ = level < 0 ? 0 : (level > 2 ? 2 : level); }

    /// Whether the left stick walks the way the left controller points rather
    /// than the way the head looks. The character turns to face that way for
    /// as long as the stick is pushed; what is seen does not turn with it.
    void setMoveByHand(bool byHand) { moveByHand_ = byHand; }

    /// The controllers the other way round: the left one points and clicks,
    /// the right stick walks and the left turns, and every button is on the
    /// opposite hand - what the settings call the left trigger is the right.
    void setLeftHanded(bool leftHanded) { leftHanded_ = leftHanded; }

    /// How big the world is made to look: 1 is its own size, a yard to 0.9144
    /// metres; at 2 everything is twice as large and a real step covers half
    /// the ground. And how far above or below the character's own eyes the
    /// view is put, in metres.
    void setWorldScale(float scale) { worldScale_ = scale < 0.25f ? 0.25f : (scale > 4.0f ? 4.0f : scale); }
    void setEyeHeightOffset(float metres) { eyeHeightOffset_ = metres; }
    /// How much glow is added over the eyes: 0 none, 1 strong. See EyeGlow.
    void setGlow(float strength) { glow_ = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength); }
    [[nodiscard]] float glow() const { return glow_; }

    // --- Hands ---

    /// Where the player's hands are this frame, placed in the world by the
    /// last stereoViews. Untracked hands are left out; see HandPoses.
    [[nodiscard]] const HandPoses& handsInWorld() const { return handsInWorld_; }
    /// Whether the hands are drawn, and the colour of their skin: 0 pale, 1 dark.
    void setShowHands(bool show) { showHands_ = show; }
    [[nodiscard]] bool showHands() const { return showHands_; }
    void setHandTone(float tone) { handTone_ = tone < 0.0f ? 0.0f : (tone > 1.0f ? 1.0f : tone); }
    [[nodiscard]] float handTone() const { return handTone_; }
    /// Copies a finished eye, whose alpha is 1 where a hand is seen and 0
    /// elsewhere, into the hands' own swapchain: shown over the panel, it puts
    /// the hands in front of it. `image` is in PRESENT_SRC and is left there.
    /// Before the eye's alpha is made opaque again (EyeGlow::record) - the
    /// runtime blends even the world's layer by it.
    void recordHandsCopy(VkCommandBuffer cmd, uint32_t eye, VkImage image);

    // --- Panels on the hand ---

    /// Where the action bar and the target frame are in the window: x, y from
    /// its top left, width, height, in the pixels the pointer works in; a
    /// width of 0 when the frame is not shown. Said every frame from the
    /// interface's layout. Both are cut out of the panel and shown again on
    /// the wrist of the hand that does not point, where the ray can reach
    /// them without looking away from the world.
    void setHandPanelRects(const glm::vec4& actionBar, const glm::vec4& target, float windowWidth,
                           float windowHeight);
    void setHandPanels(bool on) { handPanels_ = on; }
    void setHandPanelSize(float scale) { handPanelSize_ = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale); }

    // --- Windows on their own ---

    /// Where the spell book and the bags are in the window, as for
    /// setHandPanelRects; a width of 0 while one is closed. Each open one is
    /// cut out of the panel and stood nearer, to one side in front of the
    /// player, where a bar under it moves it (xr_windows.cpp).
    void setWindowRects(const glm::vec4& spellBook, const glm::vec4& bags, float windowWidth, float windowHeight);
    void setWindows(bool on) { windows_ = on; }
    void setWindowSize(float scale) { windowSize_ = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale); }
    /// Back to where they stand at first, and the saved places forgotten.
    void resetWindowPlaces();

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
    void recordDump(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);
    void finishDump(bool submitted);
    bool createActions();
    void destroyActions();
    void releaseControllerKeys();
    void loadControls();
    bool offHandYaw(float& yaw) const;
    void updateVignette(const glm::vec3& referencePosition);
    bool vignetteQuad(XrPosef& pose, XrExtent2Df& size) const;
    void releasePointer();
    void updatePointer(SDL_Window* window, bool inWorld);
    /// The right controller's ray in `base` at `time`. False while it is not tracked.
    [[nodiscard]] bool aimRay(XrSpace base, XrTime time, glm::vec3& origin, glm::vec3& direction) const;
    /// The ray as a thin sheet turned towards `head`, in the local space.
    [[nodiscard]] bool pointerBeam(XrTime time, const glm::vec3& head, XrPosef& pose, XrExtent2Df& size) const;
    /// Where the panel stands: the space it is placed in, its pose there and
    /// its size in metres. False before there is a panel.
    [[nodiscard]] bool panelQuad(XrSpace& space, XrPosef& pose, XrExtent2Df& size) const;
    /// A panel on the hand: 0 the action bar, 1 the target frame. The space is
    /// the off hand's own, so the compositor keeps it on the hand; `crop` is
    /// the part of the panel's image it shows. False while it is not shown.
    [[nodiscard]] bool handPanelQuad(uint32_t which, XrSpace& space, XrPosef& pose, XrExtent2Df& size,
                                     XrRect2Di& crop) const;
    /// A rectangle of the window as the part of the panel's image showing it.
    [[nodiscard]] bool panelCrop(const glm::vec4& rect, const glm::vec2& window, XrRect2Di& crop) const;
    /// The size the interface is drawn at for a window of `window`.
    [[nodiscard]] VkExtent2D interfaceExtentFor(VkExtent2D window) const;
    /// The larger picture the interface is drawn into, made for `extent` if it
    /// is not already that size. False when it cannot be had.
    bool ensureInterfaceTarget(VkExtent2D extent);
    void destroyInterfaceTarget();
    /// A window on its own: 0 the spell book, 1 the bags; in the local space.
    /// False while it is closed. The bar under it, which moves it, likewise.
    [[nodiscard]] bool windowQuad(uint32_t which, XrPosef& pose, XrExtent2Df& size, XrRect2Di& crop) const;
    [[nodiscard]] bool windowBarQuad(uint32_t which, XrPosef& pose, XrExtent2Df& size) const;
    /// Whether a point of the big panel, in the pixels of a window `window`
    /// in size, is where a window standing on its own was lifted off it: a
    /// hole there, and nothing for the ray to press.
    [[nodiscard]] bool liftedWindowAt(const glm::vec2& at, const glm::vec2& window) const;
    /// Opens, closes and follows the windows; once a frame, with the head known.
    void placeWindows(const glm::vec3& head, float headYaw);
    /// Moves a window by its bar while the trigger is held there. True while
    /// it does, and the trigger is then not a click.
    bool dragWindow(const glm::vec3& origin, const glm::vec3& direction, bool trigger, int barHit, float barDistance);
    void loadWindowPlaces();
    void saveWindowPlaces() const;
    /// The hand that does not point, as an action space; null without one.
    [[nodiscard]] XrSpace offHandSpace() const;
    /// When the frame last begun is to be shown; 0 before the first.
    [[nodiscard]] XrTime frameTime() const;
    void logInteractionProfile();
    // The hand trackers (XR_EXT_hand_tracking); xr_hands.cpp. Without the
    // extension there are none and no hands are drawn.
    void enableHandTrackingExtensions(std::vector<const char*>& enabled);
    void createHandTrackers();
    void destroyHandTrackers();
    void locateHands(XrTime time);
    void placeHands(const glm::mat4& trackingToWorld);
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
    bool showFrameRate_ = false;
    bool firstPerson_ = true;
    bool snapTurn_ = true;
    bool moveByHand_ = false;
    bool leftHanded_ = false;
    float worldScale_ = 1.0f;
    float eyeHeightOffset_ = 0.0f;
    float glow_ = 0.8f;
    bool showHands_ = true;
    float handTone_ = 0.5f;
    bool handPanels_ = true;
    float handPanelSize_ = 1.0f;
    glm::vec4 handPanelRect_[2]{};  // the action bar, the target frame; window pixels
    glm::vec2 handPanelWindow_{0.0f};
    // The windows on their own. Each stands at `offset` from the head it was
    // opened for, turned by that head's yaw: x to the right, y up, -z ahead.
    // The offsets are the player's, kept in vr_windows.cfg; the anchor follows
    // the head once it has turned or walked well away.
    struct WindowPlace {
        bool open = false;
        float yaw = 0.0f;
        float targetYaw = 0.0f;
        glm::vec3 anchor{0.0f};
        glm::vec3 targetAnchor{0.0f};
        glm::vec3 offset{0.0f};
    };
    float interfaceResolution_ = 1.5f;
    bool windows_ = true;
    float windowSize_ = 1.0f;
    glm::vec4 windowRect_[2]{};  // the spell book, the bags; window pixels
    glm::vec2 windowPixels_{0.0f};
    WindowPlace windowPlace_[2];
    bool windowPlacesLoaded_ = false;
    int draggedWindow_ = -1;
    int pointedWindow_ = -1;  // the window or bar the ray met last; only its bar is shown
    std::chrono::steady_clock::time_point windowPointedAt_{};
    float dragDistance_ = 0.0f;
    glm::vec3 dragFromHit_{0.0f};  // the window's centre from where the ray took its bar
    bool dragTriggerWas_ = false;
    glm::vec3 windowHead_{0.0f};
    std::chrono::steady_clock::time_point windowsMoved_{};
    struct HandData;
    HandData* hands_ = nullptr;  // null without hand tracking
    HandPoses handsInWorld_;
    /// Yards of the world to a metre of the room.
    [[nodiscard]] float yardsPerMetre() const { return 1.0f / (0.9144f * worldScale_); }
    int vignetteLevel_ = 1;
    // How far the vignette has closed, 0 to 1, and what closes it: the
    // character's own motion, read off where the head is stood from one frame
    // to the next, and a smooth turn, which the controller code reports.
    float vignetteStrength_ = 0.0f;
    bool turningSmoothly_ = false;
    bool vignetteHasReference_ = false;
    glm::vec3 vignetteReference_{0.0f};
    double vignetteReferenceTime_ = 0.0;
    bool bodyFollowsHead_ = true;
    // The head's turn round the vertical in the tracking space, as last seen,
    // and how much of its turn since recentring the game's camera has been
    // given. Radians, anticlockwise from above.
    float headYaw_ = 0.0f;
    float bodyYaw_ = 0.0f;
    // The actions and what they hold down; xr_input.cpp. Null without controllers.
    struct InputData;
    InputData* input_ = nullptr;
    // What the buttons do; read from vr_controls.cfg when the actions are made.
    ControlBindings controls_ = ControlBindings::defaults();
    bool controlsLoaded_ = false;
    // What recenter took: a place in the tracking space and a turn round its
    // vertical, taken off every eye pose before it is stood in the world.
    bool recenterPending_ = false;
    glm::vec3 headOrigin_{0.0f};
    float headOriginYaw_ = 0.0f;
    // Where the last stereoViews stood the tracking space in the world.
    bool worldPlaced_ = false;
    glm::vec3 worldReference_{0.0f};
    glm::mat3 worldBasis_{1.0f};
    // The last whole second, for drawFrameRate.
    uint32_t lastSecondFrames_ = 0;
    uint32_t lastSecondMissed_ = 0;
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
