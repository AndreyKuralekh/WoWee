#include "rendering/m2_renderer.hpp"
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <cstring>
#include <chrono>
#include <limits>
#include <set>

namespace wowee {
namespace rendering {

namespace {

// Gravity given as a packed vector rather than a pull down the Z axis.
constexpr uint32_t kParticleFlagCompressedGravity = 0x800000;

// A glow rather than a flame: some colour it passes through is bluer than it
// is red, which no flame is. The night elves' lamps hold wisps that go white,
// violet, teal; the floors and the pull below are for fire, and turned that
// into orange specks raining down the post.
bool isColdGlow(const pipeline::M2ParticleEmitter& em) {
    for (const auto& c : em.particleColor.vec3Values) {
        if (c.b > c.r) return true;
    }
    return false;
}

// Whether the flame fixes apply to this emitter of this model.
bool isFlameEmitter(const M2ModelGPU& gpu, const pipeline::M2ParticleEmitter& em) {
    if (gpu.isTorch || gpu.isBrazierOrFire || gpu.isKoboldFlame) return true;
    return gpu.isLanternLike && !isColdGlow(em);
}

}  // namespace

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations,
                                bool firstSequenceFallback) {
    // A spell model keys most emitter values in its first sequence only:
    // BestowDisease_Impact_Chest has a lifespan for Stand and none for Hold,
    // which read as zero left its skulls unborn. They hold in every sequence.
    if (firstSequenceFallback && track.globalSequence < 0 && seqIdx > 0 &&
        !track.sequences.empty() &&
        (static_cast<size_t>(seqIdx) >= track.sequences.size() ||
         track.sequences[static_cast<size_t>(seqIdx)].timestamps.empty())) {
        seqIdx = 0;
    }
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    // The same list, for the same reason; see renderM2Particles. A ribbon
    // emitter is a particle emitter as far as this list is concerned.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (gpu.isInstancePortal) return;

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;
        // Only spell visual models: a torch or brazier game object is authored dark
        // in Stand and lit in Opened, which not every caller plays.
        if (gpu.followsEmitterEnabled &&
            m2_track::sampleFloat(em.enabledIn, inst.currentSequenceIndex, inst.animTime,
                                  inst.globalSequenceTime, gpu.globalSequenceDurations,
                                  1.0f) < 0.5f) {
            inst.emitterAccumulators[ei] = 0.0f;
            continue;
        }

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
        // What the player asked to see of it, before the floor below. The order
        // is the whole point: thinning first and flooring second lets a low
        // setting take smoke, dust and spell effects down while a candle is
        // pulled back up to the handful of particles that still reads as fire.
        rate *= particleDensity_;

        // A flame reads as a flame only when enough particles are alive at once.
        // Authored rates vary wildly for the same visual intent - a candle asks
        // for 40/s over half a second, CHANDELIER01 for 1/s over six seconds,
        // which sustains a single speck per candle and looks like a bare glow.
        // Steady-state population is rate x lifespan, so floor the rate against
        // the lifespan to hold every fixture at a comparable density. Do not lower
        // this: at 7 the flames disappear entirely, as they do under any change
        // that reduces how many particles are alive or how far they travel. The
        // effect is only visible because a scattering of particles reaches open
        // air, so thinning it drops the whole thing below the threshold.
        if (rate > 0.0f && life > 0.0f && isFlameEmitter(gpu, em)) {
            constexpr float kMinLiveParticles = 15.0f;
            rate = std::max(rate, kMinLiveParticles / std::max(life, 0.1f));
        }

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(gpu.name).second) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            p.maxLife = life;
            p.tileIndex = 0.0f;
            p.twinkleSeed = dist01(particleRng_);

