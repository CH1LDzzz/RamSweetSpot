// Замер пропускной способности и задержки памяти + тест стабильности.
#pragma once

#include <atomic>
#include <functional>

#include "common.hpp"

struct BenchResult {
    bool valid = false;
    double readMBs = 0, writeMBs = 0, copyMBs = 0;  // МБ/с
    double latNs = 0;                               // задержка случайного чтения, нс
    double lat2Ns = 0;                              // две независимые цепочки, нс на обращение

    // Сводная пропускная способность — среднее геометрическое чтения, записи и копирования.
    double bw() const;
    std::string toString() const;
    bool fromString(const std::string& s);
};

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
