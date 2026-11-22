// src/game/transport_path_repository.cpp
// Owns and manages transport path data - DBC, taxi, and custom paths.
// Ported from TransportManager (path management subset).
#include "game/transport_path_repository.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <limits>

namespace wowee::game {

namespace {

bool isDeeprunTramPath(uint32_t transportEntry) {
    return transportEntry >= 176080u && transportEntry <= 176085u;
}

glm::vec3 transportAnimationOffsetToCanonical(uint32_t transportEntry, const glm::vec3& pos) {
    if (isDeeprunTramPath(transportEntry)) {
        // Deeprun's TransportAnimation rows are local subway-car offsets, not
        // server/world coordinates. Raw X is the tunnel travel axis; swapping it
        // through serverToCanonical makes the cars drive perpendicular to the rails.
        return glm::vec3(pos.x, pos.y, pos.z);
    }

    // TransportAnimation.dbc local offsets use a coordinate system where the travel
    // axis is negated relative to server world coords for ship/zeppelin-style paths.
    return core::coords::serverToCanonical(glm::vec3(-pos.x, -pos.y, pos.z));
}

} // namespace

// ── Simple lookup methods ──────────────────────────────────────

const PathEntry* TransportPathRepository::findPath(uint32_t pathId) const {
    auto it = paths_.find(pathId);
    return it != paths_.end() ? &it->second : nullptr;
}

const PathEntry* TransportPathRepository::findTaxiPath(uint32_t taxiPathId, uint32_t mapId) const {
    auto it = taxiPaths_.find(taxiPathId);
    if (it == taxiPaths_.end()) return nullptr;
    auto mIt = it->second.find(mapId);
    return mIt != it->second.end() ? &mIt->second : nullptr;
}

bool TransportPathRepository::hasPathForEntry(uint32_t entry) const {
    auto* e = findPath(entry);
    return e != nullptr && e->fromDBC;
}

bool TransportPathRepository::hasTaxiPath(uint32_t taxiPathId) const {
    auto it = taxiPaths_.find(taxiPathId);
    return it != taxiPaths_.end() && !it->second.empty();
}

bool TransportPathRepository::hasTaxiPathForMap(uint32_t taxiPathId, uint32_t mapId) const {
    return findTaxiPath(taxiPathId, mapId) != nullptr;
}

void TransportPathRepository::storePath(uint32_t pathId, PathEntry entry) {
    auto it = paths_.find(pathId);
    if (it != paths_.end()) {
        it->second = std::move(entry);
    } else {
        paths_.emplace(pathId, std::move(entry));
    }
}

// ── Query methods ──────────────────────────────────────────────

bool TransportPathRepository::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    auto* e = findPath(entry);
    if (!e) return false;
    if (!e->fromDBC || e->spline.keyCount() < 2 || e->spline.durationMs() == 0 || e->zOnly) {
        return false;
    }
    return e->spline.hasXYMovement(minXYRange);
}

uint32_t TransportPathRepository::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                                        float maxDistance,
                                                        bool allowZOnly) const {
    float bestD2 = maxDistance * maxDistance;
    uint32_t bestPathId = 0;

    for (const auto& [pathId, entry] : paths_) {
        if (!entry.fromDBC || entry.spline.durationMs() == 0 || entry.spline.keyCount() == 0) {
            continue;
        }
        if (!allowZOnly && entry.zOnly) {
            continue;
        }

        // Find nearest waypoint on this path to spawn
        size_t nearIdx = entry.spline.findNearestKey(spawnWorldPos);
        glm::vec3 diff = entry.spline.keys()[nearIdx].position - spawnWorldPos;
        float d2 = glm::dot(diff, diff);
        if (d2 < bestD2) {
            bestD2 = d2;
            bestPathId = pathId;
        }
    }

    if (bestPathId != 0) {
        LOG_INFO("TransportPathRepository: Inferred DBC path ", bestPathId,
                 " (allowZOnly=", allowZOnly ? "yes" : "no",
                 ") for spawn at (", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z,
                 "), dist=", std::sqrt(bestD2));
    }

    return bestPathId;
}

uint32_t TransportPathRepository::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return inferDbcPathForSpawn(spawnWorldPos, maxDistance, /*allowZOnly=*/false);
}

uint32_t TransportPathRepository::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    auto isUsableMovingPath = [this](uint32_t pathId) -> bool {
        auto* e = findPath(pathId);
        if (!e) return false;
        return e->fromDBC && !e->zOnly && e->spline.durationMs() > 0 && e->spline.keyCount() > 1;
    };

    // There was a table here remapping the five WotLK zeppelins onto paths
    // 193182 and 193183. Those are real TransportAnimation.dbc entries, but
    // they belong to two Cataclysm transports; none of 164871, 175080, 176495,
    // 181689 or 186238 appears in that file at all. So each zeppelin was
    // handed a stranger's loop, which is why the Undercity one flew its own
    // journey while the server flew the real one and a rider saw both.
    //
    // A zeppelin is a MO_TRANSPORT like the continent ships below, and gets
    // its route the same way: GO template data[0] into TaxiPathNode.dbc, via
    // the GO-query hook. Until that arrives it stays docked - the transport
    // the server is carrying the player on cannot be somewhere the server
    // does not think it is.

    if (displayId == 3831u) {
        static constexpr uint32_t kDeeprunTramCandidates[] = {
            176080u, 176081u, 176082u, 176083u, 176084u, 176085u
        };
        for (uint32_t id : kDeeprunTramCandidates) {
            if (id == entry && isUsableMovingPath(id)) return id;
        }
        for (uint32_t id : kDeeprunTramCandidates) {
            if (isUsableMovingPath(id)) {
                LOG_WARNING("TransportPathRepository: remapped Deeprun tram displayId 3831 entry ",
                            entry, " to DBC path ", id);
                return id;
            }
        }
    }

    // Fallback by display model family.
    const bool looksLikeShip = isOceanGoingTransportDisplay(displayId);
    if (looksLikeShip) {
        // Continent-crossing ships are server-driven MO_TRANSPORT objects: their
        // route comes from their taxi path (TaxiPathNode.dbc via GO template data[0]),
        // not from TransportAnimation.dbc. There is no correct TransportAnimation
        // fallback for them - borrowing any (previously the Deeprun Tram paths) sent
        // them underwater. Return 0 so the ship stays docked until the GO-query hook
        // assigns its taxi path; never fabricate a path from an unrelated route.
        return 0;
    }

    // Zeppelins used to fall back to the first usable id in a candidate list,
    // which is one route - so every zeppelin flew it. Three of them are already
    // remapped to 193182 above, and the display families cover five more ids, so
    // in practice the whole fleet traced the same line regardless of where it
    // spawned or where it was meant to go.
    //
    // The reasoning already written down for ships applies just as well here: a
    // transport whose route is not known stays docked. It is the same object the
    // server carries the player on, so an invented route does not merely look
    // wrong, it disagrees with where the server says the player is.
    // No last-resort "any moving path" either. Handing a transport an unrelated
    // route is what sailed the continent ships underwater, and picking the first
    // entry of an unordered map made it arbitrary as well as wrong.
    return 0;
}

