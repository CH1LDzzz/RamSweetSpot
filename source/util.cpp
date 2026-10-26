#include "common.hpp"

#include "log.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef __SWITCH__
#include <chrono>
#include <thread>
#endif

static std::string g_sdRoot =
#ifdef __SWITCH__
    "sdmc:/";
#else
    "./sdroot/";
#endif

const std::string& sdRoot() { return g_sdRoot; }
void setSdRoot(const std::string& root) {
    g_sdRoot = root;
    if (!g_sdRoot.empty() && g_sdRoot.back() != '/') g_sdRoot += '/';
}

namespace paths {
std::string appDir() { return sdRoot() + "config/ram-sweetspot/"; }
std::string state() { return appDir() + "state.txt"; }
std::string settings() { return appDir() + "settings.ini"; }
std::string timingsBackup() { return appDir() + "timings.orig.ini"; }
std::string kipBackup() { return appDir() + "kip.orig.txt"; }
std::string unstable() { return appDir() + "unstable.txt"; }
std::string kipCommonBackup() { return appDir() + "kip.common.orig.txt"; }
std::string researchDir() { return appDir(); }
std::string report() { return appDir() + "report.txt"; }
std::string rclkDir() { return sdRoot() + "config/ryazha-clk/"; }
std::string timings() { return rclkDir() + "timings.ini"; }
std::string rclkLog() { return rclkDir() + "log.txt"; }
std::string rclkLogFlag() { return rclkDir() + "log.flag"; }
}  // namespace paths

std::string timeStamp() {
    time_t t = time(nullptr);
    struct tm* p = localtime(&t);
    if (!p) return "?";
    return strf("%04d-%02d-%02d %02d:%02d:%02d", p->tm_year + 1900, p->tm_mon + 1, p->tm_mday, p->tm_hour, p->tm_min, p->tm_sec);
}

std::string vstrf(const char* fmt, va_list ap) {
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return std::string();
    }
    if ((size_t)n < sizeof(buf)) {
        va_end(ap2);
        return std::string(buf, n);
    }
    std::string s;
    s.resize(n + 1);
    vsnprintf(&s[0], n + 1, fmt, ap2);
    va_end(ap2);
    s.resize(n);
    return s;
}

std::string strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrf(fmt, ap);
    va_end(ap);
    return s;
}

std::string mhzStr(u32 hz) {
    u32 khz = hzToKhz(hz);
    u32 tenth = (khz % 1000 + 50) / 100;
    u32 whole = khz / 1000;
    if (tenth >= 10) {
        whole++;
        tenth = 0;
    }
    return strf("%u.%u", whole, tenth);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

bool startsWith(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

// Строки INI вместе с их концами строк: склейка даёт исходный текст байт в байт.
static std::vector<std::string> iniLines(const std::string& text) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) {
            v.push_back(text.substr(a));
            break;
        }
        v.push_back(text.substr(a, b - a + 1));
        a = b + 1;
    }
    return v;
}

static bool iniSection(const std::string& line, std::string* name) {
    std::string t = trim(line);
    if (t.size() < 2 || t[0] != '[' || t.back() != ']') return false;
    *name = trim(t.substr(1, t.size() - 2));
    return true;
}

static bool iniKey(const std::string& line, std::string* key, std::string* val) {
    std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    *key = trim(t.substr(0, eq));
    if (val) *val = trim(t.substr(eq + 1));
    return true;
}

std::string iniGet(const std::string& text, const std::string& section, const std::string& key) {
    std::string cur, k, v;
    for (auto& l : iniLines(text)) {
        std::string name;
        if (iniSection(l, &name)) {
            cur = name;
            continue;
        }
        if (cur == section && iniKey(l, &k, &v) && k == key) return v;
    }
    return std::string();
}

