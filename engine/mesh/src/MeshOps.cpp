#include "lucent/mesh/MeshOps.h"
#include "lucent/core/Log.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <cmath>

namespace lucent::mesh {
namespace MeshOps {
namespace {

std::vector<VertexID> CollectFaceVertices(const EditableMesh& mesh, FaceID fid) {
    std::vector<VertexID> verts;
    mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
        verts.push_back(loop.vertex);
    });
    return verts;
}

std::vector<LoopID> CollectFaceLoops(const EditableMesh& mesh, FaceID fid) {
    std::vector<LoopID> loops;
    mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
        loops.push_back(loop.id);
    });
    return loops;
}

EdgeID FindEdgeBetween(const EditableMesh& mesh, VertexID v0, VertexID v1) {
    auto edges = mesh.GetVertexEdges(v0);
    for (EdgeID eid : edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        if ((e->v0 == v0 && e->v1 == v1) || (e->v0 == v1 && e->v1 == v0)) {
            return eid;
        }
    }
    return INVALID_ID;
}

void CleanupOrphanEdges(EditableMesh& mesh) {
    std::vector<EdgeID> toRemove;
    for (const auto& e : mesh.GetEdges()) {
        if (e.id == INVALID_ID) continue;
        if (e.loop0 == INVALID_ID && e.loop1 == INVALID_ID) {
            toRemove.push_back(e.id);
        }
    }
    for (EdgeID eid : toRemove) {
        mesh.RemoveEdge(eid);
    }
}

bool FaceHasEdge(const EditableMesh& mesh, FaceID fid, EdgeID eid, size_t& loopIndex) {
    auto loops = CollectFaceLoops(mesh, fid);
    for (size_t i = 0; i < loops.size(); ++i) {
        const EMLoop* loop = mesh.GetLoop(loops[i]);
        if (!loop) continue;
        if (loop->edge == eid) {
            loopIndex = i;
            return true;
        }
    }
    return false;
}

std::vector<VertexID> BuildPathAvoidingEdge(const std::vector<VertexID>& verts, VertexID start, VertexID end) {
    std::vector<VertexID> path;
    const size_t n = verts.size();
    if (n < 2) return path;
    
    size_t startIdx = n;
    size_t endIdx = n;
    for (size_t i = 0; i < n; ++i) {
        if (verts[i] == start) startIdx = i;
        if (verts[i] == end) endIdx = i;
    }
    if (startIdx == n || endIdx == n) return path;
    
    bool forwardIsEdge = ((startIdx + 1) % n) == endIdx;
    int dir = forwardIsEdge ? -1 : 1;
    
    size_t idx = startIdx;
    while (true) {
        path.push_back(verts[idx]);
        if (verts[idx] == end) break;
        idx = (idx + n + dir) % n;
    }
    
    return path;
}

std::vector<VertexID> BuildSegmentVertices(
    EditableMesh& mesh,
    VertexID startId,
    VertexID endId,
    int segments
) {
    std::vector<VertexID> result;
    if (segments < 1) segments = 1;
    const EMVertex* startV = mesh.GetVertex(startId);
    const EMVertex* endV = mesh.GetVertex(endId);
    if (!startV || !endV) return result;
    
    result.reserve(static_cast<size_t>(segments + 1));
    result.push_back(startId);
    for (int s = 1; s < segments; ++s) {
        float t = static_cast<float>(s) / static_cast<float>(segments);
        glm::vec3 pos = glm::mix(startV->position, endV->position, t);
        VertexID vid = mesh.AddVertex(pos);
        EMVertex* v = mesh.GetVertex(vid);
        if (v) {
            v->uv = glm::mix(startV->uv, endV->uv, t);
        }
        result.push_back(vid);
    }
    result.push_back(endId);
    return result;
}

} // namespace

// ============================================================================
// Utility Operations
// ============================================================================

