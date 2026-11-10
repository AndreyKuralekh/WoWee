#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pipeline/adt_loader.hpp"

// Where the ground clutter of a map chunk stands: the tufts, flowers and
// pebbles the client scatters over the terrain from GroundEffectTexture and
// GroundEffectDoodad.
//
// The ADT says it all, per quad of the chunk's 8x8 grid:
// - noEffectDoodad: nothing grows on this quad (fields, footprints, floors);
// - doodadMapping: which of the chunk's four texture layers the quad takes its
//   ground effect from - one layer per quad, never a blend;
// - that layer's effectId names a GroundEffectTexture row: a density and up to
//   four doodads with weights.
//
// How many doodads a density makes is not documented. The starting point is
// density * groundEffectDensity / 64 per quad (groundEffectDensity being the
// client's own setting, 16..256), times a calibration factor fitted against
// the original client by eye.
//
// Placement is deterministic: the same chunk always grows the same doodads in
// the same places, so tiles that stream out and back in look the same and two
// screenshots of one spot can be compared.

namespace wowee {
namespace pipeline {

/// One GroundEffectTexture row, as far as placement needs it.
struct GroundEffectInfo {
    std::array<uint32_t, 4> doodadIds{};
    std::array<uint32_t, 4> weights{};
    uint32_t density = 0;
};

/// The row for an effect id, or null when the table has none.
using GroundEffectLookupFn = std::function<const GroundEffectInfo*(uint32_t effectId)>;

struct GroundClutterParams {
    /// The client's groundEffectDensity: 16..256, 256 being its maximum.
    float groundEffectDensity = 256.0f;
    /// Fitted against the original client by eye (Northshire, 10.11.2026): the
    /// bare formula at groundEffectDensity 256 is about twice as dense as the
    /// original drawn at 256.
    float calibration = 0.5f;
    /// Above this a density only gathers doodads into clumps in the client;
    /// the count stops growing.
    uint32_t densityCap = 24;
    /// A ceiling per quad, against a table value far outside the usual range.
    uint32_t maxPerQuad = 128;
};

/// Where the chunk sits: the seed of its placement.
struct ClutterChunkKey {
    uint32_t mapId = 0;
    int tileX = 0;
    int tileY = 0;
    int chunkX = 0;
    int chunkY = 0;
};

/// One placed doodad, in render world space.
struct GroundClutterSample {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;    // radians
    float scale = 1.0f;
    uint32_t doodadId = 0;
    /// The ground under the root: its normal and the baked shadow (MCSH) on it,
    /// 0 lit, 1 shaded. A doodad is lit as that ground is, or it glows against
    /// the shaded floor of a forest.
    float normalX = 0.0f;
    float normalY = 0.0f;
    float normalZ = 1.0f;
    float shade = 0.0f;
};

/// What happened on each quad, for the diagnostic log.
enum class ClutterQuadState : uint8_t {
    Hole,         // a terrain hole
    Disabled,     // noEffectDoodad
    NoLayer,      // doodadMapping names a layer the chunk does not have
    NoEffect,     // the layer has no effect, or the table has no row for it
    Road,         // the layer's texture is a made surface
    Empty,        // an effect, but a density or doodads that make nothing
    Grown,        // doodads placed
};

struct ClutterQuadInfo {
    ClutterQuadState state = ClutterQuadState::NoEffect;
    uint8_t layer = 0;
    uint32_t effectId = 0;
    float expected = 0.0f;  // doodads the formula asks for
    uint16_t placed = 0;
};

/// Doodads per quad for a density: density * groundEffectDensity / 64 *
/// calibration, the density capped. Fractional; the placement rounds it per
/// quad by a roll, so the average holds.
[[nodiscard]] float clutterPerQuad(uint32_t density, const GroundClutterParams& params);

/// Place the clutter of one chunk, appending to `out`.
///
/// `roadLayer[i]` marks layer i as a made surface (by its texture's name,
/// which pipeline code never sees - the caller knows it). `quads`, when given,
/// receives what happened on each of the 64 quads, y-major.
/// Returns how many doodads were placed.
size_t placeChunkClutter(const MapChunk& chunk, const ClutterChunkKey& key,
                         const GroundEffectLookupFn& effectFor,
                         const std::array<bool, 4>& roadLayer,
                         const GroundClutterParams& params,
                         std::vector<GroundClutterSample>& out,
                         std::array<ClutterQuadInfo, 64>* quads = nullptr);

/// The 64 quads as eight lines of text: '.' no effect, 'x' disabled, 'h' hole,
/// 'r' road, '-' nothing grows, otherwise the count (0-9, '+' above nine).
[[nodiscard]] std::string clutterQuadMap(const std::array<ClutterQuadInfo, 64>& quads);

} // namespace pipeline
} // namespace wowee
