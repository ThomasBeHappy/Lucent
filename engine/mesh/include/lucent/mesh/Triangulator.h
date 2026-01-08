#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace lucent::mesh {

// Ear-clipping triangulation for arbitrary polygons (ngons)
class Triangulator {
public:
    // Triangulate a polygon defined by vertices in order.
    // Returns indices into the input vertex array forming triangles.
    // faceNormal: the polygon's normal (used to determine winding)
    static std::vector<uint32_t> Triangulate(
        const std::vector<glm::vec3>& vertices,
        const glm::vec3& faceNormal
    );
    
    // 2D triangulation (projects 3D polygon onto best-fit plane)
    static std::vector<uint32_t> Triangulate2D(
        const std::vector<glm::vec2>& vertices
    );
    
private:
    // Check if a vertex is an "ear" (can be clipped)
    static bool IsEar(
        const std::vector<glm::vec2>& vertices,
        const std::vector<uint32_t>& indices,
        uint32_t prevIdx,
        uint32_t currIdx,
        uint32_t nextIdx
    );
    
    // Check if point is inside triangle
    static bool PointInTriangle(
        const glm::vec2& p,
        const glm::vec2& a,
        const glm::vec2& b,
        const glm::vec2& c
    );
    
    // Check if vertex forms a convex corner
    static bool IsConvex(
        const glm::vec2& prev,
        const glm::vec2& curr,
        const glm::vec2& next
    );
    
    // Project 3D polygon to 2D using the face normal
    static std::vector<glm::vec2> ProjectTo2D(
        const std::vector<glm::vec3>& vertices,
        const glm::vec3& normal
    );
};

} // namespace lucent::mesh
