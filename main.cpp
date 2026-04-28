/*
 * solver_zomega.cpp  —  Euclidean Tiling Solver (C++ edition)
 * ============================================================
 * High-performance C++17 port of solver_zomega.py.
 *
 * Disk-space strategy (for k=20, 150 GB budget)
 * -----------------------------------------------
 * Three changes from the previous version:
 *
 *   1. COMPACT SERIALISATION
 *      A state has 7 parallel vectors: rneig, lneig, lvert, mirro, glue,
 *      label, vertype.  Only glue, vertype, num, and label carry information
 *      that cannot be reconstructed — rneig/lneig/lvert/mirro are completely
 *      determined by vertype (they are the concatenation of the per-symbol
 *      table rows).  On-disk format stores only:
 *        num (1 byte)  |  vertype (num bytes)  |  glue (2 bytes/edge, int16)
 *        |  label (2 bytes/edge, encoded int16)
 *      A state at k=20 serialises to ~300-600 bytes vs ~4000 bytes before.
 *      That alone cuts chunk-file size by ~85%.
 *
 *   2. IMMEDIATE CHUNK DELETION
 *      Each chunk file is deleted as soon as pass-1 dedup has consumed it.
 *      Chunk files and bucket files never coexist in full.  Peak disk is
 *      max(total raw solutions, total simplest-filtered solutions), not 2×.
 *
 *   3. BUCKET FILE CLEANUP
 *      Each bucket file is deleted immediately after pass-2 processes it,
 *      so bucket files and unique-solution output never coexist in full either.
 *
 * Label encoding
 * --------------
 *   label strings look like: "0", "1", "*0", "*2", "0'", "1''", "2@5" etc.
 *   Encoded as int16_t:
 *     bit 15 (sign)  : star flag  ("*" prefix)
 *     bits 8-14      : tile index (0..127, tile > 3 uses @N, else ' count)
 *     bits 0-7       : edge index (0..127)
 *   Decoded back to string only when needed for output.
 *
 * Build:
 *   See the nauty integration note near the nauty headers below.
 *   Short form:
 *     g++ -O3 -march=native -std=c++17 -pthread -I./nauty \
 *         main_v2.cpp ./nauty/nauty.a -o solver_zomega
 *
 * Options:
 *   --workers N       worker threads           (default: 8)
 *   --max-tiles N     max vertex-orbit types   (default: 10)
 *   --fanout N        BFS fan-out target       (default: 40000)
 *   --overcommit N    chunks / worker          (default: 32)
 *   --stack-cap N     DFS stack cap (states)   (default: 50000)
 *   --output DIR      output directory         (default: solutions/)
 *   --checkpoint DIR  checkpoint directory     (default: checkpoint/)
 */

#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <numeric>       // std::iota

// ── nauty (sparse graph canonical labelling) ─────────────────────────────────
// Build:
//   1. Download nauty from https://pallini.di.uniroma1.it/ and unpack next to
//      this file (e.g. ./nauty/).
//   2. cd nauty && ./configure && make   → produces nauty.a
//   3. Compile:
//        g++ -O3 -march=native -std=c++17 -pthread -I./nauty \
//            main_v2.cpp ./nauty/nauty.a -o solver_zomega
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {
#include "nauty.h"
#include "nausparse.h"
}

// sparsenauty() uses internal global workspace that is not re-entrant.
// All calls are serialised through this mutex; everything else in
// tiling_canonical_form_complete() (graph construction, serialisation)
// runs concurrently.
static std::mutex g_nauty_mutex;

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static std::string OUTPUT_DIR     = "solutions/";
static std::string CHECKPOINT_DIR = "checkpoint/";
static std::string LIST_FILE      = "eusolver_";
static int MAX_TILES              = 10;
static int NUM_WORKERS            = 8;
static int FANOUT_TARGET          = 40000;
static int CHUNK_OVERCOMMIT       = 32;
static int STACK_FLUSH_THRESHOLD  = 50000;

// ---------------------------------------------------------------------------
// Vertex-type tables
// ---------------------------------------------------------------------------
static const char* SYMBOL_LIST[] = {
    "(3,12,12)A","(3,12,12)F","(4,6,12)",
    "(6,6,6)S","(6,6,6)R","(6,6,6)A","(6,6,6)F",
    "(3,3,4,12)","(3,4,3,12)A","(3,4,3,12)F",
    "(3,3,6,6)A","(3,3,6,6)F",
    "(3,6,3,6)S","(3,6,3,6)R","(3,6,3,6)A1","(3,6,3,6)A2","(3,6,3,6)F",
    "(3,4,4,6)","(3,4,6,4)A","(3,4,6,4)F",
    "(4,4,4,4)S4","(4,4,4,4)R4","(4,4,4,4)S2a","(4,4,4,4)S2b",
    "(4,4,4,4)R2","(4,4,4,4)A1","(4,4,4,4)A2","(4,4,4,4)F",
    "(3,3,3,3,6)A","(3,3,3,3,6)F",
    "(3,3,3,4,4)A","(3,3,3,4,4)F",
    "(3,3,4,3,4)A","(3,3,4,3,4)F",
    "(3,3,3,3,3,3)S6","(3,3,3,3,3,3)R6",
    "(3,3,3,3,3,3)S3a","(3,3,3,3,3,3)S3b","(3,3,3,3,3,3)R3",
    "(3,3,3,3,3,3)S2","(3,3,3,3,3,3)R2",
    "(3,3,3,3,3,3)A1","(3,3,3,3,3,3)A2","(3,3,3,3,3,3)F",
};
static const char* CODE_LIST[] = {
    "3a","3b","3c","3d","3e","3f","3g",
    "4a","4b","4c","4d","4e","4f","4g","4h","4i","4j","4k","4l","4m",
    "4n","4o","4p","4q","4r","4s","4t","4u",
    "5a","5b","5c","5d","5e","5f",
    "6a","6b","6c","6d","6e","6f","6g","6h","6i","6j",
};
static const int SYMBOL_COUNT = 44;

using IVec = std::vector<int>;
using SVec = std::vector<std::string>;

static std::vector<SVec> LABEL_LIST;
static std::vector<IVec> LNEIG_LIST;
static std::vector<IVec> RNEIG_LIST;
static std::vector<IVec> MIRRO_LIST;
static std::vector<IVec> LVERT_LIST;