namespace {

struct WeldGridKey {
    int64_t x = 0;
    int64_t y = 0;
    int64_t z = 0;
    bool operator==(const WeldGridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct WeldGridKeyHash {
    size_t operator()(const WeldGridKey& k) const noexcept {
        auto mix = [](uint64_t v) {
            v ^= v >> 33;
            v *= 0xff51afd7ed558ccdULL;
            v ^= v >> 33;
            v *= 0xc4ceb9fe1a85ec53ULL;
            v ^= v >> 33;
            return v;
        };
        uint64_t hx = mix(static_cast<uint64_t>(k.x));
        uint64_t hy = mix(static_cast<uint64_t>(k.y));
        uint64_t hz = mix(static_cast<uint64_t>(k.z));
        return static_cast<size_t>(hx ^ (hy << 1) ^ (hz << 2));
    }
};

static WeldGridKey WeldQuantize(const glm::vec3& p, float cell) {
    const float inv = 1.0f / cell;
    return WeldGridKey{
        static_cast<int64_t>(std::floor(p.x * inv)),
        static_cast<int64_t>(std::floor(p.y * inv)),
        static_cast<int64_t>(std::floor(p.z * inv))
    };
}

} // namespace

void WeldVerticesByDistance(EditableMesh& mesh, float distance) {
    if (distance <= 0.0f) return;

    std::vector<VertexID> vids;
    vids.reserve(mesh.VertexCount());
    for (const auto& v : mesh.GetVertices()) {
        if (v.id == INVALID_ID) continue;
        vids.push_back(v.id);
    }
    if (vids.size() < 2) return;

    const float dist2 = distance * distance;

    std::unordered_map<WeldGridKey, std::vector<VertexID>, WeldGridKeyHash> grid;
    grid.reserve(vids.size());

    std::unordered_map<VertexID, VertexID> toRep;
    toRep.reserve(vids.size());

    auto tryFindRep = [&](const glm::vec3& p, const WeldGridKey& key) -> VertexID {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    WeldGridKey nk{ key.x + dx, key.y + dy, key.z + dz };
                    auto it = grid.find(nk);
                    if (it == grid.end()) continue;
                    for (VertexID rep : it->second) {
                        const EMVertex* rv = mesh.GetVertex(rep);
                        if (!rv) continue;
                        glm::vec3 d = rv->position - p;
                        if (glm::dot(d, d) <= dist2) return rep;
                    }
                }
            }
        }
        return INVALID_ID;
    };

    for (VertexID vid : vids) {
        const EMVertex* v = mesh.GetVertex(vid);
        if (!v) continue;
        WeldGridKey k = WeldQuantize(v->position, distance);
        VertexID rep = tryFindRep(v->position, k);
        if (rep == INVALID_ID) {
            grid[k].push_back(vid);
            toRep[vid] = vid;
        } else {
            toRep[vid] = rep;
        }
    }

    bool anyMerge = false;
    for (const auto& kv : toRep) {
        if (kv.first != kv.second) { anyMerge = true; break; }
    }
    if (!anyMerge) return;

    // Representatives -> new vertex ids
    std::unordered_map<VertexID, VertexID> repToNew;
    repToNew.reserve(vids.size());

    EditableMesh newMesh;
    for (VertexID vid : vids) {
        if (toRep[vid] != vid) continue;
        const EMVertex* rv = mesh.GetVertex(vid);
        if (!rv) continue;
        VertexID newVid = newMesh.AddVertex(rv->position);
        if (EMVertex* nv = newMesh.GetVertex(newVid)) {
            nv->uv = rv->uv;      // best-effort
            nv->normal = rv->normal;
        }
        repToNew[vid] = newVid;
    }

    // Rebuild faces, preserving per-loop UVs
    for (const auto& face : mesh.GetFaces()) {
        if (face.id == INVALID_ID) continue;

        std::vector<VertexID> newFaceVerts;
        std::vector<glm::vec2> newFaceUVs;
        mesh.ForEachFaceLoop(face.id, [&](const EMLoop& loop) {
            VertexID rep = toRep[loop.vertex];
            auto it = repToNew.find(rep);
            if (it == repToNew.end()) return;
            newFaceVerts.push_back(it->second);
            newFaceUVs.push_back(loop.uv);
        });

        // Collapse consecutive duplicates introduced by welding
        if (newFaceVerts.size() >= 2) {
            std::vector<VertexID> collapsedVerts;
            std::vector<glm::vec2> collapsedUVs;
            collapsedVerts.reserve(newFaceVerts.size());
            collapsedUVs.reserve(newFaceUVs.size());
            for (size_t i = 0; i < newFaceVerts.size(); ++i) {
                if (!collapsedVerts.empty() && collapsedVerts.back() == newFaceVerts[i]) continue;
                collapsedVerts.push_back(newFaceVerts[i]);
                collapsedUVs.push_back(newFaceUVs[i]);
            }
            if (collapsedVerts.size() >= 3 && collapsedVerts.front() == collapsedVerts.back()) {
                collapsedVerts.pop_back();
                collapsedUVs.pop_back();
            }
            newFaceVerts = std::move(collapsedVerts);
            newFaceUVs = std::move(collapsedUVs);
        }

        if (newFaceVerts.size() < 3) continue;
        FaceID nf = newMesh.AddFace(newFaceVerts);
        if (nf == INVALID_ID) continue;

        // Assign per-loop UVs in the same order
        size_t idx = 0;
        newMesh.ForEachFaceLoop(nf, [&](const EMLoop& l) {
            if (EMLoop* ml = newMesh.GetLoop(l.id)) {
                if (idx < newFaceUVs.size()) ml->uv = newFaceUVs[idx];
            }
            idx++;
        });
    }

    newMesh.MakeWindingConsistentAndOutward();
    newMesh.RecalculateNormals();
    mesh = std::move(newMesh);

    LUCENT_CORE_INFO("Welded vertices by distance: {}", distance);
}

// ============================================================================
// Transform Operations
// ============================================================================

void TranslateSelection(EditableMesh& mesh, const glm::vec3& offset) {
    const auto& selection = mesh.GetSelection();
    
    // Collect all vertices to move
    std::unordered_set<VertexID> vertsToMove;
    
    // Direct vertex selection
    for (VertexID vid : selection.vertices) {
        vertsToMove.insert(vid);
    }
    
    // Vertices from edge selection
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (e) {
            vertsToMove.insert(e->v0);
            vertsToMove.insert(e->v1);
        }
    }
    
    // Vertices from face selection
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            vertsToMove.insert(loop.vertex);
        });
    }
    
    // Apply translation
    for (VertexID vid : vertsToMove) {
        EMVertex* v = mesh.GetVertex(vid);
        if (v) {
            v->position += offset;
        }
    }
    
    // Recalculate normals for affected faces
    mesh.RecalculateNormals();
}

void RotateSelection(EditableMesh& mesh, const glm::vec3& pivot, const glm::quat& rotation) {
    const auto& selection = mesh.GetSelection();
    
    std::unordered_set<VertexID> vertsToRotate;
    
    for (VertexID vid : selection.vertices) {
        vertsToRotate.insert(vid);
    }
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (e) {
            vertsToRotate.insert(e->v0);
            vertsToRotate.insert(e->v1);
        }
    }
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            vertsToRotate.insert(loop.vertex);
        });
    }
    
    for (VertexID vid : vertsToRotate) {
        EMVertex* v = mesh.GetVertex(vid);
        if (v) {
            glm::vec3 local = v->position - pivot;
            local = rotation * local;
            v->position = local + pivot;
        }
    }
    
    mesh.RecalculateNormals();
}

void ScaleSelection(EditableMesh& mesh, const glm::vec3& pivot, const glm::vec3& scale) {
    const auto& selection = mesh.GetSelection();
    
    std::unordered_set<VertexID> vertsToScale;
    
    for (VertexID vid : selection.vertices) {
        vertsToScale.insert(vid);
    }
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (e) {
            vertsToScale.insert(e->v0);
            vertsToScale.insert(e->v1);
        }
    }
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            vertsToScale.insert(loop.vertex);
        });
    }
    
    for (VertexID vid : vertsToScale) {
        EMVertex* v = mesh.GetVertex(vid);
        if (v) {
            glm::vec3 local = v->position - pivot;
            local *= scale;
            v->position = local + pivot;
        }
    }
    
    mesh.RecalculateNormals();
}

// ============================================================================
// Topology Operations
// ============================================================================