// ── Path construction from waypoints ───────────────────────────

void TransportPathRepository::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    if (waypoints.empty()) {
        LOG_ERROR("TransportPathRepository: Cannot load empty path ", pathId);
        return;
    }

    bool isZOnly = false;  // Manually loaded paths are assumed to have XY movement

    // Helper: compute segment duration from distance and speed
    auto segMsFromDist = [&](float dist) -> uint32_t {
        if (speed <= 0.0f) return 1000;
        return static_cast<uint32_t>((dist / speed) * 1000.0f);
    };

    // Single point = stationary (durationMs = 0)
    if (waypoints.size() == 1) {
        std::vector<math::SplineKey> keys;
        keys.push_back({.timeMs = 0, .position = waypoints[0]});
        math::CatmullRomSpline spline(std::move(keys), false);
        // Runtime fallbacks may replace stale runtime/taxi copies left under the
        // same entry, but must never overwrite an authentic expansion DBC path.
        const auto* existing = findPath(pathId);
        if (!existing || !existing->fromDBC) {
            storePath(pathId, PathEntry(std::move(spline), pathId, isZOnly, false, false));
        }
        LOG_INFO("TransportPathRepository: Loaded stationary path ", pathId);
        return;
    }

    // Multiple points: calculate cumulative time based on distance and speed
    std::vector<math::SplineKey> keys;
    keys.reserve(waypoints.size() + (looping ? 1 : 0));
    uint32_t cumulativeMs = 0;
    keys.push_back({.timeMs = 0, .position = waypoints[0]});

    for (size_t i = 1; i < waypoints.size(); i++) {
        float dist = glm::distance(waypoints[i-1], waypoints[i]);
        cumulativeMs += glm::max(1u, segMsFromDist(dist));
        keys.push_back({.timeMs = cumulativeMs, .position = waypoints[i]});
    }

    // Add explicit wrap segment (last → first) for looping paths.
    // By duplicating the first point at the end with cumulative time, the path
    // becomes time-closed and CatmullRomSpline handles wrap via modular time
    // without requiring special-case index wrapping during evaluation.
    if (looping && waypoints.size() >= 2) {
        float wrapDist = glm::distance(waypoints.back(), waypoints.front());
        cumulativeMs += glm::max(1u, segMsFromDist(wrapDist));
        keys.push_back({.timeMs = cumulativeMs, .position = waypoints[0]});
    }

    math::CatmullRomSpline spline(std::move(keys), false);
    // Runtime fallbacks may replace stale runtime/taxi copies left under the
    // same entry, but must never overwrite an authentic expansion DBC path.
    const auto* existing = findPath(pathId);
    if (!existing || !existing->fromDBC) {
        storePath(pathId, PathEntry(std::move(spline), pathId, isZOnly, false, false));
    }

    auto* stored = findPath(pathId);
    LOG_INFO("TransportPathRepository: Loaded path ", pathId,
             " with ", waypoints.size(), " waypoints",
             (looping ? " + wrap segment" : ""),
             ", duration=", stored ? stored->spline.durationMs() : 0, "ms, speed=", speed);
}

// ── DBC: TransportAnimation ────────────────────────────────────

