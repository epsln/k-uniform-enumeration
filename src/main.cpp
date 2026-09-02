#include "solver.h"
#include "disk_solver.h"
#include "pruner.h"
#include "vertex_catalog.h"
#include "metrics.h"
#include "zstd_stream.h"
#include "tes_store.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace catalog;

bool g_propagate = false;
bool g_binary_solutions = false;
bool g_no_spill = false;

// =============================================================================
// Shared work queue for parallel DFS  (batch-optimised)
// =============================================================================
struct SharedQueue {
	std::deque<PackedState> q;
	std::mutex mu;
	std::condition_variable cv;
	int active = 0;
	bool stop = false;

	static constexpr int BATCH = 16;

	int pop_batch(std::vector<State>& out) {
		out.clear();
		std::unique_lock<std::mutex> lk(mu);
		while (q.empty() && active > 0 && !stop)
			cv.wait(lk);
		if (q.empty()) { stop = true; cv.notify_all(); return 0; }
		int n = std::min(BATCH, (int)q.size());
		for (int i = 0; i < n; ++i) {
			out.push_back(EuclideanSolver::unpack_state(q.front()));
			q.pop_front();
		}
		active += n;
		return n;
	}

	void finish_batch(int completed, std::vector<State>& successors) {
		std::lock_guard<std::mutex> lk(mu);
		for (auto& s : successors) q.push_back(EuclideanSolver::pack_state(s));
		active -= completed;
		if (q.empty() && active == 0) stop = true;
		cv.notify_all();
	}

	int size_locked() const { return (int)q.size(); }
	int active_locked() const { return active; }
};

// =============================================================================
// Progress display
// =============================================================================
static std::string fmt_dur(double sec) {
	if (sec < 1) return "<1s";
	int s = (int)sec;
	if (s < 60)   return std::to_string(s) + "s";
	if (s < 3600) return std::to_string(s/60) + "m" + std::to_string(s%60) + "s";
	return std::to_string(s/3600) + "h" + std::to_string((s%3600)/60) + "m";
}

static std::string fmt_count(int64_t n) {
	if (n < 10000) return std::to_string(n);
	if (n < 1000000) return std::to_string(n/1000) + "K";
	return std::to_string(n/1000000) + "." + std::to_string((n/100000)%10) + "M";
}

static std::string fmt_rate(double r) {
	if (r < 1000) return std::to_string((int)r) + "/s";
	return std::to_string((int)(r/1000)) + "K/s";
}

// Current process RSS in kilobytes (reads /proc/self/status).
static long current_rss_kb() {
	std::ifstream f("/proc/self/status");
	std::string line;
	while (std::getline(f, line)) {
		if (line.rfind("VmRSS:", 0) == 0) {
			long kb = 0;
			std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb);
			return kb;
		}
	}
	return 0;
}
static std::string fmt_rss(long kb) { return fmt_count((int64_t)kb * 1024) + "B"; }

// Currently available RAM in GB (reads /proc/meminfo MemAvailable).
static long available_ram_gb() {
	std::ifstream f("/proc/meminfo");
	std::string line;
	while (std::getline(f, line)) {
		if (line.rfind("MemAvailable:", 0) == 0) {
			long kb = 0;
			std::sscanf(line.c_str(), "MemAvailable: %ld kB", &kb);
			return kb / 1024 / 1024;
		}
	}
	return 0;
}

// Time-to-completion estimate.
// The shared queue grows (expansion) then drains, so a single "queue/net-rate"
// figure is meaningless during growth.  We report:
//   - "--"  : queue empty (finishing)
//   - ".."  : rate still warming up
//   - ">"   : expansion/flat phase -> honest lower bound (time to drain the
//             current queue at the current processing rate, ignoring growth)
//   - plain : draining phase -> qsz / (rate - growth), an accurate ETA
static std::string eta_string(int64_t qsz, double rate, double growth) {
	if (qsz == 0) return "--";
	if (rate <= 10) return "..";
	double eta_floor = (double)qsz / rate;
	if (growth < -100) {                          // draining
		double net_rate = rate - growth;          // = rate + |growth|
		return fmt_dur((double)qsz / net_rate);
	}
	return ">" + fmt_dur(eta_floor);              // expanding / flat
}

// Rolling window of (time, value) samples for a stable rate estimate.
struct Window {
	std::deque<std::pair<double, int64_t>> pts;
	void add(double t, int64_t v, double keep = 30.0) {
		pts.emplace_back(t, v);
		while (pts.size() > 2 && pts.back().first - pts.front().first > keep)
			pts.pop_front();
	}
	double rate() const {
		if (pts.size() < 2) return 0.0;
		double dt = pts.back().first - pts.front().first;
		return dt > 0.5 ? (double)(pts.back().second - pts.front().second) / dt : 0.0;
	}
	double elapsed() const { return pts.empty() ? 0.0 : pts.back().first - pts.front().first; }
};

