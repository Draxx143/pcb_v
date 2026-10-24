// Step.cpp — see Step.h.
#include "Step.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace pcbv {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec3d operator+(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3d operator-(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3d operator*(const Vec3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3d& a) { return std::sqrt(dot(a, a)); }
Vec3d normalised(const Vec3d& a) {
    const double len = norm(a);
    return len > 1e-12 ? a * (1.0 / len) : Vec3d{0.0, 0.0, 1.0};
}

// ---------------------------------------------------------------------------
// Argument tree
// ---------------------------------------------------------------------------

struct Value {
    enum class Kind { Empty, Ref, Number, Text, Enum, List } kind = Kind::Empty;
    int ref = 0;
    double number = 0.0;
    std::string text;
    std::vector<Value> items;

    bool isRef() const { return kind == Kind::Ref; }
    bool isNumber() const { return kind == Kind::Number; }
    bool isList() const { return kind == Kind::List; }
    bool isEnum() const { return kind == Kind::Enum; }
    bool isText() const { return kind == Kind::Text; }
    const Value& at(std::size_t index) const {
        static const Value empty;
        return index < items.size() ? items[index] : empty;
    }
    double num(double fallback = 0.0) const { return isNumber() ? number : fallback; }
    int refOrZero() const { return isRef() ? ref : 0; }
    bool flag(bool fallback = true) const {
        if (isEnum()) return text == ".T." || text == "T" || text == ".TRUE.";
        if (isText()) return text == "T" || text == "TRUE";
        if (isNumber()) return number != 0.0;
        return fallback;
    }
};

Value parseValue(const std::string& text, std::size_t& pos);

void skipSpaces(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
                                 text[pos] == '\n')) {
        ++pos;
    }
}

std::string parseStringLiteral(const std::string& text, std::size_t& pos) {
    // pos points at the opening quote.
    ++pos;
    std::string out;
    while (pos < text.size()) {
        if (text[pos] == '\'') {
            if (pos + 1 < text.size() && text[pos + 1] == '\'') {   // escaped quote
                out.push_back('\'');
                pos += 2;
                continue;
            }
            ++pos;
            break;
        }
        out.push_back(text[pos++]);
    }
    return out;
}

std::vector<Value> parseList(const std::string& text, std::size_t& pos) {
    std::vector<Value> items;
    ++pos;   // '('
    for (;;) {
        skipSpaces(text, pos);
        if (pos >= text.size()) break;
        if (text[pos] == ')') {
            ++pos;
            break;
        }
        items.push_back(parseValue(text, pos));
        skipSpaces(text, pos);
        if (pos < text.size() && text[pos] == ',') ++pos;
    }
    return items;
}

Value parseValue(const std::string& text, std::size_t& pos) {
    Value value;
    skipSpaces(text, pos);
    if (pos >= text.size()) return value;

    const char c = text[pos];
    if (c == '(') {
        value.kind = Value::Kind::List;
        value.items = parseList(text, pos);
        return value;
    }
    if (c == '\'') {
        value.kind = Value::Kind::Text;
        value.text = parseStringLiteral(text, pos);
        return value;
    }
    if (c == '#') {
        ++pos;
        long id = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            id = id * 10 + (text[pos] - '0');
            ++pos;
        }
        value.kind = Value::Kind::Ref;
        value.ref = static_cast<int>(id);
        return value;
    }
    if (c == '.') {
        const std::size_t end = text.find('.', pos + 1);
        if (end == std::string::npos) {
            value.kind = Value::Kind::Enum;
            value.text = text.substr(pos);
            pos = text.size();
            return value;
        }
        value.kind = Value::Kind::Enum;
        value.text = text.substr(pos, end - pos + 1);
        pos = end + 1;
        return value;
    }
    if (c == '*' || c == '$') {
        ++pos;
        return value;
    }
    // Number (or something we do not care about, e.g. a typed parameter).
    char* end = nullptr;
    const double number = std::strtod(text.c_str() + pos, &end);
    if (end != nullptr && end != text.c_str() + pos) {
        value.kind = Value::Kind::Number;
        value.number = number;
        pos = static_cast<std::size_t>(end - text.c_str());
        return value;
    }
    // Identifier such as "TRUE" or "LENGTH_MEASURE(1.)".
    std::string word;
    while (pos < text.size() && text[pos] != ',' && text[pos] != ')') word.push_back(text[pos++]);
    value.kind = Value::Kind::Text;
    value.text = word;
    return value;
}

