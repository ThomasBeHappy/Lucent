#pragma once

#include "lucent/core/Core.h"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <functional>

namespace lucent::mesh {

// Forward declarations
struct EMVertex;
struct EMEdge;
struct EMLoop;
struct EMFace;
class EditableMesh;

// Unique IDs for mesh elements (stable across edits)
using VertexID = uint32_t;
using EdgeID = uint32_t;
using LoopID = uint32_t;
using FaceID = uint32_t;

constexpr uint32_t INVALID_ID = UINT32_MAX;

// Vertex data
struct EMVertex {
    VertexID id = INVALID_ID;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};  // Average normal (computed from faces)
    glm::vec2 uv{0.0f};
    
    // Connectivity: one edge that uses this vertex (for traversal)
    EdgeID edge = INVALID_ID;
    
    // Selection state
    bool selected = false;
};

// Edge data (undirected edge between two vertices)
struct EMEdge {
    EdgeID id = INVALID_ID;
    
    // The two vertices of this edge
    VertexID v0 = INVALID_ID;
    VertexID v1 = INVALID_ID;
    
    // Loops that use this edge (one per adjacent face, max 2 for manifold)
    LoopID loop0 = INVALID_ID;  // First face's loop
    LoopID loop1 = INVALID_ID;  // Second face's loop (INVALID_ID if boundary)
    
    // For fast vertex->edge traversal: next edge around v0 and v1
    EdgeID nextEdgeV0 = INVALID_ID;
    EdgeID nextEdgeV1 = INVALID_ID;
    
    // Selection state
    bool selected = false;
    
    // Is this a boundary edge?
    bool IsBoundary() const { return loop1 == INVALID_ID; }
    
    // Get the other vertex
    VertexID OtherVertex(VertexID v) const { return (v == v0) ? v1 : v0; }
};

// Loop: a corner of a face (vertex + edge reference within a face)
struct EMLoop {
    LoopID id = INVALID_ID;
    
    VertexID vertex = INVALID_ID;  // The vertex at this corner
    EdgeID edge = INVALID_ID;      // The edge following this corner (going CCW)
    FaceID face = INVALID_ID;      // The face this loop belongs to
    
    // Circular linked list within the face
    LoopID next = INVALID_ID;
    LoopID prev = INVALID_ID;
    
    // Per-loop UV (can differ from vertex UV for split UVs)
    glm::vec2 uv{0.0f};
};

// Face: an ngon defined by a loop of vertices
struct EMFace {
    FaceID id = INVALID_ID;
    
    // First loop in the circular list
    LoopID loopStart = INVALID_ID;
    
    // Cached face normal
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    
    // Number of vertices/edges in this face
    uint32_t vertCount = 0;
    
    // Selection state
    bool selected = false;
    
    // Material index for this face
    uint32_t materialIndex = 0;
};

// Triangle output for rendering
struct TriangleOutput {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
        glm::vec4 tangent;
    };
    
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Selection set
struct MeshSelection {
    std::unordered_set<VertexID> vertices;
    std::unordered_set<EdgeID> edges;
    std::unordered_set<FaceID> faces;
    
    void Clear() {
        vertices.clear();
        edges.clear();
        faces.clear();
    }
    
    bool Empty() const {
        return vertices.empty() && edges.empty() && faces.empty();
    }
};

// Main editable mesh class with ngon support
class EditableMesh : public NonCopyable {
public:
    EditableMesh() = default;
    ~EditableMesh() = default;
    
    // Move semantics
    EditableMesh(EditableMesh&&) = default;
    EditableMesh& operator=(EditableMesh&&) = default;
    
    // ========================================================================
    // Construction / Conversion
    // ========================================================================
    
    // Create from triangle mesh (groups triangles that share edges into ngons if possible)
    static EditableMesh FromTriangles(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<glm::vec2>& uvs,
        const std::vector<uint32_t>& indices
    );
    
    // Create from face-vertex representation (arbitrary ngons)
    static EditableMesh FromFaces(
        const std::vector<glm::vec3>& positions,
        const std::vector<std::vector<uint32_t>>& faceVertexIndices
    );
    
    // Convert to triangles for rendering
    TriangleOutput ToTriangles() const;
    
    // ========================================================================
    // Element Access
    // ========================================================================
    
    EMVertex* GetVertex(VertexID id);
    const EMVertex* GetVertex(VertexID id) const;
    
    EMEdge* GetEdge(EdgeID id);
    const EMEdge* GetEdge(EdgeID id) const;
    
    EMLoop* GetLoop(LoopID id);
    const EMLoop* GetLoop(LoopID id) const;
    
    EMFace* GetFace(FaceID id);
    const EMFace* GetFace(FaceID id) const;
    
    // ========================================================================
    // Iteration
    // ========================================================================
    
    const std::vector<EMVertex>& GetVertices() const { return m_Vertices; }
    const std::vector<EMEdge>& GetEdges() const { return m_Edges; }
    const std::vector<EMLoop>& GetLoops() const { return m_Loops; }
    const std::vector<EMFace>& GetFaces() const { return m_Faces; }
    
