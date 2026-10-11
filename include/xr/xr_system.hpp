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

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::xr {

class XrSystem {
public:
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

private:
    bool createInstance();
    bool findHeadset();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    std::vector<std::string> runtimeExtensions_;
    uint32_t eyeWidth_ = 0;
    uint32_t eyeHeight_ = 0;
    bool debug_ = false;
    int dumpFrames_ = 0;
};

/// The one XrSystem. The application starts it and shuts it down.
XrSystem& system();

}  // namespace wowee::xr
