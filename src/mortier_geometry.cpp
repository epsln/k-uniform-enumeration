#include "mortier_geometry.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include <boost/multiprecision/cpp_int.hpp>

bool operator==(const Z4Point& left, const Z4Point& right) {
    return left.coordinates == right.coordinates;
}

bool operator<(const Z4Point& left, const Z4Point& right) {
    return left.coordinates < right.coordinates;
}

namespace mortier_geometry {
namespace {

using Wide = boost::multiprecision::cpp_int;

Z4Point z4(int64_t a, int64_t b, int64_t c, int64_t d) {
    return Z4Point{{a, b, c, d}};
}

struct Quadratic {
    Wide rational = 0;
    Wide radical = 0;
};

struct Rational {
    Wide numerator = 0;
    Wide denominator = 1;
};

struct Control {
    int tile = 0;
    int orientation = 0; // Direction of physical edge zero.
    bool reflected = false;
    Z4Point anchor;       // Start of physical edge zero.
    size_t depth = 0;
};

struct ControlKey {
    int tile;
    int orientation;
    bool reflected;
    Z4Point anchor;

    bool operator<(const ControlKey& other) const {
        return std::tie(tile, orientation, reflected, anchor)
             < std::tie(other.tile, other.orientation, other.reflected, other.anchor);
    }
};

struct ControlClass {
    int tile;
    int orientation;
    bool reflected;

