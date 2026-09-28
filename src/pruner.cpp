#include "pruner.h"
#include "canonical.h"
#include "disk_solver.h"
#include "solver.h"
#include "vertex_catalog.h"
#include "zstd_stream.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using namespace catalog;

int g_wl_dim = 1;
int g_wl_iters = 0;   // 0 = auto: iterate 1-WL to convergence (cap = n darts)
bool g_use_bfl = false;
bool g_dedup_canon = true;
bool g_no_iso_check = false;
bool g_keep_pruner_inputs = false;
bool g_profile_pruner = false;

// Diagnostic counters (WL-hash collision rate).
std::atomic<int64_t> g_wl_hash_hits{0};
std::atomic<int64_t> g_wl_collisions{0};
std::atomic<int64_t> g_solutions_match_calls{0};
std::atomic<int> g_wl_max_iters{0};
bool g_compress_pruner_outputs = true;  // compress rolling pruner output by default

// Compress eupruned.txt on the go instead of only once at the
// end: these are opened once per combo_code and streamed to for the whole
// run, so left alone they can grow very large before ever getting
// compressed. Neither file is ever read back within this program, so we
// append a new frame onto path.zst and drop the plain tail, just triggered
// periodically on size instead of once at the end. zstd frames concatenate,
// so decompressing path.zst afterward yields the full file regardless of
// how many times it was flushed mid-run.
int64_t g_pruner_compress_threshold = 64LL * 1024 * 1024; // flush every ~64MB of plain text

struct RollingCompressedWriter {
    std::string path;
    std::ofstream out;

    explicit RollingCompressedWriter(std::string p)
        : path(std::move(p)) {
        std::error_code ec;
        fs::remove(path + ".zst", ec);
        out.open(path, std::ios::out | std::ios::trunc);
    }

    ~RollingCompressedWriter() { finish(); }

    RollingCompressedWriter(const RollingCompressedWriter&) = delete;
    RollingCompressedWriter& operator=(const RollingCompressedWriter&) = delete;

    template <typename T>
    RollingCompressedWriter& operator<<(const T& v) { out << v; return *this; }

    // Some helpers (write_cycle_final) want a raw std::ostream&.
    std::ostream& stream() { return out; }

    // Call this periodically (e.g. once per batch, or every few hundred
    // records) — NOT after every line. Stat'ing the file and potentially
    // spawning `zstd` needs to stay rare relative to the writes.
    void maybe_compress() {
        if (!g_compress_pruner_outputs) return;
        out.flush();
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec || (int64_t)sz < g_pruner_compress_threshold) return;
        out.close();
        compress_to_zst(path);                       // path -> path.zst (appended), path removed
        out.open(path, std::ios::out | std::ios::trunc);
    }

    // Flush whatever's left into the .zst. Safe to call more than once.
    void finish() {
        if (!out.is_open() && !fs::exists(path)) return;
        if (out.is_open()) out.close();
        if (!g_compress_pruner_outputs) return;
        std::error_code ec;
        if (fs::exists(path) && fs::file_size(path, ec) > 0)
            compress_to_zst(path);
        else
            fs::remove(path, ec);   // drop empty leftover rather than leaving a stray .zst frame
    }
};

// =============================================================================
// Helpers  (mirror those in solver.cpp)
// =============================================================================
static std::string edge_label(const std::string& tmpl, int tile) {
    std::string out = tmpl;
    if (tile > 3) { out += "@" + std::to_string(tile); }
    else { for (int i = 0; i < tile; ++i) out += "'"; }
    return out;
}

static char count_digit(int x) {
    if (x == 10) return 'a';
    if (x == 11) return 'b';
    return (char)('0' + x);
}

static std::string make_count_signature(const State& st) {
    std::map<std::string, int> base_counts;
    for (int t : st.vertype) {
        const std::string& sym = symbols[t];
        ++base_counts[sym.substr(0, sym.find(')') + 1)];
    }

    std::string result = std::to_string(base_counts.size());
    std::vector<int> multiplicities;
    for (const auto& [symbol, count] : base_counts) multiplicities.push_back(count);
    std::sort(multiplicities.begin(), multiplicities.end(), std::greater<int>());
    if (std::any_of(multiplicities.begin(), multiplicities.end(), [](int n) { return n > 1; })) {
        result += " (";
        for (int n : multiplicities) result += count_digit(n);
        result += ")";
    }
    return result;
}

static void materialize_output_fields(SolutionPruner::SolutionRecord& rec) {
    if (rec.vertex_line.empty()) {
        rec.vertex_line = verbal_vertices(rec.state.vertype);
        rec.signature_line = signature(rec.state.vertype);
        rec.conway_line = write_conway(rec.state);
        rec.tes_line = "eu raw " + file_signature(rec.state.vertype) + " "
                     + std::to_string(rec.solution_index) + ".tes";
    }
    rec.count_signature = make_count_signature(rec.state);
}

// =============================================================================
// Conway parsing  (ConwayParser)
// =============================================================================
static int find_closing_index(const std::string& conway) {
    for (size_t i = 0; i < conway.size(); ++i)
        if (conway[i] == ')' || conway[i] == ']')
            return (int)i;
    return -1;
}

static std::pair<std::string, std::string> decipher_symbol(const std::string& symbol) {
    bool mirror = (symbol[0] == '[');
    int i = 1;
    std::string first;
    while (symbol[i] != ' ' && symbol[i] != ')' && symbol[i] != ']')
        first += symbol[i++];
    std::string second;
    if (symbol[i] == ' ') {
        ++i;
        while (symbol[i] != ')' && symbol[i] != ']')
            second += symbol[i++];
    } else {
        second = first;
    }
    if (mirror) second = "*" + second;
    return {first, second};
}

static std::tuple<bool, int, int> decipher_edge(const std::string& text) {
    std::string s = text + " ";
    int i = 0;
    bool is_mirror = false;
    int number = 0;
    int tile = 0;

    if (s[i] == '*') { is_mirror = true; ++i; }
    while (s[i] >= '0' && s[i] <= '9') {
        number = number * 10 + (s[i] - '0');
        ++i;
    }
    while (s[i] == '\'') { ++tile; ++i; }
    if (s[i] == '@') {
        ++i;
        while (s[i] >= '0' && s[i] <= '9') {
            tile = tile * 10 + (s[i] - '0');
            ++i;
        }
    }
    return {is_mirror, number, tile};
}

