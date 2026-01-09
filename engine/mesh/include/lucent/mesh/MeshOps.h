#pragma once

#include "lucent/mesh/EditableMesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace lucent::mesh {

// Mesh editing operations
// All operations modify the mesh in-place and return success/failure
namespace MeshOps {

// ============================================================================
// Transform Operations
// ============================================================================

// Move selected vertices by offset
void TranslateSelection(EditableMesh& mesh, const glm::vec3& offset);

// Rotate selected vertices around pivot
void RotateSelection(EditableMesh& mesh, const glm::vec3& pivot, const glm::quat& rotation);

// Scale selected vertices around pivot
void ScaleSelection(EditableMesh& mesh, const glm::vec3& pivot, const glm::vec3& scale);

// ============================================================================
// Topology Operations
// ============================================================================

// Extrude selected faces along their normals (or custom direction)
// Returns the new face IDs created
std::vector<FaceID> ExtrudeFaces(EditableMesh& mesh, float distance = 0.0f);

// Extrude selected edges
std::vector<FaceID> ExtrudeEdges(EditableMesh& mesh, const glm::vec3& direction, float distance);

// Inset selected faces (create inner faces)
std::vector<FaceID> InsetFaces(EditableMesh& mesh, float thickness);

// Bevel selected edges
void BevelEdges(EditableMesh& mesh, float width, int segments = 1);

// Loop cut: insert an edge loop
// Returns the new edge IDs created
std::vector<EdgeID> LoopCut(EditableMesh& mesh, EdgeID startEdge, float position = 0.5f);

// Merge selected vertices at center
VertexID MergeVerticesAtCenter(EditableMesh& mesh);

// Merge selected vertices at last selected
VertexID MergeVerticesAtLast(EditableMesh& mesh);

// ============================================================================
// Delete Operations
// ============================================================================

// Delete selected vertices (and connected faces)
void DeleteVertices(EditableMesh& mesh);

// Delete selected edges (and connected faces)
void DeleteEdges(EditableMesh& mesh);

// Delete selected faces only (keep edges/verts)
void DeleteFaces(EditableMesh& mesh);

// Dissolve selected vertices (merge edges through vertex)
void DissolveVertices(EditableMesh& mesh);

// Dissolve selected edges (merge adjacent faces)
void DissolveEdges(EditableMesh& mesh);

// ============================================================================
// Selection Operations
// ============================================================================

// Select edge loop starting from an edge
void SelectEdgeLoop(EditableMesh& mesh, EdgeID startEdge);

// Select edge ring (perpendicular to loop)
void SelectEdgeRing(EditableMesh& mesh, EdgeID startEdge);

// Grow selection
void GrowSelection(EditableMesh& mesh);

// Shrink selection
void ShrinkSelection(EditableMesh& mesh);

// ============================================================================
// Utility Operations
// ============================================================================

// Weld vertices by position (merge within distance threshold).
// This rebuilds topology and preserves per-loop UVs where possible.
// Typical values: 1e-6 .. 1e-3 depending on your asset scale.
void WeldVerticesByDistance(EditableMesh& mesh, float distance);

// Flip normals of selected faces
void FlipNormals(EditableMesh& mesh);

// Recalculate normals for selected faces
void RecalculateNormals(EditableMesh& mesh);

// Subdivide selected faces
void SubdivideFaces(EditableMesh& mesh, int cuts = 1);

// Triangulate selected faces (convert ngons to triangles)
void TriangulateFaces(EditableMesh& mesh);

} // namespace MeshOps

} // namespace lucent::mesh
