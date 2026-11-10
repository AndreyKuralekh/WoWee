// Where ground clutter stands, from the ADT's per-quad data: the quad's
// layer (doodadMapping), the quads nothing grows on (noEffectDoodad), and the
// layer's GroundEffectTexture row.
//
// Headless: the chunks are built by hand.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <map>
#include <vector>

#include "pipeline/ground_clutter.hpp"

using namespace wowee::pipeline;

namespace {

constexpr uint32_t kGrassEffect = 11;
constexpr uint32_t kBareEffect = 12;     // in the table, density 0
constexpr uint32_t kFlowerA = 101;
constexpr uint32_t kFlowerB = 102;

const GroundEffectInfo* effectFor(uint32_t id) {
    static const GroundEffectInfo grass{{kFlowerA, kFlowerB, 0, 0}, {3, 1, 0, 0}, 8};
    static const GroundEffectInfo bare{{kFlowerA, 0, 0, 0}, {1, 0, 0, 0}, 0};
    if (id == kGrassEffect) return &grass;
    if (id == kBareEffect) return &bare;
    return nullptr;
}

TextureLayer layer(uint32_t effectId) {
    TextureLayer l{};
    l.effectId = effectId;
    return l;
}

MapChunk flatChunk() {
    MapChunk chunk;
    chunk.heightMap.heights.fill(0.0f);
    chunk.heightMap.loaded = true;
    chunk.normals.fill(0);
    return chunk;
}

void mapAllQuadsTo(MapChunk& chunk, uint32_t layerIdx) {
    const uint8_t b = static_cast<uint8_t>(layerIdx | (layerIdx << 2) | (layerIdx << 4) | (layerIdx << 6));
    chunk.doodadMapping.fill(b);
}

void mapQuad(MapChunk& chunk, int qy, int qx, uint32_t layerIdx) {
    const int quad = qy * 8 + qx;
    auto& byte = chunk.doodadMapping[quad / 4];
    const int shift = (quad % 4) * 2;
    byte = static_cast<uint8_t>((byte & ~(3 << shift)) | (layerIdx << shift));
}

GroundClutterParams params(float ged = 64.0f) {
    GroundClutterParams p;
    p.groundEffectDensity = ged;
    p.calibration = 1.0f;  // the bare formula
    return p;
}

const std::array<bool, 4> kNoRoads{};

}  // namespace

TEST_CASE("the count per quad follows density times groundEffectDensity over 64", "[clutter]") {
    REQUIRE(clutterPerQuad(8, params(64.0f)) == Catch::Approx(8.0f));
    REQUIRE(clutterPerQuad(8, params(256.0f)) == Catch::Approx(32.0f));
    REQUIRE(clutterPerQuad(1, params(16.0f)) == Catch::Approx(0.25f));
    // Past the cap a density only clumps; the count stops growing.
    REQUIRE(clutterPerQuad(99, params(64.0f)) == Catch::Approx(24.0f));
}

TEST_CASE("every quad of a grassy chunk grows the formula's count", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    const size_t n = placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(64.0f), out, &quads);
    REQUIRE(n == 64 * 8);
    REQUIRE(out.size() == n);
    for (const auto& q : quads) {
        REQUIRE(q.state == ClutterQuadState::Grown);
        REQUIRE(q.placed == 8);
    }
}

TEST_CASE("a quad marked noEffectDoodad grows nothing", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    chunk.noEffectDoodad = 1ull << (3 * 8 + 2);  // quad y=3, x=2
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(), out, &quads);
    REQUIRE(quads[3 * 8 + 2].state == ClutterQuadState::Disabled);
    REQUIRE(quads[3 * 8 + 2].placed == 0);
    REQUIRE(quads[3 * 8 + 3].state == ClutterQuadState::Grown);
    REQUIRE(out.size() == 63 * 8);
}

