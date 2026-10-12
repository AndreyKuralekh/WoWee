// The controllers: the OpenXR actions, and what is done with them.
//
// Nothing here reaches the game directly. A stick holds down the keys a
// keyboard would, a turn is the turn a mouse makes, and from there on the
// client does what it always did with them - the checks that stop a seated
// character walking or a chat line being typed into the world included.
//
// The right controller is the mouse as well. Where its ray meets the panel
// the real pointer is put, as the gamepad puts it, so that everything asking
// where the mouse is gets one answer; the trigger and the grip are its buttons.
// In the world the ray goes on past the panel, and a click that no window
// takes is a click on whatever the ray meets: the camera is given the ray to
// pick along (Camera::setPickRay), and the click itself is the mouse's.

#include "xr/xr_system.hpp"

#include "core/input.hpp"
#include "core/logger.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/framexml_takeover.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
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
    XrAction aim = XR_NULL_HANDLE;       // where the right controller points
    XrAction click = XR_NULL_HANDLE;     // its trigger: the left mouse button
    XrAction grab = XR_NULL_HANDLE;      // its grip: the right mouse button
    XrSpace aimSpace = XR_NULL_HANDLE;
    bool attached = false;

    // The pointer: which buttons it holds, where it last put the mouse, and
    // how far along the ray the panel was - nothing when it missed.
    bool mouseHeld[2] = {};
    glm::vec2 mouseAt{-1000.0f};
    float beamLength = 0.0f;
    // Whether the ray is what picks in the world just now; pickRay.
    bool inCharge = false;
    // A press that began with no window under it: the cursor is kept where it
    // was until the button comes up, or a hand that shook would make a drag
    // of every click.
    bool steady = false;
    // A press that began beside the panel. The interface is told the mouse
    // has left for as long as it lasts, and for one frame more: the release is
    // acted on in the frame it happens.
    bool worldPress = false;
    bool worldPressOver = false;

    // What this is holding down, so that only that is ever let go: the
    // keyboard and the gamepad hold the same keys by the same means.
    bool held[4] = {};  // W, S, Q, E
    bool jumpHeld = false;
    bool turnArmed = true;
    bool recenterWasDown = false;
};