std::vector<FaceID> ExtrudeFaces(EditableMesh& mesh, float distance) {
    std::vector<FaceID> newFaces;
    const auto& selection = mesh.GetSelection();
    
    if (selection.faces.empty()) return newFaces;
    
    // Compute a simple mesh centroid (used to decide "outward" for closed meshes).
    // If a face normal points toward the centroid, we flip the extrusion direction.
    glm::vec3 meshCenter(0.0f);
    uint32_t centerCount = 0;
    for (const auto& v : mesh.GetVertices()) {
        if (v.id == INVALID_ID) continue;
        meshCenter += v.position;
        centerCount++;
    }
    if (centerCount > 0) {
        meshCenter /= static_cast<float>(centerCount);
    }
    
    // For each selected face:
    // 1. Duplicate all vertices
    // 2. Create new face with duplicated vertices
    // 3. Create side faces connecting original and new vertices
    // 4. Remove original face
    
    std::unordered_map<VertexID, VertexID> vertexDuplicates;
    std::vector<FaceID> facesToRemove(selection.faces.begin(), selection.faces.end());
    std::vector<FaceID> topFaces;
    
    // First pass: collect all boundary edges (edges that have only one selected face)
    std::unordered_map<EdgeID, int> edgeFaceCount;
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            edgeFaceCount[loop.edge]++;
        });
    }
    
    // Duplicate vertices for selected faces
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            if (vertexDuplicates.find(loop.vertex) == vertexDuplicates.end()) {
                const EMVertex* oldV = mesh.GetVertex(loop.vertex);
                if (oldV) {
                    // Calculate extrusion direction (face normal)
                    // Ensure face normal is up to date (in case vertices moved)
                    mesh.RecalculateFaceNormal(fid);
                    const EMFace* face = mesh.GetFace(fid);
                    glm::vec3 extrudeDir = face ? face->normal : glm::vec3(0, 1, 0);
                    
                    // Decide outward vs inward for closed meshes:
                    // If the normal points toward the mesh center, flip it.
                    glm::vec3 faceCenter = mesh.CalculateFaceCenter(fid);
                    glm::vec3 toOutside = faceCenter - meshCenter;
                    if (glm::length(toOutside) > 1e-6f) {
                        if (glm::dot(extrudeDir, toOutside) < 0.0f) {
                            extrudeDir = -extrudeDir;
                        }
                    }
                    
                    VertexID newVid = mesh.AddVertex(oldV->position + extrudeDir * distance);
                    EMVertex* newV = mesh.GetVertex(newVid);
                    if (newV) {
                        newV->uv = oldV->uv;
                    }
                    vertexDuplicates[loop.vertex] = newVid;
                }
            }
        });
    }
    
    // Create new top faces with duplicated vertices
    for (FaceID fid : selection.faces) {
        std::vector<VertexID> newFaceVerts;
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            newFaceVerts.push_back(vertexDuplicates[loop.vertex]);
        });
        
        FaceID newFid = mesh.AddFace(newFaceVerts);
        if (newFid != INVALID_ID) {
            newFaces.push_back(newFid);
            topFaces.push_back(newFid);
        }
    }
    
    // Create side faces for boundary edges.
    //
    // IMPORTANT: Use face loop order to preserve consistent winding.
    // Using EMEdge::v0/v1 is not oriented (edges are undirected), which can flip half the side quads.
    for (FaceID fid : selection.faces) {
        // For each boundary loop edge in this face, build a quad:
        // [curr, next, next', curr'] with the same winding as the original face loop.
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            if (edgeFaceCount[loop.edge] != 1) return; // Not a boundary edge
            
            const EMLoop* nextLoop = mesh.GetLoop(loop.next);
            if (!nextLoop) return;
            
            VertexID v0 = loop.vertex;
            VertexID v1 = nextLoop->vertex;
            
            auto it0 = vertexDuplicates.find(v0);
            auto it1 = vertexDuplicates.find(v1);
            if (it0 == vertexDuplicates.end() || it1 == vertexDuplicates.end()) return;
            
            VertexID v0New = it0->second;
            VertexID v1New = it1->second;
            
            std::vector<VertexID> sideVerts = { v0, v1, v1New, v0New };
            FaceID sideFace = mesh.AddFace(sideVerts);
            if (sideFace != INVALID_ID) {
                newFaces.push_back(sideFace);
            }
        });
    }
    
    // Remove original faces
    for (FaceID fid : facesToRemove) {
        mesh.RemoveFace(fid);
    }
    
    // Select new top faces
    mesh.DeselectAll();
    for (FaceID fid : topFaces) {
        mesh.SelectFace(fid, true);
    }
    
    // Ensure consistent winding/outward so future edits + culling behave
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
    return newFaces;
}

std::vector<FaceID> ExtrudeEdges(EditableMesh& mesh, const glm::vec3& direction, float distance) {
    std::vector<FaceID> newFaces;
    const auto& selection = mesh.GetSelection();
    
    if (selection.edges.empty()) return newFaces;
    
    std::unordered_map<VertexID, VertexID> vertexDuplicates;
    
    // Duplicate vertices for selected edges
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        
        for (VertexID vid : {e->v0, e->v1}) {
            if (vertexDuplicates.find(vid) == vertexDuplicates.end()) {
                const EMVertex* oldV = mesh.GetVertex(vid);
                if (oldV) {
                    VertexID newVid = mesh.AddVertex(oldV->position + direction * distance);
                    EMVertex* newV = mesh.GetVertex(newVid);
                    if (newV) newV->uv = oldV->uv;
                    vertexDuplicates[vid] = newVid;
                }
            }
        }
    }
    
    // Create faces for each edge
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        
        std::vector<VertexID> faceVerts = {
            e->v0, e->v1,
            vertexDuplicates[e->v1],
            vertexDuplicates[e->v0]
        };
        
        FaceID fid = mesh.AddFace(faceVerts);
        if (fid != INVALID_ID) {
            newFaces.push_back(fid);
        }
    }
    
    mesh.RecalculateNormals();
    return newFaces;
}

