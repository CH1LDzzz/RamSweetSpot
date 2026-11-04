// Чипы памяти Switch и стартовые пресеты таймингов.
#pragma once

#include "common.hpp"
#include "knobs.hpp"

// Полоса частот проверенного набора: тайминги и задержка записи до своей частоты.
struct GuideBand {
    u32 upToMhz;           // включительно; 0 — всё выше прошлых полос
    int t[8];              // как common
    int wlBin;             // задержка записи по сетке JEDEC, МГц: 1333 / 1600 / 1866 / 2133; 0 — сток Ryazha-clk
};

struct TierPreset {
    const char* tier;      // тир из гайда Horizon-OC / rentry.co/mariko
    const char* chip;      // маркировка
    u32 minMhz, maxMhz;    // типичный диапазон стабильных частот
    int vdd2, vddq;        // рекомендованные напряжения, мВ
    // t1..t8 = tRCD tRP tRAS tRRD tRFC tRTW tWTR tREFI (6 = не менять)
    int common[8];
    int st[8];
    // Тест: ручная стабильная настройка такой же памяти по полосам частот (вместо common, если включено).
    const GuideBand* bands = nullptr;
    int nBands = 0;
    const char* bandsSrc = nullptr;
};

struct ChipInfo {
    int dramId = -1;
    bool known = false;
    bool mariko = false;
    std::string name;            // полная маркировка
    std::string vendor;
    int sizeGb = 0;
    const TierPreset* preset = nullptr;
};

ChipInfo chipFromDramId(int dramId);
// Тип консоли по spl HardwareType: 0 Icosa(V1) 1 Copper 2 Hoag(Lite) 3 Iowa(V2) 4 Calcio 5 Aula(OLED)
const char* consoleName(int hwType);
bool isMarikoHw(int hwType);

// Пресет -> набор ручек timings.ini (только первичные тайминги).
KnobSet presetToKnobs(const int t[8]);

// Полоса проверенного набора для частоты (nullptr — у чипа его нет).
const GuideBand* guideBandFor(const TierPreset* p, u32 hz);
// Ступень ручки WL: сколько шагов сетки MR2 от автоматики Ryazha-clk на этой частоте до wlBin (0 — не трогать).
int wlStepFor(u32 hz, int wlBin);
// Тайминги гайда для частоты: полоса проверенного набора (bands и он есть) или common.
KnobSet presetKnobsAt(const TierPreset* p, u32 hz, bool bands);
// Проверенный набор одной строкой: "до 2300: tRCD4 … WL1333; …".
std::string bandsText(const TierPreset* p);
