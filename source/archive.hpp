// Архив успешных конфигураций: частота РАМ и набор ручек, которые пользователь сохранил, чтобы применять в любой
// момент — не завися от журнала подбора (его стирает новый подбор). config/ram-sweetspot/archive.txt.
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "knobs.hpp"

struct ArchEntry {
    s64 at = 0;            // когда сохранено
    u32 hz = 0;            // частота РАМ
    KnobSet set;           // набор ручек timings.ini
    std::string name;      // откуда: «К1 после TOTK», «ручная настройка»
    double bw = 0, lat = 0;  // замер на момент сохранения (0 — нет)
    double frames = 0;     // среднее кадров TOTK (0 — не проверялся)
    int dram = -1;         // DRAM ID чипа: на другой памяти набор может не держаться
};

std::string archivePath();
std::vector<ArchEntry> archLoad();
bool archSave(const std::vector<ArchEntry>& all);
// Добавить; та же частота с тем же набором уже есть — обновить её (замеры, имя, время), а не дублировать.
bool archAdd(const ArchEntry& e);
bool archRemove(size_t idx);
// Строка для списка: «2934.4 МГц · 18601 МБ/с · TOTK 2639 · 25.10 18:46 · К1 после TOTK».
std::string archLabel(const ArchEntry& e);

// Свитспот — самая выгодная частота РАМ: поток и кадры TOTK вместе, как в итоговой оценке
// (поток^(1 − вес TOTK) × кадры^(вес TOTK), к лучшим из сравниваемых). Есть только когда сравнивать есть что — точки
// с потоком и кадрами минимум на двух частотах. Частота ниже, что уступает лучшей не больше kSweetTol, выгоднее: тот
// же результат с меньшим разгоном. Возвращает номер точки или -1.
#define SWEET_MARK "★ свитспот"
constexpr double kSweetTol = 0.003;
struct SweetPoint {
    u32 hz = 0;
    double bw = 0, frames = 0;
};
int sweetSpot(const std::vector<SweetPoint>& pts, double wGame, double* score = nullptr);
// Свитспот среди записей архива этого чипа (dram < 0 — любой) без пометки «сомнительный»; -1 — нет.
int archSweet(const std::vector<ArchEntry>& all, int dram, double wGame);