// Send a Telegram message via the Bot API (best-effort; silent on failure).
// In dry-run mode the message is printed to stderr instead of being sent.
static bool g_telegram_dry_run = false;
static void telegram_send(const std::string& token, const std::string& chat, const std::string& text) {
	if (token.empty() || chat.empty()) return;
	if (g_telegram_dry_run) {
		std::cerr << "\n[telegram] " << text << "\n";
		return;
	}
	std::string cmd = "curl -s -X POST \"https://api.telegram.org/bot" + token + "/sendMessage\""
		" --data-urlencode \"chat_id=" + chat + "\""
		" --data-urlencode \"text=" + text + "\" >/dev/null 2>&1";
	std::system(cmd.c_str());
}

// Write the completion marker (silences the watchdog) and send the final
// summary with per-k tiling counts.
static void finish_notify(const std::map<int,int>& counts, const std::string& output_dir,
		const std::string& token, const std::string& chat) {
	{
		std::ofstream m(output_dir + "/.finished");
		m << "1\n";
	}
	if (token.empty() || chat.empty()) return;
	std::string msg = "✅ eusolver finished\n";
	int total = 0;
	for (const auto& [k, c] : counts) {
		msg += "k=" + std::to_string(k) + " → " + std::to_string(c) + "\n";
		total += c;
	}
	msg += "total → " + std::to_string(total);
	telegram_send(token, chat, msg);
}

static void progress_thread_fn(SharedQueue& sq,
		std::atomic<int64_t>& total_partials,
		std::atomic<int64_t>& total_solutions,
		std::atomic<bool>& running) {
	using namespace std::chrono;
	auto t0 = steady_clock::now();
	auto last_tick = t0;
	double ema_rate = 0;       // EWMA of partials/second
	double ema_growth = 0;     // EWMA of queue growth/second
	const double alpha = 0.3;
	int64_t last_p = 0;
	int64_t last_q = 0;
	bool first_tick = true;

	while (running) {
		std::this_thread::sleep_for(milliseconds(500));
		auto now = steady_clock::now();
		double dt = duration<double>(now - last_tick).count();
		last_tick = now;

		int64_t p = total_partials.load();
		int64_t delta_p = p - last_p;
		last_p = p;

		int qsz = 0;
		{ std::lock_guard<std::mutex> lk(sq.mu);
			qsz = sq.size_locked(); }

		if (!first_tick && dt > 0.001) {
			double rate = delta_p / dt;
			ema_rate = (ema_rate == 0) ? rate : alpha * rate + (1 - alpha) * ema_rate;

			double growth = (qsz - last_q) / dt;
			ema_growth = (ema_growth == 0) ? growth : alpha * growth + (1 - alpha) * ema_growth;
		} else if (first_tick && dt > 0.001) {
			ema_rate = delta_p / dt;   // seed with first real measurement
			ema_growth = (qsz - last_q) / dt;
		}
		last_q = qsz;
		first_tick = false;

		// Queue trend indicator
		char qsign = (ema_growth > 100) ? '+' : (ema_growth < -100) ? '-' : ' ';
		double elapsed = duration<double>(now - t0).count();

		std::cerr << "\r  T:" << fmt_dur(elapsed)
			<< "  P:" << fmt_count(p)
			<< "  S:" << total_solutions
			<< "  Q:" << qsz << qsign
			<< "  R:" << fmt_rate(ema_rate)
			<< "  ETA:" << eta_string(qsz, ema_rate, ema_growth)
			<< "     " << std::flush;
	}
	std::cerr << "\r" << std::string(70, ' ') << "\r" << std::flush;
}

static void merge_worker_outputs(const std::string& output_dir,
		const std::vector<std::string>& worker_dirs) {
	std::map<std::string, std::vector<std::string>> per_combo;
	for (const auto& wd : worker_dirs) {
		if (!fs::exists(wd)) continue;
		for (const auto& entry : fs::directory_iterator(wd)) {
			if (!entry.is_regular_file()) continue;
			std::string fname = entry.path().filename().string();
			if (fname.rfind("eusolver_", 0) != 0) continue;
			std::string base = fname;
			if (has_zst_suffix(base)) base.resize(base.size() - 4);
			per_combo[base].push_back(entry.path().string());
		}
	}
	for (const auto& [base, paths] : per_combo) {
		// zstd frames concatenate, so .zst sources are byte-copied as-is;
		// plain sources are concatenated and compressed right after this call.
		bool zst = !paths.empty() && has_zst_suffix(paths.front());
		std::string dest = output_dir + "/" + base + (zst ? ".zst" : "");
		std::ofstream out(dest, std::ios::binary | std::ios::app);
		for (const auto& p : paths) {
			std::ifstream in(p, std::ios::binary);
			out << in.rdbuf();
		}
	}
}