TEST_CASE("a quad grows what its mapped layer grows, not another layer's", "[clutter]") {
    // Layer 0 is bare ground, layer 1 is grass. Only the quads doodadMapping
    // points at layer 1 grow, whatever the alpha of the grass would say.
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kBareEffect));
    chunk.layers.push_back(layer(kGrassEffect));
    mapAllQuadsTo(chunk, 0);
    mapQuad(chunk, 5, 6, 1);
    mapQuad(chunk, 0, 0, 1);
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(), out, &quads);
    REQUIRE(quads[5 * 8 + 6].state == ClutterQuadState::Grown);
    REQUIRE(quads[0].state == ClutterQuadState::Grown);
    REQUIRE(quads[1].state == ClutterQuadState::Empty);
    REQUIRE(out.size() == 2 * 8);
}

TEST_CASE("a layer the chunk does not have, or an unknown effect, grows nothing", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(999));
    mapAllQuadsTo(chunk, 0);
    mapQuad(chunk, 1, 1, 3);
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    REQUIRE(placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(), out, &quads) == 0);
    REQUIRE(quads[0].state == ClutterQuadState::NoEffect);
    REQUIRE(quads[1 * 8 + 1].state == ClutterQuadState::NoLayer);
}

TEST_CASE("a made surface grows nothing even with a grassy effect", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    REQUIRE(placeChunkClutter(chunk, {}, effectFor, {true, false, false, false}, params(), out, &quads) == 0);
    REQUIRE(quads[0].state == ClutterQuadState::Road);
}

TEST_CASE("a terrain hole grows nothing", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    chunk.holes = 1;  // the 2x2 quads at the corner
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(), out, &quads);
    REQUIRE(quads[0].state == ClutterQuadState::Hole);
    REQUIRE(quads[1 * 8 + 1].state == ClutterQuadState::Hole);
    REQUIRE(out.size() == 60 * 8);
}

TEST_CASE("the same chunk always grows the same clutter, another chunk other clutter", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    const ClutterChunkKey key{0, 32, 48, 5, 7};
    std::vector<GroundClutterSample> a, b, c;
    placeChunkClutter(chunk, key, effectFor, kNoRoads, params(), a);
    placeChunkClutter(chunk, key, effectFor, kNoRoads, params(), b);
    ClutterChunkKey other = key;
    other.chunkX = 6;
    placeChunkClutter(chunk, other, effectFor, kNoRoads, params(), c);
    REQUIRE(a.size() == b.size());
    bool allSame = true, anyDiffer = false;
    for (size_t i = 0; i < a.size(); ++i) {
        allSame = allSame && a[i].x == b[i].x && a[i].y == b[i].y && a[i].doodadId == b[i].doodadId &&
                  a[i].yaw == b[i].yaw && a[i].scale == b[i].scale;
        anyDiffer = anyDiffer || a[i].x != c[i].x || a[i].y != c[i].y;
    }
    REQUIRE(allSame);
    REQUIRE(anyDiffer);
}

TEST_CASE("a fractional count holds on average", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    // density 8 at groundEffectDensity 2 is 0.25 a quad.
    size_t total = 0;
    for (int cx = 0; cx < 16; ++cx) {
        std::vector<GroundClutterSample> out;
        total += placeChunkClutter(chunk, {0, 1, 1, cx, 0}, effectFor, kNoRoads, params(2.0f), out);
    }
    const double perQuad = static_cast<double>(total) / (16 * 64);
    REQUIRE(perQuad == Catch::Approx(0.25).margin(0.04));
}

TEST_CASE("doodads are picked by their weights", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    std::vector<GroundClutterSample> out;
    placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(256.0f), out);
    std::map<uint32_t, size_t> counts;
    for (const auto& s : out) counts[s.doodadId]++;
    REQUIRE(counts.size() == 2);
    const double shareA = static_cast<double>(counts[kFlowerA]) / static_cast<double>(out.size());
    REQUIRE(shareA == Catch::Approx(0.75).margin(0.04));
}

TEST_CASE("the quad map shows what each quad did", "[clutter]") {
    MapChunk chunk = flatChunk();
    chunk.layers.push_back(layer(kGrassEffect));
    chunk.noEffectDoodad = 1ull;  // quad 0,0
    std::vector<GroundClutterSample> out;
    std::array<ClutterQuadInfo, 64> quads;
    placeChunkClutter(chunk, {}, effectFor, kNoRoads, params(64.0f), out, &quads);
    const std::string map = clutterQuadMap(quads);
    REQUIRE(map.substr(0, 8) == "x8888888");
}
