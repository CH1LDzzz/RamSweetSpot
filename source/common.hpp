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
#define APP_VER "2.0.0"
// Сборка под публичную Ряженку (Ryazha-clk 3.5.5, без сторожа) — make PUBLIC=1; иначе — под Ryazha-clk 3.6.0.
#ifdef RAMSS_PUBLIC
#define RCLK_TARGET "Ryazha-clk 3.5.5"
#define APP_BUILD " · Ряженка 8.1.0"
#else
#define RCLK_TARGET "Ryazha-clk 3.6.0"
#define APP_BUILD ""
#endif

// Корень SD-карты. На консоли — "sdmc:/", в тестах на ПК — временная папка.
const std::string& sdRoot();
void setSdRoot(const std::string& root);

// Пути, с которыми работает приложение.
namespace paths {
std::string appDir();          // config/ram-sweetspot/
std::string state();           // журнал автоподбора
std::string settings();        // настройки
std::string timingsBackup();   // копия timings.ini до начала работы
std::string kipBackup();       // ручки пропускной способности кипа до переноса (слот:значение)
std::string unstable();        // реестр нестабильных значений (не стирается новым подбором)
std::string kipCommonBackup(); // общие тайминги кипа до «кип сначала»
std::string researchDir();     // исследование потока: research.cfg / .csv / .pending / .txt
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
// INI в стиле Atmosphere (system_settings.ini: «[секция]», «ключ = значение», комментарии «;» и «#»).
// iniGet — значение ключа в секции (нет — пусто). iniSet — поставить ключ: строка ключа заменяется, нет — дописывается
// в конец секции, нет секции — в конец файла; всё остальное, включая переводы строк, остаётся как было.
std::string iniGet(const std::string& text, const std::string& section, const std::string& key);
std::string iniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value);
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
std::string timeStamp();  // «2026-10-24 21:40:05» по местному времени
s64 epochNow();
void sleepMs(u32 ms);
