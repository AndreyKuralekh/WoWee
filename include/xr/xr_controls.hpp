#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::xr {

/// The controller buttons that can be given something to do. The right
/// trigger and grip are the pointer's mouse buttons and the sticks walk and
/// turn; those are not offered.
enum class ControlButton : uint8_t {
    LeftTrigger,
    LeftGrip,
    A,
    B,
    X,
    Y,
    LeftStick,   // pressed in
    RightStick,  // pressed in
    Menu,
    Count
};

/// What a button can do. Everything but the last few is a key the client
/// already polls, held for as long as the button is.
enum class ControlFunction : uint8_t {
    None,
    Modifier,  // held, gives the other buttons their second function
    Jump,
    Action1,
    Action2,
    Action3,
    Action4,
    Action5,
    Action6,
    Action7,
    Action8,
    Action9,
    Action10,
    Action11,
    Action12,
    TargetEnemy,
    Interact,  // what a right click on the current target does
    Autorun,
    Escape,
    Recenter,
    LoadedAction,  // the action bar button in the glowing frame; see XrSystem::loadedSlot
    SitStand,      // the X key: sit down, or stand up
    // The loaded action, its target first picked by the ray of the button's
    // hand as the button goes down; pointing at nothing keeps the target.
    AimedLoadedAction,
    // As the button goes down, what its hand's ray points at becomes the
    // target; while held, it is the modifier as well.
    LaserTarget,
    Count
};

/// The layouts the VR buttons page offers whole. Custom is any table that is
/// none of them.
enum class ControlLayout : uint8_t {
    TriggerCast,  // the left trigger aims and casts the framed spell, held
    GripCast,     // the left trigger aims (and is the modifier), the left grip casts
    Classic,      // as before task 4: the trigger casts at the target there is
    Custom,
};

/// Whether a function makes the other buttons do their second function
/// while its button is held.
[[nodiscard]] constexpr bool isModifierFunction(ControlFunction function) {
    return function == ControlFunction::Modifier || function == ControlFunction::LaserTarget;
}

constexpr std::size_t kControlButtonCount = static_cast<std::size_t>(ControlButton::Count);
constexpr std::size_t kControlFunctionCount = static_cast<std::size_t>(ControlFunction::Count);

/// What each button does, alone and with the modifier held. A button with
/// nothing for the modifier keeps doing what it does alone.
struct ControlBindings {
    std::array<ControlFunction, kControlButtonCount> plain{};
    std::array<ControlFunction, kControlButtonCount> shifted{};

    [[nodiscard]] static ControlBindings defaults();
    /// One of the layouts; Custom gives the defaults.
    [[nodiscard]] static ControlBindings layout(ControlLayout which);
    /// Which layout this table is, or Custom.
    [[nodiscard]] ControlLayout whichLayout() const;

    /// Reads what the file says over what is here; a button it does not
    /// mention is left as it was. False when there is no file.
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    bool operator==(const ControlBindings&) const = default;
};

[[nodiscard]] const char* controlButtonName(ControlButton button);
[[nodiscard]] const char* controlFunctionName(ControlFunction function);

/// vr_controls.cfg, beside the client's other settings.
[[nodiscard]] std::string controlsConfigPath();

/// The settings page reaches a binding as a setting: "vrbtn<button>" for
/// what a button does alone and "vrmod<button>" for what it does with the
/// modifier held, valued by the function's number - and "vrlayout", the
/// whole table as one of the layouts, valued by ControlLayout; choosing
/// Custom changes nothing. These say whether a key
/// is one of those, and read and write the table through it. Each has a row
/// in settings_schema.cpp, whose choices are the functions in enum order.
[[nodiscard]] bool isControlSettingKey(const std::string& key);
[[nodiscard]] std::string controlSettingValue(const ControlBindings& bindings, const std::string& key);
/// False for a key that is not a binding; `changed` says whether anything is
/// different afterwards.
bool setControlSettingValue(ControlBindings& bindings, const std::string& key, int value, bool& changed);

}  // namespace wowee::xr