    bool operator<(const ControlClass& other) const {
        return std::tie(tile, orientation, reflected)
             < std::tie(other.tile, other.orientation, other.reflected);
    }
};

int mod12(int value) {
    value %= 12;
    return value < 0 ? value + 12 : value;
}

std::string point_text(const Z4Point& p) {
    std::ostringstream out;
    out << '[' << p.coordinates[0] << ',' << p.coordinates[1] << ','
        << p.coordinates[2] << ',' << p.coordinates[3] << ']';
    return out.str();
}

Quadratic determinant(const Z4Point& left, const Z4Point& right) {
    // Cartesian coordinates, multiplied by two, are
    // (2a+c + b*sqrt(3), b+2d + c*sqrt(3)).
    Wide lx = 2 * Wide(left.coordinates[0]) + left.coordinates[2];
    Wide lxr = left.coordinates[1];
    Wide ly = left.coordinates[1] + 2 * Wide(left.coordinates[3]);
    Wide lyr = left.coordinates[2];
    Wide rx = 2 * Wide(right.coordinates[0]) + right.coordinates[2];
    Wide rxr = right.coordinates[1];
    Wide ry = right.coordinates[1] + 2 * Wide(right.coordinates[3]);
    Wide ryr = right.coordinates[2];
    return {lx * ry + 3 * lxr * ryr - ly * rx - 3 * lyr * rxr,
            lx * ryr + lxr * ry - ly * rxr - lyr * rx};
}

int sign(const Quadratic& value) {
    Wide p = value.rational;
    Wide q = value.radical;
    if (q == 0) return (p > 0) - (p < 0);
    if (p == 0) return q > 0 ? 1 : -1;
    if ((p > 0) == (q > 0)) return p > 0 ? 1 : -1;
    Wide pp = p * p;
    Wide qq3 = 3 * q * q;
    if (pp == qq3) return 0; // Cannot occur for nonzero integral p,q.
    return pp > qq3 ? (p > 0 ? 1 : -1) : (q > 0 ? 1 : -1);
}

Quadratic subtract_multiple(Quadratic value, Wide multiple, Quadratic base) {
    value.rational -= multiple * base.rational;
    value.radical -= multiple * base.radical;
    return value;
}

long double approximate(Quadratic value) {
    return value.rational.convert_to<long double>()
         + std::sqrt(3.0L) * value.radical.convert_to<long double>();
}

int64_t checked_i64(Wide value, const char* operation) {
    if (value < std::numeric_limits<int64_t>::min()
        || value > std::numeric_limits<int64_t>::max())
        throw std::overflow_error(std::string("integer overflow while ") + operation);
    return value.convert_to<int64_t>();
}

int64_t floor_ratio(Quadratic numerator, Quadratic denominator) {
    int ds = sign(denominator);
    if (ds == 0) throw std::invalid_argument("rank-one translation basis");
    if (ds < 0) {
        numerator.rational = -numerator.rational;
        numerator.radical = -numerator.radical;
        denominator.rational = -denominator.rational;
        denominator.radical = -denominator.radical;
    }
    long double guess_value = std::floor(approximate(numerator) / approximate(denominator));
    if (!std::isfinite(guess_value)
        || guess_value < static_cast<long double>(std::numeric_limits<int64_t>::min())
        || guess_value > static_cast<long double>(std::numeric_limits<int64_t>::max()))
        throw std::overflow_error("lattice quotient is outside int64 range");
    int64_t guess = static_cast<int64_t>(guess_value);
    while (sign(subtract_multiple(numerator, guess, denominator)) < 0) --guess;
    while (sign(subtract_multiple(numerator, Wide(guess) + 1, denominator)) >= 0) ++guess;
    return guess;
}

Wide wide_abs(Wide value) {
    return value < 0 ? -value : value;
}

Wide wide_gcd(Wide left, Wide right) {
    left = wide_abs(left);
    right = wide_abs(right);
    while (right != 0) {
        Wide remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

Rational rational_ratio(Quadratic numerator, Quadratic denominator) {
    Wide den = denominator.rational * denominator.rational
             - 3 * denominator.radical * denominator.radical;
    if (den == 0)
        throw std::runtime_error("zero determinant while extracting translation lattice");
    Wide p = numerator.rational * denominator.rational
           - 3 * numerator.radical * denominator.radical;
    Wide q = numerator.radical * denominator.rational
           - numerator.rational * denominator.radical;
    if (q != 0)
        throw std::runtime_error(
            "translation coordinate is not rational relative to the reference pair");
    if (den < 0) {
        p = -p;
        den = -den;
    }
    Wide divisor = wide_gcd(p, den);
    return {p / divisor, den / divisor};
}

bool wide_integral_ratio(Quadratic numerator, Quadratic denominator) {
    Rational ratio = rational_ratio(numerator, denominator);
    return ratio.denominator == 1;
}

std::vector<Z4Point> polygon_vertices(const Control& control, int sides) {
    int turn = 12 / sides;
    std::vector<Z4Point> vertices(static_cast<size_t>(sides));
    vertices[0] = control.anchor;
    for (int edge = 0; edge + 1 < sides; ++edge)
        vertices[edge + 1] = vertices[edge]
            + direction_step(control.orientation
                             + (control.reflected ? edge : -edge) * turn);
    Z4Point closure = vertices.back()
        + direction_step(control.orientation
                         + (control.reflected ? sides - 1 : 1 - sides) * turn);
    if (closure != control.anchor)
        throw std::runtime_error("exact regular polygon failed to close");
    return vertices;
}

int first_physical_edge(const TilingDescription& description, int tile, int pattern) {
    const auto& map = description.physical_to_pattern.at(static_cast<size_t>(tile));
    auto found = std::find(map.begin(), map.end(), pattern);
    if (found == map.end())
        throw std::invalid_argument("adjacency references absent pattern edge");
    return static_cast<int>(found - map.begin());
}

Control neighbour_control(const TilingDescription& description,
                          const Control& current, int physical_edge,
                          const std::vector<Z4Point>& vertices) {
    int pattern = description.physical_to_pattern[current.tile][physical_edge];
    const ConwayAdjacency& adjacent = description.adjacency[current.tile][pattern];
    int entry = first_physical_edge(description, adjacent.tile, adjacent.pattern_edge);
    Z4Point start = vertices[physical_edge];
    Z4Point end = vertices[(physical_edge + 1) % vertices.size()];
    int directed = mod12(current.orientation
                         + (current.reflected ? physical_edge : -physical_edge)
                             * (12 / description.polygon_sides[current.tile]));
    if (!adjacent.mirrored) {
        std::swap(start, end);
        directed = mod12(directed + 6);
    }
    bool reflected = current.reflected != adjacent.mirrored;
    int neighbour_turn = 12 / description.polygon_sides[adjacent.tile];
    int orientation = mod12(directed
                            + (reflected ? -entry : entry) * neighbour_turn);
    Z4Point anchor = start;
    for (int edge = 0; edge < entry; ++edge)
        anchor = anchor - direction_step(orientation
            + (reflected ? edge : -edge) * neighbour_turn);
    return {adjacent.tile, orientation, reflected, anchor, current.depth + 1};
}

void add_unique_nonzero(std::vector<Z4Point>& values, const Z4Point& value) {
    if (value == Z4Point{}) return;
    Z4Point canonical = value;
    if (-canonical < canonical) canonical = -canonical;
    if (std::find(values.begin(), values.end(), canonical) == values.end())
        values.push_back(canonical);
}

std::pair<Z4Point, Z4Point> choose_basis(const std::vector<Z4Point>& candidates) {
    const Z4Point* reference1 = nullptr;
    const Z4Point* reference2 = nullptr;
    for (size_t i = 0; i < candidates.size() && reference1 == nullptr; ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!cartesian_independent(candidates[i], candidates[j])) continue;
            Quadratic den = determinant(candidates[i], candidates[j]);
            bool rational_span = true;
            for (const Z4Point& candidate : candidates) {
                try {
                    rational_ratio(determinant(candidate, candidates[j]), den);
                    rational_ratio(determinant(candidates[i], candidate), den);
                } catch (const std::runtime_error&) {
                    rational_span = false;
                    break;
                }
            }
            if (rational_span) {
                reference1 = &candidates[i];
                reference2 = &candidates[j];
                break;
            }
        }
    }
    if (reference1 == nullptr)
        throw std::runtime_error(
            "translation candidates do not span a rank-two rational lattice");

    Quadratic reference_determinant = determinant(*reference1, *reference2);
    std::vector<std::pair<Rational, Rational>> rational_coordinates;
    Wide common_denominator = 1;
    for (const Z4Point& candidate : candidates) {
        Rational first, second;
        try {
            first = rational_ratio(determinant(candidate, *reference2),
                                   reference_determinant);
            second = rational_ratio(determinant(*reference1, candidate),
                                    reference_determinant);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) + " for candidate "
                + point_text(candidate) + " relative to " + point_text(*reference1)
                + " and " + point_text(*reference2));
        }
        rational_coordinates.push_back({first, second});
        for (const Wide& denominator : {first.denominator, second.denominator})
            common_denominator = common_denominator / wide_gcd(common_denominator, denominator)
                               * denominator;
    }

