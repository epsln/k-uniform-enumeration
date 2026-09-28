#include "dfs_engine.h"
#include "vertex_catalog.h"
#include <array>
#include <stdexcept>

using namespace catalog;

namespace {

struct Slot { int vertex_type; int slot; };
constexpr int MAX_PSIZE = 12;
using Buckets = std::array<std::array<std::array<std::vector<Slot>, 2>, MAX_PSIZE + 1>,
                           MAX_PSIZE + 1>;

// Identical construction (and therefore order) to
// EuclideanSolver::compatible_attachment_slots.
const Buckets& attachment_buckets() {
    static const Buckets buckets = [] {
        Buckets result;
        for (int gr = 0; gr < NUM_VERTEX_TYPES; ++gr) {
            int limit = attachment_limit(gr);
            for (int slot = 0; slot < limit; ++slot) {
                int left = polygon_sizes[gr][slot];
                int right = polygon_sizes[gr][right_neighbors[gr][slot]];
                int self_mirror = mirrors[gr][slot] == slot;
                result[left][right][self_mirror].push_back({gr, slot});
            }
        }
        return result;
    }();
    return buckets;
}

const std::vector<std::vector<int>>& mirror_edge_flags() {
    static const std::vector<std::vector<int>> flags = [] {
        std::vector<std::vector<int>> f(NUM_VERTEX_TYPES);
        for (int gr = 0; gr < NUM_VERTEX_TYPES; ++gr)
            for (const auto& lbl : edge_label_templates[gr])
                f[gr].push_back(!lbl.empty() && lbl[0] == '*');
        return f;
    }();
    return flags;
}

} // namespace

DfsEngine::DfsEngine(int max_polygons, SolutionFn on_solution, NodeFn on_node)
    : max_polygons_(max_polygons), on_solution_(std::move(on_solution)),
      on_node_(std::move(on_node)) {
    attachment_buckets();
    mirror_edge_flags();
}

uint32_t DfsEngine::next_epoch() {
    if (++epoch_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        epoch_ = 1;
    }
    return epoch_;
}

void DfsEngine::load(const State& st) {
    n_ = (int)st.darts.size();
    R_.resize(n_); L_.resize(n_); P_.resize(n_); M_.resize(n_); ME_.resize(n_); G_.resize(n_);
    nfree_ = 0;
    for (int i = 0; i < n_; ++i) {
        const Dart& d = st.darts[i];
        R_[i] = d.rneig; L_[i] = d.lneig; P_[i] = d.polygon_size;
        M_[i] = d.mirro; ME_[i] = d.is_mirror_edge; G_[i] = d.glue;
        if (d.glue == -1) ++nfree_;
    }
    vt_ = st.vertype;
    if (seen_.size() < (size_t)n_) seen_.resize(n_, 0);
    trail_.clear();
    work_.clear();
}

State DfsEngine::to_state() const {
    State s;
    s.darts.resize(n_);
    for (int i = 0; i < n_; ++i) {
        Dart& d = s.darts[i];
        d.rneig = R_[i]; d.lneig = L_[i]; d.polygon_size = P_[i];
        d.mirro = M_[i]; d.glue = G_[i]; d.is_mirror_edge = ME_[i];
    }
    s.vertype = vt_;
    return s;
}

PackedState DfsEngine::pack() const {
    PackedState p;
    p.vertype.reserve(vt_.size());
    for (int v : vt_) p.vertype.push_back((uint8_t)v);
    p.glue.reserve(n_);
    for (int i = 0; i < n_; ++i) p.glue.push_back((int16_t)G_[i]);
    return p;
}

void DfsEngine::attach(int gr) {
    int off = n_;
    int sl = (int)left_neighbors[gr].size();
    const auto& me = mirror_edge_flags()[gr];
    for (int sg = 0; sg < sl; ++sg) {
        R_.push_back(off + right_neighbors[gr][sg]);
        L_.push_back(off + left_neighbors[gr][sg]);
        M_.push_back(off + mirrors[gr][sg]);
        P_.push_back(polygon_sizes[gr][sg]);
        ME_.push_back(me[sg]);
        G_.push_back(-1);
    }
    n_ += sl;
    nfree_ += sl;
    vt_.push_back(gr);
    if (seen_.size() < (size_t)n_) seen_.resize(n_, 0);
}

