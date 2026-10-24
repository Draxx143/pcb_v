// ThreeD.cpp — software renderer for the 3D board preview (see ThreeD.h).
//
// Pipeline: the document is turned into flat faces in board space (millimetres, Z up),
// the camera is a simple orthographic turntable, and the faces are scan-converted into a
// colour + depth buffer with one directional light. Supersampling gives the edges their
// anti-aliasing.
#include "ThreeD.h"
#include "LayerStyle.h"

#include "Step.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace pcbv {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMilToMm = 0.0254;
constexpr double kThicknessMm = 1.6;   ///< board thickness of the preview

// ---------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

Vec3 normalised(const Vec3& a) {
    const double len = length(a);
    return len > 1e-12 ? a * (1.0 / len) : Vec3{0.0, 0.0, 1.0};
}

struct Vec2 {
    double x = 0.0, y = 0.0;
};

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

int channel(double value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0, 255.0)));
}

std::uint32_t rgb(int r, int g, int b) {
    return (static_cast<std::uint32_t>(channel(r)) << 16) |
           (static_cast<std::uint32_t>(channel(g)) << 8) | static_cast<std::uint32_t>(channel(b));
}

std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, double t) {
    const double ar = static_cast<double>((a >> 16) & 0xFF);
    const double ag = static_cast<double>((a >> 8) & 0xFF);
    const double ab = static_cast<double>(a & 0xFF);
    const double br = static_cast<double>((b >> 16) & 0xFF);
    const double bg = static_cast<double>((b >> 8) & 0xFF);
    const double bb = static_cast<double>(b & 0xFF);
    return rgb(ar + (br - ar) * t, ag + (bg - ag) * t, ab + (bb - ab) * t);
}

std::uint32_t scaleColor(std::uint32_t c, double factor) {
    return rgb(((c >> 16) & 0xFF) * factor, ((c >> 8) & 0xFF) * factor, (c & 0xFF) * factor);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

struct Face {
    std::vector<Vec3> points;
    std::uint32_t color = 0xFFFFFF;
    double alpha = 1.0;     ///< 1 = opaque, less = blended (shadows)
    double metal = 0.0;     ///< 0..1 specular strength
    double shininess = 28.0;
};

struct Scene {
    std::vector<Face> faces;

    void addPolygon(std::vector<Vec3> points, std::uint32_t color, double metal = 0.0,
                    double alpha = 1.0) {
        if (points.size() < 3) return;
        Face face;
        face.points = std::move(points);
        face.color = color;
        face.metal = metal;
        face.alpha = alpha;
        faces.push_back(std::move(face));
    }

    void addQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                 std::uint32_t color, double metal = 0.0, double alpha = 1.0) {
        addPolygon({a, b, c, d}, color, metal, alpha);
    }

    /// Four corners of an axis-aligned rectangle in the XY plane at height `z`.
    void addRect(double x0, double y0, double x1, double y1, double z, std::uint32_t color,
                 double metal = 0.0) {
        if (x1 - x0 <= 1e-9 || y1 - y0 <= 1e-9) return;
        addQuad({x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, color, metal);
    }

    /// Rotated rectangle (pads, chips) around (cx, cy).
    void addRotatedRect(double cx, double cy, double width, double height, double rotationDeg,
                        double z, std::uint32_t color, double metal = 0.0) {
        if (width <= 1e-9 || height <= 1e-9) return;
        const double rad = rotationDeg * kPi / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        const double hw = width / 2.0, hh = height / 2.0;
        std::vector<Vec3> points;
        for (const Vec2& corner : {Vec2{-hw, -hh}, Vec2{hw, -hh}, Vec2{hw, hh}, Vec2{-hw, hh}}) {
            points.push_back(Vec3{cx + corner.x * c - corner.y * s, cy + corner.x * s + corner.y * c, z});
        }
        addPolygon(points, color, metal);
    }

    /// A thick line segment (track) as a quad in the XY plane.
    void addSegment(double x1, double y1, double x2, double y2, double width, double z,
                    std::uint32_t color, double metal = 0.0) {
        const double dx = x2 - x1, dy = y2 - y1;
        const double len = std::hypot(dx, dy);
        if (len < 1e-9) {
            addRect(x1 - width / 2.0, y1 - width / 2.0, x1 + width / 2.0, y1 + width / 2.0, z,
                    color, metal);
            return;
        }
        const double nx = -dy / len * (width / 2.0);
        const double ny = dx / len * (width / 2.0);
        addQuad({x1 + nx, y1 + ny, z}, {x2 + nx, y2 + ny, z}, {x2 - nx, y2 - ny, z},
                {x1 - nx, y1 - ny, z}, color, metal);
        // Round caps keep the corners of a bent track closed.
        disc(x1, y1, width / 2.0, z, 10, color, metal);
        disc(x2, y2, width / 2.0, z, 10, color, metal);
    }

    void disc(double cx, double cy, double radius, double z, int steps, std::uint32_t color,
              double metal = 0.0) {
        if (radius <= 1e-9) return;
        std::vector<Vec3> points;
        points.reserve(static_cast<std::size_t>(steps));
        for (int i = 0; i < steps; ++i) {
            const double angle = 2.0 * kPi * static_cast<double>(i) / steps;
            points.push_back(Vec3{cx + std::cos(angle) * radius, cy + std::sin(angle) * radius, z});
        }
        addPolygon(points, color, metal);
    }

    /// Extruded box between z0 and z1 with separate colours per orientation.
    void addBox(double x0, double y0, double x1, double y1, double z0, double z1,
                std::uint32_t top, std::uint32_t side, std::uint32_t bottom, double metal = 0.0) {
        if (x1 - x0 <= 1e-9 || y1 - y0 <= 1e-9 || z1 - z0 <= 1e-9) return;
        addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, top, metal);
        addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, bottom, metal);
        addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, side, metal);
        addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, side, metal);
        addQuad({x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, side, metal);
        addQuad({x0, y1, z0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, side, metal);
    }

    /// Extruded cylinder (electrolytic capacitors, holes).
    void addCylinder(double cx, double cy, double radius, double z0, double z1, int steps,
                     std::uint32_t top, std::uint32_t side, std::uint32_t bottom,
                     double metal = 0.0) {
        if (radius <= 1e-9 || z1 - z0 <= 1e-9) return;
        std::vector<Vec3> ring;
        ring.reserve(static_cast<std::size_t>(steps));
        for (int i = 0; i < steps; ++i) {
            const double angle = 2.0 * kPi * static_cast<double>(i) / steps;
            ring.push_back(Vec3{cx + std::cos(angle) * radius, cy + std::sin(angle) * radius, 0.0});
        }
        std::vector<Vec3> topRing, bottomRing;
        for (const Vec3& point : ring) {
            topRing.push_back(Vec3{point.x, point.y, z1});
            bottomRing.push_back(Vec3{point.x, point.y, z0});
        }
        addPolygon(topRing, top, metal);
        addPolygon(bottomRing, bottom, metal);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const Vec3& a = topRing[i];
            const Vec3& b = topRing[(i + 1) % topRing.size()];
            const Vec3& c = bottomRing[(i + 1) % bottomRing.size()];
            const Vec3& d = bottomRing[i];
            addQuad(a, b, c, d, side, metal * 0.5);
        }
    }
};

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