bool TransportPathRepository::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    LOG_INFO("Loading TransportAnimation.dbc...");

    if (!assetMgr) {
        LOG_ERROR("AssetManager is null");
        return false;
    }

    // Load DBC file
    auto dbcData = assetMgr->readFile("DBFilesClient\\TransportAnimation.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("TransportAnimation.dbc not found - transports will use fallback paths");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData)) {
        LOG_ERROR("Failed to parse TransportAnimation.dbc");
        return false;
    }

    LOG_INFO("TransportAnimation.dbc: ", dbc.getRecordCount(), " records, ",
             dbc.getFieldCount(), " fields per record");

    // Debug: dump first 3 records to see all field values
    for (uint32_t i = 0; i < std::min(3u, dbc.getRecordCount()); i++) {
        LOG_INFO("  DEBUG Record ", i, ": ",
                 " [0]=", dbc.getUInt32(i, 0),
                 " [1]=", dbc.getUInt32(i, 1),
                 " [2]=", dbc.getUInt32(i, 2),
                 " [3]=", dbc.getFloat(i, 3),
                 " [4]=", dbc.getFloat(i, 4),
                 " [5]=", dbc.getFloat(i, 5),
                 " [6]=", dbc.getUInt32(i, 6));
    }

    // Group waypoints by transportEntry
    std::map<uint32_t, std::vector<std::pair<uint32_t, glm::vec3>>> waypointsByTransport;

    for (uint32_t i = 0; i < dbc.getRecordCount(); i++) {
        // uint32_t id = dbc.getUInt32(i, 0);  // Not needed
        uint32_t transportEntry = dbc.getUInt32(i, 1);
        uint32_t timeIndex = dbc.getUInt32(i, 2);
        float posX = dbc.getFloat(i, 3);
        float posY = dbc.getFloat(i, 4);
        float posZ = dbc.getFloat(i, 5);
        // uint32_t sequenceId = dbc.getUInt32(i, 6);  // Not needed for basic paths

        // RAW FLOAT SANITY CHECK: Log first 10 records to see if DBC has real data
        if (i < 10) {
            uint32_t ux = dbc.getUInt32(i, 3);
            uint32_t uy = dbc.getUInt32(i, 4);
            uint32_t uz = dbc.getUInt32(i, 5);
            LOG_INFO("TA raw rec ", i,
                     " entry=", transportEntry,
                     " t=", timeIndex,
                     " raw=(", posX, ",", posY, ",", posZ, ")",
                     " u32=(", ux, ",", uy, ",", uz, ")");
        }

        // DIAGNOSTIC: Log ALL records for problematic ferries (20655, 20657, 149046)
        // AND first few records for known-good transports to verify DBC reading
        if (i < 5 || transportEntry == 2074 ||
            transportEntry == 20655 || transportEntry == 20657 || transportEntry == 149046) {
            LOG_INFO("RAW DBC [", i, "] entry=", transportEntry, " t=", timeIndex,
                     " raw=(", posX, ",", posY, ",", posZ, ")");
        }

        waypointsByTransport[transportEntry].emplace_back(timeIndex, glm::vec3(posX, posY, posZ));
    }

    // Create time-indexed paths from waypoints
    int pathsLoaded = 0;
    for (const auto& [transportEntry, waypoints] : waypointsByTransport) {
        if (waypoints.empty()) continue;

        // Sort by timeIndex
        auto sortedWaypoints = waypoints;
        std::sort(sortedWaypoints.begin(), sortedWaypoints.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // CRITICAL: Normalize timeIndex to start at 0 (DBC records don't start at 0!)
        // This makes evaluatePosition(0) valid and stabilizes basePosition seeding
        uint32_t t0 = sortedWaypoints.front().first;

        // Build SplineKey array with normalized time indices
        std::vector<math::SplineKey> keys;
        keys.reserve(sortedWaypoints.size() + 1);  // +1 for wrap point

        // Log DBC waypoints for tram entries
        if (transportEntry >= 176080 && transportEntry <= 176085) {
            size_t mid = sortedWaypoints.size() / 4;  // ~quarter through
            size_t mid2 = sortedWaypoints.size() / 2; // ~halfway
            LOG_DEBUG("DBC path entry=", transportEntry, " nPts=", sortedWaypoints.size(),
                       " [0] t=", sortedWaypoints[0].first, " raw=(", sortedWaypoints[0].second.x, ",", sortedWaypoints[0].second.y, ",", sortedWaypoints[0].second.z, ")",
                       " [", mid, "] t=", sortedWaypoints[mid].first, " raw=(", sortedWaypoints[mid].second.x, ",", sortedWaypoints[mid].second.y, ",", sortedWaypoints[mid].second.z, ")",
                       " [", mid2, "] t=", sortedWaypoints[mid2].first, " raw=(", sortedWaypoints[mid2].second.x, ",", sortedWaypoints[mid2].second.y, ",", sortedWaypoints[mid2].second.z, ")");
        }

        // Deeprun tram frame normalization (data-driven, not expansion-gated):
        // vanilla/TBC author the tram's local path along +X ([0, +2482]), which
        // the identity mapping below was tuned against. WotLK re-exported the
        // same paths along -Y ([-2482, 0], X≈0), which made the cars drive
        // perpendicular to the tunnel. Detect the travel axis from the data
        // extents and rotate the Y-major variant into the X-major frame.
        bool tramYMajor = false;
        if (isDeeprunTramPath(transportEntry)) {
            glm::vec3 mn = sortedWaypoints.front().second;
            glm::vec3 mx = mn;
            for (const auto& wp : sortedWaypoints) {
                mn = glm::min(mn, wp.second);
                mx = glm::max(mx, wp.second);
            }
            tramYMajor = (mx.y - mn.y) > (mx.x - mn.x);
            if (tramYMajor) {
                LOG_INFO("Tram entry ", transportEntry,
                         " uses Y-major local frame (WotLK export) - rotating to X-major");
            }
        }
        auto tramNormalize = [tramYMajor](const glm::vec3& p) {
            return tramYMajor ? glm::vec3(-p.y, p.x, p.z) : p;
        };

        for (size_t idx = 0; idx < sortedWaypoints.size(); idx++) {
            const auto& [tMs, rawPos] = sortedWaypoints[idx];
            const glm::vec3 pos = tramNormalize(rawPos);

            glm::vec3 canonical = transportAnimationOffsetToCanonical(transportEntry, pos);

            // Skip waypoints where serverToCanonical zeroes nonzero inputs
            if ((pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) &&
                (canonical.x == 0.0f && canonical.y == 0.0f && canonical.z == 0.0f)) {
                LOG_ERROR("serverToCanonical ZEROED - skipping waypoint! entry=", transportEntry,
                          " server=(", pos.x, ",", pos.y, ",", pos.z, ")",
                          " → canon=(", canonical.x, ",", canonical.y, ",", canonical.z, ")");
                continue;
            }

            // Debug waypoint conversion for first transport (entry 2074)
            if (transportEntry == 2074 && idx < 5) {
                LOG_INFO("COORD CONVERT: entry=", transportEntry, " t=", tMs,
                         " serverPos=(", pos.x, ", ", pos.y, ", ", pos.z, ")",
                         " → canonical=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ")");
            }

            // DIAGNOSTIC: Log ALL conversions for problematic ferries
            if (transportEntry == 20655 || transportEntry == 20657 || transportEntry == 149046) {
                LOG_INFO("CONVERT ", transportEntry, " t=", tMs,
                         " server=(", pos.x, ",", pos.y, ",", pos.z, ")",
                         " → canon=(", canonical.x, ",", canonical.y, ",", canonical.z, ")");
            }

            keys.push_back({.timeMs = tMs - t0, .position = canonical});  // Normalize: subtract first timeIndex
        }

        // Get base duration from last normalized timeIndex
        uint32_t lastTimeMs = sortedWaypoints.back().first - t0;

        // Calculate wrap duration (last → first segment)
        // Use average segment duration as wrap duration
        uint32_t totalDelta = 0;
        int segmentCount = 0;
        for (size_t i = 1; i < sortedWaypoints.size(); i++) {
            uint32_t delta = sortedWaypoints[i].first - sortedWaypoints[i-1].first;
            if (delta > 0) {
                totalDelta += delta;
                segmentCount++;
            }
        }
        uint32_t wrapMs = (segmentCount > 0) ? (totalDelta / segmentCount) : 1000;

        // Add duplicate first point at end with wrap duration
        // This makes the wrap segment (last → first) have proper duration
        const glm::vec3 fp = tramNormalize(sortedWaypoints.front().second);
        glm::vec3 firstCanonical = transportAnimationOffsetToCanonical(transportEntry, fp);
        keys.push_back({.timeMs = lastTimeMs + wrapMs, .position = firstCanonical});

        // Build the spline (time-closed=false because we added explicit wrap point)
        math::CatmullRomSpline spline(std::move(keys), false);

        // Detect Z-only paths (elevator/bobbing animation, not real XY travel)
        const auto& sk = spline.keys();
        float minX = sk[0].position.x, maxX = minX;
        float minY = sk[0].position.y, maxY = minY;
        float minZ = sk[0].position.z, maxZ = minZ;
        for (const auto& k : sk) {
            minX = std::min(minX, k.position.x); maxX = std::max(maxX, k.position.x);
            minY = std::min(minY, k.position.y); maxY = std::max(maxY, k.position.y);
            minZ = std::min(minZ, k.position.z); maxZ = std::max(maxZ, k.position.z);
        }
        float rangeX = maxX - minX;
        float rangeY = maxY - minY;
        float rangeZ = maxZ - minZ;
        float rangeXY = std::max(rangeX, rangeY);
        // Some elevator paths have tiny XY jitter. Treat them as z-only when horizontal travel
        // is negligible compared to vertical motion.
        bool isZOnly = (rangeXY < 0.01f) || (rangeXY < 1.0f && rangeZ > 2.0f);

        // Log first, middle, and last points to verify path data
        glm::vec3 firstOffset = sk[0].position;
        size_t midIdx = sk.size() / 2;
        glm::vec3 midOffset = sk[midIdx].position;
        glm::vec3 lastOffset = sk[sk.size() - 2].position;  // -2 to skip wrap duplicate
        uint32_t durationMs = spline.durationMs();
        LOG_INFO("  Transport ", transportEntry, ": ", sk.size() - 1, " waypoints + wrap, ",
                 durationMs, "ms duration (wrap=", wrapMs, "ms, t0_normalized=", sk[0].timeMs, "ms)",
                 " rangeXY=(", rangeX, ",", rangeY, ") rangeZ=", rangeZ, " ",
                 (isZOnly ? "[Z-ONLY]" : "[XY-PATH]"),
                 " firstOffset=(", firstOffset.x, ", ", firstOffset.y, ", ", firstOffset.z, ")",
                 " midOffset=(", midOffset.x, ", ", midOffset.y, ", ", midOffset.z, ")",
                 " lastOffset=(", lastOffset.x, ", ", lastOffset.y, ", ", lastOffset.z, ")");

        // Store path
        paths_.emplace(transportEntry, PathEntry(std::move(spline), transportEntry, isZOnly, true, false));
        pathsLoaded++;
    }

    LOG_INFO("Loaded ", pathsLoaded, " transport paths from TransportAnimation.dbc");
    return pathsLoaded > 0;
}

