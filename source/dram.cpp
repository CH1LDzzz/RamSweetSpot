#include "dram.hpp"

// Данные — из https://horizon-oc.github.io/mariko.html и https://rentry.co/mariko
// (раздел "OC settings for Horizon-OC"). Индексы ступеней t1..t8 Horizon-OC
// совпадают с номерами ступеней timings.ini Ryazha-clk (одни и те же таблицы нс).
static const TierPreset kWTB = {"GOD", "Micron WT:B", 3066, 3200, 1175, 600, {4, 4, 5, 4, 2, 6, 5, 6}, {6, 6, 7, 5, 2, 6, 5, 6}};
static const TierPreset kNEE = {"GOD", "Hynix NEI/NEE/x267", 3100, 3300, 1175, 640, {3, 3, 2, 1, 5, 5, 4, 6}, {4, 4, 4, 2, 7, 6, 5, 6}};
static const TierPreset kAAMGCL = {"S", "Samsung AA-MGCL", 2766, 3100, 1175, 640, {4, 4, 5, 4, 5, 5, 6, 6}, {4, 4, 8, 5, 5, 6, 7, 6}};
// В гайде у WT:E в ST-наборе tWTR записан как "S" — берём ступень Common (4).
static const TierPreset kWTE = {"A", "Micron WT:E", 2500, 3033, 1175, 600, {2, 2, 2, 1, 4, 4, 4, 6}, {3, 5, 3, 2, 5, 4, 4, 6}};
static const TierPreset kAMMGCJ = {"B", "Samsung AM-MGCJ", 2633, 2933, 1175, 640, {3, 2, 4, 1, 4, 4, 4, 6}, {4, 3, 8, 1, 5, 4, 4, 6}};
static const TierPreset kWTF = {"C", "Micron WT:F", 2633, 2800, 1175, 600, {4, 4, 2, 4, 4, 6, 3, 6}, {5, 5, 4, 4, 5, 6, 5, 6}};
static const TierPreset kABMGCL = {"D", "Samsung AB-MGCL", 2500, 2766, 1175, 640, {4, 4, 4, 3, 4, 5, 6, 6}, {4, 4, 8, 4, 5, 6, 8, 6}};
static const TierPreset kNME = {"E", "Hynix NME", 2300, 2766, 1175, 640, {2, 2, 3, 0, 1, 2, 2, 6}, {5, 3, 4, 1, 8, 3, 3, 6}};

struct IdEntry {
    int id;
    const char* name;
    const char* vendor;
    int gb;
    bool mariko;
    const TierPreset* preset;
};

