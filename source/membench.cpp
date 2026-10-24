#include "membench.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifndef __SWITCH__
#include <chrono>
#include <thread>
#endif

// ------------------------------------------------------------------ время

static inline u64 ticksNow() {
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    using namespace std::chrono;
    return (u64)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

static inline double ticksToSec(u64 t) {
#ifdef __SWITCH__
    return (double)t / 19200000.0;
#else
    return (double)t / 1e9;
#endif
}

// ------------------------------------------------------------------ потоки

int benchThreads() { return 3; }

#ifdef __SWITCH__
struct ParArg {
    const std::function<void(int)>* fn;
    int idx;
};
static void parEntry(void* p) {
    ParArg* a = (ParArg*)p;
    (*a->fn)(a->idx);
}
#endif

void runParallel(int n, const std::function<void(int)>& fn) {
#ifdef __SWITCH__
    std::vector<Thread> th(n);
    std::vector<ParArg> args(n);
    std::vector<bool> created(n, false);
    for (int i = 0; i < n; i++) {
        args[i] = {&fn, i};
        // Приложению доступны ядра 0..2 (3-е — системное).
        Result rc = threadCreate(&th[i], parEntry, &args[i], nullptr, 0x10000, 0x2C, i % 3);
        if (R_FAILED(rc)) rc = threadCreate(&th[i], parEntry, &args[i], nullptr, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            created[i] = true;
            threadStart(&th[i]);
        }
    }
    for (int i = 0; i < n; i++) {
        if (created[i]) {
            threadWaitForExit(&th[i]);
            threadClose(&th[i]);
        } else {
            fn(i);  // не удалось создать поток — выполняем в текущем
        }
    }
#else
    std::vector<std::thread> th;
    for (int i = 0; i < n; i++) th.emplace_back([&fn, i] { fn(i); });
    for (auto& t : th) t.join();
#endif
}

// Простой «стартовый барьер» на атомике: все потоки начинают замер одновременно.
// Если какой-то поток не создался и работа идёт последовательно, ждём не дольше 100 мс.
struct SpinBarrier {
    std::atomic<int> count{0};
    int total;
    explicit SpinBarrier(int n) : total(n) {}
    void wait() {
        count.fetch_add(1);
        u64 start = ticksNow();
        u32 spins = 0;
        while (count.load() < total) {
            if ((++spins & 1023) == 0 && ticksToSec(ticksNow() - start) > 0.1) break;
        }
    }
};

size_t freeMemoryBytes() {
#ifdef __SWITCH__
    // libnx отдаёт newlib всю доступную процессу память как кучу;
    // проверяем, какой самый большой блок реально выдаёт malloc (шаг 16 МБ).
    size_t lo = 0, hi = (size_t)3800 * 1024 * 1024;
    const size_t step = 16 * 1024 * 1024;
    while (hi - lo > step) {
        size_t mid = lo + (hi - lo) / 2;
        void* p = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
#else
    return (size_t)768 * 1024 * 1024;
#endif
}

// ------------------------------------------------------------------ BenchResult

double BenchResult::bw() const {
    if (readMBs <= 0 || writeMBs <= 0 || copyMBs <= 0) return 0;
    return std::cbrt(readMBs * writeMBs * copyMBs);
}

std::string BenchResult::toString() const {
    if (!valid) return "-";
    return strf("%.1f/%.1f/%.1f/%.2f/%.2f", readMBs, writeMBs, copyMBs, latNs, lat2Ns);
}

bool BenchResult::fromString(const std::string& s) {
    *this = BenchResult();
    if (trim(s) == "-" || trim(s).empty()) return true;
    auto p = split(trim(s), '/');
    if (p.size() != 5) return false;
    bool ok[5];
    readMBs = toDouble(p[0], 0, &ok[0]);
    writeMBs = toDouble(p[1], 0, &ok[1]);
    copyMBs = toDouble(p[2], 0, &ok[2]);
    latNs = toDouble(p[3], 0, &ok[3]);
    lat2Ns = toDouble(p[4], 0, &ok[4]);
    valid = ok[0] && ok[1] && ok[2] && ok[3] && ok[4];
    return valid;
}

// ------------------------------------------------------------------ ядра

static u64 kernelRead(const u8* p, size_t bytes) {
#if defined(__aarch64__)
    const u64* q = (const u64*)p;
    size_t n = bytes / 8;
    uint64x2_t a0 = vdupq_n_u64(0), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (size_t i = 0; i + 16 <= n; i += 16) {
        a0 = veorq_u64(a0, vld1q_u64(q + i));
        a1 = veorq_u64(a1, vld1q_u64(q + i + 2));
        a2 = veorq_u64(a2, vld1q_u64(q + i + 4));
        a3 = veorq_u64(a3, vld1q_u64(q + i + 6));
        a4 = veorq_u64(a4, vld1q_u64(q + i + 8));
        a5 = veorq_u64(a5, vld1q_u64(q + i + 10));
        a6 = veorq_u64(a6, vld1q_u64(q + i + 12));
        a7 = veorq_u64(a7, vld1q_u64(q + i + 14));
    }
    a0 = veorq_u64(veorq_u64(veorq_u64(a0, a1), veorq_u64(a2, a3)), veorq_u64(veorq_u64(a4, a5), veorq_u64(a6, a7)));
    return vgetq_lane_u64(a0, 0) ^ vgetq_lane_u64(a0, 1);
#else
    const u64* q = (const u64*)p;
    size_t n = bytes / 8;
    u64 a[8] = {0};
    for (size_t i = 0; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) a[k] ^= q[i + k];
    return a[0] ^ a[1] ^ a[2] ^ a[3] ^ a[4] ^ a[5] ^ a[6] ^ a[7];
#endif
}

static void kernelWrite(u8* p, size_t bytes, u64 pattern) {
#if defined(__aarch64__)
    u64* q = (u64*)p;
    size_t n = bytes / 8;
    uint64x2_t v = vdupq_n_u64(pattern);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        vst1q_u64(q + i, v);
        vst1q_u64(q + i + 2, v);
        vst1q_u64(q + i + 4, v);
        vst1q_u64(q + i + 6, v);
        vst1q_u64(q + i + 8, v);
        vst1q_u64(q + i + 10, v);
        vst1q_u64(q + i + 12, v);
        vst1q_u64(q + i + 14, v);
    }
#else
    u64* q = (u64*)p;
    size_t n = bytes / 8;
    for (size_t i = 0; i < n; i++) q[i] = pattern;
#endif
}

static volatile u64 g_sink;

// ------------------------------------------------------------------ MemBench

bool MemBench::init(size_t bwBytesTotal, size_t latBytes) {
    shutdown();
    m_threads = benchThreads();
    // Каждому потоку — свой регион, кратный 4 КБ.
    size_t per = (bwBytesTotal / m_threads) & ~(size_t)0xFFFF;
    m_bwBytes = per * m_threads;
    m_latBytes = latBytes & ~(size_t)0xFFFF;
    m_bw = (u8*)aligned_alloc(4096, m_bwBytes);
    m_lat = (u8*)aligned_alloc(4096, m_latBytes);
    if (!m_bw || !m_lat) {
        shutdown();
        return false;
    }
    memset(m_bw, 0x5A, m_bwBytes);
    memset(m_lat, 0, m_latBytes);
    // Цепочка для замера задержки: случайный цикл по строкам кэша 64 байта (алгоритм Саттоло).
    const size_t line = 64;
    size_t lines = m_latBytes / line;
    std::vector<u32> perm(lines);
    for (size_t i = 0; i < lines; i++) perm[i] = (u32)i;
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (size_t i = lines - 1; i > 0; i--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        size_t j = x % i;  // j < i — получается один большой цикл
        std::swap(perm[i], perm[j]);
    }
    for (size_t i = 0; i < lines; i++) {
        u8** slot = (u8**)(m_lat + (size_t)i * line);
        *slot = m_lat + (size_t)perm[i] * line;
    }
    return true;
}

void MemBench::shutdown() {
    free(m_bw);
    free(m_lat);
    m_bw = m_lat = nullptr;
    m_bwBytes = m_latBytes = 0;
}

double MemBench::measureRead() {
    size_t per = m_bwBytes / m_threads;
    const int reps = 8;
    std::vector<u64> t0(m_threads), t1(m_threads);
    SpinBarrier bar(m_threads);
    runParallel(m_threads, [&](int i) {
        const u8* p = m_bw + per * i;
        u64 s = kernelRead(p, per);  // прогрев TLB
        bar.wait();
        t0[i] = ticksNow();
        for (int r = 0; r < reps; r++) s ^= kernelRead(p, per);
        t1[i] = ticksNow();
        g_sink = s;
    });
    u64 a = *std::min_element(t0.begin(), t0.end()), b = *std::max_element(t1.begin(), t1.end());
    double sec = ticksToSec(b - a);
    return sec > 0 ? (double)per * m_threads * reps / sec / 1e6 : 0;
}

double MemBench::measureWrite() {
    size_t per = m_bwBytes / m_threads;
    const int reps = 6;
    std::vector<u64> t0(m_threads), t1(m_threads);
    SpinBarrier bar(m_threads);
    runParallel(m_threads, [&](int i) {
        u8* p = m_bw + per * i;
        bar.wait();
        t0[i] = ticksNow();
        for (int r = 0; r < reps; r++) kernelWrite(p, per, 0x0123456789ABCDEFULL + r);
        t1[i] = ticksNow();
    });
    u64 a = *std::min_element(t0.begin(), t0.end()), b = *std::max_element(t1.begin(), t1.end());
    double sec = ticksToSec(b - a);
    return sec > 0 ? (double)per * m_threads * reps / sec / 1e6 : 0;
}

double MemBench::measureCopy() {
    size_t per = m_bwBytes / m_threads;
    size_t half = per / 2;
    const int reps = 6;
    std::vector<u64> t0(m_threads), t1(m_threads);
    SpinBarrier bar(m_threads);
    runParallel(m_threads, [&](int i) {
        u8* p = m_bw + per * i;
        bar.wait();
        t0[i] = ticksNow();
        for (int r = 0; r < reps; r++) memcpy(p + half, p, half);
        t1[i] = ticksNow();
    });
    u64 a = *std::min_element(t0.begin(), t0.end()), b = *std::max_element(t1.begin(), t1.end());
    double sec = ticksToSec(b - a);
    // Копирование: считаем прочитанное + записанное, как MicroMemBench.
    return sec > 0 ? (double)half * 2 * m_threads * reps / sec / 1e6 : 0;
}

double MemBench::measureLatency(bool dual) {
    const size_t hops = 1500000;
    u8* p = m_lat;
    // Прогрев.
    for (size_t i = 0; i < 20000; i++) p = *(u8**)p;
    if (!dual) {
        u64 t0 = ticksNow();
        for (size_t i = 0; i < hops; i += 4) {
            p = *(u8**)p;
            p = *(u8**)p;
            p = *(u8**)p;
            p = *(u8**)p;
        }
        u64 t1 = ticksNow();
        g_sink = (u64)(uintptr_t)p;
        return ticksToSec(t1 - t0) * 1e9 / (double)hops;
    }
    // Две независимые цепочки: вторая стартует с середины цикла.
    u8* q = p;
    for (size_t i = 0; i < (m_latBytes / 64) / 2; i++) q = *(u8**)q;
    u64 t0 = ticksNow();
    for (size_t i = 0; i < hops; i += 2) {
        p = *(u8**)p;
        q = *(u8**)q;
        p = *(u8**)p;
        q = *(u8**)q;
    }
    u64 t1 = ticksNow();
    g_sink = (u64)(uintptr_t)p ^ (u64)(uintptr_t)q;
    return ticksToSec(t1 - t0) * 1e9 / (double)(hops * 2);
}

BenchResult MemBench::run(int passes, const std::atomic<bool>* abort) {
    BenchResult best;
    if (!m_bw || !m_lat) return best;
    if (passes < 1) passes = 1;
    for (int p = 0; p < passes; p++) {
        if (abort && abort->load()) return BenchResult();
        double r = measureRead();
        double w = measureWrite();
        double c = measureCopy();
        double l = measureLatency(false);
        double l2 = measureLatency(true);
        if (!best.valid) {
            best.valid = true;
            best.readMBs = r;
            best.writeMBs = w;
            best.copyMBs = c;
            best.latNs = l;
            best.lat2Ns = l2;
        } else {
            best.readMBs = std::max(best.readMBs, r);
            best.writeMBs = std::max(best.writeMBs, w);
            best.copyMBs = std::max(best.copyMBs, c);
            best.latNs = std::min(best.latNs, l);
            best.lat2Ns = std::min(best.lat2Ns, l2);
        }
    }
    return best;
}

// ------------------------------------------------------------------ MemTest

static inline u64 splitmix(u64 x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

bool MemTest::init(size_t bytes) {
    shutdown();
    m_bytes = bytes & ~(size_t)0xFFFFF;  // кратно 1 МБ
    if (m_bytes < 16 * 1024 * 1024) return false;
    m_buf = (u8*)aligned_alloc(4096, m_bytes);
    if (!m_buf) {
        m_bytes = 0;
        return false;
    }
    memset(m_buf, 0, m_bytes);
    return true;
}

void MemTest::shutdown() {
    free(m_buf);
    m_buf = nullptr;
    m_bytes = 0;
}

namespace {
struct ThreadErr {
    u64 errors = 0;
    u64 bytes = 0;
    int passes = 0;
    bool haveFirst = false;
    size_t firstOff = 0;
    u64 expect = 0, got = 0;
    const char* pattern = "";
};

inline void checkWord(ThreadErr& e, const u64* base, size_t i, u64 expect, u64 got, const u8* buf0, const char* pat) {
    if (expect != got) {
        e.errors++;
        if (!e.haveFirst) {
            e.haveFirst = true;
            e.firstOff = (size_t)((const u8*)(base + i) - buf0);
            e.expect = expect;
            e.got = got;
            e.pattern = pat;
        }
    }
}
}  // namespace

MemTestResult MemTest::run(int seconds, const std::atomic<bool>* abort) {
    MemTestResult res;
    if (!m_buf) {
        res.ok = false;
        res.detail = "буфер теста не выделен";
        return res;
    }
    const int nt = benchThreads();
    size_t per = (m_bytes / nt) & ~(size_t)0xFFFFF;
    std::vector<ThreadErr> errs(nt);
    u64 deadline = nowMs() + (u64)seconds * 1000;
    std::atomic<bool> stop{false};

    runParallel(nt, [&](int t) {
        ThreadErr& e = errs[t];
        u64* w = (u64*)(m_buf + per * t);
        size_t n = per / 8;
        const size_t chunk = (1024 * 1024) / 8;
        u64 seedBase = 0xA5A5F00D12345678ULL ^ ((u64)t << 48);
        int pass = 0;
        auto timeUp = [&]() { return stop.load() || (abort && abort->load()) || nowMs() >= deadline; };
        while (!timeUp()) {
            u64 seed = splitmix(seedBase + pass);
            switch (pass % 5) {
                case 0: {  // случайные данные
                    for (size_t i = 0; i < n; i++) w[i] = splitmix(seed + i);
                    for (size_t i = 0; i < n; i++) checkWord(e, w, i, splitmix(seed + i), w[i], m_buf, "случайные");
                    e.bytes += per * 2;
                    break;
                }
                case 1: {  // бегущая инверсия
                    const u64 P = 0x5555555555555555ULL ^ (seed & 0xFF00FF00FF00FF00ULL);
                    for (size_t i = 0; i < n; i++) w[i] = P;
                    for (size_t i = 0; i < n; i++) {
                        checkWord(e, w, i, P, w[i], m_buf, "инверсия");
                        w[i] = ~P;
                    }
                    for (size_t i = n; i-- > 0;) {
                        checkWord(e, w, i, ~P, w[i], m_buf, "инверсия");
                        w[i] = P;
                    }
                    for (size_t i = 0; i < n; i++) checkWord(e, w, i, P, w[i], m_buf, "инверсия");
                    e.bytes += per * 6;
                    break;
                }
                case 2: {  // адрес в адресе
                    for (size_t i = 0; i < n; i++) w[i] = (u64)(uintptr_t)(w + i) ^ seed;
                    for (size_t i = 0; i < n; i++) checkWord(e, w, i, (u64)(uintptr_t)(w + i) ^ seed, w[i], m_buf, "адрес");
                    e.bytes += per * 2;
                    break;
                }
                case 3: {  // бегущий бит на шахматке
                    for (size_t i = 0; i < n; i++) w[i] = ((i & 1) ? ~0ULL : 0ULL) ^ (1ULL << ((i + pass) & 63));
                    for (size_t i = 0; i < n; i++)
                        checkWord(e, w, i, ((i & 1) ? ~0ULL : 0ULL) ^ (1ULL << ((i + pass) & 63)), w[i], m_buf, "бегущий бит");
                    e.bytes += per * 2;
                    break;
                }
                case 4: {  // копирование половин (нагрузка на шину в обе стороны)
                    size_t h = n / 2;
                    for (size_t i = 0; i < h; i++) w[i] = splitmix(seed ^ i);
                    for (size_t off = 0; off < h; off += chunk) {
                        size_t len = std::min(chunk, h - off);
                        memcpy(w + h + off, w + off, len * 8);
                    }
                    for (size_t i = 0; i < h; i++) checkWord(e, w + h, i, splitmix(seed ^ i), w[h + i], m_buf, "копирование");
                    e.bytes += per * 2;
                    break;
                }
            }
            pass++;
            e.passes = pass;
            if (e.errors > 0) stop.store(true);  // ошибка найдена — дальше гонять незачем
        }
    });

    if (abort && abort->load()) res.aborted = true;
    for (auto& e : errs) {
        res.errors += e.errors;
        res.bytes += e.bytes;
        res.passes += e.passes;
    }
    res.ok = res.errors == 0;
    if (!res.ok) {
        for (auto& e : errs) {
            if (!e.haveFirst) continue;
            res.detail = strf("%llu ошиб. (%s): +0x%zx ждали %016llx, прочитано %016llx", (unsigned long long)res.errors, e.pattern,
                              e.firstOff, (unsigned long long)e.expect, (unsigned long long)e.got);
            break;
        }
    }
    return res;
}