    std::vector<std::pair<Wide, Wide>> integer_coordinates;
    for (const auto& coordinate : rational_coordinates) {
        integer_coordinates.push_back({
            coordinate.first.numerator
                * (common_denominator / coordinate.first.denominator),
            coordinate.second.numerator
                * (common_denominator / coordinate.second.denominator)
        });
    }

    // For column HNF [(a,0),(b,c)], c is the gcd of all ordinates and
    // a*c is the gcd of all 2x2 minors.  Bezout reduction also supplies b.
    Wide c = 0;
    Wide b = 0;
    for (const auto& coordinate : integer_coordinates) {
        Wide old_c = c;
        Wide y = coordinate.second;
        Wide old_r = wide_abs(old_c), r = wide_abs(y);
        Wide old_s = 1, s = 0;
        Wide old_t = 0, t = 1;
        while (r != 0) {
            Wide quotient = old_r / r;
            Wide next_r = old_r - quotient * r;
            Wide next_s = old_s - quotient * s;
            Wide next_t = old_t - quotient * t;
            old_r = r; r = next_r;
            old_s = s; s = next_s;
            old_t = t; t = next_t;
        }
        if (old_c < 0) old_s = -old_s;
        if (y < 0) old_t = -old_t;
        c = old_r;
        b = old_s * b + old_t * coordinate.first;
    }
    if (c == 0)
        throw std::runtime_error("translation coordinate module has rank below two");