// ── DBC: TaxiPathNode ──────────────────────────────────────────

bool TransportPathRepository::taxiRouteIsCircuit(const TaxiRoute& route) {
    const auto& nodes = route.nodes;
    if (nodes.size() < 3) return false;
    // Ends on a different map than it started: the closing leg is the
    // server's teleport home, so the route is a ring by construction.
    if (nodes.front().mapId != nodes.back().mapId) return true;

    float length = 0.0f;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        if (nodes[i].mapId != nodes[i + 1].mapId) continue;
        length += glm::distance(nodes[i].position, nodes[i + 1].position);
    }
    const float endGap = glm::distance(nodes.front().position, nodes.back().position);
    // A quarter of the run, with a floor for the short routes where the ratio
    // is noise. 118 on 787 is a circuit; a pier-to-open-water shuttle has its
    // ends the whole way apart.
    return endGap < std::max(60.0f, length * 0.25f);
}

namespace {

/// The order a hull visits a route's nodes in, over one cycle.
///
/// A ring is flown once round. An open run is flown out and back, so every
/// node but the two ends is visited twice and the cycle is position-closed.
std::vector<size_t> taxiVisitOrder(const TaxiRoute& route) {
    std::vector<size_t> order;
    const size_t n = route.nodes.size();
    order.reserve(route.circuit ? n : n * 2);
    for (size_t i = 0; i < n; ++i) order.push_back(i);
    if (!route.circuit && n > 2) {
        for (size_t i = n - 1; i-- > 1; ) order.push_back(i);
    }
    return order;
}

uint32_t taxiLegMs(float dist, float speed) {
    if (speed <= 0.0f) return 1000u;
    return std::max<uint32_t>(100u, static_cast<uint32_t>(dist / speed * 1000.0f));
}

}  // namespace

float TransportPathRepository::taxiRouteSpeedFor(const TaxiRoute& route, uint32_t periodMs) {
    if (periodMs == 0 || route.cycleDistance <= 0.0f) return 0.0f;
    // The stops are authored in seconds and do not scale; only the flying does.
    if (periodMs <= route.cycleDwellMs) return 0.0f;
    const float flyingSeconds = static_cast<float>(periodMs - route.cycleDwellMs) / 1000.0f;
    if (flyingSeconds <= 0.0f) return 0.0f;
    const float speed = route.cycleDistance / flyingSeconds;
    // A speed outside this range means the period and the route do not
    // describe the same journey; better to keep the default than to believe it.
    if (!(speed > 1.0f) || speed > 200.0f) return 0.0f;
    return speed;
}