            // Position: emitter position transformed by bone matrix, anywhere
            // in the emitter's area - a plane's rectangle, a sphere's ball.
            // From one point a lamp's cloud of wisps was a few specks.
            glm::vec3 localPos = em.position;
            const float areaLength = interpFloat(em.emissionAreaLength, inst.animTime, inst.globalSequenceTime,
                                                 inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            const float areaWidth = interpFloat(em.emissionAreaWidth, inst.animTime, inst.globalSequenceTime,
                                                inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            if (em.emitterType == 1) {
                localPos.x += distN(particleRng_) * areaLength * 0.5f;
                localPos.y += distN(particleRng_) * areaWidth * 0.5f;
            } else if (em.emitterType == 2 && std::max(areaLength, areaWidth) > 0.0f) {
                glm::vec3 out(distN(particleRng_), distN(particleRng_), distN(particleRng_));
                const float outSq = glm::dot(out, out);
                if (outSq > 1e-6f) out *= glm::inversesqrt(outSq);
                const float inner = std::min(areaLength, areaWidth);
                const float outer = std::max(areaLength, areaWidth);
                localPos += out * (inner + dist01(particleRng_) * (outer - inner));
            }
            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            glm::vec3 worldPos = glm::vec3(inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
            p.position = worldPos;

            // Velocity: emission speed in upward direction + random spread
            float speed = interpFloat(em.emissionSpeed, inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            const float speedVariation = interpFloat(em.speedVariation, inst.animTime, inst.globalSequenceTime,
                                                     inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            speed *= 1.0f + distN(particleRng_) * speedVariation;
            float vRange = interpFloat(em.verticalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            float hRange = interpFloat(em.horizontalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);

            // Base direction: up in model space, transformed to world
            glm::vec3 dir(0.0f, 0.0f, 1.0f);
            // Add random spread
            dir.x += distN(particleRng_) * hRange;
            dir.y += distN(particleRng_) * hRange;
            dir.z += distN(particleRng_) * vRange;
            float lenSq = glm::dot(dir, dir);
            if (lenSq > 0.001f * 0.001f) dir *= glm::inversesqrt(lenSq);
            if (gpu.followsEmitterEnabled) {
                // The ranges are angles: tilted off the emitter's axis by up
                // to the vertical one, turned round it by up to the
                // horizontal one. Added to the axis as lengths, a fireball's
                // smoke - no tilt, any turn - flew out sideways at 2*pi to
                // one up and hung round the caster as a cloud of grey balls
                // instead of streaming behind the missile.
                const float tilt = distN(particleRng_) * vRange;
                const float turn = distN(particleRng_) * hRange;
                dir = glm::vec3(std::sin(tilt) * std::cos(turn), std::sin(tilt) * std::sin(turn), std::cos(tilt));
            }

            // Transform direction by bone + model orientation (rotation only)
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);
            p.velocity = rotMat * dir * speed;

            // When emission speed is ~0 and bone animation isn't loaded (.anim files),
            // particles pile up at the same position. Give them a drift so they
            // spread outward like a mist/spray effect instead of clustering.
            // Not for a spell visual's model: its bones are loaded, and a
            // still particle is meant to stay put - the disease's stars
            // pulse where they are born rather than flying off.
            if (std::abs(speed) < 0.01f && !gpu.followsEmitterEnabled) {
                if (gpu.isFireflyEffect) {
                    // Fireflies: gentle random drift in all directions
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.3f
                    );
                } else {
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 1.0f,
                        distN(particleRng_) * 1.0f,
                        -dist01(particleRng_) * 0.5f
                    );
                }
            }

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            if ((em.flags & kParticleFlagTiled) && totalTiles > 1) {
                if (em.flags & kParticleFlagRandomized) {
                    distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                    p.tileIndex = static_cast<float>(distTile(particleRng_));
                } else {
                    p.tileIndex = 0.0f;
                }
            }

            inst.particles.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (gpu.isSpellEffect && inst.particles.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Hoist per-emitter gravity out of the per-particle loop. Gravity (and the
    // emissionSpeed fallback) depends only on the emitter and animation time -
    // not on the particle itself - so interpFloat was being re-evaluated for
    // every particle even when 100s of particles share one emitter.
    constexpr size_t kMaxStackEmitters = 16;
    glm::vec3 emitterGravStack[kMaxStackEmitters];
    std::vector<glm::vec3> emitterGravHeap;
    const size_t numEm = gpu.particleEmitters.size();
    glm::vec3* emitterGrav = nullptr;
    if (numEm > 0) {
        if (numEm <= kMaxStackEmitters) {
            emitterGrav = emitterGravStack;
        } else {
            emitterGravHeap.resize(numEm);
            emitterGrav = emitterGravHeap.data();
        }
        for (size_t e = 0; e < numEm; ++e) {
            const auto& pem = gpu.particleEmitters[e];
            float grav = interpFloat(pem.gravity,
                                      inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations,
                                 gpu.followsEmitterEnabled);
            // The pull is down, as much as it says. Packed, it is a direction
            // as two bytes and a strength as a short in the float's bits.
            glm::vec3 pull(0.0f, 0.0f, grav);
            if (pem.flags & kParticleFlagCompressedGravity) {
                uint32_t bits = 0;
                std::memcpy(&bits, &grav, sizeof(bits));
                const float x = static_cast<float>(static_cast<int8_t>(bits & 0xFF)) / 128.0f;
                const float y = static_cast<float>(static_cast<int8_t>((bits >> 8) & 0xFF)) / 128.0f;
                const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                const float strength = static_cast<float>(static_cast<int16_t>(bits >> 16)) * 0.04238648f;
                // Some models carry the flag over a plain pull: the flames a
                // fireball's precast holds rise at 3, which read as packed
                // is 690 yards a second squared - they were out of sight in
                // a frame and through the player's head in a headset. A pull
                // no model means, where the plain number is one it could.
                constexpr float kMostPacked = 100.0f;
                constexpr float kMostPlain = 50.0f;
                if (std::abs(strength) <= kMostPacked || !std::isfinite(grav) || std::abs(grav) > kMostPlain) {
                    pull = glm::vec3(x, y, z) * strength;
                }
            }
            // None where none is authored, flames included. Upstream pulled
            // every such emitter down (5f1c4dae: without it flames were not
            // seen, before the floors on colour and size), and a candle's
            // flame, which rises at a twentieth of a yard a second, rained
            // from the wick.
            emitterGrav[e] = pull;
        }
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            if (p.modelInstance != 0) deadModelParticles_.push_back(p.modelInstance);
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        if (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm) {
            p.velocity -= emitterGrav[p.emitterIndex] * dt;
        }
        p.position += p.velocity * dt;
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (gpu.isInstancePortal) return;

    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    // Grow per-instance state arrays if needed
    if (inst.ribbonEdges.size() != emitters.size()) {
        inst.ribbonEdges.resize(emitters.size());
    }
    if (inst.ribbonEdgeAccumulators.size() != emitters.size()) {
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    }

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges    = inst.ribbonEdges[ri];
        auto& accum    = inst.ribbonEdgeAccumulators[ri];

        // Determine bone world position for spine
        glm::vec3 spineWorld = inst.position;
        // Use referenced bone; fall back to bone 0 if out of range (common for spell effects
        // where ribbon bone fields may be unset/garbage, e.g. bone=4294967295)
        uint32_t boneIdx = em.bone;
        if (boneIdx >= inst.boneMatrices.size() && !inst.boneMatrices.empty()) {
            boneIdx = 0;
        }
        if (boneIdx < inst.boneMatrices.size()) {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * inst.boneMatrices[boneIdx] * local);
        } else {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * local);
        }

        // Skip emitters that produce NaN positions (garbage bone/position data)
        if (std::isnan(spineWorld.x) || std::isnan(spineWorld.y) || std::isnan(spineWorld.z))
            continue;

        // Evaluate animated tracks (use first available sequence key, or fallback value)
        auto getFloatVal = [&](const pipeline::M2AnimationTrack& track, float fallback) -> float {
            for (const auto& seq : track.sequences) {
                if (!seq.floatValues.empty()) return seq.floatValues[0];
            }
            return fallback;
        };
        auto getVec3Val = [&](const pipeline::M2AnimationTrack& track, glm::vec3 fallback) -> glm::vec3 {
            for (const auto& seq : track.sequences) {
                if (!seq.vec3Values.empty()) return seq.vec3Values[0];
            }
            return fallback;
        };

        float visibility  = getFloatVal(em.visibilityTrack, 1.0f);
        float heightAbove = getFloatVal(em.heightAboveTrack, 0.5f);
        float heightBelow = getFloatVal(em.heightBelowTrack, 0.5f);
        glm::vec3 color   = getVec3Val(em.colorTrack, glm::vec3(1.0f));
        float alpha       = getFloatVal(em.alphaTrack, 1.0f);

        // Age existing edges and remove expired ones
        for (auto& e : edges) {
            e.age += dt;
            // Apply gravity
            if (em.gravity != 0.0f) {
                e.worldPos.z -= em.gravity * dt * dt * 0.5f;
            }
        }
        while (!edges.empty() && edges.front().age >= em.edgeLifetime) {
            edges.pop_front();
        }

        // Emit new edges based on edgesPerSecond
        if (visibility > 0.5f) {
            accum += em.edgesPerSecond * dt;
            while (accum >= 1.0f) {
                accum -= 1.0f;
                M2Instance::RibbonEdge e;
                e.worldPos    = spineWorld;
                e.color       = color;
                e.alpha       = alpha;
                // Scaled into world units here, where the instance is at hand.
                // The spine is a world position the model matrix produced, and
                // these two were model-space lengths added straight to it - so
                // a doodad placed at any scale but 1 got a trail the right
                // length and the wrong width.
                e.heightAbove = heightAbove * inst.scale;
                e.heightBelow = heightBelow * inst.scale;
                e.age         = 0.0f;
                edges.push_back(e);

                // Diagnostic: log first ribbon edge per spell effect instance+emitter
                if (gpu.isSpellEffect && edges.size() == 1) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name,
                             "' emitter=", ri, " pos=(", spineWorld.x, ",", spineWorld.y,
                             ",", spineWorld.z, ") hA=", heightAbove, " hB=", heightBelow,
                             " vis=", visibility, " eps=", em.edgesPerSecond,
                             " edgeLife=", em.edgeLifetime, " bone=", em.bone);
                }

                // Cap trail length
                if (edges.size() > 128) edges.pop_front();
            }
        } else {
            accum = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // A ribbon's width runs across the trail and across the view, and it has to
    // be computed per edge from the direction the trail is actually going.
    //
    // This used a fixed world Z, which is only right for a trail travelling
    // horizontally. A flame licks upward, so its spine ran along the very axis
    // the strip was being widened on: the top and bottom vertices of every
    // edge landed on the spine itself, the quad collapsed, and the whole trail
    // drew as one tall thin sliver with the fire texture smeared up it. That
    // is the bonfire at Grom'gol standing four times the height of the huts
    // behind it.
    const glm::vec3 camPos = cachedCamPos_;
    const glm::vec3 upWorld(0.0f, 0.0f, 1.0f);

    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    for (const auto& inst : instances) {
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) continue;
        if (gpu.ribbonEmitters.empty()) continue;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;

            const auto& em = gpu.ribbonEmitters[ri];

            // Select blend pipeline based on material blend mode
            bool additive = false;
            if (em.materialIndex < gpu.batches.size()) {
                additive = (gpu.batches[em.materialIndex].blendMode >= 3);
            }
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom)
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                float t = (em.edgeLifetime > 0.0f)
                          ? 1.0f - (e.age / em.edgeLifetime) : 1.0f;
                float a = e.alpha * t;
                float u = static_cast<float>(ei) / static_cast<float>(edges.size() - 1);

                // The trail's own direction here, from the edges either side.
                const glm::vec3& prev = edges[ei > 0 ? ei - 1 : ei].worldPos;
                const glm::vec3& next = edges[ei + 1 < edges.size() ? ei + 1 : ei].worldPos;
                glm::vec3 spineDir = next - prev;
                float spineLen = glm::length(spineDir);
                // Widen across the trail and across the line of sight. Where
                // the trail doubles back on itself or points straight at the
                // eye there is no such direction, and world up is as good an
                // answer as any.
                glm::vec3 side = upWorld;
                if (spineLen > 1e-5f) {
                    glm::vec3 toEye = camPos - e.worldPos;
                    glm::vec3 cross = glm::cross(spineDir / spineLen, toEye);
                    float crossLen = glm::length(cross);
                    if (crossLen > 1e-5f) side = cross / crossLen;
                }

                // Top vertex (one side of the spine)
                glm::vec3 top = e.worldPos + side * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f; // v = top
                written++;

                // Bottom vertex (the other side)
                glm::vec3 bot = e.worldPos - side * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f; // v = bottom
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : instances) {
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbonEdge : inst.ribbonEdges) {
                    if (ribbonEdge.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbonEdge.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!particlePipeline_ || !m2ParticleVB_) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Collect all particles from all instances, grouped by texture+blend.
    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) {
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    // Written straight into the mapped buffer rather than accumulated into a
    // vector per group and copied in afterwards; see ParticleRun.
    if (!m2ParticleVBMapped_) return;
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_);
    uint32_t vbWritten = 0;
    particleRuns_.clear();
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld. The smoke pass beside this
    // one does the same kind of work against a flat list and costs 0.003ms.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& inst = instances[idx];
        if (inst.particles.empty()) continue;
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) continue;


        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        bool cachedFlame = false;
        ParticleGroup* cachedGroup = nullptr;
        // animFrame depends only on inst.animTime + totalTiles, so it's also
        // emitter-stable within one frame.
        uint32_t cachedAnimFrame = 0;
        float cachedTilesFloat = 1.0f;
        bool cachedIsTiled = false;
        float invAnimMs = 1.0f / 1000.0f;