struct Camera {
    Vec3 eye;
    Vec3 right{1.0, 0.0, 0.0};
    Vec3 up{0.0, 0.0, 1.0};
    Vec3 forward{0.0, 1.0, 0.0};
    double scale = 1.0;
    double cx = 0.0;
    double cy = 0.0;

    /// Screen position and depth of a world point (depth grows away from the eye).
    Vec2 project(const Vec3& point, double& depth) const {
        const Vec3 relative = point - eye;
        depth = dot(relative, forward);
        return Vec2{cx + dot(relative, right) * scale, cy - dot(relative, up) * scale};
    }
};

Camera makeCamera(double yawDeg, double elevationDeg) {
    const double yaw = yawDeg * kPi / 180.0;
    const double elevation = std::clamp(elevationDeg, -89.0, 89.9) * kPi / 180.0;

    // Direction from the board towards the eye.
    const Vec3 towards{std::cos(elevation) * std::cos(yaw), std::cos(elevation) * std::sin(yaw),
                       std::sin(elevation)};
    Camera camera;
    camera.eye = towards * 400.0;   // orthographic: only the direction matters
    camera.forward = normalised(towards * -1.0);

    Vec3 worldUp{0.0, 0.0, 1.0};
    if (std::fabs(dot(camera.forward, worldUp)) > 0.999) worldUp = Vec3{0.0, 1.0, 0.0};
    camera.right = normalised(cross(camera.forward, worldUp));
    camera.up = normalised(cross(camera.right, camera.forward));
    return camera;
}

// ---------------------------------------------------------------------------
// Rasteriser
// ---------------------------------------------------------------------------

struct Renderer {
    Camera camera;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> color;
    std::vector<double> depth;
    Vec3 light{0.0, 0.0, 1.0};     ///< key light
    Vec3 fillLight{0.0, 0.0, 1.0}; ///< soft light from the opposite side
    Vec3 rimLight{0.0, 0.0, 1.0};  ///< grazing light that separates the part from the board
    Vec3 toEye{0.0, 0.0, 1.0};

    void resize(int w, int h, std::uint32_t background) {
        width = w;
        height = h;
        color.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), background);
        depth.assign(color.size(), std::numeric_limits<double>::infinity());
    }

    /// A vertical gradient behind the board instead of a flat fill: the board then sits in a
    /// studio instead of floating on a wall.
    void gradientBackground(std::uint32_t top, std::uint32_t bottom) {
        if (height <= 0) return;
        for (int y = 0; y < height; ++y) {
            const double t = height > 1 ? static_cast<double>(y) / (height - 1) : 0.0;
            const std::uint32_t row = mixColor(top, bottom, t);
            const std::size_t start = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            for (int x = 0; x < width; ++x) color[start + static_cast<std::size_t>(x)] = row;
        }
    }

    static Vec3 faceNormal(const std::vector<Vec3>& points) {
        // Newell's method: stable for any planar polygon, concave included.
        Vec3 normal{0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vec3& a = points[i];
            const Vec3& b = points[(i + 1) % points.size()];
            normal.x += (a.y - b.y) * (a.z + b.z);
            normal.y += (a.z - b.z) * (a.x + b.x);
            normal.z += (a.x - b.x) * (a.y + b.y);
        }
        return normalised(normal);
    }

    void draw(const Face& face) {
        if (face.points.size() < 3) return;

        Vec3 normal = faceNormal(face.points);
        if (dot(normal, toEye) < 0.0) normal = normal * -1.0;   // light both sides

        // Three lights: a key light that models the form, a fill that keeps the shadow side
        // readable and a rim that draws a bright edge along the silhouette. Flat shading plus
        // a tone curve is what makes a CAD preview look like a photograph of a board.
        const double key = std::max(0.0, dot(normal, light));
        const double fill = std::max(0.0, dot(normal, fillLight));
        const double rim = std::max(0.0, dot(normal, rimLight));
        const double sky = 0.5 + 0.5 * normal.z;
        const double k = 0.30 + 0.16 * sky + 0.58 * key + 0.24 * fill + 0.12 * rim;
        const Vec3 half = normalised(light + toEye);
        const double specular =
            face.metal > 0.0 ? std::pow(std::max(0.0, dot(normal, half)), face.shininess) * face.metal : 0.0;
        auto tone = [](double value) {
            const double clamped = std::clamp(value, 0.0, 255.0) / 255.0;
            return std::pow(clamped, 0.92) * 255.0;   // lifts the mid tones like a studio shot
        };
        const double r = tone(((face.color >> 16) & 0xFF) * k + 245.0 * specular);
        const double g = tone(((face.color >> 8) & 0xFF) * k + 245.0 * specular);
        const double b = tone((face.color & 0xFF) * k + 250.0 * specular);
        const std::uint32_t shaded = rgb(r, g, b);

        // Project the polygon and derive the depth plane (all faces are planar).
        std::vector<Vec2> screen;
        screen.reserve(face.points.size());
        std::vector<double> points;
        points.reserve(face.points.size());
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        for (const Vec3& point : face.points) {
            double depthValue = 0.0;
            const Vec2 projected = camera.project(point, depthValue);
            screen.push_back(projected);
            points.push_back(depthValue);
            minX = std::min(minX, projected.x);
            maxX = std::max(maxX, projected.x);
            minY = std::min(minY, projected.y);
            maxY = std::max(maxY, projected.y);
        }

        double planeA = 0.0, planeB = 0.0, planeC = 0.0;
        bool havePlane = false;
        for (std::size_t i = 1; i + 1 < screen.size() && !havePlane; ++i) {
            const Vec2& p0 = screen[0];
            const Vec2& p1 = screen[i];
            const Vec2& p2 = screen[i + 1];
            const double det = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
            if (std::fabs(det) < 1e-7) continue;
            planeA = ((points[i] - points[0]) * (p2.y - p0.y) -
                      (points[i + 1] - points[0]) * (p1.y - p0.y)) / det;
            planeB = ((points[i + 1] - points[0]) * (p1.x - p0.x) -
                      (points[i] - points[0]) * (p2.x - p0.x)) / det;
            planeC = points[0] - planeA * p0.x - planeB * p0.y;
            havePlane = true;
        }
        if (!havePlane) return;   // edge on: nothing to fill

        const int yStart = std::max(0, static_cast<int>(std::floor(minY)));
        const int yEnd = std::min(height - 1, static_cast<int>(std::ceil(maxY)));
        const int xLimit = width;
        if (yStart > yEnd) return;

        for (int y = yStart; y <= yEnd; ++y) {
            const double scan = y + 0.5;
            std::vector<double> crossings;
            crossings.reserve(screen.size());
            for (std::size_t i = 0; i < screen.size(); ++i) {
                const Vec2& a = screen[i];
                const Vec2& b = screen[(i + 1) % screen.size()];
                if ((a.y <= scan && b.y > scan) || (b.y <= scan && a.y > scan)) {
                    const double t = (scan - a.y) / (b.y - a.y);
                    crossings.push_back(a.x + t * (b.x - a.x));
                }
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end());

            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                double xa = crossings[i];
                double xb = crossings[i + 1];
                if (xb < xa) std::swap(xa, xb);
                int xStart = std::max(0, static_cast<int>(std::ceil(xa - 0.5)));
                int xEnd = std::min(xLimit - 1, static_cast<int>(std::floor(xb - 0.5)));
                for (int x = xStart; x <= xEnd; ++x) {
                    const double pixelDepth = planeA * (x + 0.5) + planeB * (y + 0.5) + planeC;
                    const std::size_t index =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x);
                    if (pixelDepth >= depth[index]) continue;
                    if (face.alpha >= 0.999) {
                        color[index] = shaded;
                        depth[index] = pixelDepth;
                    } else {
                        color[index] = mixColor(color[index], shaded, face.alpha);
                    }
                }
            }
        }
    }

    void drawScene(const Scene& scene) {
        for (const Face& face : scene.faces) draw(face);
    }

    ThreeDImage resolve(int outWidth, int outHeight, int factor) const {
        ThreeDImage image;
        image.width = outWidth;
        image.height = outHeight;
        image.pixels.assign(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight),
                            0u);
        if (factor <= 1) {
            image.pixels = color;
            return image;
        }
        const double samples = static_cast<double>(factor) * factor;
        for (int y = 0; y < outHeight; ++y) {
            for (int x = 0; x < outWidth; ++x) {
                double r = 0.0, g = 0.0, b = 0.0;
                for (int sy = 0; sy < factor; ++sy) {
                    for (int sx = 0; sx < factor; ++sx) {
                        const std::size_t index =
                            static_cast<std::size_t>(y * factor + sy) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x * factor + sx);
                        const std::uint32_t c = color[index];
                        r += (c >> 16) & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += c & 0xFF;
                    }
                }
                image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) +
                             static_cast<std::size_t>(x)] = rgb(r / samples, g / samples, b / samples);
            }
        }
        return image;
    }
};

