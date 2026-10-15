// The hand trackers: where each joint of the player's hands is, every frame.
// See XrSystem::handsInWorld and xr/xr_hands.hpp.
//
// XR_EXT_hand_tracking reports the joints whatever drives them. With
// XR_EXT_hand_tracking_data_source the runtime is asked for the hands the
// controllers imply as well as the ones its cameras see: holding a Touch
// controller, the fingers follow the trigger, the grip and the thumb resting
// on the buttons.

#include "xr/xr_system.hpp"

#include "core/logger.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::xr {

struct XrSystem::HandData {
    PFN_xrCreateHandTrackerEXT create = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy = nullptr;
    PFN_xrLocateHandJointsEXT locate = nullptr;
    XrHandTrackerEXT tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrHandJointLocationEXT, kHandJointCount> joints[2]{};
    bool located[2] = {false, false};
    bool loggedSource[2] = {false, false};
};

namespace {

bool handTrackingWanted = false;
bool dataSourceWanted = false;

template <typename Fn>
Fn instanceFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) return nullptr;
    return reinterpret_cast<Fn>(fn);
}

}  // namespace

void XrSystem::enableHandTrackingExtensions(std::vector<const char*>& enabled) {
    handTrackingWanted = runtimeOffers(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    dataSourceWanted = handTrackingWanted && runtimeOffers(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    if (handTrackingWanted) enabled.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (dataSourceWanted) enabled.push_back(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    if (!handTrackingWanted) LOG_WARNING("VR: the runtime has no hand tracking - no hands are drawn");
}

void XrSystem::createHandTrackers() {
    destroyHandTrackers();
    if (!handTrackingWanted || session_ == XR_NULL_HANDLE) return;

    XrSystemHandTrackingPropertiesEXT handProperties{};
    handProperties.type = XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT;
    XrSystemProperties properties{};
    properties.type = XR_TYPE_SYSTEM_PROPERTIES;
    properties.next = &handProperties;
    if (XR_FAILED(xrGetSystemProperties(instance_, systemId_, &properties)) ||
        handProperties.supportsHandTracking != XR_TRUE) {
        LOG_WARNING("VR: the headset does not track hands - no hands are drawn");
        return;
    }

    auto* data = new HandData();
    data->create = instanceFunction<PFN_xrCreateHandTrackerEXT>(instance_, "xrCreateHandTrackerEXT");
    data->destroy = instanceFunction<PFN_xrDestroyHandTrackerEXT>(instance_, "xrDestroyHandTrackerEXT");
    data->locate = instanceFunction<PFN_xrLocateHandJointsEXT>(instance_, "xrLocateHandJointsEXT");
    if (!data->create || !data->destroy || !data->locate) {
        LOG_WARNING("VR: the hand tracking functions are missing - no hands are drawn");
        delete data;
        return;
    }

    // The hands the cameras see, and the ones the controllers imply.
    XrHandTrackingDataSourceEXT sources[] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
                                             XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
    XrHandTrackingDataSourceInfoEXT sourceInfo{};
    sourceInfo.type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT;
    sourceInfo.requestedDataSourceCount = 2;
    sourceInfo.requestedDataSources = sources;

    for (uint32_t hand = 0; hand < 2; ++hand) {
        XrHandTrackerCreateInfoEXT info{};
        info.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;
        info.next = dataSourceWanted ? &sourceInfo : nullptr;
        info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        const XrResult result = data->create(session_, &info, &data->tracker[hand]);
        if (XR_FAILED(result)) {
            LOG_WARNING("VR: xrCreateHandTrackerEXT failed: ", resultName(result));
            data->tracker[hand] = XR_NULL_HANDLE;
        }
    }
    if (data->tracker[0] == XR_NULL_HANDLE && data->tracker[1] == XR_NULL_HANDLE) {
        delete data;
        return;
    }
    hands_ = data;
    LOG_WARNING("VR: hand tracking on", dataSourceWanted ? ", from the controllers as well" : "");
}

void XrSystem::destroyHandTrackers() {
    if (!hands_) return;
    for (XrHandTrackerEXT& tracker : hands_->tracker) {
        if (tracker != XR_NULL_HANDLE) hands_->destroy(tracker);
        tracker = XR_NULL_HANDLE;
    }
    delete hands_;
    hands_ = nullptr;
    handsInWorld_ = {};
}

void XrSystem::locateHands(XrTime time) {
    if (!hands_) return;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        hands_->located[hand] = false;
        if (hands_->tracker[hand] == XR_NULL_HANDLE) continue;

        XrHandTrackingDataSourceStateEXT sourceState{};
        sourceState.type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT;
        XrHandJointLocationsEXT locations{};
        locations.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
        locations.next = dataSourceWanted ? &sourceState : nullptr;
        locations.jointCount = kHandJointCount;
        locations.jointLocations = hands_->joints[hand].data();
        XrHandJointsLocateInfoEXT info{};
        info.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT;
        info.baseSpace = localSpace_;
        info.time = time;
        if (XR_FAILED(hands_->locate(hands_->tracker[hand], &info, &locations)) || locations.isActive != XR_TRUE) {
            continue;
        }
        // A hand with a joint the runtime cannot place is left out whole: a
        // finger stretched to the origin of the room is worse than no hand.
        constexpr XrSpaceLocationFlags kValid =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        bool complete = true;
        for (const XrHandJointLocationEXT& joint : hands_->joints[hand]) {
            if ((joint.locationFlags & kValid) != kValid) {
                complete = false;
                break;
            }
        }
        hands_->located[hand] = complete;
        if (debug_ && complete && dataSourceWanted && !hands_->loggedSource[hand] && sourceState.isActive) {
            LOG_WARNING("VR: ", hand == 0 ? "left" : "right", " hand tracked from ",
                        sourceState.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT ? "the controller"
                                                                                               : "the cameras");
            hands_->loggedSource[hand] = true;
        }
    }
}

void XrSystem::placeHands(const glm::mat4& trackingToWorld) {
    handsInWorld_.trackingToWorld = trackingToWorld;
    handsInWorld_.fake = fake_;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        handsInWorld_.tracked[hand] = hands_ && hands_->located[hand];
        if (!handsInWorld_.tracked[hand]) continue;
        for (uint32_t j = 0; j < kHandJointCount; ++j) {
            const XrPosef& pose = hands_->joints[hand][j].pose;
            glm::mat4 local = glm::mat4_cast(
                glm::quat(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z));
            local[3] = glm::vec4(pose.position.x, pose.position.y, pose.position.z, 1.0f);
            handsInWorld_.joint[hand][j] = trackingToWorld * local;
        }
    }
}

}  // namespace wowee::xr
