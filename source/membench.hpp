// Замер пропускной способности и задержки памяти + тест стабильности.
#pragma once

#include <atomic>
#include <functional>

#include "common.hpp"

// Один проход замера.
struct BenchPass {
    double r = 0, w = 0, c = 0;  // чтение, запись, копия, МБ/с
    double l = 0, l2 = 0;        // задержка одной и двух цепочек, нс
};

struct BenchResult {
    bool valid = false;
    double readMBs = 0, writeMBs = 0, copyMBs = 0;  // МБ/с
    double latNs = 0;                               // задержка случайного чтения, нс
    double lat2Ns = 0;                              // две независимые цепочки, нс на обращение
    std::vector<BenchPass> passes;                  // проходы, из которых сведён итог (в журнал подбора не пишутся)

    // Сводная пропускная способность — среднее геометрическое чтения, записи и копирования.
    double bw() const;
    std::string toString() const;
    bool fromString(const std::string& s);
};

// Версия методики замера: меняется, когда меняются абсолютные числа (размеры, сдвиги, повторы). Оценки разных версий
// между собой несравнимы. 2 — 1.9.4: буферы без наложения, копия 12 повторов, запись 8, задержка 0,5 млн переходов.
constexpr int kBenchVersion = 2;

// Свести проходы в итог: с 3 и больше — усечённое среднее (по max(1, n/5) с каждого края: при 3–4 это медиана),
// меньше — лучший. Выброс в любую сторону итог не сдвигает, а шум у среднего меньше, чем у медианы.
BenchResult aggregatePasses(const std::vector<BenchPass>& ps);

// Запустить fn(i) на n рабочих потоках (на консоли — по одному на ядро 0..2) и дождаться.
void runParallel(int n, const std::function<void(int)>& fn);
int benchThreads();

class MemBench {
public:
    bool init(size_t bwBytesTotal, size_t latBytes);
    void shutdown();
    // passes — сколько раз повторить; берётся лучший результат (чужая нагрузка портит среднее, не лучшее).
    BenchResult run(int passes, const std::atomic<bool>* abort);
    size_t bwBytes() const { return m_bwBytes; }
    size_t latBytes() const { return m_latBytes; }

private:
    double measureRead();
    double measureWrite();
    double measureCopy();
    double measureLatency(bool dual);
    u8* region(int thread, size_t* len) const;  // регион потока со сдвигом против наложения адресов
    void notePreempt(const std::vector<double>& coreShare);
    bool m_preempted = false;  // в этом проходе поток сняли с ядра — проход перемеряется

    u8* m_bw = nullptr;
    size_t m_bwBytes = 0;
    u8* m_lat = nullptr;
    size_t m_latBytes = 0;
    int m_threads = 3;
};

struct MemTestResult {
    bool ok = true;
    bool aborted = false;
    u64 errors = 0;
    u64 bytes = 0;     // сколько байт проверено суммарно
    int passes = 0;
    std::string detail;
};

class MemTest {
public:
    bool init(size_t bytes);
    void shutdown();
    MemTestResult run(int seconds, const std::atomic<bool>* abort);
    size_t size() const { return m_bytes; }

private:
    u8* m_buf = nullptr;
    size_t m_bytes = 0;
};

// Сколько памяти можно взять под буферы (байт). На ПК — фиксированное значение.
size_t freeMemoryBytes();
