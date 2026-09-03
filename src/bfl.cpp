#include "bfl.h"
#include <algorithm>
#include <deque>

std::vector<int> bfl_labels(const State& st, int start) {
    int n = (int)st.darts.size();
    int EPSILON = n;
    std::vector<int> label(n + 1, -1);

    std::vector<int> lneig(n, -1);
    for (int i = 0; i < n; ++i) lneig[st.darts[i].rneig] = i;

    label[EPSILON] = 0;

    std::deque<int> queue;
    queue.push_back(start);
    label[start] = 1;
    int next = 2;

    while (!queue.empty()) {
        int d = queue.front(); queue.pop_front();

        // β₀=σ⁻¹, β₁=σ, β₂=α, β₃=μ (mirror).  All four relations must be
        // traversed for the word to be a complete invariant of the full
        // (σ, α, μ) map — omitting μ merges mirror-distinct tilings.
        int neighbors[4] = {
            d >= 0 && d < n ? lneig[d] : EPSILON,
            d >= 0 && d < n ? st.darts[d].rneig : EPSILON,
            d >= 0 && d < n ? (st.darts[d].glue != -1 ? st.darts[d].glue : EPSILON) : EPSILON,
            d >= 0 && d < n ? st.darts[d].mirro : EPSILON,
        };

        for (int nb : neighbors) {
            if (nb < 0 || nb > n) nb = EPSILON;
            if (nb == EPSILON && d == EPSILON) continue;
            if (label[nb] != -1) continue;
            label[nb] = next++;
            queue.push_back(nb);
        }
    }

    return label;
}

std::vector<int> bfl_build_word(const State& st, const std::vector<int>& label) {
    int n = (int)st.darts.size();
    int EPSILON = n;

    std::vector<int> flag_of(n + 1, -1);
    for (int i = 0; i < n; ++i)
        if (label[i] >= 0 && label[i] <= n)
            flag_of[label[i]] = i;
    flag_of[0] = EPSILON;

    int reached = 0;
    for (int k = 1; k <= n; ++k) {
        if (flag_of[k] == -1) break;
        reached = k;
    }

    auto label_of = [&](int dart_idx) -> int {
        if (dart_idx < 0 || dart_idx >= n) return 0;
        return label[dart_idx];
    };

    std::vector<int> word;
    word.reserve(reached * 4);

    for (int k = 1; k <= reached; ++k) {
        int d = flag_of[k];
        word.push_back(d >= 0 && d < n ? st.darts[d].polygon_size : 0);
        word.push_back(label_of(d >= 0 && d < n ? st.darts[d].rneig : -1));
        word.push_back(label_of(d >= 0 && d < n ? st.darts[d].glue : -1));
        word.push_back(label_of(d >= 0 && d < n ? st.darts[d].mirro : -1));
    }

    return word;
}

std::vector<int> bfl_word_signature(const State& st) {
    int n = (int)st.darts.size();
    if (n == 0) return {};

    std::vector<int> best;
    for (int start = 0; start < n; ++start) {
        auto label = bfl_labels(st, start);
        auto word = bfl_build_word(st, label);
        if (word.empty()) continue;
        if (best.empty() || word < best)
            best = std::move(word);
    }

    return best;
}

std::vector<int> bfl_word_from_start(const State& st, int start_dart) {
    auto label = bfl_labels(st, start_dart);
    return bfl_build_word(st, label);
}

static inline uint64_t fnv64(uint64_t h, uint64_t x) { return (h ^ x) * 1099511628211ULL; }

std::array<uint64_t, 4> bfl_canonical_hash128(const State& st) {
    auto word = bfl_word_signature(st);
    std::array<uint64_t, 4> h = {
        fnv64(0x9ae16a3b2f90404fULL, 0),
        fnv64(0x9ae16a3b2f90404fULL, 1),
        fnv64(0x9ae16a3b2f90404fULL, 2),
        fnv64(0x9ae16a3b2f90404fULL, 3),
    };
    for (size_t i = 0; i < word.size(); ++i) {
        uint64_t x = (uint64_t)(uint32_t)(word[i] + 2);
        h[i & 3] = fnv64(h[i & 3], x);
        h[(i + 1) & 3] = fnv64(h[(i + 1) & 3], x ^ 0xFF);
    }
    return h;
}

std::string bfl_canonical_hash(const State& st) {
    auto h = bfl_canonical_hash128(st);
    std::string out(32, '\0');
    for (int i = 0; i < 4; ++i)
        for (int byte = 0; byte < 8; ++byte)
            out[i * 8 + byte] = static_cast<char>(
                static_cast<unsigned char>((h[i] >> (byte * 8)) & 0xff));
    return out;
}
