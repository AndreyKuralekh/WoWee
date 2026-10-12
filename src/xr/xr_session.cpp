// The Vulkan side of XrSystem: what the runtime needs from the client's
// Vulkan context, the session created on that context, and its events.

#include "xr/xr_system.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace wowee::xr {

namespace {

const char* sessionStateName(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_UNKNOWN:      return "UNKNOWN";
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "?";
    }
}

// Extension functions are not exported by the loader; each has to be asked of
// the instance by name.
template <typename Fn>
Fn instanceFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) return nullptr;
    return reinterpret_cast<Fn>(fn);
}

std::string versionString(XrVersion v) {
    return std::to_string(XR_VERSION_MAJOR(v)) + "." + std::to_string(XR_VERSION_MINOR(v)) + "." +
           std::to_string(XR_VERSION_PATCH(v));
}

}  // namespace

// xrGetVulkanInstanceExtensionsKHR and xrGetVulkanDeviceExtensionsKHR have one
// signature and one answer format: a single string of names with spaces between.
std::vector<std::string> XrSystem::vulkanExtensionList(const char* functionName) const {
    std::vector<std::string> names;
    if (!available()) return names;

    const auto fn = instanceFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(instance_, functionName);
    if (!fn) {
        LOG_WARNING("VR: the runtime has no ", functionName);
        return names;
    }
    uint32_t size = 0;
    XrResult result = fn(instance_, systemId_, 0, &size, nullptr);
    std::string buffer(size, '\0');
    if (XR_SUCCEEDED(result) && size > 0) {
        result = fn(instance_, systemId_, size, &size, buffer.data());
    }
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: ", functionName, " failed: ", resultName(result));
        return names;
    }

    std::istringstream stream(buffer.c_str());
    for (std::string name; stream >> name;) names.push_back(name);
    if (debug_) {
        for (const std::string& name : names) LOG_WARNING("VR: ", functionName, " asks for ", name);
    }
    return names;
}

std::vector<std::string> XrSystem::vulkanInstanceExtensions() const {
    return vulkanExtensionList("xrGetVulkanInstanceExtensionsKHR");
}

std::vector<std::string> XrSystem::vulkanDeviceExtensions() const {
    return vulkanExtensionList("xrGetVulkanDeviceExtensionsKHR");
}

VkPhysicalDevice XrSystem::vulkanGraphicsDevice(VkInstance vkInstance) const {
    if (!available() || vkInstance == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    const auto fn =
        instanceFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(instance_, "xrGetVulkanGraphicsDeviceKHR");
    if (!fn) return VK_NULL_HANDLE;

    VkPhysicalDevice device = VK_NULL_HANDLE;
    const XrResult result = fn(instance_, systemId_, vkInstance, &device);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrGetVulkanGraphicsDeviceKHR failed: ", resultName(result));
        return VK_NULL_HANDLE;
    }
    return device;
}

bool XrSystem::createSession(rendering::VkContext& ctx) {
    if (!available()) return false;
    ctx_ = &ctx;

    // From here on a failure means VR cannot run on the context the client
    // has already built, so all of it goes and the client stays flat.
    auto giveUp = [this](const std::string& why) {
        LOG_WARNING("VR: ", why, " - continuing as the flat client");
        shutdown();
        return false;
    };

    // The specification wants this asked before the session is created, and
    // refuses the session otherwise.
    const auto getRequirements = instanceFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(
        instance_, "xrGetVulkanGraphicsRequirementsKHR");
    if (!getRequirements) return giveUp("the runtime has no xrGetVulkanGraphicsRequirementsKHR");
    XrGraphicsRequirementsVulkanKHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    XrResult result = getRequirements(instance_, systemId_, &requirements);
    if (XR_FAILED(result)) {
        return giveUp("xrGetVulkanGraphicsRequirementsKHR failed: " + resultName(result));
    }
    if (debug_) {
        LOG_WARNING("VR: the runtime takes Vulkan ", versionString(requirements.minApiVersionSupported),
                    " to ", versionString(requirements.maxApiVersionSupported));
    }

    const VkPhysicalDevice wanted = vulkanGraphicsDevice(ctx.getInstance());
    if (wanted == VK_NULL_HANDLE || wanted != ctx.getPhysicalDevice()) {
        return giveUp("the client's Vulkan device is not the one the headset is driven from");
    }
    if (debug_) LOG_WARNING("VR: session on ", ctx.getGpuName());

    XrGraphicsBindingVulkanKHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    binding.instance = ctx.getInstance();
    binding.physicalDevice = ctx.getPhysicalDevice();
    binding.device = ctx.getDevice();
    binding.queueFamilyIndex = ctx.getGraphicsQueueFamily();
    // The graphics queue is the first of its family; the upload queue, when
    // there is one, is the second.
    binding.queueIndex = 0;

    XrSessionCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    createInfo.next = &binding;
    createInfo.systemId = systemId_;
    result = xrCreateSession(instance_, &createInfo, &session_);
    if (XR_FAILED(result)) {
        session_ = XR_NULL_HANDLE;
        return giveUp("xrCreateSession failed: " + resultName(result));
    }

    if (!createReferenceSpaces()) return giveUp("the runtime gave no local reference space");
    if (!createEyeSwapchains()) return giveUp("the eye swapchains could not be created");

    LOG_WARNING("VR: session created");
    return true;
}

