#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct SharedData { int A = 0, B = 0, C = 0, version = 0; };

enum class LockMode { Global, PerRecord };
enum class Distribution { Uniform, Biased };

struct Config {
    LockMode lockMode = LockMode::PerRecord;
    Distribution distribution = Distribution::Uniform;
    int readers = 3, writers = 2, operations = 200;
    unsigned long long seed = 11088;
    int workUs = 0, timeoutMs = 30000;
    std::string csv = "data/results.csv";
    std::string events = "data/events.csv";
    bool quiet = false, logEvents = true;
};

static constexpr int RECORDS = 16;
SharedData records[RECORDS];
SRWLOCK globalLock = SRWLOCK_INIT;
SRWLOCK recordLocks[RECORDS];
std::mutex outputMutex, eventMutex, csvMutex;
std::atomic<bool> stopRequested{ false };
std::atomic<int> invariantErrors{ 0 };
std::atomic<long long> readOps{ 0 }, writeOps{ 0 };
std::atomic<long long> readWaitNs{ 0 }, writeWaitNs{ 0 };
std::atomic<long long> maxWaitNs{ 0 };
std::atomic<long long> completed{ 0 };
std::atomic<long long> accesses[RECORDS];
std::chrono::steady_clock::time_point runStart;
std::chrono::steady_clock::time_point deadline;
Config cfg;
std::string runId;

std::string nowOffsetUs() {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - runStart).count();
    return std::to_string(us);
}
const char* modeName() { return cfg.lockMode == LockMode::Global ? "global" : "per-record"; }
const char* distName() { return cfg.distribution == Distribution::Uniform ? "uniform" : "biased"; }

void event(int worker, DWORD tid, const char* state, int resource) {
    if (!cfg.logEvents) return;
    std::lock_guard<std::mutex> guard(eventMutex);
    std::ofstream f(cfg.events, std::ios::app);
    if (!f) return;
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - runStart).count();
    f << '[' << std::setw(6) << std::setfill('0') << ms << " ms] " << worker << ',' << tid << ','
        << state << ',' << resource << '\n';
}
void consoleState(int worker, DWORD tid, const char* state) {
    if (cfg.quiet) return;
    std::lock_guard<std::mutex> guard(outputMutex);
    std::cout << (worker < cfg.readers ? "READER " : "WRITER ")
        << worker << " | TID=" << tid << " | STATE=" << state << '\n';
}

