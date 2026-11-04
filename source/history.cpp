#include "history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

static const char* kHeader = "at,ver,dram,mhz,kind,status,bw,lat,frames,fps,low1,low01,set";

std::string historyPath() { return paths::appDir() + "history.csv"; }

static std::string cleanCsv(std::string s) {
    for (auto& c : s)
        if (c == ',' || c == '\n' || c == '\r') c = ' ';
    return s;
}

void histAppend(const HistRow& r) {
    makeDirs(paths::appDir());
    bool fresh = !fileExists(historyPath());
    FILE* f = fopen(historyPath().c_str(), "a");
    if (!f) return;
    if (fresh) fprintf(f, "%s\n", kHeader);
    fprintf(f, "%lld,%s,%d,%.1f,%s,%s,%.0f,%.1f,%.0f,%.1f,%.1f,%.1f,%s\n", (long long)(r.at ? r.at : epochNow()), cleanCsv(r.ver).c_str(), r.dram, r.mhz,
            cleanCsv(r.kind).c_str(), cleanCsv(r.status).c_str(), r.bw, r.lat, r.frames, r.fps, r.low1, r.low01, cleanCsv(r.set).c_str());
    fclose(f);
}

std::vector<HistRow> histLoad() {
    std::vector<HistRow> out;
    std::string text;
    if (!readFile(historyPath(), text)) return out;
    for (auto& l : split(text, '\n')) {
        auto p = split(trim(l), ',');
        if (p.size() < 13 || p[0] == "at") continue;
        HistRow r;
        r.at = (s64)toLong(p[0], 0);
        r.ver = p[1];
        r.dram = (int)toLong(p[2], -1);
        r.mhz = toDouble(p[3], 0);
        r.kind = p[4];
        r.status = p[5];
        r.bw = toDouble(p[6], 0);
        r.lat = toDouble(p[7], 0);
        r.frames = toDouble(p[8], 0);
        r.fps = toDouble(p[9], 0);
        r.low1 = toDouble(p[10], 0);
        r.low01 = toDouble(p[11], 0);
        r.set = p[12];
        out.push_back(r);
    }
    return out;
}

std::vector<HistFreq> histSummary(const std::vector<HistRow>& rows, HistFreq* base) {
    std::map<long, HistFreq> m;  // ключ — частота в десятых МГц
    std::map<long, std::vector<double>> scans;
    HistFreq b;
    for (auto& r : rows) {
        if (r.kind == "totk" && r.mhz <= 0) {
            b.totkFrames = (b.totkFrames * b.totkRuns + r.frames) / (b.totkRuns + 1);
            b.totkRuns++;
            continue;
        }
        if (r.mhz <= 0) continue;
        long key = std::lround(r.mhz * 10);
        HistFreq& f = m[key];
        f.mhz = key / 10.0;
        if (r.kind == "totk") {
            f.totkFrames = (f.totkFrames * f.totkRuns + r.frames) / (f.totkRuns + 1);
            f.totkRuns++;
            continue;
        }
        f.tests++;
        bool ok = r.status == "ok" || r.status == "slow";
        if (ok) f.ok++;
        else if (r.status == "hang") f.hang++;
        else f.fail++;
        if (!ok || r.bw <= 0) continue;
        if (!f.latBest || (r.lat > 0 && r.lat < f.latBest)) f.latBest = r.lat;
        if (r.kind == "tuned") {
            if (r.bw > f.tunedBest) {
                f.tunedBest = r.bw;
                f.bestSet = r.set;
            }
        } else {
            scans[key].push_back(r.bw);
            f.scanBest = std::max(f.scanBest, r.bw);
        }
    }
    std::vector<HistFreq> out;
    for (auto& kv : m) {
        HistFreq f = kv.second;
        auto it = scans.find(kv.first);
        if (it != scans.end() && !it->second.empty()) {
            auto v = it->second;
            std::sort(v.begin(), v.end());
            f.scanMedian = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
        }
        out.push_back(f);
    }
    if (base) *base = b;
    return out;
}
