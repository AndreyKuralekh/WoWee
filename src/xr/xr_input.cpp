// The controllers: the OpenXR actions, and what is done with them.
//
// Nothing here reaches the game directly. A stick holds down the keys a
// keyboard would, a turn is the turn a mouse makes, and from there on the
// client does what it always did with them - the checks that stop a seated
// character walking or a chat line being typed into the world included.
//
// The other buttons do what vr_controls.cfg says (xr_controls.cpp), and nearly
// all of that is a key again: an action button, Tab, Num Lock.
//
// The right controller is the mouse as well. Where its ray meets the panel
// the real pointer is put, as the gamepad puts it, so that everything asking
// where the mouse is gets one answer; the trigger and the grip are its buttons.
// In the world the ray goes on past the panel, and a click that no window
// takes is a click on whatever the ray meets: the camera is given the ray to
// pick along (Camera::setPickRay), and the click itself is the mouse's.

#include "xr/xr_system.hpp"

#include "core/application.hpp"
#include "core/input.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/scene_pick.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

namespace wowee::xr {

struct XrSystem::InputData {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction move = XR_NULL_HANDLE;      // left stick
    XrAction turn = XR_NULL_HANDLE;      // right stick
    XrAction buttons[kControlButtonCount] = {};  // everything a function can be given to
    XrAction aim = XR_NULL_HANDLE;       // where the right controller points
    XrAction aimLeft = XR_NULL_HANDLE;   // and the left: the way to walk, if asked
    XrSpace aimLeftSpace = XR_NULL_HANDLE;
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
    // The menu on the wrist of the hand that points is pressed by the other
    // hand: by its ray, which then has the pointer, its trigger and its grip,
    // or by its index finger, near enough to touch it.
    bool menuRay = false;
    bool fingerOnMenu = false;
    bool fingerDown = false;

    // What this is holding down, so that only that is ever let go: the
    // keyboard and the gamepad hold the same keys by the same means.
    bool held[4] = {};  // W, S, Q, E
    bool turnArmed = true;

    // The buttons: which are down, and what each was doing when it went down
    // - kept until it comes up, so that letting go of the modifier first does
    // not leave another key held.
    bool buttonDown[kControlButtonCount] = {};
    ControlFunction latched[kControlButtonCount] = {};
    bool keyHeld[kControlFunctionCount] = {};  // by function; see functionKey
    bool escapeDown = false;