void DfsEngine::detach(int gr) {
    int sl = (int)left_neighbors[gr].size();
    n_ -= sl;
    nfree_ -= sl;
    R_.resize(n_); L_.resize(n_); M_.resize(n_); P_.resize(n_); ME_.resize(n_); G_.resize(n_);
    vt_.pop_back();
}

void DfsEngine::undo_to(size_t mark) {
    while (trail_.size() > mark) {
        const TrailEntry& e = trail_.back();
        int cur = G_[e.dart];
        if (cur == -1 && e.old != -1) --nfree_;
        else if (cur != -1 && e.old == -1) ++nfree_;
        G_[e.dart] = e.old;
        trail_.pop_back();
    }
}

// Exact replica of EuclideanSolver::analyze_cycles (including tie-breaking).
int DfsEngine::analyze_first_free() {
    uint32_t ep = next_epoch();
    int best = -1, best_slack = 13;
    for (int start = 0; start < n_; ++start) {
        if (seen_[start] == ep) continue;
        int left = start;
        int ring_sz = P_[R_[start]];
        bool unclosed = false;
        while (G_[left] != -1 && G_[left] != R_[start]) {
            int prev = L_[G_[left]];
            if (P_[R_[prev]] != ring_sz) { unclosed = true; break; }
            left = prev;
        }
        if (unclosed) continue;
        if (G_[left] == -1) {
            int stable = left, right = R_[left];
            int v_stable = P_[right], cnt = 0;
            while (true) {
                seen_[left] = ep; ++cnt;
                left = G_[right];
                if (left != -1) { right = R_[left]; }
                else {
                    if (seen_[start] != ep) break;
                    int sl = v_stable - cnt;
                    if (sl < best_slack) { best = stable; best_slack = sl; }
                    break;
                }
            }
        } else {
            left = start; int right = R_[left];
            while (true) {
                seen_[left] = ep;
                left = G_[right];
                if (left == start || left == -1) break;
                right = R_[left];
            }
        }
    }
    return best;
}

bool DfsEngine::check_chain(int c, int& fa, int& fb) const {
    fa = fb = -1;
    // Walk backwards to the chain start (or detect a closed chain).
    int s = c;
    bool closed = false;
    for (int steps = 0;; ++steps) {
        int g = G_[s];
        if (g == -1) break;
        int p = L_[g];
        if (p == c) { closed = true; break; }
        s = p;
        if (steps > n_) return false;   // malformed; cannot happen for involutive glue
    }
    if (closed) s = c;
    int sz = P_[R_[s]];
    int cnt = 1, cur = s;
    while (true) {
        int r = R_[cur];
        if (P_[r] != sz) return false;
        int nx = G_[r];
        if (nx == -1) {
            if (cnt == sz) { fa = s; fb = r; }
            return true;              // open chain, cnt <= sz guaranteed below
        }
        if (nx == s) return sz % cnt == 0;
        if (++cnt > sz) return false;
        cur = nx;
    }
}

bool DfsEngine::propagate() {
    while (!work_.empty()) {
        int c = work_.back(); work_.pop_back();
        int a, b;
        if (!check_chain(c, a, b)) return false;
        if (a < 0) continue;
        // Forced closure of a zero-slack open chain (as propagate_forced).
        bool am = (M_[a] == a), bm = (M_[b] == b);
        if (am != bm) return false;
        set_glue(a, b); set_glue(b, a);
        work_.push_back(a); work_.push_back(b);
        if (!am) {
            int ma = M_[a], mb = M_[b];
            if (G_[ma] != -1 && G_[ma] != mb) return false;
            if (G_[mb] != -1 && G_[mb] != ma) return false;
            set_glue(ma, mb); set_glue(mb, ma);
            work_.push_back(ma); work_.push_back(mb);
        }
    }
    return true;
}