        // How far this instance's particles actually reach, against how far
        // the model says it extends. A fire twice the height of the hut behind
        // it is either particles outliving their authored lifespan, flying at
        // the wrong speed, or drawn at the wrong size - and none of those says
        // which model it is. This does.
        float highestParticleZ = -std::numeric_limits<float>::max();
        float widestParticle = 0.0f;
        // And for a spell effect on the player, where it is drawn: see below.
        glm::vec3 drawnSum(0.0f);
        uint32_t drawnCount = 0;
        float brightest = 0.0f;

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;
            if (p.modelInstance != 0) continue;  // drawn as a model; see updateModelParticles

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];
                cachedFlame = isFlameEmitter(gpu, *cachedEm);

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedIsTiled = (cachedEm->flags & kParticleFlagTiled) && cachedTotalTiles > 1;
                if (cachedIsTiled) {
                    float animSeconds = inst.animTime * invAnimMs;
                    cachedAnimFrame = static_cast<uint32_t>(std::floor(animSeconds * cachedTotalTiles))
                                      % cachedTotalTiles;
                    cachedTilesFloat = static_cast<float>(cachedTotalTiles);
                }
            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            float rawScale = interpFBlockFloat(em.particleScale, lifeRatio);

            if (!gpu.isSpellEffect && !gpu.isFireflyEffect && !gpu.isLanternLike &&
                !gpu.isTorch && !gpu.isBrazierOrFire && !gpu.isKoboldFlame) {
                color = glm::mix(color, glm::vec3(1.0f), 0.7f);
                if (rawScale > 2.0f) alpha *= 0.02f;
                if (cachedBlendType == 3 || cachedBlendType == 4) alpha *= 0.05f;
            }
            // Flame fixtures: the authored curves can leave a particle with
            // effectively no colour or alpha for most of its life. CHANDELIER01
            // ramps scale from zero over a six second life and its candles spend
            // nearly all of that time contributing nothing - and because these
            // draw additively, a near-black particle adds literally nothing to
            // the frame. Floor colour and alpha so a lit fixture always shows
            // flame. Floors only lift the dim end, leaving torches and candles
            // that already read correctly untouched.
            if (cachedFlame) {
                color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
                alpha = std::max(alpha, 0.30f);
            }

            float scale = rawScale;
            // A lamp's specks keep the size they had under the shader's old
            // fixed scale: drawn true to the model, its embers and wisps are
            // orange balls round the flame rather than insects in its light.
            bool legacySize = false;
            if (gpu.followsEmitterEnabled) {
                // A spell visual's model at its authored size. Half as big
                // again, the disease's skull-faced puffs grew into a haze two
                // yards across that hid every skull. Its twinkle is part of
                // that size: a fireball's hand flames are 0.013 twinkled 23.
                scale = std::max(rawScale * twinkleScale(em, p), 0.05f);
            } else if (gpu.isSpellEffect) {
                scale = std::max(rawScale * 1.5f, 0.05f);
            } else if (!gpu.isFireflyEffect) {
                scale = std::min(rawScale, 1.5f);
                // Candle flames are authored at a fraction of a unit, which lands
                // sub-pixel at any normal viewing distance - the fixture glows
                // with no visible flame. Small effect-heavy models dodge this by
                // being classified as spell effects (three or more emitters and
                // few vertices) and picking up that path's floor, but a
                // chandelier is a real fixture at 370 vertices and misses the
                // cut, so its five candles rendered as nothing. Give flame
                // fixtures the same floor without making them spell effects,
                // which would also change how they blend.
                //
                // Set when the shader drew every particle a third of its size:
                // 0.05 now is the 0.15 it was then. A lamp keeps the 0.15 in
                // the old scale itself; see legacySize.
                if (cachedFlame && gpu.isLanternLike) {
                    scale = std::max(scale, 0.15f);
                    legacySize = true;
                } else if (cachedFlame) {
                    scale = std::max(scale, 0.05f);
                }
            }

            if (vbWritten >= MAX_M2_PARTICLE_VERTS) break;
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, p.position.z);
            widestParticle = std::max(widestParticle, scale);
            drawnSum += p.position;
            ++drawnCount;
            brightest = std::max(brightest, alpha);

            float* vd = vbBase + static_cast<size_t>(vbWritten) * 9;
            vd[0] = p.position.x;
            vd[1] = p.position.y;
            vd[2] = p.position.z;
            vd[3] = color.r;
            vd[4] = color.g;
            vd[5] = color.b;
            vd[6] = alpha;
            vd[7] = legacySize ? -scale : scale;
            float tileIndex = p.tileIndex;
            // A cell track says which frame of the atlas a particle shows at
            // each point of its life, so a skull forms and comes apart as it
            // rises. Without it every particle showed the same frame - the
            // first, when the emitter lacked the tiled flag.
            if (!em.headCell.floatValues.empty() && cachedTotalTiles > 1 &&
                !(em.flags & kParticleFlagRandomized)) {
                tileIndex = std::fmod(std::floor(interpFBlockFloat(em.headCell, lifeRatio)),
                                      static_cast<float>(cachedTotalTiles));
            } else if (cachedIsTiled) {
                tileIndex = p.tileIndex + static_cast<float>(cachedAnimFrame);
                while (tileIndex >= cachedTilesFloat) {
                    tileIndex -= cachedTilesFloat;
                }
            }
            vd[8] = tileIndex;
            ++vbWritten;
            ++particleRuns_.back().count;
            totalParticles++;
        }

        // Said once per model, on an absolute reach rather than a ratio
        // against the model's own bounds. A bonfire's authored box is 25 to 32
        // yards tall - it has to hold the flame's full extent - so a ratio
        // test can never trip on the one model anybody is complaining about.
        // Nothing that stands on the ground should be throwing particles eight
        // yards into the air.
        if (highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }

        // A spell effect within reach of the eyes - on the player's own hands
        // - said twice a second: how many of its particles are drawn, where
        // they are against it and the eyes, how big and how bright. Flames
        // seen in a fake headset and not in a real one left nothing else to
        // go on.
        if (gpu.followsEmitterEnabled) {
            const glm::vec3 toEyes = cachedCamPos_ - inst.position;
            if (glm::dot(toEyes, toEyes) < 9.0f) {
                const auto now = std::chrono::steady_clock::now();
                static std::chrono::steady_clock::time_point lastSaid;
                static uint32_t saidThisTime = 0;
                if (now - lastSaid > std::chrono::milliseconds(500)) {
                    lastSaid = now;
                    saidThisTime = 0;
                }
                if (saidThisTime++ < 4) {
                    const glm::vec3 mean = drawnCount ? drawnSum / static_cast<float>(drawnCount) : inst.position;
                    LOG_INFO("SpellEffect near eyes: '", gpu.name, "' id=", inst.id, " at (", inst.position.x, ",",
                             inst.position.y, ",", inst.position.z, ") eyes (", cachedCamPos_.x, ",",
                             cachedCamPos_.y, ",", cachedCamPos_.z, ") ", glm::length(toEyes), " yd; ",
                             inst.particles.size(), " live, ", drawnCount, " drawn, mean offset (",
                             mean.x - inst.position.x, ",", mean.y - inst.position.y, ",",
                             mean.z - inst.position.z, "), largest ", widestParticle, ", alpha ", brightest,
                             ", seq ", inst.currentSequenceIndex, " t=", inst.animTime, " fade ", inst.fade);
                }
            }
        }
    }

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : instances) {
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            // The buffer is full: whatever comes last in the list - a spell
            // cast a moment ago - is not drawn at all. Say who filled it.
            if (totalParticles >= MAX_M2_PARTICLE_VERTS) {
                std::unordered_map<std::string, size_t> perModel;
                for (size_t idx : particleInstanceIndices_) {
                    if (idx >= instances.size() || !instances[idx].cachedModel) continue;
                    perModel[instances[idx].cachedModel->name] += instances[idx].particles.size();
                }
                std::vector<std::pair<size_t, std::string>> top;
                for (auto& [name, n] : perModel) top.emplace_back(n, name);
                std::sort(top.begin(), top.end(), std::greater<>());
                for (size_t i = 0; i < top.size() && i < 4; ++i) {
                    LOG_WARNING("M2 particle buffer full (", MAX_M2_PARTICLE_VERTS, "): '", top[i].second,
                                "' holds ", top[i].first);
                }
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        uint8_t blendType = group.blendType;
        VkPipeline desiredPipeline = (blendType == 3 || blendType == 4)
            ? particleAdditivePipeline_ : particlePipeline_;
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            // Fallback: allocate per-frame (pool exhaustion risk - should not happen in practice)
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = (group.texture && group.texture->isValid())
                    ? group.texture : whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        if (texSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    particlePipelineLayout_, 1, 1, &texSet, 0, nullptr);
        }

        // Push constants: tileCount + alphaKey
        struct { float tileX, tileY; int alphaKey; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        const ParticleSizePush sizePush{.viewHeight = viewHeight_};
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           kParticleSizePushOffset, sizeof(sizePush), &sizePush);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        vkCmdDraw(cmd, run.count, 1, run.first, 0);
    }
}

