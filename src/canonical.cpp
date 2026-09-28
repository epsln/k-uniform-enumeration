#include "canonical.h"
#include <algorithm>
#include <stdexcept>

namespace canon {

namespace {

constexpr int MAX_SIZE = 12;

inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

} // namespace

Result canonical_form(const State& st, std::vector<int32_t>* code) {
    static thread_local std::vector<int> R, L, M, G, P;
    const int n = (int)st.darts.size();
    R.resize(n); L.resize(n); M.resize(n); G.resize(n); P.resize(n);
    for (int i = 0; i < n; ++i) {
        const Dart& d = st.darts[i];
        R[i] = d.rneig; L[i] = d.lneig; M[i] = d.mirro; G[i] = d.glue; P[i] = d.polygon_size;
    }
    return canonical_form(DartView{n, R.data(), L.data(), M.data(), G.data(), P.data()}, code);
}

Result canonical_form(const DartView& v, std::vector<int32_t>* code) {
    Result res;
    const int n = v.n;
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

    const int* R = v.R; const int* L = v.L; const int* M = v.M;
    const int* G = v.G; const int* P = v.P;
    // Free darts (partial states only) get a sentinel colour no cell can have.
    auto gcol = [&](int d) -> int { return G[d] < 0 ? 4095 : col[G[d]]; };
    // Initial cells: counting sort by (extra colour, polygon size).
    const int* C = v.C;
    auto init_key = [&](int i) { return (C ? C[i] : 0) * (MAX_SIZE + 1) + P[i]; };
    const int NKEYS = (C ? 64 : 1) * (MAX_SIZE + 1);
    static thread_local std::vector<int> count;
    count.assign(NKEYS + 1, 0);
    for (int i = 0; i < n; ++i) {
        if (G[i] < 0 && !v.partial) throw std::runtime_error("canonical_form: incomplete state");
        if (P[i] < 0 || P[i] > MAX_SIZE || (C && (C[i] < 0 || C[i] >= 64)))
            throw std::runtime_error("canonical_form: initial colour out of range");
        ++count[init_key(i) + 1];
    }
    for (int s = 1; s <= NKEYS; ++s) count[s] += count[s - 1];
    for (int i = 0; i < n; ++i) perm[count[init_key(i)]++] = i;
    for (int p = 0; p < n;) {
        int q = p;
        while (q < n && init_key(perm[q]) == init_key(perm[p])) ++q;
        cend[p] = q;
        dirty[p] = (q - p) > 1;
        for (int j = p; j < q; ++j) col[perm[j]] = p;
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
                keyed[j] = {((uint64_t)col[R[d]] << 36) | ((uint64_t)col[L[d]] << 24) |
                            ((uint64_t)col[M[d]] << 12) | (uint64_t)gcol(d), d};
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
                        dirty[col[L[d]]] = 1;   // R^-1(d)
                        dirty[col[R[d]]] = 1;   // L^-1(d)
                        dirty[col[M[d]]] = 1;
                        if (G[d] >= 0) dirty[col[G[d]]] = 1;
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
        const int d = by_col[c];
        // The extra colour must be part of the code, not only of the initial
        // partition, or differently-labelled structures could share a code.
        uint64_t w = ((uint64_t)(C ? C[d] : 0) << 52) |
                     ((uint64_t)P[d] << 48) | ((uint64_t)col[R[d]] << 36) |
                     ((uint64_t)col[L[d]] << 24) | ((uint64_t)col[M[d]] << 12) |
                     (uint64_t)gcol(d);
        h1 = mix64(h1 ^ w) + 0x632be59bd9b4e019ULL;
        h2 = mix64(h2 + w * 0x9e3779b97f4a7c15ULL) ^ (h1 >> 17);
        if (code) {
            code->push_back((C ? C[d] : 0) * 16 + P[d]); code->push_back(col[R[d]]);
            code->push_back(col[L[d]]); code->push_back(col[M[d]]);
            code->push_back(gcol(d));
        }
    }
    res.hash = {h1, h2};
    return res;
}

} // namespace canon