static void init_tables() {
    static const char* raw_labels[][13] = {
        {"0","1","*0",nullptr},
        {"0","1","2","*0","*1","*2",nullptr},
        {"0","1","2","*0","*1","*2",nullptr},
        {"0",nullptr},{"0","*0",nullptr},{"0","1","*1",nullptr},
        {"0","1","2","*0","*1","*2",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","*0","2","*2",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","1","*0","3",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","*0",nullptr},{"0","1","*0","*1",nullptr},
        {"0","1","*1","*0",nullptr},{"0","*0","2","*2",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","*0","2","*2",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0",nullptr},{"0","*0",nullptr},{"0","1",nullptr},{"0","*0",nullptr},
        {"0","1","*0","*1",nullptr},{"0","1","2","*1",nullptr},
        {"0","*0","2","*2",nullptr},
        {"0","1","2","3","*0","*1","*2","*3",nullptr},
        {"0","*0","2","3","*2",nullptr},
        {"0","1","2","3","4","*0","*1","*2","*3","*4",nullptr},
        {"0","1","*0","3","*3",nullptr},
        {"0","1","2","3","4","*0","*1","*2","*3","*4",nullptr},
        {"0","1","*1","*0","4",nullptr},
        {"0","1","2","3","4","*0","*1","*2","*3","*4",nullptr},
        {"0",nullptr},{"0","*0",nullptr},{"0","1",nullptr},{"0","*0",nullptr},
        {"0","1","*0","*1",nullptr},{"0","1","*1",nullptr},
        {"0","1","2","*0","*1","*2",nullptr},
        {"0","1","2","3","*2","*1",nullptr},
        {"0","1","2","*2","*1","*0",nullptr},
        {"0","1","2","3","4","5","*0","*1","*2","*3","*4","*5",nullptr},
    };
    static const int raw_lneig[][13] = {
        {2,0,1,-1},{2,0,1,4,5,3,-1},{2,0,1,4,5,3,-1},{0,-1},{0,1,-1},{2,0,1,-1},{2,0,1,4,5,3,-1},
        {3,0,1,2,5,6,7,4,-1},{3,0,1,2,-1},{3,0,1,2,5,6,7,4,-1},{3,0,1,2,-1},{3,0,1,2,5,6,7,4,-1},
        {1,0,-1},{1,0,3,2,-1},{3,0,1,2,-1},{3,0,1,2,-1},{3,0,1,2,5,6,7,4,-1},
        {3,0,1,2,5,6,7,4,-1},{3,0,1,2,-1},{3,0,1,2,5,6,7,4,-1},
        {0,-1},{0,1,-1},{1,0,-1},{1,0,-1},{1,0,3,2,-1},{3,0,1,2,-1},{3,0,1,2,-1},{3,0,1,2,5,6,7,4,-1},
        {4,0,1,2,3,-1},{4,0,1,2,3,6,7,8,9,5,-1},{4,0,1,2,3,-1},{4,0,1,2,3,6,7,8,9,5,-1},
        {4,0,1,2,3,-1},{4,0,1,2,3,6,7,8,9,5,-1},
        {0,-1},{0,1,-1},{1,0,-1},{1,0,-1},{1,0,3,2,-1},{2,0,1,-1},{2,0,1,4,5,3,-1},
        {5,0,1,2,3,4,-1},{5,0,1,2,3,4,-1},{5,0,1,2,3,4,7,8,9,10,11,6,-1},
    };
    static const int raw_rneig[][13] = {
        {1,2,0,-1},{1,2,0,5,3,4,-1},{1,2,0,5,3,4,-1},{0,-1},{0,1,-1},{1,2,0,-1},{1,2,0,5,3,4,-1},
        {1,2,3,0,7,4,5,6,-1},{1,2,3,0,-1},{1,2,3,0,7,4,5,6,-1},{1,2,3,0,-1},{1,2,3,0,7,4,5,6,-1},
        {1,0,-1},{1,0,3,2,-1},{1,2,3,0,-1},{1,2,3,0,-1},{1,2,3,0,7,4,5,6,-1},
        {1,2,3,0,7,4,5,6,-1},{1,2,3,0,-1},{1,2,3,0,7,4,5,6,-1},
        {0,-1},{0,1,-1},{1,0,-1},{1,0,-1},{1,0,3,2,-1},{1,2,3,0,-1},{1,2,3,0,-1},{1,2,3,0,7,4,5,6,-1},
        {1,2,3,4,0,-1},{1,2,3,4,0,9,5,6,7,8,-1},{1,2,3,4,0,-1},{1,2,3,4,0,9,5,6,7,8,-1},
        {1,2,3,4,0,-1},{1,2,3,4,0,9,5,6,7,8,-1},
        {0,-1},{0,1,-1},{1,0,-1},{1,0,-1},{1,0,3,2,-1},{1,2,0,-1},{1,2,0,5,3,4,-1},
        {1,2,3,4,5,0,-1},{1,2,3,4,5,0,-1},{1,2,3,4,5,0,11,6,7,8,9,10,-1},
    };
    static const int raw_mirro[][13] = {
        {2,1,0,-1},{3,4,5,0,1,2,-1},{3,4,5,0,1,2,-1},{0,-1},{1,0,-1},{0,2,1,-1},{3,4,5,0,1,2,-1},
        {4,5,6,7,0,1,2,3,-1},{1,0,3,2,-1},{4,5,6,7,0,1,2,3,-1},{2,1,0,3,-1},{4,5,6,7,0,1,2,3,-1},
        {1,0,-1},{2,3,0,1,-1},{3,2,1,0,-1},{1,0,3,2,-1},{4,5,6,7,0,1,2,3,-1},
        {4,5,6,7,0,1,2,3,-1},{1,0,3,2,-1},{4,5,6,7,0,1,2,3,-1},
        {0,-1},{1,0,-1},{0,1,-1},{1,0,-1},{2,3,0,1,-1},{0,3,2,1,-1},{1,0,3,2,-1},{4,5,6,7,0,1,2,3,-1},
        {1,0,4,3,2,-1},{5,6,7,8,9,0,1,2,3,4,-1},{2,1,0,4,3,-1},{5,6,7,8,9,0,1,2,3,4,-1},
        {3,2,1,0,4,-1},{5,6,7,8,9,0,1,2,3,4,-1},
        {0,-1},{1,0,-1},{0,1,-1},{1,0,-1},{2,3,0,1,-1},{0,2,1,-1},{3,4,5,0,1,2,-1},
        {0,5,4,3,2,1,-1},{5,4,3,2,1,0,-1},{6,7,8,9,10,11,0,1,2,3,4,5,-1},
    };
    static const int raw_lvert[][13] = {
        {3,12,12,-1},{3,12,12,12,12,3,-1},{4,12,6,12,6,4,-1},{6,-1},{6,6,-1},{6,6,6,-1},{6,6,6,6,6,6,-1},
        {3,12,4,3,12,4,3,3,-1},{3,12,3,4,-1},{3,12,3,4,12,3,4,3,-1},
        {3,6,6,3,-1},{3,6,6,3,6,6,3,3,-1},{3,6,-1},{3,6,6,3,-1},{3,6,3,6,-1},{3,6,3,6,-1},
        {3,6,3,6,6,3,6,3,-1},{3,6,4,4,6,4,4,3,-1},{4,6,4,3,-1},{4,6,4,3,6,4,3,4,-1},
        {4,-1},{4,4,-1},{4,4,-1},{4,4,-1},{4,4,4,4,-1},{4,4,4,4,-1},{4,4,4,4,-1},{4,4,4,4,4,4,4,4,-1},
        {3,6,3,3,3,-1},{3,6,3,3,3,6,3,3,3,3,-1},{3,4,4,3,3,-1},{3,4,4,3,3,4,4,3,3,3,-1},
        {3,4,3,4,3,-1},{3,4,3,4,3,4,3,4,3,3,-1},
        {3,-1},{3,3,-1},{3,3,-1},{3,3,-1},{3,3,3,3,-1},{3,3,3,-1},{3,3,3,3,3,3,-1},
        {3,3,3,3,3,3,-1},{3,3,3,3,3,3,-1},{3,3,3,3,3,3,3,3,3,3,3,3,-1},
    };

    LABEL_LIST.resize(SYMBOL_COUNT); LNEIG_LIST.resize(SYMBOL_COUNT);
    RNEIG_LIST.resize(SYMBOL_COUNT); MIRRO_LIST.resize(SYMBOL_COUNT);
    LVERT_LIST.resize(SYMBOL_COUNT);
    for (int s = 0; s < SYMBOL_COUNT; ++s) {
        for (int j = 0; raw_labels[s][j]; ++j) LABEL_LIST[s].push_back(raw_labels[s][j]);
        for (int j = 0; raw_lneig[s][j] != -1; ++j) LNEIG_LIST[s].push_back(raw_lneig[s][j]);
        for (int j = 0; raw_rneig[s][j] != -1; ++j) RNEIG_LIST[s].push_back(raw_rneig[s][j]);
        for (int j = 0; raw_mirro[s][j] != -1; ++j) MIRRO_LIST[s].push_back(raw_mirro[s][j]);
        for (int j = 0; raw_lvert[s][j] != -1; ++j) LVERT_LIST[s].push_back(raw_lvert[s][j]);
    }
}

static bool is_half(int sym) {
    static const std::unordered_set<std::string> H {
        "(3,12,12)F","(6,6,6)R","(3,4,3,12)F","(3,3,6,6)F",
        "(3,6,3,6)R","(3,4,6,4)F","(4,4,4,4)R4","(4,4,4,4)S2a",
        "(4,4,4,4)S2b","(4,4,4,4)A1","(3,3,3,3,6)F","(3,3,3,4,4)F",
        "(3,3,4,3,4)F","(3,3,3,3,3,3)R6","(3,3,3,3,3,3)S3a",
        "(3,3,3,3,3,3)S3b","(3,3,3,3,3,3)A1","(3,3,3,3,3,3)A2",
    };
    return H.count(SYMBOL_LIST[sym]);
}
static bool is_quarter(int sym) {
    static const std::unordered_set<std::string> Q {
        "(3,6,3,6)F","(4,4,4,4)R2","(4,4,4,4)A2","(3,3,3,3,3,3)R3"
    };
    return Q.count(SYMBOL_LIST[sym]);
}
static bool is_sixth(int sym) {
    static const std::unordered_set<std::string> S {"(6,6,6)F","(3,3,3,3,3,3)R2"};
    return S.count(SYMBOL_LIST[sym]);
}
static int ferk(int sym) {
    int q = (int)RNEIG_LIST[sym].size();
    const std::string t = SYMBOL_LIST[sym];
    if      (is_half(sym))          q /= 2;
    else if (is_quarter(sym))       q /= 4;
    else if (is_sixth(sym))         q /= 6;
    else if (t == "(4,4,4,4)F")     q /= 8;
    else if (t == "(3,3,3,3,3,3)F") q /= 12;
    return q;
}

// ---------------------------------------------------------------------------
// Label encoding
// ---------------------------------------------------------------------------
// Labels like "0", "*2", "1''", "2@5" -> int16_t:
//   bit 15       : star flag
//   bits 8-14    : tile index (0 = no tile decoration, matches Python tile arg)
//   bits 0-7     : edge index
// Python edge_label(edge_str, tile): edge_str is "0".."5" or "*0".."*5"
// We store (star, tile, edge) where edge is the numeric value of the raw label.
static int16_t encode_label(const std::string& raw_label, int tile) {
    bool star = (!raw_label.empty() && raw_label[0] == '*');
    int edge  = std::stoi(star ? raw_label.substr(1) : raw_label);
    return (int16_t)(((star ? 1 : 0) << 15) | ((tile & 0x7F) << 8) | (edge & 0xFF));
}
static std::string decode_label(int16_t v) {
    bool star = (v >> 15) & 1;
    int  tile = (v >> 8) & 0x7F;
    int  edge = v & 0xFF;
    std::string m = std::to_string(edge);
    if (tile > 3) { m += "@"; m += std::to_string(tile); }
    else          { for (int i = 0; i < tile; ++i) m += '\''; }
    if (star) m = "*" + m;
    return m;
}

// ---------------------------------------------------------------------------
// State — hot fields (used every extend_one call)
// ---------------------------------------------------------------------------
// rneig/lneig/lvert/mirro are derived from vertype, so not stored in State.
// We reconstruct them on the fly when needed, or carry them in a separate
// "expanded" view for the hot DFS loop.
//
// For the search itself we need a fully expanded view.
// We keep a StateExpanded in the DFS stack; for disk we store StateCompact.

struct State {
    IVec          rneig, lneig, lvert, mirro;  // derived, not serialised
    IVec          glue;
    std::vector<int16_t> label;
    uint8_t       num;
    std::vector<uint8_t> vertype;

    // Copy all fields directly — rneig/lneig/lvert/mirro don't change for
    // the pair loop, so we just copy them rather than rebuilding.
    // This is faster than rebuilding because rebuild_derived_into does the
    // same number of assignments but also has loop overhead and a size computation.
    State copy_essential() const {
        return *this;   // default copy: all 7 vectors copied
    }

    // Extend copy: copy existing state and append one more symbol.
    // Copies existing derived arrays directly (no full rebuild),
    // then appends only the new symbol's entries — O(sl) not O(n+sl).
    State extend_copy(int gr, int new_num) const {
        int l  = (int)glue.size();
        int sl = (int)RNEIG_LIST[gr].size();
        int total = l + sl;
        State s;
        s.num = (uint8_t)new_num;
        s.vertype.reserve(vertype.size()+1);
        s.vertype = vertype;
        s.vertype.push_back((uint8_t)gr);
        // Copy existing arrays and extend with new symbol's entries
        s.glue.resize(total, -1);
        std::copy(glue.begin(), glue.end(), s.glue.begin());
        s.label.resize(total);
        std::copy(label.begin(), label.end(), s.label.begin());
        for (int gg = 0; gg < sl; ++gg)
            s.label[l+gg] = encode_label(LABEL_LIST[gr][gg], (int)num);
        // Copy existing derived arrays, then append only new symbol's data
        s.rneig = rneig; s.rneig.resize(total);
        s.lneig = lneig; s.lneig.resize(total);
        s.lvert = lvert; s.lvert.resize(total);
        s.mirro = mirro; s.mirro.resize(total);
        for (int gg = 0; gg < sl; ++gg) {
            s.rneig[l+gg] = l + RNEIG_LIST[gr][gg];
            s.lneig[l+gg] = l + LNEIG_LIST[gr][gg];
            s.mirro[l+gg] = l + MIRRO_LIST[gr][gg];
            s.lvert[l+gg] = LVERT_LIST[gr][gg];
        }
        return s;
    }

// Shared impl: build rneig/lneig/lvert/mirro from vertype.
    static void rebuild_derived_into(State& s) {
        int total = 0;
        for (uint8_t sym : s.vertype) total += (int)RNEIG_LIST[sym].size();
        s.rneig.resize(total); s.lneig.resize(total);
        s.lvert.resize(total); s.mirro.resize(total);
        int base = 0;
        for (uint8_t sym : s.vertype) {
            int sl = (int)RNEIG_LIST[sym].size();
            for (int j = 0; j < sl; ++j) {
                s.rneig[base+j] = base + RNEIG_LIST[sym][j];
                s.lneig[base+j] = base + LNEIG_LIST[sym][j];
                s.mirro[base+j] = base + MIRRO_LIST[sym][j];
                s.lvert[base+j] = LVERT_LIST[sym][j];
            }
            base += sl;
        }
    }
};

// Free function for use during deserialisation.
static void rebuild_derived(State& s) { State::rebuild_derived_into(s); }

static State make_initial(int sym) {
    State s;
    s.num     = 1;
    s.vertype = {(uint8_t)sym};
    int sl = (int)RNEIG_LIST[sym].size();
    s.glue.assign(sl, -1);
    s.label.reserve(sl);
    for (const auto& lstr : LABEL_LIST[sym])
        s.label.push_back(encode_label(lstr, 0));
    rebuild_derived(s);
    return s;
}

// Get string label for edge i in state s
static std::string get_label(const State& s, int i) {
    return decode_label(s.label[i]);
}

// ---------------------------------------------------------------------------
// Compact serialisation
// ---------------------------------------------------------------------------
// On-disk format (raw-stream, no leading count):
//   1 byte  : num
//   1 byte  : n_edges  (can be up to ~240 for k=20, fits in uint8 up to 255)
//             Actually n_edges can exceed 255 at k=20 (12*20=240, ok)
//             But some symbols have 12 edges so max is 12*20 = 240 < 256. Safe.
//   num bytes : vertype (one uint8 per symbol)
//   n_edges * 2 bytes : glue (int16_t; -1 = 0xFFFF)
//   n_edges * 2 bytes : label (int16_t encoded)
// Total per state: 2 + num + 4*n_edges bytes
// At k=20, num=20, n_edges~120: 2+20+4*120 = 502 bytes  (vs ~4000 before)

static void write_u8(std::ostream& os, uint8_t v)  { os.write((char*)&v, 1); }
static uint8_t read_u8(std::istream& is)           { uint8_t v; is.read((char*)&v, 1); return v; }
static void write_i16(std::ostream& os, int16_t v) { os.write((char*)&v, 2); }
static int16_t read_i16(std::istream& is)          { int16_t v; is.read((char*)&v, 2); return v; }
static void write_i32(std::ostream& os, int32_t v) { os.write((char*)&v, 4); }
static int32_t read_i32(std::istream& is)          { int32_t v; is.read((char*)&v, 4); return v; }

static void write_state(std::ostream& os, const State& s) {
    int ne = (int)s.glue.size();
    write_u8(os, s.num);
    write_u8(os, (uint8_t)ne);
    for (uint8_t v : s.vertype) write_u8(os, v);
    for (int g : s.glue)        write_i16(os, (int16_t)g);
    for (int16_t l : s.label)   write_i16(os, l);
}
static State read_state(std::istream& is) {
    State s;
    s.num    = read_u8(is);
    int ne   = (int)read_u8(is);
    s.vertype.resize(s.num);
    for (auto& v : s.vertype) v = read_u8(is);
    s.glue.resize(ne);
    for (int& g : s.glue)        g = (int)(int16_t)read_i16(is);
    s.label.resize(ne);
    for (int16_t& l : s.label)   l = read_i16(is);
    rebuild_derived(s);
    return s;
}

// Write vector with leading count (frontier / early checkpoint files).
static void write_states(const std::string& path, const std::vector<State>& v) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary);
      write_i32(f, (int32_t)v.size());
      for (const auto& s : v) write_state(f, s); }
    fs::rename(tmp, path);
}
static std::vector<State> read_states(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    int32_t n = read_i32(f); std::vector<State> v(n);
    for (auto& s : v) s = read_state(f); return v;
}
// Stream states from a raw-append file (no leading count), O(1) RAM.
template<class F>
static int64_t stream_states(const std::string& path, F&& cb) {
    std::ifstream f(path, std::ios::binary);
    int64_t count = 0;
    while (f.good() && f.peek() != EOF) {
        try { cb(read_state(f)); ++count; } catch (...) { break; }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Validity check
// ---------------------------------------------------------------------------
static bool check_partial(const IVec& rneig, const IVec& lneig,
                           const IVec& lvert, const IVec&,
                           const IVec& glue,
                           int /*r0*/=-1, int /*r1*/=-1,
                           int /*r2*/=-1, int /*r3*/=-1) {
    int n = (int)rneig.size();
    for (int i = 0; i < n; ++i) {
        int rfree=rneig[i], mv=lvert[rfree], cnt=1, fe=glue[rfree];
        while (true) {
            if (fe==-1) { if (cnt>mv) return false; break; }
            if (fe==i)  { if (mv%cnt!=0) return false; break; }
            rfree=rneig[fe]; if (lvert[rfree]!=mv) return false;
            ++cnt; fe=glue[rfree];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Constraint propagation
// ---------------------------------------------------------------------------
// After a gluing, walk every open cycle. If a cycle has exactly one free edge
// remaining, that edge's partner is fully forced — glue it immediately.
// Repeat until no more forced gluings exist (fixpoint).
// Returns false if a contradiction is found (forced partner already taken,
// or polygon size violated).
static bool propagate_forced(IVec& glue,
                              const IVec& rneig, const IVec& lneig,
                              const IVec& lvert, const IVec& mirro) {
    int n=(int)glue.size();
    bool any=true;
    while(any){
        any=false;
        for(int root=0;root<n;++root){
            if(glue[root]!=-1) continue;  // already glued
            // Walk the polygon ring starting from root.
            // rneig[root] is the "right" edge of root's polygon.
            // We follow: right -> glue[right] (next left) -> rneig[next left] -> ...
            // until we return to root or hit a second free edge.
            int right=rneig[root];
            int mv=lvert[right];
            int cnt=1;           // count steps including root
            bool contradiction=false;
            int forced_other=-1; // the edge that must glue to root, if forced
            // Walk from root's right edge forward around the polygon.
            int cur_left=root;
            for(;;){
                int r=rneig[cur_left];
                if(lvert[r]!=mv){contradiction=true;break;}
                int next_left=glue[r];
                if(next_left==-1){
                    // r is a free right edge — its paired left edge (next_left)
                    // is unknown.  But we are walking from root's side:
                    // if cur_left == root we already counted root.
                    // Actually: rneig[cur_left]=r, and glue[r]==-1 means
                    // r's partner is free.  But root's partner is also free.
                    // The polygon goes: root --poly--> ... --poly--> ???
                    // We cannot continue — there is >1 free slot.
                    forced_other=-2; // >1 free
                    break;
                }
                if(next_left==root){
                    // Closed cycle with no free slot besides root itself.
                    // Root is the only free slot, so it must close back.
                    // The polygon has cnt steps.
                    if(mv % cnt != 0){contradiction=true;}
                    else forced_other=root; // self-close? only valid if root==?
                    // Actually "root closes to itself" is impossible (glue[root]=root
                    // is not a valid state). So this means the polygon is already
                    // fully closed except for root, and root must glue to the edge
                    // that feeds back into root. That edge is... we track it below.
                    break;
                }
                // next_left is a connected left edge; find the right edge continuing the ring.
                // The ring continues: next_left -> rneig[next_left] -> glue[rneig[next_left]] -> ...
                cur_left = lneig[next_left];
                // Hmm, the ring traversal: from left edge e, we go to rneig[e] (right edge),
                // then glue[rneig[e]] (the left edge of the next pair), then repeat.
                // But we need lneig to navigate around the polygon.
                // Actually the correct ring walk is:
                //   start at left edge `cur_left`
                //   its right edge is rneig[cur_left]
                //   the glue of rneig[cur_left] tells us the next left edge
                // So: cur_left = glue[rneig[cur_left]]  (if not -1)
                // Let me redo this cleanly.
                ++cnt;
                cur_left = next_left;
                if(cnt > mv){contradiction=true;break;}  // overshot
                // Prevent infinite loop
                if(cnt > n){contradiction=true;break;}
            }
            if(contradiction) return false;
            if(forced_other==-2) continue; // >1 free, not forced
            if(forced_other!=root) continue; // didn't reach a forced conclusion yet
            // forced_other==root: the polygon needs to be closed.
            // Find the edge e such that gluing root to e closes the ring.
            // Re-walk from root to find the last right edge before returning.
            // The "partner" of root is the left edge that directly feeds into root
            // from the other side of the polygon.
            // Walk the ring: right1 = rneig[root], left2 = glue[right1], right2 = rneig[left2], ...
            // The last left edge before we'd return to root is the forced partner.
            int partner=-1;
            {
                int cl=root; int steps=0;
                while(steps<cnt-1){
                    int r=rneig[cl];
                    int nl=glue[r]; if(nl==-1||nl==root){partner=cl;break;}
                    cl=nl; ++steps;
                    if(steps==cnt-1){partner=cl;break;}
                }
            }
            if(partner<0||partner>=n) continue;
            if(glue[partner]!=-1) return false; // contradiction
            bool root_mir=(mirro[root]==root);
            bool part_mir=(mirro[partner]==partner);
            if(root_mir!=part_mir) return false;
            // Apply forced gluing
            glue[root]=partner; glue[partner]=root;
            if(!root_mir){
                int mr=mirro[root], mp=mirro[partner];
                if(glue[mr]!=-1&&glue[mr]!=mp) return false;
                if(glue[mp]!=-1&&glue[mp]!=mr) return false;
                glue[mr]=mp; glue[mp]=mr;
            }
            any=true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Core search step
// ---------------------------------------------------------------------------
// thread-local seen buffer: avoids per-call heap alloc
static thread_local uint8_t tl_seen_buf[256];
static thread_local int     tl_seen_prev=0;

static void extend_one(const State& st,
                        std::vector<State>& complete,
                        std::vector<State>& partial) {
    const auto& rneig=st.rneig; const auto& lneig=st.lneig;
    const auto& lvert=st.lvert; const auto& mirro=st.mirro;
    const auto& glue =st.glue;  const auto& label=st.label;
    int num=st.num; const auto& vtype=st.vertype;
    int n=(int)rneig.size();

    // Zero only bytes used in the last call (amortised O(1))
    std::memset(tl_seen_buf, 0, tl_seen_prev < n ? n : tl_seen_prev);
    tl_seen_prev = n;
    uint8_t* seen = tl_seen_buf;

    int mcs=-1, mck=13;
    for (int cy=0; cy<n; ++cy) {
        if (seen[cy]) continue;
        int left=cy;
        while (glue[left]!=-1 && glue[left]!=rneig[cy]) left=lneig[glue[left]];
        if (glue[left]==-1) {
            int stable=left, right=rneig[left], vs=lvert[right], cnt=0;
            while (true) {
                seen[left]=1; ++cnt; left=glue[right];
                if (left==-1){int sl=vs-cnt;if(sl<mck){mcs=stable;mck=sl;}break;}
                right=rneig[left];
            }
        } else {
            left=cy; int right=rneig[left];
            while(true){seen[left]=1;left=glue[right];if(left==cy)break;right=rneig[left];}
        }
    }

    int ff=mcs;
    if (label[ff] < 0) ff = mirro[ff];  // negative int16 = star flag set
    bool mirrored=(mirro[ff]==ff);

    auto finish=[&](State&& s){
        if(!propagate_forced(s.glue,s.rneig,s.lneig,s.lvert,s.mirro)) return;
        bool done=true;
        for(int g:s.glue)if(g==-1){done=false;break;}
        (done?complete:partial).push_back(std::move(s));
    };

    // Pair ff with existing free edge.
    // Use tl_pg as a scratch glue buffer for validity checks (no alloc per trial).
    // Only call copy_essential() for accepted candidates — pays copy cost once
    // per accepted pair rather than once per tried pair.
    {
        static thread_local IVec tl_pg;
        if((int)tl_pg.size()<n) tl_pg.resize(n);
        std::copy(glue.begin(), glue.end(), tl_pg.begin());
        for (int i=0; i<n; ++i) {
            if (glue[i]!=-1) continue;
            if ((mirro[i]==i)!=mirrored) continue;
            int mi=-1,mib=-1;
            tl_pg[ff]=i; tl_pg[i]=ff;
            if(!mirrored){mi=mirro[ff];mib=mirro[i];tl_pg[mi]=mib;tl_pg[mib]=mi;}
            bool ok=check_partial(rneig,lneig,lvert,mirro,tl_pg);
            tl_pg[ff]=-1; tl_pg[i]=-1;
            if(!mirrored){tl_pg[mi]=-1;tl_pg[mib]=-1;}
            if(!ok) continue;
            // Accepted: pay copy cost once (copy_essential skips rneig/lneig/lvert/mirro)
            State s2=st.copy_essential();
            s2.glue[ff]=i; s2.glue[i]=ff;
            if(!mirrored){s2.glue[mi]=mib;s2.glue[mib]=mi;}
            finish(std::move(s2));
        }
    }

    // Attach new vertex.
    // extend_copy(gr) builds s2 cheaply: copies only glue+label+vertype, extends
    // them for gr, rebuilds derived — 4x less data than a full State copy.
    // tl_gt is a scratch glue buffer for validity checks (no alloc per trial).
    if (num < MAX_TILES) {
        static thread_local IVec tl_gt;
        for (int gr=vtype[0]; gr<SYMBOL_COUNT; ++gr) {
            int l=n, sl=(int)RNEIG_LIST[gr].size();
            int total=l+sl;

            // Build scratch glue: existing + -1 for new slots
            if((int)tl_gt.size()<total) tl_gt.resize(total);
            std::copy(glue.begin(), glue.end(), tl_gt.begin());
            std::fill(tl_gt.begin()+n, tl_gt.begin()+total, -1);

            // We need s2's mirro to check candidates. Build it once via extend_copy
            // only if at least one fk candidate passes the mirro filter — or just
            // build it once per gr (it's cheap via extend_copy).
            // extend_copy is the key optimisation: ~4x cheaper than full State copy.
            State s2 = st.extend_copy(gr, num+1);

            int fk=ferk(gr);
            bool any_valid = false;
            for (int i=l;i<l+fk;++i) {
                if ((s2.mirro[i]==i)!=mirrored) continue;
                int mi2=-1,mi2b=-1;
                tl_gt[ff]=i; tl_gt[i]=ff;
                if(!mirrored){mi2=s2.mirro[ff];mi2b=s2.mirro[i];tl_gt[mi2]=mi2b;tl_gt[mi2b]=mi2;}
                bool ok=check_partial(s2.rneig,s2.lneig,s2.lvert,s2.mirro,tl_gt);
                tl_gt[ff]=-1; tl_gt[i]=-1;
                if(!mirrored){tl_gt[mi2]=-1;tl_gt[mi2b]=-1;}
                if(!ok) continue;
                any_valid = true;
                // copy_essential for s3 is cheap: only copies glue+label+vertype
                State s3=s2.copy_essential();
                s3.glue[ff]=i; s3.glue[i]=ff;
                if(!mirrored){s3.glue[mi2]=mi2b;s3.glue[mi2b]=mi2;}
                finish(std::move(s3));
            }
            (void)any_valid;
        }
    }
}

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------
// Bitset helpers for is_simplest / are_isomorphic
// max edges = 12 * 20 = 240 → 4 uint64 words
// max 2*edges = 480 → 8 uint64 words
// ---------------------------------------------------------------------------
static constexpr int BS_N  = 4;   // words for le ≤ 240
static constexpr int BS2_N = 8;   // words for 2*le ≤ 480
using BS  = std::array<uint64_t,BS_N>;
using BS2 = std::array<uint64_t,BS2_N>;

static inline void bs_set (BS&  b,int i){b[i>>6]|= (1ULL<<(i&63));}
static inline void bs_set2(BS2& b,int i){b[i>>6]|= (1ULL<<(i&63));}
static inline bool bs_get (const BS&  b,int i){return (b[i>>6]>>(i&63))&1;}
static inline bool bs_get2(const BS2& b,int i){return (b[i>>6]>>(i&63))&1;}
static inline bool bs_singleton(const BS& b,int n){
    int c=0; for(int w=0;w<(n+63)/64;++w)c+=__builtin_popcountll(b[w]); return c==1;
}
static inline bool bs2_plural(const BS2& b,int n){
    int c=0; for(int w=0;w<(n+63)/64;++w){c+=__builtin_popcountll(b[w]);if(c>1)return true;} return false;
}

// ---------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(int n) : stop_(false), active_(0) {
        for(int i=0;i<n;++i) workers_.emplace_back([this]{loop();});
    }
    ~ThreadPool() {
        {std::unique_lock lk(mu_);stop_=true;}cv_.notify_all();
        for(auto& t:workers_)t.join();
    }
    template<class F> void enqueue(F&& f){
        {std::unique_lock lk(mu_);tasks_.push(std::forward<F>(f));}cv_.notify_one();
    }
    void wait_all(){
        std::unique_lock lk(mu_);
        done_cv_.wait(lk,[&]{return tasks_.empty()&&active_==0;});
    }
private:
    void loop(){
        // Pre-warm ALL thread-local storage to pay the TLS init guard once per
        // thread instead of on the first access inside a hot loop.
        { (void)tl_seen_buf[0]; (void)tl_seen_prev; }
        // Force init of the static thread_local vectors in extend_one
        // and is_simplest / are_isomorphic by touching them here.
        // We use a dummy lambda scope to access file-scope TLS.
        []{
            static thread_local IVec _pg, _gt;
            _pg.reserve(0); _gt.reserve(0);
        }();
        while(true){
            std::function<void()> t;
            {std::unique_lock lk(mu_);cv_.wait(lk,[&]{return stop_||!tasks_.empty();});
             if(stop_&&tasks_.empty())return;
             t=std::move(tasks_.front());tasks_.pop();++active_;}
            t();
            {std::unique_lock lk(mu_);--active_;}done_cv_.notify_one();
        }
    }
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_; std::condition_variable cv_,done_cv_;
    bool stop_; int active_;
};

// Forward declarations needed by ChunkDedup
static bool is_simplest(const State& sol);
static uint64_t solution_fingerprint(const State& sol);
static std::string make_signature(const std::vector<uint8_t>& vertype);
static bool are_isomorphic(const State& a, const State& b);
static bool are_isomorphic_base(const State& a, const State& b);

// ---------------------------------------------------------------------------
// Within-chunk deduplication cache
//
// Applied eagerly inside the DFS worker before any solution touches disk.
// Two levels of filtering, cheapest first:
//
//   Level 1 — is_simplest()
//     Pure per-solution predicate, zero cross-solution cost.
//     Rejects ~60-80% of raw solutions at k>=6, so most solutions
//     never reach level 2 or disk at all.
//
//   Level 2 — fingerprint-bucketed are_isomorphic() within each signature
//     Groups kept solutions by (signature -> fingerprint -> list).
//     A new solution is accepted only if no existing kept solution with
//     the same fingerprint is isomorphic to it.
//     False negatives are impossible: anything we miss here is caught
//     by the existing pass-2 cross-chunk dedup.
//     Cache RAM is O(unique_per_chunk) — negligible vs raw count.
// ---------------------------------------------------------------------------
// Signature key from vertype histogram — no string allocation
static inline uint64_t vertype_key(const std::vector<uint8_t>& vt){
    uint8_t h[SYMBOL_COUNT]={};
    for(uint8_t v:vt) h[v]++;
    uint64_t r=14695981039346656037ULL;
    for(int i=0;i<SYMBOL_COUNT;++i){r^=h[i];r*=1099511628211ULL;}
    return r;
}

// ---------------------------------------------------------------------------
// PartialDedup — eliminate isomorphic partial states from the DFS stack
//
// Two partial states are equivalent if they represent the same connectivity
// pattern (same tiling fragment up to relabeling of edge slots). Expanding
// equivalent states produces identical subtrees — pure wasted work.
//
// Implementation:
//   fingerprint → list of states (bounded: at most ~few per fingerprint bucket)
//   try_insert(s): if s is isomorphic to a stored state, return false (discard).
//                  otherwise store s and return true (keep).
//
// The fingerprint (state_fingerprint) is a strong invariant: non-isomorphic
// states almost always have different fingerprints, so the isomorphism check
// is rarely needed. The filter is cheap enough to run on every partial state.
//
// Soundness: we never discard a state that isn't genuinely isomorphic to one
// already in the set. False positives (hash collision) would discard a unique
// state but are ~1/2^64 per comparison — negligible.
// ---------------------------------------------------------------------------
static uint64_t state_fingerprint(const State& sol);  // forward decl
static std::string tiling_canonical_form_complete(const State& sol);
static int PDEDUP_CAP = 20000;

struct PartialDedup {
    std::unordered_map<uint64_t, std::vector<State>> cache;
    int64_t n_seen=0, n_deduped=0, n_stored=0;

    bool try_insert(const State& s) {
        ++n_seen;
        if(s.num >= MAX_TILES-1) return true;
        uint64_t fp = state_fingerprint(s);
        auto& bucket = cache[fp];
        for (const auto& kept : bucket)
            if (are_isomorphic(s, kept)) { ++n_deduped; return false; }
        if(n_stored < PDEDUP_CAP) { bucket.push_back(s); ++n_stored; }
        return true;
    }

    void clear() { cache.clear(); n_seen=0; n_deduped=0; n_stored=0; }
    double dedup_rate() const {
        return n_seen > 0 ? (double)n_deduped / n_seen : 0.0;
    }
};

struct ChunkDedup {
    // canonical-form string -> seen flag.
    // nauty guarantees identical strings iff complete states are isomorphic,
    // so a hash-set replaces the previous sig->fp->list+are_isomorphic scheme.
    // are_isomorphic is still used for partial states (PartialDedup, GlobalPartialDedup).
    std::unordered_set<std::string> canon_seen;
    int64_t n_raw=0, n_simplest=0, n_unique=0;

    bool try_insert(State& sol){
        ++n_raw;
        if(!is_simplest(sol)) return false;
        ++n_simplest;
        std::string canon = tiling_canonical_form_complete(sol);
        if(!canon_seen.insert(canon).second) return false;
        ++n_unique;
        return true;
    }
};

// ---------------------------------------------------------------------------
// DFS worker — streams pre-deduped solutions to disk, spills stack if needed
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Global shared partial-state dedup (Item 6)
// Sharded to minimize lock contention. All workers share this.
// ---------------------------------------------------------------------------
static constexpr int GPDEDUP_SHARDS = 64;
struct GlobalPartialDedup {
    struct Shard {
        std::mutex mu;
        std::unordered_map<uint64_t, std::vector<State>> cache;
        int64_t stored=0;
    };
    std::array<Shard,GPDEDUP_SHARDS> shards;
    std::atomic<int64_t> n_seen{0}, n_deduped{0};
    int cap_per_shard=0;

    void set_cap(int total){ cap_per_shard=std::max(1,total/GPDEDUP_SHARDS); }

    bool try_insert(const State& s){
        n_seen.fetch_add(1,std::memory_order_relaxed);
        if(s.num>=MAX_TILES-1) return true;
        uint64_t fp=state_fingerprint(s);
        Shard& sh=shards[fp%GPDEDUP_SHARDS];
        std::lock_guard<std::mutex> lk(sh.mu);
        auto& bucket=sh.cache[fp];
        for(const auto& kept:bucket)
            if(are_isomorphic(s,kept)){
                n_deduped.fetch_add(1,std::memory_order_relaxed);
                return false;
            }
        if(cap_per_shard==0||sh.stored<cap_per_shard){bucket.push_back(s);++sh.stored;}
        return true;
    }
    double dedup_rate()const{
        int64_t s=n_seen.load(),d=n_deduped.load();
        return s>0?(double)d/s:0.0;
    }
    void reset(){
        for(auto& sh:shards){std::lock_guard<std::mutex> lk(sh.mu);sh.cache.clear();sh.stored=0;}
        n_seen=0;n_deduped=0;
    }
};
static GlobalPartialDedup g_pdedup;

static std::tuple<int64_t,int64_t,int64_t,double> dfs_worker_to_file(std::vector<State> items,
                                                       const std::string& out_path) {
    std::string tmp_path   = out_path + ".tmp";
    std::string spill_path = out_path + ".spill";
    std::ofstream out_f(tmp_path, std::ios::binary);

    std::vector<State> stack = std::move(items);
    std::vector<State> comp, part;
    bool    spill_has_data = false;
    int64_t spill_read_pos = 0;
    ChunkDedup  dedup;

    auto spill_flush = [&]() {
        int fc=(int)stack.size()/2; if(fc==0)return;
        std::ofstream sf(spill_path,
            std::ios::binary|(spill_has_data?std::ios::app:std::ios::out));
        write_i32(sf,fc);
        for(int i=0;i<fc;++i) write_state(sf,stack[i]);
        sf.close();
        stack.erase(stack.begin(),stack.begin()+fc);
        spill_has_data=true;
    };
    auto spill_reload = [&]()->bool {
        std::ifstream sf(spill_path,std::ios::binary);
        if(!sf)return false;
        sf.seekg(spill_read_pos);
        if(sf.peek()==EOF)return false;
        int32_t batch=read_i32(sf); if(sf.fail()||batch<=0)return false;
        std::vector<State> buf(batch);
        for(auto& s:buf)s=read_state(sf);
        spill_read_pos=(int64_t)sf.tellg();
        stack.insert(stack.begin(),buf.begin(),buf.end());
        return true;
    };

    while(true){
        if(stack.empty()){if(spill_has_data&&spill_reload())continue;break;}
        State st=std::move(stack.back()); stack.pop_back();
        comp.clear(); part.clear();
        extend_one(st,comp,part);
        for(auto& s:comp)
            if(dedup.try_insert(s)) write_state(out_f,s);
        for(auto& s:part)
            if(g_pdedup.try_insert(s)) stack.push_back(std::move(s));
        if((int)stack.size()>STACK_FLUSH_THRESHOLD) spill_flush();
    }

    out_f.close();
    if(spill_has_data) fs::remove(spill_path);
    fs::rename(tmp_path,out_path);
    return {dedup.n_raw, dedup.n_simplest, dedup.n_unique, g_pdedup.dedup_rate()};
}

// ---------------------------------------------------------------------------
// Checkpoint
// ---------------------------------------------------------------------------
static std::string ckpt_path(const std::string& name){return CHECKPOINT_DIR+name;}
static std::string chunk_file(int ci){
    char buf[32]; snprintf(buf,sizeof(buf),"chunk_%05d.bin",ci);
    return ckpt_path(buf);
}

struct Progress {
    std::set<int> completed;
    int max_tiles,fanout_target,num_workers,chunk_overcommit;
};
static void save_progress(const Progress& p){
    std::string tmp=ckpt_path("progress.bin")+".tmp";
    {std::ofstream f(tmp,std::ios::binary);
     write_i32(f,p.max_tiles);write_i32(f,p.fanout_target);
     write_i32(f,p.num_workers);write_i32(f,p.chunk_overcommit);
     write_i32(f,(int)p.completed.size());
     for(int c:p.completed)write_i32(f,c);}
    fs::rename(tmp,ckpt_path("progress.bin"));
}
static bool load_progress(Progress& p){
    std::string path=ckpt_path("progress.bin");
    if(!fs::exists(path))return false;
    std::ifstream f(path,std::ios::binary);
    p.max_tiles=read_i32(f);p.fanout_target=read_i32(f);
    p.num_workers=read_i32(f);p.chunk_overcommit=read_i32(f);
    int n=read_i32(f); for(int i=0;i<n;++i) p.completed.insert(read_i32(f));
    return true;
}

// ---------------------------------------------------------------------------
// Time / ETA
// ---------------------------------------------------------------------------
static double now_s(){
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
static std::string fmt_duration(double s){
    int si=(int)s;
    if(si<60){char b[32];snprintf(b,32,"%ds",si);return b;}
    if(si<3600){char b[32];snprintf(b,32,"%dm %02ds",si/60,si%60);return b;}
    char b[32];snprintf(b,32,"%dh %02dm",si/3600,(si%3600)/60);return b;
}
struct PhaseETA {
    int total,done; double alpha,ema_spu,t0;
    PhaseETA(int total,int done=0,double alpha=0.2)
        :total(total),done(done),alpha(alpha),ema_spu(-1),t0(now_s()){}
    void update(int units,double elapsed){
        if(units>0){double spu=elapsed/units;ema_spu=(ema_spu<0)?spu:alpha*spu+(1-alpha)*ema_spu;}
        done+=units;
    }
    double elapsed()const{return now_s()-t0;}
    std::string progress_line(const std::string& label)const{
        double pct=total?100.0*done/total:0;
        std::ostringstream o;
        o<<"  ["<<label<<"]  "<<done<<"/"<<total<<" ("<<(int)pct<<"%)"
         <<"  |  elapsed "<<fmt_duration(elapsed());
        if(ema_spu>=0&&done<total)o<<"  |  ETA ~"<<fmt_duration(ema_spu*(total-done));
        else o<<"  |  ETA --";
        return o.str();
    }
};

// ---------------------------------------------------------------------------
// Phase 1: BFS fan-out
// ---------------------------------------------------------------------------
static std::vector<State> make_initial_states(){
    std::vector<State> v; v.reserve(SYMBOL_COUNT);
    for(int i=0;i<SYMBOL_COUNT;++i) v.push_back(make_initial(i));
    return v;
}

static std::pair<std::vector<State>,std::vector<State>> fan_out(int target){
    std::vector<State> frontier=make_initial_states();
    std::vector<State> early;
    int steps=0, frontier_deduped=0; double t0=now_s();
    printf("Phase 1 -- fan-out to >=%d items (BFS)...\n",target);
    std::vector<State> comp,part;
    // PartialDedup on the frontier prevents isomorphic subtrees from
    // being explored during the BFS expansion. This is the highest-leverage
    // point for partial dedup: duplicates caught here prevent entire subtrees
    // from being distributed to workers.
    PartialDedup pdedup;
    size_t head=0;
    while((int)(frontier.size()-head)<target&&head<frontier.size()){
        State st=std::move(frontier[head++]);
        comp.clear();part.clear();
        extend_one(st,comp,part);
        for(auto& s:comp)early.push_back(std::move(s));
        for(auto& s:part){
            if(pdedup.try_insert(s)) frontier.push_back(std::move(s));
            else ++frontier_deduped;
        }
        ++steps;
        if(steps%500==0){
            printf("  step %d: frontier=%d, early=%d, deduped=%d\r",
                   steps,(int)(frontier.size()-head),(int)early.size(),frontier_deduped);
            fflush(stdout);
        }
    }
    if(head>0)frontier.erase(frontier.begin(),frontier.begin()+head);
    printf("\n  done: %d steps, frontier=%d, early=%d, deduped=%d (%.0f%%), elapsed=%.1fs\n",
           steps,(int)frontier.size(),(int)early.size(),frontier_deduped,
           pdedup.dedup_rate()*100.0,now_s()-t0);
    return {frontier,early};
}

// ---------------------------------------------------------------------------
// Deduplication
// ---------------------------------------------------------------------------
// are_isomorphic works for both complete and partial states.
// Free edges (glue[i]=-1) are mapped to a sentinel node index=le2
// so that alias propagation handles them correctly:
//   - two free edges can map to each other only if all other constraints agree
//   - a free edge cannot map to a connected edge (lvert check catches this
//     since the sentinel node has lv=FREE_SENTINEL=-999)
// ---------------------------------------------------------------------------
// are_isomorphic — direct port of Python comparesolutions().
//
// SEMANTICS (verified by experiment):
//   Python comparesolutions() returns `nun` = (False in unique).
//   unique[i] is set True when len(alias[i]) == 1, i.e. the node is forced
//   to map only to itself (no valid cross-half partner remaining).
//
//   For ISOMORPHIC states: constraints don't force self-mapping; alias sets
//   stay large (contain cross-half candidates) → unique[i] stays False →
//   nun = True → comparesolutions returns True → are_isomorphic returns TRUE.
//
//   For NON-ISOMORPHIC states: constraints eliminate all cross-half candidates;
//   alias[i] shrinks to {i} (self only) → unique[i] = True → nun = False →
//   comparesolutions returns False → are_isomorphic returns FALSE.
//
//   So: are_isomorphic = True iff NOT all alias sets are singletons.
//       = True iff any alias[i] (i < le, A-half) still has a B-half candidate.
//
// Free-edge handling: Python's comparesolutions is only called on complete
// solutions (no glue[i]==-1).  For partial states we must handle free edges.
// We map A's free edges to sentinel sa=le2 and B's to sentinel sb=le2+1.
// Sentinels have a unique lv value so they can only alias each other, meaning
// free edges can only map to free edges — correct semantics.
// ---------------------------------------------------------------------------
static bool are_isomorphic(const State& a, const State& b){
    int le=(int)a.rneig.size();
    if((int)b.rneig.size()!=le) return false;
    int le2 = 2*le;
    int sa  = le2;      // sentinel for A's free edges  (glue==-1)
    int sb  = le2+1;    // sentinel for B's free edges
    int tot = le2+2;

    // ── merged arrays ───────────────────────────────────────────────────────
    static thread_local std::vector<int> rn,ln,mo,gl,lv;
    rn.resize(tot); ln.resize(tot); mo.resize(tot);
    gl.resize(tot); lv.resize(tot);

    for(int i=0;i<le;++i){
        rn[i]    = a.rneig[i];
        ln[i]    = a.lneig[i];
        mo[i]    = a.mirro[i];
        gl[i]    = (a.glue[i]==-1) ? sa : a.glue[i];
        lv[i]    = a.lvert[i];
        rn[le+i] = le + b.rneig[i];
        ln[le+i] = le + b.lneig[i];
        mo[le+i] = le + b.mirro[i];
        gl[le+i] = (b.glue[i]==-1) ? sb : le+b.glue[i];
        lv[le+i] = b.lvert[i];
    }
    static constexpr int LV_SENT = -999;
    rn[sa]=sa; ln[sa]=sa; mo[sa]=sa; gl[sa]=sa; lv[sa]=LV_SENT;
    rn[sb]=sb; ln[sb]=sb; mo[sb]=sb; gl[sb]=sb; lv[sb]=LV_SENT;

    // ── alias sets ───────────────────────────────────────────────────────────
    // alias[i] = set of nodes j that i might map to.
    // A-nodes (0..le-1): initial = B-nodes (le..le2-1) ∪ {i}
    // B-nodes (le..le2-1): initial = A-nodes (0..le-1) ∪ {i}
    // Sentinels: alias[sa]={sb}, alias[sb]={sa}  (can only map to each other)
    static thread_local std::vector<BS2> alias;
    if((int)alias.size()<tot) alias.resize(tot);
    for(int i=0;i<tot;++i) alias[i]={};

    for(int i=0;i<le;++i){
        for(int j=le;j<le2;++j) bs_set2(alias[i],j);
        bs_set2(alias[i],i);
    }
    for(int i=le;i<le2;++i){
        for(int j=0;j<le;++j) bs_set2(alias[i],j);
        bs_set2(alias[i],i);
    }
    bs_set2(alias[sa],sb);
    bs_set2(alias[sb],sa);



    // ── constraint propagation — direct port of Python's while/for loop ──────
    // Python: "while change: for i in range(le*2): for j in copy(alias[i]): ..."
    // A worklist is INCORRECT here: when alias[i] loses candidate j, any node k
    // with rneig[k]=i (or lneig/mirro/glue[k]=i) must also be re-examined, but
    // those nodes are NOT j.  The only safe translation is the direct one:
    // repeat a full pass over all le2 nodes until nothing changes.
    int fnw = (tot+63)/64;
    bool change = true;
    while(change){
        change=false;
        for(int i=0;i<le2;++i){
            for(int w=0;w<fnw;++w){
                uint64_t bits=alias[i][w], mask_rm=0;
                while(bits){
                    int bit=__builtin_ctzll(bits); bits&=bits-1;
                    int j=w*64+bit;
                    if(j==i) continue;
                    if(lv[i]!=lv[j]
                    || !bs_get2(alias[j],i)
                    || !bs_get2(alias[mo[i]],mo[j])
                    || !bs_get2(alias[gl[i]],gl[j])
                    || !bs_get2(alias[rn[i]],rn[j])
                    || !bs_get2(alias[ln[i]],ln[j]))
                        mask_rm|=(1ULL<<bit);
                }
                if(mask_rm){ alias[i][w]&=~mask_rm; change=true; }
            }
        }
    }

    // ── result ───────────────────────────────────────────────────────────────
    // ISOMORPHIC  ↔  NOT all alias sets are singletons
    //             ↔  some alias[i] for i<le still has a B-half candidate
    //             ↔  some alias[i].size() > 1  (the self-bit + at least one cross-bit)
    //
    // If every alias[i] is a singleton {i}, no cross-mapping survived →
    // states are non-isomorphic → return false.
    // Checking A-half only is sufficient; B-half is symmetric.
    for(int i=0;i<le;++i){
        int cnt=0;
        for(int w=0;w<fnw;++w) cnt+=__builtin_popcountll(alias[i][w]);
        if(cnt>1) return true;   // still has cross-half candidate → isomorphic
    }
    return false;  // all A-nodes reduced to self-only → non-isomorphic
}

#include <vector>
#include <unordered_set>
#include <string>
#include <iostream>

static bool are_isomorphic_base(const State& a, const State& b) {
    const int n = (int)a.rneig.size();
    if ((int)b.rneig.size() != n) return false;

    const int N = 2 * n;

    // --- alias sets ---
    std::vector<std::unordered_set<int>> alias(N);
    std::vector<bool> unique(N, false);

    std::unordered_set<int> fullset1, fullset2;

    for (int i = 0; i < n; ++i) {
        fullset1.insert(n + i);
        fullset2.insert(i);
    }

    // First half
    for (int i = 0; i < n; ++i) {
        alias[i] = fullset1;
        alias[i].insert(i);
    }

    // Second half
    for (int i = 0; i < n; ++i) {
        alias[n + i] = fullset2;
        alias[n + i].insert(n + i);
    }

    // --- merged arrays (equivalent to Python concatenation) ---
    std::vector<int> rneig(2 * n), lneig(2 * n), lvert(2 * n), mirro(2 * n);
    std::vector<int> glue(2 * n);
    std::vector<int16_t> label(2 * n);

    // Copy A
    for (int i = 0; i < n; ++i) {
        rneig[i] = a.rneig[i];
        lneig[i] = a.lneig[i];
        mirro[i] = a.mirro[i];
        glue[i]  = a.glue[i];
        lvert[i] = a.lvert[i];
        label[i] = a.label[i];
    }

    // Copy B with offset
    for (int i = 0; i < n; ++i) {
        rneig[n + i] = n + b.rneig[i];
        lneig[n + i] = n + b.lneig[i];
        mirro[n + i] = n + b.mirro[i];
        glue[n + i]  = n + b.glue[i];
        lvert[n + i] = b.lvert[i];
        label[n + i] = b.label[i];
    }

    // --- constraint propagation ---
    bool change = true;
    while (change) {
        change = false;

        for (int i = 0; i < N; ++i) {
            std::vector<int> to_remove;

            for (int j : alias[i]) {

                if (lvert[i] != lvert[j]) {
                    to_remove.push_back(j);
                }
                else if (!alias[j].count(i)) {
                    to_remove.push_back(j);
                }
                else if (!alias[mirro[i]].count(mirro[j])) {
                    to_remove.push_back(j);
                }
                else if (!alias[glue[i]].count(glue[j])) {
                    to_remove.push_back(j);
                }
                else if (!alias[rneig[i]].count(rneig[j])) {
                    to_remove.push_back(j);
                }
                else if (!alias[lneig[i]].count(lneig[j])) {
                    to_remove.push_back(j);
                }
            }

            if (!to_remove.empty()) {
                change = true;
                for (int j : to_remove)
                    alias[i].erase(j);
            }

            if (alias[i].size() == 1)
                unique[i] = true;
        }
    }

    // --- check: isomorphic = NOT all alias sets are singletons ---
    // unique[i]=True when alias[i] has size 1 (forced to self-map = no cross candidate)
    // all_unique=True means every node self-maps = no isomorphism to B = NON-isomorphic
    // So return !all_unique: true when isomorphic, false when non-isomorphic.
    bool all_unique = true;
    for (bool u : unique) {
        if (!u) { all_unique = false; break; }
    }
    return !all_unique;  // true = isomorphic, false = non-isomorphic
}

static bool is_simplest(const State& sol){
    int le=(int)sol.rneig.size();
    int nw=(le+63)/64;
    static thread_local std::vector<BS> alias;
    if((int)alias.size()<le) alias.resize(le);
    for(int i=0;i<le;++i){
        alias[i]={};
        int full=le/64,rem=le%64;
        for(int w=0;w<full;++w) alias[i][w]=~0ULL;
        if(rem) alias[i][full]=(1ULL<<rem)-1;
    }
    // Direct port of Python simplify(): while change, scan all nodes each pass.
    // A worklist fails here for the same reason as in are_isomorphic: when
    // alias[i] shrinks, every node k with rneig[k]=i (etc.) must be re-examined,
    // but those are not in the "removed candidates" set.
    bool change=true;
    while(change){
        change=false;
        for(int i=0;i<le;++i){
            for(int w=0;w<nw;++w){
                uint64_t bits=alias[i][w], mask_rm=0;
                while(bits){
                    int bit=__builtin_ctzll(bits); bits&=bits-1;
                    int j=w*64+bit; if(j==i) continue;
                    if(sol.lvert[i]!=sol.lvert[j]
                    || !bs_get(alias[j],i)
                    || !bs_get(alias[sol.mirro[i]],sol.mirro[j])
                    || !bs_get(alias[sol.glue[i]],sol.glue[j])
                    || !bs_get(alias[sol.rneig[i]],sol.rneig[j])
                    || !bs_get(alias[sol.lneig[i]],sol.lneig[j]))
                        mask_rm|=(1ULL<<bit);
                }
                if(mask_rm){ alias[i][w]&=~mask_rm; change=true; }
            }
        }
    }
    for(int i=0;i<le;++i) if(!bs_singleton(alias[i],le)) return false;
    return true;
}
// Fingerprint for both complete and partial states.
// For connected edge pairs: 6-tuple of local lvert topology (order-independent).
// For free edges: a sentinel tuple encoding the polygon constraint.
// Collisions are harmless (just a missed pruning); false negatives impossible.
static uint64_t state_fingerprint(const State& sol){
    using T6=std::array<int,6>;
    static thread_local T6 tbuf[256];
    int tc=0, n=(int)sol.rneig.size();
    for(int i=0;i<n;++i){
        int g=sol.glue[i];
        if(g==-1){
            tbuf[tc++]={-1,sol.lvert[sol.rneig[i]],sol.lvert[sol.lneig[i]],0,0,0};
        } else if(g>i){
            tbuf[tc++]={sol.lvert[i],sol.lvert[g],
                        sol.lvert[sol.rneig[i]],sol.lvert[sol.lneig[i]],
                        sol.lvert[sol.rneig[g]],sol.lvert[sol.lneig[g]]};
        }
    }
    std::sort(tbuf,tbuf+tc);
    uint64_t h=14695981039346656037ULL;
    for(int k=0;k<tc;++k)for(int x:tbuf[k]){h^=(uint64_t)(uint32_t)(x+2);h*=1099511628211ULL;}
    uint8_t hist[SYMBOL_COUNT]={};
    for(uint8_t v:sol.vertype) hist[v]++;
    for(int i=0;i<SYMBOL_COUNT;++i){h^=hist[i];h*=1099511628211ULL;}
    return h;
}
// Keep old name as alias for complete-state dedup (ChunkDedup)
static uint64_t solution_fingerprint(const State& sol){ return state_fingerprint(sol); }

// ---------------------------------------------------------------------------
// nauty-based canonical form for COMPLETE states  (thread-safe)
// ---------------------------------------------------------------------------
// Canonicalises the HALF-EDGE combinatorial map directly.  Each half-edge
// becomes a nauty vertex; the three structural relations (rneig, mirro, glue)
// are encoded as coloured auxiliary gadget nodes, making the graph
// automorphism group exactly equal to the combinatorial map isomorphism group.
//
// WHY NOT a vertex graph
// ----------------------
// The orbit of glue[rneig[i]] — which was used in earlier revisions — is a
// FACE traversal (polygon boundary walk), not a vertex ring.  writecyclefinal
// uses exactly left=glue[rneig[left]] to enumerate polygon cycles for output.
// Using those orbits as "tiling vertices" produces a graph that:
//   (a) fails to identify isomorphic states (under-dedup), and
//   (b) merges non-isomorphic states from different signature groups (over-dedup).
//
// GRAPH CONSTRUCTION
// ------------------
// Node classes (total 5 * nh nodes):
//   [0 .. nh)        half-edge nodes,   color = lvert[i]
//   [nh .. 2nh)      rneig-OUT gadgets, color = COL_ROUT
//   [2nh .. 3nh)     rneig-IN  gadgets, color = COL_RIN
//   [3nh .. 4nh)     mirro     gadgets, color = COL_MIRRO (or COL_MIRRO_SELF)
//   [4nh .. 5nh)     glue      gadgets, color = COL_GLUE  (or COL_GLUE_SELF)
//
// Edges:
//   rneig directed edge  i → rneig[i]:
//       half[i] — rout[i] — rin[i] — half[rneig[i]]
//     The asymmetric colours COL_ROUT ≠ COL_RIN encode direction.
//
//   mirro undirected pair {i, mirro[i]}:
//       half[i] — mirro_aux[i] — half[mirro[i]]   (degree-2 aux)
//     If mirro[i]=i: half[i] — mirro_aux[i]        (degree-1 aux, COL_MIRRO_SELF)
//
//   glue undirected pair {i, glue[i]}:  same pattern.
//
// This encoding is provably complete: the automorphisms of this coloured
// graph are in bijection with the combinatorial map isomorphisms, so
// two states produce the same nauty canonical string iff are_isomorphic()
// would return true.
//
// Only call on COMPLETE states (all glue[i] != -1).
// ---------------------------------------------------------------------------
static std::string tiling_canonical_form_complete(const State& sol) {
    const IVec& rneig = sol.rneig;
    const IVec& glue  = sol.glue;
    const IVec& lvert = sol.lvert;
    const IVec& mirro = sol.mirro;
    int nh = (int)rneig.size();

    // ── Step 1: assign compact colour ids to lvert values ────────────────────
    std::map<int,int> lvert_col;
    for (int i = 0; i < nh; ++i)
        lvert_col.emplace(lvert[i], (int)lvert_col.size());
    int nc = (int)lvert_col.size();

    // Auxiliary node colours (appended after the nc lvert colours).
    const int COL_ROUT       = nc;
    const int COL_RIN        = nc + 1;
    const int COL_MIRRO      = nc + 2;
    const int COL_MIRRO_SELF = nc + 3;
    const int COL_GLUE       = nc + 4;
    const int COL_GLUE_SELF  = nc + 5;
    const int N_COLORS       = nc + 6;

    // ── Step 2: node layout ───────────────────────────────────────────────────
    int nv = 5 * nh;
    auto idx_rout  = [&](int i){ return     nh + i; };
    auto idx_rin   = [&](int i){ return 2 * nh + i; };
    auto idx_mirro = [&](int i){ return 3 * nh + i; };
    auto idx_glue  = [&](int i){ return 4 * nh + i; };

    // ── Step 3: adjacency lists ───────────────────────────────────────────────
    std::vector<std::vector<int>> adj(nv);
    auto add_edge = [&](int u, int v) {
        adj[u].push_back(v);
        adj[v].push_back(u);
    };

    for (int i = 0; i < nh; ++i) {
        // rneig directed edge i → rneig[i]  via gadget rout[i]—rin[i]
        add_edge(i,           idx_rout(i));
        add_edge(idx_rout(i), idx_rin(i));
        add_edge(idx_rin(i),  rneig[i]);

        // mirro: aux node connects to i (always) and mirro[i] (if ≠ i)
        add_edge(i, idx_mirro(i));
        if (mirro[i] != i) add_edge(idx_mirro(i), mirro[i]);

        // glue: aux node connects to i (always) and glue[i] (if ≠ i)
        add_edge(i, idx_glue(i));
        if (glue[i] != i) add_edge(idx_glue(i), glue[i]);
    }

    // Deduplicate adjacency lists and count directed edge-ends.
    int nde = 0;
    for (int v = 0; v < nv; ++v) {
        auto& a = adj[v];
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        nde += (int)a.size();
    }

    // ── Step 4: vertex colours ────────────────────────────────────────────────
    std::vector<int> color(nv);
    for (int i = 0; i < nh; ++i) {
        color[i]             = lvert_col[lvert[i]];
        color[idx_rout(i)]   = COL_ROUT;
        color[idx_rin(i)]    = COL_RIN;
        color[idx_mirro(i)]  = (mirro[i] == i) ? COL_MIRRO_SELF : COL_MIRRO;
        color[idx_glue(i)]   = (glue[i]  == i) ? COL_GLUE_SELF  : COL_GLUE;
    }

    // ── Step 5: build nauty SparseGraph — all storage local, no statics ───────
    std::vector<size_t> sg_v(nv);
    std::vector<int>    sg_d(nv), sg_e(nde);
    sparsegraph sg; SG_INIT(sg);
    sg.nv = nv; sg.nde = nde;
    sg.v = sg_v.data(); sg.vlen = nv;
    sg.d = sg_d.data(); sg.dlen = nv;
    sg.e = sg_e.data(); sg.elen = nde;

    { int pos = 0;
      for (int v = 0; v < nv; ++v) {
          sg.v[v] = pos; sg.d[v] = (int)adj[v].size();
          for (int u : adj[v]) sg.e[pos++] = u;
      }
    }

    std::vector<size_t> cg_v(nv);
    std::vector<int>    cg_d(nv), cg_e(nde);
    sparsegraph cg; SG_INIT(cg);
    cg.v = cg_v.data(); cg.vlen = nv;
    cg.d = cg_d.data(); cg.dlen = nv;
    cg.e = cg_e.data(); cg.elen = nde;

    // ── Step 6: lab/ptn from colour partition ─────────────────────────────────
    std::vector<std::vector<int>> by_color(N_COLORS);
    for (int v = 0; v < nv; ++v) by_color[color[v]].push_back(v);

    std::vector<int> lab(nv), ptn(nv), orbits(nv);
    { int pos = 0;
      for (int c = 0; c < N_COLORS; ++c)
          for (int v : by_color[c]) lab[pos++] = v;
    }
    for (int i = 0; i < nv - 1; ++i)
        ptn[i] = (color[lab[i]] == color[lab[i+1]]) ? 1 : 0;
    ptn[nv - 1] = 0;

    // ── Step 7: call nauty (serialised — internal global workspace) ───────────
    DEFAULTOPTIONS_SPARSEGRAPH(options);
    options.defaultptn = FALSE;
    options.getcanon   = TRUE;
    statsblk stats;
    {
        std::lock_guard<std::mutex> lk(g_nauty_mutex);
        sparsenauty(&sg, lab.data(), ptn.data(), orbits.data(), &options, &stats, &cg);
        sortlists_sg(&cg);
    }

    // ── Step 8: serialise canonical graph ─────────────────────────────────────
    std::ostringstream oss;
    for (int v = 0; v < cg.nv; ++v) {
        oss << cg.d[v] << ':';
        for (int j = 0; j < cg.d[v]; ++j) {
            oss << cg.e[cg.v[v] + j];
            if (j + 1 < cg.d[v]) oss << ' ';
        }
        oss << ';';
    }
    return oss.str();
}

static std::string make_signature(const std::vector<uint8_t>& vertype){
    std::vector<int> r(SYMBOL_COUNT,0);for(int i:vertype)r[i]++;
    std::string out;
    for(int i=0;i<SYMBOL_COUNT;++i){
        if(!r[i])continue;
        if(!out.empty())out+=", ";
        out+=SYMBOL_LIST[i];
        if(r[i]>1){out+="x";out+=std::to_string(r[i]);}
    }
    return out;
}
static std::string make_file_signature(const std::vector<uint8_t>& vertype){
    std::vector<int> r(SYMBOL_COUNT,0);for(int i:vertype)r[i]++;
    std::string out;
    for(int i=0;i<SYMBOL_COUNT;++i){
        if(!r[i])continue;
        if(!out.empty())out+=" ";
        out+=CODE_LIST[i];
        if(r[i]>1)out+=std::to_string(r[i]);
    }
    return out;
}

static std::string bucket_path_for_sig(const std::string& tmp_dir, const std::string& sig){
    uint64_t h=14695981039346656037ULL;
    for(char c:sig){h^=(uint8_t)c;h*=1099511628211ULL;}
    char hex[17];snprintf(hex,sizeof(hex),"%016llx",(unsigned long long)h);
    std::string sub(hex,2);
    fs::create_directories(tmp_dir+"/"+sub);
    std::string safe;
    for(char c:sig){
        if(c==' ')safe+='_';else if(c==',')safe+='-';
        else if(c=='('||c==')'){}else safe+=c;
        if((int)safe.size()>=80)break;
    }
    return tmp_dir+"/"+sub+"/"+safe+"_"+std::string(hex,8)+".bin";
}

// Pass-1: filter + bin one chunk, then DELETE the chunk file to free space.
static std::pair<int64_t,int64_t> pass1_chunk(const std::string& chunk_path,
                                                const std::string& wdir,
                                                bool delete_after){
    fs::create_directories(wdir);
    int64_t n_raw=0,n_simp=0;
    stream_states(chunk_path,[&](State&& sol){
        ++n_raw;
        if(!is_simplest(sol))return;
        ++n_simp;
        std::string sig=make_signature(sol.vertype);
        std::string bp=bucket_path_for_sig(wdir,sig);
        std::ofstream bf(bp,std::ios::binary|std::ios::app);
        write_state(bf,sol);
    });
    // Delete source chunk immediately to free disk space.
    if(delete_after) fs::remove(chunk_path);
    return {n_raw,n_simp};
}

// Pass-2: dedup one bucket group, DELETE bucket files after reading.
// Uses nauty canonical forms: O(n) set lookups instead of O(n²) are_isomorphic.
static std::vector<State> dedup_bucket(const std::vector<std::string>& paths){
    std::vector<State> kept;
    std::unordered_set<std::string> seen_canons;
    for(const auto& p:paths){
        if(!fs::exists(p))continue;
        stream_states(p,[&](State&& s){
            std::string canon = tiling_canonical_form_complete(s);
            if(seen_canons.insert(canon).second)
                kept.push_back(std::move(s));
        });
        fs::remove(p);
    }
    return kept;
}

static std::vector<State> deduplicate_streaming(
        const std::vector<std::string>& chunk_files,
        const std::string& tmp_dir){
    int nw=NUM_WORKERS;
    fs::create_directories(tmp_dir);
    std::vector<std::string> wdirs;
    for(int i=0;i<nw;++i) wdirs.push_back(tmp_dir+"/w"+std::to_string(i));

    // Pass 1: process chunks serially per worker, delete each chunk as we go.
    // We deliberately do NOT run all chunks in parallel here, because doing so
    // would mean ALL chunk files exist simultaneously during pass-1.
    // Instead we feed chunks one by one to the worker pool so that at most
    // NUM_WORKERS chunks are being processed at any given moment, and each is
    // deleted as soon as it's consumed.  This keeps peak disk at:
    //   (n_unprocessed_chunks * avg_chunk_size) + (current bucket files)
    // which stays well under budget.
    printf("  Dedup pass 1: filter + bin (%d workers, delete-as-you-go)...\n",nw);
    double t0=now_s();
    std::atomic<int64_t> n_raw{0},n_simp{0};
    std::atomic<int> done1{0};
    int nchunks=(int)chunk_files.size();
    {
        ThreadPool pool(nw);
        std::mutex pm;
        for(int i=0;i<nchunks;++i)
            pool.enqueue([&,i]{
                auto [nr,ns]=pass1_chunk(chunk_files[i],wdirs[i%nw],/*delete_after=*/true);
                n_raw+=nr; n_simp+=ns;
                int d=++done1;
                if(d%std::max(1,nchunks/20)==0||d==nchunks){
                    std::lock_guard lk(pm);
                    printf("    chunk %d/%d  %lld raw  %lld simplest  (%s)\r",
                           d,nchunks,(long long)n_raw.load(),(long long)n_simp.load(),
                           fmt_duration(now_s()-t0).c_str());
                    fflush(stdout);
                }
            });
    }
    printf("\n  Pass 1 done: %lld raw -> %lld simplest  (%s)\n",
           (long long)n_raw.load(),(long long)n_simp.load(),
           fmt_duration(now_s()-t0).c_str());

    // Index buckets
    printf("  Indexing buckets... ");fflush(stdout);
    std::map<std::string,std::vector<std::string>> sig_buckets;
    for(const auto& wd:wdirs){
        if(!fs::exists(wd))continue;
        for(auto& entry:fs::recursive_directory_iterator(wd)){
            if(!entry.is_regular_file())continue;
            auto rel=fs::relative(entry.path(),wd).string();
            sig_buckets[rel].push_back(entry.path().string());
        }
    }
    int n_groups=(int)sig_buckets.size();
    printf("%d signature groups\n",n_groups);

    std::vector<std::vector<std::string>> ordered;
    ordered.reserve(n_groups);
    for(auto& [k,v]:sig_buckets) ordered.push_back(v);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){
        uintmax_t sa=0,sb=0;
        for(auto& p:a)if(fs::exists(p))sa+=fs::file_size(p);
        for(auto& p:b)if(fs::exists(p))sb+=fs::file_size(p);
        return sa>sb;
    });

    // Pass 2: dedup + delete bucket files as they are consumed.
    printf("  Dedup pass 2: isomorphism check (%d workers)...\n",nw);
    double t1=now_s();
    std::vector<State> unique_sols;
    std::mutex mu;
    std::atomic<int> done2{0};
    {
        ThreadPool pool(nw);
        for(auto& paths:ordered)
            pool.enqueue([&,paths]{
                auto kept=dedup_bucket(paths); // deletes files inside
                std::lock_guard lk(mu);
                for(auto& s:kept) unique_sols.push_back(std::move(s));
                int d=++done2;
                if(n_groups>0&&(d%std::max(1,n_groups/20)==0||d==n_groups)){
                    printf("    %d/%d (%d%%)  %d unique  (%s)\r",
                           d,n_groups,100*d/n_groups,(int)unique_sols.size(),
                           fmt_duration(now_s()-t1).c_str());
                    fflush(stdout);
                }
            });
    }
    printf("\n  Pass 2 done: %lld simplest -> %d unique  (%s)\n",
           (long long)n_simp.load(),(int)unique_sols.size(),
           fmt_duration(now_s()-t1).c_str());

    // Remove (now empty) worker dirs
    for(const auto& wd:wdirs) fs::remove_all(wd);
    return unique_sols;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
static std::string conway_symbol(const std::string& fi,const std::string& si){
    std::string f=fi,s=si; int mc=0;
    if(!f.empty()&&f[0]=='*'){mc++;f=f.substr(1);}
    if(!s.empty()&&s[0]=='*'){mc++;s=s.substr(1);}
    bool same=(f==s);
    if(mc!=1)return same?"("+f+")":"("+f+" "+s+")";
    else      return same?"["+f+"]":"["+f+" "+s+"]";
}
static std::string write_conway_str(const IVec& mirro,const IVec& glue,
                                     const std::vector<int16_t>& label){
    std::set<int> seen; std::string parts;
    for(int cy=0;cy<(int)glue.size();++cy){
        if(seen.count(cy)||glue[cy]==-1)continue;
        parts+=conway_symbol(decode_label(label[cy]),decode_label(label[glue[cy]]));
        seen.insert({cy,glue[cy],mirro[cy],glue[mirro[cy]]});
    }
    return parts;
}
static std::string verbal_vertices(const std::vector<uint8_t>& vt){
    std::string out;
    for(int i:vt){if(!out.empty())out+=", ";out+=SYMBOL_LIST[i];}
    return out;
}
static std::string pad2(int n){return(n<10?"0":"")+std::to_string(n);}
static std::string fine_name(const IVec& lvert,int num){
    std::string m=pad2(num)+"_"; std::set<int> sv;
    for(int v:lvert){
        if(sv.count(v))continue;sv.insert(v);
        if(v==3)m+="3";else if(v==4)m+="4";else if(v==6)m+="6";else if(v==12)m+="c";
    }
    return m;
}
static void tes_write(const std::string& conway_str,
                       const std::vector<int>& poly_size_list,
                       const std::vector<int>& repeat_list,
                       const std::string& tes_file,
                       const std::string& sol_string){
    std::ofstream tes(tes_file);
    tes<<"## Euclidean, "<<sol_string<<"\ne2.\nangleunit(deg)\n";
    for(int n:poly_size_list){
        int angle=180-(int)(360.0/n); std::string angles;
        for(int k=0;k<n;++k){if(k)angles+=",";angles+=std::to_string(angle);}
        tes<<"unittile("<<angles<<")\n";
    }
    tes<<"conway(\""<<conway_str<<"\")\n";
    for(int i=0;i<(int)repeat_list.size();++i)
        if(repeat_list[i]>1)tes<<"repeat("<<i<<","<<repeat_list[i]<<")\n";
}

static void write_cycle_final(const IVec& rneig,const IVec&,
                               const IVec& lvert,const IVec& mirro,
                               const IVec& glue,const std::vector<int16_t>& label,
                               std::ofstream& filen,
                               const std::string& tes_file,
                               const std::string& sol_string){
    std::set<int> seen;
    std::vector<std::string> mainstlist;
    std::vector<int> sublist,repeatlist;
    bool ultra_chiral=true;
    int n=(int)glue.size();

    auto lstr=[&](int i){return decode_label(label[i]);};

    for(int cy=0;cy<n;++cy){
        if(seen.count(cy))continue;
        int left=cy,right=rneig[left],v=lvert[right],cnt=0,mm=n;
        std::string mainst;
        while(true){
            seen.insert(left);
            if(mirro[right]<mm)mm=mirro[right];
            mainst+=lstr(left)+"/"+lstr(right)+"("+std::to_string(lvert[right])+")-";
            ++cnt;left=glue[right];if(left==cy)break;right=rneig[left];
        }
        mainst.pop_back();
        int ratio=v/cnt;repeatlist.push_back(ratio);
        if(ratio!=1)mainst="["+mainst+"]x"+std::to_string(ratio);
        mainstlist.push_back(mainst);

        if(seen.count(mm)){
            sublist.push_back(0);ultra_chiral=false;
        } else {
            int left2=mm,right2=rneig[left2]; std::string mainst2;
            while(true){
                seen.insert(left2);
                mainst2+=lstr(left2)+"/"+lstr(right2)+"("+std::to_string(lvert[right2])+")-";
                ++cnt;left2=glue[right2];if(left2==mm)break;right2=rneig[left2];
            }
            mainst2.pop_back();
            repeatlist.push_back(ratio);
            if(ratio!=1)mainst2="["+mainst2+"]x"+std::to_string(ratio);
            mainstlist.push_back(mainst2);
            sublist.push_back(1);sublist.push_back(2);
        }
    }

    std::string subheader;
    for(int m=0;m<(int)mainstlist.size();++m){
        int sub=sublist[m];
        if(sub==0){filen<<m<<": "<<mainstlist[m];}
        else if(sub==1){
            std::string hdr=ultra_chiral?std::to_string(m/2)+": "
                                        :std::to_string(m)+"/"+std::to_string(m+1)+": ";
            subheader=std::string(hdr.size(),' ');filen<<hdr<<mainstlist[m];
        } else {filen<<subheader<<mainstlist[m];}
        filen<<"\n";
    }
    filen<<"---\n";

    bool is_chiral=ultra_chiral;
    std::vector<std::string> work_list,left_edges,right_edges,edges;
    std::vector<int> poly_size_list,work_reps;

    if(is_chiral){
        for(int k=0;k<(int)mainstlist.size()/2;++k){
            work_list.push_back(mainstlist[2*k]);work_reps.push_back(repeatlist[2*k]);
        }
    } else {work_list=mainstlist;work_reps=repeatlist;}

    for(int m=0;m<(int)work_list.size();++m){
        const std::string& mainst=work_list[m];int rep=work_reps[m];
        std::string s=(rep>1)?mainst.substr(1,mainst.find(']')-1):mainst;
        size_t p1=s.find('('),p2=s.find(')');
        poly_size_list.push_back(std::stoi(s.substr(p1+1,p2-p1-1)));
        s+="-";
        int rev=(int)s.size()-1;while(s[rev]!='/')rev--;
        s=s.substr(rev+1)+s.substr(0,rev+1);
        int ei=0;
        while(!s.empty()){
            size_t ind=s.find('/');std::string chunk=s.substr(0,ind+1);s=s.substr(ind+1);
            size_t lp=chunk.find('('),mi=chunk.find('-');
            left_edges.push_back(chunk.substr(0,lp));
            right_edges.push_back(chunk.substr(mi+1,chunk.size()-mi-2));
            // edge_label(ei, m) but stored as string for Conway building
            std::string elabel=std::to_string(ei);
            if(m>3){elabel+="@";elabel+=std::to_string(m);}
            else{for(int t=0;t<m;++t)elabel+="'";}
            edges.push_back(elabel);
            ++ei;
        }
    }

    std::string conway_str;
    while(!left_edges.empty()){
        if(is_chiral){
            auto it=std::find(right_edges.begin(),right_edges.end(),left_edges[0]);
            if(it==right_edges.end()){
                std::string mm=(left_edges[0][0]=='*')?left_edges[0].substr(1):"*"+left_edges[0];
                auto it2=std::find(left_edges.begin(),left_edges.end(),mm);
                if(it2==left_edges.end()){conway_str+="["+edges[0]+"]";}
                else{
                    int match=(int)(it2-left_edges.begin());
                    conway_str+="["+edges[0]+" "+edges[match]+"]";
                    left_edges.erase(left_edges.begin()+match);
                    right_edges.erase(right_edges.begin()+match);
                    edges.erase(edges.begin()+match);
                }
                left_edges.erase(left_edges.begin());right_edges.erase(right_edges.begin());edges.erase(edges.begin());
                continue;
            }
        }
        auto it=std::find(right_edges.begin(),right_edges.end(),left_edges[0]);
        if(it==right_edges.end()||it==right_edges.begin()){
            conway_str+="("+edges[0]+")";
            left_edges.erase(left_edges.begin());right_edges.erase(right_edges.begin());edges.erase(edges.begin());
        } else {
            int match=(int)(it-right_edges.begin());
            conway_str+="("+edges[0]+" "+edges[match]+")";
            left_edges.erase(left_edges.begin()+match);right_edges.erase(right_edges.begin()+match);edges.erase(edges.begin()+match);
            left_edges.erase(left_edges.begin());right_edges.erase(right_edges.begin());edges.erase(edges.begin());
        }
    }
    filen<<conway_str<<"\n";
    tes_write(conway_str,poly_size_list,work_reps,tes_file,sol_string);
}

static void write_solution(const State& sol,
                            std::map<std::string,int>& sol_number_by_sig,
                            std::vector<std::string>& run_total_keys,
                            std::vector<int>& run_total_counts){
    std::string sig=make_signature(sol.vertype);
    sol_number_by_sig[sig]++;
    int ret=sol_number_by_sig[sig];
    std::string filesig=make_file_signature(sol.vertype);
    std::string fine=fine_name(sol.lvert,sol.num);
    std::string finn=pad2(sol.num);
    auto it=std::find(run_total_keys.begin(),run_total_keys.end(),fine);
    bool first=(it==run_total_keys.end());
    if(first){run_total_keys.push_back(fine);run_total_counts.push_back(1);}
    else run_total_counts[it-run_total_keys.begin()]++;
    fs::create_directories(OUTPUT_DIR+finn+"/"+fine+"/"+filesig);
    std::string fullname=OUTPUT_DIR+LIST_FILE+fine+".txt";
    std::ofstream globe(fullname,first?std::ios::out:std::ios::app);
    globe<<"Number of polygons: "<<sol.num<<"\n";
    globe<<verbal_vertices(sol.vertype)<<"\n";
    globe<<sig<<"\n";
    std::string dirsig=finn+"/"+fine+"/"+filesig;
    std::string tesfile1=dirsig+"/eu raw "+filesig+" "+std::to_string(ret)+".tes";
    std::string tesfile=OUTPUT_DIR+tesfile1;
    std::string sol_str=sig+", solution "+std::to_string(ret);
    globe<<"TES file: "<<tesfile1<<"\n";
    globe<<write_conway_str(sol.mirro,sol.glue,sol.label)<<"\n";
    write_cycle_final(sol.rneig,sol.lneig,sol.lvert,sol.mirro,
                      sol.glue,sol.label,globe,tesfile,sol_str);
    globe<<"\n";
}

static int find_minimum(const std::vector<std::vector<int>>& sv){
    int best=0,bs=0;for(int x:sv[0])bs+=x;
    for(int i=1;i<(int)sv.size();++i){
        int s=0;for(int x:sv[i])s+=x;
        if(s<bs){best=i;bs=s;continue;}
        if(s==bs)for(int k=0;k<(int)sv[i].size();++k){
            if(sv[i][k]>sv[best][k]){best=i;break;}
            if(sv[i][k]<sv[best][k])break;
        }
    }
    return best;
}

static void write_final_summary(const std::vector<std::string>& run_total_keys,
                                 const std::vector<int>& run_total_counts,
                                 const std::vector<State>& unique_solutions){
    std::vector<std::vector<int>> vs;std::vector<int> vc;
    for(const auto& sol:unique_solutions){
        std::vector<int> r(SYMBOL_COUNT,0);for(int i:sol.vertype)r[i]++;
        auto it=std::find(vs.begin(),vs.end(),r);
        if(it==vs.end()){vs.push_back(r);vc.push_back(1);}
        else vc[it-vs.begin()]++;
    }
    std::ofstream f(OUTPUT_DIR+"eu_final_results.txt");
    for(int i=0;i<(int)run_total_keys.size();++i)
        f<<run_total_keys[i]<<": "<<run_total_counts[i]<<"\n";
    f<<"\n";
    auto sc=vs;auto cc=vc;
    while(!sc.empty()){
        int idx=find_minimum(sc);
        std::string line;
        for(int i=0;i<SYMBOL_COUNT;++i){
            if(!sc[idx][i])continue;
            if(!line.empty())line+=", ";
            line+=SYMBOL_LIST[i];
            if(sc[idx][i]>1){line+="x";line+=std::to_string(sc[idx][i]);}
        }
        f<<line<<": "<<cc[idx]<<"\n";
        sc.erase(sc.begin()+idx);cc.erase(cc.begin()+idx);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static void print_help(){
    printf("Usage: solver_zomega [options]\n"
           "  --workers N       worker threads           (default: %d)\n"
           "  --max-tiles N     max vertex-orbit types   (default: %d)\n"
           "  --fanout N        BFS fan-out target       (default: %d)\n"
           "  --overcommit N    chunks / worker          (default: %d)\n"
           "  --stack-cap N     DFS stack cap (states)   (default: %d)\n"
           "  --output DIR      output directory         (default: %s)\n"
           "  --checkpoint DIR  checkpoint directory     (default: %s)\n"
           "  --help            show this help\n",
           NUM_WORKERS,MAX_TILES,FANOUT_TARGET,CHUNK_OVERCOMMIT,
           STACK_FLUSH_THRESHOLD,OUTPUT_DIR.c_str(),CHECKPOINT_DIR.c_str());
}

#ifndef SOLVER_TEST_MODE
int main(int argc,char** argv){
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if     (a=="--help")                  {print_help();return 0;}
        else if(a=="--workers"   &&i+1<argc)  NUM_WORKERS          =std::stoi(argv[++i]);
        else if(a=="--max-tiles" &&i+1<argc)  MAX_TILES            =std::stoi(argv[++i]);
        else if(a=="--fanout"    &&i+1<argc)  FANOUT_TARGET        =std::stoi(argv[++i]);
        else if(a=="--overcommit"&&i+1<argc)  CHUNK_OVERCOMMIT     =std::stoi(argv[++i]);
        else if(a=="--stack-cap" &&i+1<argc)  STACK_FLUSH_THRESHOLD=std::stoi(argv[++i]);
        else if(a=="--pdedup-cap"&&i+1<argc)  PDEDUP_CAP           =std::stoi(argv[++i]);
        else if(a=="--output"    &&i+1<argc)  OUTPUT_DIR           =argv[++i];
        else if(a=="--checkpoint"&&i+1<argc)  CHECKPOINT_DIR       =argv[++i];
        else{fprintf(stderr,"Unknown option: %s\n",a.c_str());return 1;}
    }
    if(OUTPUT_DIR.back()!='/')     OUTPUT_DIR+='/';
    if(CHECKPOINT_DIR.back()!='/') CHECKPOINT_DIR+='/';

    init_tables();
    fs::create_directories(OUTPUT_DIR);
    fs::create_directories(CHECKPOINT_DIR);

    printf("Workers: %d  |  Fanout: %d  |  Max k: %d  |  Overcommit: %dx  |  Stack cap: %d\n",
           NUM_WORKERS,FANOUT_TARGET,MAX_TILES,CHUNK_OVERCOMMIT,STACK_FLUSH_THRESHOLD);
    printf("Checkpoint: %s\n\n",fs::absolute(CHECKPOINT_DIR).c_str());

    Progress prog;bool resuming=load_progress(prog);
    if(resuming&&(prog.max_tiles!=MAX_TILES||prog.num_workers!=NUM_WORKERS
                  ||prog.chunk_overcommit!=CHUNK_OVERCOMMIT)){
        printf("  WARNING: checkpoint config differs -- starting from scratch.\n");
        resuming=false;prog.completed.clear();
    }

    std::vector<State> frontier,early_solutions;
    if(resuming){
        printf("Resuming: %d chunks already done.\n",(int)prog.completed.size());
        frontier       =read_states(ckpt_path("frontier.bin"));
        early_solutions=read_states(ckpt_path("early.bin"));
    } else {
        auto [fr,es]=fan_out(FANOUT_TARGET);
        frontier=std::move(fr);early_solutions=std::move(es);
        printf("  Saving fan-out checkpoint... ");fflush(stdout);
        write_states(ckpt_path("frontier.bin"),frontier);
        write_states(ckpt_path("early.bin"),early_solutions);
        printf("done.\n");
        prog.completed.clear();
        prog.max_tiles=MAX_TILES;prog.fanout_target=FANOUT_TARGET;
        prog.num_workers=NUM_WORKERS;prog.chunk_overcommit=CHUNK_OVERCOMMIT;
    }

    // Phase 2: parallel DFS
    g_pdedup.reset(); g_pdedup.set_cap(PDEDUP_CAP);
    int n_chunks=NUM_WORKERS*CHUNK_OVERCOMMIT;
    std::vector<std::vector<State>> chunks(n_chunks);
    for(int idx=0;idx<(int)frontier.size();++idx)
        chunks[idx%n_chunks].push_back(frontier[idx]);
    {std::vector<State>().swap(frontier);}

    std::vector<int> remaining;
    for(int i=0;i<n_chunks;++i)
        if(!prog.completed.count(i))remaining.push_back(i);

    printf("Phase 2 -- parallel DFS:\n");
    printf("  %d chunks, %d done, %d remaining.\n",
           n_chunks,(int)prog.completed.size(),(int)remaining.size());

    double t1=now_s();
    std::atomic<int> done_chunks{0};
    std::mutex merge_mu;
    PhaseETA eta2(n_chunks,(int)prog.completed.size(),0.3);

    {
        ThreadPool pool(NUM_WORKERS);
        for(int ci:remaining)
            pool.enqueue([&,ci]{
                auto ret=dfs_worker_to_file(std::move(chunks[ci]),chunk_file(ci));
                auto [n_raw,n_simp,n_uniq]=std::tie(std::get<0>(ret),std::get<1>(ret),std::get<2>(ret));
                std::lock_guard lk(merge_mu);
                prog.completed.insert(ci);
                save_progress(prog);
                int d=++done_chunks;
                double el=now_s()-t1;
                eta2.update(1,el/d);
                printf("  chunk %d/%d (idx %d)  raw=%lld simp=%lld uniq=%lld  pdedup=%.0f%%  %s  %s\n",
                       d,(int)remaining.size(),ci,
                       (long long)n_raw,(long long)n_simp,(long long)n_uniq,
                       std::get<3>(ret)*100.0,
                       fmt_duration(el).c_str(),
                       eta2.progress_line("overall").c_str());
                fflush(stdout);
            });
    }
    {std::vector<std::vector<State>>().swap(chunks);}
    printf("  Phase 2 complete -- elapsed=%s  global_pdedup=%.1f%%\n",fmt_duration(now_s()-t1).c_str(),g_pdedup.dedup_rate()*100.0);

    // Phase 3: deduplicate
    printf("\nPhase 3 -- streaming deduplication...\n");
    double t2=now_s();

    std::string early_ckpt=ckpt_path("chunk_early.bin");
    if(!early_solutions.empty()){
        // Apply is_simplest to early solutions before writing — same filter
        // as pass-1, so pass-1 just re-reads them directly (already clean).
        int64_t es_raw=(int64_t)early_solutions.size(), es_kept=0;
        {std::ofstream ef(early_ckpt,std::ios::binary);
         for(const auto& s:early_solutions)
             if(is_simplest(s)){write_state(ef,s);++es_kept;}}
        printf("  Early solutions: %lld raw -> %lld simplest (pre-filtered)\n",
               (long long)es_raw,(long long)es_kept);
        {std::vector<State>().swap(early_solutions);}
    }

    std::vector<std::string> all_chunk_paths;
    if(fs::exists(early_ckpt))all_chunk_paths.push_back(early_ckpt);
    for(int ci=0;ci<n_chunks;++ci){
        std::string p=chunk_file(ci);
        if(fs::exists(p))all_chunk_paths.push_back(p);
    }

    std::string tmp_dir=CHECKPOINT_DIR+"sig_buckets";
    auto unique=deduplicate_streaming(all_chunk_paths,tmp_dir);
    printf("  elapsed=%s\n",fmt_duration(now_s()-t2).c_str());

    // Write output
    printf("\nWriting %d unique solutions to '%s'...\n",(int)unique.size(),OUTPUT_DIR.c_str());
    std::map<std::string,int> sol_number_by_sig;
    std::vector<std::string> run_total_keys;
    std::vector<int> run_total_counts;
    PhaseETA eta_w((int)unique.size(),0,0.2);

    for(int idx=0;idx<(int)unique.size();++idx){
        double ts=now_s();
        write_solution(unique[idx],sol_number_by_sig,run_total_keys,run_total_counts);
        eta_w.update(1,now_s()-ts);
        if((idx+1)%50==0){printf("%s\r",eta_w.progress_line("Write").c_str());fflush(stdout);}
    }
    printf("  Write complete: %d solutions  (%s)%30s\n",
           (int)unique.size(),fmt_duration(eta_w.elapsed()).c_str(),"");
    write_final_summary(run_total_keys,run_total_counts,unique);
    printf("Done. %d unique solutions written.\n",(int)unique.size());

    fs::remove_all(CHECKPOINT_DIR);
    return 0;
}
#endif // SOLVER_TEST_MODE