// =============================================================================
// Cycle final writer  (ConwayCycleWriter.write_cycle_final)
// =============================================================================
CanonicalTilingOutput build_canonical_tiling_output(const State& st) {
    CanonicalTilingOutput output;
    int n = (int)st.darts.size();
    std::vector<int> seen(n, 0);
    std::vector<std::string> mainst_list;
    std::vector<int> sublist, repeat_list;
    bool ultra_chiral = true;

    auto lstr = [&](int i) { return rebuild_label(i, st.vertype); };

    for (int cy = 0; cy < n; ++cy) {
        if (seen[cy]) continue;

        int left = cy, right = st.darts[left].rneig;
        int v = st.darts[right].polygon_size;
        int cnt = 0, min_mir = n;
        std::string main_st;

        while (true) {
            seen[left] = 1;
            if (st.darts[right].mirro < min_mir) min_mir = st.darts[right].mirro;
            main_st += lstr(left) + "/" + lstr(right)
                    + "(" + std::to_string(st.darts[right].polygon_size) + ")-";
            ++cnt;
            left = st.darts[right].glue;
            if (left == cy) break;
            right = st.darts[left].rneig;
        }
        main_st.pop_back();
        int ratio = v / cnt;
        repeat_list.push_back(ratio);
        if (ratio != 1) main_st = "[" + main_st + "]x" + std::to_string(ratio);
        mainst_list.push_back(main_st);

        if (seen[min_mir]) {
            sublist.push_back(0);
            ultra_chiral = false;
        } else {
            int l2 = min_mir, r2 = st.darts[l2].rneig;
            std::string main_st2;
            while (true) {
                seen[l2] = 1;
                main_st2 += lstr(l2) + "/" + lstr(r2)
                         + "(" + std::to_string(st.darts[r2].polygon_size) + ")-";
                ++cnt;
                l2 = st.darts[r2].glue;
                if (l2 == min_mir) break;
                r2 = st.darts[l2].rneig;
            }
            main_st2.pop_back();
            repeat_list.push_back(ratio);
            if (ratio != 1) main_st2 = "[" + main_st2 + "]x" + std::to_string(ratio);
            mainst_list.push_back(main_st2);
            sublist.push_back(1);
            sublist.push_back(2);
        }
    }

    std::string subheader;
    for (int m = 0; m < (int)mainst_list.size(); ++m) {
        int sub = sublist[m];
        if (sub == 0) {
            output.cycle_lines.push_back(std::to_string(m) + ": " + mainst_list[m]);
        } else if (sub == 1) {
            std::string hdr = ultra_chiral ? std::to_string(m/2) + ": "
                                           : std::to_string(m) + "/" + std::to_string(m+1) + ": ";
            subheader = std::string(hdr.size(), ' ');
            output.cycle_lines.push_back(hdr + mainst_list[m]);
        } else {
            output.cycle_lines.push_back(subheader + mainst_list[m]);
        }
    }

    // Build assembled Conway string
    bool is_chiral = ultra_chiral;
    std::vector<std::string> work_list, left_edges, right_edges, edges;
    std::vector<int> work_reps;
    if (is_chiral) {
        for (int k = 0; k < (int)mainst_list.size() / 2; ++k) {
            work_list.push_back(mainst_list[2*k]);
            work_reps.push_back(repeat_list[2*k]);
        }
    } else {
        work_list = mainst_list;
        work_reps = repeat_list;
    }

    std::vector<int> poly_size_list;
    for (int m = 0; m < (int)work_list.size(); ++m) {
        std::string s = work_list[m];
        int rep = work_reps[m];
        if (rep > 1) s = s.substr(1, s.find(']') - 1);
        size_t p1 = s.find('('), p2 = s.find(')');
        poly_size_list.push_back(std::stoi(s.substr(p1+1, p2-p1-1)));
        s += "-";
        int rev = (int)s.size() - 1;
        while (s[rev] != '/') --rev;
        s = s.substr(rev+1) + s.substr(0, rev+1);
        int ei = 0;
        while (!s.empty()) {
            size_t ind = s.find('/');
            std::string chunk = s.substr(0, ind+1);
            s = s.substr(ind+1);
            size_t lp = chunk.find('('), mi = chunk.find('-');
            left_edges.push_back(chunk.substr(0, lp));
            right_edges.push_back(chunk.substr(mi+1, chunk.size()-mi-2));
            std::string el = std::to_string(ei);
            if (m > 3) { el += "@" + std::to_string(m); }
            else { for (int t = 0; t < m; ++t) el += "'"; }
            edges.push_back(el);
            ++ei;
        }
    }

    std::string conway_str;
    while (!left_edges.empty()) {
        if (is_chiral) {
            auto it_rt = std::find(right_edges.begin(), right_edges.end(), left_edges[0]);
            if (it_rt == right_edges.end()) {
                std::string mm = (left_edges[0][0] == '*')
                    ? left_edges[0].substr(1) : "*" + left_edges[0];
                auto it_m = std::find(left_edges.begin(), left_edges.end(), mm);
                if (it_m == left_edges.end()) {
                    conway_str += "[" + edges[0] + "]";
                } else {
                    int mi = (int)(it_m - left_edges.begin());
                    conway_str += "[" + edges[0] + " " + edges[mi] + "]";
                    left_edges.erase(left_edges.begin() + mi);
                    right_edges.erase(right_edges.begin() + mi);
                    edges.erase(edges.begin() + mi);
                }
                left_edges.erase(left_edges.begin());
                right_edges.erase(right_edges.begin());
                edges.erase(edges.begin());
                continue;
            }
        }
        auto it_rt = std::find(right_edges.begin(), right_edges.end(), left_edges[0]);
        if (it_rt == right_edges.end() || it_rt == right_edges.begin()) {
            conway_str += "(" + edges[0] + ")";
            left_edges.erase(left_edges.begin());
            right_edges.erase(right_edges.begin());
            edges.erase(edges.begin());
        } else {
            int mi = (int)(it_rt - right_edges.begin());
            conway_str += "(" + edges[0] + " " + edges[mi] + ")";
            left_edges.erase(left_edges.begin() + mi);
            right_edges.erase(right_edges.begin() + mi);
            edges.erase(edges.begin() + mi);
            left_edges.erase(left_edges.begin());
            right_edges.erase(right_edges.begin());
            edges.erase(edges.begin());
        }
    }
    output.conway = conway_str;
    output.geometry = mortier_geometry::make_tiling_description(
        poly_size_list, work_reps, conway_str);
    return output;
}

