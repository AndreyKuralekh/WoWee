// The spell book and the bags, stood on their own in front of the player.
//
// Like the panels on the wrist they are parts of the panel's own picture, cut
// out of it with a layer's subImage rectangle: nothing is drawn for them. Each
// opens where the head is looking, a little to one side and low, nearer than
// the big panel and larger for it, and turns to face the head. A thin bar
// under each moves it: the ray on the bar and the trigger held, and it goes
// where the ray goes. The grips cannot be used for that - the pointing one is
// the right mouse button, which uses an item in a bag, and the other one is
// the modifier. The bar shows only while the ray is on its window. Where each
// was put is kept in vr_windows.cfg.

#include "xr/xr_system.hpp"

#include "core/config_paths.hpp"
#include "core/logger.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace wowee::xr {

namespace {

constexpr float kWindowHeightMetres = 0.6f;  // the whole window's height, at size 1
constexpr float kBarHeight = 0.012f;
constexpr float kBarGap = 0.01f;
constexpr float kTooltipLift = 0.003f;  // in front of its window, where the two meet
constexpr const char* kWindowNames[2] = {"spellbook", "bags"};

std::string windowsConfigPath() {
    return (std::filesystem::path(core::getConfigRoot()) / "vr_windows.cfg").string();
}

// Where each stands at first: 0.7 m away, 0.2 m below the eyes, the book to
// the left and the bags to the right, as the stock interface has them.
glm::vec3 defaultOffset(uint32_t which) {
    constexpr float kDistance = 0.7f;
    const float aside = glm::radians(28.0f);
    const float x = std::sin(aside) * kDistance;
    return {which == 0 ? -x : x, -0.2f, -std::cos(aside) * kDistance};
}

float wrap(float radians) {
    return std::remainder(radians, 2.0f * glm::pi<float>());
}

glm::quat yawTurn(float yaw) {
    return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

}  // namespace

void XrSystem::setWindowRects(const glm::vec4& spellBook, const glm::vec4& bags, float windowWidth,
                              float windowHeight) {
    // Said when one opens or closes, and every ten seconds besides.
    const bool changed = (windowRect_[0].z > 1.0f) != (spellBook.z > 1.0f) || (windowRect_[1].z > 1.0f) != (bags.z > 1.0f);
    windowRect_[0] = spellBook;
    windowRect_[1] = bags;
    windowPixels_ = glm::vec2(windowWidth, windowHeight);
    if (debug_) {
        static auto lastSaid = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (changed || now - lastSaid > std::chrono::seconds(10)) {
            lastSaid = now;
            LOG_WARNING("VR: windows from the window ", windowWidth, "x", windowHeight, ": spell book ",
                        spellBook.x, ",", spellBook.y, " ", spellBook.z, "x", spellBook.w, ", bags ", bags.x, ",",
                        bags.y, " ", bags.z, "x", bags.w);
        }
    }
}

void XrSystem::placeWindows(const glm::vec3& head, float headYaw) {
    if (!windowPlacesLoaded_) loadWindowPlaces();
    windowHead_ = head;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - windowsMoved_).count());
    windowsMoved_ = now;
    const float k = 1.0f - std::exp(-dt * 6.0f);
    // Far enough round that both can be looked at without them moving.
    constexpr float kFollowAngle = glm::radians(70.0f);
    constexpr float kFollowDistance = 0.75f;
    for (uint32_t i = 0; i < 2; ++i) {
        WindowPlace& p = windowPlace_[i];
        const bool shown = windows_ && windowRect_[i].z > 1.0f && windowRect_[i].w > 1.0f;
        if (!shown) {
            p.open = false;
            if (draggedWindow_ == static_cast<int>(i)) draggedWindow_ = -1;
            continue;
        }
        if (!p.open) {
            p.open = true;
            p.yaw = p.targetYaw = headYaw;
            p.anchor = p.targetAnchor = head;
            continue;
        }
        if (draggedWindow_ == static_cast<int>(i)) continue;
        if (std::abs(wrap(headYaw - p.targetYaw)) > kFollowAngle || glm::distance(p.targetAnchor, head) > kFollowDistance) {
            p.targetYaw = headYaw;
            p.targetAnchor = head;
        }
        p.yaw = wrap(p.yaw + wrap(p.targetYaw - p.yaw) * k);
        p.anchor += (p.targetAnchor - p.anchor) * k;
    }
}

bool XrSystem::windowQuad(uint32_t which, XrPosef& pose, XrExtent2Df& size, XrRect2Di& crop) const {
    if (which > 1 || !windows_ || !windowPlace_[which].open || windowPixels_.y <= 0.0f) return false;
    const glm::vec4& rect = windowRect_[which];
    if (!panelCrop(rect, windowPixels_, crop)) return false;
    const WindowPlace& p = windowPlace_[which];
    const glm::vec3 centre = p.anchor + yawTurn(p.yaw) * p.offset;
    // Facing the head, upright.
    glm::vec3 normal = windowHead_ - centre;
    if (glm::dot(normal, normal) < 1e-4f) return false;
    normal = glm::normalize(normal);
    glm::vec3 across = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
    if (glm::dot(across, across) < 1e-4f) return false;  // straight above or below the head
    across = glm::normalize(across);
    const glm::vec3 up = glm::cross(normal, across);
    const glm::quat turn = glm::quat_cast(glm::mat3(across, up, normal));
    const float metresPerPixel = kWindowHeightMetres * windowSize_ / windowPixels_.y;
    size = {rect.z * metresPerPixel, rect.w * metresPerPixel};
    pose.orientation = {turn.x, turn.y, turn.z, turn.w};
    pose.position = {centre.x, centre.y, centre.z};
    return true;
}