namespace {

constexpr SDL_Scancode kMoveKeys[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E};
constexpr int kMouseButtons[2] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT};

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
    in.aim = createAction(in.set, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    in.click = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "click", "Left mouse button");
    in.grab = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "grab", "Right mouse button");
    if (!in.move || !in.turn || !in.jump || !in.recenter || !in.aim || !in.click || !in.grab) {
        LOG_WARNING("VR: no controllers - an action could not be created");
        return false;
    }

    // The Touch controllers, which is what a Quest has and what every runtime
    // for one offers.
    const XrActionSuggestedBinding touch[] = {
        {in.aim, path(instance_, "/user/hand/right/input/aim/pose")},
        {in.click, path(instance_, "/user/hand/right/input/trigger/value")},
        {in.grab, path(instance_, "/user/hand/right/input/squeeze/value")},
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

    // The simple controller, which every runtime has to offer: no sticks, so
    // it only points and clicks. Refused, it is merely not there.
    const XrActionSuggestedBinding simple[] = {
        {in.aim, path(instance_, "/user/hand/right/input/aim/pose")},
        {in.click, path(instance_, "/user/hand/right/input/select/click")},
    };
    suggested.interactionProfile = path(instance_, "/interaction_profiles/khr/simple_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(std::size(simple));
    suggested.suggestedBindings = simple;
    result = xrSuggestInteractionProfileBindings(instance_, &suggested);
    if (XR_FAILED(result) && debug_) {
        LOG_WARNING("VR: the simple controller's bindings were refused: ", resultName(result));
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

    XrActionSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
    spaceInfo.action = in.aim;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    result = xrCreateActionSpace(session_, &spaceInfo, &in.aimSpace);
    if (XR_FAILED(result)) {
        in.aimSpace = XR_NULL_HANDLE;
        LOG_WARNING("VR: no pointer - xrCreateActionSpace failed: ", resultName(result));
    }
    if (debug_) {
        LOG_WARNING("VR: controllers - left stick moves, right stick turns and recentres, A jumps, "
                    "the right controller points, trigger and grip are the mouse buttons");
    }
    return true;
}

void XrSystem::destroyActions() {
    if (!input_) return;
    releaseControllerKeys();
    releasePointer();
    if (input_->aimSpace != XR_NULL_HANDLE) xrDestroySpace(input_->aimSpace);
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

void XrSystem::releasePointer() {
    if (!input_) return;
    for (size_t i = 0; i < std::size(kMouseButtons); ++i) {
        if (!input_->mouseHeld[i]) continue;
        input_->mouseHeld[i] = false;
        core::Input::getInstance().setVirtualMouseButton(kMouseButtons[i], false);
        if (ImGui::GetCurrentContext()) ImGui::GetIO().AddMouseButtonEvent(static_cast<int>(i), false);
    }
    if (input_->worldPress && ImGui::GetCurrentContext()) {
        float x = 0.0f;
        float y = 0.0f;
        SDL_GetMouseState(&x, &y);
        ImGui::GetIO().AddMousePosEvent(x, y);
    }
    input_->worldPress = false;
    input_->worldPressOver = false;
    input_->steady = false;
    input_->inCharge = false;
    input_->beamLength = 0.0f;
}

bool XrSystem::pickRay(glm::vec3& origin, glm::vec3& direction) const {
    if (!input_ || !input_->inCharge || !worldPlaced_) return false;
    glm::vec3 from;
    glm::vec3 along;
    if (!aimRay(localSpace_, frameTime(), from, along)) return false;
    // As stereoViews places an eye.
    constexpr float kYardsPerMetre = 1.0f / 0.9144f;
    const glm::quat recentre = glm::angleAxis(-(headOriginYaw_ + bodyYaw_), glm::vec3(0.0f, 1.0f, 0.0f));
    origin = worldReference_ + worldBasis_ * ((recentre * (from - headOrigin_)) * kYardsPerMetre);
    direction = glm::normalize(worldBasis_ * (recentre * along));
    return true;
}

bool XrSystem::aimRay(XrSpace base, XrTime time, glm::vec3& origin, glm::vec3& direction) const {
    if (!input_ || input_->aimSpace == XR_NULL_HANDLE || base == XR_NULL_HANDLE || time == 0) return false;
    XrSpaceLocation location{};
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(input_->aimSpace, base, time, &location))) return false;
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & needed) != needed) return false;
    const XrQuaternionf& o = location.pose.orientation;
    origin = glm::vec3(location.pose.position.x, location.pose.position.y, location.pose.position.z);
    direction = glm::quat(o.w, o.x, o.y, o.z) * glm::vec3(0.0f, 0.0f, -1.0f);
    return true;
}

// A sheet has no thickness, so it is turned about the ray to face the head:
// from there it is a line whichever way the controller points.
bool XrSystem::pointerBeam(XrTime time, const glm::vec3& head, XrPosef& pose, XrExtent2Df& size) const {
    constexpr float kStart = 0.05f;   // metres ahead of the controller
    constexpr float kLength = 1.5f;   // before the pointer has said how far
    constexpr float kWidth = 0.004f;
    glm::vec3 origin;
    glm::vec3 along;
    if (!aimRay(localSpace_, time, origin, along)) return false;
    const float end = input_->beamLength > kStart * 2.0f ? input_->beamLength : kLength;
    const glm::vec3 middle = origin + along * (0.5f * (kStart + end));
    glm::vec3 facing = head - middle;
    facing -= along * glm::dot(facing, along);
    if (glm::dot(facing, facing) < 1e-6f) return false;
    facing = glm::normalize(facing);
    const glm::quat turn = glm::quat_cast(glm::mat3(glm::cross(along, facing), along, facing));
    pose.orientation = {turn.x, turn.y, turn.z, turn.w};
    pose.position = {middle.x, middle.y, middle.z};
    size = {kWidth, end - kStart};
    return true;
}

