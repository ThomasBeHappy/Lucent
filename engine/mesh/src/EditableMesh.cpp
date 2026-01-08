#include "lucent/mesh/EditableMesh.h"
#include "lucent/mesh/Triangulator.h"
#include "lucent/core/Log.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace lucent::mesh {

// ============================================================================
// Element Access
// ============================================================================

EMVertex* EditableMesh::GetVertex(VertexID id) {
    if (id >= m_Vertices.size() || m_Vertices[id].id == INVALID_ID) return nullptr;
    return &m_Vertices[id];
}

const EMVertex* EditableMesh::GetVertex(VertexID id) const {
    if (id >= m_Vertices.size() || m_Vertices[id].id == INVALID_ID) return nullptr;
    return &m_Vertices[id];
}

EMEdge* EditableMesh::GetEdge(EdgeID id) {
    if (id >= m_Edges.size() || m_Edges[id].id == INVALID_ID) return nullptr;
    return &m_Edges[id];
}

const EMEdge* EditableMesh::GetEdge(EdgeID id) const {
    if (id >= m_Edges.size() || m_Edges[id].id == INVALID_ID) return nullptr;
    return &m_Edges[id];
}

EMLoop* EditableMesh::GetLoop(LoopID id) {
    if (id >= m_Loops.size() || m_Loops[id].id == INVALID_ID) return nullptr;
    return &m_Loops[id];
}

const EMLoop* EditableMesh::GetLoop(LoopID id) const {
    if (id >= m_Loops.size() || m_Loops[id].id == INVALID_ID) return nullptr;
    return &m_Loops[id];
}

EMFace* EditableMesh::GetFace(FaceID id) {
    if (id >= m_Faces.size() || m_Faces[id].id == INVALID_ID) return nullptr;
    return &m_Faces[id];
}

const EMFace* EditableMesh::GetFace(FaceID id) const {
    if (id >= m_Faces.size() || m_Faces[id].id == INVALID_ID) return nullptr;
    return &m_Faces[id];
}

// ============================================================================
// Iteration
// ============================================================================

void EditableMesh::ForEachFaceLoop(FaceID faceId, const std::function<void(const EMLoop&)>& fn) const {
    const EMFace* face = GetFace(faceId);
    if (!face || face->loopStart == INVALID_ID) return;
    
    LoopID current = face->loopStart;
    do {
        const EMLoop* loop = GetLoop(current);
        if (!loop) break;
        fn(*loop);
        current = loop->next;
    } while (current != face->loopStart && current != INVALID_ID);
}

void EditableMesh::ForEachFaceVertex(FaceID faceId, const std::function<void(const EMVertex&)>& fn) const {
    ForEachFaceLoop(faceId, [&](const EMLoop& loop) {
        const EMVertex* v = GetVertex(loop.vertex);
        if (v) fn(*v);
    });
}

std::vector<EdgeID> EditableMesh::GetVertexEdges(VertexID vid) const {
    std::vector<EdgeID> result;
    const EMVertex* v = GetVertex(vid);
    if (!v || v->edge == INVALID_ID) return result;
    
    EdgeID startEdge = v->edge;
    EdgeID current = startEdge;
    
    do {
        const EMEdge* e = GetEdge(current);
        if (!e) break;
        result.push_back(current);
        
        // Move to next edge around this vertex
        if (e->v0 == vid) {
            current = e->nextEdgeV0;
        } else {
            current = e->nextEdgeV1;
        }
    } while (current != startEdge && current != INVALID_ID);
    
    return result;
}

std::vector<FaceID> EditableMesh::GetVertexFaces(VertexID vid) const {
    std::vector<FaceID> result;
    std::unordered_set<FaceID> seen;
    
    auto edges = GetVertexEdges(vid);
    for (EdgeID eid : edges) {
        auto faces = GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            if (seen.find(fid) == seen.end()) {
                seen.insert(fid);
                result.push_back(fid);
            }
        }
    }
    
    return result;
}

std::vector<FaceID> EditableMesh::GetEdgeFaces(EdgeID eid) const {
    std::vector<FaceID> result;
    const EMEdge* e = GetEdge(eid);
    if (!e) return result;
    
    if (e->loop0 != INVALID_ID) {
        const EMLoop* l = GetLoop(e->loop0);
        if (l && l->face != INVALID_ID) result.push_back(l->face);
    }
    if (e->loop1 != INVALID_ID) {
        const EMLoop* l = GetLoop(e->loop1);
        if (l && l->face != INVALID_ID) result.push_back(l->face);
    }
    
    return result;
}

