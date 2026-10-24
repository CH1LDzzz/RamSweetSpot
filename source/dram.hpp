// Чипы памяти Switch и стартовые пресеты таймингов.
#pragma once

#include "common.hpp"
#include "knobs.hpp"

struct TierPreset {
    const char* tier;      // тир из гайда Horizon-OC / rentry.co/mariko
    const char* chip;      // маркировка
    u32 minMhz, maxMhz;    // типичный диапазон стабильных частот
    int vdd2, vddq;        // рекомендованные напряжения, мВ
    // t1..t8 = tRCD tRP tRAS tRRD tRFC tRTW tWTR tREFI (6 = не менять)
    int common[8];
    int st[8];
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
