#pragma once
#include "state.h"
#include "vertex_catalog.h"
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Hash for the vertex-type histogram key (vector<int> of NUM_VERTEX_TYPES counts).
struct VecHash {
    size_t operator()(const std::vector<int>& v) const {
        uint64_t h = 14695981039346656037ULL;
        for (int x : v) { h ^= (uint64_t)(uint32_t)x; h *= 1099511628211ULL; }
        return (size_t)h;
    }
};
using HistogramMap = std::unordered_map<std::vector<int>, int, VecHash>;
// Cap on distinct per-signature counters (sol_idx is cosmetic only).
constexpr int64_t HISTOGRAM_CAP = 1000000;

extern bool g_propagate;
extern bool g_binary_solutions;

inline std::string rebuild_label(int slot, const std::vector<int>& vertype) {
    int offset = 0;
    int tile = 0;
    for (tile = 0; tile < (int)vertype.size(); ++tile) {
        int sl = (int)catalog::left_neighbors[vertype[tile]].size();
        if (slot < offset + sl) break;
        offset += sl;
    }
    if (tile >= (int)vertype.size()) return "?";
    int vt = vertype[tile];
    int local = slot - offset;
    const std::string& tmpl = catalog::edge_label_templates[vt][local];
    if (tile > 3) return tmpl + "@" + std::to_string(tile);
    if (tile == 0) return tmpl;
    std::string out = tmpl;
    for (int t = 0; t < tile; ++t) out += "'";
    return out;
}

inline std::string verbal_vertices(const std::vector<int>& vt) {
    std::string out;
    for (int t : vt) { if (!out.empty()) out += ", "; out += catalog::symbols[t]; }
    return out;
}

inline std::vector<int> sig_result(const std::vector<int>& vt) {
    std::vector<int> r(catalog::NUM_VERTEX_TYPES, 0);
    for (int t : vt) ++r[t];
    return r;
}

inline std::string signature(const std::vector<int>& vt) {
    auto c = sig_result(vt); std::string out;
    for (int i = 0; i < catalog::NUM_VERTEX_TYPES; ++i) {
        if (c[i] == 0) continue;
        if (!out.empty()) out += ", ";
        out += catalog::symbols[i];
        if (c[i] > 1) out += "x" + std::to_string(c[i]);
    }
    return out;
}

inline std::string file_signature(const std::vector<int>& vt) {
    auto c = sig_result(vt); std::string out;
    for (int i = 0; i < catalog::NUM_VERTEX_TYPES; ++i) {
        if (c[i] == 0) continue;
        if (!out.empty()) out += " ";
        out += catalog::codes[i];
        if (c[i] > 1) out += std::to_string(c[i]);
    }
    return out;
}

inline std::string pad2(int n) { return (n<10?"0":"") + std::to_string(n); }

inline std::string fine_name(const State& st) {
    std::string m = pad2((int)st.vertype.size()) + "_";
    std::set<int> sv; for (const auto& d : st.darts) sv.insert(d.polygon_size);
    for (int sz : {3,4,6,12}) if (sv.count(sz)) m += (sz==12?"c":std::to_string(sz));
    return m;
}

inline std::string conway_symbol(const std::string& f, const std::string& s) {
    std::string a = f, b = s; int mc = 0;
    if (!a.empty() && a[0] == '*') { ++mc; a = a.substr(1); }
    if (!b.empty() && b[0] == '*') { ++mc; b = b.substr(1); }
    return (mc != 1) ? (a==b ? "("+a+")" : "("+a+" "+b+")")
                     : (a==b ? "["+a+"]" : "["+a+" "+b+"]");
}

inline std::string write_conway(const State& st) {
    std::set<int> seen; std::string out;
    for (int i = 0; i < (int)st.darts.size(); ++i) {
        if (seen.count(i) || st.darts[i].glue == -1) continue;
        out += conway_symbol(rebuild_label(i, st.vertype),
                             rebuild_label(st.darts[i].glue, st.vertype));
        seen.insert({i, st.darts[i].glue, st.darts[i].mirro, st.darts[st.darts[i].mirro].glue});
    }
    return out;
}

class EuclideanSolver {
public:
    struct AttachmentSlot {
        int vertex_type;
        int slot;
    };

    EuclideanSolver(int max_polygons, const std::string& output_dir);
    EuclideanSolver(int max_polygons, const std::string& output_dir,
                    std::vector<State> initial_states, bool write_log = false);

    void run();
    int partials_checked() const { return partials_checked_; }
    int solutions_found()  const { return solutions_found_; }
    const std::map<std::string, std::string>& solution_files() const { return solution_files_; }

    static void reset_global_counters();
    static std::map<int, int> global_per_k_counts();
    static void read_raw_per_k(std::array<int64_t, 24>& out);
    static std::mutex& global_mutex();