// ============================================================================
// Allocation
// ============================================================================

VertexID EditableMesh::AllocVertex() {
    if (!m_FreeVertices.empty()) {
        VertexID id = m_FreeVertices.back();
        m_FreeVertices.pop_back();
        m_Vertices[id] = EMVertex{};
        m_Vertices[id].id = id;
        return id;
    }
    VertexID id = static_cast<VertexID>(m_Vertices.size());
    m_Vertices.push_back(EMVertex{});
    m_Vertices[id].id = id;
    return id;
}

EdgeID EditableMesh::AllocEdge() {
    if (!m_FreeEdges.empty()) {
        EdgeID id = m_FreeEdges.back();
        m_FreeEdges.pop_back();
        m_Edges[id] = EMEdge{};
        m_Edges[id].id = id;
        return id;
    }
    EdgeID id = static_cast<EdgeID>(m_Edges.size());
    m_Edges.push_back(EMEdge{});
    m_Edges[id].id = id;
    return id;
}

LoopID EditableMesh::AllocLoop() {
    if (!m_FreeLoops.empty()) {
        LoopID id = m_FreeLoops.back();
        m_FreeLoops.pop_back();
        m_Loops[id] = EMLoop{};
        m_Loops[id].id = id;
        return id;
    }
    LoopID id = static_cast<LoopID>(m_Loops.size());
    m_Loops.push_back(EMLoop{});
    m_Loops[id].id = id;
    return id;
}

FaceID EditableMesh::AllocFace() {
    if (!m_FreeFaces.empty()) {
        FaceID id = m_FreeFaces.back();
        m_FreeFaces.pop_back();
        m_Faces[id] = EMFace{};
        m_Faces[id].id = id;
        return id;
    }
    FaceID id = static_cast<FaceID>(m_Faces.size());
    m_Faces.push_back(EMFace{});
    m_Faces[id].id = id;
    return id;
}

void EditableMesh::FreeVertex(VertexID id) {
    if (id >= m_Vertices.size()) return;
    m_Vertices[id].id = INVALID_ID;
    m_FreeVertices.push_back(id);
    m_Selection.vertices.erase(id);
}

void EditableMesh::FreeEdge(EdgeID id) {
    if (id >= m_Edges.size()) return;
    
    // Remove from edge map
    EMEdge& e = m_Edges[id];
    if (e.v0 != INVALID_ID && e.v1 != INVALID_ID) {
        m_EdgeMap.erase(EdgeKey(e.v0, e.v1));
    }
    
    e.id = INVALID_ID;
    m_FreeEdges.push_back(id);
    m_Selection.edges.erase(id);
}

void EditableMesh::FreeLoop(LoopID id) {
    if (id >= m_Loops.size()) return;
    m_Loops[id].id = INVALID_ID;
    m_FreeLoops.push_back(id);
}

void EditableMesh::FreeFace(FaceID id) {
    if (id >= m_Faces.size()) return;
    m_Faces[id].id = INVALID_ID;
    m_FreeFaces.push_back(id);
    m_Selection.faces.erase(id);
}

// ============================================================================
// Edge Management
// ============================================================================

EdgeID EditableMesh::FindEdge(VertexID v0, VertexID v1) const {
    auto it = m_EdgeMap.find(EdgeKey(v0, v1));
    if (it != m_EdgeMap.end()) return it->second;
    return INVALID_ID;
}

EdgeID EditableMesh::FindOrCreateEdge(VertexID v0, VertexID v1) {
    EdgeID existing = FindEdge(v0, v1);
    if (existing != INVALID_ID) return existing;
    
    EdgeID eid = AllocEdge();
    EMEdge& e = m_Edges[eid];
    e.v0 = v0;
    e.v1 = v1;
    
    m_EdgeMap[EdgeKey(v0, v1)] = eid;
    
    // Link to vertices
    LinkEdgeToVertex(eid, v0);
    LinkEdgeToVertex(eid, v1);
    
    return eid;
}