    // The grip of the hand that points, while it does something other than
    // the right button: drawing the weapon, or loading a button.
    bool gripWasDown = false;
    bool gripTaken = false;
    // The loaded action: whether its button was down last frame, the key it
    // holds - the loaded button's, as of the press - and whether a cast was
    // already going when it went down, which letting go does not cancel.
    bool loadedWanted = false;
    bool loadedKeyDown = false;
    SDL_Scancode loadedKey = SDL_SCANCODE_UNKNOWN;
    bool castingAtPress = false;
};

namespace {

constexpr SDL_Scancode kMoveKeys[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E};
constexpr int kMouseButtons[2] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT};
// The action bar's keys, button by button, as the game polls them.
constexpr SDL_Scancode kActionKeys[12] = {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,     SDL_SCANCODE_4,
                                          SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,     SDL_SCANCODE_8,
                                          SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS};

// Where each button is on a Touch controller, and what its action is called.
struct ButtonSource {
    const char* action;
    const char* shown;
    const char* touchPath;
};
constexpr ButtonSource kButtonSources[kControlButtonCount] = {
    {"button_left_trigger", "Left trigger", "/user/hand/left/input/trigger/value"},
    {"button_left_grip", "Left grip", "/user/hand/left/input/squeeze/value"},
    {"button_a", "A", "/user/hand/right/input/a/click"},
    {"button_b", "B", "/user/hand/right/input/b/click"},
    {"button_x", "X", "/user/hand/left/input/x/click"},
    {"button_y", "Y", "/user/hand/left/input/y/click"},
    {"button_left_stick", "Left stick press", "/user/hand/left/input/thumbstick/click"},
    {"button_right_stick", "Right stick press", "/user/hand/right/input/thumbstick/click"},
    {"button_menu", "Menu", "/user/hand/left/input/menu/click"},
};

// The key a function holds down, for those that are a key: the ones the
// action bar, the camera and the target key poll.
SDL_Scancode functionKey(ControlFunction function) {
    switch (function) {
        case ControlFunction::Jump: return SDL_SCANCODE_SPACE;
        case ControlFunction::Action1: return SDL_SCANCODE_1;
        case ControlFunction::Action2: return SDL_SCANCODE_2;
        case ControlFunction::Action3: return SDL_SCANCODE_3;
        case ControlFunction::Action4: return SDL_SCANCODE_4;
        case ControlFunction::Action5: return SDL_SCANCODE_5;
        case ControlFunction::Action6: return SDL_SCANCODE_6;
        case ControlFunction::Action7: return SDL_SCANCODE_7;
        case ControlFunction::Action8: return SDL_SCANCODE_8;
        case ControlFunction::Action9: return SDL_SCANCODE_9;
        case ControlFunction::Action10: return SDL_SCANCODE_0;
        case ControlFunction::Action11: return SDL_SCANCODE_MINUS;
        case ControlFunction::Action12: return SDL_SCANCODE_EQUALS;
        case ControlFunction::TargetEnemy: return SDL_SCANCODE_TAB;
        case ControlFunction::Autorun: return SDL_SCANCODE_NUMLOCKCLEAR;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

// The action a button is read from. Right-handed it is its own; left-handed
// each is the same button of the other hand, so that a table written for
// "the left trigger" still means the trigger of the hand that does not point.
// The menu button has no twin: the right controller's belongs to the system.
XrAction buttonSource(const XrAction* buttons, XrAction rightTrigger, XrAction rightGrip, size_t button,
                      bool leftHanded) {
    if (!leftHanded) return buttons[button];
    const auto at = [buttons](ControlButton b) { return buttons[static_cast<size_t>(b)]; };
    switch (static_cast<ControlButton>(button)) {
        case ControlButton::LeftTrigger: return rightTrigger;
        case ControlButton::LeftGrip: return rightGrip;
        case ControlButton::A: return at(ControlButton::X);
        case ControlButton::B: return at(ControlButton::Y);
        case ControlButton::X: return at(ControlButton::A);
        case ControlButton::Y: return at(ControlButton::B);
        case ControlButton::LeftStick: return at(ControlButton::RightStick);
        case ControlButton::RightStick: return at(ControlButton::LeftStick);
        default: return buttons[button];
    }
}

// What a right click does when the ray meets nothing: the current target is
// looted if it is dead, spoken to if it will speak, and attacked otherwise.
void interactWithTarget() {
    game::GameHandler* game = core::Application::getInstance().getGameHandler();
    if (!game || !game->hasTarget()) return;
    const auto target = game->getTarget();
    if (!target) return;
    if (target->getType() == game::ObjectType::GAMEOBJECT) {
        game->interactWithGameObject(target->getGuid());
    } else if (target->getType() == game::ObjectType::UNIT) {
        const auto unit = std::static_pointer_cast<game::Unit>(target);
        if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
            game->lootTarget(target->getGuid());
        } else if (!unit->isHostile() && unit->isInteractable()) {
            game->interactWithNpc(target->getGuid());
        } else {
            game->startAutoAttack(target->getGuid());
        }
    }
}

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
    bool buttonsMade = true;
    for (size_t i = 0; i < kControlButtonCount; ++i) {
        in.buttons[i] = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, kButtonSources[i].action,
                                     kButtonSources[i].shown);
        buttonsMade = buttonsMade && in.buttons[i] != XR_NULL_HANDLE;
    }
    in.aim = createAction(in.set, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    in.aimLeft = createAction(in.set, XR_ACTION_TYPE_POSE_INPUT, "aim_left", "Walking direction");
    in.click = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "click", "Left mouse button");
    in.grab = createAction(in.set, XR_ACTION_TYPE_BOOLEAN_INPUT, "grab", "Right mouse button");
    if (!in.move || !in.turn || !buttonsMade || !in.aim || !in.aimLeft || !in.click || !in.grab) {
        LOG_WARNING("VR: no controllers - an action could not be created");
        return false;
    }

