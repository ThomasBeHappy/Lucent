#include "lucent/mesh/MeshOps.h"
#include "lucent/core/Log.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace lucent::mesh {
namespace MeshOps {

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
    // TODO: Implement full bevel
    // For MVP, just offset vertices slightly
    const auto& selection = mesh.GetSelection();
    
    if (selection.edges.empty()) return;
    
    // Simple bevel: move edge vertices perpendicular to edge
    for (EdgeID eid : selection.edges) {
        const EMEdge* e = mesh.GetEdge(eid);
        if (!e) continue;
        
        EMVertex* v0 = mesh.GetVertex(e->v0);
        EMVertex* v1 = mesh.GetVertex(e->v1);
        if (!v0 || !v1) continue;
        
        glm::vec3 edgeDir = glm::normalize(v1->position - v0->position);
        
        // Find perpendicular direction (average of adjacent face normals)
        glm::vec3 perpDir(0.0f);
        auto faces = mesh.GetEdgeFaces(eid);
        for (FaceID fid : faces) {
            const EMFace* f = mesh.GetFace(fid);
            if (f) perpDir += f->normal;
        }
        if (glm::length(perpDir) > 0.001f) {
            perpDir = glm::normalize(perpDir);
        } else {
            perpDir = glm::vec3(0, 1, 0);
        }
        
        glm::vec3 offsetDir = glm::normalize(glm::cross(edgeDir, perpDir));
        
        v0->position += offsetDir * width * 0.5f;
        v1->position += offsetDir * width * 0.5f;
    }
    
    (void)segments; // TODO: Use for multi-segment bevel
    mesh.RecalculateNormals();
}

std::vector<EdgeID> LoopCut(EditableMesh& mesh, EdgeID startEdge, float position) {
    std::vector<EdgeID> newEdges;
    
    // TODO: Implement full loop cut
    // For MVP, just split the starting edge
    
    const EMEdge* e = mesh.GetEdge(startEdge);
    if (!e) return newEdges;
    
    const EMVertex* v0 = mesh.GetVertex(e->v0);
    const EMVertex* v1 = mesh.GetVertex(e->v1);
    if (!v0 || !v1) return newEdges;
    
    // Create new vertex at position along edge
    glm::vec3 newPos = glm::mix(v0->position, v1->position, position);
    VertexID newVid = mesh.AddVertex(newPos);
    (void)newVid; // TODO: Use when implementing full loop cut
    
    // TODO: Split edge and update topology
    // This is complex and requires splitting adjacent faces
    
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
    // TODO: Implement vertex dissolve (merge edges through vertex)
    LUCENT_CORE_WARN("DissolveVertices not fully implemented");
    DeleteVertices(mesh);
}

void DissolveEdges(EditableMesh& mesh) {
    // TODO: Implement edge dissolve (merge adjacent faces)
    LUCENT_CORE_WARN("DissolveEdges not fully implemented");
    DeleteEdges(mesh);
}

// ============================================================================
// Selection Operations
// ============================================================================

void SelectEdgeLoop(EditableMesh& mesh, EdgeID startEdge) {
    // TODO: Implement edge loop selection
    mesh.SelectEdge(startEdge, true);
}

void SelectEdgeRing(EditableMesh& mesh, EdgeID startEdge) {
    // TODO: Implement edge ring selection
    mesh.SelectEdge(startEdge, true);
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
    // TODO: Implement shrink selection
    (void)mesh;
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
    // TODO: Implement face subdivision
    (void)mesh;
    (void)cuts;
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