void EditableMesh::LinkEdgeToVertex(EdgeID eid, VertexID vid) {
    EMVertex* v = GetVertex(vid);
    EMEdge* e = GetEdge(eid);
    if (!v || !e) return;
    
    if (v->edge == INVALID_ID) {
        // First edge for this vertex - self-loop
        v->edge = eid;
        if (e->v0 == vid) {
            e->nextEdgeV0 = eid;
        } else {
            e->nextEdgeV1 = eid;
        }
    } else {
        // Insert into the circular list
        EdgeID firstEdge = v->edge;
        EMEdge* first = GetEdge(firstEdge);
        if (!first) return;
        
        // Find the last edge in the ring
        EdgeID lastEdge = firstEdge;
        EMEdge* last = first;
        EdgeID nextInRing = (last->v0 == vid) ? last->nextEdgeV0 : last->nextEdgeV1;
        
        while (nextInRing != firstEdge && nextInRing != INVALID_ID) {
            lastEdge = nextInRing;
            last = GetEdge(lastEdge);
            if (!last) break;
            nextInRing = (last->v0 == vid) ? last->nextEdgeV0 : last->nextEdgeV1;
        }
        
        // Insert new edge after last
        if (last->v0 == vid) {
            last->nextEdgeV0 = eid;
        } else {
            last->nextEdgeV1 = eid;
        }
        
        if (e->v0 == vid) {
            e->nextEdgeV0 = firstEdge;
        } else {
            e->nextEdgeV1 = firstEdge;
        }
    }
}

void EditableMesh::UnlinkEdgeFromVertex(EdgeID eid, VertexID vid) {
    EMVertex* v = GetVertex(vid);
    EMEdge* e = GetEdge(eid);
    if (!v || !e || v->edge == INVALID_ID) return;
    
    // Find previous edge in ring
    EdgeID current = v->edge;
    EdgeID prev = INVALID_ID;
    
    do {
        EMEdge* curr = GetEdge(current);
        if (!curr) break;
        
        EdgeID next = (curr->v0 == vid) ? curr->nextEdgeV0 : curr->nextEdgeV1;
        
        if (next == eid) {
            prev = current;
            break;
        }
        
        current = next;
    } while (current != v->edge && current != INVALID_ID);
    
    EdgeID nextAfterE = (e->v0 == vid) ? e->nextEdgeV0 : e->nextEdgeV1;
    
    if (prev != INVALID_ID) {
        EMEdge* prevEdge = GetEdge(prev);
        if (prevEdge) {
            if (prevEdge->v0 == vid) {
                prevEdge->nextEdgeV0 = nextAfterE;
            } else {
                prevEdge->nextEdgeV1 = nextAfterE;
            }
        }
    }
    
    if (v->edge == eid) {
        v->edge = (nextAfterE != eid) ? nextAfterE : INVALID_ID;
    }
}

void EditableMesh::LinkLoopToEdge(LoopID lid, EdgeID eid) {
    EMEdge* e = GetEdge(eid);
    if (!e) return;
    
    if (e->loop0 == INVALID_ID) {
        e->loop0 = lid;
    } else if (e->loop1 == INVALID_ID) {
        e->loop1 = lid;
    } else {
        LUCENT_CORE_WARN("Edge {} already has 2 loops, mesh may be non-manifold", eid);
    }
}

void EditableMesh::UnlinkLoopFromEdge(LoopID lid, EdgeID eid) {
    EMEdge* e = GetEdge(eid);
    if (!e) return;
    
    if (e->loop0 == lid) {
        e->loop0 = e->loop1;
        e->loop1 = INVALID_ID;
    } else if (e->loop1 == lid) {
        e->loop1 = INVALID_ID;
    }
}

// ============================================================================
// Topology Modification
// ============================================================================

VertexID EditableMesh::AddVertex(const glm::vec3& position) {
    VertexID vid = AllocVertex();
    m_Vertices[vid].position = position;
    return vid;
}

FaceID EditableMesh::AddFace(const std::vector<VertexID>& vertexIds) {
    if (vertexIds.size() < 3) return INVALID_ID;
    
    FaceID fid = AllocFace();
    EMFace& face = m_Faces[fid];
    face.vertCount = static_cast<uint32_t>(vertexIds.size());
    
    // Create loops
    std::vector<LoopID> loops;
    loops.reserve(vertexIds.size());
    
    for (size_t i = 0; i < vertexIds.size(); ++i) {
        LoopID lid = AllocLoop();
        EMLoop& loop = m_Loops[lid];
        loop.vertex = vertexIds[i];
        loop.face = fid;
        
        // Edge from this vertex to next
        size_t nextIdx = (i + 1) % vertexIds.size();
        EdgeID eid = FindOrCreateEdge(vertexIds[i], vertexIds[nextIdx]);
        loop.edge = eid;
        
        // Link loop to edge
        LinkLoopToEdge(lid, eid);
        
        // Copy UV from vertex
        const EMVertex* v = GetVertex(vertexIds[i]);
        if (v) loop.uv = v->uv;
        
        loops.push_back(lid);
    }
    
    // Link loops in circular list
    for (size_t i = 0; i < loops.size(); ++i) {
        size_t prevIdx = (i + loops.size() - 1) % loops.size();
        size_t nextIdx = (i + 1) % loops.size();
        
        m_Loops[loops[i]].prev = loops[prevIdx];
        m_Loops[loops[i]].next = loops[nextIdx];
    }
    
    face.loopStart = loops[0];
    
    // Calculate face normal
    RecalculateFaceNormal(fid);
    
    return fid;
}

