#pragma once

#include <glm/vec3.hpp>

namespace wowee {
namespace audio {

// A fire to be heard (from the renderer's fire registry): where its flames
// are, and what kind - 0 a small fire or hearth, 1 a large one, 2 a torch.
struct FireSoundSource {
    glm::vec3 position{0.0f};
    int kind = 0;
};

// Falling water to be heard (TASK_13): 0 a small waterfall, 1 a tall one,
// 2 a fountain.
struct WaterSoundSource {
    glm::vec3 position{0.0f};
    int kind = 0;
};

} // namespace audio
} // namespace wowee
