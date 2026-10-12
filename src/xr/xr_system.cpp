#include "xr/xr_system.hpp"

#include "core/env_flag.hpp"
#include "core/logger.hpp"
#include "xr/xr_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace wowee::xr {

namespace {

// The graphics binding. Version 1 of the Vulkan extension hands back lists of
// extension names for the client to add to its own instance and device, which
// is what the client's vk-bootstrap start-up can take as it is; version 2
// wants to create both itself.
constexpr const char* kVulkanExtension = "XR_KHR_vulkan_enable";

std::string versionString(XrVersion v) {
    return std::to_string(XR_VERSION_MAJOR(v)) + "." + std::to_string(XR_VERSION_MINOR(v)) + "." +
           std::to_string(XR_VERSION_PATCH(v));
}

}  // namespace

XrSystem& system() {
    static XrSystem instance;
    return instance;
}

bool XrSystem::requested() {
    return core::envFlagEnabled("WOWEE_VR");
}

bool XrSystem::runtimeOffers(const char* extensionName) const {
    return std::find(runtimeExtensions_.begin(), runtimeExtensions_.end(), extensionName) !=
           runtimeExtensions_.end();
}

std::string XrSystem::resultName(XrResult result) const {
    // xrResultToString needs an instance, and the results worth naming most
    // are the ones that arrive before there is one.
    if (instance_ != XR_NULL_HANDLE) {
        char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
        if (XR_SUCCEEDED(xrResultToString(instance_, result, buffer))) return buffer;
    }
    switch (result) {
        case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        default:                               return "XrResult " + std::to_string(static_cast<int>(result));
    }
}

bool XrSystem::initialize() {
    // Read whether or not VR was asked for: forcing the lean frame without a
    // headset is how it is measured on a desk.
    if (const char* profile = std::getenv("WOWEE_VR_PROFILE"); profile && *profile) {
        leanForced_ = core::envValueEnables(profile, false);
        leanRefused_ = !leanForced_;
    }
    debug_ = core::envFlagEnabled("WOWEE_VR_DEBUG");
    if (core::envFlagEnabled("WOWEE_VR_FAKE")) {
        startFake();
        return false;  // no runtime behind it: nothing for the Vulkan context to ask
    }
    if (!requested()) return false;

    dumpFrames_ = core::envIntClamped("WOWEE_VR_DUMP_FRAMES", 0, 0, 10000);

    if (!platform::initializeLoader() || !createInstance() || !findHeadset()) {
        shutdown();
        LOG_WARNING("VR: not available - continuing as the flat client");
        return false;
    }
    return true;
}

bool XrSystem::createInstance() {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        // The usual way to arrive here: no OpenXR runtime is installed or active.
        LOG_WARNING("VR: no OpenXR runtime answered: ", resultName(result));
        return false;
    }
    XrExtensionProperties blankProperties{};
    blankProperties.type = XR_TYPE_EXTENSION_PROPERTIES;
    std::vector<XrExtensionProperties> properties(count, blankProperties);
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: could not list the runtime's extensions: ", resultName(result));
        return false;
    }
    runtimeExtensions_.clear();
    for (const XrExtensionProperties& p : properties) runtimeExtensions_.emplace_back(p.extensionName);

    if (debug_) {
        LOG_WARNING("VR: the runtime offers ", runtimeExtensions_.size(), " extensions");
        for (const XrExtensionProperties& p : properties) {
            LOG_WARNING("VR:   ", p.extensionName, " v", p.extensionVersion);
        }
    }

    std::vector<const char*> enabled = platform::requiredInstanceExtensions();
    enabled.push_back(kVulkanExtension);
    for (const char* name : enabled) {
        if (!runtimeOffers(name)) {
            LOG_WARNING("VR: the runtime does not offer ", name);
            return false;
        }
    }

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = platform::instanceCreateInfoNext();
    std::strncpy(createInfo.applicationInfo.applicationName, "Wowee", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(createInfo.applicationInfo.engineName, "Wowee", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    // 1.0, not the version of the headers: nothing here needs 1.1, and a
    // runtime that stops at 1.0 refuses an instance that asks for more.
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    createInfo.enabledExtensionNames = enabled.data();

    result = xrCreateInstance(&createInfo, &instance_);
    if (XR_FAILED(result)) {
        instance_ = XR_NULL_HANDLE;
        LOG_WARNING("VR: xrCreateInstance failed: ", resultName(result));
        return false;
    }

    XrInstanceProperties instanceProperties{};
    instanceProperties.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &instanceProperties))) {
        LOG_WARNING("VR: OpenXR runtime ", instanceProperties.runtimeName, " ",
                    versionString(instanceProperties.runtimeVersion));
    }
    return true;
}

bool XrSystem::findHeadset() {
    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(instance_, &systemInfo, &systemId_);
    if (XR_FAILED(result)) {
        systemId_ = XR_NULL_SYSTEM_ID;
        if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            LOG_WARNING("VR: no headset is connected to the runtime");
        } else {
            LOG_WARNING("VR: xrGetSystem failed: ", resultName(result));
        }
        return false;
    }

    XrSystemProperties systemProperties{};
    systemProperties.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &systemProperties))) {
        LOG_WARNING("VR: headset ", systemProperties.systemName, ", up to ",
                    systemProperties.graphicsProperties.maxSwapchainImageWidth, "x",
                    systemProperties.graphicsProperties.maxSwapchainImageHeight, " and ",
                    systemProperties.graphicsProperties.maxLayerCount, " layers");
    }

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               0, &viewCount, nullptr);
    XrViewConfigurationView blankView{};
    blankView.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    std::vector<XrViewConfigurationView> views(viewCount, blankView);
    if (XR_SUCCEEDED(result)) {
        result = xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                                   &viewCount, views.data());
    }
    if (XR_FAILED(result) || viewCount != 2) {
        LOG_WARNING("VR: the headset has no stereo view configuration: ", resultName(result), ", ",
                    viewCount, " views");
        return false;
    }

    eyeWidth_ = views[0].recommendedImageRectWidth;
    eyeHeight_ = views[0].recommendedImageRectHeight;
    LOG_WARNING("VR: recommended eye image ", eyeWidth_, "x", eyeHeight_, ", up to ",
                views[0].maxSwapchainSampleCount, " samples");

    // WOWEE_VR_EYE_SCALE, in percent of the recommended size along each side.
    // The eye images are most of what the GPU fills in VR, so this is the
    // first thing to turn down when a frame does not fit.
    const int scalePercent = core::envIntClamped("WOWEE_VR_EYE_SCALE", 100, 25, 200);
    if (scalePercent != 100) {
        eyeWidth_ = std::min(views[0].maxImageRectWidth, eyeWidth_ * static_cast<uint32_t>(scalePercent) / 100u);
        eyeHeight_ = std::min(views[0].maxImageRectHeight, eyeHeight_ * static_cast<uint32_t>(scalePercent) / 100u);
        LOG_WARNING("VR: eye image scaled to ", scalePercent, "%: ", eyeWidth_, "x", eyeHeight_);
    }
    return true;
}

void XrSystem::shutdown() {
    destroySession();
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }
    systemId_ = XR_NULL_SYSTEM_ID;
    eyeWidth_ = 0;
    eyeHeight_ = 0;
    runtimeExtensions_.clear();
}

}  // namespace wowee::xr