PathEntry TransportPathRepository::buildTaxiRouteSlice(const TaxiRoute& route,
                                                      uint32_t mapId,
                                                      float speed) {
    const std::vector<size_t> order = taxiVisitOrder(route);

    // Walk the whole route once, on every map, so every slice shares a clock.
    struct Visit { size_t node; uint32_t arriveMs; uint32_t departMs; };
    std::vector<Visit> visits;
    visits.reserve(order.size());
    uint32_t t = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const TaxiRouteNode& nd = route.nodes[order[k]];
        const uint32_t arrive = t;
        t += nd.dwellMs;
        visits.push_back({order[k], arrive, t});
        const TaxiRouteNode& nxt = route.nodes[order[(k + 1) % order.size()]];
        // A map change costs no time. The server teleports the hull across and
        // the rider gets a loading screen; there is no crossing to animate.
        if (nxt.mapId == nd.mapId) {
            t += taxiLegMs(glm::distance(nd.position, nxt.position), speed);
        }
    }
    const uint32_t cycleMs = std::max(1u, t);

    // This map's runs through that timeline.
    struct Stop { uint32_t arriveMs; uint32_t departMs; glm::vec3 pos; };
    std::vector<std::vector<Stop>> runs;
    bool inRun = false;
    for (const Visit& v : visits) {
        const TaxiRouteNode& nd = route.nodes[v.node];
        if (nd.mapId != mapId) { inRun = false; continue; }
        if (!inRun) { runs.emplace_back(); inRun = true; }
        runs.back().push_back({v.arriveMs, v.departMs, nd.position});
    }

    if (runs.empty()) {
        return PathEntry(math::CatmullRomSpline({}, false), route.nodes.empty() ? 0u : mapId,
                         false, false, true);
    }

    const bool wrapsHere = route.nodes[order.front()].mapId == mapId &&
                           route.nodes[order.back()].mapId == mapId;

    std::vector<math::SplineKey> keys;
    std::vector<std::pair<uint32_t, uint32_t>> presence;
    keys.reserve(visits.size() * 2 + 4);

    // Before the hull first arrives it is on another map. It waits at the
    // position it will arrive at, rather than being swept there: an
    // interpolation across the gap is a hull crossing Tirisfal in one frame,
    // and a tangent taken off that leg points the bow anywhere at all.
    const uint32_t firstArrive = runs.front().front().arriveMs;
    if (firstArrive >= 2) {
        keys.push_back({0u, runs.front().front().pos});
        keys.push_back({firstArrive - 1u, runs.front().front().pos});
    }

    for (size_t r = 0; r < runs.size(); ++r) {
        if (r > 0) {
            // Between two visits to this map, hold at the node just left until
            // halfway and at the node about to be reached after - so the
            // teleport falls in the middle of the gap, where the hull is
            // hidden, and both ends of every run are continuous.
            const uint32_t gapFrom = runs[r - 1].back().departMs;
            const uint32_t gapTo = runs[r].front().arriveMs;
            if (gapTo > gapFrom + 3) {
                const uint32_t mid = gapFrom + (gapTo - gapFrom) / 2;
                keys.push_back({gapFrom + 1u, runs[r - 1].back().pos});
                keys.push_back({mid, runs[r - 1].back().pos});
                keys.push_back({mid + 1u, runs[r].front().pos});
                keys.push_back({gapTo - 1u, runs[r].front().pos});
            }
        }
        for (const Stop& stop : runs[r]) {
            keys.push_back({stop.arriveMs, stop.pos});
            // A repeated position is how CatmullRomSpline holds an exact stop.
            if (stop.departMs > stop.arriveMs) {
                keys.push_back({stop.departMs, stop.pos});
            }
        }
        presence.emplace_back(runs[r].front().arriveMs, runs[r].back().departMs);
    }

    if (wrapsHere) {
        // The closing leg is flown here too, back to the first node.
        if (cycleMs > keys.back().timeMs) {
            keys.push_back({cycleMs, route.nodes[order.front()].position});
        }
        presence.back().second = cycleMs;
    } else if (keys.back().timeMs < cycleMs) {
        // And it waits where it left, until the cycle wraps round again.
        keys.push_back({cycleMs, runs.back().back().pos});
    }

    PathEntry entry(math::CatmullRomSpline(std::move(keys), false), 0u, false, false, true);
    for (const Visit& v : visits) {
        if (v.departMs > v.arriveMs) {
            entry.routeOriginMs = v.arriveMs;
            break;
        }
    }
    // A route that never leaves this map is always present; saying so with an
    // empty list keeps presentAt free for every path that is not a ferry.
    if (!(presence.size() == 1 && presence.front().first == 0 &&
          presence.front().second >= cycleMs)) {
        entry.presentMs = std::move(presence);

        // The heading at each end of a run on this map, level, from the first
        // two distinct points there. Where the route closes on this map its
        // two ends are one continuous leg and not an edge at all.
        const auto heading = [](const std::vector<Stop>& run, bool atEnd, glm::vec3& out) {
            const size_t n = run.size();
            for (size_t i = 1; i < n; ++i) {
                glm::vec3 d = atEnd ? run[n - 1].pos - run[n - 1 - i].pos
                                    : run[i].pos - run[0].pos;
                d.z = 0.0f;
                const float len = glm::length(d);
                if (len > 0.01f) { out = d / len; return true; }
            }
            return false;
        };
        entry.edgeSpeed = speed;
        for (size_t r = 0; r < runs.size(); ++r) {
            glm::vec3 dir(0.0f);
            if (!(wrapsHere && r == 0) && heading(runs[r], false, dir)) {
                entry.arrivals.push_back({runs[r].front().arriveMs, runs[r].front().pos, dir});
            }
            if (!(wrapsHere && r + 1 == runs.size()) && heading(runs[r], true, dir)) {
                entry.departures.push_back({runs[r].back().departMs, runs[r].back().pos, dir});
            }
        }
    }
    return entry;
}

namespace {

/// A Catmull-Rom curve through one run of a route, with a virtual point before
/// the first node and the last node repeated, as the server builds it. Its
/// lengths are measured in three straight steps per segment: they feed the
/// timing, so a more accurate length would be a different period.
struct ServerSpline {
    std::vector<glm::vec3> points;
    std::vector<double> lengths;

    explicit ServerSpline(const std::vector<glm::vec3>& controls) {
        const size_t n = controls.size();
        points.reserve(n + 2);
        points.push_back(controls[0] + (controls[1] - controls[0]) * -1.0f);
        points.insert(points.end(), controls.begin(), controls.end());
        points.push_back(controls[n - 1]);
        lengths.assign(n + 1, 0.0);
        double total = 0.0;
        for (size_t i = 1; i < n; ++i) {
            glm::vec3 current = points[i];
            float segment = 0.0f;
            double sum = 0.0;
            for (int step = 1; step <= 3; ++step) {
                const glm::vec3 next = at(i, static_cast<float>(step) / 3.0f);
                sum += glm::length(next - current);
                current = next;
            }
            segment = static_cast<float>(sum);
            total += segment;
            lengths[i + 1] = total;
        }
    }

    [[nodiscard]] glm::vec3 at(size_t index, float t) const {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return points[index - 1] * (-0.5f * t3 + t2 - 0.5f * t) +
               points[index] * (1.5f * t3 - 2.5f * t2 + 1.0f) +
               points[index + 1] * (-1.5f * t3 + 2.0f * t2 + 0.5f * t) +
               points[index + 2] * (0.5f * t3 - 0.5f * t2);
    }
};

struct ServerFrame {
    const TaxiRouteNode* node = nullptr;
    bool teleport = false;
    float distFromPrev = 0.0f;
    float nextDistFromPrev = 0.0f;
    float distSinceStop = 0.0f;
    float distUntilStop = 0.0f;
    float timeFrom = 0.0f;
    float timeTo = 0.0f;
    uint32_t arriveMs = 0;
    uint32_t departMs = 0;
    uint32_t nextArriveMs = 0;
    size_t splineIndex = 0;
    int spline = -1;