std::string tes_document(const CanonicalTilingOutput& output, const std::string& solution_label) {
    std::ostringstream tes;
    tes << "## Euclidean, " << solution_label << "\n";
    tes << "e2.\n";
    tes << "angleunit(deg)\n";
    for (int sz : output.geometry.polygon_sides) {
        int angle = 180 - 360 / sz;
        std::string angles;
        for (int k = 0; k < sz; ++k) {
            if (k) angles += ",";
            angles += std::to_string(angle);
        }
        tes << "unittile(" << angles << ")\n";
    }
    tes << "conway(\"" << output.conway << "\")\n";
    for (size_t i = 0; i < output.geometry.repeats.size(); ++i)
        if (output.geometry.repeats[i] > 1)
            tes << "repeat(" << i << "," << output.geometry.repeats[i] << ")\n";
    return tes.str();
}

void write_cycle_final(const State& st, std::ostream& out,
                        TesStore* tes_store, const std::string& combo,
                        const std::string& tes_filename,
                        const std::string& solution_label) {
    CanonicalTilingOutput output = build_canonical_tiling_output(st);
    for (const std::string& line : output.cycle_lines) out << line << "\n";
    out << "---\n" << output.conway << "\n";
    if (!tes_store) return;
    tes_store->add(combo, solution_label, tes_filename, tes_document(output, solution_label));
}

// =============================================================================
// SolutionPruner implementation
// =============================================================================
SolutionPruner::SolutionPruner(const std::string& output_dir)
    : output_dir_(output_dir) {}

// ---------------------------------------------------------------------------
// make_glue — recreates glue[] from a Conway symbol string
// ---------------------------------------------------------------------------
std::vector<int> SolutionPruner::make_glue(const std::string& conway,
                                            const std::vector<Dart>& darts,
                                            const std::vector<int>& vertype) {
    int n = (int)darts.size();
    std::vector<std::string> label(n);
    int offset = 0;
    for (size_t tile = 0; tile < vertype.size(); ++tile) {
        int vt = vertype[tile];
        int sl = (int)left_neighbors[vt].size();
        for (int sg = 0; sg < sl; ++sg)
            label[offset + sg] = edge_label(edge_label_templates[vt][sg], (int)tile);
        offset += sl;
    }

    std::vector<int> glue(darts.size(), -1);
    std::string remaining = conway;
    while (remaining.size() > 1) {
        int end = find_closing_index(remaining);
        std::string symbol = remaining.substr(0, (size_t)end + 1);
        remaining = remaining.substr((size_t)end + 1);
        auto [first_text, second_text] = decipher_symbol(symbol);

        std::vector<int> indices;
        for (const auto& text : {first_text, second_text}) {
            auto [is_mirror, number, tile] = decipher_edge(text);
            std::string slot_label = (is_mirror ? "*" : "") + edge_label(std::to_string(number), tile);
            auto it = std::find(label.begin(), label.end(), slot_label);
            if (it == label.end()) return glue; // label not found — corrupted input
            indices.push_back((int)(it - label.begin()));
        }
        int a = indices[0], b = indices[1];
        if (a < 0 || a >= (int)glue.size() || b < 0 || b >= (int)glue.size()) continue;
        glue[a] = b;
        glue[b] = a;
        glue[darts[a].mirro] = darts[b].mirro;
        glue[darts[b].mirro] = darts[a].mirro;
    }
    return glue;
}

// ---------------------------------------------------------------------------
// decode_solution
// ---------------------------------------------------------------------------
State SolutionPruner::decode_solution(const std::string& vertex_line,
                                       const std::string& conway_line) {
    // Parse vertex types from the comma-separated line
    std::vector<int> vertype;
    {
        std::string s = vertex_line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        size_t pos = 0;
        while (pos < s.size()) {
            size_t next = s.find(", ", pos);
            std::string tok = s.substr(pos, (next == std::string::npos ? s.size() : next) - pos);
            auto it = std::find(symbols.begin(), symbols.end(), tok);
            vertype.push_back((int)(it - symbols.begin()));
            if (next == std::string::npos) break;
            pos = next + 2;
        }
    }

    // Build state arrays
    State st;
    for (size_t tile = 0; tile < vertype.size(); ++tile) {
        int vt = vertype[tile];
        int offset = (int)st.darts.size();
        int sl = (int)left_neighbors[vt].size();
        for (int sg = 0; sg < sl; ++sg) {
            Dart d;
            d.rneig = offset + right_neighbors[vt][sg];
            d.lneig = offset + left_neighbors[vt][sg];
            d.mirro = offset + mirrors[vt][sg];
            d.polygon_size = polygon_sizes[vt][sg];
            d.glue = -1;
            d.is_mirror_edge = edge_label_templates[vt][sg][0] == '*';
            st.darts.push_back(d);
        }
    }
    st.vertype = vertype;

    std::string cw = conway_line;
    while (!cw.empty() && (cw.back() == '\n' || cw.back() == '\r'))
        cw.pop_back();
    std::vector<int> glue = make_glue(cw, st.darts, st.vertype);
    // Validate: all glue entries must be valid for complete solutions
    for (int g : glue) if (g < 0 || g >= (int)glue.size()) {
        // Corrupt state — return empty state so caller can skip
        st.darts.clear(); return st;
    }
    for (int i = 0; i < (int)glue.size(); ++i) st.darts[i].glue = glue[i];
    return st;
}

// ---------------------------------------------------------------------------
// read_next_solution — streaming parser, reads one solution from file
// Returns true if a solution was read and false only on clean EOF.
// ---------------------------------------------------------------------------
bool read_next_solution(std::istream& in, SolutionPruner::SolutionRecord& rec) {
    std::string line;
    bool saw_nonempty = false;

    // Find next "Number of polygons:" line
    while (std::getline(in, line)) {
        if (line.rfind("Number of polygons:", 0) == 0) break;
        if (!line.empty()) saw_nonempty = true;
    }
    if (in.bad()) throw std::runtime_error("failed while reading text solution stream");
    if (in.eof()) {
        if (saw_nonempty) throw std::runtime_error("unexpected trailing text in solution stream");
        return false;
    }
    if (in.fail()) throw std::runtime_error("failed while scanning text solution stream");

    // Read the 4 fixed header lines
    if (!std::getline(in, rec.vertex_line)
            || !std::getline(in, rec.signature_line)
            || !std::getline(in, rec.tes_line)
            || !std::getline(in, rec.conway_line))
        throw std::runtime_error("truncated text solution header");

    // Skip cycle description lines until "---"
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '-' && line == "---") break;
    }
    if (line != "---") throw std::runtime_error("truncated text solution cycles");

    // Skip assembled conway and blank line
    if (!std::getline(in, line)) throw std::runtime_error("missing assembled Conway symbol");
    if (!std::getline(in, line) && !in.eof())
        throw std::runtime_error("failed after text solution record");

    rec.state = SolutionPruner::decode_solution(rec.vertex_line, rec.conway_line);

    return true;
}

