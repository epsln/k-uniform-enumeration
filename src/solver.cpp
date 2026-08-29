#include "solver.h"
#include "vertex_catalog.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <mutex>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace catalog;

// =============================================================================
// Global counters
// =============================================================================
static std::mutex g_mutex;
static std::map<int, int> g_per_k_counts;
static std::array<std::atomic<int64_t>, 24> g_raw_per_k;  // lock-free for progress

void EuclideanSolver::reset_global_counters() {
    std::lock_guard<std::mutex> lk(g_mutex); g_per_k_counts.clear();
    for (auto& a : g_raw_per_k) a.store(0);
}
std::map<int, int> EuclideanSolver::global_per_k_counts() {
    std::lock_guard<std::mutex> lk(g_mutex); return g_per_k_counts;
}
void EuclideanSolver::read_raw_per_k(std::array<int64_t, 24>& out) {
    for (size_t i = 0; i < 24; ++i) out[i] = g_raw_per_k[i].load();
}
std::mutex& EuclideanSolver::global_mutex() { return g_mutex; }

// =============================================================================
// Catalog delegates
// =============================================================================
int EuclideanSolver::neighbors_len(int gr) { return (int)left_neighbors[gr].size(); }
int EuclideanSolver::attach_limit(int gr) { return attachment_limit(gr); }
int EuclideanSolver::polygon_size_of(int gr, int slot) { return polygon_sizes[gr][slot]; }

static void fill_neighbors(State& s, int gr, int offset, int sl) {
    for (int sg = 0; sg < sl; ++sg) {
        Dart d;
        d.rneig = offset + right_neighbors[gr][sg];
        d.lneig = offset + left_neighbors[gr][sg];
        d.mirro = offset + mirrors[gr][sg];
        d.polygon_size = polygon_sizes[gr][sg];
        d.glue = -1;
        d.is_mirror_edge = edge_label_templates[gr][sg][0] == '*';
        s.darts.push_back(d);
    }
}

State EuclideanSolver::extend_state(const State& base, int gr, int offset, int sl) {
    State s;
    s.darts = base.darts;
    s.vertype = base.vertype;
    fill_neighbors(s, gr, offset, sl);
    s.vertype.push_back(gr);
    return s;
}

// =============================================================================
// Construction
// =============================================================================
EuclideanSolver::EuclideanSolver(int mp, const std::string& odir)
    : max_polygons_(mp), output_dir_(odir), write_log_(true) {
    fs::create_directories(output_dir_);
    for (int vt = 0; vt < NUM_VERTEX_TYPES; ++vt)
        queue_.push_back(pack_state(make_initial(vt)));
    log_.open(output_dir_ + "/euoutput.txt");
}
EuclideanSolver::EuclideanSolver(int mp, const std::string& odir,
                                   std::vector<State> initials, bool wlog)
    : max_polygons_(mp), output_dir_(odir), write_log_(wlog) {
    fs::create_directories(output_dir_);
    for (auto& s : initials) queue_.push_back(pack_state(s));
    if (write_log_) log_.open(output_dir_ + "/euoutput.txt");
}

State EuclideanSolver::make_initial(int vt) {
    State s; int sl = (int)left_neighbors[vt].size();
    s.darts.reserve(sl);
    for (int j = 0; j < sl; ++j) {
        Dart d;
        d.rneig = right_neighbors[vt][j];
        d.lneig = left_neighbors[vt][j];
        d.polygon_size = polygon_sizes[vt][j];
        d.mirro = mirrors[vt][j];
        d.glue = -1;
        d.is_mirror_edge = edge_label_templates[vt][j][0] == '*';
        s.darts.push_back(d);
    }
    s.vertype = {vt}; return s;
}