void EditableMesh::RemoveVertex(VertexID vid) {
    // First remove all faces that use this vertex
    auto faces = GetVertexFaces(vid);
    for (FaceID fid : faces) {
        RemoveFace(fid);
    }
    
    // Remove any remaining edges
    auto edges = GetVertexEdges(vid);
    for (EdgeID eid : edges) {
        RemoveEdge(eid);
    }
    
    FreeVertex(vid);
}

void EditableMesh::RemoveFace(FaceID fid) {
    EMFace* face = GetFace(fid);
    if (!face) return;
    
    // Collect loops and unlink from edges
    std::vector<LoopID> loopsToRemove;
    ForEachFaceLoop(fid, [&](const EMLoop& loop) {
        loopsToRemove.push_back(loop.id);
    });
    
    for (LoopID lid : loopsToRemove) {
        EMLoop* loop = GetLoop(lid);
        if (loop && loop->edge != INVALID_ID) {
            UnlinkLoopFromEdge(lid, loop->edge);
        }
        FreeLoop(lid);
    }
    
    FreeFace(fid);
}

void EditableMesh::RemoveEdge(EdgeID eid) {
    EMEdge* e = GetEdge(eid);
    if (!e) return;
    
    // Remove faces that use this edge
    if (e->loop0 != INVALID_ID) {
        EMLoop* loop = GetLoop(e->loop0);
        if (loop) RemoveFace(loop->face);
    }
    if (e->loop1 != INVALID_ID) {
        EMLoop* loop = GetLoop(e->loop1);
        if (loop) RemoveFace(loop->face);
    }
    
    // Unlink from vertices
    if (e->v0 != INVALID_ID) UnlinkEdgeFromVertex(eid, e->v0);
    if (e->v1 != INVALID_ID) UnlinkEdgeFromVertex(eid, e->v1);
    
    FreeEdge(eid);
}

// ============================================================================
// Geometry
// ============================================================================

