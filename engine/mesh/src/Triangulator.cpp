#include "lucent/mesh/Triangulator.h"
#include <algorithm>
#include <cmath>

namespace lucent::mesh {

std::vector<glm::vec2> Triangulator::ProjectTo2D(
    const std::vector<glm::vec3>& vertices,
    const glm::vec3& normal
) {
    if (vertices.empty()) return {};
    
    // Find the dominant axis of the normal to project onto a 2D plane
    glm::vec3 absNormal = glm::abs(normal);
    
    // Choose projection plane based on largest normal component
    int dropAxis;
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        dropAxis = 0; // Drop X, project onto YZ
    } else if (absNormal.y >= absNormal.z) {
        dropAxis = 1; // Drop Y, project onto XZ
    } else {
        dropAxis = 2; // Drop Z, project onto XY
    }
    
    std::vector<glm::vec2> result;
    result.reserve(vertices.size());
    
    for (const auto& v : vertices) {
        glm::vec2 p;
        switch (dropAxis) {
            case 0: p = glm::vec2(v.y, v.z); break;
            case 1: p = glm::vec2(v.x, v.z); break;
            case 2: p = glm::vec2(v.x, v.y); break;
        }
        result.push_back(p);
    }
    return result;
}

bool Triangulator::IsConvex(
    const glm::vec2& prev,
    const glm::vec2& curr,
    const glm::vec2& next
) {
    // Cross product of (curr - prev) and (next - curr)
    glm::vec2 e1 = curr - prev;
    glm::vec2 e2 = next - curr;
    float cross = e1.x * e2.y - e1.y * e2.x;
    return cross >= 0.0f; // Counter-clockwise = convex
}

bool Triangulator::PointInTriangle(
    const glm::vec2& p,
    const glm::vec2& a,
    const glm::vec2& b,
    const glm::vec2& c
) {
    auto sign = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
        return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
    };
    
    float d1 = sign(p, a, b);
    float d2 = sign(p, b, c);
    float d3 = sign(p, c, a);
    
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    
    return !(hasNeg && hasPos);
}

bool Triangulator::IsEar(
    const std::vector<glm::vec2>& vertices,
    const std::vector<uint32_t>& indices,
    uint32_t prevIdx,
    uint32_t currIdx,
    uint32_t nextIdx
) {
    const glm::vec2& prev = vertices[indices[prevIdx]];
    const glm::vec2& curr = vertices[indices[currIdx]];
    const glm::vec2& next = vertices[indices[nextIdx]];
    
    // Must be convex
    if (!IsConvex(prev, curr, next)) {
        return false;
    }
    
    // Check that no other vertex is inside this triangle
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i == prevIdx || i == currIdx || i == nextIdx) continue;
        
        const glm::vec2& p = vertices[indices[i]];
        
        // Skip if point is very close to triangle vertices
        float eps = 0.0001f;
        if (glm::distance(p, prev) < eps || glm::distance(p, curr) < eps || glm::distance(p, next) < eps) {
            continue;
        }
        
        if (PointInTriangle(p, prev, curr, next)) {
            return false;
        }
    }
    
    return true;
}

std::vector<uint32_t> Triangulator::Triangulate2D(const std::vector<glm::vec2>& vertices) {
    std::vector<uint32_t> result;
    
    if (vertices.size() < 3) return result;
    if (vertices.size() == 3) {
        return {0, 1, 2};
    }
    
    // Initialize index list
    std::vector<uint32_t> indices;
    indices.reserve(vertices.size());
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        indices.push_back(i);
    }
    
    // Ear clipping
    size_t maxIterations = vertices.size() * vertices.size(); // Safety limit
    size_t iterations = 0;
    
    while (indices.size() > 3 && iterations < maxIterations) {
        bool earFound = false;
        
        for (size_t i = 0; i < indices.size(); ++i) {
            size_t prevIdx = (i + indices.size() - 1) % indices.size();
            size_t nextIdx = (i + 1) % indices.size();
            
            if (IsEar(vertices, indices, 
                      static_cast<uint32_t>(prevIdx), 
                      static_cast<uint32_t>(i), 
                      static_cast<uint32_t>(nextIdx))) {
                // Add triangle
                result.push_back(indices[prevIdx]);
                result.push_back(indices[i]);
                result.push_back(indices[nextIdx]);
                
                // Remove the ear vertex
                indices.erase(indices.begin() + static_cast<ptrdiff_t>(i));
                earFound = true;
                break;
            }
        }
        
        if (!earFound) {
            // Degenerate polygon - try to salvage with simple fan
            break;
        }
        
        ++iterations;
    }
    
    // Handle remaining triangle
    if (indices.size() == 3) {
        result.push_back(indices[0]);
        result.push_back(indices[1]);
        result.push_back(indices[2]);
    } else if (indices.size() > 3) {
        // Fallback: simple fan triangulation from first vertex
        for (size_t i = 1; i + 1 < indices.size(); ++i) {
            result.push_back(indices[0]);
            result.push_back(indices[i]);
            result.push_back(indices[i + 1]);
        }
    }
    
    return result;
}

std::vector<uint32_t> Triangulator::Triangulate(
    const std::vector<glm::vec3>& vertices,
    const glm::vec3& faceNormal
) {
    if (vertices.size() < 3) return {};
    if (vertices.size() == 3) {
        return {0, 1, 2};
    }
    
    // Project to 2D (no reordering here — we keep indices aligned with the input vertex order)
    std::vector<glm::vec2> projected = ProjectTo2D(vertices, faceNormal);
    
    // Determine whether we need to reverse polygon order so the triangulation winding
    // matches the supplied face normal (and our convention of CCW front faces).
    //
    // NOTE: We *must not* reverse the projected vertex array in-place without also remapping indices,
    // otherwise the resulting triangle indices no longer correspond to the original 3D vertex order.
    glm::vec3 absNormal = glm::abs(faceNormal);
    int dropAxis;
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        dropAxis = 0; // Drop X, project onto YZ
    } else if (absNormal.y >= absNormal.z) {
        dropAxis = 1; // Drop Y, project onto XZ
    } else {
        dropAxis = 2; // Drop Z, project onto XY
    }
    
    bool normalPositive = false;
    switch (dropAxis) {
        case 0: normalPositive = faceNormal.x > 0; break;
        case 1: normalPositive = faceNormal.y > 0; break;
        case 2: normalPositive = faceNormal.z > 0; break;
    }
    
    // Shoelace signed area (2x area). Positive => CCW in the projected 2D plane.
    float area2 = 0.0f;
    for (size_t i = 0; i < projected.size(); ++i) {
        const glm::vec2& a = projected[i];
        const glm::vec2& b = projected[(i + 1) % projected.size()];
        area2 += a.x * b.y - b.x * a.y;
    }
    
    bool isCCW = area2 > 0.0f;
    bool needsReverse = (isCCW != normalPositive);
    
    if (!needsReverse) {
        return Triangulate2D(projected);
    }
    
    // Triangulate a reversed order polygon, then map indices back to the original order.
    std::vector<glm::vec2> reversedProjected = projected;
    std::reverse(reversedProjected.begin(), reversedProjected.end());
    
    std::vector<uint32_t> tri = Triangulate2D(reversedProjected);
    for (uint32_t& idx : tri) {
        idx = static_cast<uint32_t>(projected.size() - 1u - idx);
    }
    return tri;
}

} // namespace lucent::mesh