struct Entity {
    int id = 0;
    std::string type;
    std::vector<Value> args;
};

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

struct Placement {
    Vec3d origin;
    Vec3d x{1.0, 0.0, 0.0};
    Vec3d y{0.0, 1.0, 0.0};
    Vec3d z{0.0, 0.0, 1.0};
};

struct Point2 {
    double u = 0.0;
    double v = 0.0;
};

class Reader {
public:
    bool parse(const std::string& text, StepMesh& mesh, std::string* error);

private:
    const Entity* entity(int id) const {
        const auto it = entities_.find(id);
        return it == entities_.end() ? nullptr : &it->second;
    }
    const Entity* entityOfType(int id, const char* type) const {
        const Entity* found = entity(id);
        return (found != nullptr && found->type == type) ? found : nullptr;
    }

    Vec3d point(int id) const;
    Vec3d direction(int id, const Vec3d& fallback) const;
    Placement placement(int id) const;

    void buildLoop(int edgeLoopId, std::vector<Vec3d>& out) const;
    void buildEdge(int orientedEdgeId, std::vector<Vec3d>& out) const;
    void buildCurve(int edgeCurveId, bool fromStart, std::vector<Vec3d>& out) const;

    bool tessellateFace(const Entity& face, StepMesh& mesh, std::string& why) const;

    std::map<int, Entity> entities_;
};

Vec3d Reader::point(int id) const {
    const Entity* entity = entityOfType(id, "CARTESIAN_POINT");
    if (entity == nullptr) return Vec3d{0.0, 0.0, 0.0};
    const Value& coordinates = entity->args.at(1);
    return Vec3d{coordinates.at(0).num(), coordinates.at(1).num(), coordinates.at(2).num()};
}

Vec3d Reader::direction(int id, const Vec3d& fallback) const {
    const Entity* entity = entityOfType(id, "DIRECTION");
    if (entity == nullptr) return fallback;
    const Value& coordinates = entity->args.at(1);
    return normalised(Vec3d{coordinates.at(0).num(), coordinates.at(1).num(), coordinates.at(2).num()});
}

Placement Reader::placement(int id) const {
    Placement result;
    const Entity* entity = entityOfType(id, "AXIS2_PLACEMENT_3D");
    if (entity == nullptr) return result;
    result.origin = point(entity->args.at(1).refOrZero());
    result.z = direction(entity->args.at(2).refOrZero(), Vec3d{0.0, 0.0, 1.0});
    const Vec3d refX = direction(entity->args.at(3).refOrZero(), Vec3d{1.0, 0.0, 0.0});
    result.x = normalised(refX - result.z * dot(refX, result.z));
    result.y = cross(result.z, result.x);
    return result;
}