// DRAM ID — из hekate (bdk/mem/sdram.h).
static const IdEntry kIds[] = {
    {0, "Samsung K4F6E304HB-MGCH", "Samsung", 4, false, nullptr},
    {1, "Hynix H9HCNNNBPUMLHR-NLE", "Hynix", 4, false, nullptr},
    {2, "Micron MT53B512M32D2NP-062 WT:C", "Micron", 4, false, nullptr},
    {4, "Samsung K4FHE3D4HM-MGCH", "Samsung", 6, false, nullptr},
    {7, "Samsung K4FBE3D4HM-MGXX", "Samsung", 8, false, nullptr},
    {3, "Hynix H9HCNNNBKMMLXR-NEE", "Hynix", 4, true, &kNEE},
    {5, "Hynix H9HCNNNBKMMLXR-NEE", "Hynix", 4, true, &kNEE},
    {6, "Hynix H9HCNNNBKMMLXR-NEE", "Hynix", 4, true, &kNEE},
    {8, "Samsung K4U6E3S4AM-MGCJ", "Samsung", 4, true, &kAMMGCJ},
    {9, "Samsung K4UBE3D4AM-MGCJ", "Samsung", 8, true, &kAMMGCJ},
    {10, "Hynix H9HCNNNBKMMLHR-NME", "Hynix", 4, true, &kNME},
    {11, "Micron MT53E512M32D2NP-046 WT:E", "Micron", 4, true, &kWTE},
    {12, "Samsung K4U6E3S4AM-MGCJ", "Samsung", 4, true, &kAMMGCJ},
    {13, "Samsung K4UBE3D4AM-MGCJ", "Samsung", 8, true, &kAMMGCJ},
    {14, "Hynix H9HCNNNBKMMLHR-NME", "Hynix", 4, true, &kNME},
    {15, "Micron MT53E512M32D2NP-046 WT:E", "Micron", 4, true, &kWTE},
    {17, "Samsung K4U6E3S4AA-MGCL", "Samsung", 4, true, &kAAMGCL},
    {18, "Samsung K4UBE3D4AA-MGCL", "Samsung", 8, true, &kAAMGCL},
    {19, "Samsung K4U6E3S4AA-MGCL", "Samsung", 4, true, &kAAMGCL},
    {20, "Samsung K4U6E3S4AB-MGCL", "Samsung", 4, true, &kABMGCL},
    {21, "Samsung K4U6E3S4AB-MGCL", "Samsung", 4, true, &kABMGCL},
    {22, "Samsung K4U6E3S4AB-MGCL", "Samsung", 4, true, &kABMGCL},
    {23, "Samsung K4UBE3D4AA-MGCL", "Samsung", 8, true, &kAAMGCL},
    {24, "Samsung K4U6E3S4AA-MGCL", "Samsung", 4, true, &kAAMGCL},
    {25, "Micron MT53E512M32D2NP-046 WT:F", "Micron", 4, true, &kWTF},
    {26, "Micron MT53E512M32D2NP-046 WT:F", "Micron", 4, true, &kWTF},
    {27, "Micron MT53E512M32D2NP-046 WT:F", "Micron", 4, true, &kWTF},
    {28, "Samsung K4UBE3D4AA-MGCL", "Samsung", 8, true, &kAAMGCL},
    {29, "Hynix H54G46CYRBX267", "Hynix", 4, true, &kNEE},
    {30, "Hynix H54G46CYRBX267", "Hynix", 4, true, &kNEE},
    {31, "Hynix H54G46CYRBX267", "Hynix", 4, true, &kNEE},
    {32, "Micron MT53E512M32D1NP-046 WT:B", "Micron", 4, true, &kWTB},
    {33, "Micron MT53E512M32D1NP-046 WT:B", "Micron", 4, true, &kWTB},
    {34, "Micron MT53E512M32D1NP-046 WT:B", "Micron", 4, true, &kWTB},
};

ChipInfo chipFromDramId(int dramId) {
    ChipInfo c;
    c.dramId = dramId;
    for (auto& e : kIds) {
        if (e.id != dramId) continue;
        c.known = true;
        c.mariko = e.mariko;
        c.name = e.name;
        c.vendor = e.vendor;
        c.sizeGb = e.gb;
        c.preset = e.preset;
        return c;
    }
    c.name = strf("неизвестный чип (DRAM ID %d)", dramId);
    return c;
}

const char* consoleName(int hwType) {
    switch (hwType) {
        case 0: return "Erista (V1)";
        case 1: return "Copper";
        case 2: return "Mariko Lite";
        case 3: return "Mariko V2";
        case 4: return "Calcio";
        case 5: return "Mariko OLED";
        default: return "неизвестная консоль";
    }
}

bool isMarikoHw(int hwType) { return hwType == 2 || hwType == 3 || hwType == 4 || hwType == 5; }

KnobSet presetToKnobs(const int t[8]) {
    static const char* keys[8] = {"tRCD", "tRP", "tRAS", "tRRD", "tRFC", "tRTW", "tWTR", "tREFI"};
    KnobSet s;
    for (int i = 0; i < 8; i++) {
        if (i == 7 && t[i] >= 6) continue;  // tREFI 6 = не менять
        const KnobDef* d = findKnob(keys[i]);
        if (!d) continue;
        int v = t[i];
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        s[keys[i]] = v;
    }
    return s;
}
