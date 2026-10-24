// Step.h — a small ISO-10303-21 (STEP) reader that turns a solid into triangles.
//
// Altium keeps the real 3D model of a footprint inside the library: a STEP file
// ("Open CASCADE" export, AP214/AP242) stored zlib compressed in the "Models" storage.
// Reading it is what makes the 3D view show the actual part instead of a guessed box.
//
// This is not a full STEP kernel. It handles the representation every vendor ships for
// component bodies: manifold solids made of planar, cylindrical, conical, spherical and
// toroidal faces bounded by line/arc edges (plus B-spline edges, which are drawn as
// straight chords). Anything else is counted and skipped instead of being drawn wrong.
#pragma once

#include <string>
#include <vector>

namespace pcbv {

struct StepTriangle {
    double a[3]{};
    double b[3]{};
    double c[3]{};
};

struct StepMesh {
    std::vector<StepTriangle> triangles;
    double minX = 1e18, minY = 1e18, minZ = 1e18;
    double maxX = -1e18, maxY = -1e18, maxZ = -1e18;
    int faces = 0;         ///< faces that were tessellated
    int skippedFaces = 0;  ///< faces with unsupported surfaces
    /// Colour declared by the model itself (COLOUR_RGB), 0..1 per channel.
    bool hasColor = false;
    double colorR = 0.5, colorG = 0.5, colorB = 0.5;

    bool valid() const { return !triangles.empty() && minX <= maxX; }
    void add(const StepTriangle& triangle);
    void centre(double& x, double& y, double& z) const;
    double width() const { return maxX - minX; }
    double depth() const { return maxY - minY; }
    double height() const { return maxZ - minZ; }
};

/// Parses STEP text and fills `mesh`. Returns false (with a reason) when the file is not a
/// STEP file or nothing could be tessellated.
bool parseStepMesh(const std::string& text, StepMesh& mesh, std::string* error = nullptr);

/// True when the bytes look like a STEP file.
bool looksLikeStep(const std::string& bytes);

} // namespace pcbv