    // The Touch controllers, which is what a Quest has and what every runtime
    // for one offers.
    // Every button is bound to its own action here and stays so; which
    // function a button has is decided when it is pressed, so changing that
    // on the settings page needs nothing from the runtime.
    std::vector<XrActionSuggestedBinding> touch = {
        {in.aim, path(instance_, "/user/hand/right/input/aim/pose")},
        {in.aimLeft, path(instance_, "/user/hand/left/input/aim/pose")},
        {in.click, path(instance_, "/user/hand/right/input/trigger/value")},
        {in.grab, path(instance_, "/user/hand/right/input/squeeze/value")},
        {in.move, path(instance_, "/user/hand/left/input/thumbstick")},
        {in.turn, path(instance_, "/user/hand/right/input/thumbstick")},
    };
    for (size_t i = 0; i < kControlButtonCount; ++i) {
        touch.push_back({in.buttons[i], path(instance_, kButtonSources[i].touchPath)});
    }
    XrInteractionProfileSuggestedBinding suggested{};
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = path(instance_, "/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(touch.size());
    suggested.suggestedBindings = touch.data();
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
    spaceInfo.action = in.aimLeft;
    if (XR_FAILED(xrCreateActionSpace(session_, &spaceInfo, &in.aimLeftSpace))) in.aimLeftSpace = XR_NULL_HANDLE;
    loadControls();
    if (debug_) {
        LOG_WARNING("VR: controllers - left stick moves, right stick turns, the right controller points, "
                    "its trigger and grip are the mouse buttons");
        for (size_t i = 0; i < kControlButtonCount; ++i) {
            LOG_WARNING("VR:   ", kButtonSources[i].shown, " - ", controlFunctionName(controls_.plain[i]),
                        controls_.shifted[i] != ControlFunction::None ? ", with the modifier " : "",
                        controls_.shifted[i] != ControlFunction::None
                            ? controlFunctionName(controls_.shifted[i]) : "");
        }
    }
    return true;
}

void XrSystem::destroyActions() {
    if (!input_) return;
    releaseControllerKeys();
    releasePointer();
    if (input_->aimSpace != XR_NULL_HANDLE) xrDestroySpace(input_->aimSpace);
    if (input_->aimLeftSpace != XR_NULL_HANDLE) xrDestroySpace(input_->aimLeftSpace);
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
    for (size_t f = 0; f < kControlFunctionCount; ++f) {
        if (input_->keyHeld[f]) input.setVirtualKey(functionKey(static_cast<ControlFunction>(f)), false);
        input_->keyHeld[f] = false;
    }
    if (input_->escapeDown && ImGui::GetCurrentContext()) ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    input_->escapeDown = false;
    if (input_->loadedKeyDown) input.setVirtualKey(input_->loadedKey, false);
    input_->loadedKeyDown = false;
    input_->loadedWanted = false;
    // Forgotten as well as released: a button still down when the controllers
    // are live again is a fresh press.
    for (size_t i = 0; i < kControlButtonCount; ++i) {
        input_->buttonDown[i] = false;
        input_->latched[i] = ControlFunction::None;
    }
}

void XrSystem::loadControls() {
    if (controlsLoaded_) return;
    controlsLoaded_ = true;
    controls_.load(controlsConfigPath());
}

std::string XrSystem::controlSetting(const std::string& key) {
    loadControls();
    return controlSettingValue(controls_, key);
}

bool XrSystem::setControlSetting(const std::string& key, int value) {
    loadControls();
    bool changed = false;
    if (!setControlSettingValue(controls_, key, value, changed)) return false;
    if (changed) controls_.save(controlsConfigPath());
    return true;
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
    const float kYardsPerMetre = yardsPerMetre();
    const glm::quat recentre = glm::angleAxis(-(headOriginYaw_ + bodyYaw_), glm::vec3(0.0f, 1.0f, 0.0f));
    origin = worldReference_ + worldBasis_ * ((recentre * (from - headOrigin_)) * kYardsPerMetre);
    direction = glm::normalize(worldBasis_ * (recentre * along));
    return true;
}

bool XrSystem::aimRay(XrSpace base, XrTime time, glm::vec3& origin, glm::vec3& direction) const {
    // The hand that points: the right one, unless the player is left-handed.
    return handRay(pointingHandSpace(), base, time, origin, direction);
}

bool XrSystem::handRay(XrSpace hand, XrSpace base, XrTime time, glm::vec3& origin, glm::vec3& direction) const {
    if (!input_ || hand == XR_NULL_HANDLE || base == XR_NULL_HANDLE || time == 0) return false;
    XrSpaceLocation location{};
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(hand, base, time, &location))) return false;
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & needed) != needed) return false;
    const XrQuaternionf& o = location.pose.orientation;
    origin = glm::vec3(location.pose.position.x, location.pose.position.y, location.pose.position.z);
    direction = glm::quat(o.w, o.x, o.y, o.z) * glm::vec3(0.0f, 0.0f, -1.0f);
    return true;
}

XrSpace XrSystem::offHandSpace() const {
    if (!input_) return XR_NULL_HANDLE;
    return leftHanded_ ? input_->aimSpace : input_->aimLeftSpace;
}

XrSpace XrSystem::pointingHandSpace() const {
    if (!input_) return XR_NULL_HANDLE;
    return leftHanded_ ? input_->aimLeftSpace : input_->aimSpace;
}

