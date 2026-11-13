#pragma once

#include <algorithm>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline {
    struct ADTTerrain;
    struct LiquidData;
    struct WMOLiquid;
}

namespace rendering {

class Camera;
class VkContext;

/**
 * Water surface for a single map chunk
 */
struct WaterSurface {
    glm::vec3 position;
    glm::vec3 origin;
    glm::vec3 stepX;
    glm::vec3 stepY;
    float minHeight;
    float maxHeight;
    uint16_t liquidType;

    int tileX = -1, tileY = -1;
    uint32_t wmoId = 0;

    uint8_t xOffset = 0;
    uint8_t yOffset = 0;
    uint8_t width = 8;
    uint8_t height = 8;

    std::vector<float> heights;
    std::vector<uint8_t> mask;

    // Vulkan render data
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    int indexCount = 0;

    // Per-surface material UBO
    ::VkBuffer materialUBO = VK_NULL_HANDLE;
    VmaAllocation materialAlloc = VK_NULL_HANDLE;

    // Material descriptor set (set 1)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    [[nodiscard]] bool hasHeightData() const { return !heights.empty(); }
};

/**
 * Water renderer (Vulkan) with planar reflections, Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

struct WaterFrameUBOData {
    glm::mat4 reflViewProj{1.0f};
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
    glm::vec4 moonDir{0.0f};                 // xyz = toward the moon, w = how much it lights the water
};

class WaterRenderer {
public:
    WaterRenderer();
    ~WaterRenderer();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();

    void loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append = false,
                         int tileX = -1, int tileY = -1);

    void loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix, uint32_t wmoId);
    void removeWMO(uint32_t wmoId);
    void removeTile(int tileX, int tileY);
    void clear();

    void recreatePipelines();

    // Separate 1x pass for MSAA mode - water rendered after MSAA resolve
    bool createWater1xPass(VkFormat colorFormat, VkFormat depthFormat);
    void createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                    VkImageView depthView, VkExtent2D extent);
    void destroyWater1xResources();
    bool hasWater1xPass() const { return water1xRenderPass != VK_NULL_HANDLE; }
    VkRenderPass getWater1xRenderPass() const { return water1xRenderPass; }
    VkFramebuffer getWater1xFramebuffer(uint32_t index) const {
        return index < water1xFramebuffers.size() ? water1xFramebuffers[index] : VK_NULL_HANDLE;
    }

    /// Which picture the water refracts. The window's copy is the size of the
    /// window; a headset's eyes are another size and shape, so they have a
    /// copy of their own, made the first time an eye is copied. Both eyes go
    /// through the one copy in turn: an eye copies, draws its water, and the
    /// next eye's copy waits for that draw to have read it.
    enum class SceneTarget : uint32_t { Window = 0, Eyes = 1 };

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float time, bool use1x = false,
                uint32_t frameIndex = 0, SceneTarget target = SceneTarget::Window);
    /// The source is left in the layouts a scene pass ends with: colour in
    /// PRESENT_SRC, depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
    void captureSceneHistory(VkCommandBuffer cmd,
                             VkImage srcColorImage,
                             VkImage srcDepthImage,
                             VkExtent2D srcExtent,
                             bool srcDepthIsMsaa,
                             uint32_t frameIndex = 0,
                             SceneTarget target = SceneTarget::Window);

    /// The eyes' scene copy at `extent`, made now if it is not (waiting the
    /// frames in flight out, as captureSceneHistory would), and the view of
    /// its depth for frame-in-flight `frameIndex` - for what is recorded
    /// against it before the copy is taken. VK_NULL_HANDLE if it cannot be.
    VkImageView prepareEyeSceneDepth(VkExtent2D extent, uint32_t frameIndex);

    /// The eyes' water refracts the bed by moving it in the world, at this
    /// strength (1 as designed, 0 a still bed); below 0, the window's
    /// screen-space refraction as before.
    void setEyeRefractionStrength(float s) { eyeRefractionStrength_ = s; }
    /// And the view of its colour, once prepareEyeSceneDepth has made it.
    VkImageView eyeSceneColor(uint32_t frameIndex) {
        return sceneTarget(SceneTarget::Eyes).frames[frameIndex % SCENE_HISTORY_FRAMES].colorView;
    }

    // --- Planar reflection pass ---
    // Call sequence: beginReflectionPass → [render scene] → endReflectionPass
    bool beginReflectionPass(VkCommandBuffer cmd);
    void endReflectionPass(VkCommandBuffer cmd);

    // Get the dominant water height near a position (for reflection plane)
    std::optional<float> getDominantWaterHeight(const glm::vec3& cameraPos) const;

    // Compute reflected view matrix for a given water height
    static glm::mat4 computeReflectedView(const Camera& camera, float waterHeight);
    // Compute oblique clip projection to clip below-water geometry in reflection
    static glm::mat4 computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view, float waterHeight);

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    // --- Sky probe ---
    // The sky round the camera as a cube, for a reflection that needs no
    // second pass over the scene: one face a frame, drawn by the sky system
    // into the reflection pass, then the whole cube's mips after the sixth.
    // It holds sky only - no buildings, no shore - and is the same for both
    // of a headset's eyes, so nothing in it differs between them.
    static constexpr uint32_t kSkyProbeFaces = 6;
    static constexpr uint32_t kSkyProbeSize = 256;
    bool hasSkyProbe() const { return skyProbeImage_ != VK_NULL_HANDLE; }
    /// Opens the reflection pass on one face of the cube. Outside any pass.
    bool beginSkyProbeFace(VkCommandBuffer cmd, uint32_t face);
    /// Closes it; after the last face, rebuilds the mips and the cube counts.
    void endSkyProbeFace(VkCommandBuffer cmd, uint32_t face);
    /// A face's camera: 90 degrees, square, looking down an axis of the world
    /// the way a cube map is read (no y flip - the sky draws without culling).
    static glm::mat4 skyProbeFaceView(uint32_t face, const glm::vec3& position);
    static glm::mat4 skyProbeProjection(float nearPlane, float farPlane);
    /// Whether the water reads the sky probe at all (Water reflections).
    void setSkyReflection(bool on) { skyReflection_ = on; }
    /// Where the moon the sky draws stands, and how much of the night's light
    /// it is (0 by day, under the horizon, or with no moon drawn): the track it
    /// lays on the water is drawn toward it, not toward the zone's night light.
    void setMoon(const glm::vec3& towardMoon, float strength) { frameUBO_.moonDir = glm::vec4(towardMoon, strength); }

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}; }
    bool hasReflectionPass() const { return reflectionRenderPass != VK_NULL_HANDLE; }
    bool hasSurfaces() const { return !surfaces.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    void setRefractionEnabled(bool enabled);
    bool isRefractionEnabled() const { return refractionEnabled; }
    // Display brightness (1.0 = neutral). The scene-history capture used for
    // refraction bakes this in, so the shader divides it back out.
    // Size of the target the water is drawn into. Screen-space lookups derive
    // their UVs from this rather than from the refraction texture's own size,
    // which is deliberately smaller than the frame.
    void setRenderExtent(VkExtent2D e) { renderExtent_ = e; }
    /// The near and far planes of the projection the water is drawn through,
    /// when that is not the camera's own (a headset's eyes have their own near
    /// plane). The shader turns the depth copy into yards with them.
    void setDepthRange(float nearPlane, float farPlane) { depthRangeOverride_ = glm::vec2(nearPlane, farPlane); }
    void clearDepthRange() { depthRangeOverride_.reset(); }
    /// How far into the water the bed shows (Water clarity): 1 as designed,
    /// 2 through twice the depth.
    void setClarity(float clarity) { clarity_ = std::clamp(clarity, 0.5f, 2.0f); }

    /// Where a point sits on a surface, if that surface has water there:
    /// the projection and the render mask, which every query asks first.
    std::optional<glm::vec2> wateredGridPosition(const WaterSurface& surface,
                                                 float glX, float glY) const;

    std::optional<float> getWaterHeightAt(float glX, float glY) const;
    /// Like getWaterHeightAt but only returns water surfaces whose height is
    /// close to the query Z (within maxAbove units above). Avoids false
    /// underwater detection from elevated WMO water far above the camera.
    std::optional<float> getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove = 15.0f) const;
    std::optional<uint16_t> getWaterTypeAt(float glX, float glY) const;
    bool isWmoWaterAt(float glX, float glY) const;

    int getSurfaceCount() const { return static_cast<int>(surfaces.size()); }

private:
    // How far each point of a tile's sea is from its shore, and which way the
    // shore lies: the surf's wave fronts run along the coast and toward it.
    struct ShoreField;
    void createWaterMesh(WaterSurface& surface, const ShoreField* shore = nullptr);
    void destroyWaterMesh(WaterSurface& surface);

    glm::vec4 getLiquidColor(uint16_t liquidType) const;
    float getLiquidAlpha(uint16_t liquidType) const;

    void updateMaterialUBO(WaterSurface& surface);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(SceneTarget target, VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources(SceneTarget target);

    // Reflection pass resources
    void createReflectionResources();
    void destroyReflectionResources();
    void createSkyProbeResources();
    void destroySkyProbeResources();

    VkImage skyProbeImage_ = VK_NULL_HANDLE;
    VmaAllocation skyProbeAlloc_ = VK_NULL_HANDLE;
    VkImageView skyProbeCubeView_ = VK_NULL_HANDLE;
    VkImageView skyProbeFaceViews_[kSkyProbeFaces] = {};
    VkFramebuffer skyProbeFramebuffers_[kSkyProbeFaces] = {};
    VkImage skyProbeDepthImage_ = VK_NULL_HANDLE;
    VmaAllocation skyProbeDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView skyProbeDepthView_ = VK_NULL_HANDLE;
    VkSampler skyProbeSampler_ = VK_NULL_HANDLE;
    uint32_t skyProbeMips_ = 1;
    uint32_t skyProbeFacesDrawn_ = 0;  // since the probe was made, up to six
    bool skyProbeReady_ = false;       // every face drawn and the mips built
    bool skyReflection_ = true;

    VkContext* vkCtx = nullptr;

    // Pipeline
    VkPipeline waterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One reflection/wake UBO slot per frame in flight, at an offset every
    // device accepts for a uniform buffer (Vulkan caps the alignment at 256).
    static constexpr VkDeviceSize kFrameUBOStride =
        (sizeof(WaterFrameUBOData) + 255) / 256 * 256;
    struct PerFrameSceneHistory {
        VkImage colorImage = VK_NULL_HANDLE;
        VmaAllocation colorAlloc = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAlloc = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };
    struct SceneHistoryTarget {
        PerFrameSceneHistory frames[SCENE_HISTORY_FRAMES];
        VkExtent2D extent = {.width = 0, .height = 0};
        bool ready = false;  // copied for the draw that comes next
        VkDescriptorPool pool = VK_NULL_HANDLE;
    };
    static constexpr uint32_t kSceneTargets = 2;
    SceneHistoryTarget sceneTargets_[kSceneTargets];
    SceneHistoryTarget& sceneTarget(SceneTarget t) { return sceneTargets_[static_cast<uint32_t>(t)]; }
    mutable uint32_t renderDiagCounter_ = 0;

    // Planar reflection resources
    static constexpr uint32_t REFLECTION_WIDTH = 512;
    static constexpr uint32_t REFLECTION_HEIGHT = 512;
    VkRenderPass reflectionRenderPass = VK_NULL_HANDLE;
    VkFramebuffer reflectionFramebuffer = VK_NULL_HANDLE;
    VkImage reflectionColorImage = VK_NULL_HANDLE;
    VmaAllocation reflectionColorAlloc = VK_NULL_HANDLE;
    VkImageView reflectionColorView = VK_NULL_HANDLE;
    VkImage reflectionDepthImage = VK_NULL_HANDLE;
    VmaAllocation reflectionDepthAlloc = VK_NULL_HANDLE;
    VkImageView reflectionDepthView = VK_NULL_HANDLE;
    VkSampler reflectionSampler = VK_NULL_HANDLE;
    VkImageLayout reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Reflection UBO (mat4 reflViewProj)
    // Surface disturbance trail. Points age out, spreading as they go; the
    // per-point drift is what turns a swimmer's pair of emissions into a V.
    struct WakePoint {
        glm::vec2 pos{0.0f};
        glm::vec2 drift{0.0f};
        float age = 0.0f;        // seconds
        float life = 1.0f;       // seconds
        float strength = 0.0f;
    };
    std::vector<WakePoint> wakePoints_;
    WaterFrameUBOData frameUBO_{};
    glm::vec2 lastWakeEmitPos_{0.0f};
    bool hasWakeEmitPos_ = false;

    void uploadFrameUBO();

    ::VkBuffer reflectionUBO = VK_NULL_HANDLE;
    VmaAllocation reflectionUBOAlloc = VK_NULL_HANDLE;
    void* reflectionUBOMapped = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    VkPipeline water1xPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    std::vector<WaterSurface> surfaces;
    bool renderingEnabled = true;
    bool refractionEnabled = false;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
    std::optional<glm::vec2> depthRangeOverride_;
    float clarity_ = 1.3f;
    float eyeRefractionStrength_ = -1.0f;  // < 0: screen-space refraction in the eyes too
    // The reflection pass ran this frame and set its matrix. Without it the
    // reflection texture holds whatever was last drawn into it, from another
    // camera and maybe another place, and the water is told to leave it out.
    bool reflectionFresh_ = false;
};

} // namespace rendering
} // namespace wowee