// ---------------------------------------------------------------------------
// Board geometry
// ---------------------------------------------------------------------------

bool isTopCopper(int layer) { return layer == 1; }
bool isBottomCopper(int layer) { return layer == 32; }
bool isInnerCopper(int layer) { return layer >= 2 && layer <= 31; }
bool isTopSilk(int layer) { return layer == 33; }
bool isBottomSilk(int layer) { return layer == 34 || layer == 36 || layer == 38; }

struct BoardFrame {
    double centreX = 0.0;   // mil
    double centreY = 0.0;
    std::pair<double, double> toMm(double milX, double milY) const {
        return {(milX - centreX) * kMilToMm, (milY - centreY) * kMilToMm};
    }
};

struct Palette {
    std::uint32_t mask = 0x16684a;
    std::uint32_t maskDark = 0x0e4c36;
    std::uint32_t maskMid = 0x125b41;
    std::uint32_t copperTop = 0x2f8f63;
    std::uint32_t copperBottom = 0x2f7f7f;
    std::uint32_t copperBase = 0x1f7a52;   ///< raised copper, closer to the mask colour
    std::uint32_t copperTrench = 0x0b4a32; ///< the seam around a track under the mask
    std::uint32_t pad = 0xd8b26a;
    std::uint32_t padEdge = 0x8a6f38;
    std::uint32_t barrel = 0xd9ae35;        ///< plated wall of a drilled hole
    std::uint32_t silk = 0xe8edf2;
    std::uint32_t substrate = 0xa8996f;
    std::uint32_t hole = 0x0b0d10;
    std::uint32_t body = 0x24272c;
    std::uint32_t bodySide = 0x1b1e22;
    std::uint32_t metal = 0xc3cad2;
    std::uint32_t skyTop = 0x11161c;        ///< studio background, top row
    std::uint32_t skyBottom = 0x0b0f13;     ///< studio background, bottom row
};

/// The 3D palette of the theme in force, so the preview and the flat view agree on colours.
Palette paletteFor(const ThemeColours& colours) {
    Palette palette;
    palette.mask = colours.mask;
    palette.maskDark = scaleColor(palette.mask, 0.60);
    palette.maskMid = scaleColor(palette.mask, 0.78);
    palette.copperTop = colours.copperUnderMask;
    palette.copperBottom = scaleColor(colours.copperUnderMask, 0.86);
    palette.copperBase = mixColor(palette.copperTop, palette.mask, 0.25);
    palette.copperTrench = scaleColor(palette.mask, 0.52);
    palette.pad = colours.pad;
    palette.padEdge = colours.padEdge;
    palette.barrel = colours.barrel;
    palette.silk = colours.silk;
    palette.substrate = colours.substrate;
    palette.hole = colours.hole;
    palette.body = colours.body;
    palette.bodySide = colours.bodySide;
    palette.metal = colours.metal;
    palette.skyTop = colours.threeTop;
    palette.skyBottom = colours.threeBottom;
    return palette;
}

/// Extent of one component in its own frame (millimetres, body area only).
struct BodyPlan {
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;   // local mm
    double height = 0.5;                                      // mm above the board
    bool chip = false;                                        // two SMD terminations
    bool cylinder = false;
    double radius = 0.0;                                      // cylinder only
};