// =============================================================================
// Compact state pack / unpack
// =============================================================================
State EuclideanSolver::rebuild_from_vertype_glue(const std::vector<int>& vertype,
                                                   const std::vector<int>& glue) {
    State s;
    s.vertype = vertype;
    for (size_t tile = 0; tile < vertype.size(); ++tile) {
        int vt = vertype[tile];
        int offset = (int)s.darts.size();
        int sl = (int)left_neighbors[vt].size();
        for (int sg = 0; sg < sl; ++sg) {
            Dart d;
            d.rneig = offset + right_neighbors[vt][sg];
            d.lneig = offset + left_neighbors[vt][sg];
            d.mirro = offset + mirrors[vt][sg];
            d.polygon_size = polygon_sizes[vt][sg];
            d.glue = (offset + sg < (int)glue.size()) ? glue[offset + sg] : -1;
            d.is_mirror_edge = edge_label_templates[vt][sg][0] == '*';
            s.darts.push_back(d);
        }
    }
    return s;
}

PackedState EuclideanSolver::pack_state(const State& st) {
    PackedState p;
    p.vertype.reserve(st.vertype.size());
    for (int v : st.vertype) p.vertype.push_back((uint8_t)v);
    p.glue.reserve(st.darts.size());
    for (const auto& d : st.darts) p.glue.push_back((int16_t)d.glue);
    return p;
}

State EuclideanSolver::unpack_state(const PackedState& p) {
    std::vector<int> vt(p.vertype.begin(), p.vertype.end());
    std::vector<int> gl(p.glue.begin(), p.glue.end());
    return rebuild_from_vertype_glue(vt, gl);
}

// =============================================================================
// Validity
// =============================================================================
bool EuclideanSolver::check_partial(const State& st) {
    int n = (int)st.darts.size();
    static thread_local std::vector<uint8_t> seen;
    seen.assign(n, 0);
    for (int i = 0; i < n; ++i) {
        if (seen[i]) continue;
        int free = i, rfree = st.darts[free].rneig;
        int main_sz = st.darts[rfree].polygon_size, cnt = 1;
        while (true) {
            seen[free] = 1;
            free = st.darts[rfree].glue;
            if (free == -1) { if (cnt > main_sz) return false; break; }
            if (free == i)  { if (main_sz % cnt != 0) return false; break; }
            rfree = st.darts[free].rneig; ++cnt;
            if (st.darts[rfree].polygon_size != main_sz) return false;
        }
    }
    return true;
}

// =============================================================================
// Cycle analysis
// =============================================================================
std::pair<int,int> EuclideanSolver::analyze_cycles(const State& st) {
    int n = (int)st.darts.size();
    static thread_local std::vector<int> seen;
    seen.assign(n, 0);
    std::pair<int,int> tightest = {-1, 13};
    for (int start = 0; start < n; ++start) {
        if (seen[start]) continue;
        int left = start;
        int ring_sz = st.darts[st.darts[start].rneig].polygon_size;
        bool unclosed = false;
        while (st.darts[left].glue != -1 && st.darts[left].glue != st.darts[start].rneig) {
            int prev = st.darts[st.darts[left].glue].lneig;
            if (st.darts[st.darts[prev].rneig].polygon_size != ring_sz) { unclosed = true; break; }
            left = prev;
        }
        if (unclosed) continue;
        if (st.darts[left].glue == -1) {
            int stable = left, right = st.darts[left].rneig;
            int v_stable = st.darts[right].polygon_size, cnt = 0;
            while (true) {
                seen[left] = 1; ++cnt;
                left = st.darts[right].glue;
                if (left != -1) { right = st.darts[left].rneig; }
                else { if (!seen[start]) break; int sl = v_stable - cnt; if (sl < tightest.second) tightest = {stable, sl}; break; }
            }
        } else {
            left = start; int right = st.darts[left].rneig;
            while (true) { seen[left] = 1; left = st.darts[right].glue; if (left == start || left == -1) break; right = st.darts[left].rneig; }
        }
    }
    return tightest;
}

