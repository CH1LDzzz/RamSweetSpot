// Клиент службы Ryazha-clk ("ryz:clk"; совместим с Horizon-OC "hoc:clk").
// Номера команд сверены с обработчиком в сисмодуле Ryazha-clk 3.6.0; у 3.5.5 (Ряженка 8.1.0) — те же:
//   0 GetApiVersion, 2 GetCurrentContext (буфер 0x500), 5 GetProfiles, 6 SetProfiles,
//   8 SetOverride {u32 module, u32 hz}, 9 GetConfigValues, 10 SetConfigValues, 11 GetFreqList {u32 module, u32 maxCount},
//   12 SetKipData (настройки кипа из конфига — в кип или в parked.bin сторожа).
// 9/10/12 — только на этапе переноса в кип и при «Вернуть как было».
// Команды 3 (Exit — выключает демон) и 7 (SetEnabled) приложение не вызывает никогда.
#pragma once

#include "common.hpp"

enum RclkModule : u32 { RclkModule_CPU = 0, RclkModule_GPU = 1, RclkModule_MEM = 2 };
enum RclkProfile : u32 {
    RclkProfile_Handheld = 0,
    RclkProfile_HandheldCharging = 1,
    RclkProfile_HandheldChargingUSB = 2,
    RclkProfile_HandheldChargingOfficial = 3,
    RclkProfile_Docked = 4,
};

// «Стабильная» часть контекста (первые 0xB4 байт), её раскладка не меняется между версиями.
struct RclkContext {
    bool valid = false;
    u32 freqs[5] = {};          // Гц, что записано у pcv
    u32 realFreqs[5] = {};      // Гц, измеренная частота
    u32 overrideFreqs[5] = {};  // Гц, временные значения
    s32 temps[11] = {};         // милли-°C: 0 SoC, 1 плата, 2 корпус, 3 батарея, 7 MEM/PLLX
    s32 power[2] = {};          // мВт: сейчас, среднее
    u32 voltages[7] = {};       // 0 SoC, 1 VDD2, 2 CPU, 3 GPU, 4 VDDQ (в мкВ)
    u64 applicationId = 0;
    u32 profile = 0;
};

namespace rclk {
// Подключиться к демону. svcName — имя найденной службы.
bool init(std::string* svcName);
void exit();
bool connected();
bool apiVersion(u32* out);
bool versionString(std::string* out);
bool context(RclkContext* out);
bool freqList(u32 module, std::vector<u32>* out);
inline bool memFreqList(std::vector<u32>* out) { return freqList(RclkModule_MEM, out); }
bool setOverride(u32 module, u32 hz);  // 0 — снять временное значение
inline bool setMemOverride(u32 hz) { return setOverride(RclkModule_MEM, hz); }
// Профили: МГц, [профиль][модуль] 5x5.
bool getProfiles(u64 tid, u32 mhz[25]);
bool setProfiles(u64 tid, const u32 mhz[25]);

// Настройки демона целиком (команды 9 и 10) и запись настроек кипа в кип (12). Раскладка списка — Ryazha-clk 3.6.0
// (API 2): 533 значения по 8 байт, ручки пропускной способности кипа tMEH[0..39] — с номера 416, ревизия таблицы — 67.
// У 3.5.5 — первые 525 из них в том же порядке (8 последних — батарея и сторож — появились в 3.6.0). Демон берёт буфер
// не меньше своего списка, поэтому один и тот же вызов годится обоим; хвост, который демон не заполнил, — нули.
constexpr u32 kConfigCount = 533;
constexpr u32 kConfigCount355 = 525;
constexpr u32 kCfgTmehFirst = 416;
constexpr u32 kCfgCustRev = 67;
constexpr u32 kCfgWatchdog = 532;  // сторож Ryazha-clk: 1 — включён
bool getConfig(std::vector<u64>* values);
u32 configCount();  // сколько значений демон заполнил в последнем getConfig (0 — ещё не читали)
bool setConfig(const std::vector<u64>& values);
bool setKipData();
}  // namespace rclk

// Напряжение из контекста в мВ (демон отдаёт мкВ; на всякий случай понимаем и мВ).
inline int voltMv(u32 v) { return v > 20000 ? (int)(v / 1000) : (int)v; }