bool acquireLock(int index, bool write, int worker, DWORD tid) {
    auto begin = std::chrono::steady_clock::now();
    event(worker, tid, "WAITING", index);
    consoleState(worker, tid, "WAITING");
    bool acquired = false;
    while (!stopRequested.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            stopRequested.store(true);
            break;
        }
        if (cfg.lockMode == LockMode::Global) {
            acquired = write ? TryAcquireSRWLockExclusive(&globalLock)
                : TryAcquireSRWLockShared(&globalLock);
        }
        else {
            acquired = write ? TryAcquireSRWLockExclusive(&recordLocks[index])
                : TryAcquireSRWLockShared(&recordLocks[index]);
        }
        if (acquired) break;
        Sleep(0);
    }
    auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    (write ? writeWaitNs : readWaitNs).fetch_add(waited);
    long long old = maxWaitNs.load();
    while (waited > old && !maxWaitNs.compare_exchange_weak(old, waited)) {}
    if (acquired) {
        event(worker, tid, "ACQUIRED", index);
        consoleState(worker, tid, "RUNNING");
    }
    return acquired;
}
void releaseLock(int index, bool write, int worker, DWORD tid) {
    if (cfg.lockMode == LockMode::Global) {
        if (write) ReleaseSRWLockExclusive(&globalLock);
        else ReleaseSRWLockShared(&globalLock);
    }
    else {
        if (write) ReleaseSRWLockExclusive(&recordLocks[index]);
        else ReleaseSRWLockShared(&recordLocks[index]);
    }
    event(worker, tid, "RELEASED", index);
}
int chooseRecord(std::mt19937_64& rng) {
    if (cfg.distribution == Distribution::Uniform) {
        return std::uniform_int_distribution<int>(0, RECORDS - 1)(rng);
    }
    if (std::uniform_int_distribution<int>(1, 100)(rng) <= 80)
        return std::uniform_int_distribution<int>(0, 1)(rng);
    return std::uniform_int_distribution<int>(2, RECORDS - 1)(rng);
}
void worker(int workerId, bool write, int operations) {
    DWORD tid = GetCurrentThreadId();
    consoleState(workerId, tid, "RUNNING");
    event(workerId, tid, "WORKING", -1);
    std::mt19937_64 rng(cfg.seed + static_cast<unsigned long long>(workerId) * 1000003ULL);
    for (int n = 0; n < operations && !stopRequested.load(); ++n) {
        int index = chooseRecord(rng);
        if (!acquireLock(index, write, workerId, tid)) break;
        if (write) {
            int next = records[index].A + 1;
            records[index].A = next;
            records[index].B = 2 * next;
            records[index].C = 3 * next;
            ++records[index].version;
            writeOps.fetch_add(1);
        }
        else {
            const auto& r = records[index];
            if (r.B != 2 * r.A || r.C != 3 * r.A)
                invariantErrors.fetch_add(1);
            readOps.fetch_add(1);
        }
        accesses[index].fetch_add(1);
        completed.fetch_add(1);
        releaseLock(index, write, workerId, tid);
        if (cfg.workUs > 0) {
            auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(cfg.workUs);
            while (std::chrono::steady_clock::now() < until && !stopRequested.load())
                std::this_thread::yield();
        }
    }
    event(workerId, tid, "FINISHED", -1);
    consoleState(workerId, tid, "FINISHED");
}
bool parseInt(const char* s, int& out) {
    try { size_t n = 0; int v = std::stoi(s, &n); if (s[n] != '\0') return false; out = v; return true; }
    catch (...) { return false; }
}
void usage() {
    std::cout << "LR4.exe --lock global|per-record --dist uniform|biased --readers N --writers N "
        "--ops N --seed N --work-us N --timeout MS --csv PATH --events PATH "
        "--quiet --no-events\n";
}
bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) return nullptr; return argv[++i]; };
        if (a == "--help") { usage(); return false; }
        else if (a == "--lock") { auto v = val(); if (!v)return false; std::string s = v; if (s == "global")cfg.lockMode = LockMode::Global; else if (s == "per-record")cfg.lockMode = LockMode::PerRecord; else return false; }
        else if (a == "--dist") { auto v = val(); if (!v)return false; std::string s = v; if (s == "uniform")cfg.distribution = Distribution::Uniform; else if (s == "biased")cfg.distribution = Distribution::Biased; else return false; }
        else if (a == "--readers" || a == "--writers" || a == "--ops" || a == "--work-us" || a == "--timeout") {
            auto v = val(); int x; if (!v || !parseInt(v, x))return false;
            if (a == "--readers")cfg.readers = x; else if (a == "--writers")cfg.writers = x; else if (a == "--ops")cfg.operations = x; else if (a == "--work-us")cfg.workUs = x; else cfg.timeoutMs = x;
        }
        else if (a == "--seed") { auto v = val(); if (!v)return false; try { cfg.seed = std::stoull(v); } catch (...) { return false; } }
        else if (a == "--csv") { auto v = val(); if (!v)return false; cfg.csv = v; }
        else if (a == "--events") { auto v = val(); if (!v)return false; cfg.events = v; }
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "--no-events") cfg.logEvents = false;
        else { std::cerr << "Unknown argument: " << a << '\n'; return false; }
    }
    return cfg.readers > 0 && cfg.writers > 0 && cfg.operations > 0 &&
        cfg.workUs >= 0 && cfg.timeoutMs > 0;
}
void appendCsv() {
    std::lock_guard<std::mutex> g(csvMutex);
    bool header = !std::ifstream(cfg.csv).good();
    std::ofstream f(cfg.csv, std::ios::app);
    if (!f) { std::cerr << "Cannot open CSV: " << cfg.csv << '\n'; return; }
    if (header) f << "run_id,student_id,lock_mode,distribution,readers,writers,ops_per_worker,seed,completed,read_ops,write_ops,elapsed_ms,throughput_ops_sec,avg_read_wait_us,avg_write_wait_us,max_wait_us,invariant_errors,sum_versions,timed_out\n";
    long long versions = 0;
    for (const auto& r : records) versions += r.version;
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
    double avgR = readOps ? (double)readWaitNs.load() / readOps.load() / 1000.0 : 0;
    double avgW = writeOps ? (double)writeWaitNs.load() / writeOps.load() / 1000.0 : 0;
    f << runId << ",104," << modeName() << ',' << distName() << ',' << cfg.readers << ','
        << cfg.writers << ',' << cfg.operations << ',' << cfg.seed << ',' << completed << ','
        << readOps << ',' << writeOps << ',' << std::fixed << std::setprecision(3)
        << elapsed * 1000 << ',' << (elapsed > 0 ? completed.load() / elapsed : 0) << ','
        << avgR << ',' << avgW << ',' << maxWaitNs.load() / 1000.0 << ','
        << invariantErrors << ',' << versions << ',' << (stopRequested.load() ? 1 : 0) << '\n';
}
int main(int argc, char** argv) {
    if (!parseArgs(argc, argv)) { usage(); return 2; }
    for (int i = 0; i < RECORDS; ++i) { InitializeSRWLock(&recordLocks[i]); accesses[i] = 0; }
    runId = std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    runStart = std::chrono::steady_clock::now();
    deadline = runStart + std::chrono::milliseconds(cfg.timeoutMs);
    std::vector<std::thread> threads;
    try {
        for (int i = 0; i < cfg.readers; ++i) threads.emplace_back(worker, i, false, cfg.operations);
        for (int i = 0; i < cfg.writers; ++i) threads.emplace_back(worker, cfg.readers + i, true, cfg.operations);
    }
    catch (const std::exception& e) {
        stopRequested.store(true);
        std::cerr << "Thread creation failed: " << e.what() << '\n';
    }
    for (auto& t : threads) if (t.joinable()) t.join();
    auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
    appendCsv();
    if (!cfg.quiet) {
        std::cout << "\nPID=" << GetCurrentProcessId() << " | lock=" << modeName()
            << " | dist=" << distName() << " | elapsed=" << elapsed << " ms\n"
            << "Completed=" << completed << " | reads=" << readOps << " | writes=" << writeOps
            << " | invariant_errors=" << invariantErrors << '\n';
        for (int i = 0; i < RECORDS; ++i) std::cout << "record[" << i << "]=" << accesses[i].load()
            << (i == RECORDS - 1 ? '\n' : ' ');
    }
    bool allDone = completed.load() == (long long)(cfg.readers + cfg.writers) * cfg.operations;
    return (!allDone || invariantErrors.load() != 0) ? 1 : 0;
}