void Reader::buildCurve(int edgeCurveId, bool fromStart, std::vector<Vec3d>& out) const {
    const Entity* edge = entityOfType(edgeCurveId, "EDGE_CURVE");
    if (edge == nullptr) return;
    const int startId = edge->args.at(1).refOrZero();
    const int endId = edge->args.at(2).refOrZero();
    const int curveId = edge->args.at(3).refOrZero();
    const bool sameSense = edge->args.at(4).flag(true);

    const Entity* startVertex = entityOfType(startId, "VERTEX_POINT");
    const Entity* endVertex = entityOfType(endId, "VERTEX_POINT");
    const Vec3d start = startVertex != nullptr ? point(startVertex->args.at(1).refOrZero()) : Vec3d{};
    const Vec3d end = endVertex != nullptr ? point(endVertex->args.at(1).refOrZero()) : Vec3d{};

    std::vector<Vec3d> points;
    if (const Entity* circle = entityOfType(curveId, "CIRCLE")) {
        const Placement p = placement(circle->args.at(1).refOrZero());
        const double radius = circle->args.at(2).num();
        if (radius > 1e-9) {
            // The angle of a point on the circle, in radians. (It used to be divided by the
            // radius, which shortened every partial arc: a cylinder side bounded by arcs
            // then collapsed into a flat disc.)
            auto angleOf = [&p](const Vec3d& world) {
                const Vec3d relative = world - p.origin;
                return std::atan2(dot(relative, p.y), dot(relative, p.x));
            };
            double from = angleOf(start);
            double to = angleOf(end);
            if (sameSense) {
                while (to <= from + 1e-9) to += 2.0 * kPi;
            } else {
                while (to >= from - 1e-9) to -= 2.0 * kPi;
            }
            const double sweep = to - from;
            const int steps = std::clamp(static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 12.0))), 1, 128);
            for (int i = 0; i <= steps; ++i) {
                const double angle = from + sweep * static_cast<double>(i) / steps;
                points.push_back(p.origin + p.x * (radius * std::cos(angle)) +
                                 p.y * (radius * std::sin(angle)));
            }
        }
    } else if (const Entity* ellipse = entityOfType(curveId, "ELLIPSE")) {
        const Placement p = placement(ellipse->args.at(1).refOrZero());
        const double major = ellipse->args.at(2).num();
        const double minor = ellipse->args.at(3).num();
        if (major > 1e-9 && minor > 1e-9) {
            auto angleOf = [&p](const Vec3d& world) {
                const Vec3d relative = world - p.origin;
                return std::atan2(dot(relative, p.y) / 1.0, dot(relative, p.x) / 1.0);
            };
            double from = angleOf(start);
            double to = angleOf(end);
            if (sameSense) {
                while (to <= from + 1e-9) to += 2.0 * kPi;
            } else {
                while (to >= from - 1e-9) to -= 2.0 * kPi;
            }
            const int steps = std::clamp(static_cast<int>(std::ceil(std::fabs(to - from) / (kPi / 12.0))), 1, 128);
            for (int i = 0; i <= steps; ++i) {
                const double angle = from + (to - from) * static_cast<double>(i) / steps;
                points.push_back(p.origin + p.x * (major * std::cos(angle)) +
                                 p.y * (minor * std::sin(angle)));
            }
        }
    }
    if (points.size() < 2) {
        // A line, a B-spline edge or anything else we cannot sample: a straight chord
        // between the two vertices is a good approximation for a component body.
        points = {start, end};
    }
    if (!sameSense && entityOfType(curveId, "CIRCLE") == nullptr &&
        entityOfType(curveId, "ELLIPSE") == nullptr) {
        std::reverse(points.begin(), points.end());
    }
    if (!fromStart) std::reverse(points.begin(), points.end());
    out.insert(out.end(), points.begin(), points.end());
}

void Reader::buildEdge(int orientedEdgeId, std::vector<Vec3d>& out) const {
    const Entity* oriented = entityOfType(orientedEdgeId, "ORIENTED_EDGE");
    if (oriented == nullptr) return;
    const int edgeId = oriented->args.at(3).refOrZero();
    const bool orientation = oriented->args.at(4).flag(true);
    buildCurve(edgeId, orientation, out);
}

void Reader::buildLoop(int edgeLoopId, std::vector<Vec3d>& out) const {
    const Entity* loop = entityOfType(edgeLoopId, "EDGE_LOOP");
    if (loop == nullptr) return;
    const Value& edges = loop->args.at(1);
    for (const Value& edge : edges.items) {
        if (edge.isRef()) buildEdge(edge.ref, out);
    }
    // Drop the duplicated vertices between consecutive edges.
    std::vector<Vec3d> cleaned;
    for (const Vec3d& point3d : out) {
        if (!cleaned.empty()) {
            const Vec3d delta = point3d - cleaned.back();
            if (norm(delta) < 1e-7) continue;
        }
        cleaned.push_back(point3d);
    }
    if (cleaned.size() > 1 && norm(cleaned.front() - cleaned.back()) < 1e-7) cleaned.pop_back();
    out.swap(cleaned);
}