    Wide determinant_gcd = 0;
    for (size_t i = 0; i < integer_coordinates.size(); ++i) {
        for (size_t j = i + 1; j < integer_coordinates.size(); ++j) {
            Wide minor = integer_coordinates[i].first * integer_coordinates[j].second
                       - integer_coordinates[i].second * integer_coordinates[j].first;
            determinant_gcd = wide_gcd(determinant_gcd, minor);
        }
    }
    if (determinant_gcd == 0 || determinant_gcd % c != 0)
        throw std::runtime_error("failed to compute rank-two translation lattice HNF");
    Wide a = determinant_gcd / c;
    b %= a;
    if (b < 0) b += a;

    for (const auto& coordinate : integer_coordinates) {
        if (coordinate.second % c != 0
            || (coordinate.first - (coordinate.second / c) * b) % a != 0)
            throw std::runtime_error("computed HNF does not contain every translation candidate");
    }

    auto map_coordinate = [&](Wide first, Wide second) {
        Z4Point result;
        for (size_t component = 0; component < result.coordinates.size(); ++component) {
            Wide numerator = first * reference1->coordinates[component]
                           + second * reference2->coordinates[component];
            if (numerator % common_denominator != 0)
                throw std::runtime_error(
                    "translation lattice basis does not map to integral Z4 coordinates");
            result.coordinates[component] = checked_i64(
                numerator / common_denominator, "mapping translation lattice basis to Z4");
        }
        return result;
    };
    Z4Point basis1 = map_coordinate(a, 0);
    Z4Point basis2 = map_coordinate(b, c);
    if (!cartesian_independent(basis1, basis2))
        throw std::runtime_error("mapped translation lattice basis has rank below two");
    for (const Z4Point& candidate : candidates) {
        Quadratic den = determinant(basis1, basis2);
        if (!wide_integral_ratio(determinant(candidate, basis2), den)
            || !wide_integral_ratio(determinant(basis1, candidate), den))
            throw std::runtime_error(
                "mapped translation lattice basis does not contain every candidate");
    }
    normalize_basis(basis1, basis2);
    return {basis1, basis2};
}

std::string ref_error(size_t offset, const std::string& reason) {
    return "invalid Conway string at byte " + std::to_string(offset) + ": " + reason;
}

std::pair<int, int> parse_reference(const std::string& text, size_t base_offset) {
    size_t i = 0;
    if (text.empty() || text[i] < '0' || text[i] > '9')
        throw std::invalid_argument(ref_error(base_offset, "expected edge number"));
    int edge = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        edge = edge * 10 + text[i] - '0';
        ++i;
    }
    int tile = 0;
    if (i < text.size() && text[i] == '@') {
        ++i;
        if (i == text.size() || text[i] < '0' || text[i] > '9')
            throw std::invalid_argument(ref_error(base_offset + i, "expected tile number"));
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            tile = tile * 10 + text[i] - '0';
            ++i;
        }
    } else {
        while (i < text.size() && text[i] == '\'') {
            ++tile;
            ++i;
        }
    }
    if (i != text.size())
        throw std::invalid_argument(ref_error(base_offset + i, "unexpected character in edge reference"));
    return {tile, edge};
}

void validate_face_geometry(const MortierRecord& record) {
    std::set<Z4Point> seeds(record.seeds.begin(), record.seeds.end());
    std::map<Z4Point, std::vector<int>> stars;
    for (const Z4Point& seed : seeds) {
        auto& star = stars[seed];
        for (int direction = 0; direction < 12; ++direction) {
            Z4Point neighbour = reduce_mod_lattice(seed + direction_step(direction),
                                                   record.t1, record.t2);
            if (seeds.count(neighbour)) star.push_back(direction);
        }
        if (star.size() < 2)
            throw std::runtime_error("seed " + point_text(seed)
                                     + " has fewer than two reconstructed neighbours");
    }
    for (const auto& item : stars) {
        const Z4Point& seed = item.first;
        const std::vector<int>& star = item.second;
        for (size_t corner = 0; corner < star.size(); ++corner) {
            int direction = star[corner];
            int next = star[(corner + 1) % star.size()];
            if (next <= direction) next += 12;
            int denominator = 6 - (next - direction);
            if (denominator <= 0 || 12 % denominator != 0)
                throw std::runtime_error("non-regular corner at seed " + point_text(seed));
            int sides = 12 / denominator;
            Z4Point vertex = seed;
            int walk_direction = direction;
            for (int edge = 0; edge < sides; ++edge) {
                vertex = reduce_mod_lattice(vertex + direction_step(walk_direction),
                                            record.t1, record.t2);
                if (!seeds.count(vertex))
                    throw std::runtime_error("face walk leaves reconstructed vertex set at seed "
                                             + point_text(seed));
                walk_direction = mod12(walk_direction + 12 / sides);
            }
            if (vertex != seed)
                throw std::runtime_error("face walk does not close at seed " + point_text(seed));
        }
    }
}

} // namespace