std::vector<FaceID> InsetFaces(EditableMesh& mesh, float thickness) {
    std::vector<FaceID> newFaces;
    const auto& selection = mesh.GetSelection();
    
    if (selection.faces.empty()) return newFaces;
    
    std::vector<FaceID> facesToProcess(selection.faces.begin(), selection.faces.end());
    
    for (FaceID fid : facesToProcess) {
        const EMFace* face = mesh.GetFace(fid);
        if (!face) continue;
        
        // Collect face vertices and calculate center
        std::vector<VertexID> outerVerts;
        glm::vec3 center(0.0f);
        
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            outerVerts.push_back(loop.vertex);
            const EMVertex* v = mesh.GetVertex(loop.vertex);
            if (v) center += v->position;
        });
        
        if (outerVerts.empty()) continue;
        center /= static_cast<float>(outerVerts.size());
        
        // Create inset vertices (move toward center)
        std::vector<VertexID> innerVerts;
        for (VertexID vid : outerVerts) {
            const EMVertex* v = mesh.GetVertex(vid);
            if (v) {
                glm::vec3 dir = glm::normalize(center - v->position);
                glm::vec3 newPos = v->position + dir * thickness;
                VertexID newVid = mesh.AddVertex(newPos);
                EMVertex* newV = mesh.GetVertex(newVid);
                if (newV) newV->uv = v->uv;
                innerVerts.push_back(newVid);
            }
        }
        
        // Remove original face
        mesh.RemoveFace(fid);
        
        // Create inner face
        FaceID innerFace = mesh.AddFace(innerVerts);
        if (innerFace != INVALID_ID) {
            newFaces.push_back(innerFace);
        }
        
        // Create border faces (quads connecting outer to inner)
        for (size_t i = 0; i < outerVerts.size(); ++i) {
            size_t next = (i + 1) % outerVerts.size();
            
            std::vector<VertexID> borderVerts = {
                outerVerts[i],
                outerVerts[next],
                innerVerts[next],
                innerVerts[i]
            };
            
            FaceID borderFace = mesh.AddFace(borderVerts);
            if (borderFace != INVALID_ID) {
                newFaces.push_back(borderFace);
            }
        }
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
    return newFaces;
}

void BevelEdges(EditableMesh& mesh, float width, int segments) {
    const auto& selection = mesh.GetSelection();
    
    if (selection.edges.empty()) return;
    if (segments < 1) segments = 1;
    
    std::vector<EdgeID> edgesToBevel(selection.edges.begin(), selection.edges.end());
    std::vector<FaceID> newFaces;
    
    for (EdgeID eid : edgesToBevel) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        
        const EMVertex* v0 = mesh.GetVertex(e->v0);
        const EMVertex* v1 = mesh.GetVertex(e->v1);
        if (!v0 || !v1) continue;
        
        auto edgeFaces = mesh.GetEdgeFaces(eid);
        if (edgeFaces.empty()) continue;
        
        struct FaceOffset {
            FaceID face = INVALID_ID;
            VertexID v0Offset = INVALID_ID;
            VertexID v1Offset = INVALID_ID;
            bool valid = false;
        };
        
        std::vector<FaceOffset> offsets;
        offsets.reserve(edgeFaces.size());
        
        for (FaceID fid : edgeFaces) {
            EMFace* face = mesh.GetFace(fid);
            if (!face) continue;
            mesh.RecalculateFaceNormal(fid);
            
            std::vector<VertexID> faceVerts = CollectFaceVertices(mesh, fid);
            if (faceVerts.size() < 3) continue;
            
            size_t startIdx = faceVerts.size();
            bool forward = false;
            for (size_t i = 0; i < faceVerts.size(); ++i) {
                size_t next = (i + 1) % faceVerts.size();
                if (faceVerts[i] == e->v0 && faceVerts[next] == e->v1) {
                    startIdx = i;
                    forward = true;
                    break;
                }
                if (faceVerts[i] == e->v1 && faceVerts[next] == e->v0) {
                    startIdx = i;
                    forward = false;
                    break;
                }
            }
            if (startIdx == faceVerts.size()) continue;
            
            VertexID fV0 = forward ? e->v0 : e->v1;
            VertexID fV1 = forward ? e->v1 : e->v0;
            const EMVertex* fv0 = mesh.GetVertex(fV0);
            const EMVertex* fv1 = mesh.GetVertex(fV1);
            if (!fv0 || !fv1) continue;
            
            glm::vec3 edgeDir = glm::normalize(fv1->position - fv0->position);
            glm::vec3 offsetDir = glm::normalize(glm::cross(face->normal, edgeDir));
            glm::vec3 faceCenter = mesh.CalculateFaceCenter(fid);
            glm::vec3 edgeCenter = (fv0->position + fv1->position) * 0.5f;
            if (glm::dot(offsetDir, faceCenter - edgeCenter) < 0.0f) {
                offsetDir = -offsetDir;
            }
            
            VertexID v0Offset = mesh.AddVertex(fv0->position + offsetDir * width);
            VertexID v1Offset = mesh.AddVertex(fv1->position + offsetDir * width);
            EMVertex* newV0 = mesh.GetVertex(v0Offset);
            EMVertex* newV1 = mesh.GetVertex(v1Offset);
            if (newV0) newV0->uv = fv0->uv;
            if (newV1) newV1->uv = fv1->uv;
            
            faceVerts[startIdx] = v0Offset;
            faceVerts[(startIdx + 1) % faceVerts.size()] = v1Offset;
            
            mesh.RemoveFace(fid);
            FaceID newFid = mesh.AddFace(faceVerts);
            if (newFid != INVALID_ID) {
                newFaces.push_back(newFid);
            }
            
            offsets.push_back({fid, v0Offset, v1Offset, true});
        }
        
        if (offsets.size() == 2) {
            auto chain0 = BuildSegmentVertices(mesh, offsets[0].v0Offset, offsets[1].v0Offset, segments);
            auto chain1 = BuildSegmentVertices(mesh, offsets[0].v1Offset, offsets[1].v1Offset, segments);
            
            size_t count = std::min(chain0.size(), chain1.size());
            for (size_t i = 0; i + 1 < count; ++i) {
                FaceID bevelFace = mesh.AddFace({chain0[i], chain1[i], chain1[i + 1], chain0[i + 1]});
                if (bevelFace != INVALID_ID) {
                    newFaces.push_back(bevelFace);
                }
            }
        } else if (offsets.size() == 1) {
            auto chain0 = BuildSegmentVertices(mesh, e->v0, offsets[0].v0Offset, segments);
            auto chain1 = BuildSegmentVertices(mesh, e->v1, offsets[0].v1Offset, segments);
            size_t count = std::min(chain0.size(), chain1.size());
            for (size_t i = 0; i + 1 < count; ++i) {
                FaceID bevelFace = mesh.AddFace({chain0[i], chain1[i], chain1[i + 1], chain0[i + 1]});
                if (bevelFace != INVALID_ID) {
                    newFaces.push_back(bevelFace);
                }
            }
        }
    }
    
    CleanupOrphanEdges(mesh);
    
    mesh.DeselectAll();
    for (FaceID fid : newFaces) {
        mesh.SelectFace(fid, true);
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
}