// Same decisions as EuclideanSolver::propagate_unique_partners, but candidate
// counts come from a (left, right, self-mirror) histogram instead of an O(n)
// scan per source.
bool DfsEngine::unique_partners() {
    if ((int)vt_.size() < max_polygons_) return true;
    while (true) {
        int cnt[MAX_PSIZE + 1][MAX_PSIZE + 1][2] = {};
        for (int t = 0; t < n_; ++t)
            if (G_[t] == -1) ++cnt[P_[t]][P_[R_[t]]][M_[t] == t];
        bool changed = false;
        for (int s = 0; s < n_; ++s) {
            if (G_[s] != -1) continue;
            int c = cnt[P_[R_[s]]][P_[s]][M_[s] == s];
            if (c == 0) return false;
            if (c != 1) continue;
            int t = -1;
            for (int j = 0; j < n_; ++j)
                if (G_[j] == -1 && seam_compatible(s, j)) { t = j; break; }
            int ms = M_[s], mt = M_[t];
            set_glue(s, t); set_glue(t, s);
            work_.push_back(s); work_.push_back(t);
            if (ms != s) {
                if ((G_[ms] != -1 && G_[ms] != mt) || (G_[mt] != -1 && G_[mt] != ms))
                    return false;
                set_glue(ms, mt); set_glue(mt, ms);
                work_.push_back(ms); work_.push_back(mt);
            }
            if (!propagate()) return false;
            changed = true;
            break;
        }
        if (!changed) return true;
    }
}

bool DfsEngine::finish_child() {
    if (!propagate()) return false;
    return unique_partners();
}

void DfsEngine::visit_child() {
    if (child_hook) child_hook(to_state());
    else if (nfree_ == 0) on_solution_(to_state());
    else if (should_donate && should_donate()) donate(pack());
    else dfs();
}

void DfsEngine::dfs() {
    ++nodes_;
    if (on_node_) on_node_();
    int ff = analyze_first_free();
    if (ff < 0) return;
    if (ME_[ff]) ff = M_[ff];
    bool mirrored = (M_[ff] == ff);
    int n = n_;

    auto glue_branch = [&](int i) {
        // Mirrors extend_into's unconditional assignments.
        set_glue(ff, i); set_glue(i, ff);
        work_.push_back(ff); work_.push_back(i);
        if (!mirrored) {
            int mf = M_[ff], mi = M_[i];
            set_glue(mf, mi); set_glue(mi, mf);
            work_.push_back(mf); work_.push_back(mi);
        }
    };

    for (int i = 0; i < n; ++i) {
        if (G_[i] != -1) continue;
        if ((M_[i] == i) != mirrored) continue;
        if (!seam_compatible(ff, i)) continue;
        size_t mark = trail_.size();
        work_.clear();
        glue_branch(i);
        if (finish_child()) visit_child();
        undo_to(mark);
    }

    if ((int)vt_.size() < max_polygons_) {
        int left = P_[ff], right = P_[R_[ff]];
        if (left > MAX_PSIZE || right > MAX_PSIZE) return;
        const auto& slots = attachment_buckets()[right][left][mirrored ? 1 : 0];
        int prepared = -1;
        for (const auto& a : slots) {
            int gr = a.vertex_type;
            if (gr < vt_[0]) continue;
            if (gr != prepared) {
                if (prepared >= 0) detach(prepared);
                attach(gr);
                prepared = gr;
            }
            size_t mark = trail_.size();
            work_.clear();
            glue_branch(n + a.slot);
            if (finish_child()) visit_child();
            undo_to(mark);
        }
        if (prepared >= 0) detach(prepared);
    }
}

void DfsEngine::run(const State& root) {
    load(root);
    if (nfree_ == 0) {
        ++nodes_;
        if (on_node_) on_node_();
        on_solution_(root);
        return;
    }
    dfs();
}