// =============================================================================
// Constraint propagation
// =============================================================================
bool EuclideanSolver::propagate_forced(std::vector<Dart>& darts) {
    int n = (int)darts.size();
    auto G = [&](int i) -> int& { return darts[i].glue; };
    auto R = [&](int i) { return darts[i].rneig; };
    auto L = [&](int i) { return darts[i].lneig; };
    auto P = [&](int i) { return darts[i].polygon_size; };
    auto M = [&](int i) { return darts[i].mirro; };

    bool changed = true;
    static thread_local std::vector<int> seen;
    while (changed) {
        changed = false;
        seen.assign(n, 0);

        for (int start = 0; start < n; ++start) {
            if (seen[start]) continue;

            int beg = start;
            while (G(beg) != -1 && G(beg) != R(start))
                beg = L(G(beg));

            if (G(beg) == R(start)) {
                int cur = beg, r = R(cur);
                while (true) {
                    seen[cur] = 1;
                    int nxt = G(r);
                    if (nxt == beg || nxt == -1) break;
                    cur = nxt; r = R(cur);
                }
                continue;
            }

            int cur = beg, segs = 0;
            int sz = P(R(beg));
            int free_r = -1;

            while (true) {
                seen[cur] = 1;
                int r = R(cur);
                if (P(r) != sz) return false;
                int nxt = G(r);
                if (nxt == -1) { free_r = r; break; }
                ++segs;
                if (nxt == start) { free_r = -2; break; }
                if (segs > sz || segs > n) return false;
                cur = nxt;
            }
            if (free_r == -2) continue;

            int slack = sz - (segs + 1);
            if (slack != 0) continue;

            int a = beg, b = free_r;
            if (G(a) != -1 || G(b) != -1) {
                if (G(a) != b || G(b) != a) return false;
                continue;
            }
            bool am = (M(a) == a), bm = (M(b) == b);
            if (am != bm) return false;

            G(a) = b; G(b) = a;
            if (!am) {
                int ma = M(a), mb = M(b);
                if (G(ma) != -1 && G(ma) != mb) return false;
                if (G(mb) != -1 && G(mb) != ma) return false;
                G(ma) = mb; G(mb) = ma;
            }
            changed = true;
            break;
        }
    }
    return true;
}

// =============================================================================
// Partial dedup — disabled (net negative for performance at tested k)
// =============================================================================
void EuclideanSolver::set_pdedup_cap(int) {}

bool EuclideanSolver::partial_dedup_check(const State&, int) { return true; }

// =============================================================================
// Canonical labeling filter for partial states.
// Adapted from SolutionPruner::is_canonical_labeling — handles glue[i]==-1.
// Non-canonical partials are skipped: some other construction order will
// produce the canonical version.  Reduces search tree by ~50%.
// =============================================================================
bool EuclideanSolver::is_canonical_partial(const State& st) {
    int n = (int)st.darts.size();
    if (n <= 1 || st.vertype.size() <= 1) return true;  // always keep roots
    int nw = (n + 63) / 64;

    using BS4 = std::array<uint64_t, 4>;
    static thread_local std::vector<BS4> alias_buf;
    if ((int)alias_buf.size() < n) alias_buf.resize(n);
    BS4* alias = alias_buf.data();

    for (int i = 0; i < n; ++i) {
        for (int w = 0; w < nw; ++w) alias[i][w] = ~0ULL;
        int rem = n & 63;
        if (rem) alias[i][nw - 1] &= (1ULL << rem) - 1;
    }

    auto bs4_get = [&](const BS4& b, int idx) -> bool {
        return idx >= 0 && idx < n && ((b[idx >> 6] >> (idx & 63)) & 1);
    };

    std::vector<bool> unique(n, false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            for (int w = 0; w < nw; ++w) {
                uint64_t bits = alias[i][w];
                while (bits) {
                    int bit = __builtin_ctzll(bits); bits &= bits - 1;
                    int j = w * 64 + bit;
                    if (j == i || j >= n) continue;

                    bool ok = (st.darts[i].polygon_size == st.darts[j].polygon_size)
                           && bs4_get(alias[j], i)
                           && bs4_get(alias[st.darts[i].mirro], st.darts[j].mirro)
                           && bs4_get(alias[st.darts[i].rneig], st.darts[j].rneig)
                           && bs4_get(alias[st.darts[i].lneig], st.darts[j].lneig);

                    if (ok) {
                        int gi = st.darts[i].glue, gj = st.darts[j].glue;
                        if (gi != -1 || gj != -1) {
                            if (gi == -1 || gj == -1) ok = false;
                            else ok = bs4_get(alias[gi], gj);
                        }
                    }

                    if (!ok) {
                        alias[i][w] &= ~(1ULL << bit);
                        changed = true;
                    }
                }
            }

            int cnt = 0;
            for (int w = 0; w < nw; ++w) cnt += __builtin_popcountll(alias[i][w]);
            if (cnt == 1) unique[i] = true;
        }
    }

    for (int i = 0; i < n; ++i)
        if (!unique[i]) return false;
    return true;
}