std::vector<EdgeID> LoopCut(EditableMesh& mesh, EdgeID startEdge, float position) {
    std::vector<EdgeID> newEdges;
    
    const EMEdge* e = mesh.GetEdge(startEdge);
    if (!e) return newEdges;
    
    position = std::clamp(position, 0.0f, 1.0f);
    
    std::unordered_set<EdgeID> loopEdges;
    std::vector<EdgeID> edgeQueue;
    edgeQueue.push_back(startEdge);
    loopEdges.insert(startEdge);
    
    while (!edgeQueue.empty()) {
        EdgeID eid = edgeQueue.back();
        edgeQueue.pop_back();
        
        auto faces = mesh.GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            const EMFace* face = mesh.GetFace(fid);
            if (!face || face->vertCount != 4) continue;
            
            size_t loopIndex = 0;
            if (!FaceHasEdge(mesh, fid, eid, loopIndex)) continue;
            
            auto loops = CollectFaceLoops(mesh, fid);
            size_t oppositeIndex = (loopIndex + 2) % loops.size();
            const EMLoop* oppositeLoop = mesh.GetLoop(loops[oppositeIndex]);
            if (!oppositeLoop) continue;
            
            EdgeID oppEid = oppositeLoop->edge;
            if (oppEid == INVALID_ID) continue;
            
            if (loopEdges.insert(oppEid).second) {
                edgeQueue.push_back(oppEid);
            }
        }
    }
    
    std::unordered_map<EdgeID, VertexID> splitVertices;
    std::unordered_set<FaceID> splitFaces;
    std::unordered_set<EdgeID> newEdgeSet;
    
    for (EdgeID eid : loopEdges) {
        auto faces = mesh.GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            if (splitFaces.count(fid)) continue;
            
            const EMFace* face = mesh.GetFace(fid);
            if (!face || face->vertCount != 4) continue;
            
            size_t loopIndex = 0;
            if (!FaceHasEdge(mesh, fid, eid, loopIndex)) continue;
            
            auto loops = CollectFaceLoops(mesh, fid);
            if (loops.size() != 4) continue;
            
            const EMLoop* loop0 = mesh.GetLoop(loops[loopIndex]);
            const EMLoop* loop1 = mesh.GetLoop(loops[(loopIndex + 1) % loops.size()]);
            const EMLoop* loop2 = mesh.GetLoop(loops[(loopIndex + 2) % loops.size()]);
            const EMLoop* loop3 = mesh.GetLoop(loops[(loopIndex + 3) % loops.size()]);
            if (!loop0 || !loop1 || !loop2 || !loop3) continue;
            
            VertexID v0 = loop0->vertex;
            VertexID v1 = loop1->vertex;
            VertexID v2 = loop2->vertex;
            VertexID v3 = loop3->vertex;
            
            VertexID newEdgeV = INVALID_ID;
            auto it = splitVertices.find(eid);
            if (it != splitVertices.end()) {
                newEdgeV = it->second;
            } else {
                const EMVertex* ev0 = mesh.GetVertex(v0);
                const EMVertex* ev1 = mesh.GetVertex(v1);
                if (!ev0 || !ev1) continue;
                glm::vec3 newPos = glm::mix(ev0->position, ev1->position, position);
                newEdgeV = mesh.AddVertex(newPos);
                EMVertex* newV = mesh.GetVertex(newEdgeV);
                if (newV) newV->uv = glm::mix(ev0->uv, ev1->uv, position);
                splitVertices[eid] = newEdgeV;
            }
            
            EdgeID oppEdge = loop2->edge;
            VertexID newOppV = INVALID_ID;
            auto oppIt = splitVertices.find(oppEdge);
            if (oppIt != splitVertices.end()) {
                newOppV = oppIt->second;
            } else {
                const EMVertex* ev2 = mesh.GetVertex(v2);
                const EMVertex* ev3 = mesh.GetVertex(v3);
                if (!ev2 || !ev3) continue;
                glm::vec3 newPos = glm::mix(ev2->position, ev3->position, position);
                newOppV = mesh.AddVertex(newPos);
                EMVertex* newV = mesh.GetVertex(newOppV);
                if (newV) newV->uv = glm::mix(ev2->uv, ev3->uv, position);
                splitVertices[oppEdge] = newOppV;
            }
            
            mesh.RemoveFace(fid);
            // Create two new quads. Return values are not currently used, but keeping the calls for side effects.
            (void)mesh.AddFace({v0, newEdgeV, newOppV, v3});
            (void)mesh.AddFace({newEdgeV, v1, v2, newOppV});
            splitFaces.insert(fid);
            
            EdgeID cutEdge = FindEdgeBetween(mesh, newEdgeV, newOppV);
            if (cutEdge != INVALID_ID) {
                newEdgeSet.insert(cutEdge);
            }
        }
    }
    
    CleanupOrphanEdges(mesh);
    
    newEdges.assign(newEdgeSet.begin(), newEdgeSet.end());
    
    mesh.DeselectAll();
    for (EdgeID eid : newEdges) {
        mesh.SelectEdge(eid, true);
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
    return newEdges;
}

VertexID MergeVerticesAtCenter(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    
    if (selection.vertices.size() < 2) return INVALID_ID;
    
    // Calculate center
    glm::vec3 center(0.0f);
    for (VertexID vid : selection.vertices) {
        const EMVertex* v = mesh.GetVertex(vid);
        if (v) center += v->position;
    }
    center /= static_cast<float>(selection.vertices.size());
    
    // Create new vertex at center
    VertexID newVid = mesh.AddVertex(center);
    
    // TODO: Update all faces to use new vertex instead of old ones
    // This is complex topology modification
    
    // For now, just move first vertex to center
    auto it = selection.vertices.begin();
    if (it != selection.vertices.end()) {
        EMVertex* v = mesh.GetVertex(*it);
        if (v) v->position = center;
        return *it;
    }
    
    return newVid;
}

