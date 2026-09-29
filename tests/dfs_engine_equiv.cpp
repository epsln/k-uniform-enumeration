// Checks that DfsEngine generates exactly the children of extend_into for
// every state of a BFS fan-out.
#include "dfs_engine.h"
#include "disk_solver.h"
#include "solver.h"
#include "vertex_catalog.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

bool g_propagate = true;
bool g_binary_solutions = false;

static std::vector<int> key(const State& s) {
    std::vector<int> k(s.vertype.begin(), s.vertype.end());
    k.push_back(-99);
    for (const auto& d : s.darts) k.push_back(d.glue);
    return k;
}

int main(int argc, char** argv) {
    int k = argc > 1 ? std::atoi(argv[1]) : 6;
    int target = argc > 2 ? std::atoi(argv[2]) : 20000;
    // Walk the legacy search tree breadth-first (up to `target` nodes) and
    // compare the children generated at every node.
    std::vector<PackedState> frontier;
    for (int vt = 0; vt < catalog::NUM_VERTEX_TYPES; ++vt)
        frontier.push_back(EuclideanSolver::pack_state(EuclideanSolver::make_initial(vt)));
    DfsEngine eng(k, [](const State&) {});
    long bad = 0, total = 0;
    for (size_t head = 0; head < frontier.size() && (int)head < target; ++head) {
        State st = EuclideanSolver::unpack_state(frontier[head]);
        std::vector<std::vector<int>> a, b;
        EuclideanSolver::extend_into(st, [&](State&& c) {
            a.push_back(key(c));
            for (const auto& d : c.darts)
                if (d.glue == -1) { frontier.push_back(EuclideanSolver::pack_state(c)); break; }
        }, k);
        eng.child_hook = [&](const State& c) { b.push_back(key(c)); };
        eng.run(st);
        total += a.size();
        if (a != b) {
            if (++bad <= 3) {
                std::cerr << "mismatch: legacy " << a.size() << " engine " << b.size() << "\n";
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                std::cerr << "  (as sets " << (a == b ? "equal: order differs" : "differ") << ")\n";
                for (size_t c = 0; c < std::min(a.size(), b.size()); ++c) {
                    if (a[c] == b[c]) continue;
                    std::cerr << "  child " << c << " size " << a[c].size() << "/" << b[c].size() << ":";
                    for (size_t j = 0; j < std::min(a[c].size(), b[c].size()); ++j)
                        if (a[c][j] != b[c][j]) std::cerr << " [" << j << "] " << a[c][j] << " vs " << b[c][j];
                    std::cerr << "\n";
                    break;
                }
            }
        }
    }
    std::cout << std::min<size_t>(frontier.size(), target) << " states, " << total << " children, " << bad << " mismatches\n";
    return bad ? 1 : 0;
}