void EditableMesh::RecalculateNormals() {
    // Reset all vertex normals
    for (auto& v : m_Vertices) {
        if (v.id != INVALID_ID) {
            v.normal = glm::vec3(0.0f);
        }
    }
    
    // Calculate face normals and accumulate to vertices
    for (auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        RecalculateFaceNormal(face.id);
        
        // Add face normal to all vertices
        ForEachFaceLoop(face.id, [&](const EMLoop& loop) {
            EMVertex* v = GetVertex(loop.vertex);
            if (v) v->normal += face.normal;
        });
    }
    
    // Normalize vertex normals
    for (auto& v : m_Vertices) {
        if (v.id != INVALID_ID) {
            float len = glm::length(v.normal);
            if (len > 0.0001f) {
                v.normal /= len;
            } else {
                v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
    }
}

void EditableMesh::RecalculateFaceNormal(FaceID fid) {
    EMFace* face = GetFace(fid);
    if (!face) return;
    
    // Newell's method for polygon normal
    glm::vec3 normal(0.0f);
    
    std::vector<glm::vec3> positions;
    ForEachFaceVertex(fid, [&](const EMVertex& v) {
        positions.push_back(v.position);
    });
    
    if (positions.size() < 3) return;
    
    for (size_t i = 0; i < positions.size(); ++i) {
        const glm::vec3& current = positions[i];
        const glm::vec3& next = positions[(i + 1) % positions.size()];
        
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }
    
    float len = glm::length(normal);
    if (len > 0.0001f) {
        face->normal = normal / len;
    } else {
        face->normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

glm::vec3 EditableMesh::CalculateFaceCenter(FaceID fid) const {
    glm::vec3 center(0.0f);
    uint32_t count = 0;
    
    ForEachFaceVertex(fid, [&](const EMVertex& v) {
        center += v.position;
        ++count;
    });
    
    if (count > 0) center /= static_cast<float>(count);
    return center;
}

// ============================================================================
// Orientation / Winding
// ============================================================================

namespace {
struct DirectedEdge {
    VertexID a = INVALID_ID;
    VertexID b = INVALID_ID;
};
} // namespace

void EditableMesh::MakeWindingConsistent() {
    // BFS across manifold edges:
    // Adjacent faces must traverse shared edges in opposite directions.
    std::unordered_map<FaceID, bool> visited;
    std::unordered_map<FaceID, bool> flip;
    
    auto directedEdgeForLoop = [this](LoopID lid) -> DirectedEdge {
        const EMLoop* l = GetLoop(lid);
        if (!l) return {};
        const EMLoop* n = GetLoop(l->next);
        if (!n) return {};
        return { l->vertex, n->vertex };
    };
    
    auto flipFaceInPlace = [this](FaceID fid) {
        // Collect loops in current order
        std::vector<LoopID> loops;
        ForEachFaceLoop(fid, [&](const EMLoop& loop) { loops.push_back(loop.id); });
        if (loops.size() < 3) return;
        
        // Capture corner data (vertex + per-loop UV)
        std::vector<VertexID> verts;
        std::vector<glm::vec2> uvs;
        verts.reserve(loops.size());
        uvs.reserve(loops.size());
        for (LoopID lid : loops) {
            const EMLoop* l = GetLoop(lid);
            if (!l) return;
            verts.push_back(l->vertex);
            uvs.push_back(l->uv);
        }
        
        std::reverse(verts.begin(), verts.end());
        std::reverse(uvs.begin(), uvs.end());
        
        // Unlink old edge usage, update vertex/uv
        for (size_t i = 0; i < loops.size(); ++i) {
            LoopID lid = loops[i];
            EMLoop* l = GetLoop(lid);
            if (!l) continue;
            if (l->edge != INVALID_ID) {
                UnlinkLoopFromEdge(lid, l->edge);
            }
            l->vertex = verts[i];
            l->uv = uvs[i];
        }
        
        // Rebuild edges based on (vertex -> next vertex) in the loop ring
        for (size_t i = 0; i < loops.size(); ++i) {
            LoopID lid = loops[i];
            EMLoop* l = GetLoop(lid);
            if (!l) continue;
            
            VertexID v0 = l->vertex;
            VertexID v1 = GetLoop(loops[(i + 1) % loops.size()])->vertex;
            EdgeID eid = FindOrCreateEdge(v0, v1);
            
            l->edge = eid;
            LinkLoopToEdge(lid, eid);
        }
        
        RecalculateFaceNormal(fid);
    };
    
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        FaceID startF = face.id;
        if (visited[startF]) continue;
        
        visited[startF] = true;
        flip[startF] = false;
        
        std::queue<FaceID> q;
        q.push(startF);
        
        while (!q.empty()) {
            FaceID f = q.front();
            q.pop();
            
            ForEachFaceLoop(f, [&](const EMLoop& loop) {
                EdgeID eid = loop.edge;
                const EMEdge* e = GetEdge(eid);
                if (!e) return;
                
                // Only propagate across edges with two loops (manifold interior edges)
                if (e->loop0 == INVALID_ID || e->loop1 == INVALID_ID) return;
                
                // Find the neighbor loop on this edge
                LoopID otherLid = (e->loop0 == loop.id) ? e->loop1 : (e->loop1 == loop.id ? e->loop0 : INVALID_ID);
                if (otherLid == INVALID_ID) return;
                
                const EMLoop* otherLoop = GetLoop(otherLid);
                if (!otherLoop) return;
                FaceID g = otherLoop->face;
                if (g == INVALID_ID || g == f) return;
                
                DirectedEdge df = directedEdgeForLoop(loop.id);
                DirectedEdge dg = directedEdgeForLoop(otherLid);
                if (df.a == INVALID_ID || dg.a == INVALID_ID) return;
                
                bool sameDir = (df.a == dg.a && df.b == dg.b);
                bool desiredFlipG = flip[f] ^ sameDir;
                
                if (!visited[g]) {
                    visited[g] = true;
                    flip[g] = desiredFlipG;
                    q.push(g);
                }
            });
        }
    }
    
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        if (flip[face.id]) {
            flipFaceInPlace(face.id);
        }
    }
}

float EditableMesh::ComputeSignedVolume() const {
    // Signed volume using triangulated faces. Positive => outward CCW winding.
    double volume = 0.0;
    
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        
        std::vector<glm::vec3> facePositions;
        facePositions.reserve(face.vertCount);
        ForEachFaceVertex(face.id, [&](const EMVertex& v) {
            facePositions.push_back(v.position);
        });
        if (facePositions.size() < 3) continue;
        
        // Use current face normal for triangulation
        std::vector<uint32_t> tri = Triangulator::Triangulate(facePositions, face.normal);
        if (tri.size() < 3) continue;
        
        for (size_t i = 0; i + 2 < tri.size(); i += 3) {
            const glm::vec3& p0 = facePositions[tri[i + 0]];
            const glm::vec3& p1 = facePositions[tri[i + 1]];
            const glm::vec3& p2 = facePositions[tri[i + 2]];
            volume += glm::dot(p0, glm::cross(p1, p2)) / 6.0;
        }
    }
    
    return static_cast<float>(volume);
}

void EditableMesh::MakeWindingConsistentAndOutward() {
    MakeWindingConsistent();
    
    // If mesh is open (any boundary edge), outward is ambiguous; we stop after consistency.
    for (const auto& e : m_Edges) {
        if (e.id == INVALID_ID) continue;
        if (e.loop0 == INVALID_ID || e.loop1 == INVALID_ID) {
            RecalculateNormals();
            return;
        }
    }
    
    // Decide global outward by signed volume
    float vol = ComputeSignedVolume();
    if (vol < 0.0f) {
        // Flip every face once
        for (const auto& face : m_Faces) {
            if (face.id == INVALID_ID) continue;
            
            std::vector<LoopID> loops;
            ForEachFaceLoop(face.id, [&](const EMLoop& loop) { loops.push_back(loop.id); });
            if (loops.size() < 3) continue;
            
            std::vector<VertexID> verts;
            std::vector<glm::vec2> uvs;
            verts.reserve(loops.size());
            uvs.reserve(loops.size());
            for (LoopID lid : loops) {
                const EMLoop* l = GetLoop(lid);
                if (!l) continue;
                verts.push_back(l->vertex);
                uvs.push_back(l->uv);
            }
            std::reverse(verts.begin(), verts.end());
            std::reverse(uvs.begin(), uvs.end());
            
            for (size_t i = 0; i < loops.size(); ++i) {
                LoopID lid = loops[i];
                EMLoop* l = GetLoop(lid);
                if (!l) continue;
                if (l->edge != INVALID_ID) UnlinkLoopFromEdge(lid, l->edge);
                l->vertex = verts[i];
                l->uv = uvs[i];
            }
            for (size_t i = 0; i < loops.size(); ++i) {
                LoopID lid = loops[i];
                EMLoop* l = GetLoop(lid);
                if (!l) continue;
                VertexID v0 = l->vertex;
                VertexID v1 = GetLoop(loops[(i + 1) % loops.size()])->vertex;
                EdgeID eid = FindOrCreateEdge(v0, v1);
                l->edge = eid;
                LinkLoopToEdge(lid, eid);
            }
            RecalculateFaceNormal(face.id);
        }
    }
    
    RecalculateNormals();
}

// ============================================================================
// Selection
// ============================================================================

void EditableMesh::SelectVertex(VertexID vid, bool add) {
    if (!add) m_Selection.Clear();
    EMVertex* v = GetVertex(vid);
    if (v) {
        v->selected = true;
        m_Selection.vertices.insert(vid);
    }
}

void EditableMesh::SelectEdge(EdgeID eid, bool add) {
    if (!add) m_Selection.Clear();
    EMEdge* e = GetEdge(eid);
    if (e) {
        e->selected = true;
        m_Selection.edges.insert(eid);
    }
}

void EditableMesh::SelectFace(FaceID fid, bool add) {
    if (!add) m_Selection.Clear();
    EMFace* f = GetFace(fid);
    if (f) {
        f->selected = true;
        m_Selection.faces.insert(fid);
    }
}

void EditableMesh::SelectAll() {
    for (auto& v : m_Vertices) {
        if (v.id != INVALID_ID) {
            v.selected = true;
            m_Selection.vertices.insert(v.id);
        }
    }
    for (auto& e : m_Edges) {
        if (e.id != INVALID_ID) {
            e.selected = true;
            m_Selection.edges.insert(e.id);
        }
    }
    for (auto& f : m_Faces) {
        if (f.id != INVALID_ID) {
            f.selected = true;
            m_Selection.faces.insert(f.id);
        }
    }
}

void EditableMesh::DeselectAll() {
    for (auto& v : m_Vertices) v.selected = false;
    for (auto& e : m_Edges) e.selected = false;
    for (auto& f : m_Faces) f.selected = false;
    m_Selection.Clear();
}

void EditableMesh::SelectionVertsToEdges() {
    for (VertexID vid : m_Selection.vertices) {
        auto edges = GetVertexEdges(vid);
        for (EdgeID eid : edges) {
            EMEdge* e = GetEdge(eid);
            if (e && m_Selection.vertices.count(e->v0) && m_Selection.vertices.count(e->v1)) {
                e->selected = true;
                m_Selection.edges.insert(eid);
            }
        }
    }
}

void EditableMesh::SelectionVertsToFaces() {
    for (auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        
        bool allSelected = true;
        ForEachFaceLoop(face.id, [&](const EMLoop& loop) {
            if (m_Selection.vertices.find(loop.vertex) == m_Selection.vertices.end()) {
                allSelected = false;
            }
        });
        
        if (allSelected) {
            face.selected = true;
            m_Selection.faces.insert(face.id);
        }
    }
}

void EditableMesh::SelectionEdgesToVerts() {
    for (EdgeID eid : m_Selection.edges) {
        EMEdge* e = GetEdge(eid);
        if (e) {
            if (EMVertex* v0 = GetVertex(e->v0)) {
                v0->selected = true;
                m_Selection.vertices.insert(e->v0);
            }
            if (EMVertex* v1 = GetVertex(e->v1)) {
                v1->selected = true;
                m_Selection.vertices.insert(e->v1);
            }
        }
    }
}

void EditableMesh::SelectionEdgesToFaces() {
    for (EdgeID eid : m_Selection.edges) {
        auto faces = GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            // Check if all edges of face are selected
            bool allEdgesSelected = true;
            ForEachFaceLoop(fid, [&](const EMLoop& loop) {
                if (m_Selection.edges.find(loop.edge) == m_Selection.edges.end()) {
                    allEdgesSelected = false;
                }
            });
            
            if (allEdgesSelected) {
                EMFace* f = GetFace(fid);
                if (f) {
                    f->selected = true;
                    m_Selection.faces.insert(fid);
                }
            }
        }
    }
}