VertexID MergeVerticesAtLast(EditableMesh& mesh) {
    // Similar to MergeAtCenter but use last selected vertex position
    // Note: unordered_set has no defined "last" - we just pick one arbitrarily
    const auto& selection = mesh.GetSelection();
    
    if (selection.vertices.size() < 2) return INVALID_ID;
    
    // Pick any vertex as the target (first one in iteration order)
    auto it = selection.vertices.begin();
    if (it == selection.vertices.end()) return INVALID_ID;
    
    const EMVertex* target = mesh.GetVertex(*it);
    if (!target) return INVALID_ID;
    
    glm::vec3 targetPos = target->position;
    VertexID targetVid = *it;
    
    // Move all other selected vertices to target position
    for (VertexID vid : selection.vertices) {
        if (vid != targetVid) {
            EMVertex* v = mesh.GetVertex(vid);
            if (v) v->position = targetPos;
        }
    }
    
    return targetVid;
}

// ============================================================================
// Delete Operations
// ============================================================================

void DeleteVertices(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    std::vector<VertexID> toDelete(selection.vertices.begin(), selection.vertices.end());
    
    for (VertexID vid : toDelete) {
        mesh.RemoveVertex(vid);
    }
}

void DeleteEdges(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    std::vector<EdgeID> toDelete(selection.edges.begin(), selection.edges.end());
    
    for (EdgeID eid : toDelete) {
        mesh.RemoveEdge(eid);
    }
}

void DeleteFaces(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    std::vector<FaceID> toDelete(selection.faces.begin(), selection.faces.end());
    
    for (FaceID fid : toDelete) {
        mesh.RemoveFace(fid);
    }
}

void DissolveVertices(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    if (selection.vertices.empty()) return;
    
    std::unordered_set<FaceID> facesToUpdate;
    for (VertexID vid : selection.vertices) {
        auto faces = mesh.GetVertexFaces(vid);
        facesToUpdate.insert(faces.begin(), faces.end());
    }
    
    std::vector<FaceID> newFaces;
    
    for (FaceID fid : facesToUpdate) {
        const EMFace* face = mesh.GetFace(fid);
        if (!face) continue;
        
        std::vector<VertexID> verts = CollectFaceVertices(mesh, fid);
        verts.erase(
            std::remove_if(
                verts.begin(),
                verts.end(),
                [&](VertexID vid) { return selection.vertices.count(vid) > 0; }
            ),
            verts.end()
        );
        
        mesh.RemoveFace(fid);
        if (verts.size() >= 3) {
            FaceID newFid = mesh.AddFace(verts);
            if (newFid != INVALID_ID) newFaces.push_back(newFid);
        }
    }
    
    for (VertexID vid : selection.vertices) {
        if (mesh.GetVertex(vid)) {
            mesh.RemoveVertex(vid);
        }
    }
    
    CleanupOrphanEdges(mesh);
    
    mesh.DeselectAll();
    for (FaceID fid : newFaces) {
        mesh.SelectFace(fid, true);
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
}

void DissolveEdges(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    if (selection.edges.empty()) return;
    
    std::vector<EdgeID> edgesToDissolve(selection.edges.begin(), selection.edges.end());
    std::vector<FaceID> newFaces;
    
    for (EdgeID eid : edgesToDissolve) {
        const EMEdge* edge = mesh.GetEdge(eid);
        if (!edge) continue;
        
        auto faces = mesh.GetEdgeFaces(eid);
        if (faces.size() < 1) continue;
        
        if (faces.size() == 1) {
            FaceID fid = faces[0];
            std::vector<VertexID> verts = CollectFaceVertices(mesh, fid);
            verts.erase(
                std::remove_if(
                    verts.begin(),
                    verts.end(),
                    [&](VertexID vid) { return vid == edge->v0 || vid == edge->v1; }
                ),
                verts.end()
            );
            mesh.RemoveFace(fid);
            if (verts.size() >= 3) {
                FaceID newFid = mesh.AddFace(verts);
                if (newFid != INVALID_ID) newFaces.push_back(newFid);
            }
            continue;
        }
        
        FaceID f0 = faces[0];
        FaceID f1 = faces[1];
        if (!mesh.GetFace(f0) || !mesh.GetFace(f1)) continue;
        
        std::vector<VertexID> verts0 = CollectFaceVertices(mesh, f0);
        std::vector<VertexID> verts1 = CollectFaceVertices(mesh, f1);
        
        std::vector<VertexID> path0 = BuildPathAvoidingEdge(verts0, edge->v0, edge->v1);
        std::vector<VertexID> path1 = BuildPathAvoidingEdge(verts1, edge->v1, edge->v0);
        if (path0.size() < 2 || path1.size() < 2) continue;
        
        std::vector<VertexID> merged;
        merged.reserve(path0.size() + path1.size() - 2);
        merged.insert(merged.end(), path0.begin(), path0.end());
        merged.insert(merged.end(), path1.begin() + 1, path1.end() - 1);
        
        mesh.RemoveFace(f0);
        mesh.RemoveFace(f1);
        FaceID newFid = mesh.AddFace(merged);
        if (newFid != INVALID_ID) newFaces.push_back(newFid);
    }
    
    CleanupOrphanEdges(mesh);
    
    mesh.DeselectAll();
    for (FaceID fid : newFaces) {
        mesh.SelectFace(fid, true);
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
}

// ============================================================================
// Selection Operations
// ============================================================================

void SelectEdgeLoop(EditableMesh& mesh, EdgeID startEdge) {
    const EMEdge* e = mesh.GetEdge(startEdge);
    if (!e) return;
    
    std::unordered_set<EdgeID> loopEdges;
    std::vector<EdgeID> edgeQueue;
    edgeQueue.push_back(startEdge);
    loopEdges.insert(startEdge);
    
    while (!edgeQueue.empty()) {
        EdgeID eid = edgeQueue.back();
        edgeQueue.pop_back();
        
        auto faces = mesh.GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            const EMFace* face = mesh.GetFace(fid);
            if (!face || face->vertCount != 4) continue;
            
            size_t loopIndex = 0;
            if (!FaceHasEdge(mesh, fid, eid, loopIndex)) continue;
            
            auto loops = CollectFaceLoops(mesh, fid);
            size_t oppositeIndex = (loopIndex + 2) % loops.size();
            const EMLoop* oppositeLoop = mesh.GetLoop(loops[oppositeIndex]);
            if (!oppositeLoop) continue;
            
            EdgeID oppEid = oppositeLoop->edge;
            if (oppEid != INVALID_ID && loopEdges.insert(oppEid).second) {
                edgeQueue.push_back(oppEid);
            }
        }
    }
    
    mesh.DeselectAll();
    for (EdgeID eid : loopEdges) {
        mesh.SelectEdge(eid, true);
    }
}

void SelectEdgeRing(EditableMesh& mesh, EdgeID startEdge) {
    const EMEdge* e = mesh.GetEdge(startEdge);
    if (!e) return;
    
    std::unordered_set<EdgeID> ringEdges;
    std::vector<EdgeID> edgeQueue;
    edgeQueue.push_back(startEdge);
    ringEdges.insert(startEdge);
    
    while (!edgeQueue.empty()) {
        EdgeID eid = edgeQueue.back();
        edgeQueue.pop_back();
        
        auto faces = mesh.GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            const EMFace* face = mesh.GetFace(fid);
            if (!face || face->vertCount != 4) continue;
            
            std::vector<LoopID> loops = CollectFaceLoops(mesh, fid);
            for (size_t i = 0; i < loops.size(); ++i) {
                const EMLoop* loop = mesh.GetLoop(loops[i]);
                if (!loop) continue;
                if (loop->edge != eid) continue;
                
                const EMLoop* prevLoop = mesh.GetLoop(loop->prev);
                const EMLoop* nextLoop = mesh.GetLoop(loop->next);
                if (prevLoop && prevLoop->edge != INVALID_ID) {
                    if (ringEdges.insert(prevLoop->edge).second) {
                        edgeQueue.push_back(prevLoop->edge);
                    }
                }
                if (nextLoop && nextLoop->edge != INVALID_ID) {
                    if (ringEdges.insert(nextLoop->edge).second) {
                        edgeQueue.push_back(nextLoop->edge);
                    }
                }
            }
        }
    }
    
    mesh.DeselectAll();
    for (EdgeID eid : ringEdges) {
        mesh.SelectEdge(eid, true);
    }
}

