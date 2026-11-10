#include "pipeline/ground_clutter.hpp"

#include <algorithm>
#include <cmath>

#include "pipeline/terrain_mesh.hpp"

namespace wowee {
namespace pipeline {

namespace {

constexpr float kPi = 3.14159265358979f;
// A tile is 533 1/3 yards and 16 chunks; a chunk is 8 quads.
constexpr float kQuadSize = (1600.0f / 3.0f) / 16.0f / 8.0f;

// A small, well-mixed hash: placement must not drift with the standard
// library's choice of generator, or a client built elsewhere grows different
// grass on the same ground.
uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

struct QuadRandom {
    uint32_t state;
    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return mix(state);
    }
    // 0..1
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

uint32_t quadSeed(const ClutterChunkKey& key, int quadX, int quadY) {
    uint32_t h = mix(key.mapId * 0x9e3779b9u);
    h = mix(h ^ static_cast<uint32_t>(key.tileX * 73856093));
    h = mix(h ^ static_cast<uint32_t>(key.tileY * 19349663));
    h = mix(h ^ static_cast<uint32_t>((key.chunkY * 16 + key.chunkX) * 83492791));
    h = mix(h ^ static_cast<uint32_t>(quadY * 8 + quadX + 1));
    return h;
}

}  // namespace

float clutterPerQuad(uint32_t density, const GroundClutterParams& params) {
    const float d = static_cast<float>(std::min(density, params.densityCap));
    const float n = d * params.groundEffectDensity / 64.0f * params.calibration;
    return std::clamp(n, 0.0f, static_cast<float>(params.maxPerQuad));
}

size_t placeChunkClutter(const MapChunk& chunk, const ClutterChunkKey& key,
                         const GroundEffectLookupFn& effectFor,
                         const std::array<bool, 4>& roadLayer,
                         const GroundClutterParams& params,
                         std::vector<GroundClutterSample>& out,
                         std::array<ClutterQuadInfo, 64>* quads) {
    if (quads) quads->fill(ClutterQuadInfo{});
    if (!chunk.hasHeightMap() || chunk.layers.empty() || !effectFor) return 0;

    size_t placed = 0;
    for (int qy = 0; qy < 8; ++qy) {
        for (int qx = 0; qx < 8; ++qx) {
            ClutterQuadInfo info;
            auto record = [&] { if (quads) (*quads)[qy * 8 + qx] = info; };

            if (chunk.isHole(qy, qx)) { info.state = ClutterQuadState::Hole; record(); continue; }
            if (chunk.isEffectDisabled(qy, qx)) { info.state = ClutterQuadState::Disabled; record(); continue; }

            const uint32_t layer = chunk.effectLayerFor(qy, qx);
            info.layer = static_cast<uint8_t>(layer);
            if (layer >= chunk.layers.size()) { info.state = ClutterQuadState::NoLayer; record(); continue; }

            const uint32_t effectId = chunk.layers[layer].effectId;
            info.effectId = effectId;
            const GroundEffectInfo* effect =
                (effectId != 0 && effectId != 0xFFFFFFFFu && effectId != 0xFFFFu) ? effectFor(effectId) : nullptr;
            if (!effect) { info.state = ClutterQuadState::NoEffect; record(); continue; }
            if (roadLayer[layer]) { info.state = ClutterQuadState::Road; record(); continue; }

            uint32_t totalWeight = 0;
            for (int i = 0; i < 4; ++i) {
                if (effect->doodadIds[i] != 0) totalWeight += std::max(effect->weights[i], 1u);
            }
            info.expected = clutterPerQuad(effect->density, params);
            if (totalWeight == 0 || info.expected <= 0.0f) {
                info.state = ClutterQuadState::Empty;
                record();
                continue;
            }

            QuadRandom rng{quadSeed(key, qx, qy)};
            // The fraction rounds by a roll of the quad's own generator, so a
            // density of 0.3 a quad grows on three quads in ten, always the
            // same three.
            const float whole = std::floor(info.expected);
            const uint32_t count =
                static_cast<uint32_t>(whole) + (rng.unit() < info.expected - whole ? 1u : 0u);

            for (uint32_t n = 0; n < count; ++n) {
                const float fracX = static_cast<float>(qx) + rng.unit();
                const float fracY = static_cast<float>(qy) + rng.unit();
                uint32_t roll = rng.next() % totalWeight;
                uint32_t doodadId = 0;
                for (int i = 0; i < 4; ++i) {
                    if (effect->doodadIds[i] == 0) continue;
                    const uint32_t w = std::max(effect->weights[i], 1u);
                    if (roll < w) { doodadId = effect->doodadIds[i]; break; }
                    roll -= w;
                }
                const glm::vec3 p = TerrainMeshGenerator::chunkSurfacePoint(
                    chunk.position, chunk.heightMap, fracX, fracY, kQuadSize);
                GroundClutterSample s;
                s.x = p.x;
                s.y = p.y;
                s.z = p.z;
                s.yaw = rng.unit() * 2.0f * kPi;
                s.scale = 0.85f + rng.unit() * 0.30f;
                s.doodadId = doodadId;
                out.push_back(s);
                ++placed;
            }
            info.placed = static_cast<uint16_t>(std::min<uint32_t>(count, 0xFFFFu));
            info.state = count > 0 ? ClutterQuadState::Grown : ClutterQuadState::Empty;
            record();
        }
    }
    return placed;
}

std::string clutterQuadMap(const std::array<ClutterQuadInfo, 64>& quads) {
    std::string s;
    s.reserve(8 * 9);
    for (int qy = 0; qy < 8; ++qy) {
        for (int qx = 0; qx < 8; ++qx) {
            const auto& q = quads[qy * 8 + qx];
            char c = '.';
            switch (q.state) {
                case ClutterQuadState::Hole: c = 'h'; break;
                case ClutterQuadState::Disabled: c = 'x'; break;
                case ClutterQuadState::NoLayer:
                case ClutterQuadState::NoEffect: c = '.'; break;
                case ClutterQuadState::Road: c = 'r'; break;
                case ClutterQuadState::Empty: c = '-'; break;
                case ClutterQuadState::Grown:
                    c = q.placed > 9 ? '+' : static_cast<char>('0' + q.placed);
                    break;
            }
            s.push_back(c);
        }
        if (qy < 7) s.push_back('\n');
    }
    return s;
}

}  // namespace pipeline
}  // namespace wowee