// =============================================================================
// Helper (kept for possible future partial dedup)
// =============================================================================
static inline uint64_t fnv64(uint64_t h, uint64_t x) { return (h ^ x) * 1099511628211ULL; }

// =============================================================================
// Lexicographic edge ordering — deterministic, enables canonical pruning.
// =============================================================================
int EuclideanSolver::first_free_lex(const State& st) {
    int best = -1;
    int n = (int)st.darts.size();
    for (int i = 0; i < n; ++i) {
        if (st.darts[i].glue != -1) continue;
        if (best == -1) { best = i; continue; }
        int ki = st.darts[i].is_mirror_edge ? st.darts[i].mirro : i;
        int kb = st.darts[best].is_mirror_edge ? st.darts[best].mirro : best;
        int si = st.darts[ki].polygon_size;
        int sb = st.darts[kb].polygon_size;
        if (si < sb) best = i;
        else if (si == sb) {
            if (!st.darts[i].is_mirror_edge && st.darts[best].is_mirror_edge) best = i;
            else if (st.darts[i].is_mirror_edge == st.darts[best].is_mirror_edge && i < best) best = i;
        }
    }
    return best;
}

// =============================================================================
// Extend (instance method, calls template extend_into)
// =============================================================================
void EuclideanSolver::extend(const State& st) {
    extend_into(st, [this](State&& cand) {
        bool complete = true;
        for (const auto& d : cand.darts) if (d.glue == -1) { complete = false; break; }
        if (complete) write_solution(cand);
        else queue_.push_back(pack_state(cand));
    }, max_polygons_);
}

// =============================================================================
// Solution output (private instance method)
// =============================================================================
void EuclideanSolver::write_solution(const State& st) {
    ++solutions_found_;
    write_solution_static(st, output_dir_, g_mutex,
                          run_totals_, solution_files_,
                          vertex_combos_);
}

