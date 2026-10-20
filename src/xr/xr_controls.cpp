// What the controllers' buttons do: the table, where it is kept, and how
// the settings page reaches it. Reading the buttons and acting on
// them is xr_input.cpp.

#include "xr/xr_controls.hpp"

#include "core/config_paths.hpp"
#include "core/logger.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace wowee::xr {

namespace {

// The second name is what the file says, and must not change once written.
struct ButtonName {
    const char* shown;
    const char* id;
};
constexpr ButtonName kButtons[kControlButtonCount] = {
    {"Left trigger", "lefttrigger"},
    {"Left grip", "leftgrip"},
    {"A", "a"},
    {"B", "b"},
    {"X", "x"},
    {"Y", "y"},
    {"Left stick press", "leftstick"},
    {"Right stick press", "rightstick"},
    {"Menu", "menu"},
};

constexpr ButtonName kFunctions[kControlFunctionCount] = {
    {"Nothing", "none"},
    {"Modifier (hold)", "modifier"},
    {"Jump", "jump"},
    {"Action button 1", "action1"},
    {"Action button 2", "action2"},
    {"Action button 3", "action3"},
    {"Action button 4", "action4"},
    {"Action button 5", "action5"},
    {"Action button 6", "action6"},
    {"Action button 7", "action7"},
    {"Action button 8", "action8"},
    {"Action button 9", "action9"},
    {"Action button 10", "action10"},
    {"Action button 11", "action11"},
    {"Action button 12", "action12"},
    {"Target nearest enemy", "targetenemy"},
    {"Interact with target", "interact"},
    {"Autorun", "autorun"},
    {"Escape / game menu", "escape"},
    {"Recenter view", "recenter"},
    {"Spell in the glowing frame", "loaded"},
};

constexpr std::string_view kShiftedSuffix = ".modifier";

bool functionFromId(std::string_view id, ControlFunction& out) {
    for (std::size_t i = 0; i < kControlFunctionCount; ++i) {
        if (id == kFunctions[i].id) {
            out = static_cast<ControlFunction>(i);
            return true;
        }
    }
    return false;
}

// "vrbtnlefttrigger" -> the left trigger, alone; "vrmod..." with the modifier.
bool parseSettingKey(std::string_view key, std::size_t& button, bool& shiftedOut) {
    constexpr std::string_view kPlain = "vrbtn";
    constexpr std::string_view kShifted = "vrmod";
    if (key.substr(0, kPlain.size()) == kPlain) {
        shiftedOut = false;
    } else if (key.substr(0, kShifted.size()) == kShifted) {
        shiftedOut = true;
    } else {
        return false;
    }
    key.remove_prefix(kPlain.size());
    for (std::size_t i = 0; i < kControlButtonCount; ++i) {
        if (key == kButtons[i].id) {
            button = i;
            return true;
        }
    }
    return false;
}

}  // namespace

ControlBindings ControlBindings::defaults() {
    ControlBindings b;
    const auto set = [&b](ControlButton button, ControlFunction plain, ControlFunction shifted) {
        b.plain[static_cast<std::size_t>(button)] = plain;
        b.shifted[static_cast<std::size_t>(button)] = shifted;
    };
    // The right hand aims, the left one fires: the left trigger fires the
    // button in the glowing frame, which the right grip moves on the bar.
    // Seven of the bar's twelve are reached without the pointer.
    set(ControlButton::LeftTrigger, ControlFunction::LoadedAction, ControlFunction::Action7);
    set(ControlButton::LeftGrip, ControlFunction::Modifier, ControlFunction::None);
    set(ControlButton::A, ControlFunction::Jump, ControlFunction::Action3);
    set(ControlButton::B, ControlFunction::Action2, ControlFunction::Action4);
    set(ControlButton::X, ControlFunction::TargetEnemy, ControlFunction::Action5);
    set(ControlButton::Y, ControlFunction::Interact, ControlFunction::Action6);
    set(ControlButton::LeftStick, ControlFunction::Autorun, ControlFunction::None);
    set(ControlButton::RightStick, ControlFunction::Recenter, ControlFunction::None);
    set(ControlButton::Menu, ControlFunction::Escape, ControlFunction::None);
    return b;
}

bool ControlBindings::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string_view key(line.data(), eq);
        const std::string_view value(line.data() + eq + 1, line.size() - eq - 1);
        const bool isShifted = key.size() > kShiftedSuffix.size() &&
                               key.substr(key.size() - kShiftedSuffix.size()) == kShiftedSuffix;
        if (isShifted) key.remove_suffix(kShiftedSuffix.size());
        ControlFunction function = ControlFunction::None;
        if (!functionFromId(value, function)) continue;
        if (isShifted && function == ControlFunction::Modifier) continue;
        for (std::size_t i = 0; i < kControlButtonCount; ++i) {
            if (key == kButtons[i].id) (isShifted ? shifted : plain)[i] = function;
        }
    }
    return true;
}

bool ControlBindings::save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        LOG_WARNING("VR: could not write ", path);
        return false;
    }
    file << "# What the VR controllers' buttons do; edited on the VR settings tab.\n";
    for (std::size_t i = 0; i < kControlButtonCount; ++i) {
        file << kButtons[i].id << '=' << kFunctions[static_cast<std::size_t>(plain[i])].id << '\n';
        file << kButtons[i].id << kShiftedSuffix << '='
             << kFunctions[static_cast<std::size_t>(shifted[i])].id << '\n';
    }
    return static_cast<bool>(file);
}

const char* controlButtonName(ControlButton button) {
    const auto i = static_cast<std::size_t>(button);
    return i < kControlButtonCount ? kButtons[i].shown : "?";
}

const char* controlFunctionName(ControlFunction function) {
    const auto i = static_cast<std::size_t>(function);
    return i < kControlFunctionCount ? kFunctions[i].shown : "?";
}

std::string controlsConfigPath() {
    return (std::filesystem::path(core::getConfigRoot()) / "vr_controls.cfg").string();
}

bool isControlSettingKey(const std::string& key) {
    std::size_t button = 0;
    bool isShifted = false;
    return parseSettingKey(key, button, isShifted);
}

std::string controlSettingValue(const ControlBindings& bindings, const std::string& key) {
    std::size_t button = 0;
    bool isShifted = false;
    if (!parseSettingKey(key, button, isShifted)) return {};
    return std::to_string(static_cast<int>((isShifted ? bindings.shifted : bindings.plain)[button]));
}

bool setControlSettingValue(ControlBindings& bindings, const std::string& key, int value, bool& changed) {
    changed = false;
    std::size_t button = 0;
    bool isShifted = false;
    if (!parseSettingKey(key, button, isShifted)) return false;
    if (value < 0 || value >= static_cast<int>(kControlFunctionCount)) return true;
    auto function = static_cast<ControlFunction>(value);
    // A modifier that only exists while the modifier is held is no modifier,
    // and the button that is the modifier has no second function.
    if (isShifted && (function == ControlFunction::Modifier ||
                      bindings.plain[button] == ControlFunction::Modifier)) {
        function = ControlFunction::None;
    }
    ControlFunction& slot = (isShifted ? bindings.shifted : bindings.plain)[button];
    changed = slot != function;
    slot = function;
    if (!isShifted && function == ControlFunction::Modifier &&
        bindings.shifted[button] != ControlFunction::None) {
        bindings.shifted[button] = ControlFunction::None;
        changed = true;
    }
    return true;
}

}  // namespace wowee::xr