    [[nodiscard]] bool isStop() const { return node->actionFlags == 2u; }
};

struct ServerTimeline {
    std::vector<ServerFrame> frames;
    std::vector<ServerSpline> splines;
    float speed = 0.0f;
    float accel = 0.0f;
    float accelTime = 0.0f;
    float accelDist = 0.0f;
    uint32_t periodMs = 0;

    /// The hull's position on a moving frame.
    [[nodiscard]] glm::vec3 positionAt(const ServerFrame& frame, uint32_t timerMs) const {
        const float now = static_cast<float>(timerMs) * 0.001f;
        const float depart = static_cast<float>(frame.departMs) * 0.001f;
        const float sinceStop = frame.timeFrom + (now - depart);
        const float untilStop = frame.timeTo - (now - depart);
        float segmentPos = 0.0f;
        if (sinceStop < untilStop) {
            const float dist = sinceStop < accelTime
                ? 0.5f * accel * sinceStop * sinceStop
                : accelDist + (sinceStop - accelTime) * speed;
            segmentPos = dist - frame.distSinceStop;
        } else {
            const float dist = untilStop < accelTime
                ? 0.5f * accel * untilStop * untilStop
                : accelDist + (untilStop - accelTime) * speed;
            segmentPos = frame.distUntilStop - dist;
        }
        const float u = std::clamp(segmentPos / frame.nextDistFromPrev, 0.0f, 1.0f);
        return splines[static_cast<size_t>(frame.spline)].at(frame.splineIndex, u);
    }

    /// How fast the hull is going as it passes a frame.
    [[nodiscard]] float speedAt(const ServerFrame& frame) const {
        const float fromStop = std::min(frame.distSinceStop, frame.distUntilStop);
        return std::min(speed, std::sqrt(2.0f * accel * std::max(fromStop, 0.0f)));
    }
};

/// TransportMgr::GeneratePath, step for step.
std::optional<ServerTimeline> buildServerTimeline(const TaxiRoute& route, float speed, float accel) {
    if (speed <= 0.0f || accel <= 0.0f || route.nodes.size() < 4) return std::nullopt;

    ServerTimeline line;
    line.speed = speed;
    line.accel = accel;
    line.accelTime = speed / accel;
    line.accelDist = 0.5f * speed * speed / accel;

    // The node before a map change and the one after it are never flown: the
    // last node kept teleports straight to the one after those.
    auto& frames = line.frames;
    bool mapChange = false;
    for (size_t i = 0; i < route.nodes.size(); ++i) {
        if (mapChange) { mapChange = false; continue; }
        const TaxiRouteNode& node = route.nodes[i];
        if (i + 1 != route.nodes.size() &&
            ((node.actionFlags & 1u) != 0u || node.mapId != route.nodes[i + 1].mapId)) {
            if (frames.empty()) return std::nullopt;
            frames.back().teleport = true;
            mapChange = true;
        } else {
            ServerFrame frame;
            frame.node = &node;
            frames.push_back(frame);
        }
    }
    if (frames.size() < 4) return std::nullopt;
    frames.erase(frames.begin());
    frames.pop_back();
    frames.back().teleport = true;

    std::vector<glm::vec3> points;
    points.reserve(frames.size());
    for (const ServerFrame& frame : frames) points.push_back(frame.node->position);

    int firstStop = frames[0].isStop() ? 0 : -1;
    int lastStop = firstStop;
    frames[0].splineIndex = 1;
    line.splines.reserve(frames.size());

    size_t start = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i - 1].teleport || i + 1 == frames.size()) {
            const size_t extra = frames[i - 1].teleport ? 0u : 1u;
            if (i + extra - start < 2) return std::nullopt;   // no curve through one point
            line.splines.emplace_back(std::vector<glm::vec3>(
                points.begin() + static_cast<std::ptrdiff_t>(start),
                points.begin() + static_cast<std::ptrdiff_t>(i + extra)));
            const int spline = static_cast<int>(line.splines.size()) - 1;
            const ServerSpline& curve = line.splines.back();
            for (size_t j = start; j < i + extra; ++j) {
                frames[j].splineIndex = j - start + 1;
                frames[j].distFromPrev = static_cast<float>(
                    curve.lengths[j + 1 - start] - curve.lengths[j - start]);
                if (j > 0) frames[j - 1].nextDistFromPrev = frames[j].distFromPrev;
                frames[j].spline = spline;
            }
            if (frames[i - 1].teleport) {
                frames[i].splineIndex = i - start + 1;
                frames[i].distFromPrev = 0.0f;
                frames[i - 1].nextDistFromPrev = 0.0f;
                frames[i].spline = spline;
            }
            start = i;
        }
        if (frames[i].isStop()) {
            if (firstStop == -1) firstStop = static_cast<int>(i);
            lastStop = static_cast<int>(i);
        }
    }
    frames.back().nextDistFromPrev = frames.front().distFromPrev;
    if (firstStop == -1 || lastStop == -1) firstStop = lastStop = 0;

    const size_t n = frames.size();
    float tmp = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + static_cast<size_t>(lastStop)) % n;
        if (frames[j].isStop() || j == static_cast<size_t>(lastStop)) tmp = 0.0f;
        else tmp += frames[j].distFromPrev;
        frames[j].distSinceStop = tmp;
    }
    tmp = 0.0f;
    for (size_t i = n; i-- > 0; ) {
        const size_t j = (i + static_cast<size_t>(firstStop)) % n;
        tmp += frames[(j + 1) % n].distFromPrev;
        frames[j].distUntilStop = tmp;
        if (frames[j].isStop() || j == static_cast<size_t>(firstStop)) tmp = 0.0f;
    }

    for (ServerFrame& f : frames) {
        const float total = f.distSinceStop + f.distUntilStop;
        if (total < 2.0f * line.accelDist) {
            if (f.distSinceStop < f.distUntilStop) {
                f.timeTo = 2.0f * std::sqrt(total / accel) - std::sqrt(2.0f * f.distSinceStop / accel);
            } else {
                f.timeTo = std::sqrt(2.0f * f.distUntilStop / accel);
            }
        } else if (f.distSinceStop < line.accelDist) {
            f.timeTo = total / speed + speed / accel - std::sqrt(2.0f * f.distSinceStop / accel);
        } else if (f.distUntilStop < line.accelDist) {
            f.timeTo = std::sqrt(2.0f * f.distUntilStop / accel);
        } else {
            f.timeTo = f.distUntilStop / speed + 0.5f * speed / accel;
        }
    }

    float segmentTime = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + static_cast<size_t>(lastStop)) % n;
        if (frames[j].isStop() || j == static_cast<size_t>(lastStop)) segmentTime = frames[j].timeTo;
        frames[j].timeFrom = segmentTime - frames[j].timeTo;
    }

    const auto delaySeconds = [](const ServerFrame& f) {
        return static_cast<float>(f.node->dwellMs / 1000u);
    };
    float pathTime = 0.0f;
    if (frames[0].isStop()) {
        pathTime = delaySeconds(frames[0]);
        frames[0].departMs = static_cast<uint32_t>(pathTime * 1000.0f);
    }
    for (size_t i = 1; i < n; ++i) {
        pathTime += frames[i - 1].timeTo;
        if (frames[i].isStop()) {
            frames[i].arriveMs = static_cast<uint32_t>(pathTime * 1000.0f);
            frames[i - 1].nextArriveMs = frames[i].arriveMs;
            pathTime += delaySeconds(frames[i]);
            frames[i].departMs = static_cast<uint32_t>(pathTime * 1000.0f);
        } else {
            pathTime -= frames[i].timeTo;
            frames[i].arriveMs = static_cast<uint32_t>(pathTime * 1000.0f);
            frames[i - 1].nextArriveMs = frames[i].arriveMs;
            frames[i].departMs = frames[i].arriveMs;
        }
    }
    frames.back().nextArriveMs = frames.back().departMs;
    line.periodMs = frames.back().departMs;
    return line;
}

}  // namespace

