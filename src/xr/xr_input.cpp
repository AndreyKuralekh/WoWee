// The controllers: the OpenXR actions, and what is done with them.
//
// Nothing here reaches the game directly. A stick holds down the keys a
// keyboard would, a turn is the turn a mouse makes, and from there on the
// client does what it always did with them - the checks that stop a seated
// character walking or a chat line being typed into the world included.

#include "xr/xr_system.hpp"

#include "core/input.hpp"
#include "core/logger.hpp"
#include "rendering/camera_controller.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace wowee::xr {

struct XrSystem::InputData {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction move = XR_NULL_HANDLE;      // left stick
    XrAction turn = XR_NULL_HANDLE;      // right stick
    XrAction jump = XR_NULL_HANDLE;      // A
    XrAction recenter = XR_NULL_HANDLE;  // pressing the right stick in
    bool attached = false;

    // What this is holding down, so that only that is ever let go: the
    // keyboard and the gamepad hold the same keys by the same means.
    bool held[4] = {};  // W, S, Q, E
    bool jumpHeld = false;
    bool turnArmed = true;
    bool recenterWasDown = false;
};

namespace {

constexpr SDL_Scancode kMoveKeys[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E};

XrPath path(XrInstance instance, const char* text) {
    XrPath result = XR_NULL_PATH;
    xrStringToPath(instance, text, &result);
    return result;
}

XrAction createAction(XrActionSet set, XrActionType type, const char* name, const char* shown) {
    XrActionCreateInfo info{};
    info.type = XR_TYPE_ACTION_CREATE_INFO;
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, shown, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(set, &info, &action))) return XR_NULL_HANDLE;
    return action;
}

glm::vec2 stick(XrSession session, XrAction action) {
    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    XrActionStateVector2f state{};
    state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    if (XR_FAILED(xrGetActionStateVector2f(session, &info, &state)) || !state.isActive) return glm::vec2(0.0f);
    return glm::vec2(state.currentState.x, state.currentState.y);
}

bool button(XrSession session, XrAction action) {
    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    XrActionStateBoolean state{};
    state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    if (XR_FAILED(xrGetActionStateBoolean(session, &info, &state)) || !state.isActive) return false;
    return state.currentState == XR_TRUE;
}

float wrapRadians(float angle) {
    constexpr float kPi = 3.14159265358979f;
    return angle - 2.0f * kPi * std::floor((angle + kPi) / (2.0f * kPi));
}

// A stick as a key: down past `press`, and up again only under `release`, so
// that one resting near the edge does not chatter.
bool pushed(float value, bool wasDown) {
    constexpr float kPress = 0.5f;
    constexpr float kRelease = 0.3f;
    return value > (wasDown ? kRelease : kPress);
}

}  // namespace