namespace {

/// Ear clipping for a simple polygon (counter clockwise). Returns triangle indices.
std::vector<int> triangulate(const std::vector<Point2>& points) {
    std::vector<int> indices;
    const int count = static_cast<int>(points.size());
    if (count < 3) return indices;

    std::vector<int> remaining(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) remaining[static_cast<std::size_t>(i)] = i;

    auto area = [&points](int a, int b, int c) {
        return (points[static_cast<std::size_t>(b)].u - points[static_cast<std::size_t>(a)].u) *
                   (points[static_cast<std::size_t>(c)].v - points[static_cast<std::size_t>(a)].v) -
               (points[static_cast<std::size_t>(c)].u - points[static_cast<std::size_t>(a)].u) *
                   (points[static_cast<std::size_t>(b)].v - points[static_cast<std::size_t>(a)].v);
    };

    int guard = 0;
    while (remaining.size() > 3 && guard < count * count + 16) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const int a = remaining[(i + remaining.size() - 1) % remaining.size()];
            const int b = remaining[i];
            const int c = remaining[(i + 1) % remaining.size()];
            if (area(a, b, c) <= 1e-12) continue;   // reflex or degenerate
            bool contains = false;
            for (const int other : remaining) {
                if (other == a || other == b || other == c) continue;
                const double d1 = area(a, b, other);
                const double d2 = area(b, c, other);
                const double d3 = area(c, a, other);
                if (d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) {
                    contains = true;
                    break;
                }
            }
            if (contains) continue;
            indices.insert(indices.end(), {a, b, c});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        ++guard;
        if (!clipped) break;
    }
    if (remaining.size() == 3) {
        indices.insert(indices.end(), {remaining[0], remaining[1], remaining[2]});
    } else if (!remaining.empty()) {
        for (std::size_t i = 1; i + 1 < remaining.size(); ++i) {
            indices.insert(indices.end(), {remaining[0], remaining[i], remaining[i + 1]});
        }
    }
    return indices;
}

double polygonArea(const std::vector<Point2>& points) {
    double sum = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point2& a = points[i];
        const Point2& b = points[(i + 1) % points.size()];
        sum += a.u * b.v - b.u * a.v;
    }
    return sum / 2.0;
}

enum class SurfaceKind { Plane, Cylinder, Cone, Sphere, Torus, Unsupported };

struct Surface {
    SurfaceKind kind = SurfaceKind::Unsupported;
    Placement placement;
    double a = 0.0;   // radius / major radius
    double b = 0.0;   // semi angle (radians) / minor radius
};

} // namespace (helpers)

} // namespace

void StepMesh::add(const StepTriangle& triangle) {
    triangles.push_back(triangle);
    for (const double* vertex : {triangle.a, triangle.b, triangle.c}) {
        minX = std::min(minX, vertex[0]);
        minY = std::min(minY, vertex[1]);
        minZ = std::min(minZ, vertex[2]);
        maxX = std::max(maxX, vertex[0]);
        maxY = std::max(maxY, vertex[1]);
        maxZ = std::max(maxZ, vertex[2]);
    }
}

void StepMesh::centre(double& x, double& y, double& z) const {
    if (!valid()) {
        x = y = z = 0.0;
        return;
    }
    x = (minX + maxX) / 2.0;
    y = (minY + maxY) / 2.0;
    z = (minZ + maxZ) / 2.0;
}