// ---------------------------------------------------------------------------
// read_next_solution_bin — reads one PackedState from a binary stream,
// unpacks to State, and computes text fields for the pruner.
// ---------------------------------------------------------------------------
bool read_next_solution_bin(std::istream& in, SolutionPruner::SolutionRecord& rec,
                             int& sol_idx) {
    if (in.peek() == EOF) {
        if (in.bad()) throw std::runtime_error("failed while reading binary solution stream");
        return false;
    }

    State st = read_state_bin(in);
    for (const Dart& d : st.darts)
        if (d.glue < 0)
            throw std::runtime_error("binary solution contains an unglued dart");

    rec.state = std::move(st);
    rec.solution_index = ++sol_idx;

    return true;
}

// ---------------------------------------------------------------------------
// is_canonical_labeling — bit-set alias refinement (O(1) membership)
// ---------------------------------------------------------------------------
using BS4 = std::array<uint64_t, 4>;
struct AliasRows {
    int n;
    int nw;
    std::vector<BS4>& small;
    std::vector<uint64_t>& large;

    uint64_t* operator[](int i) {
        return n <= 256 ? small[i].data() : large.data() + (size_t)i * nw;
    }
    const uint64_t* operator[](int i) const {
        return n <= 256 ? small[i].data() : large.data() + (size_t)i * nw;
    }
};

static AliasRows alias_rows(int n, int nw, std::vector<BS4>& small,
                            std::vector<uint64_t>& large) {
    if (n <= 256) {
        if ((int)small.size() < n) small.resize(n);
    } else {
        large.resize((size_t)n * nw);
    }
    return {n, nw, small, large};
}

static inline bool alias_get(const AliasRows& alias, int row, int i) {
    return row >= 0 && row < alias.n && i >= 0 && i < alias.n
        && ((alias[row][i >> 6] >> (i & 63)) & 1);
}
static inline bool alias_singleton(const AliasRows& alias, int row) {
    int c = 0;
    for (int w = 0; w < alias.nw; ++w) {
        c += __builtin_popcountll(alias[row][w]);
        if (c > 1) return false;
    }
    return c == 1;
}

std::pair<bool, std::string> SolutionPruner::is_canonical_labeling(const State& st) {
    int n = (int)st.darts.size();
    int nw = (n + 63) / 64;

    static thread_local std::vector<BS4> alias_small;
    static thread_local std::vector<uint64_t> alias_large;
    AliasRows alias = alias_rows(n, nw, alias_small, alias_large);

    // Initialise: all candidates {0..n-1}
    for (int i = 0; i < n; ++i) {
        for (int w = 0; w < nw; ++w) alias[i][w] = ~0ULL;
        int rem = n & 63;
        if (rem) alias[i][nw-1] &= (1ULL << rem) - 1;
    }

    std::vector<bool> unique(n, false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            for (int w = 0; w < nw; ++w) {
                uint64_t bits = alias[i][w];
                while (bits) {
                    int bit = __builtin_ctzll(bits); bits &= bits - 1;
                    int j = w * 64 + bit; if (j == i) continue;
                    if (st.darts[i].polygon_size != st.darts[j].polygon_size
                        || !alias_get(alias, j, i)
                        || !alias_get(alias, st.darts[i].mirro, st.darts[j].mirro)
                        || !alias_get(alias, st.darts[i].glue, st.darts[j].glue)
                        || !alias_get(alias, st.darts[i].rneig, st.darts[j].rneig)
                        || !alias_get(alias, st.darts[i].lneig, st.darts[j].lneig))
                    {
                        alias[i][w] &= ~(1ULL << bit);
                        changed = true;
                    }
                }
            }
            if (alias_singleton(alias, i))
                unique[i] = true;
        }
    }

    bool all_unique = true;
    for (int i = 0; i < n; ++i)
        if (!unique[i]) { all_unique = false; break; }

    if (all_unique) return {true, ""};

    // The caller only needs the decision. Building labels for every rejected
    // solution was a significant allocation-heavy hot path.
    return {false, ""};
}

// ---------------------------------------------------------------------------
// solutions_match — mirrors Python _solutions_match
// ---------------------------------------------------------------------------
std::pair<bool, std::string> SolutionPruner::solutions_match(const State& a, const State& b) {
    int n = (int)a.darts.size();
    if ((int)b.darts.size() != n) return {false, ""};
    if (n == 0) return {true, ""};

    static thread_local std::vector<int> map_ab, map_ba, queue;
    map_ab.resize(n);
    map_ba.resize(n);
    queue.resize(n);

    auto assign = [&](int ai, int bi, int& tail) {
        if (map_ab[ai] != -1 || map_ba[bi] != -1)
            return map_ab[ai] == bi && map_ba[bi] == ai;
        if (a.darts[ai].polygon_size != b.darts[bi].polygon_size) return false;
        map_ab[ai] = bi;
        map_ba[bi] = ai;
        queue[tail++] = ai;
        return true;
    };

    for (int root_b = 0; root_b < n; ++root_b) {
        if (a.darts[0].polygon_size != b.darts[root_b].polygon_size) continue;
        std::fill(map_ab.begin(), map_ab.end(), -1);
        std::fill(map_ba.begin(), map_ba.end(), -1);
        int head = 0, tail = 0;
        if (!assign(0, root_b, tail)) continue;

        bool valid = true;
        while (head < tail && valid) {
            int ai = queue[head++];
            int bi = map_ab[ai];
            const Dart& ad = a.darts[ai];
            const Dart& bd = b.darts[bi];
            valid = assign(ad.rneig, bd.rneig, tail)
                 && assign(ad.lneig, bd.lneig, tail)
                 && assign(ad.mirro, bd.mirro, tail)
                 && assign(ad.glue, bd.glue, tail);
        }
        if (valid && tail == n) return {true, ""};
    }
    return {false, ""};
}