BodyPlan planBody(const Component& component) {
    BodyPlan plan;
    const double rad = -component.rotation * kPi / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);

    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    double padX = 0.0, padY = 0.0;
    bool anyPad = false;
    bool allThroughHole = true;
    for (const Pad& pad : component.pads) {
        const double dx = (pad.x - component.x) * kMilToMm;
        const double dy = (pad.y - component.y) * kMilToMm;
        const double localX = dx * c - dy * s;
        const double localY = dx * s + dy * c;
        const double halfW = std::max(pad.width * kMilToMm, 0.05) / 2.0;
        const double halfH = std::max(pad.height * kMilToMm, 0.05) / 2.0;
        minX = std::min(minX, localX - halfW);
        maxX = std::max(maxX, localX + halfW);
        minY = std::min(minY, localY - halfH);
        maxY = std::max(maxY, localY + halfH);
        padX = std::max(padX, pad.width * kMilToMm);
        padY = std::max(padY, pad.height * kMilToMm);
        if (pad.holeSize <= 0.0) allThroughHole = false;
        anyPad = true;
    }
    if (!anyPad) {
        plan.minX = -0.5; plan.maxX = 0.5;
        plan.minY = -0.3; plan.maxY = 0.3;
        plan.height = 0.5;
        return plan;
    }

    const double spanX = maxX - minX;
    const double spanY = maxY - minY;
    const bool alongX = spanX >= spanY;
    const double spanAlong = alongX ? spanX : spanY;
    const double sizeAlong = alongX ? padX : padY;
    const double sizeAcross = alongX ? padY : padX;

    if (component.pads.size() == 2) {
        // A chip part: the body sits between the two terminations.
        const double inset = std::max(sizeAlong * 0.55, spanAlong * 0.12);
        const double halfAcross = std::max(sizeAcross * 0.52, 0.15);
        if (alongX) {
            plan.minX = minX + inset;
            plan.maxX = maxX - inset;
            plan.minY = -halfAcross;
            plan.maxY = halfAcross;
        } else {
            plan.minY = minY + inset;
            plan.maxY = maxY - inset;
            plan.minX = -halfAcross;
            plan.maxX = halfAcross;
        }
        if (plan.maxX - plan.minX < 0.12) {
            const double middle = (plan.minX + plan.maxX) / 2.0;
            plan.minX = middle - 0.06;
            plan.maxX = middle + 0.06;
        }
        if (plan.maxY - plan.minY < 0.12) {
            const double middle = (plan.minY + plan.maxY) / 2.0;
            plan.minY = middle - 0.06;
            plan.maxY = middle + 0.06;
        }
        plan.chip = true;
        plan.height = component.height > 0.0 ? component.height * kMilToMm : 0.55;
        // A tall, fully through-hole two pin part is a can (electrolytic capacitor).
        if (allThroughHole && std::hypot(spanX, spanY) > 1.4 && plan.height > 1.6) {
            plan.cylinder = true;
            plan.radius = std::max(0.6, std::hypot(spanX, spanY) * 0.42);
        }
    } else {
        // An IC or a connector: the body is the pad ring minus a little.
        const double insetX = std::min(padX * 0.6, spanX * 0.22);
        const double insetY = std::min(padY * 0.6, spanY * 0.22);
        plan.minX = minX + insetX;
        plan.maxX = maxX - insetX;
        plan.minY = minY + insetY;
        plan.maxY = maxY - insetY;
        plan.height = component.height > 0.0 ? component.height * kMilToMm : 1.0;
    }
    plan.height = std::clamp(plan.height, 0.25, 12.0);
    return plan;
}

// ---------------------------------------------------------------------------
// Real 3D models
// ---------------------------------------------------------------------------
//
// A component whose footprint came with a 3D model (Attachment "MODELID={…}" in the
// library, the STEP file itself in the library's "Models" storage) is drawn from that
// triangle mesh. The transformation is the one Altium itself applies:
//   * ROTX / ROTY / ROTZ of the model record rotate the model into the footprint frame,
//   * the model is then centred on the component origin and rested on the board surface,
//   * DZ lifts it by the offset stored with the model,
//   * the component rotation turns it on the board, exactly like the pads beside it.
struct PlacedModel {
    std::vector<StepTriangle> triangles;   ///< in the component frame (mm, Z = 0 on the board)
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    double height = 0.0;
    bool hasColor = false;                 ///< the model declared its own colour
    double colorR = 0.5, colorG = 0.5, colorB = 0.5;
    bool valid() const { return !triangles.empty(); }
};

void rotatePoint(double& x, double& y, double& z, const EmbeddedModel& model) {
    const double rx = model.rotationX * kPi / 180.0;
    const double ry = model.rotationY * kPi / 180.0;
    const double rz = model.rotationZ * kPi / 180.0;
    // X
    {
        const double c = std::cos(rx), s = std::sin(rx);
        const double ny = y * c - z * s;
        const double nz = y * s + z * c;
        y = ny;
        z = nz;
    }
    // Y
    {
        const double c = std::cos(ry), s = std::sin(ry);
        const double nx = x * c + z * s;
        const double nz = -x * s + z * c;
        x = nx;
        z = nz;
    }
    // Z
    {
        const double c = std::cos(rz), s = std::sin(rz);
        const double nx = x * c - y * s;
        const double ny = x * s + y * c;
        x = nx;
        y = ny;
    }
}

/// Places a STEP mesh in the component frame. A model that carries its own placement
/// (a bundle records the offset/rotation the footprint was authored with) is used exactly
/// as authored; an Altium library model has no placement of its own, so it is centred on
/// the footprint origin and rested on the board — which is what Altium shows for those.
PlacedModel placeModel(const StepMesh& mesh, const EmbeddedModel& model) {
    PlacedModel placed;
    if (!mesh.valid()) return placed;

    std::vector<StepTriangle> rotated = mesh.triangles;
    double minX = 1e18, minY = 1e18, minZ = 1e18;
    double maxX = -1e18, maxY = -1e18, maxZ = -1e18;
    for (StepTriangle& triangle : rotated) {
        for (double* point : {triangle.a, triangle.b, triangle.c}) {
            rotatePoint(point[0], point[1], point[2], model);
            point[0] += model.dx;
            point[1] += model.dy;
            point[2] += model.dz;
            minX = std::min(minX, point[0]);
            maxX = std::max(maxX, point[0]);
            minY = std::min(minY, point[1]);
            maxY = std::max(maxY, point[1]);
            minZ = std::min(minZ, point[2]);
            maxZ = std::max(maxZ, point[2]);
        }
    }
    const double cx = model.hasPlacement ? 0.0 : (minX + maxX) / 2.0;
    const double cy = model.hasPlacement ? 0.0 : (minY + maxY) / 2.0;
    const double cz = model.hasPlacement ? 0.0 : minZ;
    for (StepTriangle& triangle : rotated) {
        for (double* point : {triangle.a, triangle.b, triangle.c}) {
            point[0] -= cx;
            point[1] -= cy;
            point[2] -= cz;
        }
    }
    placed.minX = minX - cx;
    placed.maxX = maxX - cx;
    placed.minY = minY - cy;
    placed.maxY = maxY - cy;
    placed.height = std::max(maxZ - cz, 0.01);
    placed.hasColor = mesh.hasColor;
    placed.colorR = mesh.colorR;
    placed.colorG = mesh.colorG;
    placed.colorB = mesh.colorB;
    placed.triangles = std::move(rotated);
    return placed;
}

