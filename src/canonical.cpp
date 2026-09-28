#include "canonical.h"
#include <algorithm>
#include <stdexcept>

namespace canon {

namespace {

inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

} // namespace

Result canonical_form(const State& st, std::vector<int32_t>* code) {
    Result res;
    const int n = (int)st.darts.size();
    if (n == 0) return res;
    if (n >= 4096) throw std::runtime_error("canonical_form: too many darts");

    // Partition refinement with colours = start position of the dart's cell
    // (nauty style). Singletons never change colour, and only cells that hold
    // a preimage of a recoloured dart are re-examined. Every decision depends
    // on colours alone, so the final labelling is isomorphism-invariant.
    static thread_local std::vector<int> col, perm, cend;
    static thread_local std::vector<uint8_t> dirty;
    static thread_local std::vector<std::pair<uint64_t, int>> keyed;
    col.resize(n); perm.resize(n); cend.resize(n); dirty.assign(n, 0); keyed.resize(n);

    const Dart* D = st.darts.data();
    for (int i = 0; i < n; ++i) {
        if (D[i].glue < 0) throw std::runtime_error("canonical_form: incomplete state");
        keyed[i] = {(uint64_t)D[i].polygon_size, i};
    }
    std::sort(keyed.begin(), keyed.end());
    for (int p = 0; p < n;) {
        int q = p;
        while (q < n && keyed[q].first == keyed[p].first) ++q;
        cend[p] = q;
        dirty[p] = (q - p) > 1;
        for (int j = p; j < q; ++j) { perm[j] = keyed[j].second; col[keyed[j].second] = p; }
        p = q;
    }

    bool again = true;
    while (again) {
        again = false;
        for (int s = 0; s < n;) {
            int e = cend[s];
            if (e - s == 1 || !dirty[s]) { s = e; continue; }
            dirty[s] = 0;
            for (int j = s; j < e; ++j) {
                int d = perm[j];
                keyed[j] = {((uint64_t)col[D[d].rneig] << 36) | ((uint64_t)col[D[d].lneig] << 24) |
                            ((uint64_t)col[D[d].mirro] << 12) | (uint64_t)col[D[d].glue], d};
            }
            auto* kb = keyed.data() + s;
            auto* ke = keyed.data() + e;
            if (e - s <= 16) {
                for (auto* a = kb + 1; a < ke; ++a) {
                    auto v = *a; auto* b = a;
                    while (b > kb && v < *(b - 1)) { *b = *(b - 1); --b; }
                    *b = v;
                }
            } else {
                std::sort(kb, ke);
            }
            if (kb->first == (ke - 1)->first) { s = e; continue; }
            again = true;
            for (int a = s; a < e;) {
                int b = a;
                while (b < e && keyed[b].first == keyed[a].first) ++b;
                cend[a] = b;
                for (int j = a; j < b; ++j) {
                    int d = keyed[j].second;
                    perm[j] = d;
                    if (col[d] != a) {
                        col[d] = a;
                        dirty[col[D[d].lneig]] = 1;   // R^-1(d)
                        dirty[col[D[d].rneig]] = 1;   // L^-1(d)
                        dirty[col[D[d].mirro]] = 1;
                        dirty[col[D[d].glue]] = 1;
                    }
                }
                a = b;
            }
            // Neighbours inside this cell may have been recoloured after
            // their mark was taken, so conservatively re-examine every
            // non-singleton sub-cell.
            for (int a = s; a < e; a = cend[a]) dirty[a] = (cend[a] - a) > 1;
            s = e;
        }
    }
    int classes = 0;
    for (int s = 0; s < n; s = cend[s]) ++classes;
    if (classes < n) return res;    // non-discrete: some darts are bisimilar

    res.minimal = true;
    // Canonical code: relation table in colour order.
    const std::vector<int>& by_col = perm;
    uint64_t h1 = 0x9e3779b97f4a7c15ULL ^ (uint64_t)n, h2 = 0xc2b2ae3d27d4eb4fULL + (uint64_t)n;
    if (code) { code->clear(); code->reserve((size_t)n * 5); }
    for (int c = 0; c < n; ++c) {
        const Dart& d = D[by_col[c]];
        uint64_t w = ((uint64_t)d.polygon_size << 48) | ((uint64_t)col[d.rneig] << 36) |
                     ((uint64_t)col[d.lneig] << 24) | ((uint64_t)col[d.mirro] << 12) |
                     (uint64_t)col[d.glue];
        h1 = mix64(h1 ^ w) + 0x632be59bd9b4e019ULL;
        h2 = mix64(h2 + w * 0x9e3779b97f4a7c15ULL) ^ (h1 >> 17);
        if (code) {
            code->push_back(d.polygon_size); code->push_back(col[d.rneig]);
            code->push_back(col[d.lneig]); code->push_back(col[d.mirro]);
            code->push_back(col[d.glue]);
        }
    }
    res.hash = {h1, h2};
    return res;
}

} // namespace canon