    static State make_initial(int vertex_type);
    static PackedState pack_state(const State& st);
    static State unpack_state(const PackedState& p);
    static State rebuild_from_vertype_glue(const std::vector<uint8_t>& vertype,
                                            const std::vector<int16_t>& glue);
    static State rebuild_from_vertype_glue(const std::vector<int>& vertype,
                                            const std::vector<int>& glue);
    static bool check_partial(const State& st);
    static std::pair<int,int> analyze_cycles(const State& st);
    static bool propagate_forced(std::vector<Dart>& darts);

    // Partial dedup and helpers (capped per worker, disabled by default)
    static bool partial_dedup_check(const State& cand, int max_polygons);
    static void set_pdedup_cap(int cap);

    // Canonical labeling check for partial states (glue[i]==-1 handled).
    // Returns true if this partial state's labeling is canonical — skip otherwise.
    static bool is_canonical_partial(const State& st);

    // Lexicographic edge ordering: deterministic, enables canonical pruning.
    // Returns -1 if no free edges.
    static int first_free_lex(const State& st);

    // Helpers needed by extend_into template (must be declared before use)
    static int neighbors_len(int gr);
    static int attach_limit(int gr);
    static int polygon_size_of(int gr, int slot);
    static State extend_state(const State& base, int gr, int offset, int sl);
    static bool seam_compatible(int a, int b, const std::vector<Dart>& darts);
    static const std::vector<AttachmentSlot>& compatible_attachment_slots(int source,
                                                                           const std::vector<Dart>& darts);
    static bool propagate_unique_partners(State& st, int max_polygons);

    // Core extend logic: for a given state, find first_free, then iterate over
    // all valid candidates (pairings + new-vertex attachments).  Calls `cb` for
    // each candidate.  `max_polygons` is the k limit.
    template<typename F>
    static void extend_into(const State& st, F&& cb, int max_polygons) {
        int first_free = analyze_cycles(st).first;
        if (first_free < 0) return;
        if (st.darts[first_free].is_mirror_edge)
            first_free = st.darts[first_free].mirro;
        bool mirrored = (st.darts[first_free].mirro == first_free);
        int n = (int)st.darts.size();

        // Pre-build free edge list to avoid scanning glued edges
        static thread_local std::vector<int> free_edges;
        free_edges.clear();
        for (int i = 0; i < n; ++i)
            if (st.darts[i].glue == -1) free_edges.push_back(i);

        for (int free_i = 0; free_i < (int)free_edges.size(); ++free_i) {
            int i = free_edges[free_i];
            if ((st.darts[i].mirro == i) != mirrored) continue;
            if (!seam_compatible(first_free, i, st.darts)) continue;
            State cand = st;
            cand.darts[first_free].glue = i; cand.darts[i].glue = first_free;
            if (!mirrored) {
                cand.darts[st.darts[first_free].mirro].glue = st.darts[i].mirro;
                cand.darts[st.darts[i].mirro].glue = st.darts[first_free].mirro;
            }
            if (g_propagate && !propagate_forced(cand.darts))
                continue;
            if (!check_partial(cand)) continue;
            if (!propagate_unique_partners(cand, max_polygons)) continue;
            cb(std::move(cand));
        }
        if ((int)st.vertype.size() < max_polygons) {
            const auto& attachments = compatible_attachment_slots(first_free, st.darts);
            int prepared_type = -1;
            State prepared;
            for (const auto& attachment : attachments) {
                int gr = attachment.vertex_type;
                if (gr < st.vertype[0]) continue;
                if (gr != prepared_type) {
                    int sl = neighbors_len(gr);
                    prepared = extend_state(st, gr, n, sl);
                    prepared_type = gr;
                }
                int i = n + attachment.slot;
                State cand = prepared;
                cand.darts[first_free].glue = i; cand.darts[i].glue = first_free;
                if (!mirrored) {
                    cand.darts[cand.darts[first_free].mirro].glue = cand.darts[i].mirro;
                    cand.darts[cand.darts[i].mirro].glue = cand.darts[first_free].mirro;
                }
                if (!check_partial(cand)) continue;
                if (g_propagate && !propagate_forced(cand.darts))
                    continue;
                if (!check_partial(cand)) continue;
                if (!propagate_unique_partners(cand, max_polygons)) continue;
                cb(std::move(cand));
            }
        }
    }

    static void write_solution_static(const State& st, const std::string& output_dir,
                                        std::mutex& mu,
                                        std::map<std::string,int>& run_totals,
                                        std::map<std::string,std::string>& solution_files,
                                        HistogramMap& vertex_combos);

private:
    int max_polygons_;
    std::string output_dir_;
    std::deque<PackedState> queue_;
    int partials_checked_ = 0;
    int solutions_found_ = 0;
    std::ofstream log_;
    bool write_log_;

    std::map<std::string, int> run_totals_;
    std::map<std::string, std::string> solution_files_;
    HistogramMap vertex_combos_;
    std::map<std::string, int> sol_number_by_sig_;

    void extend(const State& st);
    void write_solution(const State& st);
    void write_summary();
};
