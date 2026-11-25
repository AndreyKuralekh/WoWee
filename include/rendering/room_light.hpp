#pragma once

#include <glm/glm.hpp>
#include <functional>

namespace wowee::rendering {

/// The light of a building's room at a point: what a doodad placed there by
/// the building is lit with (pipeline::doodadRoomLights), for what the server
/// puts there instead - a sittable chair, a chest, a person. Lit by the sky,
/// those drew dark and blue in a lit room.
struct RoomLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 direct{0.0f};
    glm::vec3 toLight{0.0f, 0.0f, 1.0f};  ///< unit, world
};

/// WMORenderer::roomLightAt, handed to the renderers that light by it. False
/// outside any room the sky does not reach.
using RoomLightQuery = std::function<bool(const glm::vec3& worldPos, RoomLight& out)>;

}  // namespace wowee::rendering