// Which way the hand that does not point - the left, unless the player is
// left-handed - points round the vertical, as headYaw_ is measured. False
// while it is not tracked or points nearly straight up or down, which says
// nothing about a direction to walk in.
bool XrSystem::offHandYaw(float& yaw) const {
    if (!input_) return false;
    const XrSpace hand = leftHanded_ ? input_->aimSpace : input_->aimLeftSpace;
    if (hand == XR_NULL_HANDLE || localSpace_ == XR_NULL_HANDLE || frameTime() == 0) return false;
    XrSpaceLocation location{};
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(hand, localSpace_, frameTime(), &location))) return false;
    if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) return false;
    const XrQuaternionf& o = location.pose.orientation;
    const glm::vec3 forward = glm::quat(o.w, o.x, o.y, o.z) * glm::vec3(0.0f, 0.0f, -1.0f);
    if (std::abs(forward.x) + std::abs(forward.z) < 0.35f) return false;
    yaw = std::atan2(-forward.x, -forward.z);
    return true;
}

// Whether the hand that points is where a weapon is sheathed: reached back
// over the shoulder on its own side - a two-handed weapon is on the back - or
// down to the hip on the other, where a one-handed one hangs to be drawn
// across the body. Measured from the head, turned only round the vertical,
// so that it holds sitting or standing and wherever the head looks.
bool XrSystem::handAtSheath() const {
    const XrSpace hand = pointingHandSpace();
    if (!input_ || hand == XR_NULL_HANDLE || viewSpace_ == XR_NULL_HANDLE || localSpace_ == XR_NULL_HANDLE ||
        frameTime() == 0) {
        return false;
    }
    constexpr XrSpaceLocationFlags kNeeded = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    XrSpaceLocation head{};
    head.type = XR_TYPE_SPACE_LOCATION;
    XrSpaceLocation held{};
    held.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(viewSpace_, localSpace_, frameTime(), &head)) ||
        XR_FAILED(xrLocateSpace(hand, localSpace_, frameTime(), &held)) ||
        (head.locationFlags & kNeeded) != kNeeded || (held.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
        return false;
    }
    const XrQuaternionf& o = head.pose.orientation;
    glm::vec3 ahead = glm::quat(o.w, o.x, o.y, o.z) * glm::vec3(0.0f, 0.0f, -1.0f);
    ahead.y = 0.0f;
    if (glm::length(ahead) < 0.2f) return false;
    ahead = glm::normalize(ahead);
    const glm::vec3 right = glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 from = glm::vec3(held.pose.position.x, held.pose.position.y, held.pose.position.z) -
                           glm::vec3(head.pose.position.x, head.pose.position.y, head.pose.position.z);
    const float up = from.y;
    const float forward = glm::dot(from, ahead);
    const float outward = glm::dot(from, right) * (leftHanded_ ? -1.0f : 1.0f);  // to its own side
    const bool overShoulder = up > -0.2f && forward < -0.05f && outward > -0.15f;
    const bool farHip = up < -0.4f && up > -1.1f && outward < -0.08f && std::abs(forward) < 0.3f;
    return overShoulder || farHip;
}

// The ray of the hand that does not point, in the world, as pickRay's.
bool XrSystem::offHandWorldRay(glm::vec3& origin, glm::vec3& direction) const {
    if (!input_ || !worldPlaced_) return false;
    glm::vec3 from;
    glm::vec3 along;
    if (!handRay(offHandSpace(), localSpace_, frameTime(), from, along)) return false;
    const float kYardsPerMetre = yardsPerMetre();
    const glm::quat recentre = glm::angleAxis(-(headOriginYaw_ + bodyYaw_), glm::vec3(0.0f, 1.0f, 0.0f));
    origin = worldReference_ + worldBasis_ * ((recentre * (from - headOrigin_)) * kYardsPerMetre);
    direction = glm::normalize(worldBasis_ * (recentre * along));
    return true;
}