namespace {

bool Reader::tessellateFace(const Entity& face, StepMesh& mesh, std::string& why) const {
    // ADVANCED_FACE('',(#bounds),#surface,.T.)
    const int surfaceId = face.args.at(2).refOrZero();
    const Entity* surfaceEntity = entity(surfaceId);
    if (surfaceEntity == nullptr) {
        ++mesh.skippedFaces;
        return false;
    }

    Surface surface;
    surface.kind = SurfaceKind::Unsupported;
    if (surfaceEntity->type == "PLANE") {
        surface.kind = SurfaceKind::Plane;
        surface.placement = placement(surfaceEntity->args.at(1).refOrZero());
    } else if (surfaceEntity->type == "CYLINDRICAL_SURFACE") {
        surface.kind = SurfaceKind::Cylinder;
        surface.placement = placement(surfaceEntity->args.at(1).refOrZero());
        surface.a = surfaceEntity->args.at(2).num();
    } else if (surfaceEntity->type == "CONICAL_SURFACE") {
        surface.kind = SurfaceKind::Cone;
        surface.placement = placement(surfaceEntity->args.at(1).refOrZero());
        surface.a = surfaceEntity->args.at(2).num();
        surface.b = surfaceEntity->args.at(3).num();
    } else if (surfaceEntity->type == "SPHERICAL_SURFACE") {
        surface.kind = SurfaceKind::Sphere;
        surface.placement = placement(surfaceEntity->args.at(1).refOrZero());
        surface.a = surfaceEntity->args.at(2).num();
    } else if (surfaceEntity->type == "TOROIDAL_SURFACE") {
        surface.kind = SurfaceKind::Torus;
        surface.placement = placement(surfaceEntity->args.at(1).refOrZero());
        surface.a = surfaceEntity->args.at(2).num();
        surface.b = surfaceEntity->args.at(3).num();
    } else {
        ++mesh.skippedFaces;
        if (why.empty()) why = "unsupported surface: " + surfaceEntity->type;
        return false;
    }
    if (surface.kind == SurfaceKind::Torus) {   // rare and expensive: skip
        ++mesh.skippedFaces;
        return false;
    }

    // The outer bound is the face; holes are ignored (component bodies rarely have any and
    // a missing hole is far less visible than a missing face).
    const Value& bounds = face.args.at(1);
    int loopId = 0;
    for (const Value& bound : bounds.items) {
        const Entity* boundEntity = entity(bound.refOrZero());
        if (boundEntity == nullptr) continue;
        const int candidate = boundEntity->args.at(1).refOrZero();
        if (boundEntity->type == "FACE_OUTER_BOUND") {
            loopId = candidate;
            break;
        }
        if (loopId == 0) loopId = candidate;
    }
    if (loopId == 0) {
        ++mesh.skippedFaces;
        return false;
    }

    std::vector<Vec3d> world;
    buildLoop(loopId, world);
    if (world.size() < 3) {
        ++mesh.skippedFaces;
        return false;
    }

    const Placement& p = surface.placement;
    std::vector<Point2> projected;
    projected.reserve(world.size());
    for (const Vec3d& vertex : world) {
        const Vec3d relative = vertex - p.origin;
        Point2 parameter;
        switch (surface.kind) {
            case SurfaceKind::Plane:
                parameter.u = dot(relative, p.x);
                parameter.v = dot(relative, p.y);
                break;
            case SurfaceKind::Cylinder:
                parameter.u = std::atan2(dot(relative, p.y), dot(relative, p.x)) * surface.a;
                parameter.v = dot(relative, p.z);
                break;
            case SurfaceKind::Cone: {
                parameter.u = std::atan2(dot(relative, p.y), dot(relative, p.x));
                parameter.v = dot(relative, p.z);
                // Unrolled radius at this height.
                const double radius = surface.a + parameter.v * std::tan(surface.b);
                parameter.u *= std::max(radius, 1e-6);
                break;
            }
            case SurfaceKind::Sphere:
                parameter.u = std::atan2(dot(relative, p.y), dot(relative, p.x));
                parameter.v = std::asin(std::clamp(dot(relative, p.z) / std::max(surface.a, 1e-9), -1.0, 1.0));
                break;
            default:
                break;
        }
        projected.push_back(parameter);
    }

    // Unwrap the angular parameter so a loop that runs around a cylinder or a sphere stays
    // continuous instead of jumping from +pi to -pi in the middle of the tessellation.
    if (surface.kind == SurfaceKind::Cylinder || surface.kind == SurfaceKind::Cone ||
        surface.kind == SurfaceKind::Sphere) {
        for (std::size_t i = 1; i < projected.size(); ++i) {
            while (projected[i].u - projected[i - 1].u > kPi * surface.a) projected[i].u -= 2.0 * kPi * surface.a;
            while (projected[i - 1].u - projected[i].u > kPi * surface.a) projected[i].u += 2.0 * kPi * surface.a;
        }
    }

    auto toWorld = [&surface](const Point2& parameter) {
        const Placement& base = surface.placement;
        switch (surface.kind) {
            case SurfaceKind::Plane:
                return base.origin + base.x * parameter.u + base.y * parameter.v;
            case SurfaceKind::Cylinder: {
                const double angle = parameter.u / std::max(surface.a, 1e-9);
                return base.origin + base.x * (surface.a * std::cos(angle)) +
                       base.y * (surface.a * std::sin(angle)) + base.z * parameter.v;
            }
            case SurfaceKind::Cone: {
                const double radius = surface.a + parameter.v * std::tan(surface.b);
                const double angle = parameter.u / std::max(std::fabs(radius), 1e-9);
                return base.origin + base.x * (radius * std::cos(angle)) +
                       base.y * (radius * std::sin(angle)) + base.z * parameter.v;
            }
            case SurfaceKind::Sphere: {
                return base.origin +
                       base.x * (surface.a * std::cos(parameter.v) * std::cos(parameter.u)) +
                       base.y * (surface.a * std::cos(parameter.v) * std::sin(parameter.u)) +
                       base.z * (surface.a * std::sin(parameter.v));
            }
            default:
                return base.origin;
        }
    };

    // The side of a cylinder or a cone is bounded by two arcs and two seams, so in
    // (arc length, height) space its outline is a rectangle: one chain of vertices at the
    // bottom height, one at the top. Triangulating that outline as a single polygon gives
    // slivers that collapse into discs (the body of an axial resistor came out as a blob),
    // so build the strip from the two heights directly.
    if ((surface.kind == SurfaceKind::Cylinder || surface.kind == SurfaceKind::Cone) &&
        projected.size() >= 6) {
        double vMin = 1e18, vMax = -1e18;
        for (const Point2& pt : projected) {
            vMin = std::min(vMin, pt.v);
            vMax = std::max(vMax, pt.v);
        }
        const double vTolerance = std::max(1e-6, (vMax - vMin) * 0.02);
        if (vMax - vMin > vTolerance * 4.0) {
            int lowCount = 0, highCount = 0;
            double uMin = 1e18, uMax = -1e18;
            for (const Point2& pt : projected) {
                const bool low = pt.v - vMin <= vTolerance;
                const bool high = vMax - pt.v <= vTolerance;
                if (low) ++lowCount;
                if (high) ++highCount;
                if (low || high) {
                    uMin = std::min(uMin, pt.u);
                    uMax = std::max(uMax, pt.u);
                }
            }
            const double sweep = uMax - uMin;
            if (lowCount >= 2 && highCount >= 2 && sweep > 1e-6) {
                const double step = std::max(surface.a, 0.05) * kPi / 16.0;
                const int segments = std::clamp(static_cast<int>(std::ceil(sweep / step)), 2, 128);
                StepTriangle band;
                for (int i = 0; i < segments; ++i) {
                    const double u0 = uMin + sweep * static_cast<double>(i) / segments;
                    const double u1 = uMin + sweep * static_cast<double>(i + 1) / segments;
                    const Vec3d a = toWorld(Point2{u0, vMin});
                    const Vec3d b = toWorld(Point2{u1, vMin});
                    const Vec3d c = toWorld(Point2{u1, vMax});
                    const Vec3d d = toWorld(Point2{u0, vMax});
                    if (norm(cross(b - a, c - a)) > 1e-10) {
                        band.a[0] = a.x; band.a[1] = a.y; band.a[2] = a.z;
                        band.b[0] = b.x; band.b[1] = b.y; band.b[2] = b.z;
                        band.c[0] = c.x; band.c[1] = c.y; band.c[2] = c.z;
                        mesh.add(band);
                    }
                    if (norm(cross(c - a, d - a)) > 1e-10) {
                        band.a[0] = a.x; band.a[1] = a.y; band.a[2] = a.z;
                        band.b[0] = c.x; band.b[1] = c.y; band.b[2] = c.z;
                        band.c[0] = d.x; band.c[1] = d.y; band.c[2] = d.z;
                        mesh.add(band);
                    }
                }
                ++mesh.faces;
                return true;
            }
        }
    }

    std::vector<Point2> oriented = projected;
    if (polygonArea(oriented) < 0.0) std::reverse(oriented.begin(), oriented.end());
    std::vector<int> indices = triangulate(oriented);
    if (indices.size() < 3) {
        ++mesh.skippedFaces;
        return false;
    }

    StepTriangle triangle;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3d a = toWorld(projected[static_cast<std::size_t>(indices[i])]);
        const Vec3d b = toWorld(projected[static_cast<std::size_t>(indices[i + 1])]);
        const Vec3d c = toWorld(projected[static_cast<std::size_t>(indices[i + 2])]);
        if (norm(cross(b - a, c - a)) < 1e-10) continue;   // degenerate
        triangle.a[0] = a.x; triangle.a[1] = a.y; triangle.a[2] = a.z;
        triangle.b[0] = b.x; triangle.b[1] = b.y; triangle.b[2] = b.z;
        triangle.c[0] = c.x; triangle.c[1] = c.y; triangle.c[2] = c.z;
        mesh.add(triangle);
    }
    ++mesh.faces;
    return true;
}