void GrowSelection(EditableMesh& mesh) {
    auto& selection = mesh.GetSelection();
    
    // Grow vertices
    std::unordered_set<VertexID> newVerts;
    for (VertexID vid : selection.vertices) {
        auto edges = mesh.GetVertexEdges(vid);
        for (EdgeID eid : edges) {
            const EMEdge* e = mesh.GetEdge(eid);
            if (e) {
                newVerts.insert(e->v0);
                newVerts.insert(e->v1);
            }
        }
    }
    for (VertexID vid : newVerts) {
        mesh.SelectVertex(vid, true);
    }
    
    // Grow edges
    std::unordered_set<EdgeID> newEdges;
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (e) {
            for (auto nextEid : mesh.GetVertexEdges(e->v0)) {
                newEdges.insert(nextEid);
            }
            for (auto nextEid : mesh.GetVertexEdges(e->v1)) {
                newEdges.insert(nextEid);
            }
        }
    }
    for (EdgeID eid : newEdges) {
        mesh.SelectEdge(eid, true);
    }
    
    // Grow faces
    std::unordered_set<FaceID> newFaces;
    for (FaceID fid : selection.faces) {
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            auto adjFaces = mesh.GetEdgeFaces(loop.edge);
            for (FaceID adjFid : adjFaces) {
                newFaces.insert(adjFid);
            }
        });
    }
    for (FaceID fid : newFaces) {
        mesh.SelectFace(fid, true);
    }
}

void ShrinkSelection(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    
    std::unordered_set<VertexID> shrinkVerts;
    for (VertexID vid : selection.vertices) {
        bool keep = true;
        auto edges = mesh.GetVertexEdges(vid);
        for (EdgeID eid : edges) {
            const EMEdge* e = mesh.GetEdge(eid);
            if (!e) continue;
            VertexID other = e->OtherVertex(vid);
            if (selection.vertices.count(other) == 0) {
                keep = false;
                break;
            }
        }
        if (keep) shrinkVerts.insert(vid);
    }
    
    std::unordered_set<EdgeID> shrinkEdges;
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        bool keep = true;
        auto edges0 = mesh.GetVertexEdges(e->v0);
        auto edges1 = mesh.GetVertexEdges(e->v1);
        for (EdgeID adj : edges0) {
            if (selection.edges.count(adj) == 0) {
                keep = false;
                break;
            }
        }
        if (keep) {
            for (EdgeID adj : edges1) {
                if (selection.edges.count(adj) == 0) {
                    keep = false;
                    break;
                }
            }
        }
        if (keep) shrinkEdges.insert(eid);
    }
    
    std::unordered_set<FaceID> shrinkFaces;
    for (FaceID fid : selection.faces) {
        bool keep = true;
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            auto adjFaces = mesh.GetEdgeFaces(loop.edge);
            for (FaceID adj : adjFaces) {
                if (selection.faces.count(adj) == 0) {
                    keep = false;
                    break;
                }
            }
        });
        if (keep) shrinkFaces.insert(fid);
    }
    
    mesh.DeselectAll();
    for (VertexID vid : shrinkVerts) mesh.SelectVertex(vid, true);
    for (EdgeID eid : shrinkEdges) mesh.SelectEdge(eid, true);
    for (FaceID fid : shrinkFaces) mesh.SelectFace(fid, true);
}

// ============================================================================
// Utility Operations
// ============================================================================

void FlipNormals(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    
    for (FaceID fid : selection.faces) {
        EMFace* face = mesh.GetFace(fid);
        if (face) {
            face->normal = -face->normal;
            
            // TODO: Reverse loop winding order
        }
    }
}

void RecalculateNormals(EditableMesh& mesh) {
    mesh.RecalculateNormals();
}

