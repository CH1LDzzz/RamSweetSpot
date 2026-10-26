#include "archive.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

std::string archivePath() { return paths::appDir() + "archive.txt"; }

static std::string cleanField(std::string s) {
    for (auto& c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::vector<ArchEntry> archLoad() {
    std::vector<ArchEntry> out;
    std::string text;
    if (!readFile(archivePath(), text)) return out;
    for (auto& raw : split(text, '\n')) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == ';') continue;
        auto p = split(l, '|');
        if (p.size() < 8) continue;
        ArchEntry e;
        e.at = (s64)toLong(p[0], 0);
        e.hz = (u32)strtoul(p[1].c_str(), nullptr, 10);
        e.dram = (int)toLong(p[2], -1);
        e.bw = toDouble(p[3], 0);
        e.lat = toDouble(p[4], 0);
        e.frames = toDouble(p[5], 0);
        e.name = p[6];
        if (!e.hz || !knobSetFromString(p[7], e.set)) continue;
        out.push_back(e);
    }
    return out;
}

bool archSave(const std::vector<ArchEntry>& all) {
    std::string o = "; RamSweetSpot: архив успешных конфигураций. время|частота, Гц|DRAM ID|поток|задержка|кадры TOTK|откуда|набор\n";
    for (auto& e : all)
        o += strf("%lld|%u|%d|%.0f|%.1f|%.0f|%s|%s\n", (long long)e.at, e.hz, e.dram, e.bw, e.lat, e.frames, cleanField(e.name).c_str(),
                  knobSetToString(e.set).c_str());
    makeDirs(paths::appDir());
    return writeFileSafe(archivePath(), o);
}

bool archAdd(const ArchEntry& e) {
    auto all = archLoad();
    ArchEntry n = e;
    if (!n.at) n.at = epochNow();
    for (auto& x : all)
        if (x.hz == n.hz && x.set == n.set) {
            if (!n.frames) n.frames = x.frames;  // повторное сохранение без TOTK не стирает кадры
            x = n;
            return archSave(all);
        }
    all.push_back(n);
    return archSave(all);
}

int sweetSpot(const std::vector<SweetPoint>& pts, double wGame, double* score) {
    double maxBw = 0, maxF = 0;
    std::vector<u32> hzs;
    for (auto& p : pts) {
        if (p.bw <= 0 || p.frames <= 0) continue;
        maxBw = std::max(maxBw, p.bw);
        maxF = std::max(maxF, p.frames);
        if (std::find(hzs.begin(), hzs.end(), p.hz) == hzs.end()) hzs.push_back(p.hz);
    }
    if (hzs.size() < 2) return -1;
    wGame = std::min(1.0, std::max(0.0, wGame));
    std::vector<double> sc(pts.size(), 0);
    double top = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        if (pts[i].bw <= 0 || pts[i].frames <= 0) continue;
        sc[i] = std::pow(pts[i].bw / maxBw, 1.0 - wGame) * std::pow(pts[i].frames / maxF, wGame);
        top = std::max(top, sc[i]);
    }
    int best = -1;
    for (size_t i = 0; i < pts.size(); i++) {
        if (sc[i] <= 0 || sc[i] < top * (1.0 - kSweetTol)) continue;
        if (best < 0 || pts[i].hz < pts[best].hz || (pts[i].hz == pts[best].hz && sc[i] > sc[best])) best = (int)i;
    }
    if (score && best >= 0) *score = sc[best];
    return best;
}

int archSweet(const std::vector<ArchEntry>& all, int dram, double wGame) {
    std::vector<SweetPoint> pts;
    for (auto& e : all) {
        SweetPoint p;
        bool ok = (dram < 0 || e.dram < 0 || e.dram == dram) && e.name.find("сомнительный") == std::string::npos;
        if (ok) p = {e.hz, e.bw, e.frames};
        pts.push_back(p);
    }
    return sweetSpot(pts, wGame);
}

bool archRemove(size_t idx) {
    auto all = archLoad();
    if (idx >= all.size()) return false;
    all.erase(all.begin() + idx);
    return archSave(all);
}

std::string archLabel(const ArchEntry& e) {
    std::string o = mhzStr(e.hz) + " МГц";
    if (e.bw > 0) o += strf(" · %.0f МБ/с", e.bw);
    if (e.frames > 0) o += strf(" · TOTK %.0f", e.frames);
    if (e.at) {
        time_t t = (time_t)e.at;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%d.%m %H:%M", &tmv);
        o += std::string(" · ") + buf;
    }
    if (!e.name.empty()) o += " · " + e.name;
    return o;
}