// The mouse, put where the ray meets the panel. Left alone when it misses, so
// that a controller lying on the desk does not take the mouse away.
void XrSystem::updatePointer(SDL_Window* window, bool inWorld) {
    InputData& in = *input_;
    // As far as the panel where a window takes the click, and before there is
    // a world; on into the world otherwise. Nothing hides a layer, so the ray
    // does not stop at what it meets there.
    constexpr float kIntoWorld = 20.0f;
    constexpr float kBesidePanel = 1.5f;
    in.beamLength = inWorld ? kIntoWorld : kBesidePanel;
    ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
    // Both as of the last frame, which is where the cursor still is.
    const bool overUi = (io && io->WantCaptureMouse) || ui::frameXmlOwnsMouse();
    const bool anyHeld = in.mouseHeld[0] || in.mouseHeld[1];

    bool onPanel = false;
    glm::vec2 at(0.0f);
    XrSpace space = XR_NULL_HANDLE;
    XrPosef quad{};
    XrExtent2Df size{};
    glm::vec3 origin;
    glm::vec3 direction;
    const bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (focused && panelQuad(space, quad, size) && aimRay(space, frameTime(), origin, direction)) {
        const glm::quat turn(quad.orientation.w, quad.orientation.x, quad.orientation.y, quad.orientation.z);
        const glm::vec3 centre(quad.position.x, quad.position.y, quad.position.z);
        const glm::vec3 normal = turn * glm::vec3(0.0f, 0.0f, 1.0f);
        // From the front only, and not along the sheet.
        const float closing = glm::dot(direction, normal);
        const float distance = closing < -0.05f ? glm::dot(centre - origin, normal) / closing : 0.0f;
        if (distance > 0.0f) {
            const glm::vec3 hit = origin + direction * distance - centre;
            const float u = glm::dot(hit, turn * glm::vec3(1.0f, 0.0f, 0.0f)) / size.width + 0.5f;
            const float v = 0.5f - glm::dot(hit, turn * glm::vec3(0.0f, 1.0f, 0.0f)) / size.height;
            int w = 0;
            int h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f && w > 0 && h > 0) {
                onPanel = true;
                if (overUi || !inWorld) in.beamLength = distance;
                at = glm::vec2(u * static_cast<float>(w - 1), v * static_cast<float>(h - 1));
            }
        }
    }

    if (onPanel && !(anyHeld && in.steady)) {
        // A hand is never still: near where the pointer already is it goes
        // half the way each frame, and anything under a pixel is let be.
        constexpr float kSteadyWithin = 40.0f;
        const float moved = glm::distance(at, in.mouseAt);
        if (moved < kSteadyWithin) at = in.mouseAt + (at - in.mouseAt) * 0.5f;
        if (moved > 1.0f) {
            in.mouseAt = at;
            SDL_WarpMouseInWindow(window, at.x, at.y);
            // Told at once as well: a click in this same frame has to land here.
            if (io) io->AddMousePosEvent(at.x, at.y);
        }
    }

    // A press starts on the panel, or in the world beside it; once down, a
    // button is held until it is let go, wherever the ray has gone since -
    // that is what a drag is.
    // Beside the panel only with a ray to click along.
    const bool tracked = focused && inWorld && aimRay(localSpace_, frameTime(), origin, direction);
    const bool wanted[2] = {button(session_, in.click), button(session_, in.grab)};
    for (size_t i = 0; i < std::size(kMouseButtons); ++i) {
        const bool down = wanted[i] && (in.mouseHeld[i] || onPanel || tracked);
        if (down == in.mouseHeld[i]) continue;
        if (down && !in.mouseHeld[0] && !in.mouseHeld[1]) {
            in.steady = inWorld && !(onPanel && overUi);
            if (!onPanel) {
                // The cursor is wherever it was left, and may well be on a
                // window: this click is not for that window.
                in.worldPress = true;
                in.worldPressOver = false;
                if (io) io->AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
        }
        in.mouseHeld[i] = down;
        core::Input::getInstance().setVirtualMouseButton(kMouseButtons[i], down);
        if (io) io->AddMouseButtonEvent(static_cast<int>(i), down);
    }

    in.inCharge = inWorld && (onPanel || in.worldPress);
    if (in.worldPress && !in.mouseHeld[0] && !in.mouseHeld[1]) {
        if (in.worldPressOver) {
            in.worldPress = false;
            float x = 0.0f;
            float y = 0.0f;
            SDL_GetMouseState(&x, &y);
            if (io) io->AddMousePosEvent(x, y);
        }
        in.worldPressOver = true;
    }
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

void XrSystem::updateControllers(rendering::CameraController* camera, bool inWorld, float deltaTime,
                                 SDL_Window* window) {
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
    if (!running_) {
        releaseControllerKeys();
        releasePointer();
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
        releasePointer();
        return;
    }

    // The pointer works wherever there is a panel: the login screen and the
    // character list are nothing else.
    updatePointer(window, live);
    if (!live) {
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