std::string iniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    auto lines = iniLines(text);
    std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    auto join = [&] {
        std::string o;
        for (auto& l : lines) o += l;
        return o;
    };
    int secEnd = -1;  // последняя непустая строка нужной секции (или её заголовок)
    bool inSec = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string name, k;
        if (iniSection(lines[i], &name)) {
            inSec = name == section;
            if (inSec) secEnd = (int)i;
            continue;
        }
        if (!inSec) continue;
        if (iniKey(lines[i], &k, nullptr) && k == key) {
            const std::string& l = lines[i];
            std::string end = l.size() >= 2 && l.compare(l.size() - 2, 2, "\r\n") == 0 ? "\r\n" : (!l.empty() && l.back() == '\n' ? "\n" : "");
            lines[i] = key + " = " + value + end;
            return join();
        }
        if (!trim(lines[i]).empty()) secEnd = (int)i;
    }
    std::string add = key + " = " + value + eol;
    if (secEnd >= 0) {
        if (lines[secEnd].empty() || lines[secEnd].back() != '\n') lines[secEnd] += eol;
        lines.insert(lines.begin() + secEnd + 1, add);
        return join();
    }
    std::string out = join();
    if (!out.empty() && out.back() != '\n') out += eol;
    if (!out.empty()) out += eol;
    return out + "[" + section + "]" + eol + add;
}

long toLong(const std::string& s, long def, bool* ok) {
    std::string t = trim(s);
    if (t.empty()) {
        if (ok) *ok = false;
        return def;
    }
    char* end = nullptr;
    errno = 0;
    long v = strtol(t.c_str(), &end, 10);
    bool good = errno == 0 && end && *end == 0;
    if (ok) *ok = good;
    return good ? v : def;
}

double toDouble(const std::string& s, double def, bool* ok) {
    std::string t = trim(s);
    if (t.empty()) {
        if (ok) *ok = false;
        return def;
    }
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    bool good = end && *end == 0 && std::isfinite(v);
    if (ok) *ok = good;
    return good ? v : def;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool fileStat(const std::string& path, u64* size, s64* mtime) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (size) *size = (u64)st.st_size;
    if (mtime) *mtime = (s64)st.st_mtime;
    return true;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGE("Файл %s: не открылся на запись (%s)", path.c_str(), strerror(errno));
        return false;
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) LOGE("Файл %s: запись не удалась (%s)", path.c_str(), strerror(errno));
    return ok;
}

bool writeFileSafe(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    if (!writeFile(tmp, data)) return false;
    // На FAT переименование поверх существующего файла не работает — сначала удаляем.
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        // Не вышло — пишем напрямую, временный файл оставляем как запасной.
        LOGW("Файл %s: переименование не удалось (%s), пишу напрямую", path.c_str(), strerror(errno));
        return writeFile(path, data);
    }
    return true;
}

bool readFileSafe(const std::string& path, std::string& out) {
    if (readFile(path, out) && !out.empty()) return true;
    // Основной файл пропал или пуст (оборвалась замена) — берём временный.
    return readFile(path + ".tmp", out);
}

bool copyFile(const std::string& from, const std::string& to) {
    std::string data;
    if (!readFile(from, data)) return false;
    return writeFile(to, data);
}

bool removeFile(const std::string& path) { return remove(path.c_str()) == 0; }

void makeDirs(const std::string& dir) {
    std::string cur;
    size_t start = 0;
    // Не пытаемся создавать "sdmc:" и корень.
    size_t colon = dir.find(":/");
    if (colon != std::string::npos) {
        cur = dir.substr(0, colon + 2);
        start = colon + 2;
    } else if (!dir.empty() && dir[0] == '/') {
        cur = "/";
        start = 1;
    }
    for (size_t i = start; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(start, i - start);
            if (!part.empty() && part != ".") {
                cur += part;
                mkdir(cur.c_str(), 0777);
                cur += "/";
            } else if (part == ".") {
                cur += "./";
            }
            start = i + 1;
        }
    }
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}

u64 nowMs() {
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
#else
    using namespace std::chrono;
    return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

s64 epochNow() { return (s64)time(nullptr); }

void sleepMs(u32 ms) {
#ifdef __SWITCH__
    svcSleepThread((s64)ms * 1000000LL);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}