// =============================================================================
// Summary output
// =============================================================================
void EuclideanSolver::write_solution_static(const State& st, const std::string& output_dir,
                                              std::mutex& mu,
                                              std::map<std::string,int>& run_totals,
                                              std::map<std::string,std::string>& solution_files,
                                              HistogramMap& vertex_combos) {
    std::string combo = fine_name(st);
    std::string ext = g_binary_solutions ? ".bin" : ".txt";
    std::string path = output_dir + "/eusolver_" + combo + ext;
    bool is_new = (run_totals.find(combo) == run_totals.end());
    run_totals[combo] = (is_new ? 1 : run_totals[combo] + 1);
    solution_files[combo] = path;

    auto hist = sig_result(st.vertype);
    int sol_idx;
    auto it = vertex_combos.find(hist);
    if (it != vertex_combos.end()) {
        sol_idx = ++it->second;
    } else if ((int64_t)vertex_combos.size() < HISTOGRAM_CAP) {
        vertex_combos[hist] = 1; sol_idx = 1;
    } else {
        sol_idx = 1;  // cap reached; cosmetic index only
    }
    std::string sig = signature(st.vertype);
    {
        std::lock_guard<std::mutex> lk(mu);
        int k = (int)st.vertype.size();
        auto kit = g_per_k_counts.find(k);
        g_per_k_counts[k] = (kit != g_per_k_counts.end() ? kit->second + 1 : 1);
    }
    if ((size_t)st.vertype.size() < g_raw_per_k.size())
        g_raw_per_k[st.vertype.size()].fetch_add(1);

    if (g_binary_solutions) {
        std::ofstream bout(path, std::ios::binary | (is_new ? std::ios::out : std::ios::app));
        uint8_t nv = (uint8_t)st.vertype.size();
        uint8_t ne = (uint8_t)st.darts.size();
        bout.write((char*)&nv, 1);
        bout.write((char*)&ne, 1);
        for (int v : st.vertype) { uint8_t x = (uint8_t)v; bout.write((char*)&x, 1); }
        for (const auto& d : st.darts) { int16_t x = (int16_t)d.glue; bout.write((char*)&x, 2); }
        return;
    }

    std::ofstream out(path, is_new ? std::ios::out : std::ios::app);
    out << "Number of polygons: " << st.vertype.size() << "\n";
    out << verbal_vertices(st.vertype) << "\n";
    out << sig << "\n";
    std::string filesig = file_signature(st.vertype);
    std::string tes_rel = pad2((int)st.vertype.size())+"/"+combo+"/"+filesig
                        +"/eu raw "+filesig+" "+std::to_string(sol_idx)+".tes";
    out << "TES file: " << tes_rel << "\n";
    out << write_conway(st) << "\n";

    int nn = (int)st.darts.size();
    std::vector<int> seen(nn,0);
    std::vector<std::string> ms; std::vector<int> ss, rp; bool uc=true;
    for (int cy=0;cy<nn;++cy){if(seen[cy])continue;
        int l=cy,r=st.darts[l].rneig;int v=st.darts[r].polygon_size,cnt=0,mm=nn;std::string t;
        while(true){seen[l]=1;if(st.darts[r].mirro<mm)mm=st.darts[r].mirro;
            t+=rebuild_label(l,st.vertype)+"/"+rebuild_label(r,st.vertype)+"("+std::to_string(st.darts[r].polygon_size)+")-";
            ++cnt;l=st.darts[r].glue;if(l==cy)break;r=st.darts[l].rneig;}
        t.pop_back();int ratio=v/cnt;rp.push_back(ratio);
        if(ratio!=1)t="["+t+"]x"+std::to_string(ratio);
        ms.push_back(t);
        if(seen[mm]){ss.push_back(0);uc=false;}else{int l2=mm,r2=st.darts[l2].rneig;std::string t2;
            while(true){seen[l2]=1;t2+=rebuild_label(l2,st.vertype)+"/"+rebuild_label(r2,st.vertype)
                +"("+std::to_string(st.darts[r2].polygon_size)+")-";
                ++cnt;l2=st.darts[r2].glue;if(l2==mm)break;r2=st.darts[l2].rneig;}
            t2.pop_back();rp.push_back(ratio);
            if(ratio!=1)t2="["+t2+"]x"+std::to_string(ratio);
            ms.push_back(t2);ss.push_back(1);ss.push_back(2);}
    }
    std::string sh;
    for(int m=0;m<(int)ms.size();++m){int& s=ss[m];
        if(s==0)out<<m<<": "<<ms[m];
        else if(s==1){std::string h=uc?std::to_string(m/2)+": "
                        :std::to_string(m)+"/"+std::to_string(m+1)+": ";
                      sh=std::string(h.size(),' ');out<<h<<ms[m];}
        else { out<<sh<<ms[m]; }
        out<<"\n";
    }
    out<<"---\n";
    bool ic=uc;std::vector<std::string> wl,wle,wre,we;std::vector<int> wr;
    if(ic){for(int k=0;k<(int)ms.size()/2;++k){wl.push_back(ms[2*k]);wr.push_back(rp[2*k]);}}
    else{wl=ms;wr=rp;}
    for(int m=0;m<(int)wl.size();++m){std::string s=wl[m];int rep=wr[m];
        if(rep>1)s=s.substr(1,s.find(']')-1);
        s+="-";
        int rev=(int)s.size()-1;while(s[rev]!='/')--rev;s=s.substr(rev+1)+s.substr(0,rev+1);
        int ei=0;while(!s.empty()){size_t ind=s.find('/');std::string ch=s.substr(0,ind+1);s=s.substr(ind+1);
            size_t lp=ch.find('('),mi=ch.find('-');
            wle.push_back(ch.substr(0,lp));wre.push_back(ch.substr(mi+1,ch.size()-mi-2));
            std::string el=std::to_string(ei);if(m>3){el+="@";el+=std::to_string(m);}
            else{for(int t=0;t<m;++t)el+="'";}we.push_back(el);++ei;}}
    std::string cw;
    while(!wle.empty()){if(ic){auto rt=std::find(wre.begin(),wre.end(),wle[0]);
        if(rt==wre.end()){std::string mm=(wle[0][0]=='*')?wle[0].substr(1):"*"+wle[0];
            auto im=std::find(wle.begin(),wle.end(),mm);
            if(im==wle.end())cw+="["+we[0]+"]";else{int mi=(int)(im-wle.begin());
                cw+="["+we[0]+" "+we[mi]+"]";wle.erase(wle.begin()+mi);
                wre.erase(wre.begin()+mi);we.erase(we.begin()+mi);}
            wle.erase(wle.begin());wre.erase(wre.begin());we.erase(we.begin());continue;}
    }auto rt=std::find(wre.begin(),wre.end(),wle[0]);
    if(rt==wre.end()||rt==wre.begin()){cw+="("+we[0]+")";wle.erase(wle.begin());wre.erase(wre.begin());we.erase(we.begin());}
    else{int mi=(int)(rt-wre.begin());cw+="("+we[0]+" "+we[mi]+")";
        wle.erase(wle.begin()+mi);wre.erase(wre.begin()+mi);we.erase(we.begin()+mi);
        wle.erase(wle.begin());wre.erase(wre.begin());we.erase(we.begin());}}
    out<<cw<<"\n\n";
}