/// Parsing a 50 kB STEP file costs tens of milliseconds, and the preview re-renders while
/// the user drags the board, so decoded models are kept for the rest of the session.
const PlacedModel* modelFor(const Document& document, int index) {
    if (index < 0 || index >= static_cast<int>(document.models.size())) return nullptr;
    const EmbeddedModel& model = document.models[static_cast<std::size_t>(index)];
    if (model.stepText.empty()) return nullptr;

    std::uint64_t key = 1469598103934665603ull;
    auto mix = [&key](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            key ^= bytes[i];
            key *= 1099511628211ull;
        }
    };
    mix(model.stepText.data(), model.stepText.size());
    mix(&model.rotationX, sizeof(model.rotationX));
    mix(&model.rotationY, sizeof(model.rotationY));
    mix(&model.rotationZ, sizeof(model.rotationZ));
    mix(&model.dx, sizeof(model.dx));
    mix(&model.dy, sizeof(model.dy));
    mix(&model.dz, sizeof(model.dz));

    static std::vector<std::pair<std::uint64_t, PlacedModel>> cache;
    for (const auto& entry : cache) {
        if (entry.first == key) return entry.second.valid() ? &entry.second : nullptr;
    }
    if (cache.size() > 32) cache.erase(cache.begin(), cache.begin() + 8);

    PlacedModel placed;
    StepMesh mesh;
    std::string error;
    if (parseStepMesh(model.stepText, mesh, &error)) placed = placeModel(mesh, model);
    cache.emplace_back(key, std::move(placed));
    return cache.back().second.valid() ? &cache.back().second : nullptr;
}

BoardFrame boardFrame(const Document& document) {
    BoardFrame frame;
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    if (document.geometry.outline.size() >= 3) {
        for (const Point& point : document.geometry.outline) {
            minX = std::min(minX, point.x);
            maxX = std::max(maxX, point.x);
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
        }
    } else if (document.bounds.valid) {
        minX = document.bounds.minX;
        maxX = document.bounds.maxX;
        minY = document.bounds.minY;
        maxY = document.bounds.maxY;
    }
    if (minX > maxX) {
        frame.centreX = frame.centreY = 0.0;
        return frame;
    }
    frame.centreX = (minX + maxX) / 2.0;
    frame.centreY = (minY + maxY) / 2.0;
    return frame;
}