bool operator!=(const Z4Point& left, const Z4Point& right) { return !(left == right); }
Z4Point operator+(const Z4Point& left, const Z4Point& right) {
    Z4Point result;
    for (size_t i = 0; i < 4; ++i)
        result.coordinates[i] = checked_i64(Wide(left.coordinates[i])
                                                + right.coordinates[i],
                                            "adding exact points");
    return result;
}
Z4Point operator-(const Z4Point& left, const Z4Point& right) { return left + (-right); }
Z4Point operator-(const Z4Point& value) {
    Z4Point result;
    for (size_t i = 0; i < 4; ++i)
        result.coordinates[i] = checked_i64(-Wide(value.coordinates[i]),
                                            "negating an exact point");
    return result;
}
Z4Point operator*(int64_t scale, const Z4Point& value) {
    Z4Point result;
    for (size_t i = 0; i < 4; ++i)
        result.coordinates[i] = checked_i64(Wide(scale) * value.coordinates[i],
                                            "scaling an exact point");
    return result;
}

Z4Point direction_step(int direction) {
    static const std::array<Z4Point, 12> steps{{
        z4(1, 0, 0, 0), z4(0, 1, 0, 0), z4(0, 0, 1, 0), z4(0, 0, 0, 1),
        z4(-1, 0, 1, 0), z4(0, -1, 0, 1), z4(-1, 0, 0, 0), z4(0, -1, 0, 0),
        z4(0, 0, -1, 0), z4(0, 0, 0, -1), z4(1, 0, -1, 0), z4(0, 1, 0, -1)
    }};
    return steps[static_cast<size_t>(mod12(direction))];
}

std::pair<long double, long double> cartesian(const Z4Point& point) {
    static const long double root3 = std::sqrt(3.0L);
    return {point.coordinates[0] + root3 * point.coordinates[1] / 2
                + point.coordinates[2] / 2.0L,
            point.coordinates[1] / 2.0L + root3 * point.coordinates[2] / 2
                + point.coordinates[3]};
}

bool cartesian_independent(const Z4Point& first, const Z4Point& second) {
    Quadratic d = determinant(first, second);
    return d.rational != 0 || d.radical != 0;
}