// ---------------------------------------------------------------------------
// OnlineDedup — per-worker online dedup with total cap + sliding-window reset.
// ---------------------------------------------------------------------------
bool OnlineDedup::check_and_remember(const State& st) {
    if (g_use_bfl) {
        auto h = bfl_canonical_hash128(st);
        bool all_zero = true;
				//std::cout << "  Online Pruner cap: " << cap_ << " stored" << stored_ << "length: " << seen_bfl;
        for (uint64_t x : h) if (x != 0) { all_zero = false; break; }
        if (all_zero) return false;              // degenerate — don't dedup
        if (stored_ >= cap_) { seen_bfl_.clear(); stored_ = 0; }
        if (seen_bfl_.count(h)) return true;     // dup (hash collision ~impossible)
        seen_bfl_.insert(h);
        ++stored_;
        return false;
    }

    std::string h = wl_hash_dispatch(st);
    if (stored_ >= cap_) { seen_wl_.clear(); stored_ = 0; }
    auto it = seen_wl_.find(h);
    if (it != seen_wl_.end()) {
        if (g_no_iso_check) return true;   // trust the WL hash, skip isomorphism fallback
        for (const auto& kept : it->second) {
            State k = EuclideanSolver::unpack_state(kept);
            if (SolutionPruner::solutions_match(st, k).first) return true;
        }
    }
    seen_wl_[h].push_back(EuclideanSolver::pack_state(st));
    ++stored_;
    return false;
}

// =============================================================================
// WL graph hash  (WLHasher.hash)
// =============================================================================

static inline uint64_t fnv64(uint64_t h, uint64_t x) {
    return (h ^ x) * 1099511628211ULL;
}

static int count_distinct(const std::vector<uint64_t>& v) {
    if (v.empty()) return 0;
    std::vector<uint64_t> s = v;
    std::sort(s.begin(), s.end());
    int c = 1;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] != s[i - 1]) ++c;
    return c;
}

std::string wl_hash(const State& st, int iterations) {
    int n = (int)st.darts.size();
    if (n == 0) return "";

    // Verify glue indices are valid
    for (int i = 0; i < n; ++i) {
        if (st.darts[i].glue < 0 || st.darts[i].glue >= n) return "";
        if (st.darts[i].rneig < 0 || st.darts[i].rneig >= n) return "";
        if (st.darts[i].lneig < 0 || st.darts[i].lneig >= n) return "";
        if (st.darts[i].mirro < 0 || st.darts[i].mirro >= n) return "";
    }

    std::vector<uint64_t> labels(n);
    for (int i = 0; i < n; ++i)
        labels[i] = fnv64(14695981039346656037ULL, (uint64_t)(uint32_t)st.darts[i].polygon_size);

    // 1-WL colour refinement converges once the number of distinct colours
    // stops growing (each round strictly refines the partition otherwise),
    // which happens in at most n rounds.  Iterate to convergence so the hash
    // is as discriminating as 1-WL can be — enough iterations eliminates
    // hash collisions at large k (see collision-rate diagnostics).
    int cap = iterations > 0 ? iterations : (g_wl_iters > 0 ? g_wl_iters : n);
    if (cap > n) cap = n;

    std::vector<uint64_t> new_labels(n);
    int prev_distinct = count_distinct(labels);
    int used = 0;
    for (int iter = 0; iter < cap; ++iter) {
        for (int i = 0; i < n; ++i) {
            uint64_t typed[4] = {
                fnv64(0x0000000000000001ULL, labels[st.darts[i].rneig]),
                fnv64(0x0001000000000001ULL, labels[st.darts[i].lneig]),
                fnv64(0x0002000000000001ULL, labels[st.darts[i].mirro]),
                fnv64(0x0003000000000001ULL, labels[st.darts[i].glue]),
            };
            std::sort(typed, typed + 4);
            uint64_t h = labels[i];
            for (uint64_t t : typed) h = fnv64(h, t);
            new_labels[i] = h;
        }
        std::swap(labels, new_labels);
        ++used;
        int distinct = count_distinct(labels);
        if (distinct == prev_distinct) break;   // converged
        prev_distinct = distinct;
    }
    int u = used;
    int cur = g_wl_max_iters.load(std::memory_order_relaxed);
    while (u > cur && !g_wl_max_iters.compare_exchange_weak(cur, u, std::memory_order_relaxed))
        ;

    // Canonical 32-byte hash of sorted multiset
    std::sort(labels.begin(), labels.end());
    uint64_t h[4] = {
        fnv64(0x9ae16a3b2f90404fULL, 0),
        fnv64(0x9ae16a3b2f90404fULL, 1),
        fnv64(0x9ae16a3b2f90404fULL, 2),
        fnv64(0x9ae16a3b2f90404fULL, 3),
    };
    for (int i = 0; i < n; ++i) {
        h[i & 3] = fnv64(h[i & 3], labels[i]);
        h[(i+1) & 3] = fnv64(h[(i+1) & 3], labels[i] ^ 0xFF);
    }
    std::string out(32, '\0');
    for (int i = 0; i < 4; ++i)
        std::memcpy(&out[i * sizeof(uint64_t)], &h[i], sizeof(uint64_t));
    return out;
}

// WL hash for partial states — handles glue[i] == -1 using sentinel neighbour.
std::string wl_hash_partial(const State& st, int iterations) {
    int n = (int)st.darts.size();
    if (n == 0) return "";
    for (int i = 0; i < n; ++i) {
        if (st.darts[i].rneig < 0 || st.darts[i].rneig >= n) return "";
        if (st.darts[i].lneig < 0 || st.darts[i].lneig >= n) return "";
        if (st.darts[i].mirro < 0 || st.darts[i].mirro >= n) return "";
    }
    if (iterations <= 0) iterations = (g_wl_iters > 0 ? g_wl_iters : n);

    std::vector<uint64_t> labels(n);
    for (int i = 0; i < n; ++i)
        labels[i] = fnv64(14695981039346656037ULL, (uint64_t)(uint32_t)st.darts[i].polygon_size);

    std::vector<uint64_t> new_labels(n);
    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < n; ++i) {
            uint64_t typed[4] = {
                fnv64(0x0000000000000001ULL, labels[st.darts[i].rneig]),
                fnv64(0x0001000000000001ULL, labels[st.darts[i].lneig]),
                fnv64(0x0002000000000001ULL, labels[st.darts[i].mirro]),
                st.darts[i].glue != -1
                    ? fnv64(0x0003000000000001ULL, labels[st.darts[i].glue])
                    : 0xDEADBEEF00003001ULL,
            };
            std::sort(typed, typed + 4);
            uint64_t h = labels[i];
            for (uint64_t t : typed) h = fnv64(h, t);
            new_labels[i] = h;
        }
        std::swap(labels, new_labels);
    }

    std::sort(labels.begin(), labels.end());
    uint64_t h[4] = {
        fnv64(0x9ae16a3b2f90404fULL, 0),
        fnv64(0x9ae16a3b2f90404fULL, 1),
        fnv64(0x9ae16a3b2f90404fULL, 2),
        fnv64(0x9ae16a3b2f90404fULL, 3),
    };
    for (int i = 0; i < n; ++i) {
        h[i & 3] = fnv64(h[i & 3], labels[i]);
        h[(i+1) & 3] = fnv64(h[(i+1) & 3], labels[i] ^ 0xFF);
    }
    std::string out(32, '\0');
    for (int i = 0; i < 4; ++i)
        std::memcpy(&out[i * sizeof(uint64_t)], &h[i], sizeof(uint64_t));
    return out;
}

