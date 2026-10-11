#pragma once

/**
 * xr_platform.hpp - the part of OpenXR start-up that differs by platform.
 *
 * Everything else under xr/ is the same code on Windows and on Android, and
 * this is what lets it be: the loader on Android has to be handed the Java VM
 * and the activity before its first call, and the instance has to be created
 * with them chained in, while on a desktop there is nothing to do at all.
 *
 * Only OpenXR extensions go here. Nothing vendor specific that has no
 * extension behind it, and no second VR API.
 */

#include <openxr/openxr.h>

#include <vector>

namespace wowee::xr::platform {

/// Prepares the OpenXR loader. Has to precede every other OpenXR call.
///
/// False means the loader cannot be used on this device, and the client
/// carries on flat.
[[nodiscard]] bool initializeLoader();

/// Instance extensions this platform cannot create an instance without.
[[nodiscard]] std::vector<const char*> requiredInstanceExtensions();

/// What to chain into XrInstanceCreateInfo::next, or nullptr.
///
/// The structure lives as long as the process, so the pointer stays good
/// through xrCreateInstance.
[[nodiscard]] const void* instanceCreateInfoNext();

}  // namespace wowee::xr::platform