std::optional<PathEntry> TransportPathRepository::buildServerRouteSlice(
    const TaxiRoute& route, uint32_t mapId, float moveSpeed, float accelRate,
    uint32_t publishedPeriodMs) {
    const auto timeline = buildServerTimeline(route, moveSpeed, accelRate);
    if (!timeline) return std::nullopt;
    // Rounding in the server's float arithmetic can differ by a millisecond.
    constexpr uint32_t kPeriodToleranceMs = 3u;
    const uint32_t period = timeline->periodMs;
    const uint32_t difference = period > publishedPeriodMs ? period - publishedPeriodMs
                                                           : publishedPeriodMs - period;
    if (period == 0 || difference > kPeriodToleranceMs) return std::nullopt;

    const auto& frames = timeline->frames;
    struct Run { size_t first; size_t last; };
    std::vector<Run> runs;
    size_t runStart = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].teleport) continue;
        if (frames[runStart].node->mapId == mapId && i > runStart) runs.push_back({runStart, i});
        runStart = i + 1;
    }
    if (runs.empty()) return std::nullopt;

    std::vector<math::SplineKey> keys;
    keys.reserve(frames.size() * 40u);
    const auto addKey = [&keys](uint32_t timeMs, const glm::vec3& position) {
        if (keys.empty() || timeMs > keys.back().timeMs) keys.push_back({timeMs, position});
    };
    std::vector<std::pair<uint32_t, uint32_t>> presence;

    // Dense enough that the curve between two keys is the server's. Keys are
    // also kept apart in space: two nearly equal positions are how a stop is
    // written on this spline, and a hull creeping out of a dock would read as
    // a string of little stops.
    constexpr uint32_t kSampleMs = 250u;
    constexpr float kMinKeySpacing = 0.5f;

    for (size_t r = 0; r < runs.size(); ++r) {
        const ServerFrame& first = frames[runs[r].first];
        const uint32_t arrive = first.arriveMs;
        if (keys.empty()) {
            if (arrive > 0) addKey(0u, first.node->position);
        } else {
            // Between two runs on this map: hold at the last node to halfway
            // and at the next one after it, so that neither end of a run has
            // a curve reaching across the gap.
            const uint32_t from = keys.back().timeMs;
            const glm::vec3 held = keys.back().position;
            const uint32_t to = std::max(arrive, from + 3u);
            const uint32_t mid = from + (to - from) / 2u;
            addKey(from + 1u, held);
            addKey(mid, held);
            addKey(mid + 1u, first.node->position);
            addKey(to, first.node->position);
        }

        for (size_t i = runs[r].first; i <= runs[r].last; ++i) {
            const ServerFrame& frame = frames[i];
            if (keys.empty() || glm::distance(keys.back().position, frame.node->position) >= kMinKeySpacing ||
                frame.isStop() || i == runs[r].first || i == runs[r].last) {
                addKey(frame.arriveMs, frame.node->position);
            }
            if (frame.departMs > frame.arriveMs) addKey(frame.departMs, frame.node->position);
            if (i == runs[r].last || frame.nextDistFromPrev <= 0.0f) continue;

            const ServerFrame& next = frames[i + 1];
            const bool endsHere = next.isStop() || i + 1 == runs[r].last;
            for (uint32_t t = frame.departMs + kSampleMs; t < frame.nextArriveMs; t += kSampleMs) {
                const glm::vec3 p = timeline->positionAt(frame, t);
                if (glm::distance(keys.back().position, p) < kMinKeySpacing) continue;
                if (endsHere && glm::distance(next.node->position, p) < kMinKeySpacing) continue;
                addKey(t, p);
            }
        }

        const uint32_t leave = frames[runs[r].last].arriveMs;
        if (!presence.empty() && presence.back().second == arrive) presence.back().second = leave;
        else presence.emplace_back(arrive, leave);
    }
    if (keys.back().timeMs < period) addKey(period, keys.back().position);

    PathEntry entry(math::CatmullRomSpline(std::move(keys), false), 0u, false, false, true);
    entry.serverTimeline = true;
    if (presence.size() == 1 && presence.front().first == 0 && presence.front().second >= period) {
        return entry;   // Never leaves this map.
    }
    entry.presentMs = std::move(presence);
    entry.edgeSpeed = moveSpeed;

    // The hull's heading and speed where it crosses each edge of a span here,
    // so what is drawn past the edge carries straight on from it.
    const auto level = [](glm::vec3 d, glm::vec3& out) {
        d.z = 0.0f;
        const float length = glm::length(d);
        if (length <= 0.001f) return false;
        out = d / length;
        return true;
    };
    for (size_t r = 0; r < runs.size(); ++r) {
        const ServerFrame& first = frames[runs[r].first];
        const ServerFrame& beforeLast = frames[runs[r].last - 1];
        const ServerFrame& last = frames[runs[r].last];
        glm::vec3 direction(0.0f);
        const bool joinsPrevious = r > 0 && frames[runs[r - 1].last].arriveMs == first.arriveMs;
        const bool joinsNext = r + 1 < runs.size() && frames[runs[r + 1].first].arriveMs == last.arriveMs;
        if (!joinsPrevious && first.spline >= 0) {
            const ServerSpline& curve = timeline->splines[static_cast<size_t>(first.spline)];
            if (level(curve.at(first.splineIndex, 0.05f) - curve.at(first.splineIndex, 0.0f), direction)) {
                entry.arrivals.push_back({first.arriveMs, first.node->position, direction,
                                          timeline->speedAt(first)});
            }
        }
        if (!joinsNext && beforeLast.spline >= 0) {
            const ServerSpline& curve = timeline->splines[static_cast<size_t>(beforeLast.spline)];
            if (level(curve.at(beforeLast.splineIndex, 1.0f) - curve.at(beforeLast.splineIndex, 0.95f), direction)) {
                entry.departures.push_back({last.arriveMs, last.node->position, direction,
                                            timeline->speedAt(last)});
            }
        }
    }
    return entry;
}