namespace {

/// Picks the geometry part out of a complex entity instance such as
/// `( BOUNDED_SURFACE() B_SPLINE_SURFACE() B_SPLINE_SURFACE_WITH_KNOTS(...) )` and returns
/// its argument text. The names are tried most specific first, so a b-spline wins over the
/// generic `BOUNDED_SURFACE` marker.
std::string unwrapComplexEntity(const std::string& body, std::string& type) {
    static const char* kPreferred[] = {
        "B_SPLINE_SURFACE_WITH_KNOTS", "B_SPLINE_CURVE_WITH_KNOTS",
        "SURFACE_OF_LINEAR_EXTRUSION", "SURFACE_OF_REVOLUTION",
        "CYLINDRICAL_SURFACE", "CONICAL_SURFACE", "SPHERICAL_SURFACE", "TOROIDAL_SURFACE",
        "B_SPLINE_SURFACE",   "B_SPLINE_CURVE", "PLANE", "CIRCLE", "ELLIPSE", "LINE",
    };
    struct Part { std::string name; std::string args; };
    std::vector<Part> parts;
    std::size_t i = 0;
    while (i < body.size()) {
        // A part looks like NAME(...) or NAME().
        while (i < body.size() && !std::isalnum(static_cast<unsigned char>(body[i])) &&
               body[i] != '_')
            ++i;
        const std::size_t start = i;
        while (i < body.size() && (std::isalnum(static_cast<unsigned char>(body[i])) ||
                                   body[i] == '_'))
            ++i;
        if (i == start) break;
        const std::string name = body.substr(start, i - start);
        std::size_t open = i;
        while (open < body.size() && body[open] == ' ') ++open;
        if (open >= body.size() || body[open] != '(') continue;
        std::size_t scan = open + 1;
        int depth = 1;
        while (scan < body.size() && depth > 0) {
            if (body[scan] == '\'') {
                ++scan;
                while (scan < body.size() && body[scan] != '\'') ++scan;
                if (scan < body.size()) ++scan;
                continue;
            }
            if (body[scan] == '(') ++depth;
            if (body[scan] == ')') --depth;
            ++scan;
        }
        parts.push_back({name, body.substr(open + 1, scan > open + 1 ? scan - open - 2 : 0)});
        i = scan;
    }
    for (const char* wanted : kPreferred) {
        for (const Part& part : parts) {
            if (part.name == wanted) {
                type = part.name;
                return part.args;
            }
        }
    }
    if (!parts.empty()) {
        type = parts.back().name;
        return parts.back().args;
    }
    return body;
}

} // namespace

