// RamSweetSpot — общие типы и утилиты.
#pragma once

#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>

#ifdef __SWITCH__
#include <switch.h>
#else
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

#define APP_NAME "RamSweetSpot"
#define APP_VER "1.3.0"

// Корень SD-карты. На консоли — "sdmc:/", в тестах на ПК — временная папка.
const std::string& sdRoot();
void setSdRoot(const std::string& root);

// Пути, с которыми работает приложение.
namespace paths {
std::string appDir();          // config/ram-sweetspot/
std::string state();           // журнал автоподбора
std::string settings();        // настройки
std::string timingsBackup();   // копия timings.ini до начала работы
std::string report();          // текстовый отчёт
std::string rclkDir();         // config/ryazha-clk/
std::string timings();         // config/ryazha-clk/timings.ini
std::string rclkLog();         // config/ryazha-clk/log.txt
std::string rclkLogFlag();     // config/ryazha-clk/log.flag
}  // namespace paths

std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string vstrf(const char* fmt, va_list ap);

// 2934400000 Гц -> "2934.4"
std::string mhzStr(u32 hz);
inline u32 hzToKhz(u32 hz) { return (hz + 500) / 1000; }

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
bool startsWith(const std::string& s, const char* prefix);
long toLong(const std::string& s, long def, bool* ok = nullptr);
double toDouble(const std::string& s, double def, bool* ok = nullptr);

bool fileExists(const std::string& path);
bool fileStat(const std::string& path, u64* size, s64* mtime);
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
// Запись через временный файл: старая версия не теряется, если запись оборвалась.
bool writeFileSafe(const std::string& path, const std::string& data);
bool readFileSafe(const std::string& path, std::string& out);
bool copyFile(const std::string& from, const std::string& to);
bool removeFile(const std::string& path);
void makeDirs(const std::string& dir);
std::vector<std::string> listDir(const std::string& dir);

u64 nowMs();
s64 epochNow();
void sleepMs(u32 ms);