static void compress_merged_solutions(const std::string& output_dir) {
	for (const auto& entry : fs::directory_iterator(output_dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (fname.rfind("eusolver_", 0) == 0 && !has_zst_suffix(fname))
			compress_to_zst(entry.path().string());
	}
}

static std::map<int,int> run_pruner(const std::string& output_dir, int num_workers) {
	std::vector<std::string> solution_files;
	for (const auto& entry : fs::directory_iterator(output_dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (fname.rfind("eusolver_", 0) == 0)
			solution_files.push_back(entry.path().string());
	}
	std::sort(solution_files.begin(), solution_files.end());
	std::map<int,int> counts;
	{
		std::string pruned_dir = output_dir + "/wl";
		fs::remove(pruned_dir + "/tilings.sqlite3.tmp");
		fs::remove(pruned_dir + "/tilings.sqlite3.tmp-journal");
		fs::remove(pruned_dir + "/tilings.sqlite3.tmp-wal");
		fs::remove(pruned_dir + "/tilings.sqlite3.tmp-shm");
		WLPruner pruner(pruned_dir, num_workers);
		pruner.run(solution_files);
		int total = 0;
		for (const auto& [k, count] : pruner.solutions_per_k()) {
			std::cout << "  k=" << k << " -> " << count << " unique tiling"
				<< (count != 1 ? "s" : "") << "\n";
			total += count;
			counts[k] = count;
		}
		std::cout << "  total: " << total << "\n";
	}
	return counts;
}

static void clear_old_files(const std::string& dir) {
	if (!fs::exists(dir)) return;
	for (const auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (fname.rfind("eusolver_", 0) == 0)
			fs::remove(entry.path());
	}
}

int main(int argc, char** argv) {
	int max_polygons = 5;
	int num_workers = (int)std::thread::hardware_concurrency();
	if (num_workers < 1) num_workers = 4;
	std::string output_dir = "solutions";
	std::string mode = "memory";
	int fanout_target = 0;    // 0 = auto-scale
	int spill_threshold = 0;  // 0 = auto-scale
	int chunks_user = 0;      // 0 = auto (num_workers * 32)
	int pdedup_mode = 0;  // 0=per-worker, 1=global shared
	std::string dedup_mode  = "wl";
	int64_t sol_dedup_cap_user = 0;  // 0 = auto-scale
	int max_ram_gb = 0;  // 0 = auto (k-based formula)
	bool resume = false;
	std::string extract_database, extract_output;
	bool prune_only = false;
	int64_t extract_id = 0;
	std::string telegram_token, telegram_chat;
	int notify_minutes = 30;
	g_propagate = true;

	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--max-polygons" && i + 1 < argc) max_polygons = std::stoi(argv[++i]);
		else if (arg == "--workers" && i + 1 < argc) num_workers = std::stoi(argv[++i]);
		else if (arg == "--output" && i + 1 < argc) output_dir = argv[++i];
		else if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
		else if (arg == "--wl-dim" && i + 1 < argc) { int d = std::stoi(argv[++i]); g_wl_dim = (d == 2 ? 2 : 1); }
		else if (arg == "--wl-iters" && i + 1 < argc) g_wl_iters = std::stoi(argv[++i]);
		else if (arg == "--pdedup" && i + 1 < argc) { std::string v = argv[++i]; pdedup_mode = (v == "shared" ? 1 : 0); }
		else if (arg == "--dedup" && i + 1 < argc) {
			std::string v = argv[++i];
			if (v == "bfl") { dedup_mode = "bfl"; g_use_bfl = true; }
			else { dedup_mode = "wl"; g_use_bfl = false; }
		}
		else if (arg == "--no-iso-check") { g_no_iso_check = true; }
		else if (arg == "--prune-only") { prune_only = true; }
		else if (arg == "--keep-pruner-inputs") { g_keep_pruner_inputs = true; }
		else if (arg == "--propagate") { g_propagate = true; }
		else if (arg == "--binary-solutions") { g_binary_solutions = true; }
		else if (arg == "--no-spill") { g_no_spill = true; }
		else if (arg == "--fanout" && i + 1 < argc) fanout_target = std::stoi(argv[++i]);
		else if (arg == "--spill" && i + 1 < argc) spill_threshold = std::stoi(argv[++i]);
		else if (arg == "--chunks" && i + 1 < argc) chunks_user = std::stoi(argv[++i]);
		else if (arg == "--sol-dedup-cap" && i + 1 < argc) sol_dedup_cap_user = std::stoll(argv[++i]);
		else if (arg == "--max-ram-gb" && i + 1 < argc) max_ram_gb = std::stoi(argv[++i]);
		else if (arg == "--resume") { resume = true; }
		else if (arg == "--extract-tes" && i + 1 < argc) extract_database = argv[++i];
		else if (arg == "--extract-output" && i + 1 < argc) extract_output = argv[++i];
		else if (arg == "--tes-id" && i + 1 < argc) extract_id = std::stoll(argv[++i]);
		else if (arg == "--compress-solutions") { g_compress_solutions = true; g_compress_pruner_outputs = true;}
		else if (arg == "--compress-threshold-mb" && i + 1 < argc)
			g_compress_threshold = (int64_t)std::stoll(argv[++i]) * 1024 * 1024;
		else if (arg == "--telegram-token" && i + 1 < argc) telegram_token = argv[++i];
		else if (arg == "--telegram-chat" && i + 1 < argc) telegram_chat = argv[++i];
		else if (arg == "--notify-minutes" && i + 1 < argc) notify_minutes = std::stoi(argv[++i]);
		else if (arg == "--telegram-dry-run") { g_telegram_dry_run = true; }
		else if (arg == "--help") {
			std::cout << "Usage: eusolver [options]\n"
				<< "  --max-polygons N   max vertex types (default: 5)\n"
				<< "  --workers N        number of threads (default: hw)\n"
				<< "  --output DIR       output directory (default: solutions)\n"
				<< "  --mode memory|disk solver mode (default: memory)\n"
				<< "  --wl-dim 1|2       WL hash dimension (default: 1)\n"
				<< "  --wl-iters N       WL iteration cap (default: 0 = iterate to convergence)\n"
				<< "  --pdedup local|shared  partial dedup scope (default: local)\n"
				<< "  --dedup wl|bfl       online solver dedup mode (default: wl)\n"
				<< "  --no-iso-check      trust the WL hash, skip the O(n^2) isomorphism fallback\n"
				<< "  --prune-only        prune existing eusolver_* files in --output\n"
				<< "  --keep-pruner-inputs preserve raw solution files after pruning\n"
				<< "  --fanout N         BFS fan-out target (0=auto, default: 40000)\n"
				<< "  --spill N          disk spill threshold (0=auto, default: 50000)\n"
				<< "  --chunks N         number of frontier chunks (0=auto, default: workers*32)\n"
				<< "  --sol-dedup-cap N  online solution dedup cap: total stored entries per worker (0=auto)\n"
				<< "  --max-ram-gb N     RAM budget for queue spill/dedup auto-scaling (0=auto: 70% of free RAM)\n"
				<< "  --binary-solutions write solutions as binary .bin files\n"
				<< "  --no-spill          keep all partial states in RAM (no disk spill)\n"
				<< "  --compress-solutions  compress worker .bin output with zstd\n"
				<< "  --compress-threshold-mb N  mid-run compression threshold (default 256)\n"
				<< "  --extract-tes DB   extract .tes records from a pruner SQLite database\n"
				<< "  --extract-output DIR  extraction directory (required with --extract-tes)\n"
				<< "  --tes-id N         extract only this database entry id (default: all)\n"
				<< "  --telegram-token T  Telegram bot token for progress updates\n"
				<< "  --telegram-chat C   Telegram chat id to message\n"
				<< "  --notify-minutes N  interval between Telegram updates (default 30)\n"
				<< "  --telegram-dry-run  print messages to stderr instead of sending\n"
				<< "  --help             show this help\n";
			return 0;
		}
		else {
			std::cerr << "Unknown option: " << arg << "\n";
			return 1;
		}
	}
	if (num_workers < 1) num_workers = 1;
	if (!extract_database.empty()) {
		if (extract_output.empty()) {
			std::cerr << "--extract-output is required with --extract-tes\n";
			return 1;
		}
		try {
			int64_t count = extract_tes_database(extract_database, extract_output, extract_id);
			std::cout << "Extracted " << count << " .tes file" << (count == 1 ? "" : "s")
			          << " to " << extract_output << "\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "TES extraction failed: " << e.what() << "\n";
			return 1;
		}
	}
	if (prune_only) {
		try {
			auto start = std::chrono::steady_clock::now();
			run_pruner(output_dir, num_workers);
			auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			std::cout << "Pruner phase:  " << std::fixed << std::setprecision(1) << elapsed << "s\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "Pruner failed: " << e.what() << "\n";
			return 1;
		}
	}

	// Auto-scale thresholds
	if (fanout_target <= 0)  fanout_target  = std::max(5000, num_workers * 1000);

	// Auto-detect a safe RAM budget when --max-ram-gb is not given: use 70% of
	// currently available RAM so the solver never crowds out other processes
	// (the cause of the k=20 OOMs).
	if (max_ram_gb <= 0) {
		long avail = available_ram_gb();
		if (avail > 0) max_ram_gb = std::max(4, (int)(avail * 7 / 10));
	}

	if (max_ram_gb > 0) {
		int64_t ram_bytes = (int64_t)max_ram_gb * 1000000000LL;
		// PackedState size ≈ vector overhead + malloc overhead (~64B) + vertype
		// (k B) + glue (2 B per dart).  Use the max 12 slots/vertex so the spill
		// threshold never underestimates the RAM the queue actually uses.
		int64_t per_entry = 64 + max_polygons + 2LL * max_polygons * 12;
		// Online dedup entry size: BFL stores a 32-byte inline key (~80B with
		// node + bucket array + malloc overhead); WL stores a full PackedState
		// plus unordered_map node/key overhead.
		int64_t dedup_entry = g_use_bfl ? 80 : (per_entry + 200);

		// Partition the budget: 25% queue spill, 40% online dedup, 35% headroom.
		int64_t queue_budget = ram_bytes / 4;
		int64_t dedup_budget = ram_bytes * 2 / 5;

		if (spill_threshold <= 0)
			spill_threshold = (int)std::max((int64_t)50000, queue_budget / num_workers / per_entry);
		if (sol_dedup_cap_user <= 0)
			sol_dedup_cap_user = std::max((int64_t)10000, dedup_budget / num_workers / dedup_entry);
	} else {
		if (spill_threshold <= 0)
			spill_threshold = std::max(50000, 500000 * 64 / (max_polygons * max_polygons));
		if (sol_dedup_cap_user <= 0) sol_dedup_cap_user = 100000;
	}

	std::cout << "Mode: " << mode << "  |  k=" << max_polygons
		<< "  |  Workers: " << num_workers
		<< "  |  Dedup: " << dedup_mode
		<< "  |  ram-budget: " << max_ram_gb << "G"
		<< "  |  spill: " << spill_threshold
		<< "  |  dedup cap: " << sol_dedup_cap_user
		<< "\n\n";

	// Remove any stale completion marker (from a previous run) so the watchdog
	// doesn't mistake this run for already-finished.
	fs::remove(output_dir + "/.finished");

	// Telegram: announce start + spawn a detached watchdog that alerts if this
	// process dies before writing the .finished marker (covers OOM/SIGKILL).
	if (!telegram_token.empty() && !telegram_chat.empty()) {
		std::string msg = "🚀 eusolver started\n"
			"k=" + std::to_string(max_polygons)
			+ " · workers=" + std::to_string(num_workers)
			+ " · mode=" + mode
			+ " · dedup=" + dedup_mode + "\n"
			"spill=" + std::to_string(spill_threshold)
			+ " · dedup-cap=" + std::to_string(sol_dedup_cap_user);
		telegram_send(telegram_token, telegram_chat, msg);

		pid_t parent_pid = getpid();
		if (fork() == 0) {
			setsid();  // detach from the controlling terminal / parent group
			std::string done_marker = output_dir + "/.finished";
			while (true) {
				sleep(30);
				if (kill(parent_pid, 0) != 0) {          // parent gone
					if (!fs::exists(done_marker)) {
						std::string dead = "⚠️ eusolver died unexpectedly\nk="
							+ std::to_string(max_polygons) + " · pid="
							+ std::to_string((long)parent_pid);
						std::ifstream st(output_dir + "/last_status.txt");
						std::string line;
						if (std::getline(st, line) && !line.empty())
							dead += "\n" + line;
						telegram_send(telegram_token, telegram_chat, dead);
					}
					_exit(0);
				}
			}
		}
	}

	auto t0 = std::chrono::steady_clock::now();

	if (mode == "disk") {
		set_pdedup_shared(pdedup_mode == 1);
		fs::create_directories(output_dir);

		std::vector<std::string> chunk_paths;
		int n_chunks = chunks_user > 0 ? chunks_user : num_workers * 32;
		auto t1 = std::chrono::steady_clock::now();

		if (!resume) {
			// Clear old solver output files
			for (const auto& entry : fs::directory_iterator(output_dir)) {
				if (entry.is_regular_file() && entry.path().filename().string().rfind("eusolver_",0)==0)
					fs::remove(entry.path());
			}
			// Phase 1: BFS fan-out
			std::cout << "Phase 1: BFS fan-out to " << fanout_target << " frontier states...\n";
			std::vector<State> early_solutions;
			std::vector<State> frontier = bfs_fanout(fanout_target, max_polygons, &early_solutions);
			t1 = std::chrono::steady_clock::now();
			std::cout << "  Done: " << frontier.size() << " frontier, "
			          << early_solutions.size() << " early solutions  ("
			          << std::fixed << std::setprecision(1)
			          << std::chrono::duration<double>(t1 - t0).count() << "s)\n";

			// Write early solutions
			{
				std::mutex mu;
				std::map<std::string,int> rt;
				std::map<std::string,std::string> sf;
				HistogramMap vc;
				for (auto& s : early_solutions)
					EuclideanSolver::write_solution_static(s, output_dir, mu, rt, sf, vc);
			}
			std::cout << "  Early solutions written.\n";

			// Write frontier as chunks
			std::vector<std::vector<State>> chunks(n_chunks);
			for (size_t i = 0; i < frontier.size(); ++i)
				chunks[i % n_chunks].push_back(std::move(frontier[i]));
			{ std::vector<State>().swap(frontier); }

			for (int ci = 0; ci < n_chunks; ++ci) {
				if (chunks[ci].empty()) continue;
				std::string cp = output_dir + "/chunk_" + std::to_string(ci) + ".bin";
				write_states_bin(cp, chunks[ci]);
				chunk_paths.push_back(cp);
			}
		} else {
			// Resume: reuse existing chunk files
			std::cout << "Resuming from " << output_dir << "...\n";
			for (int ci = 0; ci < n_chunks; ++ci) {
				std::string cp = output_dir + "/chunk_" + std::to_string(ci) + ".bin";
				if (fs::exists(cp)) chunk_paths.push_back(cp);
			}
			std::cout << "  Found " << chunk_paths.size() << " chunk files\n";
		}

		// Phase 2: parallel disk workers (shared atomic chunk pool)
		std::cout << "Phase 2: " << n_chunks << " chunks, " << num_workers
		          << " workers (disk DFS)...\n";
		std::vector<std::string> worker_dirs;
		for (int w = 0; w < num_workers; ++w)
			worker_dirs.push_back(output_dir + "/_worker_" + std::to_string(w));

		// Shared progress for disk workers
		for (int w = 0; w < MAX_WORKERS; ++w) {
			g_disk_partials[w].store(0);
			g_disk_solutions[w].store(0);
			g_disk_queue[w].store(0);
			g_disk_spilled[w].store(0);
		}
		g_disk_running.store(true);
		std::atomic<int> next_chunk{0};

		const std::string status_path = output_dir + "/status.json";
		const bool is_tty = isatty(STDERR_FILENO);
		MetricsWriter metrics(output_dir + "/metrics");

		std::thread disk_progress([&, status_path, is_tty]() {
			using namespace std::chrono;
			auto t0 = steady_clock::now();
			double wall_base = (double)system_clock::now().time_since_epoch().count() / 1e9;
			Window total_window;
			std::vector<Window> wwin(num_workers);
			int prev_lines = 0;
			int tick = 0;
			double last_notify = -1e9;

			while (g_disk_running.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(milliseconds(500));
				auto now = steady_clock::now();
				double t = duration<double>(now - t0).count();
				++tick;

				int64_t P = 0, S = 0, Q = 0, Qram = 0, Qspill = 0;
				std::vector<int64_t> wp(num_workers), ws(num_workers), wq(num_workers);
				for (int w = 0; w < num_workers; ++w) {
					int64_t pw = g_disk_partials[w].load(std::memory_order_relaxed);
					int64_t sw = g_disk_solutions[w].load(std::memory_order_relaxed);
					int64_t qw_ram = g_disk_queue[w].load(std::memory_order_relaxed);
					int64_t qw_spill = g_disk_spilled[w].load(std::memory_order_relaxed);
					int64_t qw = qw_ram + qw_spill;
					wp[w] = pw; ws[w] = sw; wq[w] = qw;
					P += pw; S += sw; Q += qw; Qram += qw_ram; Qspill += qw_spill;
					wwin[w].add(t, pw);
				}
				total_window.add(t, P);

				double rate = total_window.rate();
				// Remaining work lives mostly in the not-yet-processed chunks
				// (the per-worker queue is only a thin BFS slice).  Estimate it
				// as (unclaimed chunks * average chunk cost) + current queue.
				int claimed = next_chunk.load(std::memory_order_relaxed);
				double eta_s = 0.0;
				if (rate > 1.0) {
					double remaining = (double)Q;
					if (claimed > 0 && claimed < n_chunks) {
						double avg_chunk = (double)P / (double)claimed;
						remaining += (double)(n_chunks - claimed) * avg_chunk;
					}
					eta_s = remaining / rate;
				}

				std::string eta_str;
				if (Q == 0 && claimed >= n_chunks) eta_str = "--";
				else if (rate <= 1.0) eta_str = "..";
				else if (eta_s < 1.0) eta_str = "<1s";
				else eta_str = fmt_dur(eta_s);

				std::string summary = "  T:" + fmt_dur(t)
					+ "  P:" + fmt_count(P)
					+ "  S:" + fmt_count(S)
					+ "  Q:" + fmt_count(Q)
					+ "  R:" + fmt_rate(rate)
					+ "  ETA:" + eta_str
					+ "  RSS:" + fmt_rss(current_rss_kb());

				std::ostringstream buf;
				if (is_tty) {
					if (prev_lines > 0) buf << "\033[" << prev_lines << "A";
					buf << summary << "\n";
					for (int w = 0; w < num_workers; ++w) {
						buf << "  W" << w
							<< "  P:" << fmt_count(wp[w])
							<< "  S:" << fmt_count(ws[w])
							<< "  Q:" << fmt_count(wq[w])
							<< "  R:" << fmt_rate(wwin[w].rate()) << "\n";
					}
					buf << "\033[J";
					prev_lines = num_workers + 1;
				} else if (tick % 60 == 0) {
					buf << summary << "\n";
				}
				std::cerr << buf.str() << std::flush;

				if (tick % 4 == 0) {
					std::ofstream f(status_path + ".tmp");
					f << "{\"elapsed_s\":" << (int64_t)t
					  << ",\"partials\":" << P
					  << ",\"solutions\":" << S
					  << ",\"queue\":" << Q
					  << ",\"rate\":" << (int64_t)rate
					  << ",\"eta_s\":" << (int64_t)eta_s
					  << ",\"rss_kb\":" << current_rss_kb()
					  << ",\"workers\":[";
					for (int w = 0; w < num_workers; ++w) {
						if (w) f << ",";
						f << "{\"p\":" << wp[w] << ",\"s\":" << ws[w] << ",\"q\":" << wq[w] << "}";
					}
					f << "]}\n";
					f.close();
					fs::rename(status_path + ".tmp", status_path);

					std::ofstream ls(output_dir + "/last_status.txt", std::ios::trunc);
					ls << "elapsed " << fmt_dur(t) << " · ETA " << eta_str
					   << " · RSS " << fmt_rss(current_rss_kb()) << "\n"
					   << "P " << fmt_count(P) << " · S " << fmt_count(S)
					   << " · Q " << fmt_count(Q) << " · R " << fmt_rate(rate) << "\n";
					ls.close();
					// TensorBoard metrics
					double wall = wall_base + t;
					metrics.write_scalar("partials", tick, wall, (double)P);
					metrics.write_scalar("solutions", tick, wall, (double)S);
					metrics.write_scalar("queue", tick, wall, (double)Q);
					metrics.write_scalar("queue_in_ram", tick, wall, (double)Qram);
					metrics.write_scalar("queue_spilled", tick, wall, (double)Qspill);
					metrics.write_scalar("rss_mb", tick, wall, (double)(current_rss_kb() / 1024));
					metrics.write_scalar("rate", tick, wall, rate);
					metrics.write_scalar("eta_s", tick, wall, eta_s);
					for (int w = 0; w < num_workers; ++w) {
						std::string wtag = std::to_string(w);
						metrics.write_scalar("worker_" + wtag + "/partials", tick, wall, (double)wp[w]);
						metrics.write_scalar("worker_" + wtag + "/solutions", tick, wall, (double)ws[w]);
						metrics.write_scalar("worker_" + wtag + "/queue", tick, wall, (double)wq[w]);
						metrics.write_scalar("worker_" + wtag + "/rate", tick, wall, wwin[w].rate());
					}
				}

				if (!telegram_token.empty() && !telegram_chat.empty()
					&& t - last_notify >= (double)notify_minutes * 60.0) {
					last_notify = t;
					std::ostringstream m;
					m << "⏱ " << fmt_dur(t) << " · ETA " << eta_str
					  << " · RSS " << fmt_rss(current_rss_kb()) << "\n"
					  << "P " << fmt_count(P) << " · S " << fmt_count(S)
					  << " · Q " << fmt_count(Q) << " · R " << fmt_rate(rate) << "\n";
					for (int w = 0; w < num_workers; ++w) {
						m << "W" << w << "  P " << fmt_count(wp[w])
						  << "  S " << fmt_count(ws[w])
						  << "  Q " << fmt_count(wq[w])
						  << "  R " << fmt_rate(wwin[w].rate()) << "\n";
					}
					telegram_send(telegram_token, telegram_chat, m.str());
				}
			}
			std::cerr << "\r" << std::string(70, ' ') << "\r" << std::flush;
		});

		std::vector<std::thread> threads;
		for (int w = 0; w < num_workers; ++w) {
			bool nospill = g_no_spill;
			threads.emplace_back([&, w, nospill]() {
				disk_solver_worker(chunk_paths, next_chunk,
					worker_dirs[w], max_polygons,
					nospill ? 0x7fffffff : spill_threshold,
					sol_dedup_cap_user, w);
			});
		}
		for (auto& t : threads) t.join();
		g_disk_running.store(false);
		disk_progress.join();
		for (const auto& cp : chunk_paths) fs::remove(cp);

		auto t2 = std::chrono::steady_clock::now();
		std::cout << "Solver phase: " << std::fixed << std::setprecision(1)
		          << std::chrono::duration<double>(t2 - t1).count() << "s\n";

		merge_worker_outputs(output_dir, worker_dirs);
		for (const auto& wd : worker_dirs) fs::remove_all(wd);
		compress_merged_solutions(output_dir);

		std::cout << "Pruner phase starting...\n";
		auto counts = run_pruner(output_dir, num_workers);
		std::cout << "\nPruner phase:  " << std::fixed << std::setprecision(1)
		          << std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count() << "s\n";
		finish_notify(counts, output_dir, telegram_token, telegram_chat);

		} else {
			// === In-memory pipeline with shared work queue ===
			clear_old_files(output_dir);
			fs::create_directories(output_dir);
			EuclideanSolver::reset_global_counters();

			// Shared queue and per-worker output tracking
			SharedQueue sq;
			{
				std::vector<State> init_batch;
				for (int vt = 0; vt < NUM_VERTEX_TYPES; ++vt)
					init_batch.push_back(EuclideanSolver::make_initial(vt));
				sq.finish_batch(0, init_batch);
			}

			// Each worker writes to its own directory for thread safety
			std::vector<std::string> worker_dirs;
			for (int w = 0; w < num_workers; ++w)
				worker_dirs.push_back(output_dir + "/_worker_" + std::to_string(w));

			// Shared counters (updated by workers, read by progress thread)
			std::atomic<int64_t> total_partials{0}, total_solutions{0};
			std::atomic<int64_t> total_deduped{0}, total_sol_deduped{0};
			std::atomic<bool> solver_running{true};

			std::thread progress(progress_thread_fn, std::ref(sq),
					std::ref(total_partials),
					std::ref(total_solutions),
					std::ref(solver_running));

			std::vector<std::thread> threads;

			// Online dedup: per-worker WL hash set (no mutex, no shared state)
			int64_t sol_dedup_cap = sol_dedup_cap_user > 0
				? sol_dedup_cap_user
				: 200000 * 64 / (max_polygons * max_polygons);

			for (int w = 0; w < num_workers; ++w) {
				threads.emplace_back([&, w]() {
						const std::string& wdir = worker_dirs[w];
						OnlineDedup local_dedup(sol_dedup_cap); // per-worker, capped + sliding-window
						fs::create_directories(wdir);
						std::mutex mu;
						std::map<std::string,int> rt;
						std::map<std::string,std::string> sf;
						HistogramMap vc;
						int64_t pd_seen = 0, pd_deduped = 0;
						int64_t solutions = 0, sol_accepted = 0;
						std::vector<State> local_batch;
						std::vector<State> work_items;

						while (true) {
						int nb = sq.pop_batch(work_items);
						if (nb == 0) break;

						local_batch.clear();
						for (auto& st : work_items) {
						total_partials.fetch_add(1);
						EuclideanSolver::extend_into(st, [&](State&& cand) {
								bool done = true;
								for (const auto& d : cand.darts) if (d.glue == -1) { done = false; break; }
								if (done) {
								++solutions;
								if (local_dedup.check_and_remember(cand)) return;
								++sol_accepted;
								EuclideanSolver::write_solution_static(cand, wdir, mu,
										rt, sf, vc);
								} else {
								++pd_seen;
								local_batch.push_back(std::move(cand));
								}
								}, max_polygons);
						}
						sq.finish_batch(nb, local_batch);
						total_solutions.fetch_add(sol_accepted);
						total_sol_deduped.fetch_add(solutions - sol_accepted);
						total_deduped.fetch_add(pd_deduped);
						solutions = 0; sol_accepted = 0; pd_deduped = 0;
						}
						total_solutions.fetch_add(sol_accepted);
						total_sol_deduped.fetch_add(solutions - sol_accepted);
						total_deduped.fetch_add(pd_deduped);
				});
			}
			for (auto& t : threads) t.join();
			solver_running = false;
			progress.join();

			int64_t tp = total_partials.load();
			int64_t ts = total_solutions.load();
			int64_t td = total_deduped.load();
			int64_t tsd = total_sol_deduped.load();

			auto t1 = std::chrono::steady_clock::now();
			std::cout << "\nSolver phase: " << std::fixed << std::setprecision(1)
				<< std::chrono::duration<double>(t1 - t0).count() << "s  ("
				<< tp << " partials, " << ts
				<< " sols, dd " << td << "/" << tsd << ")\n";

			merge_worker_outputs(output_dir, worker_dirs);
			for (const auto& wd : worker_dirs) fs::remove_all(wd);
			compress_merged_solutions(output_dir);

			std::cout << "Pruner phase starting...\n";
			auto counts = run_pruner(output_dir, num_workers);
			auto t2 = std::chrono::steady_clock::now();
			std::cout << "\nPruner phase:  " << std::fixed << std::setprecision(1)
				<< std::chrono::duration<double>(t2 - t1).count() << "s\n";
			finish_notify(counts, output_dir, telegram_token, telegram_chat);
		}

		std::cout << "\nNOTE: This enumeration omits the (4,8,8) vertex type;\n"
			<< "expected k=1 count is 10 (not 11 from the literature).\n";
		return 0;
	}