void EditableMesh::SelectionFacesToVerts() {
    for (FaceID fid : m_Selection.faces) {
        ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            EMVertex* v = GetVertex(loop.vertex);
            if (v) {
                v->selected = true;
                m_Selection.vertices.insert(loop.vertex);
            }
        });
    }
}

void EditableMesh::SelectionFacesToEdges() {
    for (FaceID fid : m_Selection.faces) {
        ForEachFaceLoop(fid, [&](const EMLoop& loop) {
            EMEdge* e = GetEdge(loop.edge);
            if (e) {
                e->selected = true;
                m_Selection.edges.insert(loop.edge);
            }
        });
    }
}

// ============================================================================
// Validation
// ============================================================================

bool EditableMesh::IsValid() const {
    // Basic validation
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        if (face.loopStart == INVALID_ID) return false;
        if (face.vertCount < 3) return false;
    }
    
    return true;
}

// ============================================================================
// Construction / Conversion
// ============================================================================

EditableMesh EditableMesh::FromTriangles(
    const std::vector<glm::vec3>& positions,
    const std::vector<glm::vec3>& normals,
    const std::vector<glm::vec2>& uvs,
    const std::vector<uint32_t>& indices
) {
    EditableMesh mesh;
    
    // Add vertices
    for (size_t i = 0; i < positions.size(); ++i) {
        VertexID vid = mesh.AddVertex(positions[i]);
        if (i < normals.size()) mesh.m_Vertices[vid].normal = normals[i];
        if (i < uvs.size()) mesh.m_Vertices[vid].uv = uvs[i];
    }
    
    // Add triangle faces
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        VertexID i0 = indices[i];
        VertexID i1 = indices[i + 1];
        VertexID i2 = indices[i + 2];
        
        // Ensure winding matches provided vertex normals (important for edit ops like Extrude which use face normals).
        // If the triangle's geometric normal points opposite the average of its vertex normals, flip winding.
        if (i0 < positions.size() && i1 < positions.size() && i2 < positions.size() &&
            i0 < normals.size() && i1 < normals.size() && i2 < normals.size()) {
            const glm::vec3& p0 = positions[i0];
            const glm::vec3& p1 = positions[i1];
            const glm::vec3& p2 = positions[i2];
            
            glm::vec3 triN = glm::cross(p1 - p0, p2 - p0);
            float triLen = glm::length(triN);
            if (triLen > 1e-6f) {
                triN /= triLen;
                glm::vec3 avgN = glm::normalize(normals[i0] + normals[i1] + normals[i2]);
                if (glm::length(avgN) > 1e-6f) {
                    if (glm::dot(triN, avgN) < 0.0f) {
                        std::swap(i1, i2);
                    }
                }
            }
        }
        
        std::vector<VertexID> faceVerts = { i0, i1, i2 };
        mesh.AddFace(faceVerts);
    }
    
    return std::move(mesh);
}