bool XrSystem::createReferenceSpaces() {
    XrReferenceSpaceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    createInfo.poseInReferenceSpace.orientation.w = 1.0f;

    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    const XrResult result = xrCreateReferenceSpace(session_, &createInfo, &localSpace_);
    if (XR_FAILED(result)) {
        localSpace_ = XR_NULL_HANDLE;
        LOG_WARNING("VR: no LOCAL reference space: ", resultName(result));
        return false;
    }

    // The stage is the room, with its floor; a runtime that was never told
    // about one does not offer it, and that is not a reason to refuse VR.
    uint32_t count = 0;
    xrEnumerateReferenceSpaces(session_, 0, &count, nullptr);
    std::vector<XrReferenceSpaceType> types(count);
    if (count > 0) xrEnumerateReferenceSpaces(session_, count, &count, types.data());
    if (std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != types.end()) {
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session_, &createInfo, &stageSpace_))) {
            stageSpace_ = XR_NULL_HANDLE;
        }
    }
    // The head, as a space: where a panel goes that is to stay in front of the
    // eyes whatever they do. Only missed if that panel is asked for.
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(session_, &createInfo, &viewSpace_))) viewSpace_ = XR_NULL_HANDLE;
    if (debug_) {
        LOG_WARNING("VR: reference spaces LOCAL",
                    stageSpace_ != XR_NULL_HANDLE ? " and STAGE" : " (no STAGE)");
    }
    return true;
}

void XrSystem::destroySession() {
    if (running_) {
        // Asked to stop rather than cut off: the runtime answers with STOPPING,
        // which is where the session may be ended, and it is given a second to.
        // A runtime that says nothing in that time is destroyed as it stands.
        xrRequestExitSession(session_);
        for (int i = 0; i < 100 && running_; ++i) {
            pollEvents();
            if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (session_ == XR_NULL_HANDLE) return;  // EXITING arrived and already did all of this
        endSession();
    }
    destroyEyeSwapchains();
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    if (stageSpace_ != XR_NULL_HANDLE) xrDestroySpace(stageSpace_);
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    viewSpace_ = XR_NULL_HANDLE;
    localSpace_ = XR_NULL_HANDLE;
    stageSpace_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
        if (debug_) LOG_WARNING("VR: session destroyed");
    }
    sessionState_ = XR_SESSION_STATE_UNKNOWN;
    ctx_ = nullptr;
}

void XrSystem::pollEvents() {
    if (instance_ == XR_NULL_HANDLE) return;

    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(instance_, &event) != XR_SUCCESS) return;

        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                if (changed.session == session_) onSessionStateChanged(changed.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOG_WARNING("VR: the runtime is going away - continuing as the flat client");
                shutdown();
                return;
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                LOG_WARNING("VR: the runtime dropped events");
                break;
            default:
                break;
        }
    }
}

void XrSystem::onSessionStateChanged(XrSessionState newState) {
    if (debug_) {
        LOG_WARNING("VR: session state ", sessionStateName(sessionState_), " -> ", sessionStateName(newState));
    }
    sessionState_ = newState;

    if (newState == XR_SESSION_STATE_READY) {
        beginSession();
    } else if (newState == XR_SESSION_STATE_STOPPING) {
        endSession();
    } else if (newState == XR_SESSION_STATE_EXITING || newState == XR_SESSION_STATE_LOSS_PENDING) {
        LOG_WARNING("VR: the runtime ended the session - continuing as the flat client");
        destroySession();
    }
}

}  // namespace wowee::xr