void validate_tiling_description(TilingDescription& description) {
    size_t count = description.polygon_sides.size();
    if (count == 0) throw std::invalid_argument("tiling has no polygon types");
    if (description.repeats.empty()) description.repeats.assign(count, 1);
    if (description.repeats.size() != count)
        throw std::invalid_argument("repeat count does not match polygon type count");
    if (description.physical_to_pattern.empty()) description.physical_to_pattern.resize(count);
    if (description.physical_to_pattern.size() != count)
        throw std::invalid_argument("physical-pattern mapping does not match polygon type count");
    for (size_t tile = 0; tile < count; ++tile) {
        int sides = description.polygon_sides[tile];
        int repeat = description.repeats[tile];
        if (sides < 3 || 12 % sides != 0)
            throw std::invalid_argument("polygon type " + std::to_string(tile)
                + " has " + std::to_string(sides)
                + " sides; exact 12-direction development supports only divisors of 12");
        if (repeat <= 0 || sides % repeat != 0)
            throw std::invalid_argument("invalid repeat for polygon type " + std::to_string(tile));
        auto& mapping = description.physical_to_pattern[tile];
        if (mapping.empty()) {
            int period = sides / repeat;
            for (int edge = 0; edge < sides; ++edge) mapping.push_back(edge % period);
        }
        if (mapping.size() != static_cast<size_t>(sides))
            throw std::invalid_argument("physical-pattern mapping has wrong length for polygon type "
                                        + std::to_string(tile));
        std::set<int> patterns(mapping.begin(), mapping.end());
        if (patterns.empty() || *patterns.begin() != 0
            || *patterns.rbegin() + 1 != static_cast<int>(patterns.size()))
            throw std::invalid_argument("pattern edge indices must be contiguous from zero");
    }
    if (description.adjacency.size() != count)
        throw std::invalid_argument("adjacency does not match polygon type count");
    for (size_t tile = 0; tile < count; ++tile) {
        int patterns = 1 + *std::max_element(description.physical_to_pattern[tile].begin(),
                                             description.physical_to_pattern[tile].end());
        if (description.adjacency[tile].size() != static_cast<size_t>(patterns))
            throw std::invalid_argument("incomplete adjacency for polygon type "
                                        + std::to_string(tile));
        for (int edge = 0; edge < patterns; ++edge) {
            const ConwayAdjacency& a = description.adjacency[tile][edge];
            if (a.tile < 0 || static_cast<size_t>(a.tile) >= count)
                throw std::invalid_argument("adjacency references invalid polygon type");
            if (a.pattern_edge < 0
                || static_cast<size_t>(a.pattern_edge) >= description.adjacency[a.tile].size())
                throw std::invalid_argument("adjacency references invalid pattern edge");
            const ConwayAdjacency& back = description.adjacency[a.tile][a.pattern_edge];
            if (back.tile != static_cast<int>(tile) || back.pattern_edge != edge
                || back.mirrored != a.mirrored)
                throw std::invalid_argument("Conway adjacency is not reciprocal at polygon type "
                                            + std::to_string(tile) + ", pattern "
                                            + std::to_string(edge));
        }
    }
}

std::vector<std::vector<ConwayAdjacency>> parse_conway_adjacency(
    const std::string& conway, const std::vector<std::vector<int>>& mappings) {
    std::vector<std::vector<ConwayAdjacency>> result(mappings.size());
    for (size_t tile = 0; tile < mappings.size(); ++tile) {
        if (mappings[tile].empty())
            throw std::invalid_argument("cannot parse Conway adjacency with an empty mapping");
        int count = 1 + *std::max_element(mappings[tile].begin(), mappings[tile].end());
        result[tile].resize(static_cast<size_t>(count));
    }
    size_t cursor = 0;
    while (cursor < conway.size()) {
        while (cursor < conway.size()
               && (conway[cursor] == ' ' || conway[cursor] == '\t'
                   || conway[cursor] == '\r' || conway[cursor] == '\n')) ++cursor;
        if (cursor == conway.size()) break;
        char open = conway[cursor];
        if (open != '(' && open != '[')
            throw std::invalid_argument(ref_error(cursor, "expected '(' or '['"));
        char close = open == '(' ? ')' : ']';
        size_t end = conway.find(close, cursor + 1);
        if (end == std::string::npos)
            throw std::invalid_argument(ref_error(cursor, "missing closing delimiter"));
        std::istringstream body(conway.substr(cursor + 1, end - cursor - 1));
        std::vector<std::string> refs;
        std::string ref;
        while (body >> ref) refs.push_back(ref);
        if (refs.size() != 1 && refs.size() != 2)
            throw std::invalid_argument(ref_error(cursor, "pair must contain one or two references"));
        auto first = parse_reference(refs[0], cursor + 1);
        auto second = refs.size() == 1 ? first : parse_reference(refs[1], cursor + 1);
        for (const auto& value : {first, second}) {
            if (value.first < 0 || static_cast<size_t>(value.first) >= result.size()
                || value.second < 0
                || static_cast<size_t>(value.second) >= result[value.first].size())
                throw std::invalid_argument(ref_error(cursor, "reference is outside pattern mapping"));
            if (result[value.first][value.second].tile != -1)
                throw std::invalid_argument(ref_error(cursor, "pattern edge is paired more than once"));
        }
        bool mirrored = open == '[';
        result[first.first][first.second] = {second.first, second.second, mirrored};
        if (first != second)
            result[second.first][second.second] = {first.first, first.second, mirrored};
        cursor = end + 1;
    }
    return result;
}

