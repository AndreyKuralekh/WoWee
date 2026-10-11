#include "xr/xr_platform.hpp"

#include "core/logger.hpp"

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr_platform.h>
#endif

namespace wowee::xr::platform {

#ifdef __ANDROID__

namespace {

// The VM and the activity, asked of SDL once: SDLActivity owns both, and they
// do not change while the process lives.
struct AndroidHandles {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
};

const AndroidHandles& androidHandles() {
    static const AndroidHandles handles = [] {
        AndroidHandles h;
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        if (env) env->GetJavaVM(&h.vm);
        h.activity = static_cast<jobject>(SDL_GetAndroidActivity());
        return h;
    }();
    return handles;
}

}  // namespace

bool initializeLoader() {
    const AndroidHandles& h = androidHandles();
    if (!h.vm || !h.activity) {
        LOG_WARNING("VR: no Java VM or activity from SDL - the OpenXR loader cannot start");
        return false;
    }

    PFN_xrInitializeLoaderKHR initializeLoaderKHR = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoaderKHR))) ||
        !initializeLoaderKHR) {
        LOG_WARNING("VR: the OpenXR loader has no xrInitializeLoaderKHR");
        return false;
    }

    XrLoaderInitInfoAndroidKHR info{};
    info.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    info.applicationVM = h.vm;
    info.applicationContext = h.activity;
    const XrResult result =
        initializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info));
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: xrInitializeLoaderKHR failed: ", static_cast<int>(result));
        return false;
    }
    return true;
}

std::vector<const char*> requiredInstanceExtensions() {
    return {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
}

const void* instanceCreateInfoNext() {
    static XrInstanceCreateInfoAndroidKHR info{};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    const AndroidHandles& h = androidHandles();
    info.applicationVM = h.vm;
    info.applicationActivity = h.activity;
    return &info;
}

#else

bool initializeLoader() {
    return true;
}

std::vector<const char*> requiredInstanceExtensions() {
    return {};
}

const void* instanceCreateInfoNext() {
    return nullptr;
}

#endif

}  // namespace wowee::xr::platform
