#include "log.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace logx {

namespace {
std::mutex g_mtx;
int g_errors = 0;
int g_writes = 0;
const u64 kMaxSize = 1024 * 1024;

std::string dir() { return paths::appDir() + "logs/"; }

void rotateIfBig() {
    u64 size = 0;
    if (fileStat(file(), &size, nullptr) && size > kMaxSize) {
        remove(oldFile().c_str());
        rename(file().c_str(), oldFile().c_str());
    }
}

std::string stamp() {
    time_t t = time(nullptr);
    struct tm tmv = {};
#ifdef __SWITCH__
    struct tm* p = localtime(&t);
    if (p) tmv = *p;
#else
    localtime_r(&t, &tmv);
#endif
    return strf("%04d-%02d-%02d %02d:%02d:%02d.%03u", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                (unsigned)(nowMs() % 1000));
}

void append(const std::string& line) {
    // Каждая строка сразу на карту: если консоль зависнет, последние записи останутся.
    FILE* f = fopen(file().c_str(), "ab");
    if (!f) {
        makeDirs(dir());
        f = fopen(file().c_str(), "ab");
        if (!f) return;
    }
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}
}  // namespace

std::string file() { return dir() + "ramss.log"; }
std::string oldFile() { return dir() + "ramss.old.log"; }

void begin() {
    std::lock_guard<std::mutex> lk(g_mtx);
    makeDirs(dir());
    rotateIfBig();
    g_errors = 0;
    append(strf("\n%s [I] ===== %s %s: новый сеанс =====\n", stamp().c_str(), APP_NAME, APP_VER));
}

void write(char level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = vstrf(fmt, ap);
    va_end(ap);
    for (auto& c : msg)
        if (c == '\n' || c == '\r') c = ' ';
    std::lock_guard<std::mutex> lk(g_mtx);
    if (level == 'E') g_errors++;
    if (++g_writes % 200 == 0) rotateIfBig();
    append(strf("%s [%c] %s\n", stamp().c_str(), level, msg.c_str()));
}

std::vector<std::string> tail(int lines) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<std::string> out;
    std::string data;
    if (!readFile(file(), data)) return out;
    if (data.size() > 32768) data = data.substr(data.size() - 32768);
    auto all = split(data, '\n');
    for (int i = (int)all.size() - 1; i >= 0 && (int)out.size() < lines; i--) {
        std::string l = trim(all[i]);
        if (!l.empty()) out.insert(out.begin(), l);
    }
    return out;
}

int errorsThisSession() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_errors;
}

}  // namespace logx