TilingDescription make_tiling_description(std::vector<int> polygon_sides,
                                           std::vector<int> repeats,
                                           const std::string& conway) {
    TilingDescription description;
    description.polygon_sides = std::move(polygon_sides);
    description.repeats = std::move(repeats);
    description.physical_to_pattern.resize(description.polygon_sides.size());
    if (description.repeats.empty())
        description.repeats.assign(description.polygon_sides.size(), 1);
    if (description.repeats.size() != description.polygon_sides.size())
        throw std::invalid_argument("repeat count does not match polygon type count");
    for (size_t tile = 0; tile < description.polygon_sides.size(); ++tile) {
        int sides = description.polygon_sides[tile], repeat = description.repeats[tile];
        if (repeat <= 0 || sides % repeat != 0)
            throw std::invalid_argument("invalid repeat for polygon type " + std::to_string(tile));
        int period = sides / repeat;
        for (int edge = 0; edge < sides; ++edge)
            description.physical_to_pattern[tile].push_back(edge % period);
    }
    description.adjacency = parse_conway_adjacency(conway,
                                                    description.physical_to_pattern);
    validate_tiling_description(description);
    return description;
}

TilingDescription make_tiling_description(
    std::vector<int> polygon_sides,
    std::vector<std::vector<int>> physical_to_pattern,
    std::vector<std::vector<ConwayAdjacency>> adjacency) {
    TilingDescription description;
    description.polygon_sides = std::move(polygon_sides);
    description.repeats.assign(description.polygon_sides.size(), 1);
    description.physical_to_pattern = std::move(physical_to_pattern);
    description.adjacency = std::move(adjacency);
    validate_tiling_description(description);
    return description;
}

Z4Point reduce_mod_lattice(const Z4Point& point, const Z4Point& t1,
                           const Z4Point& t2) {
    Quadratic denominator = determinant(t1, t2);
    if (sign(denominator) == 0)
        throw std::invalid_argument("cannot reduce modulo a rank-one lattice");
    int64_t first = floor_ratio(determinant(point, t2), denominator);
    int64_t second = floor_ratio(determinant(t1, point), denominator);
    return point - first * t1 - second * t2;
}

void normalize_basis(Z4Point& t1, Z4Point& t2) {
    if (!cartesian_independent(t1, t2))
        throw std::invalid_argument("cannot normalize a rank-one lattice basis");
    std::array<std::pair<Z4Point, Z4Point>, 8> alternatives{{
        {t1, t2}, {-t1, t2}, {t1, -t2}, {-t1, -t2},
        {t2, t1}, {-t2, t1}, {t2, -t1}, {-t2, -t1}
    }};
    auto best = *std::min_element(alternatives.begin(), alternatives.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.first, left.second) < std::tie(right.first, right.second);
        });
    t1 = best.first;
    t2 = best.second;
}