// Without controllers the headset is still a headset: a failure here is said
// once and the keyboard and the mouse carry on.
bool XrSystem::createActions() {
    destroyActions();
    input_ = new InputData();
    InputData& in = *input_;

    XrActionSetCreateInfo setInfo{};
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::strncpy(setInfo.actionSetName, "world", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "World", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    XrResult result = xrCreateActionSet(instance_, &setInfo, &in.set);
    if (XR_FAILED(result)) {
        in.set = XR_NULL_HANDLE;
        LOG_WARNING("VR: no controllers - xrCreateActionSet failed: ", resultName(result));
        return false;
    }

    in.move = createAction(in.set, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
    in.turn = createAction(in.set, XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn");
    in.jump = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "jump", "Jump");
    in.recenter = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter");
    if (!in.move || !in.turn || !in.jump || !in.recenter) {
        LOG_WARNING("VR: no controllers - an action could not be created");
        return false;
    }

    // The Touch controllers, which is what a Quest has and what every runtime
    // for one offers. The simple controller has no sticks to bind any of this
    // to; it gets its turn when there is a trigger to pull.
    const XrActionSuggestedBinding touch[] = {
        {in.move, path(instance_, "/user/hand/left/input/thumbstick")},
        {in.turn, path(instance_, "/user/hand/right/input/thumbstick")},
        {in.jump, path(instance_, "/user/hand/right/input/a/click")},
        {in.recenter, path(instance_, "/user/hand/right/input/thumbstick/click")},
    };
    XrInteractionProfileSuggestedBinding suggested{};
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = path(instance_, "/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(std::size(touch));
    suggested.suggestedBindings = touch;
    result = xrSuggestInteractionProfileBindings(instance_, &suggested);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: no controllers - the runtime refused the bindings: ", resultName(result));
        return false;
    }

    XrSessionActionSetsAttachInfo attach{};
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets = &in.set;
    result = xrAttachSessionActionSets(session_, &attach);
    if (XR_FAILED(result)) {
        LOG_WARNING("VR: no controllers - xrAttachSessionActionSets failed: ", resultName(result));
        return false;
    }
    in.attached = true;
    if (debug_) LOG_WARNING("VR: controllers - left stick moves, right stick turns and recentres, A jumps");
    return true;
}

void XrSystem::destroyActions() {
    if (!input_) return;
    releaseControllerKeys();
    // The actions go with their set.
    if (input_->set != XR_NULL_HANDLE) xrDestroyActionSet(input_->set);
    delete input_;
    input_ = nullptr;
}

void XrSystem::releaseControllerKeys() {
    if (!input_) return;
    core::Input& input = core::Input::getInstance();
    for (size_t i = 0; i < std::size(kMoveKeys); ++i) {
        if (input_->held[i]) input.setVirtualKey(kMoveKeys[i], false);
        input_->held[i] = false;
    }
    if (input_->jumpHeld) input.setVirtualKey(SDL_SCANCODE_SPACE, false);
    input_->jumpHeld = false;
}

// Which controller the runtime decided it is talking to, said when it decides.
void XrSystem::logInteractionProfile() {
    if (!debug_ || session_ == XR_NULL_HANDLE) return;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        XrInteractionProfileState state{};
        state.type = XR_TYPE_INTERACTION_PROFILE_STATE;
        if (XR_FAILED(xrGetCurrentInteractionProfile(session_, path(instance_, hand), &state))) continue;
        char name[XR_MAX_PATH_LENGTH] = "none";
        uint32_t length = 0;
        if (state.interactionProfile != XR_NULL_PATH) {
            xrPathToString(instance_, state.interactionProfile, XR_MAX_PATH_LENGTH, &length, name);
        }
        LOG_WARNING("VR: ", hand, " is ", name);
    }
}

void XrSystem::updateControllers(rendering::CameraController* camera, bool inWorld, float deltaTime) {
    // In a headset the way the view faces is the way the body faces: walking
    // forward has to go where forward looks, whatever turned the view there -
    // a controller, or the mouse.
    const bool live = running_ && inWorld && camera;
    if (live) camera->setSteering(true);

    // The head's turn, handed to the game as a turn of its camera - which is
    // the turn of the character - and counted, so that the view takes the same
    // angle off again and nothing is seen to move. Counted by what the camera
    // did, not by what it was asked: it may refuse. A degree of slack keeps a
    // head that is merely alive from being reported to the server all day.
    // Only from the character's own eyes: seen from behind, the camera would
    // swing round the character with every glance.
    if (live && bodyFollowsHead_ && firstPerson_) {
        const float turned = wrapRadians(wrapRadians(headYaw_ - headOriginYaw_) - bodyYaw_);
        if (std::abs(turned) > glm::radians(1.0f)) {
            const float before = camera->getYaw();
            const float perDegree = 1.0f / std::max(camera->getMouseSensitivity(), 0.0001f);
            camera->applyLookDelta(-glm::degrees(turned) * perDegree, 0.0f);
            bodyYaw_ = wrapRadians(bodyYaw_ + wrapRadians(glm::radians(camera->getYaw() - before)));
        }
    }

    if (!input_ || !input_->attached) return;
    InputData& in = *input_;
    if (!live) {
        releaseControllerKeys();
        return;
    }

    XrActiveActionSet active{};
    active.actionSet = in.set;
    XrActionsSyncInfo sync{};
    sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a success, and means nothing was read: the
    // headset is off, or the runtime's own menu is up.
    if (xrSyncActions(session_, &sync) != XR_SUCCESS) {
        releaseControllerKeys();
        return;
    }

    core::Input& input = core::Input::getInstance();
    const glm::vec2 move = stick(session_, in.move);
    // Q and E sidestep, as the gamepad's stick does; A and D would turn.
    const float pushes[4] = {move.y, -move.y, -move.x, move.x};
    for (size_t i = 0; i < std::size(kMoveKeys); ++i) {
        const bool down = pushed(pushes[i], in.held[i]);
        if (down != in.held[i]) input.setVirtualKey(kMoveKeys[i], down);
        in.held[i] = down;
    }

    const bool jump = button(session_, in.jump);
    if (jump != in.jumpHeld) input.setVirtualKey(SDL_SCANCODE_SPACE, jump);
    in.jumpHeld = jump;

    const bool recenterDown = button(session_, in.recenter);
    if (recenterDown && !in.recenterWasDown) recenter();
    in.recenterWasDown = recenterDown;

    // The turn is the mouse's: so many pixels of it, which the camera scales
    // by its own sensitivity - divided out here so that the angle is an angle.
    // Not while a line is being typed, as the keys are not.
    const float turn = stick(session_, in.turn).x;
    const bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    const float perDegree = 1.0f / std::max(camera->getMouseSensitivity(), 0.0001f);
    if (typing) {
        in.turnArmed = false;
    } else if (snapTurn_) {
        constexpr float kSnapDegrees = 45.0f;
        constexpr float kSnapAt = 0.7f;
        constexpr float kRearmUnder = 0.3f;
        if (in.turnArmed && std::abs(turn) > kSnapAt) {
            in.turnArmed = false;
            camera->applyLookDelta(std::copysign(kSnapDegrees, turn) * perDegree, 0.0f);
        } else if (std::abs(turn) < kRearmUnder) {
            in.turnArmed = true;
        }
    } else {
        constexpr float kDegreesPerSecond = 120.0f;
        constexpr float kDeadZone = 0.2f;
        if (std::abs(turn) > kDeadZone) {
            camera->applyLookDelta(turn * kDegreesPerSecond * deltaTime * perDegree, 0.0f);
        }
    }
}

}  // namespace wowee::xr