void EuclideanSolver::write_summary() {
    std::string path = output_dir_ + "/eu_final_results.txt";
    std::ofstream out(path);
    for (const auto& [code, count] : run_totals_) out << code << ": " << count << "\n";
    out << "\n";
    std::vector<std::pair<std::vector<int>, int>> ordered;
    for (const auto& [hist, count] : vertex_combos_) ordered.push_back({hist, count});
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        int sa=0,sb=0; for(int x:a.first)sa+=x; for(int x:b.first)sb+=x;
        if(sa!=sb) return sa<sb;
        for(size_t k=0;k<a.first.size();++k) if(a.first[k]!=b.first[k]) return -a.first[k]<-b.first[k];
        return false; });
    for (const auto& [hist, count] : ordered) {
        std::string line;
        for (int i = 0; i < NUM_VERTEX_TYPES; ++i) {
            if (!hist[i]) continue;
            if (!line.empty()) line += ", ";
            line += symbols[i]; if (hist[i] > 1) line += "x" + std::to_string(hist[i]);
        }
        out << line << ": " << count << "\n";
    }
}

void EuclideanSolver::run() {
    while (!queue_.empty()) {
        State st = unpack_state(queue_.front()); queue_.pop_front();
        extend(st); ++partials_checked_;
        if (partials_checked_ % 5000 == 0)
            std::cerr << "\r  [" << output_dir_ << "] " << partials_checked_
                      << " partials, " << solutions_found_ << " sols, Q="
                      << queue_.size() << "    " << std::flush;
    }
    std::cerr << "\r  [" << output_dir_ << "] done: " << partials_checked_
              << " partials, " << solutions_found_ << " sols                            \n";
    if (log_.is_open()) log_.close();
    write_summary();
}
