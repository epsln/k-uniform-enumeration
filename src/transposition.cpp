#include "transposition.h"
#include <algorithm>

TranspositionTable::TranspositionTable(size_t megabytes) : locks_(new std::mutex[LOCKS]) {
    size_t entries = std::max<size_t>(WAYS, megabytes * 1024 * 1024 / sizeof(slots_[0]));
    size_t buckets = 1;
    while (buckets * 2 * WAYS <= entries) buckets *= 2;
    slots_.assign(buckets * WAYS, {0, 0});
    next_victim_.assign(buckets, 0);
    bucket_mask_ = buckets - 1;
}

bool TranspositionTable::seen_or_insert(const std::array<uint64_t, 2>& key) {
    std::array<uint64_t, 2> h = key;
    if (h[0] == 0 && h[1] == 0) h[1] = 1;     // all-zero marks an empty slot
    probes_.fetch_add(1, std::memory_order_relaxed);
    size_t b = (size_t)(h[0] ^ (h[1] >> 11)) & bucket_mask_;
    std::lock_guard<std::mutex> lk(locks_[b % LOCKS]);
    auto* e = &slots_[b * WAYS];
    for (int w = 0; w < WAYS; ++w) {
        if (e[w] == h) { hits_.fetch_add(1, std::memory_order_relaxed); return true; }
    }
    for (int w = 0; w < WAYS; ++w) {
        if (e[w][0] == 0 && e[w][1] == 0) { e[w] = h; return false; }
    }
    e[next_victim_[b]] = h;
    next_victim_[b] = (uint8_t)((next_victim_[b] + 1) % WAYS);
    return false;
}
