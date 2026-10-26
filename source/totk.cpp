#include "totk.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

bool TotkRun::korok() const {
    std::string l = location;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l.find("korok") != std::string::npos;
}

std::string TotkRun::toString() const {
    std::string loc = location;
    for (auto& c : loc)
        if (c == ';' || c == '|' || c == '\n') c = ' ';
    return strf("%s|%.0f|%.2f|%.2f|%.2f", loc.c_str(), frames, avg, low1, low01);
}

bool TotkRun::fromString(const std::string& s) {
    auto p = split(s, '|');
    if (p.size() != 5) return false;
    location = p[0];
    bool ok[4];
    frames = toDouble(p[1], 0, &ok[0]);
    avg = toDouble(p[2], 0, &ok[1]);
    low1 = toDouble(p[3], 0, &ok[2]);
    low01 = toDouble(p[4], 0, &ok[3]);
    return ok[0] && ok[1] && ok[2] && ok[3];
}

// Число после метки ("Average FPS: 57" / "Average FPS: 57.3"). Возвращает -1, если не нашлось.
static double numberAfter(const std::string& line, const char* label) {
    std::string low = line;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    std::string lab = label;
    for (auto& c : lab) c = (char)tolower((unsigned char)c);
    size_t p = 0;
    while ((p = low.find(lab, p)) != std::string::npos) {
        // "1% fps" не должен находиться внутри "0.1% fps".
        if (lab[0] == '1' && p > 0 && (low[p - 1] == '.' || isdigit((unsigned char)low[p - 1]))) {
            p += lab.size();
            continue;
        }
        size_t q = p + lab.size();
        while (q < low.size() && (low[q] == ' ' || low[q] == ':' || low[q] == '=' || low[q] == '\t')) q++;
        // Число: цифры и не больше одного разделителя, за которым идут цифры ("57", "57.5", "57,5").
        size_t e = q;
        bool sep = false;
        while (e < low.size()) {
            char c = low[e];
            if (isdigit((unsigned char)c)) {
                e++;
            } else if ((c == '.' || c == ',') && !sep && e + 1 < low.size() && isdigit((unsigned char)low[e + 1])) {
                sep = true;
                e++;
            } else {
                break;
            }
        }
        if (e > q) {
            std::string num = line.substr(q, e - q);
            std::replace(num.begin(), num.end(), ',', '.');
            bool ok = false;
            double v = toDouble(num, -1, &ok);
            if (ok) return v;
        }
        p = q;
    }
    return -1;
}

std::vector<TotkRun> parseTotkBench(const std::string& text) {
    std::vector<TotkRun> out;
    std::string pendingLoc;
    bool havePending = false;
    for (auto& raw : split(text, '\n')) {
        std::string line = trim(raw);
        if (line.empty()) continue;
        std::string up = line;
        for (auto& c : up) c = (char)toupper((unsigned char)c);
        size_t b = up.find("BENCHMARK FOR ");
        if (b != std::string::npos) {
            size_t s = b + strlen("BENCHMARK FOR ");
            size_t e = up.find(" COMPLETED", s);
            pendingLoc = trim(line.substr(s, e == std::string::npos ? std::string::npos : e - s));
            havePending = true;
            continue;
        }
        double frames = numberAfter(line, "total frames");
        double avg = numberAfter(line, "average fps");
        if (avg < 0) avg = numberAfter(line, "avg fps");
        double l01 = numberAfter(line, "0.1% fps");
        if (l01 < 0) l01 = numberAfter(line, "0.1%");
        double l1 = numberAfter(line, "1% fps");
        if (l1 < 0) l1 = numberAfter(line, "1%");
        if (avg >= 0 && (frames >= 0 || l1 >= 0)) {
            TotkRun r;
            r.location = havePending ? pendingLoc : std::string("?");
            r.avg = avg;
            r.frames = frames >= 0 ? frames : 0;
            r.low1 = l1 >= 0 ? l1 : 0;
            r.low01 = l01 >= 0 ? l01 : 0;
            out.push_back(r);
            havePending = false;
        }
    }
    return out;
}