EditableMesh EditableMesh::FromFaces(
    const std::vector<glm::vec3>& positions,
    const std::vector<std::vector<uint32_t>>& faceVertexIndices
) {
    EditableMesh mesh;
    
    // Add vertices
    for (const auto& pos : positions) {
        mesh.AddVertex(pos);
    }
    
    // Add faces
    for (const auto& faceIndices : faceVertexIndices) {
        std::vector<VertexID> vids;
        vids.reserve(faceIndices.size());
        for (uint32_t idx : faceIndices) {
            vids.push_back(idx);
        }
        mesh.AddFace(vids);
    }
    
    mesh.RecalculateNormals();
    return std::move(mesh);
}

TriangleOutput EditableMesh::ToTriangles() const {
    TriangleOutput output;
    
    // Triangulate each face
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        
        // Collect face vertices
        std::vector<glm::vec3> facePositions;
        std::vector<glm::vec3> faceNormals;
        std::vector<glm::vec2> faceUVs;
        
        ForEachFaceLoop(face.id, [&](const EMLoop& loop) {
            const EMVertex* v = GetVertex(loop.vertex);
            if (v) {
                facePositions.push_back(v->position);
                faceNormals.push_back(v->normal);
                faceUVs.push_back(loop.uv);
            }
        });
        
        if (facePositions.size() < 3) continue;
        
        // Triangulate
        std::vector<uint32_t> triIndices = Triangulator::Triangulate(facePositions, face.normal);
        
        // Add to output
        uint32_t baseVertex = static_cast<uint32_t>(output.vertices.size());
        
        for (size_t i = 0; i < facePositions.size(); ++i) {
            TriangleOutput::Vertex v;
            v.position = facePositions[i];
            v.normal = faceNormals[i];
            v.uv = faceUVs[i];
            
            // Calculate tangent (simplified - uses face normal)
            glm::vec3 tangent = glm::normalize(glm::cross(face.normal, glm::vec3(0, 1, 0)));
            if (glm::length(tangent) < 0.001f) {
                tangent = glm::normalize(glm::cross(face.normal, glm::vec3(1, 0, 0)));
            }
            v.tangent = glm::vec4(tangent, 1.0f);
            
            output.vertices.push_back(v);
        }
        
        for (uint32_t idx : triIndices) {
            output.indices.push_back(baseVertex + idx);
        }
    }
    
    return output;
}