const TaxiRoute* TransportPathRepository::findTaxiRoute(uint32_t taxiPathId) const {
    auto it = taxiRoutes_.find(taxiPathId);
    return it != taxiRoutes_.end() ? &it->second : nullptr;
}

bool TransportPathRepository::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    LOG_INFO("Loading TaxiPathNode.dbc...");

    if (!assetMgr) {
        LOG_ERROR("AssetManager is null");
        return false;
    }

    auto dbcData = assetMgr->readFile("DBFilesClient\\TaxiPathNode.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("TaxiPathNode.dbc not found - MO_TRANSPORT will use fallback paths");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData)) {
        LOG_ERROR("Failed to parse TaxiPathNode.dbc");
        return false;
    }

    LOG_INFO("TaxiPathNode.dbc: ", dbc.getRecordCount(), " records, ",
             dbc.getFieldCount(), " fields per record");

    // Grouped by PathID alone, with each node's map kept on it.
    //
    // These used to be split per (PathID, MapID) and each slice timed on its
    // own, stretched to the whole route's length with the surplus spent at the
    // pier. That gave every map its own clock: the Undercity zeppelin flew a
    // circuit of its tower and waited, while the server flew it to Howling
    // Fjord and back. A route is one journey with one clock, and the server
    // publishes a phase against it - so it is built whole here and each map
    // takes its own view of it, sharing the cycle.
    struct TaxiNode {
        uint32_t nodeIndex;
        uint32_t mapId;
        float x, y, z;
        uint32_t delaySeconds;
        uint32_t actionFlags;
    };
    std::map<uint32_t, std::vector<TaxiNode>> nodesByPath;

    for (uint32_t i = 0; i < dbc.getRecordCount(); i++) {
        uint32_t pathId = dbc.getUInt32(i, 1);    // PathID
        uint32_t nodeIdx = dbc.getUInt32(i, 2);   // NodeIndex
        uint32_t mapId = dbc.getUInt32(i, 3);     // MapID
        float posX = dbc.getFloat(i, 4);          // X (server coords)
        float posY = dbc.getFloat(i, 5);          // Y (server coords)
        float posZ = dbc.getFloat(i, 6);          // Z (server coords)
        uint32_t actionFlags = dbc.getUInt32(i, 7);  // 1 = teleport, 2 = stop
        uint32_t delaySeconds = dbc.getUInt32(i, 8); // Dock dwell time

        nodesByPath[pathId].push_back({.nodeIndex = nodeIdx, .mapId = mapId,
                                       .x = posX, .y = posY, .z = posZ,
                                       .delaySeconds = delaySeconds,
                                       .actionFlags = actionFlags});
    }

    int pathsLoaded = 0;
    for (auto& [pathId, nodes] : nodesByPath) {
        if (nodes.size() < 2) continue;
        std::sort(nodes.begin(), nodes.end(),
                  [](const TaxiNode& a, const TaxiNode& b) { return a.nodeIndex < b.nodeIndex; });

        TaxiRoute route;
        route.nodes.reserve(nodes.size());
        for (const auto& node : nodes) {
            route.nodes.push_back({
                .position = core::coords::serverToCanonical(glm::vec3(node.x, node.y, node.z)),
                .mapId = node.mapId,
                .dwellMs = node.delaySeconds * 1000u,
                .actionFlags = node.actionFlags});
        }
        route.circuit = taxiRouteIsCircuit(route);

        // What one cycle costs, in the two currencies that scale differently:
        // distance flown, which a speed converts to time, and time stopped,
        // which no speed touches.
        const std::vector<size_t> order = taxiVisitOrder(route);
        for (size_t k = 0; k < order.size(); ++k) {
            const TaxiRouteNode& nd = route.nodes[order[k]];
            route.cycleDwellMs += nd.dwellMs;
            const TaxiRouteNode& nxt = route.nodes[order[(k + 1) % order.size()]];
            if (nxt.mapId == nd.mapId) {
                route.cycleDistance += glm::distance(nd.position, nxt.position);
            }
        }

        std::set<uint32_t> maps;
        for (const auto& node : route.nodes) maps.insert(node.mapId);
        for (uint32_t mapId : maps) {
            PathEntry slice = buildTaxiRouteSlice(route, mapId);
            if (slice.spline.keyCount() < 2) continue;
            // TaxiPathNode is a separate, per-map route source. Do not label it
            // as TransportAnimation DBC data: copied taxi slices live
            // temporarily in paths_ for the active transport, and fromDBC=true
            // made later inference offer Bravery's cached route to unrelated
            // icebreakers after zoning.
            slice.pathId = pathId;
            taxiPaths_[pathId].emplace(mapId, std::move(slice));
            pathsLoaded++;
        }
        taxiRoutes_[pathId] = std::move(route);
    }

    LOG_INFO("Loaded ", taxiRoutes_.size(), " TaxiPathNode routes as ", pathsLoaded,
             " per-map slices, each sharing its route's cycle");
    return pathsLoaded > 0;
}

} // namespace wowee::game
