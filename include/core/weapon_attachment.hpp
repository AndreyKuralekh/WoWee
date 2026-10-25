#pragma once

#include "game/inventory.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>

namespace wowee {
namespace core {

// Where a held or put-away weapon hangs, for the local character and for NPCs
// alike: an NPC's sheath state comes from UNIT_FIELD_BYTES_2, the player's
// from the Z toggle.

constexpr uint32_t kAttachShield = 0;
// M2 attachment 11 is the helm; 0 is the shield mount, which is where head
// gear was going - attached successfully, on the forearm, invisible on the head.
constexpr uint32_t kAttachRightHand = 1;
constexpr uint32_t kAttachLeftHand = 2;
constexpr uint32_t kAttachBack = 12;
// The model's own sheath points, which carry the sheathed orientation. Its
// plain hip points do not, and some models name a second hip point at the
// feet.
constexpr uint32_t kAttachSheathShield = 28;
constexpr uint32_t kAttachHipWeaponLeft = 32;
constexpr uint32_t kAttachHipWeaponRight = 33;

inline uint32_t weaponAttachment(bool sheathed, game::EquipSlot slot, uint8_t inventoryType) {
    if (!sheathed) {
        if (inventoryType == game::InvType::SHIELD) return kAttachShield;
        return slot == game::EquipSlot::OFF_HAND ? kAttachLeftHand : kAttachRightHand;
    }

    if (inventoryType == game::InvType::TWO_HAND) return kAttachBack;
    if (inventoryType == game::InvType::SHIELD) return kAttachSheathShield;
    if (inventoryType == game::InvType::ONE_HAND ||
        inventoryType == game::InvType::MAIN_HAND) {
        // Main hand on the left hip, drawn across the body.
        return slot == game::EquipSlot::OFF_HAND ? kAttachHipWeaponRight : kAttachHipWeaponLeft;
    }

    // Holdables and other items with no sheath position are hidden, matching
    // the original client rather than pinning books/orbs to an arbitrary bone.
    return UINT32_MAX;
}

inline glm::mat4 weaponLocalTransform(bool sheathed, game::EquipSlot /*slot*/,
                                      uint8_t inventoryType) {
    glm::mat4 transform(1.0f);
    if (!sheathed || inventoryType == game::InvType::SHIELD) return transform;

    if (inventoryType == game::InvType::TWO_HAND) {
        // Weapon models are authored for a hand with their long axis pointing
        // forward. Stand that axis up, cant it across the back, and move it off
        // the spine so the grip sits below the opposite shoulder.
        // Weapon models are authored along local X, which is also the character's
        // front/back axis. First rotate weapon X completely onto character Z, then
        // cant that vertical axis within the Y/Z back plane. This ordering is
        // important: rotating around X cannot change an X-aligned blade.
        // The final innermost roll spins the blade about its own long axis so
        // the flat rests against the back instead of the sharp edge.
        // Values tuned against the live attachment frame. Its Z axis moves the
        // weapon laterally rather than vertically.
        transform = glm::translate(transform, glm::vec3(-0.03f, -0.10f, 0.0f));
        transform = glm::rotate(transform, glm::radians(33.0f), glm::vec3(1, 0, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(0, 1, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(1, 0, 0));
    }
    return transform;
}

} // namespace core
} // namespace wowee