bool XrSystem::windowBarQuad(uint32_t which, XrPosef& pose, XrExtent2Df& size) const {
    // Only while the ray is on the window or its bar, or moving it: a bar
    // under every open window all the time is a stripe across the view.
    if (pointedWindow_ != static_cast<int>(which) && draggedWindow_ != static_cast<int>(which)) return false;
    XrRect2Di crop{};
    XrExtent2Df window{};
    if (!windowQuad(which, pose, window, crop)) return false;
    const glm::quat turn(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    const glm::vec3 centre = glm::vec3(pose.position.x, pose.position.y, pose.position.z) -
                             turn * glm::vec3(0.0f, 0.5f * window.height + kBarGap + 0.5f * kBarHeight, 0.0f);
    pose.position = {centre.x, centre.y, centre.z};
    size = {std::max(0.08f, window.width * 0.4f), kBarHeight};
    return true;
}

int XrSystem::tooltipHost() const {
    const glm::vec4& t = tooltipRect_;
    if (!windows_ || t.z <= 1.0f || t.w <= 1.0f) return -1;
    int host = -1;
    float most = 0.0f;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!windowPlace_[i].open) continue;
        const glm::vec4& r = windowRect_[i];
        const float w = std::min(t.x + t.z, r.x + r.z) - std::max(t.x, r.x);
        const float h = std::min(t.y + t.w, r.y + r.w) - std::max(t.y, r.y);
        if (w <= 0.0f || h <= 0.0f) continue;
        if (pointedWindow_ == static_cast<int>(i)) return pointedWindow_;
        if (w * h > most) {
            most = w * h;
            host = static_cast<int>(i);
        }
    }
    return host;
}

bool XrSystem::tooltipQuad(uint32_t host, XrPosef& pose, XrExtent2Df& size, XrRect2Di& crop) const {
    XrExtent2Df window{};
    XrRect2Di windowCrop{};
    if (!windowQuad(host, pose, window, windowCrop) || !panelCrop(tooltipRect_, windowPixels_, crop)) return false;
    const glm::vec4& w = windowRect_[host];
    const glm::vec4& t = tooltipRect_;
    const float metresPerPixel = kWindowHeightMetres * windowSize_ / windowPixels_.y;
    // Pixels run down the picture, the quad's y up it.
    const glm::vec2 shift = (glm::vec2(t.x + 0.5f * t.z, t.y + 0.5f * t.w) - glm::vec2(w.x + 0.5f * w.z, w.y + 0.5f * w.w)) *
                            metresPerPixel;
    const glm::quat turn(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    const glm::vec3 centre = glm::vec3(pose.position.x, pose.position.y, pose.position.z) +
                             turn * glm::vec3(shift.x, -shift.y, kTooltipLift);
    pose.position = {centre.x, centre.y, centre.z};
    size = {t.z * metresPerPixel, t.w * metresPerPixel};
    return true;
}

bool XrSystem::dragWindow(const glm::vec3& origin, const glm::vec3& direction, bool trigger, int barHit,
                          float barDistance) {
    const bool pressed = trigger && !dragTriggerWas_;
    dragTriggerWas_ = trigger;
    if (draggedWindow_ >= 0 && !trigger) {
        draggedWindow_ = -1;
        saveWindowPlaces();
        return false;
    }
    if (pressed && barHit >= 0 && barHit < 2) {
        XrPosef pose{};
        XrExtent2Df size{};
        XrRect2Di crop{};
        if (!windowQuad(static_cast<uint32_t>(barHit), pose, size, crop)) return false;
        draggedWindow_ = barHit;
        dragDistance_ = barDistance;
        dragFromHit_ = glm::vec3(pose.position.x, pose.position.y, pose.position.z) - (origin + direction * barDistance);
    }
    if (draggedWindow_ < 0) return false;
    WindowPlace& p = windowPlace_[draggedWindow_];
    const glm::vec3 centre = origin + direction * dragDistance_ + dragFromHit_;
    p.offset = glm::inverse(yawTurn(p.yaw)) * (centre - p.anchor);
    return true;
}

void XrSystem::loadWindowPlaces() {
    windowPlacesLoaded_ = true;
    for (uint32_t i = 0; i < 2; ++i) windowPlace_[i].offset = defaultOffset(i);
    std::ifstream file(windowsConfigPath());
    std::string name;
    glm::vec3 offset;
    while (file >> name >> offset.x >> offset.y >> offset.z) {
        for (uint32_t i = 0; i < 2; ++i) {
            // Within arm's reach and a little more; anything else is not a place.
            if (name == kWindowNames[i] && glm::length(offset) < 3.0f) windowPlace_[i].offset = offset;
        }
    }
}

void XrSystem::saveWindowPlaces() const {
    std::ofstream file(windowsConfigPath(), std::ios::trunc);
    if (!file) {
        LOG_WARNING("VR: could not save the windows' places to ", windowsConfigPath());
        return;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const glm::vec3& o = windowPlace_[i].offset;
        file << kWindowNames[i] << ' ' << o.x << ' ' << o.y << ' ' << o.z << '\n';
    }
}

void XrSystem::resetWindowPlaces() {
    for (uint32_t i = 0; i < 2; ++i) windowPlace_[i].offset = defaultOffset(i);
    windowPlacesLoaded_ = true;
    draggedWindow_ = -1;
    std::error_code ignored;
    std::filesystem::remove(windowsConfigPath(), ignored);
}

}  // namespace wowee::xr