DevelopmentResult develop_exact_geometry(TilingDescription description,
                                         const DevelopmentOptions& options) {
    validate_tiling_description(description);
    if (options.max_tiles == 0 || options.max_quotient_tiles == 0)
        throw std::invalid_argument("development limits must be nonzero");

    std::deque<Control> queue{{Control{0, 0, false, Z4Point{}, 0}}};
    std::set<ControlKey> visited;
    std::map<ControlClass, Z4Point> origins;
    std::vector<Z4Point> candidates;
    while (!queue.empty() && visited.size() < options.max_tiles) {
        Control control = queue.front();
        queue.pop_front();
        ControlKey key{control.tile, control.orientation, control.reflected, control.anchor};
        if (!visited.insert(key).second) continue;
        auto inserted = origins.emplace(ControlClass{control.tile, control.orientation,
                                                     control.reflected},
                                         control.anchor);
        if (!inserted.second)
            add_unique_nonzero(candidates, control.anchor - inserted.first->second);
        if (control.depth >= options.max_depth) continue;
        int sides = description.polygon_sides[control.tile];
        std::vector<Z4Point> vertices = polygon_vertices(control, sides);
        for (int edge = 0; edge < sides; ++edge)
            queue.push_back(neighbour_control(description, control, edge, vertices));
    }
    if (visited.size() == options.max_tiles && !queue.empty() && candidates.size() < 2)
        throw std::runtime_error("max_tiles reached before translations were discovered");
    auto basis = choose_basis(candidates);

    // Close the exact labelled tile development on the quotient lattice.
    std::deque<Control> quotient_queue{{Control{0, 0, false, Z4Point{}, 0}}};
    std::set<ControlKey> quotient_controls;
    std::set<Z4Point> seed_set;
    std::map<Z4Point, std::set<int>> expected_stars;
    while (!quotient_queue.empty()) {
        Control control = quotient_queue.front();
        quotient_queue.pop_front();
        control.anchor = reduce_mod_lattice(control.anchor, basis.first, basis.second);
        ControlKey key{control.tile, control.orientation, control.reflected, control.anchor};
        if (!quotient_controls.insert(key).second) continue;
        if (quotient_controls.size() > options.max_quotient_tiles)
            throw std::runtime_error("quotient development exceeded max_quotient_tiles");
        int sides = description.polygon_sides[control.tile];
        std::vector<Z4Point> vertices = polygon_vertices(control, sides);
        for (const Z4Point& vertex : vertices)
            seed_set.insert(reduce_mod_lattice(vertex, basis.first, basis.second));
        for (int edge = 0; edge < sides; ++edge) {
            int direction = mod12(control.orientation
                                  + (control.reflected ? edge : -edge) * (12 / sides));
            Z4Point start = reduce_mod_lattice(vertices[edge], basis.first, basis.second);
            Z4Point end = reduce_mod_lattice(vertices[(edge + 1) % sides],
                                              basis.first, basis.second);
            expected_stars[start].insert(direction);
            expected_stars[end].insert(mod12(direction + 6));
        }
        for (int edge = 0; edge < sides; ++edge)
            quotient_queue.push_back(neighbour_control(description, control, edge, vertices));
    }

    for (const Z4Point& seed : seed_set) {
        std::set<int> reconstructed;
        for (int direction = 0; direction < 12; ++direction) {
            Z4Point adjacent = reduce_mod_lattice(seed + direction_step(direction),
                                                   basis.first, basis.second);
            if (seed_set.count(adjacent)) reconstructed.insert(direction);
        }
        if (reconstructed != expected_stars[seed])
            throw std::runtime_error("reconstructed local star differs from developed polygons at seed "
                                     + point_text(seed));
    }

    MortierRecord record{basis.first, basis.second,
                         std::vector<Z4Point>(seed_set.begin(), seed_set.end())};
    validate_mortier_record(record);
    return {std::move(record), visited.size(), quotient_controls.size(), candidates.size()};
}

void validate_mortier_record(const MortierRecord& record) {
    if (!cartesian_independent(record.t1, record.t2))
        throw std::invalid_argument("Mortier translations have Cartesian rank below two");
    if (record.seeds.empty()) throw std::invalid_argument("Mortier seed set is empty");
    Z4Point normalized1 = record.t1, normalized2 = record.t2;
    normalize_basis(normalized1, normalized2);
    if (normalized1 != record.t1 || normalized2 != record.t2)
        throw std::invalid_argument("Mortier translation basis is not deterministically normalized");
    if (!std::is_sorted(record.seeds.begin(), record.seeds.end()))
        throw std::invalid_argument("Mortier seeds are not sorted");
    for (size_t i = 0; i < record.seeds.size(); ++i) {
        Z4Point reduced = reduce_mod_lattice(record.seeds[i], record.t1, record.t2);
        if (reduced != record.seeds[i])
            throw std::invalid_argument("Mortier seed is outside the normalized fundamental cell");
        if (i && record.seeds[i] == record.seeds[i - 1])
            throw std::invalid_argument("Mortier seed set contains a duplicate residue");
    }
    validate_face_geometry(record);
}

} // namespace mortier_geometry
