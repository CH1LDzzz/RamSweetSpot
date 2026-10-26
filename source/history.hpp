// История частот за всё время: каждый замер прогона частот, итог длинной проверки и прогон TOTK —
// строкой в config/ram-sweetspot/history.csv. Не стирается новым подбором: со временем видно, какие частоты
// у этой консоли держатся лучше и сколько им добавляют тайминги.
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

struct HistRow {
    s64 at = 0;              // время, эпоха
    std::string ver;         // версия приложения
    int dram = -1;           // DRAM ID чипа
    double mhz = 0;          // частота РАМ (0 — «Как было»)
    std::string kind;        // scan:stock / scan:guide / scan:kip / research:stock / research:guide / tuned / totk
    std::string status;      // ok / slow / fail / hang
    double bw = 0, lat = 0;  // МБ/с, нс (замеры)
    double frames = 0, fps = 0, low1 = 0, low01 = 0;  // TOTK
    std::string set;         // набор ручек (кратко)
};

// Сводка по одной частоте.
struct HistFreq {
    double mhz = 0;
    int tests = 0, ok = 0, fail = 0, hang = 0;  // замеры прогона, длинные проверки и исследование
    double scanMedian = 0;   // медиана потока прогона частот (общие тайминги)
    double scanBest = 0;
    double tunedBest = 0;    // лучший поток после доводки таймингов (прошёл длинную проверку)
    double latBest = 0;      // лучшая задержка среди стабильных
    int totkRuns = 0;
    double totkFrames = 0;   // среднее кадров по прогонам TOTK
    std::string bestSet;     // набор лучшего доведённого
    double stability() const { return tests ? (double)ok / tests : 0; }
    double best() const { return tunedBest > scanBest ? tunedBest : scanBest; }
};

std::string historyPath();
void histAppend(const HistRow& r);
std::vector<HistRow> histLoad();
// Сводка по частотам, по возрастанию; «Как было» (mhz 0) — отдельно в *base (кадры TOTK), если передан.
std::vector<HistFreq> histSummary(const std::vector<HistRow>& rows, HistFreq* base = nullptr);