    size_t VertexCount() const { return m_Vertices.size() - m_FreeVertices.size(); }
    size_t EdgeCount() const { return m_Edges.size() - m_FreeEdges.size(); }
    size_t FaceCount() const { return m_Faces.size() - m_FreeFaces.size(); }
    
    // Iterate over face loops
    void ForEachFaceLoop(FaceID faceId, const std::function<void(const EMLoop&)>& fn) const;
    void ForEachFaceVertex(FaceID faceId, const std::function<void(const EMVertex&)>& fn) const;
    
    // Get edges around a vertex
    std::vector<EdgeID> GetVertexEdges(VertexID vid) const;
    
    // Get faces around a vertex
    std::vector<FaceID> GetVertexFaces(VertexID vid) const;
    
    // Get faces adjacent to an edge
    std::vector<FaceID> GetEdgeFaces(EdgeID eid) const;
    
    // ========================================================================
    // Topology Modification (Low-level)
    // ========================================================================
    
    VertexID AddVertex(const glm::vec3& position);
    FaceID AddFace(const std::vector<VertexID>& vertexIds);
    
    void RemoveVertex(VertexID vid);
    void RemoveFace(FaceID fid);
    void RemoveEdge(EdgeID eid);
    
    // ========================================================================
    // Geometry Operations
    // ========================================================================
    
    void RecalculateNormals();
    void RecalculateFaceNormal(FaceID fid);
    glm::vec3 CalculateFaceCenter(FaceID fid) const;
    
    // ========================================================================
    // Orientation / Winding
    // ========================================================================
    
    // Ensure all faces in each connected component have consistent winding
    // (adjacent faces traverse shared edges in opposite directions).
    void MakeWindingConsistent();
    
    // Compute signed volume (only meaningful for closed meshes). Positive means outward winding.
    float ComputeSignedVolume() const;
    
    // Blender-grade: make winding consistent, then ensure outward orientation using signed volume.
    // If the mesh is not closed, this will still make winding locally consistent where possible.
    void MakeWindingConsistentAndOutward();
    
    // ========================================================================
    // Selection
    // ========================================================================
    
    MeshSelection& GetSelection() { return m_Selection; }
    const MeshSelection& GetSelection() const { return m_Selection; }
    
    void SelectVertex(VertexID vid, bool add = false);
    void SelectEdge(EdgeID eid, bool add = false);
    void SelectFace(FaceID fid, bool add = false);
    void SelectAll();
    void DeselectAll();
    
    // Convert selection between modes
    void SelectionVertsToEdges();
    void SelectionVertsToFaces();
    void SelectionEdgesToVerts();
    void SelectionEdgesToFaces();
    void SelectionFacesToVerts();
    void SelectionFacesToEdges();
    
    // ========================================================================
    // Validation
    // ========================================================================
    
    bool IsValid() const;
    
    // ========================================================================
    // Serialization (for scene save/load)
    // ========================================================================
    
    struct SerializedData {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> uvs;
        std::vector<std::vector<uint32_t>> faceVertexIndices;
    };
    
    SerializedData Serialize() const;
    static EditableMesh Deserialize(const SerializedData& data);
    
private:
    // Find or create edge between two vertices
    EdgeID FindOrCreateEdge(VertexID v0, VertexID v1);
    EdgeID FindEdge(VertexID v0, VertexID v1) const;
    
    // Allocate/free elements
    VertexID AllocVertex();
    EdgeID AllocEdge();
    LoopID AllocLoop();
    FaceID AllocFace();
    
    void FreeVertex(VertexID id);
    void FreeEdge(EdgeID id);
    void FreeLoop(LoopID id);
    void FreeFace(FaceID id);
    
    // Link edge to vertex's edge ring
    void LinkEdgeToVertex(EdgeID eid, VertexID vid);
    void UnlinkEdgeFromVertex(EdgeID eid, VertexID vid);
    
    // Link loop to edge
    void LinkLoopToEdge(LoopID lid, EdgeID eid);
    void UnlinkLoopFromEdge(LoopID lid, EdgeID eid);
    
private:
    std::vector<EMVertex> m_Vertices;
    std::vector<EMEdge> m_Edges;
    std::vector<EMLoop> m_Loops;
    std::vector<EMFace> m_Faces;
    
    // Free lists for recycling IDs
    std::vector<VertexID> m_FreeVertices;
    std::vector<EdgeID> m_FreeEdges;
    std::vector<LoopID> m_FreeLoops;
    std::vector<FaceID> m_FreeFaces;
    
    // Selection state
    MeshSelection m_Selection;
    
    // Edge lookup: hash(v0, v1) -> EdgeID
    std::unordered_map<uint64_t, EdgeID> m_EdgeMap;
    
    static uint64_t EdgeKey(VertexID v0, VertexID v1) {
        if (v0 > v1) std::swap(v0, v1);
        return (static_cast<uint64_t>(v0) << 32) | v1;
    }
};

} // namespace lucent::mesh
