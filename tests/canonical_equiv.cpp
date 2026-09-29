// Validates canon::canonical_form against the pruner's canonical-labeling
// filter and the reference unique-tiling counts.
#include "canonical.h"
#include "dfs_engine.h"
#include "pruner.h"
#include "solver.h"
#include "vertex_catalog.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

bool g_propagate = true;
bool g_binary_solutions = false;

int main(int argc, char** argv) {
    int K = argc > 1 ? std::atoi(argv[1]) : 8;
    const long ref[] = {0, 10, 20, 61, 151, 332, 673, 1472, 2850, 5960, 11866, 24459, 49794};
    long leaves = 0, mismatch = 0;
    std::map<int, std::set<std::vector<int32_t>>> codes;
    std::map<int, std::set<std::array<uint64_t, 2>>> hashes;
    DfsEngine eng(K, [&](const State& s) {
        ++leaves;
        std::vector<int32_t> code;
        auto r = canon::canonical_form(s, &code);
        bool ref_ok = SolutionPruner::is_canonical_labeling(s).first;
        if (r.minimal != ref_ok) ++mismatch;
        if (r.minimal) {
            codes[(int)s.vertype.size()].insert(code);
            hashes[(int)s.vertype.size()].insert(r.hash);
        }
    });
    for (int vt = 0; vt < catalog::NUM_VERTEX_TYPES; ++vt)
        eng.run(EuclideanSolver::make_initial(vt));
    int bad = 0;
    for (int k = 1; k <= K; ++k) {
        long c = (long)codes[k].size(), h = (long)hashes[k].size();
        bool ok = c == ref[k] && h == c;
        if (!ok) ++bad;
        std::cout << "k=" << k << " codes=" << c << " hashes=" << h << " ref=" << ref[k]
                  << (ok ? "" : "  <-- MISMATCH") << "\n";
    }
    std::cout << leaves << " leaves, " << mismatch << " minimal/is_canonical mismatches\n";
    return (bad || mismatch) ? 1 : 0;
}