bool Reader::parse(const std::string& text, StepMesh& mesh, std::string* error) {
    // ---- split into entities -------------------------------------------------
    std::size_t pos = text.find("DATA;");
    if (pos == std::string::npos) {
        if (error != nullptr) *error = "no DATA section";
        return false;
    }
    pos += 5;
    const std::size_t end = text.find("ENDSEC;", pos);
    const std::size_t limit = end == std::string::npos ? text.size() : end;

    while (pos < limit) {
        skipSpaces(text, pos);
        if (pos >= limit) break;
        if (text[pos] != '#') {
            ++pos;
            continue;
        }
        ++pos;
        int id = 0;
        while (pos < limit && text[pos] >= '0' && text[pos] <= '9') {
            id = id * 10 + (text[pos] - '0');
            ++pos;
        }
        skipSpaces(text, pos);
        if (pos < limit && text[pos] == '=') ++pos;
        skipSpaces(text, pos);
        std::string type;
        while (pos < limit && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
            type.push_back(text[pos++]);
        }
        skipSpaces(text, pos);
        if (pos >= limit || text[pos] != '(') continue;
        std::size_t scan = pos + 1;
        int depth = 1;
        while (scan < limit && depth > 0) {
            const char c = text[scan];
            if (c == '\'') {
                ++scan;
                while (scan < limit) {
                    if (text[scan] == '\'') {
                        if (scan + 1 < limit && text[scan + 1] == '\'') {
                            scan += 2;
                            continue;
                        }
                        ++scan;
                        break;
                    }
                    ++scan;
                }
                continue;
            }
            if (c == '(') ++depth;
            if (c == ')') --depth;
            ++scan;
        }
        std::string arguments = text.substr(pos + 1, scan > pos + 1 ? scan - pos - 2 : 0);
        // Some exporters write a complex instance instead of a single entity:
        //     #12 = ( BOUNDED_SURFACE() CYLINDRICAL_SURFACE('',#13,0.8) );
        // The geometry lives in one of the parts, so pick the most specific one and use its
        // arguments; otherwise every such face (the side of an axial resistor body, the can
        // of an LED, …) would be dropped and the part would render as flat discs only.
        std::string complexType;
        if (type.empty()) {
            arguments = unwrapComplexEntity(arguments, complexType);
        }
        if (!complexType.empty()) type = complexType;
        Entity entity;
        entity.id = id;
        entity.type = type;
        std::size_t argPos = 0;
        while (argPos <= arguments.size()) {
            skipSpaces(arguments, argPos);
            if (argPos >= arguments.size()) break;
            entity.args.push_back(parseValue(arguments, argPos));
            skipSpaces(arguments, argPos);
            if (argPos < arguments.size() && arguments[argPos] == ',') ++argPos;
            else if (argPos >= arguments.size()) break;
            else ++argPos;
        }
        entities_[id] = std::move(entity);
        pos = scan;
    }
    if (entities_.empty()) {
        if (error != nullptr) *error = "no entities";
        return false;
    }

    // The model usually declares one colour for the whole body; using it keeps a black
    // plastic package black and a metal can metallic instead of one grey for every part.
    // A package usually declares a bright colour for its metal and a dark one for its
    // moulded body; the darkest is the body, which is the face the eye reads first.
    double bestLuma = 1e18;
    for (const auto& entry : entities_) {
        const Entity& colour = entry.second;
        if (colour.type != "COLOUR_RGB" || colour.args.size() < 4) continue;
        const double r = colour.args[1].num(-1.0);
        const double g = colour.args[2].num(-1.0);
        const double b = colour.args[3].num(-1.0);
        if (r < 0.0 || g < 0.0 || b < 0.0) continue;
        const double luma = r * 0.299 + g * 0.587 + b * 0.114;
        if (luma >= bestLuma) continue;
        bestLuma = luma;
        mesh.hasColor = true;
        mesh.colorR = std::min(r, 1.0);
        mesh.colorG = std::min(g, 1.0);
        mesh.colorB = std::min(b, 1.0);
    }

    // ---- tessellate every solid ---------------------------------------------
    std::string why;
    std::size_t solids = 0;
    for (const auto& entry : entities_) {
        const Entity& solid = entry.second;
        if (solid.type != "MANIFOLD_SOLID_BREP" && solid.type != "BREP_WITH_VOIDS" &&
            solid.type != "FACETED_BREP" && solid.type != "SHELL_BASED_SURFACE_MODEL") {
            continue;
        }
        std::vector<int> shells;
        if (solid.type == "MANIFOLD_SOLID_BREP" || solid.type == "FACETED_BREP") {
            shells.push_back(solid.args.at(1).refOrZero());
        } else if (solid.type == "BREP_WITH_VOIDS") {
            shells.push_back(solid.args.at(1).refOrZero());
            for (const Value& voidShell : solid.args.at(2).items) shells.push_back(voidShell.refOrZero());
        } else {
            for (const Value& shell : solid.args.at(1).items) shells.push_back(shell.refOrZero());
        }
        ++solids;
        std::vector<int> faceIds;
        for (const int shellId : shells) {
            const Entity* shell = entity(shellId);
            if (shell == nullptr) continue;
            for (const Value& face : shell->args.at(1).items) faceIds.push_back(face.refOrZero());
        }
        for (const int faceId : faceIds) {
            const Entity* face = entity(faceId);
            if (face == nullptr) continue;
            if (face->type != "ADVANCED_FACE" && face->type != "FACE_SURFACE") continue;
            tessellateFace(*face, mesh, why);
        }
    }

    if (mesh.triangles.empty()) {
        if (error != nullptr) {
            *error = solids == 0 ? "no solid geometry" : (why.empty() ? "empty tessellation" : why);
        }
        return false;
    }
    return true;
}

} // namespace

bool looksLikeStep(const std::string& bytes) {
    return bytes.find("ISO-10303-21") != std::string::npos;
}

bool parseStepMesh(const std::string& text, StepMesh& mesh, std::string* error) {
    mesh = StepMesh();
    Reader reader;
    return reader.parse(text, mesh, error);
}

} // namespace pcbv