static u64 fnv1a(const std::string& s) {
    u64 h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::vector<std::string> benchFileCandidates() {
    // FAT не различает регистр, поэтому других написаний перебирать не нужно.
    return {sdRoot() + "TOTKBench.txt", sdRoot() + "TOTKBenchmark.txt"};
}

static std::vector<std::string> benchFilesForMarks() {
    std::vector<std::string> out;
    for (auto& p : benchFileCandidates())
        if (fileExists(p)) out.push_back(p);
    if (out.empty()) out.push_back(sdRoot() + "TOTKBench.txt");
    return out;
}

static void appendText(const std::string& path, const std::string& text) {
    std::string old;
    readFile(path, old);
    // Мод мог оставить файл без перевода строки в конце — тогда подпись прилипла бы к его строке.
    std::string pre = (!old.empty() && old.back() != '\n') ? "\n" : "";
    writeFile(path, old + pre + text);
}

void appendBenchMarker(const std::string& title, const std::string& detail) {
    const std::string line(72, '-');
    std::string t = "\n" + line + "\n" + title + "\n";
    if (!detail.empty()) t += detail + "\n";
    t += line + "\n";
    for (auto& p : benchFilesForMarks()) appendText(p, t);
}

void appendBenchNote(const std::string& line) {
    for (auto& p : benchFilesForMarks()) appendText(p, line + "\n");
}

static std::vector<std::string> crashDirs() {
    // erpt_reports не берём: туда пишутся и обычные системные события, не только падения.
    return {sdRoot() + "atmosphere/crash_reports", sdRoot() + "atmosphere/fatal_reports"};
}

static std::vector<std::string> listCrashFiles() {
    std::vector<std::string> out;
    for (auto& d : crashDirs())
        for (auto& f : listDir(d)) out.push_back(d + "/" + f);
    std::sort(out.begin(), out.end());
    return out;
}

BenchSnapshot takeBenchSnapshot() {
    BenchSnapshot s;
    s.epoch = epochNow();
    std::vector<std::string> seen;
    for (auto& path : benchFileCandidates()) {
        BenchSnapshot::File f;
        f.path = path;
        u64 size = 0;
        s64 mt = 0;
        if (fileStat(path, &size, &mt)) {
            std::string data;
            readFile(path, data);
            f.exists = true;
            f.size = data.size();
            f.mtime = mt;
            f.hash = fnv1a(data);
            f.runs = (int)parseTotkBench(data).size();
        }
        s.files.push_back(f);
    }
    s.crashFiles = listCrashFiles();
    return s;
}

std::string BenchSnapshot::serialize() const {
    std::string out = strf("%lld", (long long)epoch);
    for (auto& f : files)
        out += strf(";%s,%d,%llu,%lld,%llu,%d", f.path.c_str(), f.exists ? 1 : 0, (unsigned long long)f.size, (long long)f.mtime,
                    (unsigned long long)f.hash, f.runs);
    out += ";#crash";
    for (auto& c : crashFiles) out += ";" + c;
    return out;
}

bool BenchSnapshot::deserialize(const std::string& s) {
    files.clear();
    crashFiles.clear();
    auto parts = split(s, ';');
    if (parts.empty()) return false;
    epoch = (s64)toLong(parts[0], 0);
    bool inCrash = false;
    for (size_t i = 1; i < parts.size(); i++) {
        if (parts[i] == "#crash") {
            inCrash = true;
            continue;
        }
        if (inCrash) {
            if (!parts[i].empty()) crashFiles.push_back(parts[i]);
            continue;
        }
        auto f = split(parts[i], ',');
        if (f.size() != 6) continue;
        File x;
        x.path = f[0];
        x.exists = f[1] == "1";
        x.size = (u64)strtoull(f[2].c_str(), nullptr, 10);
        x.mtime = (s64)strtoll(f[3].c_str(), nullptr, 10);
        x.hash = (u64)strtoull(f[4].c_str(), nullptr, 10);
        x.runs = (int)toLong(f[5], 0);
        files.push_back(x);
    }
    return true;
}

std::vector<TotkRun> newRunsSince(const BenchSnapshot& before, std::string* latestFile) {
    std::vector<TotkRun> best;
    s64 bestMt = -1;
    for (auto& path : benchFileCandidates()) {
        u64 size = 0;
        s64 mt = 0;
        if (!fileStat(path, &size, &mt)) continue;
        std::string data;
        if (!readFile(path, data)) continue;
        u64 h = fnv1a(data);
        const BenchSnapshot::File* old = nullptr;
        for (auto& f : before.files)
            if (f.path == path) old = &f;
        if (old && old->exists && old->hash == h && old->size == data.size()) continue;  // файл не менялся
        auto runs = parseTotkBench(data);
        if (runs.empty()) continue;
        std::vector<TotkRun> fresh;
        if (old && old->exists && (int)runs.size() > old->runs && data.size() > old->size) {
            // Мод дописывает в конец — новые блоки после старых.
            fresh.assign(runs.begin() + old->runs, runs.end());
        } else {
            // Файл перезаписан — новый результат один, последний.
            fresh.push_back(runs.back());
        }
        if (mt >= bestMt) {
            bestMt = mt;
            best = fresh;
            if (latestFile) *latestFile = path;
        }
    }
    return best;
}

bool lastRun(TotkRun* out, std::string* file) {
    s64 bestMt = -1;
    bool found = false;
    for (auto& path : benchFileCandidates()) {
        u64 size = 0;
        s64 mt = 0;
        if (!fileStat(path, &size, &mt)) continue;
        std::string data;
        if (!readFile(path, data)) continue;
        auto runs = parseTotkBench(data);
        if (runs.empty() || mt < bestMt) continue;
        bestMt = mt;
        *out = runs.back();
        if (file) *file = path;
        found = true;
    }
    return found;
}

std::vector<std::string> crashReportNames() {
    std::vector<std::string> out;
    for (auto& f : listCrashFiles()) {
        size_t p = f.rfind('/');
        out.push_back(p == std::string::npos ? f : f.substr(p + 1));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> newCrashReports(const BenchSnapshot& before) {
    std::vector<std::string> out;
    for (auto& f : listCrashFiles())
        if (!std::binary_search(before.crashFiles.begin(), before.crashFiles.end(), f)) out.push_back(f);
    return out;
}

std::string findUltraCamIni() {
    const char* rel[] = {
        "atmosphere/contents/0100F2C0115B6000/romfs/UltraCam/maxlastbreath.ini",
        "atmosphere/contents/0100F2C0115B6000/files/UltraCam/maxlastbreath.ini",
        "atmosphere/contents/0100F2C0115B6000/romfs/ultracam/maxlastbreath.ini",
    };
    for (auto r : rel)
        if (fileExists(sdRoot() + r)) return sdRoot() + r;
    return std::string();
}

int readBenchmarkIndex(const std::string& iniPath) {
    std::string data;
    if (iniPath.empty() || !readFile(iniPath, data)) return -1;
    bool inSec = false;
    for (auto& raw : split(data, '\n')) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (l[0] == '[') {
            inSec = l == "[Benchmark]";
            continue;
        }
        if (!inSec) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if (trim(l.substr(0, eq)) == "Benchmark") return (int)toLong(l.substr(eq + 1), -1);
    }
    return -1;
}

bool writeBenchmarkIndex(const std::string& iniPath, int index) {
    std::string data;
    if (iniPath.empty() || !readFile(iniPath, data)) return false;
    auto lines = split(data, '\n');
    bool inSec = false, done = false;
    for (auto& raw : lines) {
        std::string l = trim(raw);
        if (!l.empty() && l[0] == '[') {
            inSec = l == "[Benchmark]";
            continue;
        }
        if (!inSec || l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq != std::string::npos && trim(l.substr(0, eq)) == "Benchmark") {
            bool cr = !raw.empty() && raw.back() == '\r';
            raw = strf("Benchmark = %d", index) + (cr ? "\r" : "");
            done = true;
        }
    }
    if (!done) return false;
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        out += lines[i];
        if (i + 1 < lines.size()) out += "\n";
    }
    return writeFileSafe(iniPath, out);
}

const char* benchmarkName(int index) {
    switch (index) {
        case 0: return "Kakariko";
        case 1: return "Great Sky Island";
        case 2: return "Lookout Landing";
        case 3: return "Goron City";
        case 4: return "Korok Forest";
        default: return "?";
    }
}

bool launchTotk(std::string* err) {
#ifdef __SWITCH__
    Result rc = appletRequestLaunchApplication(TOTK_TITLE_ID, nullptr);
    if (R_FAILED(rc)) {
        if (err) *err = strf("система не запустила игру (0x%X) — запустите TOTK вручную", rc);
        return false;
    }
    return true;
#else
    if (err) *err = "запуск игры доступен только на консоли";
    return false;
#endif
}