// A sheet has no thickness, so it is turned about the ray to face the head:
// from there it is a line whichever way the controller points.
bool XrSystem::pointerBeam(XrTime time, const glm::vec3& head, XrPosef& pose, XrExtent2Df& size) const {
    constexpr float kStart = 0.05f;   // metres ahead of the controller
    constexpr float kLength = 1.5f;   // before the pointer has said how far
    constexpr float kWidth = 0.004f;
    // From the other hand while its ray has the menu, and none while a finger
    // is on it: the point under the finger is what shows.
    if (input_->fingerOnMenu) return false;
    glm::vec3 origin;
    glm::vec3 along;
    if (!handRay(input_->menuRay ? offHandSpace() : pointingHandSpace(), localSpace_, time, origin, along)) return false;
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
    int windowWidth = 0;
    int windowHeight = 0;
    if (window) SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    // Where the ray meets a sheet, from its front: how far along the ray, and
    // where on the sheet from its top left, 0 to 1 across each side.
    const auto meet = [&](const XrPosef& sheet, const XrExtent2Df& extent, float& distance, glm::vec2& uv) {
        const glm::quat turn(sheet.orientation.w, sheet.orientation.x, sheet.orientation.y, sheet.orientation.z);
        const glm::vec3 centre(sheet.position.x, sheet.position.y, sheet.position.z);
        const glm::vec3 normal = turn * glm::vec3(0.0f, 0.0f, 1.0f);
        // From the front only, and not along the sheet.
        const float closing = glm::dot(direction, normal);
        distance = closing < -0.05f ? glm::dot(centre - origin, normal) / closing : 0.0f;
        if (distance <= 0.0f) return false;
        const glm::vec3 hit = origin + direction * distance - centre;
        uv.x = glm::dot(hit, turn * glm::vec3(1.0f, 0.0f, 0.0f)) / extent.width + 0.5f;
        uv.y = 0.5f - glm::dot(hit, turn * glm::vec3(0.0f, 1.0f, 0.0f)) / extent.height;
        return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
    };
    // The menu on the wrist of the hand that points, pressed by the other
    // hand. Its ray takes the pointer once it is on the menu with that hand's
    // trigger and grip let go, and keeps it until it leaves the menu by more
    // than a little; its index finger takes it within a few centimetres of the
    // menu's face, and presses where it touches. Neither changes hands while a
    // button is down.
    const bool offTrigger =
        button(session_, leftHanded_ ? in.click : in.buttons[static_cast<size_t>(ControlButton::LeftTrigger)]);
    const bool offGrip = button(session_, leftHanded_ ? in.grab : in.buttons[static_cast<size_t>(ControlButton::LeftGrip)]);
    bool rayOnMenu = false;
    bool rayNearMenu = false;
    float menuDistance = 0.0f;
    glm::vec2 rayAt(0.0f);
    bool fingerOver = false;
    float fingerDepth = 0.0f;
    glm::vec2 fingerAt(0.0f);
    if (focused && inWorld && windowWidth > 0 && windowHeight > 0) {
        XrRect2Di crop{};
        XrSpace menuSpace = XR_NULL_HANDLE;
        XrPosef menu{};
        XrExtent2Df menuSize{};
        if (handPanelQuad(2, menuSpace, menu, menuSize, crop)) {
            const glm::vec2 window(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
            const glm::vec4& rect = handPanelRect_[2];
            const auto pixel = [&](const glm::vec2& uv) {
                const glm::vec2 inside = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f));
                return glm::clamp((glm::vec2(rect.x, rect.y) + inside * glm::vec2(rect.z, rect.w)) *
                                      (window / handPanelWindow_),
                                  glm::vec2(0.0f), window - 1.0f);
            };
            const glm::quat turn(menu.orientation.w, menu.orientation.x, menu.orientation.y, menu.orientation.z);
            const glm::vec3 centre(menu.position.x, menu.position.y, menu.position.z);
            const glm::vec3 normal = turn * glm::vec3(0.0f, 0.0f, 1.0f);
            const uint32_t other = leftHanded_ ? 1 : 0;
            if (menuSpace == localSpace_ && handsInWorld_.tracked[other]) {
                constexpr float kOver = 0.03f;     // metres in front of the face: the finger has the pointer
                constexpr float kThrough = 0.04f;  // behind it: gone through, and not pressing any more
                const glm::vec3 tip(handsInWorld_.joint[other][XR_HAND_JOINT_INDEX_TIP_EXT][3]);
                fingerDepth = glm::dot(tip - centre, normal);
                const glm::vec3 onFace = tip - centre - normal * fingerDepth;
                const glm::vec2 uv(glm::dot(onFace, turn * glm::vec3(1.0f, 0.0f, 0.0f)) / menuSize.width + 0.5f,
                                   0.5f - glm::dot(onFace, turn * glm::vec3(0.0f, 1.0f, 0.0f)) / menuSize.height);
                fingerOver = uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f && fingerDepth < kOver &&
                             fingerDepth > -kThrough;
                fingerAt = pixel(uv);
            }
            if (handRay(offHandSpace(), menuSpace, frameTime(), origin, direction)) {
                constexpr float kLetGo = 0.03f;  // metres beyond the menu's edge before the ray lets it go
                glm::vec2 uv(-1.0f);
                rayOnMenu = meet(menu, menuSize, menuDistance, uv);
                const glm::vec2 slack(kLetGo / menuSize.width, kLetGo / menuSize.height);
                rayNearMenu = menuDistance > 0.0f && uv.x >= -slack.x && uv.x <= 1.0f + slack.x &&
                              uv.y >= -slack.y && uv.y <= 1.0f + slack.y;
                rayAt = pixel(uv);
            }
        }
    }
    if (!anyHeld) {
        const bool wasRay = in.menuRay;
        const bool wasFinger = in.fingerOnMenu;
        in.menuRay = in.menuRay ? rayNearMenu : rayOnMenu && !offTrigger && !offGrip;
        in.fingerOnMenu = fingerOver && !in.menuRay;
        if (debug_ && (wasRay != in.menuRay || wasFinger != in.fingerOnMenu)) {
            LOG_WARNING("VR: the menu on the wrist is ",
                        in.menuRay ? "the other hand's ray's" : (in.fingerOnMenu ? "the finger's" : "let go"));
        }
    }
    // Pressed within a few millimetres of the face - the tip joint is the
    // middle of the fingertip - and let go once drawn back off it.
    constexpr float kPress = 0.006f;
    constexpr float kRelease = 0.015f;
    if (!in.fingerOnMenu) {
        in.fingerDown = false;
    } else if (!in.fingerDown) {
        in.fingerDown = fingerOver && fingerDepth < kPress;
    } else {
        in.fingerDown = fingerOver && fingerDepth < kRelease;
    }
    const bool menuTaken = in.menuRay || in.fingerOnMenu;

    // The panels on the wrist and the windows on their own first: they are
    // nearer, and stand in front of the big one. The nearest the ray meets
    // takes it. A point on one is a point in its part of the window; a point
    // on a window's bar is for moving the window, not for the mouse.
    int barHit = -1;
    float barDistance = 0.0f;
    int windowHit = -1;
    if (menuTaken) {
        onPanel = true;
        at = in.menuRay ? rayAt : fingerAt;
        in.beamLength = in.menuRay ? menuDistance : 0.0f;
    } else if (focused && inWorld && windowWidth > 0 && windowHeight > 0) {
        const glm::vec2 window(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
        float nearest = FLT_MAX;
        const auto take = [&](float distance, const glm::vec4& rect, const glm::vec2& pixels, const glm::vec2& uv) {
            nearest = distance;
            onPanel = true;
            barHit = -1;
            windowHit = -1;
            in.beamLength = distance;
            at = (glm::vec2(rect.x, rect.y) + uv * glm::vec2(rect.z, rect.w)) * (window / pixels);
            at = glm::clamp(at, glm::vec2(0.0f), window - 1.0f);
        };
        for (uint32_t which = 0; which < 2; ++which) {
            XrRect2Di crop{};
            float distance = 0.0f;
            glm::vec2 uv(0.0f);
            if (handPanelQuad(which, space, quad, size, crop) && aimRay(space, frameTime(), origin, direction) &&
                meet(quad, size, distance, uv) && distance < nearest) {
                take(distance, handPanelRect_[which], handPanelWindow_, uv);
            }
        }
        if (aimRay(localSpace_, frameTime(), origin, direction)) {
            for (uint32_t which = 0; which < 2; ++which) {
                XrRect2Di crop{};
                float distance = 0.0f;
                glm::vec2 uv(0.0f);
                if (windowQuad(which, quad, size, crop) && meet(quad, size, distance, uv) && distance < nearest) {
                    take(distance, windowRect_[which], windowPixels_, uv);
                    windowHit = static_cast<int>(which);
                }
                if (windowBarQuad(which, quad, size) && meet(quad, size, distance, uv) && distance < nearest) {
                    nearest = distance;
                    onPanel = false;
                    barHit = static_cast<int>(which);
                    windowHit = barHit;
                    barDistance = distance;
                    in.beamLength = distance;
                }
            }
        }
    }
    // A window's bar comes with the ray on the window, and stays a moment
    // after it leaves: long enough to cross the gap down to the bar.
    const auto now = std::chrono::steady_clock::now();
    if (windowHit >= 0) {
        pointedWindow_ = windowHit;
        windowPointedAt_ = now;
    } else if (now - windowPointedAt_ > std::chrono::milliseconds(600)) {
        pointedWindow_ = -1;
    }

    // A window being moved has the ray, and the trigger with it.
    const auto leftTrigger = in.buttons[static_cast<size_t>(ControlButton::LeftTrigger)];
    const bool trigger = button(session_, leftHanded_ ? leftTrigger : in.click);
    bool moving = false;
    {
        glm::vec3 from;
        glm::vec3 along;
        const bool ray = focused && inWorld && aimRay(localSpace_, frameTime(), from, along);
        moving = dragWindow(from, along, ray && trigger && !in.mouseHeld[0] && !menuTaken, ray ? barHit : -1,
                            barDistance);
        if (moving) {
            onPanel = false;
            in.beamLength = dragDistance_;
        }
    }
    if (!onPanel && barHit < 0 && !moving && focused && panelQuad(space, quad, size) &&
        aimRay(space, frameTime(), origin, direction)) {
        float distance = 0.0f;
        glm::vec2 uv(0.0f);
        const glm::vec2 pixels(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
        if (meet(quad, size, distance, uv) && windowWidth > 0 && windowHeight > 0 &&
            !liftedWindowAt(uv * (pixels - 1.0f), pixels)) {
            onPanel = true;
            if (overUi || !inWorld) in.beamLength = distance;
            at = uv * (pixels - 1.0f);
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
    // The trigger and the grip of the hand that points; not the trigger while
    // it holds a window by its bar.
    // While the other hand has the menu, its trigger and grip instead.
    const auto leftGrip = in.buttons[static_cast<size_t>(ControlButton::LeftGrip)];
    // Before it is the right button, that hand's grip squeezed over the
    // shoulder or at the far hip draws the weapon or puts it away, and
    // squeezed on an action bar button puts that button in the glowing frame.
    // Either keeps the grip until it is let go.
    const bool pointGrip = button(session_, leftHanded_ ? leftGrip : in.grab);
    if (pointGrip && !in.gripWasDown && !menuTaken && !in.mouseHeld[1]) {
        if (drawByReach_ && inWorld && handAtSheath()) {
            sheathToggle_ = true;
            in.gripTaken = true;
        } else if (onPanel) {
            for (int i = 0; i < static_cast<int>(actionButtonRects_.size()); ++i) {
                const glm::vec4& r = actionButtonRects_[static_cast<size_t>(i)];
                if (r.z > 0.0f && at.x >= r.x && at.x < r.x + r.z && at.y >= r.y && at.y < r.y + r.w) {
                    loadedSlot_ = i;
                    in.gripTaken = true;
                    break;
                }
            }
        }
    }
    if (!pointGrip) in.gripTaken = false;
    in.gripWasDown = pointGrip;
    const bool wanted[2] = {
        menuTaken ? (in.menuRay && offTrigger) || in.fingerDown : trigger && !moving && (barHit < 0 || in.mouseHeld[0]),
        menuTaken ? in.menuRay && offGrip : pointGrip && !in.gripTaken};
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

    // A spell waiting for a place on the ground is aimed by the ray wherever
    // it points, not only while a button is held beside the panel.
    game::GameHandler* game = core::Application::getInstance().getGameHandler();
    const bool aimingGround = tracked && game && game->isAwaitingGroundTarget();
    in.inCharge = inWorld && (onPanel || in.worldPress || aimingGround);
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
    turningSmoothly_ = false;
    if (live) camera->setSteering(true);

    // The head's turn, handed to the game as a turn of its camera - which is
    // the turn of the character - and counted, so that the view takes the same
    // angle off again and nothing is seen to move. Counted by what the camera
    // did, not by what it was asked: it may refuse. A degree of slack keeps a
    // head that is merely alive from being reported to the server all day.
    // Only from the character's own eyes: seen from behind, the camera would
    // swing round the character with every glance.
    //
    // With the walking direction taken from the left hand, the character faces
    // the way that controller points for as long as the stick is pushed - the
    // keys the stick holds walk the way the character faces - and goes back to
    // facing with the head, if it does that, when the stick is let go.
    float facing = headYaw_;
    bool follows = bodyFollowsHead_;
    if (moveByHand_ && input_ && input_->attached &&
        (input_->held[0] || input_->held[1] || input_->held[2] || input_->held[3])) {
        float handYaw = 0.0f;
        if (offHandYaw(handYaw)) {
            facing = handYaw;
            follows = true;
        }
    }
    if (live && follows && firstPerson_) {
        const float turned = wrapRadians(wrapRadians(facing - headOriginYaw_) - bodyYaw_);
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
    // Left-handed, the sticks change places with everything else.
    const glm::vec2 move = stick(session_, leftHanded_ ? in.turn : in.move);
    // Q and E sidestep, as the gamepad's stick does; A and D would turn.
    const float pushes[4] = {move.y, -move.y, -move.x, move.x};
    for (size_t i = 0; i < std::size(kMoveKeys); ++i) {
        const bool down = pushed(pushes[i], in.held[i]);
        if (down != in.held[i]) input.setVirtualKey(kMoveKeys[i], down);
        in.held[i] = down;
    }

    // Not while a line is being typed: a key held then is a letter in it.
    const bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;

    // The buttons. What one does is settled as it goes down - with the
    // modifier or without - and kept until it comes up.
    bool down[kControlButtonCount];
    bool modifier = false;
    for (size_t i = 0; i < kControlButtonCount; ++i) {
        down[i] = !typing && button(session_, buttonSource(in.buttons, in.click, in.grab, i, leftHanded_));
        // The other hand's trigger and grip are the mouse's while its ray has
        // the menu on the wrist.
        if (in.menuRay && (i == static_cast<size_t>(ControlButton::LeftTrigger) ||
                           i == static_cast<size_t>(ControlButton::LeftGrip))) {
            down[i] = false;
        }
        if (down[i] && controls_.plain[i] == ControlFunction::Modifier) modifier = true;
    }
    bool keyWanted[kControlFunctionCount] = {};
    bool escape = false;
    for (size_t i = 0; i < kControlButtonCount; ++i) {
        if (down[i] && !in.buttonDown[i]) {
            const bool second = modifier && controls_.shifted[i] != ControlFunction::None;
            in.latched[i] = second ? controls_.shifted[i] : controls_.plain[i];
            if (in.latched[i] == ControlFunction::Recenter) recenter();
            if (in.latched[i] == ControlFunction::Interact) interactWithTarget();
        } else if (!down[i]) {
            in.latched[i] = ControlFunction::None;
        }
        in.buttonDown[i] = down[i];
        keyWanted[static_cast<size_t>(in.latched[i])] = true;
        if (in.latched[i] == ControlFunction::Escape) escape = true;
    }
    // By function, so that two buttons given the same one hold one key
    // between them and neither lets go of it for the other.
    for (size_t f = 0; f < kControlFunctionCount; ++f) {
        const SDL_Scancode key = functionKey(static_cast<ControlFunction>(f));
        if (key == SDL_SCANCODE_UNKNOWN || keyWanted[f] == in.keyHeld[f]) continue;
        input.setVirtualKey(key, keyWanted[f]);
        in.keyHeld[f] = keyWanted[f];
    }
    // The loaded action: the key of the button in the glowing frame. As its
    // button goes down the other hand aims, if asked - what it points at is
    // made the target - and the key follows a frame later, so that the cast
    // goes at that target. Let go while a cast it began is still going, the
    // cast is cancelled: held to the end, the spell goes.
    {
        const bool loaded = keyWanted[static_cast<size_t>(ControlFunction::LoadedAction)];
        game::GameHandler* game = core::Application::getInstance().getGameHandler();
        if (loaded && !in.loadedWanted) {
            in.loadedKey = kActionKeys[std::clamp(loadedSlot_, 0, 11)];
            in.castingAtPress = game && game->isCasting();
            glm::vec3 origin;
            glm::vec3 direction;
            if (aimWithHand_ && game && offHandWorldRay(origin, direction)) {
                rendering::Ray ray{};
                ray.origin = origin;
                ray.direction = direction;
                const uint64_t aimed = ui::pickScene(*game, ray, ui::ScenePickParams{}).resolve();
                if (aimed != 0 && aimed != game->getTargetGuid()) game->setTarget(aimed);
            }
        } else if (!loaded && in.loadedWanted && game && !in.castingAtPress && game->isCasting()) {
            game->cancelCast();
        }
        const bool press = loaded && in.loadedWanted;
        if (press != in.loadedKeyDown) {
            input.setVirtualKey(in.loadedKey, press);
            in.loadedKeyDown = press;
        }
        in.loadedWanted = loaded;
    }

    // Escape is the interface's: it is asked of ImGui, which a held scancode
    // never reaches.
    if (escape != in.escapeDown && ImGui::GetCurrentContext()) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, escape);
        in.escapeDown = escape;
    }

    // The turn is the mouse's: so many pixels of it, which the camera scales
    // by its own sensitivity - divided out here so that the angle is an angle.
    // Not while a line is being typed, as the keys are not.
    const float turn = stick(session_, leftHanded_ ? in.move : in.turn).x;
    const float perDegree = 1.0f / std::max(camera->getMouseSensitivity(), 0.0001f);
    turningSmoothly_ = false;
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
            turningSmoothly_ = true;  // which closes the vignette
        }
    }
}

}  // namespace wowee::xr