// ============================================================================
// Serialization
// ============================================================================

EditableMesh::SerializedData EditableMesh::Serialize() const {
    SerializedData data;
    
    // Build vertex index remapping (skip free slots)
    std::unordered_map<VertexID, uint32_t> vertexRemap;
    for (const auto& v : m_Vertices) {
        if (v.id != INVALID_ID) {
            uint32_t newIdx = static_cast<uint32_t>(data.positions.size());
            vertexRemap[v.id] = newIdx;
            data.positions.push_back(v.position);
            data.uvs.push_back(v.uv);
        }
    }
    
    // Serialize faces
    for (const auto& face : m_Faces) {
        if (face.id == INVALID_ID) continue;
        
        std::vector<uint32_t> faceIndices;
        ForEachFaceLoop(face.id, [&](const EMLoop& loop) {
            auto it = vertexRemap.find(loop.vertex);
            if (it != vertexRemap.end()) {
                faceIndices.push_back(it->second);
            }
        });
        
        if (faceIndices.size() >= 3) {
            data.faceVertexIndices.push_back(faceIndices);
        }
    }
    
    return data;
}

EditableMesh EditableMesh::Deserialize(const SerializedData& data) {
    EditableMesh mesh;
    
    // Add vertices
    for (size_t i = 0; i < data.positions.size(); ++i) {
        VertexID vid = mesh.AddVertex(data.positions[i]);
        if (i < data.uvs.size()) {
            mesh.m_Vertices[vid].uv = data.uvs[i];
        }
    }
    
    // Add faces
    for (const auto& faceIndices : data.faceVertexIndices) {
        std::vector<VertexID> vids(faceIndices.begin(), faceIndices.end());
        mesh.AddFace(vids);
    }
    
    mesh.RecalculateNormals();
    return std::move(mesh);
}

} // namespace lucent::mesh