void M2Renderer::renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (smokeParticles.empty() || !smokePipeline_ || !smokeVB_) return;

    // Build vertex data: pos(3) + lifeRatio(1) + size(1) + isSpark(1) per particle
    size_t count = std::min(smokeParticles.size(), static_cast<size_t>(MAX_SMOKE_PARTICLES));
    float* dst = static_cast<float*>(smokeVBMapped_);
    for (size_t i = 0; i < count; i++) {
        const auto& p = smokeParticles[i];
        *dst++ = p.position.x;
        *dst++ = p.position.y;
        *dst++ = p.position.z;
        *dst++ = p.life / p.maxLife;
        *dst++ = p.size;
        *dst++ = p.isSpark;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smokePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            smokePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Push constant: screenHeight
    float screenHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height);
    vkCmdPushConstants(cmd, smokePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float), &screenHeight);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &smokeVB_, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
}

float M2Renderer::twinkleScale(const pipeline::M2ParticleEmitter& em, const M2Particle& p) {
    if (em.twinklePercent <= 0.0f || p.twinkleSeed >= em.twinklePercent) return 1.0f;
    if (em.twinkleScaleMin == em.twinkleScaleMax) return em.twinkleScaleMin;
    const float wave = 0.5f + 0.5f * std::sin((p.life * em.twinkleSpeed + p.twinkleSeed) * glm::two_pi<float>());
    return glm::mix(em.twinkleScaleMin, em.twinkleScaleMax, wave);
}