std::vector<std::pair<double, double>> boardOutlineMm(const Document& document,
                                                      const BoardFrame& frame) {
    std::vector<std::pair<double, double>> outline;
    if (document.geometry.outline.size() >= 3) {
        outline.reserve(document.geometry.outline.size());
        for (const Point& point : document.geometry.outline) outline.push_back(frame.toMm(point.x, point.y));
        return outline;
    }
    if (document.bounds.valid) {
        const double x0 = document.bounds.minX, x1 = document.bounds.maxX;
        const double y0 = document.bounds.minY, y1 = document.bounds.maxY;
        outline.push_back(frame.toMm(x0, y0));
        outline.push_back(frame.toMm(x1, y0));
        outline.push_back(frame.toMm(x1, y1));
        outline.push_back(frame.toMm(x0, y1));
    }
    return outline;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

ThreeDImage render3D(const Document& document, const BoardRenderOptions& options,
                     const ThreeDView& view) {
    ThreeDImage empty;

    Camera camera = makeCamera(view.yawDeg, view.elevationDeg);
    const Vec3 light = normalised(camera.up * 0.85 + camera.right * -0.45 + camera.forward * -0.35);
    const Vec3 fillLight = normalised(camera.up * 0.30 + camera.right * 0.85 + camera.forward * -0.40);
    const Vec3 rimLight = normalised(camera.up * 0.10 + camera.right * 0.20 + camera.forward * -1.0 -
                                     camera.up * 0.9);
    const Vec3 toEye = normalised(camera.forward * -1.0);

    Palette palette = paletteFor(themeColours());
    if (view.maskColor != 0) {
        palette.mask = view.maskColor;
        palette.maskDark = scaleColor(palette.mask, 0.60);
        palette.maskMid = scaleColor(palette.mask, 0.78);
    }

    const BoardFrame frame = boardFrame(document);
    const std::vector<std::pair<double, double>> outline = boardOutlineMm(document, frame);
    if (outline.size() < 3) return empty;

    const double topZ = kThicknessMm / 2.0;
    const double bottomZ = -kThicknessMm / 2.0;
    const double copperZ = topZ + 0.010;
    const double padZ = topZ + 0.016;
    const double silkZ = topZ + 0.022;

    Scene scene;

    // --- board slab ---------------------------------------------------------
    {
        std::vector<Vec3> top, bottom;
        for (const auto& point : outline) {
            top.push_back(Vec3{point.first, point.second, topZ});
            bottom.push_back(Vec3{point.first, point.second, bottomZ});
        }
        scene.addPolygon(top, palette.mask);
        scene.addPolygon(bottom, palette.maskMid);
        for (std::size_t i = 0; i < outline.size(); ++i) {
            const auto& a = outline[i];
            const auto& b = outline[(i + 1) % outline.size()];
            // The laminate edge, with a brighter bead along the very top: that thin line is
            // what tells the eye how thick the board is.
            const double bead = topZ - 0.06;
            scene.addQuad({a.first, a.second, topZ}, {b.first, b.second, topZ},
                          {b.first, b.second, bead}, {a.first, a.second, bead},
                          mixColor(palette.mask, 0xffffff, 0.35));
            scene.addQuad({a.first, a.second, bead}, {b.first, b.second, bead},
                          {b.first, b.second, bottomZ}, {a.first, a.second, bottomZ},
                          palette.substrate);
        }
    }

    // --- copper, silkscreen -------------------------------------------------
    for (const TrackFeature& track : document.geometry.tracks) {
        const int layer = track.layer;
        const bool copper = isTopCopper(layer) || isBottomCopper(layer) || isInnerCopper(layer);
        const bool silk = isTopSilk(layer) || isBottomSilk(layer);
        if (!copper && !silk) continue;
        if (options.isHidden(layer)) continue;
        if (silk && !options.silk) continue;
        if (isInnerCopper(layer)) continue;   // inside the board: nothing to see

        const double width = std::max(track.width, layer == 74 ? 6.0 : 4.0);
        const auto a = frame.toMm(track.x1, track.y1);
        const auto b = frame.toMm(track.x2, track.y2);
        const double z = isBottomCopper(layer) ? bottomZ - 0.010 : (silk ? silkZ : copperZ);
        if (!silk) {
            // Under the mask a track shows as a raised ribbon: a darker seam around it
            // gives the copper definition at any zoom level.
            scene.addSegment(a.first, a.second, b.first, b.second,
                             (width + 4.0) * kMilToMm, z - 0.004, palette.copperTrench);
        }
        std::uint32_t colour = palette.copperTop;
        if (silk) colour = palette.silk;
        else if (isBottomCopper(layer)) colour = palette.copperBottom;
        scene.addSegment(a.first, a.second, b.first, b.second, width * kMilToMm, z, colour);
    }

    for (const ArcFeature& arc : document.geometry.arcs) {
        const int layer = arc.layer;
        if (options.isHidden(layer)) continue;
        const bool silk = isTopSilk(layer) || isBottomSilk(layer);
        if (!(isTopCopper(layer) || isBottomCopper(layer) || silk)) continue;
        if (silk && !options.silk) continue;
        double sweep = std::fmod(arc.endAngle - arc.startAngle, 360.0);
        if (sweep <= 1e-6) sweep += 360.0;
        const int steps = std::max(8, static_cast<int>(std::fabs(sweep) / 6.0));
        const double z = isBottomCopper(layer) ? bottomZ - 0.010 : (silk ? silkZ : copperZ);
        const std::uint32_t colour = silk ? palette.silk
                                          : (isBottomCopper(layer) ? palette.copperBottom : palette.copperTop);
        const double width = std::max(arc.width, 4.0) * kMilToMm;
        Vec2 previous{0.0, 0.0};
        for (int i = 0; i <= steps; ++i) {
            const double angle = (arc.startAngle + sweep * static_cast<double>(i) / steps) * kPi / 180.0;
            const double x = arc.cx + std::cos(angle) * arc.radius;
            const double y = arc.cy + std::sin(angle) * arc.radius;
            const auto point = frame.toMm(x, y);
            if (i > 0) {
                scene.addSegment(previous.x, previous.y, point.first, point.second, width, z, colour);
            }
            previous = Vec2{point.first, point.second};
        }
    }

    for (const RegionFeature& region : document.geometry.regions) {
        const int layer = region.layer;
        if (options.isHidden(layer)) continue;
        const bool silk = isTopSilk(layer) || isBottomSilk(layer);
        const bool copper = isTopCopper(layer) || isBottomCopper(layer) || isInnerCopper(layer);
        if (!silk && !copper) continue;
        if (silk && !options.silk) continue;
        if (isInnerCopper(layer)) continue;
        const double z = isBottomCopper(layer) ? bottomZ - 0.008 : (silk ? silkZ : copperZ);
        const std::uint32_t colour = silk ? palette.silk
                                          : (isBottomCopper(layer) ? palette.copperBottom : palette.copperTop);
        std::vector<Vec3> points;
        points.reserve(region.outline.size());
        for (const Point& point : region.outline) {
            const auto mm = frame.toMm(point.x, point.y);
            points.push_back(Vec3{mm.first, mm.second, z});
        }
        scene.addPolygon(points, colour);
    }

    for (const FillFeature& fill : document.geometry.fills) {
        const int layer = fill.layer;
        if (options.isHidden(layer)) continue;
        const bool silk = isTopSilk(layer) || isBottomSilk(layer);
        const bool copper = isTopCopper(layer) || isBottomCopper(layer);
        if (!silk && !copper) continue;
        if (silk && !options.silk) continue;
        const double z = isBottomCopper(layer) ? bottomZ - 0.008 : (silk ? silkZ : copperZ);
        const std::uint32_t colour = silk ? palette.silk
                                          : (isBottomCopper(layer) ? palette.copperBottom : palette.copperTop);
        const auto a = frame.toMm(fill.x1, fill.y1);
        const auto b = frame.toMm(fill.x2, fill.y2);
        const double cx = (a.first + b.first) / 2.0;
        const double cy = (a.second + b.second) / 2.0;
        scene.addRotatedRect(cx, cy, std::fabs(b.first - a.first), std::fabs(b.second - a.second),
                             fill.rotation, z, colour);
    }

    // --- pads ---------------------------------------------------------------
    auto drawPad = [&](const Pad& pad, double z, bool top, bool withHole) {
        const auto centre = frame.toMm(pad.x, pad.y);
        const double width = std::max(pad.width * kMilToMm, 0.05);
        const double height = std::max(pad.height * kMilToMm, 0.05);
        const bool round = pad.holeSize > 0.0 && std::fabs(width - height) < std::max(width, height) * 0.15;
        // A thin darker ring separates the bare metal from the mask.
        const double ringZ = z - 0.003;
        if (round) {
            scene.disc(centre.first, centre.second, width / 2.0 + 0.05, ringZ, 20, palette.padEdge);
            scene.disc(centre.first, centre.second, width / 2.0, z, 20, palette.pad, 0.7);
        } else {
            scene.addRotatedRect(centre.first, centre.second, width + 0.10, height + 0.10,
                                 pad.rotation, ringZ, palette.padEdge);
            scene.addRotatedRect(centre.first, centre.second, width, height, pad.rotation, z,
                                 palette.pad, 0.7);
        }
        if (withHole && pad.holeSize > 0.0) {
            // A drilled hole is plated: the gold barrel is visible around the void, and the
            // wall catches the light on the side that faces it.
            const double holeRadius = pad.holeSize * kMilToMm / 2.0;
            const double facing = top ? 1.0 : -1.0;
            scene.disc(centre.first, centre.second, holeRadius * 1.12, z + 0.001 * facing, 18,
                       palette.barrel, 0.55);
            scene.disc(centre.first - holeRadius * 0.12, centre.second + holeRadius * 0.10,
                       holeRadius * 1.02, z + 0.002 * facing, 18,
                       scaleColor(palette.barrel, 0.72), 0.30);
            scene.disc(centre.first, centre.second, holeRadius * 0.86, z + 0.003 * facing, 16,
                       palette.hole);
        }
    };

    if (options.pads) {
        for (const Pad& pad : document.geometry.pads) {
            const bool throughHole = pad.holeSize > 0.0;
            const bool top = throughHole || pad.layer == 1 || pad.layer == 74 ||
                             (!isBottomCopper(pad.layer) && pad.layer != 0);
            const bool bottom = throughHole || isBottomCopper(pad.layer);
            if (top && !options.isHidden(1)) drawPad(pad, padZ, true, throughHole);
            if (bottom && !options.isHidden(32)) drawPad(pad, bottomZ - 0.016, false, throughHole);
        }
        for (const ViaFeature& via : document.geometry.vias) {
            const auto centre = frame.toMm(via.x, via.y);
            scene.disc(centre.first, centre.second, std::max(via.diameter, 10.0) * kMilToMm / 2.0,
                       topZ + 0.012, 18, palette.pad, 0.6);
            if (via.hole > 0.0) {
                const double holeRadius = via.hole * kMilToMm / 2.0;
                scene.disc(centre.first, centre.second, holeRadius * 1.10, topZ + 0.014, 16,
                           palette.barrel, 0.5);
                scene.disc(centre.first, centre.second, holeRadius * 0.85, topZ + 0.016, 12,
                           palette.hole);
            }
        }
    }

    // --- component bodies ---------------------------------------------------
    for (const Component& component : document.components) {
        const bool onBottom = component.layer.find("BOTTOM") != std::string::npos ||
                              component.layer.find("Bottom") != std::string::npos;
        if (onBottom && options.isHidden(32)) continue;
        if (!onBottom && options.isHidden(1)) continue;

        // The real model when the footprint brought one, a plan body otherwise.
        const PlacedModel* placed = modelFor(document, component.modelIndex);
        const BodyPlan plan = planBody(component);
        const auto centre = frame.toMm(component.x, component.y);
        const double baseZ = onBottom ? bottomZ : topZ;
        double height = plan.height;
        double minX = plan.minX, maxX = plan.maxX, minY = plan.minY, maxY = plan.maxY;
        if (placed != nullptr) {
            height = placed->height;
            minX = placed->minX;
            maxX = placed->maxX;
            minY = placed->minY;
            maxY = placed->maxY;
        }
        const double bodyZ = onBottom ? baseZ - height : baseZ + height;
        const double sign = onBottom ? -1.0 : 1.0;

        const std::uint32_t bodyTop = palette.body;
        const std::uint32_t bodySide = palette.bodySide;

        if (placed != nullptr) {
            // Rotate the STEP mesh onto the board exactly like the pads around it.
            const double rad = component.rotation * kPi / 180.0;
            const double c = std::cos(rad), s = std::sin(rad);
            auto toWorld = [&](const double* point) {
                return Vec3{centre.first + point[0] * c - point[1] * s,
                            centre.second + point[0] * s + point[1] * c,
                            baseZ + sign * point[2]};   // dz is already baked in by placeModel
            };
            std::uint32_t colour = palette.body;
            double metal = 0.10;
            if (placed->hasColor) {
                const int r = static_cast<int>(std::clamp(placed->colorR, 0.0, 1.0) * 255.0 + 0.5);
                const int g = static_cast<int>(std::clamp(placed->colorG, 0.0, 1.0) * 255.0 + 0.5);
                const int b = static_cast<int>(std::clamp(placed->colorB, 0.0, 1.0) * 255.0 + 0.5);
                colour = (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
                         static_cast<std::uint32_t>(b);
                const double brightness = (placed->colorR + placed->colorG + placed->colorB) / 3.0;
                const double spread = std::max({placed->colorR, placed->colorG, placed->colorB}) -
                                      std::min({placed->colorR, placed->colorG, placed->colorB});
                metal = (spread < 0.12 && brightness > 0.45) ? 0.55 : 0.10;   // metal can / tab
            }
            for (const StepTriangle& triangle : placed->triangles) {
                scene.addPolygon({toWorld(triangle.a), toWorld(triangle.b), toWorld(triangle.c)},
                                 colour, metal);
            }
        } else if (plan.cylinder) {
            scene.addCylinder(centre.first, centre.second, plan.radius, baseZ, bodyZ, 24,
                              scaleColor(palette.metal, 0.92), scaleColor(palette.metal, 0.70),
                              bodySide, 0.25);
            scene.disc(centre.first, centre.second, plan.radius * 0.94, bodyZ + 0.002 * sign, 24,
                       scaleColor(0x2b3038, 1.0));
            // Polarity stripe.
            scene.addRotatedRect(centre.first - plan.radius * 0.55, centre.second,
                                 plan.radius * 0.5, plan.radius * 1.2, 0.0, bodyZ + 0.004 * sign,
                                 scaleColor(0xd8dde3, 0.9));
        } else {
            // Body, rotated into the board frame.
            const double rad = component.rotation * kPi / 180.0;
            const double c = std::cos(rad), s = std::sin(rad);
            auto localToWorld = [&](double lx, double ly, double z) {
                return Vec3{centre.first + lx * c - ly * s, centre.second + lx * s + ly * c, z};
            };
            const double z0 = baseZ;
            const double z1 = bodyZ;
            scene.addPolygon({localToWorld(plan.minX, plan.minY, z1), localToWorld(plan.maxX, plan.minY, z1),
                              localToWorld(plan.maxX, plan.maxY, z1), localToWorld(plan.minX, plan.maxY, z1)},
                             bodyTop, 0.15);
            const Vec3 corners[4] = {localToWorld(plan.minX, plan.minY, 0.0),
                                     localToWorld(plan.maxX, plan.minY, 0.0),
                                     localToWorld(plan.maxX, plan.maxY, 0.0),
                                     localToWorld(plan.minX, plan.maxY, 0.0)};
            for (int i = 0; i < 4; ++i) {
                const Vec3 a = corners[i];
                const Vec3 b = corners[(i + 1) % 4];
                scene.addQuad({a.x, a.y, z0}, {b.x, b.y, z0}, {b.x, b.y, z1}, {a.x, a.y, z1},
                              bodySide);
            }
            if (plan.chip) {
                // Metal end caps of a chip part.
                const bool alongX = (plan.maxX - plan.minX) >= (plan.maxY - plan.minY);
                const double capWidth =
                    alongX ? (plan.maxX - plan.minX) * 0.22 : (plan.maxY - plan.minY) * 0.22;
                for (int side = 0; side < 2; ++side) {
                    const double signSide = side == 0 ? -1.0 : 1.0;
                    std::vector<Vec3> top4;
                    if (alongX) {
                        const double x = signSide < 0 ? plan.minX : plan.maxX - capWidth;
                        top4 = {localToWorld(x, plan.minY, z1 + 0.001), localToWorld(x + capWidth, plan.minY, z1 + 0.001),
                                localToWorld(x + capWidth, plan.maxY, z1 + 0.001), localToWorld(x, plan.maxY, z1 + 0.001)};
                    } else {
                        const double y = signSide < 0 ? plan.minY : plan.maxY - capWidth;
                        top4 = {localToWorld(plan.minX, y, z1 + 0.001), localToWorld(plan.maxX, y, z1 + 0.001),
                                localToWorld(plan.maxX, y + capWidth, z1 + 0.001), localToWorld(plan.minX, y + capWidth, z1 + 0.001)};
                    }
                    scene.addPolygon(top4, palette.metal, 0.55);
                }
            } else {
                // Pin 1 marker of an IC / connector.
                const double dotX = plan.minX + (plan.maxX - plan.minX) * 0.18;
                const double dotY = plan.maxY - (plan.maxY - plan.minY) * 0.18;
                const Vec3 marker = localToWorld(dotX, dotY, z1 + 0.004);
                scene.disc(marker.x, marker.y, std::max(0.12, (plan.maxX - plan.minX) * 0.05),
                           z1 + 0.004, 12, 0xdadfe6);
            }
        }

        // Contact shadow: the body footprint dropped on the board along the light.
        if (view.shadows) {
            const double drop = height + 0.02;
            const double dx = -light.x / std::max(light.z, 0.2) * drop;
            const double dy = -light.y / std::max(light.z, 0.2) * drop;
            const double rad = component.rotation * kPi / 180.0;
            const double c = std::cos(rad), s = std::sin(rad);
            // Three offset copies at falling opacity: a cheap blur that reads as a soft
            // shadow instead of a hard black rectangle.
            for (const auto& layer : {std::pair<double, double>{1.00, 0.20},
                                      std::pair<double, double>{1.16, 0.12},
                                      std::pair<double, double>{1.34, 0.06}}) {
                std::vector<Vec3> shadow;
                for (const Vec2& local : {Vec2{minX, minY}, Vec2{maxX, minY},
                                          Vec2{maxX, maxY}, Vec2{minX, maxY}}) {
                    shadow.push_back(Vec3{centre.first + local.x * c - local.y * s + dx * layer.first,
                                          centre.second + local.x * s + local.y * c + dy * layer.first,
                                          onBottom ? bottomZ - 0.004 : topZ + 0.004});
                }
                scene.addPolygon(shadow, 0x000000, 0.0, layer.second);
            }
        }
    }

    // --- camera fit ---------------------------------------------------------
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const Face& face : scene.faces) {
        for (const Vec3& point : face.points) {
            const Vec3 relative = point - camera.eye;
            const double x = dot(relative, camera.right);
            const double y = dot(relative, camera.up);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    if (minX > maxX) return empty;

    const int factor = std::clamp(view.supersample, 1, 3);
    const int outWidth = std::max(1, view.width);
    const int outHeight = std::max(1, view.height);
    const int width = outWidth * factor;
    const int height = outHeight * factor;

    const double spanX = std::max(maxX - minX, 1e-3);
    const double spanY = std::max(maxY - minY, 1e-3);
    const double zoom = std::clamp(view.zoom, 0.05, 40.0);
    camera.scale = std::min(width * 0.90 / spanX, height * 0.90 / spanY) * zoom;
    camera.cx = width / 2.0 - (minX + maxX) / 2.0 * camera.scale;
    camera.cy = height / 2.0 - (minY + maxY) / 2.0 * camera.scale;

    Renderer renderer;
    renderer.camera = camera;
    renderer.light = light;
    renderer.fillLight = fillLight;
    renderer.rimLight = rimLight;
    renderer.toEye = toEye;
    renderer.resize(width, height, palette.skyBottom);
    renderer.gradientBackground(palette.skyTop, palette.skyBottom);
    renderer.drawScene(scene);

    ThreeDImage image = renderer.resolve(outWidth, outHeight, factor);
    image.labels = {};

    // --- labels -------------------------------------------------------------
    if (view.labels) {
        for (const Component& component : document.components) {
            if (component.designator.empty()) continue;
            const auto centre = frame.toMm(component.x, component.y);
            // The designator sits on top of the body, not on the board under it, so a tall
            // part never swallows its own label.
            const PlacedModel* model = modelFor(document, component.modelIndex);
            const double bodyHeight = model != nullptr ? model->height : planBody(component).height;
            const bool onBottom = component.layer.find("BOTTOM") != std::string::npos ||
                                  component.layer.find("Bottom") != std::string::npos;
            const double labelZ = onBottom ? bottomZ - bodyHeight - 0.2 : topZ + bodyHeight + 0.2;
            double depth = 0.0;
            const Vec2 projected = camera.project(Vec3{centre.first, centre.second, labelZ}, depth);
            ThreeDLabel label;
            label.x = projected.x / factor;
            label.y = projected.y / factor;
            label.size = 11.0;
            label.text = component.designator;
            label.color = 0xffffff;
            image.labels.push_back(std::move(label));
        }
    }
    return image;
}

std::string threeDToBmp(const ThreeDImage& image) {
    if (!image.valid()) return {};
    const int rowBytes = image.width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const std::uint32_t dataSize = static_cast<std::uint32_t>((rowBytes + padding) * image.height);
    const std::uint32_t fileSize = 54 + dataSize;

    std::string out;
    out.reserve(fileSize + 16);
    auto put16 = [&out](std::uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    auto put32 = [&out](std::uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>((value >> 16) & 0xFF));
        out.push_back(static_cast<char>((value >> 24) & 0xFF));
    };

    out.push_back('B');
    out.push_back('M');
    put32(fileSize);
    put16(0);
    put16(0);
    put32(54);
    put32(40);                                   // header size
    put32(static_cast<std::uint32_t>(image.width));
    put32(static_cast<std::uint32_t>(image.height));
    put16(1);                                    // planes
    put16(24);                                   // bits per pixel
    put32(0);                                    // no compression
    put32(dataSize);
    put32(2835);                                 // 72 dpi
    put32(2835);
    put32(0);
    put32(0);

    for (int y = image.height - 1; y >= 0; --y) {   // BMP rows run bottom to top
        for (int x = 0; x < image.width; ++x) {
            const std::uint32_t c = image.at(x, y);
            out.push_back(static_cast<char>(c & 0xFF));
            out.push_back(static_cast<char>((c >> 8) & 0xFF));
            out.push_back(static_cast<char>((c >> 16) & 0xFF));
        }
        for (int i = 0; i < padding; ++i) out.push_back('\0');
    }
    return out;
}

} // namespace pcbv