// =============================================================================
// 2-WL graph hash — operates on n² ordered slot pairs.
// Correct 2-WL: neighbourhood of (i,j) = multiset{(color(i,k), color(k,j)) ∀k}.
// O(n³) per iteration.  Significantly more discriminating than 1-WL for
// tilings with uniform polygon sizes.
// =============================================================================
std::string wl_hash_2(const State& st, int iterations) {
    int n = (int)st.darts.size();
    if (n == 0) return "";
    for (int i = 0; i < n; ++i) {
        if (st.darts[i].glue < 0 || st.darts[i].glue >= n) return "";
        if (st.darts[i].rneig < 0 || st.darts[i].rneig >= n) return "";
        if (st.darts[i].lneig < 0 || st.darts[i].lneig >= n) return "";
        if (st.darts[i].mirro < 0 || st.darts[i].mirro >= n) return "";
    }
    if (iterations <= 0) iterations = 25;   // 2-WL is O(n^3)/iter: keep a fixed default

    std::vector<uint64_t> colours(n * n);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            uint64_t h;
            if (i == j)      h = fnv64(14695981039346656037ULL, 0xAA00000000000001ULL);
            else if (j == st.darts[i].rneig) h = fnv64(14695981039346656037ULL, 0xBB10000000000001ULL);
            else if (j == st.darts[i].lneig) h = fnv64(14695981039346656037ULL, 0xBB20000000000001ULL);
            else if (j == st.darts[i].mirro) h = fnv64(14695981039346656037ULL, 0xBB30000000000001ULL);
            else if (j == st.darts[i].glue)  h = fnv64(14695981039346656037ULL, 0xBB40000000000001ULL);
            else { h  = fnv64(14695981039346656037ULL, (uint64_t)(uint32_t)st.darts[i].polygon_size);
                   h  = fnv64(h, (uint64_t)(uint32_t)st.darts[j].polygon_size); }
            colours[i * n + j] = h;
        }
    }

    std::vector<uint64_t> next(n * n);
    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                // Multiset of pairs: {(colour(i,k), colour(k,j)) ∀ k}
                // Accumulate with commutative XOR + k-seeded hashes
                uint64_t sum = 0;
                for (int k = 0; k < n; ++k) {
                    sum += fnv64(colours[i * n + k], colours[k * n + j]);
                }
                uint64_t h = colours[i * n + j];
                h = fnv64(h, sum);
                next[i * n + j] = h;
            }
        }
        std::swap(colours, next);
    }

    // Canonical 32-byte hash of sorted multiset
    std::sort(colours.begin(), colours.end());
    uint64_t h[4] = {
        fnv64(0x9ae16a3b2f90404fULL, 0),
        fnv64(0x9ae16a3b2f90404fULL, 1),
        fnv64(0x9ae16a3b2f90404fULL, 2),
        fnv64(0x9ae16a3b2f90404fULL, 3),
    };
    int nn = n * n;
    for (int i = 0; i < nn; ++i) {
        h[i & 3] = fnv64(h[i & 3], colours[i]);
        h[(i+1) & 3] = fnv64(h[(i+1) & 3], colours[i] ^ 0xFF);
    }
    std::string out(32, '\0');
    for (int i = 0; i < 4; ++i)
        std::memcpy(&out[i * sizeof(uint64_t)], &h[i], sizeof(uint64_t));
    return out;
}

// DiskWordIndex — exact disk-backed canonical-word set
// =============================================================================
static uint64_t word_hash(const std::vector<int>& w) {
    uint64_t h = 14695981039346656037ULL;
    for (int x : w) { h ^= (uint64_t)(uint32_t)x; h *= 1099511628211ULL; }
    return h;
}

DiskWordIndex::DiskWordIndex(const std::string& path) : path_(path) {
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
}

void DiskWordIndex::insert(const std::vector<int>& w) {
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    int32_t len = (int32_t)w.size();
    f.write((const char*)&len, 4);
    for (int x : w) { int32_t v = (int32_t)x; f.write((const char*)&v, 4); }
    f.close();
    index_[word_hash(w)].push_back(next_offset_);
    next_offset_ += 4 + (uint64_t)w.size() * 4;
}