void SubdivideFaces(EditableMesh& mesh, int cuts) {
    if (cuts < 1) return;
    
    const auto& selection = mesh.GetSelection();
    std::vector<FaceID> facesToSubdivide(selection.faces.begin(), selection.faces.end());
    std::vector<FaceID> newFaces;
    
    for (FaceID fid : facesToSubdivide) {
        const EMFace* face = mesh.GetFace(fid);
        if (!face) continue;
        
        std::vector<VertexID> verts = CollectFaceVertices(mesh, fid);
        if (verts.size() < 3) continue;
        
        std::vector<glm::vec3> positions;
        positions.reserve(verts.size());
        for (VertexID vid : verts) {
            const EMVertex* v = mesh.GetVertex(vid);
            if (v) positions.push_back(v->position);
        }
        if (positions.size() < 3) continue;
        
        mesh.RecalculateFaceNormal(fid);
        // NOTE: We don't currently ship an ear-clipping triangulator here.
        // Fall back to simple fan triangulation for subdivision seeding (works well for convex faces).
        std::vector<uint32_t> tri;
        tri.reserve((positions.size() - 2) * 3);
        for (uint32_t i = 1; i + 1 < static_cast<uint32_t>(positions.size()); ++i) {
            tri.push_back(0u);
            tri.push_back(i);
            tri.push_back(i + 1u);
        }
        if (tri.size() < 3) continue;
        
        mesh.RemoveFace(fid);
        
        std::unordered_map<uint64_t, std::vector<VertexID>> edgePoints;
        auto getEdgeKey = [](VertexID a, VertexID b) -> uint64_t {
            if (a > b) std::swap(a, b);
            return (static_cast<uint64_t>(a) << 32) | b;
        };
        
        auto getEdgePoint = [&](VertexID a, VertexID b, int idx) -> VertexID {
            uint64_t key = getEdgeKey(a, b);
            auto& pts = edgePoints[key];
            if (pts.empty()) {
                pts.resize(static_cast<size_t>(cuts + 2), INVALID_ID);
            }
            int forwardIdx = (a <= b) ? idx : (cuts + 1 - idx);
            if (pts[forwardIdx] != INVALID_ID) return pts[forwardIdx];
            
            const EMVertex* v0 = mesh.GetVertex(a);
            const EMVertex* v1 = mesh.GetVertex(b);
            if (!v0 || !v1) return INVALID_ID;
            
            float t = static_cast<float>(idx) / static_cast<float>(cuts + 1);
            VertexID vid = (idx == 0) ? a : (idx == cuts + 1 ? b : mesh.AddVertex(glm::mix(v0->position, v1->position, t)));
            if (vid != a && vid != b) {
                EMVertex* nv = mesh.GetVertex(vid);
                if (nv) nv->uv = glm::mix(v0->uv, v1->uv, t);
            }
            pts[forwardIdx] = vid;
            return vid;
        };
        
        for (size_t i = 0; i + 2 < tri.size(); i += 3) {
            VertexID v0 = verts[tri[i]];
            VertexID v1 = verts[tri[i + 1]];
            VertexID v2 = verts[tri[i + 2]];
            
            std::vector<std::vector<VertexID>> grid;
            grid.resize(static_cast<size_t>(cuts + 2));
            
            for (int row = 0; row <= cuts + 1; ++row) {
                int count = (cuts + 2) - row;
                grid[row].resize(static_cast<size_t>(count), INVALID_ID);
                
                VertexID rowStart = getEdgePoint(v0, v2, row);
                VertexID rowEnd = getEdgePoint(v1, v2, row);
                
                const EMVertex* startV = mesh.GetVertex(rowStart);
                const EMVertex* endV = mesh.GetVertex(rowEnd);
                
                for (int col = 0; col < count; ++col) {
                    if (row == 0) {
                        grid[row][col] = getEdgePoint(v0, v1, col);
                        continue;
                    }
                    if (col == 0) {
                        grid[row][col] = rowStart;
                        continue;
                    }
                    if (col == count - 1) {
                        grid[row][col] = rowEnd;
                        continue;
                    }
                    
                    if (!startV || !endV) continue;
                    float t = static_cast<float>(col) / static_cast<float>(count - 1);
                    glm::vec3 pos = glm::mix(startV->position, endV->position, t);
                    VertexID vid = mesh.AddVertex(pos);
                    EMVertex* nv = mesh.GetVertex(vid);
                    if (nv) nv->uv = glm::mix(startV->uv, endV->uv, t);
                    grid[row][col] = vid;
                }
            }
            
            for (int row = 0; row < cuts + 1; ++row) {
                for (int col = 0; col < (cuts + 1 - row); ++col) {
                    VertexID v00 = grid[row][col];
                    VertexID v01 = grid[row][col + 1];
                    VertexID v10 = grid[row + 1][col];
                    if (v00 == INVALID_ID || v01 == INVALID_ID || v10 == INVALID_ID) continue;
                    
                    FaceID f0 = mesh.AddFace({v00, v01, v10});
                    if (f0 != INVALID_ID) newFaces.push_back(f0);
                    
                    if (col < (cuts - row)) {
                        VertexID v11 = grid[row + 1][col + 1];
                        if (v11 != INVALID_ID) {
                            FaceID f1 = mesh.AddFace({v01, v11, v10});
                            if (f1 != INVALID_ID) newFaces.push_back(f1);
                        }
                    }
                }
            }
        }
    }
    
    CleanupOrphanEdges(mesh);
    
    mesh.DeselectAll();
    for (FaceID fid : newFaces) {
        mesh.SelectFace(fid, true);
    }
    
    mesh.MakeWindingConsistentAndOutward();
    mesh.RecalculateNormals();
}

void TriangulateFaces(EditableMesh& mesh) {
    const auto& selection = mesh.GetSelection();
    
    std::vector<FaceID> facesToTriangulate(selection.faces.begin(), selection.faces.end());
    
    for (FaceID fid : facesToTriangulate) {
        const EMFace* face = mesh.GetFace(fid);
        if (!face || face->vertCount <= 3) continue; // Already a triangle
        
        // Collect face vertices
        std::vector<VertexID> verts;
        mesh.ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            verts.push_back(loop.vertex);
        });
        
        // Remove original face
        mesh.RemoveFace(fid);
        
        // Create triangles using fan triangulation
        for (size_t i = 1; i + 1 < verts.size(); ++i) {
            mesh.AddFace({verts[0], verts[i], verts[i + 1]});
        }
    }
    
    mesh.RecalculateNormals();
}

} // namespace MeshOps
} // namespace lucent::mesh
