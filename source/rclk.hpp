// Клиент службы Ryazha-clk ("ryz:clk"; совместим с Horizon-OC "hoc:clk").
// Номера команд сверены с обработчиком в сисмодуле Ryazha-clk 3.6.0:
//   0 GetApiVersion, 2 GetCurrentContext (буфер 0x500), 5 GetProfiles, 6 SetProfiles,
//   8 SetOverride {u32 module, u32 hz}, 11 GetFreqList {u32 module, u32 maxCount}.
// Команды 3 (Exit — выключает демон), 7 (SetEnabled), 10 (SetConfigValues), 12 (SetKipData)
// приложение не вызывает никогда.
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
}  // namespace rclk

// Напряжение из контекста в мВ (демон отдаёт мкВ; на всякий случай понимаем и мВ).
inline int voltMv(u32 v) { return v > 20000 ? (int)(v / 1000) : (int)v; }