bool DiskWordIndex::contains(const std::vector<int>& w) {
    auto it = index_.find(word_hash(w));
    if (it == index_.end()) return false;
    std::ifstream f(path_, std::ios::binary);
    for (uint64_t off : it->second) {
        f.seekg((std::streamoff)off);
        int32_t len; f.read((char*)&len, 4);
        if (f.fail() || len != (int32_t)w.size()) continue;
        bool match = true;
        for (int i = 0; i < len; ++i) {
            int32_t v; f.read((char*)&v, 4);
            if (f.fail() || v != w[i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// =============================================================================
// WLPruner
// =============================================================================
WLPruner::WLPruner(const std::string& output_dir, int num_workers,
                   FinalOutputFormat format)
    : SolutionPruner(output_dir), solutions_words_(output_dir + "/words.bin"),
      num_workers_(num_workers), format_(format) {
    if (format_ == FinalOutputFormat::Tes)
        tes_store_ = std::make_unique<TesStore>(output_dir + "/tilings.sqlite3.tmp");
    else if (format_ == FinalOutputFormat::Mortier)
        mortier_store_ = std::make_unique<MortierStore>(output_dir + "/tilings.sqlite3.tmp");
}

static std::string combo_of_input(const std::string& path) {
    std::string combo_code = fs::path(path).filename().string();
    if (combo_code.find("eusolver_") == 0) combo_code = combo_code.substr(9);
    if (has_zst_suffix(combo_code)) combo_code = combo_code.substr(0, combo_code.size() - 4);
    size_t dot = combo_code.rfind('.');
    if (dot != std::string::npos) combo_code = combo_code.substr(0, dot);
    return combo_code;
}

bool WLPruner::HashSet128::insert(std::array<uint64_t, 2> h) {
    if (h[0] == 0 && h[1] == 0) h[1] = 1;          // reserve all-zero as "empty"
    if ((used + 1) * 10 > slots.size() * 7) {
        std::vector<std::array<uint64_t, 2>> old;
        old.swap(slots);
        slots.assign(std::max<size_t>(1024, old.size() * 2), {0, 0});
        used = 0;
        for (const auto& e : old)
            if (e[0] || e[1]) insert(e);
    }
    size_t mask = slots.size() - 1;
    for (size_t i = (size_t)(h[0] ^ (h[1] >> 7)) & mask;; i = (i + 1) & mask) {
        auto& e = slots[i];
        if (!e[0] && !e[1]) { e = h; ++used; return true; }
        if (e == h) return false;
    }
}

void WLPruner::run(const std::vector<std::string>& listfile_paths) {
    fs::create_directories(output_dir_);
    // Several inputs can share a combo (e.g. .txt and .bin, or outputs of
    // different runs). They must share one pruned writer -- a writer per file
    // truncated the previous file's output -- and one dedup scope.
    std::vector<std::pair<std::string, std::string>> inputs;
    for (const auto& p : listfile_paths) inputs.push_back({combo_of_input(p), p});
    std::stable_sort(inputs.begin(), inputs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    int n = (int)inputs.size();
    for (int i = 0; i < n;) {
        const std::string combo = inputs[i].first;
        std::string out_dir = output_dir_ + "/" + combo;
        fs::create_directories(out_dir);
        RollingCompressedWriter pruned_out(out_dir + "/eupruned.txt");
        canon_seen_.clear();
        solutions_by_hash_.clear();
        for (; i < n && inputs[i].first == combo; ++i) {
            if (n > 5 && i % std::max(1, n/10) == 0) {
                std::cerr << "\r  pruner: " << (i+1) << "/" << n << " files  [";
                bool first = true;
                for (const auto& [k, cnt] : solutions_per_k_) {
                    if (!first) std::cerr << " ";
                    std::cerr << "k" << k << ":" << cnt;
                    first = false;
                }
                std::cerr << "]" << std::flush;
            }
            process_file_wl(inputs[i].second, combo, pruned_out.stream(),
                            [&] { pruned_out.maybe_compress(); });
        }
    }
    canon_seen_.clear();
    solutions_by_hash_.clear();
    if (tes_store_) {
        tes_store_->finish();
        fs::rename(output_dir_ + "/tilings.sqlite3.tmp", output_dir_ + "/tilings.sqlite3");
    }
    if (mortier_store_) {
        mortier_store_->finish();
        fs::rename(output_dir_ + "/tilings.sqlite3.tmp", output_dir_ + "/tilings.sqlite3");
    }
    if (!g_keep_pruner_inputs) {
        for (const auto& path : listfile_paths) {
            std::error_code ec;
            fs::remove(path, ec);
            if (ec) std::cerr << "  warning: could not remove pruned input " << path
                              << ": " << ec.message() << "\n";
        }
    }
    if (n > 5) std::cerr << "\r" << std::string(60, ' ') << "\r" << std::flush;
    if (g_dedup_canon) {
        // exact canonical-form dedup: no diagnostics needed
    } else if (g_use_bfl) {
        std::cerr << "  BFL words: " << solutions_words_.size() << " unique\n";
    } else {
        std::cerr << "  WL diag: hash_hits=" << g_wl_hash_hits.load()
                  << " collisions=" << g_wl_collisions.load()
                  << " iso_calls=" << g_solutions_match_calls.load()
                  << " max_iters=" << g_wl_max_iters.load() << "\n";
    }
}

void WLPruner::parallel_for(int n, const std::function<void(int)>& fn) const {
    if (num_workers_ <= 1 || n < 10) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    int nw = std::min(num_workers_, n);
    std::vector<std::thread> threads;
    std::atomic<int> idx{0};
    for (int w = 0; w < nw; ++w) {
        threads.emplace_back([&]() {
            while (true) {
                int i = idx.fetch_add(1);
                if (i >= n) break;
                fn(i);
            }
        });
    }
    for (auto& t : threads) t.join();
}

WLPruner::CanonicalResult WLPruner::compute_canonical_and_wl(const State& st) const {
    CanonicalResult r;
    if (g_dedup_canon) {
        auto cf = canon::canonical_form(st);
        r.is_canonical = cf.minimal;
        r.canon_hash = cf.hash;
        return r;
    }
    auto [ok, log] = is_canonical_labeling(st);
    r.is_canonical = ok;
    (void)log;
    if (ok) {
        if (g_use_bfl)
            r.bfl_word = bfl_word_signature(st);
        else
            r.wl_hash = wl_hash_dispatch(st);
    }
    return r;
}

void WLPruner::process_file_wl(const std::string& path, const std::string& combo_code,
                               std::ostream& pruned_out,
                               const std::function<void()>& maybe_compress) {
    if (!fs::exists(path)) throw std::runtime_error("solution input disappeared: " + path);

    static constexpr int BATCH = 16384;
    std::vector<SolutionRecord> batch;
    batch.reserve(BATCH);
    std::vector<CanonicalResult> results;
    results.reserve(BATCH);

    std::string base = path;
    if (has_zst_suffix(base)) base.resize(base.size() - 4);
    bool is_bin = (base.size() > 4 && base.substr(base.size() - 4) == ".bin");
    auto in = open_solution_istream(path);
    if (!in) throw std::runtime_error("cannot open solution input: " + path);
    std::istream& in_ref = *in;
    int sol_idx = 0;
    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds decode_time{0};
    std::chrono::nanoseconds compute_time{0};
    std::chrono::nanoseconds consume_time{0};
    std::chrono::nanoseconds format_time{0}, write_time{0};
    int64_t records = 0;

    while (true) {
        auto stage_start = Clock::now();
        batch.clear();
        results.clear();
        SolutionRecord rec;
        bool ok;
        if (is_bin)
            ok = read_next_solution_bin(in_ref, rec, sol_idx);
        else
            ok = read_next_solution(in_ref, rec);
        if (!ok) break;
        batch.push_back(std::move(rec));

        // Fill rest of batch
        for (int i = 1; i < BATCH; ++i) {
            SolutionRecord rec2;
            bool ok2;
            if (is_bin)
                ok2 = read_next_solution_bin(in_ref, rec2, sol_idx);
            else
                ok2 = read_next_solution(in_ref, rec2);
            if (!ok2) break;
            batch.push_back(std::move(rec2));
        }
        if (batch.empty()) break;
        decode_time += Clock::now() - stage_start;
        records += (int64_t)batch.size();

        int n = (int)batch.size();
        results.resize(n);

        stage_start = Clock::now();
        parallel_for(n, [&](int i) { results[i] = compute_canonical_and_wl(batch[i].state); });
        compute_time += Clock::now() - stage_start;

        stage_start = Clock::now();
        // Stage 2 (sequential): dedup decisions, in input order.
        std::vector<int> unique_idx;
        for (int i = 0; i < n; ++i) {
            auto& rec2 = batch[i];
            auto& r = results[i];
            if (!r.is_canonical) continue;

            bool is_dup = false;
            if (g_dedup_canon) {
                is_dup = !canon_seen_.insert(r.canon_hash);
            } else if (g_use_bfl) {
                // Exact disk-backed canonical-word dedup.  The canonical word
                // (lex-min over all starting darts) identifies the tiling up to
                // isomorphism (Theorem 1); words live on disk, hashes in RAM.
                const auto& word = r.bfl_word;
                if (!word.empty()) {
                    if (solutions_words_.contains(word)) {
                        is_dup = true;
                    } else {
                        solutions_words_.insert(word);
                    }
                }
            } else {
                auto it = solutions_by_hash_.find(r.wl_hash);
                if (it != solutions_by_hash_.end()) {
                    g_wl_hash_hits++;
                    if (g_no_iso_check) {
                        is_dup = true;   // trust the WL hash, skip isomorphism fallback
                    } else {
                        for (const auto& stored : it->second) {
                            State stored_st = EuclideanSolver::unpack_state(stored);
                            g_solutions_match_calls++;
                            if (solutions_match(rec2.state, stored_st).first) {
                                is_dup = true;
                                break;
                            }
                        }
                        if (!is_dup) g_wl_collisions++;
                    }
                }
            }
            if (is_dup) continue;

            if (!g_use_bfl && !g_dedup_canon) {
                if (g_no_iso_check) solutions_by_hash_.try_emplace(r.wl_hash);  // track key only
                else solutions_by_hash_[r.wl_hash].push_back(EuclideanSolver::pack_state(rec2.state));
            }
            int k = (int)rec2.state.vertype.size();
            auto kit = solutions_per_k_.find(k);
            solutions_per_k_[k] = (kit != solutions_per_k_.end() ? kit->second + 1 : 1);
            unique_idx.push_back(i);
        }

        // Stage 3 (parallel): format every unique tiling's outputs.
        struct Formatted {
            std::string text, sig_raw, tes_filename, tes_doc;
            std::vector<uint8_t> mortier_hash;
            mortier_geometry::DevelopmentResult developed;
            std::string error;
        };
        std::vector<Formatted> formatted(unique_idx.size());
        auto format_start = Clock::now();
        parallel_for((int)unique_idx.size(), [&](int u) {
            auto& rec2 = batch[unique_idx[u]];
            auto& f = formatted[u];
            try {
                materialize_output_fields(rec2);
                std::string old_rel = rec2.tes_line.substr(rec2.tes_line.find("eu"));
                while (!old_rel.empty() && (old_rel.back() == '\n' || old_rel.back() == '\r'))
                    old_rel.pop_back();
                f.tes_filename = old_rel;
                if (f.tes_filename.find("eu raw ") == 0)
                    f.tes_filename = "eu " + f.tes_filename.substr(7);
                f.sig_raw = rec2.signature_line;
                while (!f.sig_raw.empty() && (f.sig_raw.back() == '\n' || f.sig_raw.back() == '\r'))
                    f.sig_raw.pop_back();

                std::ostringstream out;
                out << rec2.vertex_line << "\n" << rec2.signature_line << "\n"
                    << "Count type: " << rec2.count_signature << "\n"
                    << rec2.tes_line << "\n" << rec2.conway_line << "\n";
                CanonicalTilingOutput output = build_canonical_tiling_output(rec2.state);
                for (const std::string& line : output.cycle_lines) out << line << "\n";
                out << "---\n" << output.conway << "\n\n";
                f.text = out.str();
                if (mortier_store_) {
                    try {
                        f.developed = mortier_geometry::develop_exact_geometry(output.geometry);
                    } catch (const std::exception& error) {
                        throw std::runtime_error("Mortier export failed for " + f.sig_raw
                                                 + ": " + error.what());
                    }
                    std::string hash = bfl_canonical_hash(rec2.state);
                    f.mortier_hash.assign(hash.begin(), hash.end());
                } else if (tes_store_) {
                    f.tes_doc = tes_document(output, f.sig_raw);
                }
            } catch (const std::exception& e) {
                f.error = e.what();
            }
        });

        format_time += Clock::now() - format_start;
        // Stage 4 (sequential): write in input order.
        auto write_start = Clock::now();
        for (size_t u = 0; u < unique_idx.size(); ++u) {
            auto& f = formatted[u];
            if (!f.error.empty()) throw std::runtime_error(f.error);
            pruned_out << f.text;
            if (mortier_store_)
                mortier_store_->add(f.mortier_hash, (int)batch[unique_idx[u]].state.vertype.size(),
                                    f.developed.record, f.sig_raw);
            else if (tes_store_)
                tes_store_->add(combo_code, f.sig_raw, f.tes_filename, f.tes_doc);
        }

        write_time += Clock::now() - write_start;
        // Once per batch (not per line) is plenty granular given batches are
        // up to 2000 solutions — keeps the stat()+possible zstd spawn rare.
        maybe_compress();
        consume_time += Clock::now() - stage_start;
    }
    auto millis = [](std::chrono::nanoseconds d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    if (g_profile_pruner) {
        std::cerr << "  pruner profile " << combo_code << ": records=" << records
                  << " decode=" << millis(decode_time) << "ms"
                  << " compute=" << millis(compute_time) << "ms"
                  << " consume=" << millis(consume_time) << "ms"
                  << " format=" << millis(format_time) << "ms"
                  << " write=" << millis(write_time) << "ms\n";
    }
}