void M2Renderer::setEmitterParticleModel(uint32_t modelId, size_t emitterIndex, uint32_t particleModelId) {
    auto it = models.find(modelId);
    if (it == models.end() || emitterIndex >= it->second.particleEmitters.size()) return;
    auto& slots = it->second.emitterParticleModels;
    if (slots.size() < it->second.particleEmitters.size()) slots.resize(it->second.particleEmitters.size(), 0);
    slots[emitterIndex] = particleModelId;
}

void M2Renderer::updateModelParticles() {
    // An emitting instance removed by any path leaves its particles' models
    // behind; they go with it here rather than in each removal path.
    for (const auto& [child, owner] : modelParticleOwner_) {
        if (!instanceIndexById.count(owner)) deadModelParticles_.push_back(child);
    }
    for (uint32_t id : deadModelParticles_) {
        modelParticleOwner_.erase(id);
        auto it = instanceIndexById.find(id);
        if (it == instanceIndexById.end()) continue;
        instances[it->second].fade = 0.0f;
        modelParticlePool_[instances[it->second].modelId].push_back(id);
    }
    deadModelParticles_.clear();

    std::vector<uint32_t> owners;
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (inst.cachedModel && !inst.cachedModel->emitterParticleModels.empty() && !inst.particles.empty())
            owners.push_back(inst.id);
    }

    for (uint32_t ownerId : owners) {
        for (size_t pi = 0;; ++pi) {
            // Looked up again each time round: creating an instance can move
            // every instance, the owner included.
            auto ownerIt = instanceIndexById.find(ownerId);
            if (ownerIt == instanceIndexById.end()) break;
            M2Instance& owner = instances[ownerIt->second];
            if (pi >= owner.particles.size()) break;
            const M2ModelGPU& gpu = *owner.cachedModel;
            const M2Particle& p = owner.particles[pi];
            if (p.emitterIndex < 0 || static_cast<size_t>(p.emitterIndex) >= gpu.emitterParticleModels.size())
                continue;
            const uint32_t particleModel = gpu.emitterParticleModels[p.emitterIndex];
            if (particleModel == 0) continue;

            const auto& em = gpu.particleEmitters[p.emitterIndex];
            const float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            const float scale = std::max(interpFBlockFloat(em.particleScale, lifeRatio) * twinkleScale(em, p), 0.001f);
            const float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            const glm::vec3 position = p.position;
            // Nothing in the emitter turns it, so it turns to the viewer
            // about the vertical: a skull looks at whoever is looking.
            const glm::vec3 toCamera = cachedCamPos_ - position;
            const float yaw = std::atan2(toCamera.y, toCamera.x);

            uint32_t id = p.modelInstance;
            if (id == 0) {
                auto& spare = modelParticlePool_[particleModel];
                while (!spare.empty() && !instanceIndexById.count(spare.back())) spare.pop_back();
                if (!spare.empty()) {
                    id = spare.back();
                    spare.pop_back();
                } else {
                    id = createInstance(particleModel, position, glm::vec3(0.0f), scale);
                }
                if (id == 0) continue;
                auto again = instanceIndexById.find(ownerId);
                if (again == instanceIndexById.end()) {
                    setInstanceFade(id, 0.0f);
                    modelParticlePool_[particleModel].push_back(id);
                    break;
                }
                instances[again->second].particles[pi].modelInstance = id;
                modelParticleOwner_[id] = ownerId;
            }
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), position);
            transform = glm::rotate(transform, yaw, glm::vec3(0.0f, 0.0f, 1.0f));
            transform = glm::scale(transform, glm::vec3(scale));
            setInstanceTransform(id, transform);
            setInstanceFade(id, alpha);
        }
    }
}

} // namespace rendering
} // namespace wowee
