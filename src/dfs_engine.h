#pragma once
// In-place depth-first search engine.
//
// Explores exactly the same search tree as EuclideanSolver::extend_into (same
// branching dart, same candidate order, same acceptance rule) but mutates one
// flat structure-of-arrays state and undoes glues via a trail instead of
// copying a State per child.
//
// Validity is checked incrementally: a glue only changes the polygon chains
// through the glued darts, and every chain violation (size mismatch, open chain
// longer than its polygon, closed chain not dividing it) is monotone -- once a
// chain is invalid no further glue can repair it.  Because every node handed to
// the engine is valid and at a forced-closure fixpoint, checking only touched
// chains is equivalent to the full check_partial/propagate_forced scans.
#include "state.h"
#include <cstdint>
#include <functional>
#include <vector>

class DfsEngine {
public:
    // Called for each complete tiling found.  The State is only built for
    // solutions, which are rare relative to interior nodes.
    using SolutionFn = std::function<void(const State&)>;
    // Called once per expanded interior node (for progress reporting).
    using NodeFn = std::function<void()>;

    DfsEngine(int max_polygons, SolutionFn on_solution, NodeFn on_node = {});

    // Run the complete subtree rooted at `root`. The root must be a state
    // produced by extend_into/the engine (valid and at a propagation fixpoint).
    void run(const State& root);

    int64_t nodes() const { return nodes_; }

    // Work sharing: when should_donate() returns true, a non-complete child is
    // handed to donate() (packed) instead of being explored here.
    std::function<bool()> should_donate;
    std::function<void(PackedState&&)> donate;
    PackedState pack() const;

    // Optional leaf hook: receives the engine at a complete leaf (use view()
    // and to_state()); when set, it replaces on_solution. Lets the caller
    // filter leaves without materialising a State for each.
    std::function<void(const DfsEngine&)> on_leaf;
    // Optional: called on entry to every interior node; returning true skips
    // its subtree (e.g. an isomorphic partial state was already explored).
    std::function<bool(const DfsEngine&)> prune_node;
    int num_vertices() const { return (int)vt_.size(); }
    struct View { int n; const int *R, *L, *M, *G, *P, *T; };   // T: vertex type per dart
    View view() const { return {n_, R_.data(), L_.data(), M_.data(), G_.data(), P_.data(), T_.data()}; }
    State to_state() const;

    // Testing aid: when set, run() expands only the root and passes every
    // child (complete or not) to this callback instead of recursing.
    std::function<void(const State&)> child_hook;

private:
    int max_polygons_;
    SolutionFn on_solution_;
    NodeFn on_node_;
    int64_t nodes_ = 0;

    // Structure-of-arrays dart data.
    std::vector<int> R_, L_, P_, M_, ME_, G_, T_;
    std::vector<int> vt_;
    int n_ = 0;
    int nfree_ = 0;

    struct TrailEntry { int dart; int old; };
    std::vector<TrailEntry> trail_;
    std::vector<int> work_;          // worklist of touched corners
    std::vector<uint32_t> seen_;     // epoch-stamped visit marks
    uint32_t epoch_ = 0;

    void load(const State& st);

    void attach(int gr);
    void detach(int gr);

    inline void set_glue(int d, int v) {
        int old = G_[d];
        if (old == v) return;
        trail_.push_back({d, old});
        if (old == -1) --nfree_;
        else if (v == -1) ++nfree_;
        G_[d] = v;
    }
    void undo_to(size_t mark);

    bool seam_compatible(int a, int b) const {
        return (M_[a] == a) == (M_[b] == b) &&
               P_[a] == P_[R_[b]] && P_[b] == P_[R_[a]];
    }

    int analyze_first_free();
    // Validate the chain containing corner c; if it is an open chain with zero
    // slack, report its free ends in (fa, fb). Returns false on violation.
    bool check_chain(int c, int& fa, int& fb) const;
    bool propagate();                // drain work_, applying forced closures
    bool unique_partners();
    bool glue_pair(int a, int b);    // glue a<->b plus mirror pair, push work
    bool finish_child();             // propagate + (k==max) unique partners

    void dfs();
    void visit_child();
    uint32_t next_epoch();
};
