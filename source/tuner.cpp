#include "tuner.hpp"

#include "archive.hpp"
#include "history.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// ============================================================== Settings

const char* Settings::ramModeName(int mode) {
    return mode == ModeMax ? "максимум" : mode == ModeMedium ? "средний" : "безопасный";
}

int Settings::nextRamMode(int mode) { return mode == ModeSafe ? ModeMedium : mode == ModeMedium ? ModeMax : ModeSafe; }

void Settings::applyRamMode(int mode) {
    ramMode = (mode == ModeMax || mode == ModeMedium) ? mode : ModeSafe;
    if (ramMode == ModeMedium) {
        // Средний, околостабильный: каждое значение — на ступень мягче найденного предела.
        margin = 1;
        quickSec = 8;
        verifySec = 120;
        kipCheckSec = 60;
        wGame = 65;
        advanced = true;
        allowTrefiHigh = false;  // 11700 нс портит данные на жаре — не для «около стабильного»
        timingPasses = 5;
        refreshTune = false;     // финиш tREFI × tRFC — ради последних долей процента, не ради стабильности
    } else if (ramMode == ModeSafe) {
        // Безопасный: стабильность гарантирована — длинные проверки и запас, без рискованных ручек.
        margin = 2;
        quickSec = 10;
        verifySec = 300;
        kipCheckSec = 120;
        wGame = 50;
        advanced = false;
        allowTrefiHigh = false;
        timingPasses = 5;
        refreshTune = false;
    } else {
        // Максимум: поток и TOTK — главное, стабильность — на втором плане.
        margin = 0;
        quickSec = 6;
        verifySec = 60;
        kipCheckSec = 30;
        wGame = 80;
        advanced = true;
        allowTrefiHigh = true;
        // Максимум: ловим и самые мелкие приросты (от 0,12 %). 9 замеров — отбор; прирост, похожий на настоящий,
        // подтверждается парным замером А/Б (см. Tuner::pairedConfirm), там и набирается точность.
        timingPasses = 9;
        refreshTune = true;
    }
    totkRuns = 3;
}

std::string Settings::serialize() const {
    std::string s;
    s += strf("ram_mode=%d\n", ramMode);
    s += strf("totk_runs=%d\n", totkRuns);
    s += "settings_v=7\n";
    s += strf("w_bw=%d\n", wBw);
    s += strf("w_game=%d\n", wGame);
    s += strf("quick_sec=%d\n", quickSec);
    s += strf("verify_sec=%d\n", verifySec);
    s += strf("bench_passes=%d\n", benchPasses);
    s += strf("margin=%d\n", margin);
    s += strf("patience=%d\n", patience);
    s += strf("min_mhz=%d\n", minMhz);
    s += strf("max_mhz=%d\n", maxMhz);
    s += strf("scan_fail_stop=%d\n", scanFailStop);
    s += strf("num_targets=%d\n", numTargets);
    s += strf("temp_limit=%d\n", tempLimit);
    s += strf("allow_trefi_high=%d\n", allowTrefiHigh ? 1 : 0);
    s += strf("advanced=%d\n", advanced ? 1 : 0);
    s += strf("use_preset=%d\n", usePreset ? 1 : 0);
    s += strf("use_daemon_log=%d\n", useDaemonLog ? 1 : 0);
    s += strf("auto_launch=%d\n", autoLaunch ? 1 : 0);
    s += strf("cpu_tune_mhz=%d\n", cpuTuneMhz);
    s += strf("cpu_game_mhz=%d\n", cpuGameMhz);
    s += strf("gpu_game_mhz=%d\n", gpuGameMhz);
    s += strf("optimize=%d\n", optimize ? 1 : 0);
    s += strf("scan_guide=%d\n", scanGuide ? 1 : 0);
    s += strf("peak_drop_mb=%d\n", peakDropMb);
    s += strf("peak_targets=%d\n", peakTargets);
    s += strf("scan_passes=%d\n", scanPasses);
    s += strf("scan_hang_stop=%d\n", scanHangStop);
    s += strf("research_runs=%d\n", researchRuns);
    s += strf("auto_resume=%d\n", autoResume ? 1 : 0);
    s += strf("timing_passes=%d\n", timingPasses);
    s += strf("kip_stage=%d\n", kipStage ? 1 : 0);
    s += strf("kip_first=%d\n", kipFirst ? 1 : 0);
    s += strf("kip_check_sec=%d\n", kipCheckSec);
    s += strf("refresh_tune=%d\n", refreshTune ? 1 : 0);
    s += strf("hm=%d\n", hidden);
    return s;
}

void Settings::deserialize(const std::string& text) {
    // До 1.5.0 по умолчанию поток и задержка весили поровну; теперь цель — пропускная способность.
    bool v7 = text.find("settings_v=7") != std::string::npos;
    bool v6 = text.find("settings_v=6") != std::string::npos || v7;
    bool v5 = text.find("settings_v=5") != std::string::npos || v6;
    bool v4 = text.find("settings_v=4") != std::string::npos || v5;
    bool v2 = text.find("settings_v=2") != std::string::npos || text.find("settings_v=3") != std::string::npos || v4;
    // До 1.6.7 порог пика потока по умолчанию был 150 МБ/с, теперь 100: прежнее значение по умолчанию переводим.
    bool v3 = text.find("settings_v=3") != std::string::npos || v4;
    for (auto& raw : split(text, '\n')) {
        std::string l = trim(raw);
        size_t eq = l.find('=');
        if (l.empty() || l[0] == ';' || eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq));
        bool ok = false;
        long v = toLong(l.substr(eq + 1), 0, &ok);
        if (!ok) continue;
        auto clampi = [](long x, long lo, long hi) { return (int)std::max(lo, std::min(hi, x)); };
        // До 1.7.6 по умолчанию задержка не учитывалась (100 %); теперь 80 %: тайминги строк дают в основном задержку.
        if (k == "w_bw") wBw = !v2 ? 80 : (!v4 && v == 100) ? 80 : clampi(v, 0, 100);
        else if (k == "ram_mode") ramMode = clampi(v, 0, 2);
        else if (k == "totk_runs") totkRuns = clampi(v, 1, 10);
        else if (k == "w_game") wGame = clampi(v, 0, 100);
        else if (k == "quick_sec") quickSec = clampi(v, 1, 120);
        else if (k == "verify_sec") verifySec = clampi(v, 5, 1800);
        else if (k == "bench_passes") benchPasses = clampi(v, 1, 10);
        else if (k == "margin") margin = clampi(v, 0, 3);
        else if (k == "patience") patience = clampi(v, 1, 5);
        else if (k == "min_mhz") minMhz = clampi(v, 0, 4000);
        else if (k == "max_mhz") maxMhz = clampi(v, 0, 4000);
        else if (k == "scan_fail_stop") scanFailStop = clampi(v, 0, 20);
        else if (k == "num_targets") numTargets = clampi(v, 1, 4);
        else if (k == "temp_limit") tempLimit = clampi(v, 50, 90);
        else if (k == "allow_trefi_high") allowTrefiHigh = v != 0;
        else if (k == "advanced") advanced = v != 0;
        else if (k == "use_preset") usePreset = v != 0;
        else if (k == "use_daemon_log") useDaemonLog = v != 0;
        else if (k == "auto_launch") autoLaunch = v != 0;
        else if (k == "cpu_tune_mhz") cpuTuneMhz = clampi(v, 0, 3100);
        else if (k == "cpu_game_mhz") cpuGameMhz = clampi(v, 0, 3100);
        else if (k == "gpu_game_mhz") gpuGameMhz = clampi(v, 0, 1600);
        else if (k == "optimize") optimize = v != 0;
        else if (k == "scan_guide") scanGuide = v != 0;
        else if (k == "peak_drop_mb") peakDropMb = (!v3 && v == 150) ? 100 : clampi(v, 20, 2000);
        else if (k == "peak_targets") peakTargets = clampi(v, 0, 4);
        else if (k == "scan_passes") scanPasses = clampi(v, 1, 10);
        else if (k == "scan_hang_stop") scanHangStop = clampi(v, 0, 10);
        else if (k == "research_runs") researchRuns = clampi(v, 1, 10);
        else if (k == "auto_resume") autoResume = v != 0;
        else if (k == "timing_passes") timingPasses = clampi(v, 1, 30);
        else if (k == "kip_stage") kipStage = v != 0;
        else if (k == "kip_first") kipFirst = v != 0;
        else if (k == "kip_check_sec") kipCheckSec = clampi(v, 10, 600);
        else if (k == "refresh_tune") refreshTune = v != 0;
        else if (k == "hm") hidden = clampi(v, 0, 255);
    }
    // Настройки до 1.6.0 — без режима: ставим безопасный целиком (его проверки длиннее прежних).
    if (text.find("ram_mode=") == std::string::npos) applyRamMode(0);
    // У «максимума» по умолчанию было 5 замеров (до 1.8.3), потом 15 (до 1.8.7; при чтении урезалось до 10).
    // Теперь 9 на отбор, точность — у парного подтверждения: прежние значения по умолчанию переводим.
    if (!v6 && ramMode == ModeMax && (timingPasses == 5 || timingPasses == 10 || timingPasses == 15)) timingPasses = 9;
    // До 2.0 финиш tREFI × tRFC по умолчанию стоял включённым во всех режимах (работал только в «максимуме»):
    // теперь он по умолчанию — только в «максимуме», в остальных выключен ради стабильности.
    if (!v7) refreshTune = ramMode == ModeMax && (text.find("refresh_tune=") == std::string::npos || refreshTune);
}

bool Settings::load() {
    std::string text;
    if (!readFile(paths::settings(), text)) {
        applyRamMode(ramMode);  // первый запуск: значения режима по умолчанию (безопасный)
        return false;
    }
    deserialize(text);
    return true;
}

bool Settings::save() const {
    makeDirs(paths::appDir());
    return writeFileSafe(paths::settings(), serialize());
}

// ============================================================== мелочи

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "не запущен";
        case Phase::Baseline: return "1. Базовый замер";
        case Phase::FreqScan: return "2. Прогон частот";
        case Phase::Seed: return "3. Тайминги: старт";
        case Phase::Timing: return "3. Тайминги";
        case Phase::Verify: return "5. Длинная проверка";
        case Phase::Optimize: return "4. Оптимизация таймингов";
        case Phase::KipFirst: return "1б. Кип сначала (перезагрузка)";
        case Phase::Kip: return "6. Перенос в кип (перезагрузки)";
        case Phase::Refresh: return "6б. Финиш: tREFI × tRFC";
        case Phase::Game: return "7. Проверка в TOTK";
        case Phase::Done: return "Готово";
        case Phase::Failed: return "Остановлен с ошибкой";
    }
    return "?";
}

// Только ручки пропускной способности на нуле (сток движка); первичные тайминги — из кипа.
static KnobSet stockBwSet() {
    KnobSet s;
    for (auto& k : allKnobs())
        if (k.absentIsZero) s[k.key] = 0;
    return s;
}

KnobSet ryazhaStockSet() {
    KnobSet s;
    for (auto& k : allKnobs()) s[k.key] = k.absentIsZero ? 0 : std::max(0, k.min);
    return s;
}

std::string UnstableEntry::toString() const {
    auto cl = [](std::string x) {
        for (auto& c : x)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        return x;
    };
    return strf("%u|%s|%s|%d|%s|%s|%lld", hz, cl(kind).c_str(), key.empty() ? "-" : key.c_str(), val, knobSetToString(set).c_str(), cl(detail).c_str(),
                (long long)at);
}

bool UnstableEntry::fromString(const std::string& s) {
    auto p = split(s, '|');
    if (p.size() < 7) return false;
    hz = (u32)strtoul(p[0].c_str(), nullptr, 10);
    kind = p[1];
    key = p[2] == "-" ? std::string() : p[2];
    val = (int)toLong(p[3], 0);
    knobSetFromString(p[4], set);
    detail = p[5];
    at = (s64)strtoll(p[6].c_str(), nullptr, 10);
    return hz != 0;
}

static std::string failKind(const std::string& reason) {
    if (reason.find("memtest") != std::string::npos) return "memtest";
    if (reason.find("зависание") != std::string::npos) return "зависание";
    if (reason.find("PLL") != std::string::npos) return "PLL drop";
    if (reason.find("медленнее") != std::string::npos) return "просадка потока";
    return "сбой";
}

void Tuner::noteUnstable(u32 hz, const std::string& kind, const std::string& key, int val, const KnobSet& set, const std::string& detail) {
    UnstableEntry e;
    e.hz = hz;
    e.kind = kind;
    e.key = key;
    e.val = val;
    e.set = set;
    e.detail = detail;
    e.at = epochNow();
    unstable.push_back(e);
    if (unstable.size() > 2000) unstable.erase(unstable.begin(), unstable.begin() + (unstable.size() - 2000));
    unstableDirty = true;
    TLOG("  ⚑ нестабильно [%s] %s МГц%s: %s | набор: %s", kind.c_str(), mhzStr(hz).c_str(), key.empty() ? "" : strf(", %s = %d", key.c_str(), val).c_str(),
         detail.c_str(), knobSetShort(set).c_str());
}

void Tuner::loadUnstable(ITarget& t) {
    if (unstableLoaded) return;
    unstableLoaded = true;
    std::string text;
    if (!t.loadText(paths::unstable(), text)) return;
    std::vector<UnstableEntry> loaded;
    int dropped = 0;
    for (auto& l : split(text, '\n')) {
        UnstableEntry e;
        if (trim(l).empty() || trim(l)[0] == ';' || !e.fromString(trim(l))) continue;
        // До 1.7.7 пробы в безопасном режиме сторожа (после зависания частота не ставится, стоит 1600) шли как сбои.
        // Это не нестабильность ручки — такие записи выбрасываем.
        if (e.detail.find("pcv не перешёл") != std::string::npos && e.detail.find("стоит 1600") != std::string::npos) {
            dropped++;
            continue;
        }
        loaded.push_back(e);
    }
    if (dropped) {
        LOGI("Реестр нестабильных: выброшено %d записей «pcv не перешёл, стоит 1600» — пробы в безопасном режиме сторожа, не сбои", dropped);
        unstableDirty = true;
    }
    unstable.insert(unstable.begin(), loaded.begin(), loaded.end());
}

void Tuner::flushUnstable(ITarget& t) {
    std::string o;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (!unstableDirty) return;
        unstableDirty = false;
        o = "; RamSweetSpot: значения, на которых консоль была нестабильна. частота|вид|ручка|значение|набор|подробности|время\n";
        for (auto& e : unstable) o += e.toString() + "\n";
    }
    t.saveText(paths::unstable(), o);
}

bool Tuner::unstableBound(u32 hz, const std::string& key, int* bound) const {
    const KnobDef* d = findKnob(key);
    if (!d) return false;
    bool have = false;
    for (auto& e : unstable) {
        if (e.hz != hz || e.key != key) continue;
        if (e.kind != "memtest" && e.kind != "зависание") continue;
        if (d->order == KnobOrder::Up) {
            if (!have || e.val < *bound) *bound = e.val;
        } else if (d->order == KnobOrder::Down) {
            if (!have || e.val > *bound) *bound = e.val;
        } else {
            continue;
        }
        have = true;
    }
    return have;
}

int kipSlotOf(const std::string& key) {
    // Ключи пропускной способности timings.ini и их слоты tMEH (timing_overrides.cpp, массив ARR_T).
    static const struct {
        const char* key;
        int slot;
    } kMap[] = {{"RL", 0},       {"WL", 1},      {"WLset", 2},   {"tFAW", 3},      {"rext", 4},     {"wext", 5},     {"arbSafe", 6},
                {"arbWin", 7},   {"latAllow", 8}, {"tRPab", 9},  {"tXP", 10},      {"tCKESR", 11},  {"preRef", 12},  {"tW2P", 13},
                {"ramPD", 14},   {"arbPrio", 15}, {"outReq", 16}, {"ring1", 17},   {"prevn", 18},   {"preSlack", 19}, {"drvDQ", 20},
                {"odtDQ", 21},   {"odtCA", 22},  {"odtSoC", 23}, {"ptsaCpu", 24},  {"soonSlack", 25}, {"daTurns", 26}, {"daCovers", 27}};
    for (auto& m : kMap)
        if (key == m.key) return m.slot;
    return -1;
}

static bool loosenTimingIn(KnobSet& set, int round, std::string* desc);  // порядок отжима — ниже, у loosenForVerify()

// --- финиш tREFI × tRFC
// Доля времени на регенерацию ≈ tRFC / tREFI (ступени — как в knobValueText).
static double refreshShare(Tuner::RfPair p) {
    static const int rfc[] = {140, 130, 120, 110, 100, 90, 80, 70, 60, 50, 40};
    static const int refi[] = {3900, 5850, 7800, 11700, 15600};
    if (p.first < 0 || p.first > 4 || p.second < 0 || p.second > 10) return 1;
    return (double)rfc[p.second] / refi[p.first];
}
static KnobSet withPair(KnobSet s, Tuner::RfPair p) {
    s["tREFI"] = p.first;
    s["tRFC"] = p.second;
    return s;
}
static Tuner::RfPair pairIn(const KnobSet& s) {
    auto r = s.find("tREFI"), f = s.find("tRFC");
    return {r != s.end() ? r->second : 0, f != s.end() ? f->second : 0};
}
static int pairCode(Tuner::RfPair p) { return p.first * 100 + p.second; }
static Tuner::RfPair pairOf(int v) { return {v / 100, v % 100}; }
static std::string pairStr(Tuner::RfPair p) {
    const KnobDef* dr = findKnob("tREFI");
    const KnobDef* df = findKnob("tRFC");
    return strf("tREFI %d (%s), tRFC %d (%s)", p.first, dr ? knobValueText(*dr, p.first).c_str() : "?", p.second,
                df ? knobValueText(*df, p.second).c_str() : "?");
}
static std::string pairsToStr(const std::vector<Tuner::RfPair>& v) {
    std::string o;
    for (auto& p : v) o += (o.empty() ? "" : ";") + strf("%d:%d", p.first, p.second);
    return o.empty() ? "-" : o;
}
static std::vector<Tuner::RfPair> pairsFromStr(const std::string& s) {
    std::vector<Tuner::RfPair> v;
    for (auto& x : split(trim(s), ';')) {
        auto q = split(x, ':');
        if (q.size() == 2) v.push_back({(int)toLong(q[0], 0), (int)toLong(q[1], 0)});
    }
    return v;
}

// Набор с этим значением ручки (ноль у «нет в наборе = 0» — убрать из набора).
static KnobSet withKnob(KnobSet s, const std::string& key, int val) {
    const KnobDef* d = findKnob(key);
    if (d && d->absentIsZero && val == 0)
        s.erase(key);
    else
        s[key] = val;
    return s;
}

static std::string clean(const std::string& s) {
    std::string o = s;
    for (auto& c : o)
        if (c == '\n' || c == '\r' || c == '|') c = ' ';
    return o;
}

std::string Candidate::name() const {
    if (baseline) return "Как было";
    return strf("К%d · %s МГц", id, mhzStr(hz).c_str());
}

std::string TrialSpec::serialize() const {
    return strf("%d|%u|%s|%d|%d|%s|%d|%s|%s", kind, hz, knobSetToString(set).c_str(), stabSec, passes, key.empty() ? "-" : key.c_str(), val,
                clean(desc).c_str(), knobSetToString(ref).c_str());
}

bool TrialSpec::deserialize(const std::string& s) {
    auto p = split(s, '|');
    if (p.size() < 8) return false;
    kind = (int)toLong(p[0], 0);
    hz = (u32)strtoul(p[1].c_str(), nullptr, 10);
    if (!knobSetFromString(p[2], set)) return false;
    stabSec = (int)toLong(p[3], 0);
    passes = (int)toLong(p[4], 1);
    key = p[5] == "-" ? std::string() : p[5];
    val = (int)toLong(p[6], 0);
    desc = p[7];
    ref.clear();
    if (p.size() >= 9 && !knobSetFromString(p[8], ref)) return false;  // до 1.8.7 поля не было
    return kind != None;
}

void Tuner::ev(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrf(fmt, ap);
    va_end(ap);
    events.push_back(clean(s));
    LOGI("[подбор] %s", events.back().c_str());
    TLOG("· %s", events.back().c_str());
    if (events.size() > 80) events.erase(events.begin(), events.begin() + (events.size() - 80));
}

double Tuner::scoreOf(const BenchResult& b) const {
    if (!b.valid || !base.valid || b.bw() <= 0 || b.latNs <= 0 || base.bw() <= 0 || base.latNs <= 0) return 0;
    double wb = cfg.wBw / 100.0, wl = 1.0 - wb;
    return std::pow(b.bw() / base.bw(), wb) * std::pow(base.latNs / b.latNs, wl);
}

// Оценка одного прохода — как scoreOf, без деления на базу (для шума важны только относительные отклонения).
static double passScore(const BenchPass& p, int wBw) {
    if (p.r <= 0 || p.w <= 0 || p.c <= 0 || p.l <= 0) return 0;
    double wb = wBw / 100.0;
    return std::pow(std::cbrt(p.r * p.w * p.c), wb) * std::pow(1.0 / p.l, 1.0 - wb);
}

double Tuner::passSigma() const {
    // Меньше 20 степеней свободы — оценка ненадёжна: берём осторожную 0,15 % (на консоли в 1.8.4 было 0,136 %).
    if (noiseDf < 20) return 0.0015;
    return std::sqrt(noiseSS / noiseDf);
}

double Tuner::trialSigma(int passes) const {
    // Усечённое среднее нормального шума: дисперсия примерно на 20 % больше, чем у простого среднего.
    if (passes < 3) return passSigma();
    return passSigma() * 1.1 / std::sqrt((double)passes);
}

double Tuner::pairNoise() const {
    double sd = std::sqrt(2.0) * trialSigma(kPairPasses) / std::sqrt((double)kPairMaxBlocks);
    return 2.5 * std::max(kPairSigmaTarget, sd);
}

void Tuner::addNoise(const BenchResult& b) {
    if (b.passes.size() < 3) return;
    std::vector<double> s;
    for (auto& p : b.passes) {
        double v = passScore(p, cfg.wBw);
        if (v > 0) s.push_back(v);
    }
    if (s.size() < 3) return;
    double m = 0;
    for (double v : s) m += v;
    m /= (double)s.size();
    // Выброс (чужая нагрузка) итог пробы не сдвигает — усечённое среднее; в шум он идёт не больше чем 5σ.
    double cap = 5 * passSigma();
    for (double v : s) {
        double d = std::max(-cap, std::min(cap, v / m - 1));
        noiseSS += d * d;
    }
    noiseDf += (int)s.size() - 1;
}

TrialOut Tuner::pairedConfirm(ITarget& t, u32 hz, const KnobSet& a, const KnobSet& b, std::atomic<bool>* stop, double thr) {
    // Эталон А и кандидат Б по очереди (А Б, Б А, А Б …): медленный дрейф (нагрев, фон) одинаково сдвигает обе
    // половины блока и из разницы уходит. Решение — по средней разнице блоков, а не по сравнению со старым замером.
    TrialOut out;
    std::vector<double> d;
    std::vector<BenchPass> pa, pb;
    double sumA = 0, sumB = 0;
    double sdModel = std::sqrt(2.0) * trialSigma(kPairPasses);
    for (int blk = 0; blk < kPairMaxBlocks; blk++) {
        double sc[2] = {0, 0};
        for (int half = 0; half < 2; half++) {
            bool isB = (blk % 2 == 0) == (half == 1);
            TrialSpec s;
            s.kind = TrialSpec::Confirm;
            s.hz = hz;
            s.set = isB ? b : a;
            s.stabSec = 0;
            s.passes = kPairPasses;
            TrialOut o = runTrialCore(t, s, stop);
            if (o.aborted) {
                out.aborted = true;
                return out;
            }
            if (!o.ok) {
                out.refFailed = !isB;
                out.reason = (isB ? "кандидат: " : "эталон: ") + o.reason;
                out.blocks = blk;
                return out;
            }
            out.extra.push_back(o.b);
            out.ai = o.ai;
            sc[isB ? 1 : 0] = o.score;
            auto& dst = isB ? pb : pa;
            dst.insert(dst.end(), o.b.passes.begin(), o.b.passes.end());
        }
        sumA += sc[0];
        sumB += sc[1];
        d.push_back(sc[1] / sc[0] - 1);
        int n = (int)d.size();
        double mean = 0;
        for (double x : d) mean += x;
        mean /= n;
        double sd = sdModel;
        if (n >= 4) {
            // С 4 блоков — и по самим блокам: если разница гуляет сильнее модели (дрейф быстрее блока), верим ей.
            double v = 0;
            for (double x : d) v += (x - mean) * (x - mean);
            sd = std::max(sd, std::sqrt(v / (n - 1)));
        }
        out.delta = mean;
        out.sigma = sd / std::sqrt((double)n);
        out.blocks = n;
        TLOG("  А/Б блок %d: А %.4f, Б %.4f (%+.3f %%) · среднее %+.3f %% ± %.3f %%", n, sc[0], sc[1], d.back() * 100, mean * 100, out.sigma * 100);
        if (n >= kPairMinBlocks && (out.sigma <= kPairSigmaTarget || std::fabs(mean - thr) >= 2.5 * out.sigma)) break;
    }
    out.ok = true;
    out.b = aggregatePasses(pb);
    out.score = sumB / out.blocks;
    out.refScore = sumA / out.blocks;
    return out;
}

u32 Tuner::stockHz() const {
    u32 best = 1600000000u;
    u32 bestD = 0xFFFFFFFF;
    for (u32 hz : ladder) {
        u32 d = hz > 1600000000u ? hz - 1600000000u : 1600000000u - hz;
        if (d < bestD) {
            bestD = d;
            best = hz;
        }
    }
    return best;
}

bool Tuner::isAuto() const {
    return phase == Phase::Baseline || phase == Phase::FreqScan || phase == Phase::Seed || phase == Phase::Timing || phase == Phase::Verify ||
           phase == Phase::Kip || phase == Phase::Optimize || phase == Phase::KipFirst || phase == Phase::Refresh;
}

void Tuner::resetAll() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Settings keep = cfg;
    std::vector<u32> keepLadder = ladder;
    ChipInfo keepChip = chip;
    phase = Phase::Idle;
    status.clear();
    error.clear();
    trials = 0;
    startedEpoch = 0;
    events.clear();
    baseRuns.clear();
    base = BenchResult();
    eps = 0.005;
    noiseSS = 0;
    noiseDf = 0;
    needRef = false;
    chainAB = chainEnd = false;
    benchVer = kBenchVersion;
    optLoss = 0;
    rfDone = false;
    rfQueue.clear();
    rfStage = rfPos = 0;
    rfPairs.clear();
    rfStack.clear();
    rfBad.clear();
    rfStart = rfPick = RfPair(0, 0);
    rfPickScore = 0;
    coarse.clear();
    scanPos = 0;
    lastOk = -1;
    failRun = 0;
    lastOkBw = 0;
    scanPass = 0;
    stockLastOk = -1;
    guideChosen = false;
    guideSet.clear();
    singleHz = 0;
    freqCap = 0;
    kfStep = 0;
    kfBootAt = 0;
    kfTried = kfDone = false;
    kfVals.clear();
    points.clear();
    targets.clear();
    tIdx = 0;
    seedStage = 0;
    inhScore = 0;
    presetSet.clear();
    plan.clear();
    cur.clear();
    curScore = 0;
    kIdx = 0;
    seqStart = 0;
    seqStartInh = true;
    seqPos = 0;
    haveBest = false;
    bestVal = 0;
    bestScore = startScore = 0;
    noimp = 0;
    haveFail = false;
    failVal = 0;
    remeasure = false;
    remeasureFails = 0;
    accepted.clear();
    verifyTries = 0;
    loosenRound = 0;
    cands.clear();
    gamePendingCand = -1;
    gameActiveCand = -1;
    gameSnapshot.clear();
    kipStep = KipWrite;
    kipSrc = -1;
    kipVals.clear();
    kipOrig.clear();
    kipAccepted.clear();
    kipQueue.clear();
    kipTesting.clear();
    kipFinal = kipDirty = false;
    kipRejected.clear();
    kipReboots = 0;
    kipRebootAt = 0;
    kipUptimeAt = 0;
    kipCheckIdx = 0;
    kipFixTries = 0;
    kipFixCand = -1;
    kipFixBackup.clear();
    kipPre.clear();
    kipPrePos = 0;
    kipHangs = 0;
    kipLog.clear();
    pending = TrialSpec();
    cfg = keep;
    ladder = keepLadder;
    chip = keepChip;
}

bool Tuner::bandsOn() const { return cfg.communityBands && chip.preset && chip.preset->bands; }

KnobSet Tuner::guideAt(u32 hz) const {
    KnobSet g = ryazhaStockSet();
    if (chip.preset)
        for (auto& kv : presetKnobsAt(chip.preset, hz, cfg.communityBands)) g[kv.first] = kv.second;
    return g;
}

KnobSet Tuner::presetAt(u32 hz) const {
    if (presetSet.empty() || !bandsOn()) return presetSet;
    return guideAt(hz);
}

KnobSet Tuner::kipScanSet(u32 hz) const {
    KnobSet s = stockBwSet();
    if (bandsOn()) s["WL"] = wlStepFor(hz, guideBandFor(chip.preset, hz)->wlBin);
    return s;
}

std::string Tuner::guideName() const {
    if (!chip.preset) return "гайд";
    if (bandsOn()) return strf("%s (тест, по полосам частот): %s", chip.preset->bandsSrc, bandsText(chip.preset).c_str());
    return strf("гайд Horizon-OC «Common» для %s (тир %s): %s", chip.preset->chip, chip.preset->tier,
                knobSetShort(presetToKnobs(chip.preset->common)).c_str());
}

void Tuner::begin(const std::vector<u32>& lad, const ChipInfo& c) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    resetAll();
    ladder = lad;
    std::sort(ladder.begin(), ladder.end());
    ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
    chip = c;
    startedEpoch = epochNow();
    plan.clear();
    for (auto& ak : autoKnobPlan(cfg.allowTrefiHigh, cfg.advanced)) plan.push_back({ak.def->key, ak.capMax, ak.capMin});
    if (cfg.usePreset && chip.preset) presetSet = presetToKnobs(chip.preset->common);
    phase = Phase::Baseline;
    ev("Старт подбора: %s, ступеней частоты %zu", chip.name.c_str(), ladder.size());
    if (!ladder.empty()) {
        ev("Лестница Ryazha-clk: %s … %s МГц", mhzStr(ladder.front()).c_str(), mhzStr(ladder.back()).c_str());
        // Лестницу строит загрузчик Ryazha-clk при загрузке, до «Макс. частоты РАМ» из оверлея. Если она короче
        // запрошенного верха, выше прогон не пойдёт — частот там у демона просто нет.
        u32 want = cfg.maxMhz > 0 ? (u32)cfg.maxMhz : 2900u;
        if (ladder.back() / 1000000u + 40 < want)
            ev("Внимание: лестница кончается на %s МГц — выше прогон не пойдёт. Поднимите «Макс. частоту РАМ» в Ryazha-clk и "
               "перезагрузите консоль",
               mhzStr(ladder.back()).c_str());
    }
    if (chip.preset)
        ev("Пресет «Common» для %s (тир %s): %s, ориентир %u–%u МГц", chip.preset->chip, chip.preset->tier, knobSetShort(presetSet).c_str(),
           chip.preset->minMhz, chip.preset->maxMhz);
    if (bandsOn()) ev("Тайминги гайда — %s", guideName().c_str());
}

// ============================================================== шаг

bool Tuner::step(ITarget& t, std::atomic<bool>* stop) {
    TrialSpec spec;
    std::string snap;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        loadUnstable(t);
        if (!isAuto()) return false;
    }
    {
        bool kip;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            kip = phase == Phase::Kip && (kipStep == KipWrite || kipStep == KipBoot);
        }
        if (kip) return kipAdvance(t);
    }
    {
        bool kf;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            kf = phase == Phase::KipFirst;
        }
        if (kf) return kipFirstAdvance(t);
    }
    // Остывание до начала пробы (пока ничего не применено — журнал чистый).
    int temp = t.socTempC();
    if (temp >= cfg.tempLimit) {
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            status = strf("Остывание: SoC %d °C (порог %d °C)", temp, cfg.tempLimit);
        }
        while (temp >= cfg.tempLimit - 5 && !(stop && stop->load())) {
            t.idle(2000);
            temp = t.socTempC();
            std::lock_guard<std::recursive_mutex> lk(mtx);
            status = strf("Остывание: SoC %d °C (ждём %d °C)", temp, cfg.tempLimit - 5);
        }
        return !(stop && stop->load());
    }
    bool warm = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        spec = nextTrial();
        // Перенос в кип и «кип сначала» — без прогрева: после загрузки с новым кипом первая нагрузка — сама проверка
        // загрузки, иначе зависание из-за значения кипа случилось бы в прогреве и виновник бы не нашёлся.
        warm = spec.valid() && spec.hz && !sessionWarm && phase != Phase::Kip && phase != Phase::KipFirst;
    }
    if (warm && !t.safeMode()) {
        // Первая проба сеанса (после запуска приложения, перезагрузки, возврата из игры) — сперва прогрев на рабочей
        // частоте. До записи пробы в журнал: зависание при прогреве ей не засчитывается.
        sessionWarm = true;
        if (!warmUp(t, spec)) {
            sessionWarm = false;
            return false;  // пауза
        }
        std::lock_guard<std::recursive_mutex> lk(mtx);
        spec = nextTrial();  // после прогрева эталон меряется заново — следующая проба могла смениться
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (!spec.valid()) {
            snap = serialize();
        } else {
            pending = spec;
            pendingBoot = t.bootEpoch();
            pendingCrash = crashReportNames();
            trials++;
            status = spec.desc;
            snap = serialize();
        }
    }
    t.saveText(paths::state(), snap);
    if (!spec.valid()) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        return isAuto();
    }

    TrialOut out = runTrial(t, spec, stop);

    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        pending = TrialSpec();
        if (out.aborted) {
            trials--;
            status = safeHalt ? "Ryazha-clk в безопасном режиме после зависания — нужна перезагрузка, проба будет повторена"
                              : "Пауза — проба будет повторена";
        } else {
            handleResult(spec, out);
        }
        snap = serialize();
    }
    t.saveText(paths::state(), snap);
    flushUnstable(t);
    return !out.aborted;
}

static const char* trialKindName(int k) {
    switch (k) {
        case TrialSpec::Base: return "база";
        case TrialSpec::Scan: return "прогон частот";
        case TrialSpec::SeedInh: return "старт: сток";
        case TrialSpec::SeedPreset: return "старт: пресет";
        case TrialSpec::Knob: return "ручка";
        case TrialSpec::Remeasure: return "перепроверка";
        case TrialSpec::Verify: return "длинная проверка";
        case TrialSpec::KipCheck: return "кип после загрузки";
        case TrialSpec::KipPre: return "кип: живьём до записи";
        case TrialSpec::Optimize: return "оптимизация";
        case TrialSpec::OptGuard: return "проверка итога оптимизации";
        case TrialSpec::Confirm: return "подтверждение А/Б";
        case TrialSpec::Ref: return "эталон заново";
        case TrialSpec::RefTune: return "финиш: tREFI × tRFC";
        case TrialSpec::RefVerify: return "финиш: длинная проверка";
    }
    return "?";
}

bool Tuner::chainKey(const std::string& key) const {
    const KnobDef* d = findKnob(key);
    return maxMode() && d && d->risky && (d->order == KnobOrder::Up || d->order == KnobOrder::Down);
}

bool Tuner::warmUp(ITarget& t, const TrialSpec& spec) {
    // Сразу после загрузки (или долгого простоя) память и SoC холодные, и первые минуты замеры ползут вниз: в 1.9.1
    // после перезагрузки эталон упал на 0,4 %, tWTR 7 и 8 вышли «хуже старта», и ручку исправила только оптимизация.
    // Поэтому — kWarmSec обычных замеров на рабочей частоте и уже проверенном наборе, в счёт не идут. Не memtest:
    // длинный memtest на значениях вплотную к пределу сам может повесить консоль, а зависание прогрева ни одной
    // пробе не засчитать — после перезагрузки прогрев повторился бы. Замер — та же нагрузка, что у любой пробы.
    KnobSet set;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if ((phase == Phase::Timing || phase == Phase::Optimize) && !cur.empty())
            set = cur;
        else if (!spec.ref.empty())
            set = spec.ref;
        else
            set = spec.set;
    }
    u64 t0 = nowMs();
    TLOG("▶ прогрев %d с на %s МГц перед первой пробой сеанса (не в счёт) | набор: %s", kWarmSec, mhzStr(spec.hz).c_str(),
         set.empty() ? "общие кипа" : knobSetShort(set).c_str());
    ApplyInfo ai;
    std::string err;
    if (!t.apply(spec.hz, set, &ai, &err)) {
        TLOG("  прогрев: не применилось (%s) — без прогрева", err.c_str());
        return true;
    }
    bool ab = false;
    BenchResult first, last;
    u64 u0 = t.uptimeMs();
    for (int n = 0; n < 40 && t.uptimeMs() - u0 < (u64)kWarmSec * 1000; n++) {
        last = t.bench(3, &ab);
        if (ab) return false;
        if (!first.valid) first = last;
    }
    TLOG("  прогрев: поток %.0f → %.0f МБ/с, задержка %.1f → %.1f нс | %.1f с", first.bw(), last.bw(), first.latNs, last.latNs,
         (nowMs() - t0) / 1000.0);
    std::lock_guard<std::recursive_mutex> lk(mtx);
    sessionWarm = true;
    ev("Прогрев %d с на %s МГц перед первой пробой сеанса: поток %.0f → %.0f МБ/с — эталон замеряю заново", kWarmSec, mhzStr(spec.hz).c_str(),
       first.bw(), last.bw());
    // Эталоны, замеренные до перерыва, — холодные или вчерашние: следующая проба — замер выбранного заново.
    if (phase == Phase::Timing) {
        needRef = true;
    } else if (phase == Phase::Optimize && optHaveBest && !confirmPending) {
        optVals.insert(optVals.begin() + std::min<size_t>(optPos, optVals.size()), optBestVal);
        optHaveBest = false;
    } else if (phase == Phase::Refresh && rfStage == 1 && !confirmPending && rfPick == rfStart) {
        rfStage = 0;
    }
    return true;
}

TrialOut Tuner::runTrial(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop) {
    if (!sessionWarm && !t.safeMode()) {
        // Первый замер после запуска занижен (прогрев кэшей, буферов, частоты ЦПУ) — разогрев, в расчёт не идёт.
        sessionWarm = true;
        bool ab = false;
        BenchResult w = t.bench(std::max(3, cfg.benchPasses), &ab);
        if (w.valid) TLOG("  разогревочный замер (не в счёт): поток %.0f МБ/с, задержка %.1f нс", w.bw(), w.latNs);
    }
    u64 t0 = nowMs();
    TLOG("▶ проба #%d [%s] %s | %s МГц | memtest %d с, замеров %d | набор: %s", trials, trialKindName(spec.kind), spec.desc.c_str(),
         mhzStr(spec.hz).c_str(), spec.stabSec, spec.passes, spec.set.empty() ? "общие кипа" : knobSetShort(spec.set).c_str());
    bool paired = spec.kind == TrialSpec::Confirm || (spec.kind == TrialSpec::OptGuard && !spec.ref.empty());
    TrialOut out = paired ? pairedConfirm(t, spec.hz, spec.ref, spec.set, stop, spec.thr) : runTrialCore(t, spec, stop);
    std::string sens = t.sensors();
    if (out.aborted)
        TLOG("  ⏸ прервана (пауза / потеря фокуса) за %.1f с%s%s", (nowMs() - t0) / 1000.0, sens.empty() ? "" : " | ", sens.c_str());
    else if (out.ok)
        TLOG("  ✔ стабильно: чтение %.0f, запись %.0f, копия %.0f МБ/с (поток %.0f), задержка %.1f / %.1f нс, оценка %.4f | %.1f с%s%s",
             out.b.readMBs, out.b.writeMBs, out.b.copyMBs, out.b.bw(), out.b.latNs, out.b.lat2Ns, out.score, (nowMs() - t0) / 1000.0,
             sens.empty() ? "" : " | ", sens.c_str());
    else
        TLOG("  ✘ СБОЙ: %s | %.1f с%s%s", out.reason.c_str(), (nowMs() - t0) / 1000.0, sens.empty() ? "" : " | ", sens.c_str());
    return out;
}

TrialOut Tuner::runTrialCore(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop) {
    TrialOut out;
    std::string err;
    if (t.safeMode()) {
        // После зависания демон в безопасном режиме: не ставит частоты и не применяет живые тайминги. Проба ни о чём
        // не говорит — не сбой, а пауза до перезагрузки через систему (на любой частоте, и на стоковой тоже).
        out.aborted = true;
        out.reason = "Ryazha-clk в безопасном режиме после зависания";
        safeHalt = true;
        TLOG("  ⏸ не засчитана: Ryazha-clk в безопасном режиме после зависания — частоты и тайминги не ставятся до перезагрузки");
        return out;
    }
    bool applied = t.apply(spec.hz, spec.set, &out.ai, &err);
    // Ошибка записи timings.ini на карту (I/O error) — сбой карты, а не памяти: повторяем, а не засчитываем ручке.
    for (int retry = 0; !applied && retry < 3 && err.find("timings.ini") != std::string::npos; retry++) {
        TLOG("  ошибка записи на карту (%s) — повтор %d из 3", err.c_str(), retry + 1);
        t.idle(1000);
        err.clear();
        applied = t.apply(spec.hz, spec.set, &out.ai, &err);
    }
    if (!applied) {
        out.reason = err.empty() ? "не удалось применить" : err;
        LOGE("Проба «%s»: применение не удалось: %s", spec.desc.c_str(), out.reason.c_str());
        if (err.find("timings.ini") != std::string::npos) {
            // Карта так и не записала файл: проба не о стабильности — пауза, повторится при продолжении.
            out.aborted = true;
            TLOG("  ⏸ не засчитана: timings.ini не записывается на карту — проверьте карту, проба повторится");
        }
        return out;
    }
    TLOG("  применено: реально %s МГц, демон подтвердил тайминги: %s", mhzStr(out.ai.realHz).c_str(), out.ai.confirmed ? "да" : "нет");
    for (auto& n : out.ai.notApplied) TLOG("  демон: %s", n.c_str());
    auto offFreq = [&](u32 real) {
        if (real == 0 || spec.hz == 0) return false;
        double d = std::fabs((double)real - (double)spec.hz) / (double)spec.hz;
        return d > 0.03;
    };
    if (offFreq(out.ai.realHz)) {
        out.reason = strf("PLL drop: реально %s МГц вместо %s", mhzStr(out.ai.realHz).c_str(), mhzStr(spec.hz).c_str());
        return out;
    }
    if (stop && stop->load()) {
        out.aborted = true;
        return out;
    }
    if (spec.stabSec > 0) {
        bool aborted = false;
        u64 m0 = nowMs();
        if (!t.stability(spec.stabSec, &err, &aborted)) {
            if (aborted) {
                out.aborted = true;
                return out;
            }
            out.reason = "memtest: " + err;
            TLOG("  memtest %d с: ОШИБКИ через %.1f с — %s", spec.stabSec, (nowMs() - m0) / 1000.0, err.c_str());
            return out;
        }
        TLOG("  memtest %d с: без ошибок", spec.stabSec);
    }
    bool aborted = false;
    out.b = t.bench(spec.passes, &aborted);
    if (aborted) {
        out.aborted = true;
        return out;
    }
    // Правдоподобие: шина 64 бит, DDR — не больше частоты × 16 байт в секунду (+15 % на погрешность таймера).
    // Больше — ошибка замера (так в 1.3–1.6 компилятор выбрасывал цикл чтения), а не память.
    for (int attempt = 0; out.b.valid && spec.hz && attempt < 3; attempt++) {
        double peak = spec.hz / 1e6 * 16.0 * 1.15;
        double worst = std::max(out.b.readMBs, std::max(out.b.writeMBs, out.b.copyMBs));
        if (worst <= peak) break;
        LOGE("Замер нереален: чтение %.0f, запись %.0f, копия %.0f МБ/с при потолке шины %.0f МБ/с на %s МГц — повторяю", out.b.readMBs,
             out.b.writeMBs, out.b.copyMBs, peak, mhzStr(spec.hz).c_str());
        TLOG("  ⚠ замер нереален (%.0f МБ/с > потолка шины %.0f) — повтор %d", worst, peak, attempt + 1);
        if (attempt == 2) {
            out.b.valid = false;
            break;
        }
        out.b = t.bench(spec.passes, &aborted);
        if (aborted) {
            out.aborted = true;
            return out;
        }
    }
    if (!out.b.valid) {
        out.reason = "замер не удался (ошибка замера, не память)";
        return out;
    }
    u32 real2 = t.realHz();
    if (offFreq(real2)) {
        out.reason = strf("PLL drop под нагрузкой: %s МГц", mhzStr(real2).c_str());
        return out;
    }
    out.score = scoreOf(out.b);
    out.ok = true;
    return out;
}

// ============================================================== выбор следующей пробы

TrialSpec Tuner::nextTrial() {
    for (int guard = 0; guard < 200; guard++) {
        TrialSpec s;
        switch (phase) {
            case Phase::Baseline: {
                if (baseRuns.size() >= 3) {
                    finishBaseline();
                    continue;
                }
                s.kind = TrialSpec::Base;
                s.hz = stockHz();
                s.stabSec = 0;
                s.passes = std::max(cfg.benchPasses, cfg.timingPasses);  // шум — при тех же условиях, что и пробы таймингов
                s.desc = strf("Базовый замер %zu/3 на %s МГц", baseRuns.size() + 1, mhzStr(s.hz).c_str());
                return s;
            }
            case Phase::FreqScan: {
                int idx = scanNextIdx();
                if (idx < 0) {
                    finishScan();
                    continue;
                }
                s.kind = TrialSpec::Scan;
                s.hz = ladder[idx];
                s.set = kfDone ? kipScanSet(s.hz) : (scanPass == 1 ? guideAt(s.hz) : ryazhaStockSet());
                s.stabSec = cfg.quickSec;
                s.passes = cfg.scanPasses;  // 5 замеров, медиана: точнее сравнение ступеней и пики потока
                s.desc = strf("Частота %s МГц (прогон %d — %s: ступень %d из %zu)", mhzStr(s.hz).c_str(), scanPass + 1,
                              scanPass == 1 ? "тайминги гайда" : "сток Ryazha-clk", scanPos + 1, coarse.size());
                return s;
            }
            case Phase::Seed: {
                if (guideChosen && seedStage == 0) seedStage = 1;  // на стоке эти частоты не держатся — сразу гайд
                if (tIdx >= (int)targets.size()) {
                    if (cfg.kipStage)
                        enterKip();
                    else
                        enterGame();
                    continue;
                }
                s.hz = targets[tIdx];
                s.stabSec = cfg.quickSec;
                s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                if (seedStage == 0) {
                    s.kind = TrialSpec::SeedInh;
                    s.set = kfDone ? kipScanSet(s.hz) : ryazhaStockSet();
                    s.desc = strf("%s МГц: сток Ryazha-clk", mhzStr(s.hz).c_str());
                } else {
                    s.kind = TrialSpec::SeedPreset;
                    s.set = presetAt(s.hz);
                    s.desc = strf("%s МГц: пресет чипа %s", mhzStr(s.hz).c_str(), knobSetShort(s.set).c_str());
                }
                return s;
            }
            case Phase::Timing: {
                u32 hz = targets[tIdx];
                if (remeasure) {
                    s.kind = TrialSpec::Remeasure;
                    s.hz = hz;
                    s.set = cur;
                    s.stabSec = cfg.quickSec;
                    s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                    s.desc = strf("%s МГц: перепроверка с запасом", mhzStr(hz).c_str());
                    return s;
                }
                if (kIdx >= (int)plan.size()) {
                    ev("%s МГц: подбор ручек закончен — %s", mhzStr(hz).c_str(), knobSetShort(cur).c_str());
                TLOG("Абсолютные пределы на %s МГц (последнее стабильное перед сбоем): %s; запас режима %d", mhzStr(hz).c_str(),
                     absLimits.empty() ? "—" : knobSetShort(absLimits).c_str(), cfg.margin);
                    verifyTries = 0;
                    if (cfg.optimize)
                        enterOptimize();
                    else
                        phase = Phase::Verify;
                    continue;
                }
                auto seq = buildSeq();
                if (chainAB && (chainEnd || seqPos >= (int)seq.size())) {
                    // Цепочка закончилась (сбой или конец ступеней): один замер А/Б — лучшая ступень против старта.
                    s.kind = TrialSpec::Confirm;
                    s.hz = hz;
                    s.key = plan[kIdx].key;
                    s.val = bestVal;
                    s.ref = cur;
                    s.set = withKnob(cur, s.key, bestVal);
                    s.stabSec = 0;
                    s.passes = kPairPasses;
                    s.thr = kMaxGain;
                    s.desc = strf("%s МГц: %s = %d — цепочка, подтверждение А/Б против старта", mhzStr(hz).c_str(), s.key.c_str(), bestVal);
                    return s;
                }
                if (seqPos >= (int)seq.size()) {
                    finishKnob();
                    continue;
                }
                if (needRef && !confirmPending) {
                    // Эталон заново, без memtest: набор уже проверен, нужна только его честная оценка сейчас.
                    s.kind = TrialSpec::Ref;
                    s.hz = hz;
                    s.key = plan[kIdx].key;
                    // Цепочка сравнивает каждую ступень со стартом — заново меряется старт, а не лучшая ступень.
                    s.set = haveBest && !chainKey(s.key) ? withKnob(cur, s.key, bestVal) : cur;
                    s.stabSec = 0;
                    s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                    s.desc = strf("%s МГц: %s — эталон заново (%s)", mhzStr(hz).c_str(), s.key.c_str(),
                                  haveBest ? strf("лучшее %d", bestVal).c_str() : "до первой пробы");
                    return s;
                }
                const KnobDef* d = findKnob(plan[kIdx].key);
                s.kind = TrialSpec::Knob;
                s.hz = hz;
                s.set = cur;
                s.key = plan[kIdx].key;
                s.val = seq[seqPos];
                if (d->absentIsZero && s.val == 0)
                    s.set.erase(s.key);
                else
                    s.set[s.key] = s.val;
                s.stabSec = cfg.quickSec;
                s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                s.desc = strf("%s МГц: %s = %d (%s)%s", mhzStr(hz).c_str(), s.key.c_str(), s.val, knobValueText(*d, s.val).c_str(),
                              confirmPending ? " — повтор спорного прироста" : "");
                if (confirmPending && maxMode()) {
                    // Похоже на прирост — эталон (лучшее значение или стартовое) против этого по очереди.
                    s.kind = TrialSpec::Confirm;
                    s.ref = haveBest ? withKnob(cur, s.key, bestVal) : cur;
                    s.stabSec = 0;
                    s.passes = kPairPasses;
                    s.thr = kMaxGain;
                    s.desc = strf("%s МГц: %s = %d — подтверждение А/Б против %s", mhzStr(hz).c_str(), s.key.c_str(), s.val,
                                  haveBest ? strf("%d", bestVal).c_str() : "старта");
                }
                return s;
            }
            case Phase::Kip: {
                if (kipStep == KipPre) {
                    // Живая проверка значений для кипа на каждой частоте кандидатов — до записи и перезагрузок.
                    while (kipPrePos < (int)kipPre.size() &&
                           (kipPre[kipPrePos].first >= (int)cands.size() || !kipVals.count(kipPre[kipPrePos].second)))
                        kipPrePos++;
                    if (kipPrePos >= (int)kipPre.size()) {
                        kipStep = KipWrite;
                        ev("Кип: живая проверка закончена — в кип пойдут: %s", kipVals.empty() ? "ничего" : knobSetCompact(kipVals).c_str());
                        return s;  // дальше запись — её ведёт kipAdvance()
                    }
                    const Candidate& c = cands[kipPre[kipPrePos].first];
                    const std::string& key = kipPre[kipPrePos].second;
                    s.kind = TrialSpec::KipPre;
                    s.hz = c.hz;
                    s.set = c.set;
                    s.set[key] = kipVals[key];
                    s.key = key;
                    s.val = kipVals[key];
                    s.stabSec = cfg.kipCheckSec;
                    s.passes = cfg.benchPasses;
                    s.desc = strf("Кип, живьём до записи: %s = %d на %s МГц (%d из %zu)", key.c_str(), s.val, mhzStr(c.hz).c_str(), kipPrePos + 1,
                                  kipPre.size());
                    return s;
                }
                if (kipStep != KipCheckRun) return s;  // запись и перезагрузку ведёт kipAdvance()
                auto ids = kipCheckCands();
                if (kipCheckIdx >= (int)ids.size()) {
                    kipGroupPassed();
                    return s;
                }
                const Candidate& c = cands[ids[kipCheckIdx]];
                std::vector<std::string> inKip = kipAccepted;
                inKip.insert(inKip.end(), kipTesting.begin(), kipTesting.end());
                s.kind = TrialSpec::KipCheck;
                s.hz = c.hz;
                s.set = kipResidual(c.set, inKip);
                s.stabSec = cfg.kipCheckSec;
                s.passes = cfg.benchPasses;
                s.desc = strf("Кип, загрузка %d: проверка %s МГц (%d из %zu)", kipReboots, mhzStr(c.hz).c_str(), kipCheckIdx + 1, ids.size());
                return s;
            }
            case Phase::Optimize: {
                u32 hz = targets[tIdx];
                if (optKey >= (int)optKeys.size()) {
                    if (optChanged && optPass == 0) {
                        optPass = 1;  // что-то сдвинулось — второй круг: соседи могли поменяться местами
                        optChanged = false;
                        optKey = 0;
                        optPrepareKey();
                        ev("%s МГц: оптимизация, второй круг", mhzStr(hz).c_str());
                        continue;
                    }
                    if (optGuard < 4 && !optStart.empty() && optStart != cur) {
                        // Итог против набора до оптимизации — замером А/Б (раньше — по две отдельные пробы, в пределах шума).
                        s.kind = TrialSpec::OptGuard;
                        s.hz = hz;
                        s.ref = optStart;
                        s.set = cur;
                        s.stabSec = 0;
                        s.passes = kPairPasses;
                        s.thr = guardThr();
                        s.desc = strf("%s МГц: проверка итога оптимизации — А/Б против набора до неё", mhzStr(hz).c_str());
                        return s;
                    }
                    ev("%s МГц: оптимизация закончена — %s", mhzStr(hz).c_str(), knobSetShort(cur).c_str());
                    phase = Phase::Verify;
                    continue;
                }
                if (optPos >= (int)optVals.size()) {
                    optKey++;
                    optPrepareKey();
                    continue;
                }
                const std::string& key = optKeys[optKey];
                s.kind = TrialSpec::Optimize;
                s.hz = hz;
                s.set = cur;
                s.set[key] = optVals[optPos];
                s.key = key;
                s.val = optVals[optPos];
                s.stabSec = cfg.quickSec;
                s.passes = std::max(cfg.benchPasses + 1, cfg.timingPasses);
                s.desc = strf("%s МГц: оптимизация %s = %d (%d из %zu, круг %d)", mhzStr(hz).c_str(), key.c_str(), s.val, optPos + 1, optVals.size(),
                              optPass + 1);
                if (confirmPending && optHaveBest) {
                    s.kind = TrialSpec::Confirm;
                    s.ref = cur;
                    s.ref[key] = optBestVal;
                    s.stabSec = 0;
                    s.passes = kPairPasses;
                    s.thr = optThr(s.val < optBestVal);
                    s.desc = strf("%s МГц: оптимизация %s = %d — подтверждение А/Б против %d", mhzStr(hz).c_str(), key.c_str(), s.val, optBestVal);
                }
                return s;
            }
            case Phase::Refresh: {
                if (rfQueue.empty() || rfQueue[0] >= (int)cands.size()) {
                    if (!rfQueue.empty()) {
                        rfNext();
                        continue;
                    }
                    rfDone = true;
                    enterGame();
                    continue;
                }
                const Candidate& c = cands[rfQueue[0]];
                s.hz = c.hz;
                s.key = "tREFI";
                if (rfStage == 0) {
                    // Эталон — набор кандидата как есть, без memtest: он уже проверен на этом кипе.
                    s.kind = TrialSpec::RefTune;
                    s.set = c.set;
                    s.val = pairCode(pairIn(c.set));
                    s.stabSec = 0;
                    s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                    s.desc = strf("Финиш %s: эталон (%s)", c.name().c_str(), pairStr(pairIn(c.set)).c_str());
                    return s;
                }
                if (rfStage == 1) {
                    // Пары — от меньшего сдвига к большему; дальше выбранной по доле регенерации и туже сбойной — мимо.
                    while (rfPos < (int)rfPairs.size() && !confirmPending && rfSkip(rfPairs[rfPos])) rfPos++;
                    if (rfPos >= (int)rfPairs.size()) {
                        if (rfPick != rfStart) {
                            rfStage = 2;
                        } else {
                            ev("Финиш %s: пара не поменялась — %s", c.name().c_str(), pairStr(rfStart).c_str());
                            rfNext();
                        }
                        continue;
                    }
                    RfPair p = rfPairs[rfPos];
                    s.val = pairCode(p);
                    s.set = withPair(c.set, p);
                    if (confirmPending) {
                        s.kind = TrialSpec::Confirm;
                        s.ref = withPair(c.set, rfPick);
                        s.stabSec = 0;
                        s.passes = kPairPasses;
                        s.thr = rfThr();
                        s.desc = strf("Финиш %s: %s — подтверждение А/Б против %s", c.name().c_str(), pairStr(p).c_str(), pairStr(rfPick).c_str());
                    } else {
                        s.kind = TrialSpec::RefTune;
                        s.stabSec = cfg.quickSec;
                        s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
                        s.desc = strf("Финиш %s: %s (%d из %zu)", c.name().c_str(), pairStr(p).c_str(), rfPos + 1, rfPairs.size());
                    }
                    return s;
                }
                // Выбранная пара — длинной проверкой: длинный tREFI теряет данные прежде всего на прогретой памяти,
                // memtest сам её греет.
                s.kind = TrialSpec::RefVerify;
                s.set = withPair(c.set, rfPick);
                s.val = pairCode(rfPick);
                s.stabSec = std::max(cfg.verifySec, 120);
                s.passes = cfg.benchPasses + 1;
                s.desc = strf("Финиш %s: длинная проверка %s, %d с", c.name().c_str(), pairStr(rfPick).c_str(), s.stabSec);
                return s;
            }
            case Phase::Verify: {
                s.kind = TrialSpec::Verify;
                s.hz = targets[tIdx];
                s.set = cur;
                s.stabSec = cfg.verifySec;
                s.passes = cfg.benchPasses + 1;
                s.desc = strf("%s МГц: длинная проверка %d с", mhzStr(s.hz).c_str(), cfg.verifySec);
                return s;
            }
            default:
                return s;
        }
    }
    phase = Phase::Failed;
    error = "внутренняя ошибка: зацикливание выбора пробы";
    return TrialSpec();
}

// ============================================================== обработка результата

void Tuner::handleResult(const TrialSpec& spec, const TrialOut& out) {
    if (!out.ai.notApplied.empty())
        for (auto& n : out.ai.notApplied) ev("Демон: %s", n.c_str());
    if (!out.ok && !out.aborted && spec.kind != TrialSpec::Base && out.reason.find("замер не удался") == std::string::npos) {
        // Подтверждение А/Б: сбой эталона — про набор целиком, сбой кандидата — как у пробы ручки.
        bool keyed = spec.kind == TrialSpec::Knob || (spec.kind == TrialSpec::Confirm && !out.refFailed && phase == Phase::Timing);
        noteUnstable(spec.hz, failKind(out.reason), keyed ? spec.key : std::string(), keyed ? spec.val : 0, out.refFailed ? spec.ref : spec.set,
                     out.reason);
    }
    // Шум прохода — по пробам на рабочих частотах (у подтверждения А/Б — по половинам блоков, сводка Б их повторяет).
    // База (1600 МГц) и прогон частот шумят в 3–4 раза сильнее (1.8.7: 0,65 % и 0,46 % против 0,17 %) — не в счёт.
    if (out.ok && spec.kind != TrialSpec::Confirm && spec.kind != TrialSpec::Base && spec.kind != TrialSpec::Scan) addNoise(out.b);
    for (auto& b : out.extra) addNoise(b);

    switch (spec.kind) {
        case TrialSpec::Base: {
            if (!out.ok) {
                phase = Phase::Failed;
                error = "базовый замер на стоковой частоте не удался: " + out.reason;
                ev("%s", error.c_str());
                return;
            }
            baseRuns.push_back(out.b);
            ev("Базовый замер: поток %.0f МБ/с, задержка %.1f нс", out.b.bw(), out.b.latNs);
            return;
        }
        case TrialSpec::Scan: {
            int idx = -1;
            for (size_t i = 0; i < ladder.size(); i++)
                if (ladder[i] == spec.hz) idx = (int)i;
            FreqPoint fp;
            fp.hz = spec.hz;
            fp.b = out.b;
            fp.score = out.score;
            bool ok = out.ok;
            std::string reason = out.reason;
            bool slower = false;
            if (ok && lastOk >= 0 && out.b.bw() < lastOkBw * (1.0 - std::max(0.03, eps * 2))) {
                // Не сбой: прогон идёт дальше, но в цели такая ступень не попадёт.
                slower = true;
                reason = strf("медленнее предыдущей стабильной ступени (%.0f < %.0f МБ/с) — похоже на PLL drop", out.b.bw(), lastOkBw);
            }
            fp.status = slower ? 3 : (ok ? 1 : 2);
            fp.reason = reason;
            fp.pass = scanPass;
            histTrial(spec, out, kfDone ? "scan:kip" : (scanPass == 1 ? "scan:guide" : "scan:stock"));
            points.erase(std::remove_if(points.begin(), points.end(), [&](const FreqPoint& p) { return p.hz == spec.hz && p.pass == fp.pass; }),
                         points.end());
            points.push_back(fp);
            std::sort(points.begin(), points.end(), [](const FreqPoint& a, const FreqPoint& b) { return a.hz != b.hz ? a.hz < b.hz : a.pass < b.pass; });
            scanPos++;
            if (slower) {
                failRun = 0;
                hangRun = 0;
                ev("%s МГц — %s", mhzStr(spec.hz).c_str(), reason.c_str());
            } else if (ok) {
                ev("%s МГц — стабильно: поток %.0f МБ/с, задержка %.1f нс, оценка %.3f", mhzStr(spec.hz).c_str(), out.b.bw(), out.b.latNs,
                   out.score);
                failRun = 0;
                hangRun = 0;
                if (idx > lastOk) lastOk = idx;
                lastOkBw = out.b.bw();
            } else {
                failRun++;
                if (failKind(reason) == "зависание") hangRun++;
                ev("%s МГц — сбой: %s", mhzStr(spec.hz).c_str(), reason.c_str());
                if (cfg.scanHangStop > 0 && hangRun >= cfg.scanHangStop && scanPos < (int)coarse.size()) {
                    ev("%d зависания подряд (последнее — %s МГц): выше память физически не берёт — прогон %s закончен, переходим дальше",
                       hangRun, mhzStr(spec.hz).c_str(), scanPass == 1 ? "на таймингах гайда" : "на стоке");
                    scanPos = (int)coarse.size();
                    // Крайняя стабильная частота этого прогона — потолок для всего дальше. Но если прогон на наборе гайда,
                    // а сток Ryazha-clk эту частоту уже прошёл, виноват набор таймингов, а не частота: потолка нет.
                    u32 cap = 0;
                    bool stockHeld = false;
                    for (auto& p : points) {
                        if (p.pass == scanPass && p.status == 1 && p.hz < spec.hz && p.hz > cap) cap = p.hz;
                        if (scanPass == 1 && p.pass == 0 && p.status == 1 && p.hz >= spec.hz) stockHeld = true;
                    }
                    if (stockHeld) {
                        cap = 0;
                        ev("Сток Ryazha-clk эти частоты держит — зависает набор таймингов гайда, а не частота: потолок частоте не ставим");
                    }
                    if (cap && (!freqCap || cap < freqCap)) {
                        freqCap = cap;
                        ev("Потолок частоты: %s МГц — дальше все проверки не выше неё", mhzStr(freqCap).c_str());
                    }
                }
                // Сбой не останавливает прогон: следующая ступень проверяется с нуля. Остановка — только по настройке.
                if (cfg.scanFailStop > 0 && failRun >= cfg.scanFailStop && scanPos < (int)coarse.size()) {
                    ev("%d сбоев подряд — прогон остановлен на %s МГц (настройка «Сбоев подряд до остановки»)", failRun,
                       mhzStr(spec.hz).c_str());
                    scanPos = (int)coarse.size();
                }
            }
            return;
        }
        case TrialSpec::SeedInh: {
            if (!out.ok) {
                if (!presetSet.empty()) {
                    ev("%s МГц не держится на стоке Ryazha-clk (%s) — пробую тайминги гайда", mhzStr(spec.hz).c_str(), out.reason.c_str());
                    inhScore = 0;
                    seedStage = 1;
                    return;
                }
                ev("%s МГц больше не держится на стоке Ryazha-clk (%s) — цель пропущена", mhzStr(spec.hz).c_str(), out.reason.c_str());
                nextTarget();
                return;
            }
            inhScore = out.score;
            if (!presetSet.empty()) {
                seedStage = 1;
            } else if (kfDone) {
                // Старт — с обученных таймингов кипа: явными значениями, чтобы ужесточать от них.
                KnobSet st = kipScanSet(spec.hz);
                for (auto& kv : kfVals) st[kv.first] = kv.second;
                startTiming(st, inhScore);
            } else {
                startTiming(ryazhaStockSet(), inhScore);
            }
            return;
        }
        case TrialSpec::SeedPreset: {
            if (out.ok && out.score >= inhScore * (1.0 - eps)) {
                ev("Пресет чипа держится (оценка %.3f против %.3f у общих) — стартуем с него", out.score, inhScore);
                startTiming(presetAt(spec.hz), std::max(out.score, 0.0));
            } else {
                if (!out.ok && (guideChosen || inhScore <= 0)) {
                    ev("%s МГц больше не держится и на таймингах гайда (%s) — цель пропущена", mhzStr(spec.hz).c_str(), out.reason.c_str());
                    nextTarget();
                    return;
                }
                if (out.ok)
                    ev("Сток Ryazha-clk быстрее пресета (%.3f > %.3f) — стартуем с него", inhScore, out.score);
                else
                    ev("Пресет чипа не прошёл (%s) — стартуем со стока Ryazha-clk", out.reason.c_str());
                startTiming(ryazhaStockSet(), inhScore);
            }
            return;
        }
        case TrialSpec::Optimize: {
            const std::string& key = spec.key;
            if (out.ok) {
                double bw = out.b.bw();
                ev("Оптимизация %s = %d: поток %.0f МБ/с, задержка %.1f нс, оценка %.3f", key.c_str(), spec.val, bw, out.b.latNs, out.score);
                if (!optHaveBest) {
                    // Первое стабильное — обычно действующее значение, оно идёт первым — эталон перебора.
                    optHaveBest = true;
                    optBestVal = spec.val;
                    optBestScore = out.score;
                    optBestBw = bw;
                } else {
                    // Остальные сравниваются с выбранным: явно не дотягивают до порога — мимо, иначе решает замер А/Б.
                    bool softer = spec.val < optBestVal;
                    double thr = optThr(softer), dlt = out.score / optBestScore - 1, sd = std::sqrt(2.0) * trialSigma(spec.passes);
                    if (dlt >= thr - kScreenK * sd) {
                        confirmPending = true;
                        ev("Оптимизация %s = %d: %+.2f %% к %d (порог %+.2f %%) — подтверждаю замером А/Б", key.c_str(), spec.val, dlt * 100,
                           optBestVal, thr * 100);
                        return;  // optPos не двигаем: следующая проба — А/Б этого значения
                    }
                }
            } else {
                ev("Оптимизация %s = %d: нестабильно (%s)", key.c_str(), spec.val, out.reason.c_str());
            }
            optPos++;
            if (optPos >= (int)optVals.size()) optFinishKey();
            return;
        }
        case TrialSpec::Confirm: {
            confirmPending = false;
            if (phase == Phase::Refresh) {
                RfPair p = pairOf(spec.val);
                if (!out.ok) {
                    if (!out.refFailed) rfBad.push_back(p);
                    ev("Финиш: %s — подтверждение А/Б не прошло (%s)", pairStr(p).c_str(), out.reason.c_str());
                    rfPos++;
                    return;
                }
                bool acc = out.delta >= rfThr();
                ev("Финиш: %s против %s — А/Б, блоков %d: %+.3f %% ± %.3f %% — %s", pairStr(p).c_str(), pairStr(rfPick).c_str(), out.blocks,
                   out.delta * 100, out.sigma * 100, acc ? "лучше" : "не лучше");
                if (acc) {
                    rfStack.push_back(rfPick);
                    rfPick = p;
                    rfPickScore = out.score;
                } else {
                    rfPickScore = out.refScore;
                }
                rfPos++;
                return;
            }
            bool inOpt = phase == Phase::Optimize;
            if (!inOpt && phase != Phase::Timing) return;
            if (!inOpt && chainAB) {
                // Конец цепочки рискованного тайминга: лучшая по отбору ступень против старта.
                chainAB = chainEnd = false;
                if (!out.ok) {
                    ev("%s = %d: цепочка — подтверждение А/Б не прошло (%s), остаётся старт", spec.key.c_str(), spec.val, out.reason.c_str());
                    haveBest = false;
                    if (!out.refFailed) {
                        if (!limits.count(spec.key)) limits[spec.key] = spec.val;
                        haveFail = true;
                        failVal = spec.val;
                    }
                    finishKnob();
                    return;
                }
                bool acc = out.delta >= kMaxGain;
                ev("%s: цепочка, %d против старта — А/Б, блоков %d: %+.3f %% ± %.3f %% (шум %.2f %%, порог %.2f %%) — %s", spec.key.c_str(), spec.val,
                   out.blocks, out.delta * 100, out.sigma * 100, 2.5 * out.sigma * 100, kMaxGain * 100, acc ? "лучше" : "остаётся старт");
                if (acc) {
                    bestScore = out.score;
                } else {
                    haveBest = false;
                    startScore = curScore = out.refScore;
                }
                finishKnob();
                return;
            }
            if (!out.ok) {
                if (inOpt) {
                    ev("Оптимизация %s = %d: подтверждение А/Б не прошло (%s) — остаётся %d", spec.key.c_str(), spec.val, out.reason.c_str(),
                       optBestVal);
                    optPos++;
                    if (optPos >= (int)optVals.size()) optFinishKey();
                } else if (out.refFailed) {
                    ev("%s = %d: подтверждение А/Б — сбой эталона (%s), значение не принято", spec.key.c_str(), spec.val, out.reason.c_str());
                    noimp++;
                    knobNext();
                } else {
                    // Как сбой пробы ручки: предел найден.
                    if (!limits.count(spec.key)) limits[spec.key] = spec.val;
                    haveFail = true;
                    failVal = spec.val;
                    ev("%s = %d — сбой при подтверждении А/Б: %s", spec.key.c_str(), spec.val, out.reason.c_str());
                    finishKnob();
                }
                return;
            }
            double thr = inOpt ? spec.thr : kMaxGain;
            bool acc = out.delta >= thr;
            std::string res = strf("А/Б, блоков %d: %+.3f %% ± %.3f %% (шум %.2f %%, порог %+.2f %%)", out.blocks, out.delta * 100, out.sigma * 100,
                                   2.5 * out.sigma * 100, thr * 100);
            if (inOpt) {
                ev("Оптимизация %s = %d против %d: %s — %s", spec.key.c_str(), spec.val, optBestVal, res.c_str(),
                   acc ? (out.delta >= 0 ? "лучше" : "не хуже, мягче — берём") : strf("остаётся %d", optBestVal).c_str());
                if (acc) {
                    optLoss = std::min(0.0, optLoss + out.delta);
                    optBestVal = spec.val;
                    optBestScore = out.score;
                    optBestBw = out.b.bw();
                } else {
                    optBestScore = out.refScore;  // эталон только что замерен заново — сравнение дальше с ним
                }
                optPos++;
                if (optPos >= (int)optVals.size()) optFinishKey();
                return;
            }
            ev("%s = %d: %s — %s", spec.key.c_str(), spec.val, res.c_str(), acc ? "лучше" : "без прироста");
            if (acc) {
                haveBest = true;
                bestVal = spec.val;
                bestScore = out.score;
                noimp = 0;
            } else {
                (haveBest ? bestScore : startScore) = out.refScore;  // эталон только что замерен заново
                noimp++;
            }
            knobNext();
            return;
        }
        case TrialSpec::RefTune: {
            if (phase != Phase::Refresh || rfQueue.empty()) return;
            RfPair p = pairOf(spec.val);
            if (rfStage == 0) {
                const Candidate& c = cands[rfQueue[0]];
                if (!out.ok) {
                    ev("Финиш %s: эталон не прошёл (%s) — кандидат остаётся как был", c.name().c_str(), out.reason.c_str());
                    rfNext();
                    return;
                }
                rfStart = rfPick = p;
                rfPickScore = out.score;
                rfPairs = refreshPairs(c.hz, rfStart);
                rfPos = 0;
                rfStage = 1;
                ev("Финиш %s: эталон %s — оценка %.3f; пар на пробу %zu", c.name().c_str(), pairStr(p).c_str(), out.score, rfPairs.size());
                return;
            }
            if (!out.ok) {
                rfBad.push_back(p);
                ev("Финиш: %s — сбой (%s)", pairStr(p).c_str(), out.reason.c_str());
                rfPos++;
                return;
            }
            double dlt = out.score / rfPickScore - 1, sd = std::sqrt(2.0) * trialSigma(spec.passes);
            if (dlt >= rfThr() - kScreenK * sd) {
                confirmPending = true;
                ev("Финиш: %s — %+.2f %% к %s, подтверждаю замером А/Б", pairStr(p).c_str(), dlt * 100, pairStr(rfPick).c_str());
                return;  // rfPos не двигаем: следующая проба — А/Б этой пары
            }
            ev("Финиш: %s — %+.2f %% к %s, без прироста", pairStr(p).c_str(), dlt * 100, pairStr(rfPick).c_str());
            rfPos++;
            return;
        }
        case TrialSpec::RefVerify: {
            if (phase != Phase::Refresh || rfQueue.empty()) return;
            Candidate& c = cands[rfQueue[0]];
            if (!out.ok) {
                rfBad.push_back(rfPick);
                RfPair was = rfPick;
                rfPick = rfStack.empty() ? rfStart : rfStack.back();
                if (!rfStack.empty()) rfStack.pop_back();
                ev("Финиш %s: %s не прошла длинную проверку (%s) — назад к %s", c.name().c_str(), pairStr(was).c_str(), out.reason.c_str(),
                   pairStr(rfPick).c_str());
                if (rfPick == rfStart) rfNext();  // исходная пара уже проверена на этом кипе
                return;
            }
            std::string ch = strf("tREFI %d→%d, tRFC %d→%d", rfStart.first, rfPick.first, rfStart.second, rfPick.second);
            c.set = withPair(c.set, rfPick);
            c.b = out.b;
            c.synth = out.score;
            c.note += (c.note.empty() ? "финиш: " : "; финиш: ") + ch;
            ev("Финиш %s: %s — прошла длинную проверку, поток %.0f МБ/с, оценка %.3f", c.name().c_str(), ch.c_str(), out.b.bw(), out.score);
            rfNext();
            return;
        }
        case TrialSpec::Ref: {
            needRef = false;
            if (!out.ok) {
                ev("%s: эталон заново не прошёл (%s) — перепроверка набора", spec.key.c_str(), out.reason.c_str());
                remeasure = true;
                return;
            }
            bool chain = chainKey(spec.key);
            double was = haveBest && !chain ? bestScore : startScore;
            if (haveBest && !chain) {
                bestScore = out.score;
            } else {
                // Лучшая ступень цепочки замерена до перерыва — сдвигаем её оценку так же, как сдвинулся старт.
                if (chain && haveBest && startScore > 0) bestScore *= out.score / startScore;
                startScore = out.score;
                curScore = out.score;
            }
            ev("%s: эталон заново %.3f (был %.3f, %+.2f %%)", spec.key.c_str(), out.score, was, was > 0 ? (out.score / was - 1) * 100 : 0.0);
            return;
        }
        case TrialSpec::OptGuard: {
            if (!spec.ref.empty()) {
                // А/Б: итог (Б) против набора до оптимизации (А).
                optGuard = 4;
                if (!out.ok) {
                    if (out.refFailed) {
                        ev("Набор до оптимизации нестабилен (%s) — остаётся итог оптимизации", out.reason.c_str());
                    } else {
                        cur = optStart;
                        ev("Итог оптимизации нестабилен (%s) — возвращён набор до оптимизации: %s", out.reason.c_str(), knobSetShort(cur).c_str());
                    }
                } else if (out.delta < spec.thr) {
                    ev("Итог оптимизации хуже набора до неё: А/Б, блоков %d: %+.3f %% ± %.3f %% (порог %+.2f %%) — возвращён прежний: %s", out.blocks,
                       out.delta * 100, out.sigma * 100, spec.thr * 100, knobSetShort(optStart).c_str());
                    cur = optStart;
                    curScore = out.refScore;
                } else {
                    ev("Итог оптимизации не хуже набора до неё: А/Б, блоков %d: %+.3f %% ± %.3f %% (порог %+.2f %%) — оставлен", out.blocks,
                       out.delta * 100, out.sigma * 100, spec.thr * 100);
                    curScore = out.score;
                }
                optStart = cur;
                return;
            }
            // Журнал до 1.8.8: защита по отдельным пробам «после / до».
            bool after = optGuard % 2 == 0;
            if (!out.ok) {
                if (after) {
                    cur = optStart;
                    ev("Итог оптимизации нестабилен (%s) — возвращён набор до оптимизации: %s", out.reason.c_str(), knobSetShort(cur).c_str());
                } else {
                    ev("Набор до оптимизации нестабилен (%s) — остаётся итог оптимизации", out.reason.c_str());
                }
                optGuard = 4;
                optStart = cur;
                return;
            }
            (after ? optGuardA : optGuardB) += out.score;
            optGuard++;
            if (optGuard >= 4) {
                double a = optGuardA / 2, b = optGuardB / 2;
                if (a < b * (1.0 - eps * 0.5)) {
                    ev("Итог оптимизации хуже набора до неё: оценка %.3f против %.3f (%+.2f %%) — возвращён прежний: %s", a, b, (a / b - 1) * 100,
                       knobSetShort(optStart).c_str());
                    cur = optStart;
                    curScore = b;
                } else {
                    ev("Итог оптимизации не хуже набора до неё: оценка %.3f против %.3f (%+.2f %%) — оставлен", a, b, (a / b - 1) * 100);
                    curScore = a;
                }
                optStart = cur;
            }
            return;
        }
        case TrialSpec::Knob: {
            if (!out.ok) {
                confirmPending = false;
                if (!limits.count(spec.key)) limits[spec.key] = spec.val;
                haveFail = true;
                failVal = spec.val;
                ev("%s = %d — сбой: %s", spec.key.c_str(), spec.val, out.reason.c_str());
                if (chainAB)
                    chainEnd = true;  // сначала — замер А/Б лучшей ступени цепочки против старта
                else
                    finishKnob();
                return;
            }
            std::string note;
            if (!out.ai.notApplied.empty()) note = " (демон: не дошло до регистров)";
            if (maxMode()) {
                if (chainKey(spec.key)) {
                    // Цепочка рискованного тайминга идёт до сбоя: каждая ступень — против старта (он замерен заново), лучшая
                    // по отбору запоминается, а замер А/Б — один, в конце: она против старта (раньше — на каждую ступень,
                    // в 1.8.7 это две трети времени подбора).
                    double dlt = out.score / startScore - 1, sd = std::sqrt(2.0) * trialSigma(spec.passes);
                    if (dlt >= kMaxGain - kScreenK * sd && (!haveBest || out.score > bestScore)) {
                        haveBest = true;
                        bestVal = spec.val;
                        bestScore = out.score;
                        chainAB = true;
                        ev("%s = %d: оценка %.3f (%+.2f %% к старту) — лучшая в цепочке, замер А/Б — в конце%s", spec.key.c_str(), spec.val,
                           out.score, dlt * 100, note.c_str());
                    } else {
                        ev("%s = %d: оценка %.3f (%+.2f %% к старту)%s", spec.key.c_str(), spec.val, out.score, dlt * 100, note.c_str());
                    }
                    knobNext();
                    return;
                }
                // «Максимум»: проба — отбор. Явно ниже 0,12 % − 1,5σ — без прироста, иначе решает замер А/Б.
                double ref = haveBest ? bestScore : startScore;
                double dlt = out.score / ref - 1, sd = std::sqrt(2.0) * trialSigma(spec.passes);
                if (dlt >= kMaxGain - kScreenK * sd) {
                    confirmPending = true;
                    confirmScore = out.score;
                    ev("%s = %d: оценка %.3f (%+.2f %%) — похоже на прирост, подтверждаю замером А/Б%s", spec.key.c_str(), spec.val, out.score,
                       dlt * 100, note.c_str());
                    return;  // seqPos не двигаем: следующая проба — А/Б этого значения
                }
                if (out.score >= startScore * (1.0 - eps)) noimp++;
                ev("%s = %d: оценка %.3f (%+.2f %%) — без прироста%s", spec.key.c_str(), spec.val, out.score, dlt * 100, note.c_str());
                knobNext();
                return;
            }
            double ref = std::max(startScore, haveBest ? bestScore : 0.0);
            double score = out.score, thr = eps;
            if (confirmPending) {
                // Повтор спорной пробы: решаем по среднему двух замеров — у среднего шум в √2 раз меньше, порог тоже.
                score = (confirmScore + out.score) / 2;
                thr = eps / std::sqrt(2.0);
                confirmPending = false;
                ev("%s = %d: повтор %.3f, среднее двух %.3f (%+.2f %% при пороге %.2f %%)", spec.key.c_str(), spec.val, out.score, score,
                   (score / ref - 1) * 100, thr * 100);
            } else if (score > ref * (1.0 + eps * 0.5) && score <= ref * (1.0 + eps)) {
                // Прирост между половиной порога и порогом: может быть и шумом, и настоящим — та же проба ещё раз.
                confirmPending = true;
                confirmScore = score;
                ev("%s = %d: оценка %.3f (%+.2f %%) — спорный прирост, замеряю ещё раз%s", spec.key.c_str(), spec.val, score, (score / ref - 1) * 100,
                   note.c_str());
                return;  // seqPos не двигаем: следующая проба — то же значение
            }
            if (score > ref * (1.0 + thr)) {
                haveBest = true;
                bestVal = spec.val;
                bestScore = score;
                noimp = 0;
                needRef = true;  // принятый замер отобран как лучший — он завышен, следующий эталон меряем заново
                ev("%s = %d: оценка %.3f (%+.2f %%) — лучше%s", spec.key.c_str(), spec.val, score, (score / ref - 1) * 100, note.c_str());
            } else {
                if (score >= startScore * (1.0 - eps)) noimp++;
                ev("%s = %d: оценка %.3f (%+.2f %%) — без прироста%s", spec.key.c_str(), spec.val, score, (score / ref - 1) * 100, note.c_str());
            }
            knobNext();
            return;
        }
        case TrialSpec::Remeasure: {
            if (out.ok) {
                curScore = out.score;
                startScore = curScore;
                remeasure = false;
                remeasureFails = 0;
                return;
            }
            remeasureFails++;
            ev("Перепроверка не прошла (%s)", out.reason.c_str());
            if (remeasureFails > 3 || accepted.empty()) {
                ev("%s МГц: цель пропущена — не удаётся вернуть стабильность", mhzStr(spec.hz).c_str());
                remeasure = false;
                nextTarget();
                return;
            }
            // Ступень мягче (от пика — к последнему рабочему), а не сразу к стартовому значению.
            noteUnstable(spec.hz, failKind(out.reason), accepted.back().key, cur.count(accepted.back().key) ? cur[accepted.back().key] : 0, cur,
                         "перепроверка: " + out.reason);
            if (!loosenOne(accepted.size() - 1)) {
                auto last = accepted.back();
                accepted.pop_back();
                setValue(cur, last.key, last.prev);
                ev("Откат %s к %s", last.key.c_str(), last.prev == "inh" ? "общему" : last.prev.c_str());
            }
            remeasure = true;
            return;
        }
        case TrialSpec::Verify: {
            histTrial(spec, out, "tuned");
            if (out.ok) {
                addCandidate(spec.hz, cur, out.b, out.score);
                ev("%s МГц прошла длинную проверку: поток %.0f МБ/с, задержка %.1f нс, оценка %.3f", mhzStr(spec.hz).c_str(), out.b.bw(),
                   out.b.latNs, out.score);
                nextTarget();
                return;
            }
            verifyTries++;
            ev("Длинная проверка %s МГц не прошла (%s), попытка %d", mhzStr(spec.hz).c_str(), out.reason.c_str(), verifyTries);
            if (verifyTries > 16) {
                ev("%s МГц пропущена — нестабильна даже после ослабления", mhzStr(spec.hz).c_str());
                nextTarget();
                return;
            }
            if (!loosenForVerify()) {
                if (!cur.empty()) {
                    cur.clear();
                    ev("Сброс к общим таймингам кипа");
                } else {
                    ev("%s МГц пропущена — нестабильна на общих таймингах", mhzStr(spec.hz).c_str());
                    nextTarget();
                }
            }
            return;
        }
        case TrialSpec::KipPre: {
            kipPrePos++;
            if (!out.ok) {
                if (failKind(out.reason) == "зависание") kipHangs++;
                std::string why = strf("живьём на %s МГц: %s — значение кипа действует на все частоты, в кип не идёт", mhzStr(spec.hz).c_str(),
                                       out.reason.c_str());
                kipRejected.push_back({spec.key, spec.val, why});
                kipVals.erase(spec.key);
                for (auto& g : kipQueue) g.erase(std::remove(g.begin(), g.end(), spec.key), g.end());
                kipLog.push_back("отклонено до записи: " + spec.key + strf(" = %d — ", spec.val) + why);
                ev("Кип: %s = %d отклонено до записи — %s", spec.key.c_str(), spec.val, why.c_str());
            } else {
                ev("Кип: %s = %d держится живьём на %s МГц", spec.key.c_str(), spec.val, mhzStr(spec.hz).c_str());
            }
            return;
        }
        case TrialSpec::KipCheck: {
            if (!out.ok && failKind(out.reason) == "зависание") {
                // Зависание после загрузки с новым кипом — виноват кип: тайминги не отжимаем (каждая попытка
                // повесила бы консоль снова), значение откатывается сразу.
                kipHangs++;
                kipFixCand = -1;
                if (kipHangs >= kipHangLimit()) {
                    kipAbort(strf("%d зависания за этап", kipHangs));
                    return;
                }
                kipGroupFailed(strf("%s МГц: зависание с этим кипом", mhzStr(spec.hz).c_str()));
                return;
            }
            if (!out.ok) {
                // Нестабильно: сначала отжимаем тайминги кандидата (живьём, без перезагрузки), и только когда их не
                // осталось — считаем виновными ручки в кипе.
                auto ids = kipCheckCands();
                std::string d;
                if (kipCheckIdx < (int)ids.size() && kipFixTries < 16) {
                    Candidate& c = cands[ids[kipCheckIdx]];
                    if (kipFixCand != ids[kipCheckIdx]) {
                        kipFixCand = ids[kipCheckIdx];
                        kipFixBackup = c.set;
                    }
                    if (loosenTimingIn(c.set, c.loosened, &d)) {
                        c.loosened++;
                        kipFixTries++;
                        std::string l = strf("загрузка %d, %s МГц нестабильна (%s) — сначала отжимаю тайминги: %s", kipReboots, mhzStr(spec.hz).c_str(),
                                             out.reason.c_str(), d.c_str());
                        kipLog.push_back(l);
                        ev("Кип: %s", l.c_str());
                        return;
                    }
                }
                if (kipFixCand >= 0 && kipFixCand < (int)cands.size()) {
                    // Тайминги отжаты до конца, а нестабильно по-прежнему — дело в ручках кипа. Тайминги возвращаем.
                    cands[kipFixCand].set = kipFixBackup;
                    std::string l = strf("%s МГц: тайминги не помогли — возвращаю их как были", mhzStr(spec.hz).c_str());
                    kipLog.push_back(l);
                    ev("Кип: %s", l.c_str());
                }
                kipFixCand = -1;
                kipGroupFailed(strf("%s МГц: %s (тайминги отжаты до конца)", mhzStr(spec.hz).c_str(), out.reason.c_str()));
                return;
            }
            double live = 0;
            for (auto& c : cands)
                if (!c.baseline && c.hz == spec.hz) live = c.synth;
            std::string l = strf("загрузка %d, %s МГц: стабильно, поток %.0f МБ/с, задержка %.1f нс, оценка %.3f (живьём %.3f)", kipReboots,
                                 mhzStr(spec.hz).c_str(), out.b.bw(), out.b.latNs, out.score, live);
            kipLog.push_back(l);
            ev("Кип, %s", l.c_str());
            {
                auto ids = kipCheckCands();
                if (kipCheckIdx < (int)ids.size()) {
                    cands[ids[kipCheckIdx]].b = out.b;  // кандидат теперь живёт с этим кипом
                    cands[ids[kipCheckIdx]].synth = out.score;
                }
            }
            kipFixCand = -1;  // отжатые тайминги помогли — остаются
            kipFixTries = 0;
            kipCheckIdx++;
            return;
        }
    }
}

void Tuner::finishBaseline() {
    BenchResult avg;
    avg.valid = true;
    for (auto& b : baseRuns) {
        avg.readMBs += b.readMBs;
        avg.writeMBs += b.writeMBs;
        avg.copyMBs += b.copyMBs;
        avg.latNs += b.latNs;
        avg.lat2Ns += b.lat2Ns;
    }
    double n = (double)baseRuns.size();
    avg.readMBs /= n;
    avg.writeMBs /= n;
    avg.copyMBs /= n;
    avg.latNs /= n;
    avg.lat2Ns /= n;
    base = avg;
    double lo = 1e9, hi = 0;
    for (auto& b : baseRuns) {
        double s = scoreOf(b);
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    double spread = hi > 0 ? (hi - lo) : 0;
    if (maxMode()) {
        // Максимум: прирост засчитывается от 0,12 %, порог не растёт. Спорное решает замер А/Б: блоков столько, чтобы шум
        // разницы (2,5σ) дошёл до 0,10 %. Разброс трёх базовых замеров для этого слишком грубый — шум прохода
        // копится по всем пробам сеанса.
        eps = kMaxGain;
        ev("Шум замера %.2f %% (разброс базы), проход ±%.3f %% — прирост засчитывается от %.2f %%, спорный подтверждает замер А/Б (шум разницы до "
           "%.2f %%)",
           spread * 100, passSigma() * 100, kMaxGain * 100, pairNoise() * 100);
    } else {
        eps = std::max(0.003, std::min(0.02, spread * 1.5));
        ev("Шум замера %.2f %% — порог значимого прироста %.2f %%", spread * 100, eps * 100);
    }
    afterBaseline();
}

void Tuner::afterBaseline() {
    if (cfg.kipFirst && !kfTried && chip.preset) {
        phase = Phase::KipFirst;
        kfStep = 0;
        return;
    }
    if (singleHz) {
        // Одна частота: без прогонов. Старт — сток Ryazha-clk против таймингов гайда (Seed сравнит и возьмёт лучшее;
        // сток не держится — сразу гайд).
        targets = {singleHz};
        tIdx = 0;
        seedStage = 0;
        if (chip.preset && presetSet.empty()) {
            guideSet = guideAt(singleHz);
            presetSet = guideSet;
        }
        phase = Phase::Seed;
        ev("Подбор одной частоты: %s МГц — без прогонов частот; старт: сток Ryazha-clk%s", mhzStr(singleHz).c_str(),
           presetSet.empty() ? "" : " или тайминги гайда, что держится лучше");
        return;
    }
    phase = Phase::FreqScan;
    buildCoarse();
}

void Tuner::buildCoarse() {
    coarse.clear();
    scanPos = 0;
    lastOk = -1;
    failRun = 0;
    hangRun = 0;
    lastOkBw = 0;
    if (ladder.empty()) return;
    u64 minHz = cfg.minMhz > 0 ? (u64)cfg.minMhz * 1000000ULL - 500000ULL : 0;
    u64 maxHz = cfg.maxMhz > 0 ? (u64)cfg.maxMhz * 1000000ULL + 500000ULL : ~0ULL;
    // Каждая ступень лестницы демона в диапазоне — по возрастанию, без пропусков и без остановки на первом сбое.
    for (size_t i = 0; i < ladder.size(); i++)
        if (ladder[i] >= minHz && ladder[i] <= maxHz) coarse.push_back((int)i);
    if (coarse.empty()) {
        ev("В диапазоне %d…%s МГц нет ни одной ступени лестницы", cfg.minMhz, cfg.maxMhz ? strf("%d", cfg.maxMhz).c_str() : "верх");
        return;
    }
    ev("Прогон частот: %zu ступеней от %s до %s МГц, каждая — memtest и замер", coarse.size(), mhzStr(ladder[coarse.front()]).c_str(),
       mhzStr(ladder[coarse.back()]).c_str());
}

int Tuner::scanNextIdx() const { return scanPos < (int)coarse.size() ? coarse[scanPos] : -1; }

void Tuner::finishScan() {
    u32 stock = stockHz();
    if (scanPass == 0 && cfg.scanGuide && chip.preset && !kfDone) {
        // Прогон 2: тайминги гайда поверх стока Ryazha-clk — по всему диапазону, как и первый: и предел частоты, и пики
        // потока на тех же ступенях сравниваются напрямую.
        int keepStock = lastOk;
        guideSet = guideAt(0);  // для журнала и отчёта; на каждой ступени — guideAt(её частота)
        buildCoarse();
        stockLastOk = keepStock;
        if (freqCap)
            coarse.erase(std::remove_if(coarse.begin(), coarse.end(), [&](int i) { return ladder[i] > freqCap; }), coarse.end());
        if (!coarse.empty()) {
            scanPass = 1;
            ev("Сток Ryazha-clk держит до %s МГц. Прогон 2 — %s; весь диапазон %s…%s МГц",
               stockLastOk >= 0 ? mhzStr(ladder[stockLastOk]).c_str() : "—", guideName().c_str(), mhzStr(ladder[coarse.front()]).c_str(),
               mhzStr(ladder[coarse.back()]).c_str());
            return;
        }
    }
    if (kfDone) {
        std::string dl;
        for (auto& p : points)
            if (hzDiscarded(p.hz)) dl += (dl.empty() ? "" : ", ") + mhzStr(p.hz);
        if (!dl.empty()) ev("Отброшены (на кипе с таймингами гайда без живых правок не держатся): %s МГц", dl.c_str());
    }
    if (scanPass == 1) {
        int gOk = lastOk, sOk = stockLastOk;
        guideChosen = gOk >= 0 && (sOk < 0 || ladder[gOk] > ladder[sOk]);
        lastOk = guideChosen ? gOk : sOk;
        ev("Итог двух прогонов: сток Ryazha-clk — до %s МГц, тайминги гайда — до %s МГц. Доводим от %s",
           sOk >= 0 ? mhzStr(ladder[sOk]).c_str() : "—", gOk >= 0 ? mhzStr(ladder[gOk]).c_str() : "—",
           guideChosen ? "таймингов гайда" : "стока Ryazha-clk");
        if (guideChosen) presetSet = guideSet;
        scanPass = 2;  // прогоны закончены
    }
    if (lastOk < 0 || ladder[lastOk] <= stock) {
        phase = Phase::Failed;
        error = "ни одна разгонная частота не прошла проверку — проверьте VDD2/VDDQ и потолок РАМ в Ryazha-clk";
        ev("%s", error.c_str());
        return;
    }
    u32 fmax = ladder[lastOk];
    ev("Максимальная стабильная частота на общих таймингах: %s МГц", mhzStr(fmax).c_str());
    targets.clear();
    targets.push_back(fmax);
    // Остальные цели: сперва «JEDEC-частоты» (гайд Horizon-OC: часто держат таймингы туже), потом соседние ступени.
    static const double jedec[] = {1866, 1996.8, 2133, 2400, 2666, 2933, 3200};
    std::vector<u32> ok;
    int usePass = guideChosen ? 1 : 0;
    if (freqCap) ev("Цели и доводка — не выше потолка %s МГц (выше память зависала)", mhzStr(freqCap).c_str());
    for (auto& p : points)
        if (p.pass == usePass && p.status == 1 && p.hz < fmax && p.hz > stock && p.hz + 450000000u >= fmax && (!freqCap || p.hz <= freqCap))
            ok.push_back(p.hz);
    std::sort(ok.rbegin(), ok.rend());
    auto isJedec = [&](u32 hz) {
        for (double j : jedec)
            if (std::fabs(hz / 1e6 - j) / j < 0.006) return true;
        return false;
    };
    for (u32 hz : ok)
        if ((int)targets.size() < cfg.numTargets && isJedec(hz)) targets.push_back(hz);
    for (u32 hz : ok)
        if ((int)targets.size() < cfg.numTargets && std::find(targets.begin(), targets.end(), hz) == targets.end()) targets.push_back(hz);
    // Пики потока: частота, после которой поток падает. Не цель «по потоку», а ещё одна частота для доводки —
    // высокий поток на меньшей частоте.
    auto peaks = bandwidthPeaks(usePass);
    if (!peaks.empty()) {
        std::string pl;
        for (auto& pk : peaks) pl += (pl.empty() ? "" : ", ") + strf("%s (дальше −%.0f МБ/с)", mhzStr(pk.first).c_str(), pk.second);
        ev("Пики потока (следующая ступень медленнее на %d МБ/с и больше): %s", cfg.peakDropMb, pl.c_str());
        // Самые быстрые пики — первыми.
        std::vector<std::pair<double, u32>> byBw;
        for (auto& pk : peaks)
            for (auto& p : points)
                if (p.pass == usePass && p.hz == pk.first) byBw.push_back({p.b.bw(), p.hz});
        std::sort(byBw.rbegin(), byBw.rend());
        int added = 0;
        for (auto& bp : byBw) {
            if (added >= cfg.peakTargets) break;
            if (bp.second <= stock || (freqCap && bp.second > freqCap) || std::find(targets.begin(), targets.end(), bp.second) != targets.end())
                continue;
            targets.push_back(bp.second);
            added++;
        }
    }
    std::string list;
    for (u32 hz : targets) list += (list.empty() ? "" : ", ") + mhzStr(hz);
    ev("Доводим тайминги на частотах: %s МГц", list.c_str());
    phase = Phase::Seed;
    tIdx = 0;
    seedStage = 0;
}

std::vector<std::pair<u32, double>> Tuner::bandwidthPeaks(int pass) const {
    std::vector<const FreqPoint*> pts;
    for (auto& p : points)
        if (p.pass == pass && p.b.valid && (p.status == 1 || p.status == 3)) pts.push_back(&p);
    std::sort(pts.begin(), pts.end(), [](const FreqPoint* a, const FreqPoint* b) { return a->hz < b->hz; });
    std::vector<std::pair<u32, double>> out;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        if (pts[i]->status != 1) continue;
        double drop = pts[i]->b.bw() - pts[i + 1]->b.bw();
        if (drop >= cfg.peakDropMb) out.push_back({pts[i]->hz, drop});
    }
    return out;
}

// ============================================================== исследование потока

static std::string resCfg() { return paths::researchDir() + "research.cfg"; }
static std::string resCsv() { return paths::researchDir() + "research.csv"; }
static std::string resPending() { return paths::researchDir() + "research.pending"; }
static std::string resTxt() { return paths::researchDir() + "research.txt"; }
static const char* kResHeader = "hz_mhz,pass,rep,status,read,write,copy,bw,lat,lat2";

bool Tuner::researchActive() { return fileExists(resCfg()); }

void Tuner::researchStart(int reps) {
    writeFile(resCfg(), strf("reps=%d\nstarted=%lld\n", reps, (long long)epochNow()));
    writeFile(resCsv(), std::string(kResHeader) + "\n");
    removeFile(resPending());
    TLOG("===== Исследование потока: %d повторов прогона лестницы (сток и гайд) =====", reps);
}

std::string Tuner::researchStop() {
    std::string csv;
    readFile(resCsv(), csv);
    writeFile(resTxt(), researchTable(csv) + "\n(исследование прервано — таблица по собранным данным)\n");
    removeFile(resCfg());
    removeFile(resPending());
    TLOG("===== Исследование потока прервано =====");
    return "Исследование прервано: таблица по собранному — config/ram-sweetspot/research.txt";
}

bool Tuner::researchRecoverHang(std::string* what, s64 bootNow) {
    std::string p;
    if (!readFile(resPending(), p) || trim(p).empty()) return false;
    auto f = split(trim(p), '|');
    removeFile(resPending());
    if (f.size() < 3) return false;
    if (f.size() >= 4 && bootNow) {
        // Та же загрузка и нет нового отчёта о падении — исследование просто закрыли: ступень повторится.
        s64 boot = (s64)toLong(f[3], 0);
        std::vector<std::string> was = f.size() >= 5 ? split(f[4], ';') : std::vector<std::string>();
        std::sort(was.begin(), was.end());
        bool crashed = false;
        for (auto& c : crashReportNames())
            if (!std::binary_search(was.begin(), was.end(), c)) crashed = true;
        if (std::llabs(bootNow - boot) <= 5 && !crashed) {
            TLOG("  исследование: ступень %s МГц прервана выходом из приложения — не сбой, повторю", f[0].c_str());
            return false;
        }
    }
    std::string csv;
    readFile(resCsv(), csv);
    writeFile(resCsv(), csv + strf("%s,%s,%s,hang,0,0,0,0,0,0\n", f[0].c_str(), f[1].c_str(), f[2].c_str()));
    if (what) *what = f[0] + " МГц (" + f[1] + ", повтор " + f[2] + ")";
    TLOG("  ⚑ исследование: зависание на %s МГц (%s, повтор %s)", f[0].c_str(), f[1].c_str(), f[2].c_str());
    return true;
}

namespace {
struct ResRow {
    double mhz = 0;
    std::string pass;
    int rep = 0;
    std::string status;
    double r = 0, w = 0, c = 0, bw = 0, lat = 0;
};

std::vector<ResRow> parseRes(const std::string& csv) {
    std::vector<ResRow> out;
    for (auto& raw : split(csv, '\n')) {
        auto f = split(trim(raw), ',');
        if (f.size() < 10 || f[0] == "hz_mhz") continue;
        ResRow r;
        r.mhz = toDouble(f[0], 0);
        r.pass = f[1];
        r.rep = (int)toLong(f[2], 0);
        r.status = f[3];
        r.r = toDouble(f[4], 0);
        r.w = toDouble(f[5], 0);
        r.c = toDouble(f[6], 0);
        r.bw = toDouble(f[7], 0);
        r.lat = toDouble(f[8], 0);
        out.push_back(r);
    }
    return out;
}
}  // namespace

std::string Tuner::researchTable(const std::string& csv) {
    auto rows = parseRes(csv);
    std::string o = "Исследование потока: среднее и разброс по ступеням (поток = ∛(чтение·запись·копия), МБ/с)\n";
    o += "Δ — прирост среднего к прошлой ступени; «рост»/«спад» — отличим от шума (|Δ| > 2·σ разницы средних), «≈» — в пределах шума.\n";
    for (const char* pass : {"stock", "guide"}) {
        std::map<double, std::vector<double>> bw, lat;
        std::map<double, int> fails, hangs;
        for (auto& r : rows) {
            if (r.pass != pass) continue;
            if (r.status == "ok") {
                bw[r.mhz].push_back(r.bw);
                lat[r.mhz].push_back(r.lat);
            } else if (r.status == "hang") {
                hangs[r.mhz]++;
            } else {
                fails[r.mhz]++;
            }
            if (!bw.count(r.mhz)) bw[r.mhz];
        }
        if (bw.empty()) continue;
        o += strf("\n=== %s ===\n", std::string(pass) == "stock" ? "сток Ryazha-clk" : "тайминги гайда Horizon-OC «Common»");
        o += "     МГц   n   среднее      σ     мин     макс       Δ   оценка   задержка  сбои\n";
        double prevMean = 0, prevSe2 = 0;
        bool havePrev = false;
        int rises = 0, flats = 0, drops = 0;
        std::string plateau;
        double curFrom = 0, curTo = 0, prevHz = 0;
        int curLen = 0;
        for (auto& kv : bw) {
            auto& v = kv.second;
            int n = (int)v.size();
            std::string bad;
            if (fails.count(kv.first)) bad += strf("ошибок %d ", fails[kv.first]);
            if (hangs.count(kv.first)) bad += strf("зависаний %d", hangs[kv.first]);
            if (n == 0) {
                o += strf("%8.1f   0         —      —       —        —       —        —          —  %s\n", kv.first, bad.c_str());
                havePrev = false;
                continue;
            }
            double mean = 0, sd = 0, mn = 1e18, mx = 0, lt = 0;
            for (double x : v) {
                mean += x;
                mn = std::min(mn, x);
                mx = std::max(mx, x);
            }
            mean /= n;
            for (double x : v) sd += (x - mean) * (x - mean);
            sd = n > 1 ? std::sqrt(sd / (n - 1)) : 0;
            for (double x : lat[kv.first]) lt += x;
            lt /= std::max<size_t>(1, lat[kv.first].size());
            double se2 = n > 1 ? sd * sd / n : 0;
            std::string verdict = "—", d = "—";
            if (havePrev) {
                double delta = mean - prevMean;
                double noise = 2.0 * std::sqrt(se2 + prevSe2);
                d = strf("%+.0f", delta);
                if (n < 2 || noise <= 0) {
                    verdict = "мало данных";
                } else if (delta > noise) {
                    verdict = "рост";
                    rises++;
                } else if (delta < -noise) {
                    verdict = "спад";
                    drops++;
                } else {
                    verdict = "≈";
                    flats++;
                }
                if (verdict == "≈") {
                    if (curLen == 0) curFrom = prevHz;
                    curTo = kv.first;
                    curLen++;
                } else {
                    if (curLen >= 2) plateau += strf("%s%.1f…%.1f", plateau.empty() ? "" : ", ", curFrom, curTo);
                    curLen = 0;
                }
            }
            o += strf("%8.1f %3d %9.0f %6.0f %7.0f %8.0f %7s %8s %7.1f нс  %s\n", kv.first, n, mean, sd, mn, mx, d.c_str(), verdict.c_str(), lt,
                      bad.c_str());
            prevMean = mean;
            prevSe2 = se2;
            prevHz = kv.first;
            havePrev = true;
        }
        if (curLen >= 2) plateau += strf("%s%.1f…%.1f", plateau.empty() ? "" : ", ", curFrom, curTo);
        o += strf("Итог: достоверный рост на %d ступенях, спад на %d, в пределах шума — %d.", rises, drops, flats);
        o += plateau.empty() ? " Плато (2+ ступени подряд без отличимого роста) не найдено.\n"
                             : " Плато (2+ ступени подряд без отличимого роста): " + plateau + " МГц.\n";
    }
    return o;
}

bool Tuner::research(ITarget& t, const std::vector<u32>& lad, const ChipInfo& c, std::atomic<bool>* stop, std::string* msg) {
    std::string cfgText;
    if (!readFile(resCfg(), cfgText)) {
        *msg = "исследование не запущено";
        return false;
    }
    int reps = cfg.researchRuns;
    for (auto& l : split(cfgText, '\n'))
        if (startsWith(trim(l), "reps=")) reps = (int)toLong(trim(l).substr(5), reps);
    KnobSet guide;
    if (c.preset) {
        guide = ryazhaStockSet();
        for (auto& kv : presetToKnobs(c.preset->common)) guide[kv.first] = kv.second;
    }
    std::vector<u32> steps;
    for (u32 hz : lad) {
        if (cfg.minMhz > 0 && hz + 500000u < (u32)cfg.minMhz * 1000000u) continue;
        if (cfg.maxMhz > 0 && hz > (u32)cfg.maxMhz * 1000000u + 500000u) continue;
        steps.push_back(hz);
    }
    std::sort(steps.begin(), steps.end());
    std::vector<std::pair<std::string, KnobSet>> passes = {{"stock", ryazhaStockSet()}};
    if (!guide.empty()) passes.push_back({"guide", guide});
    int total = reps * (int)passes.size() * (int)steps.size();
    for (int rep = 1; rep <= reps; rep++) {
        for (auto& ps : passes) {
            // Что уже сделано, сколько зависаний подряд и потолок этого прогона — из записи (переживает перезагрузки).
            std::string csv;
            readFile(resCsv(), csv);
            auto rows = parseRes(csv);
            u32 cap = 0;  // потолок: после 2 зависаний подряд в прошлых повторах выше не идём
            for (int pr = 1; pr < rep; pr++) {
                int hr = 0;
                double lastOk = 0;
                std::vector<ResRow> rr;
                for (auto& r : rows)
                    if (r.pass == ps.first && r.rep == pr) rr.push_back(r);
                std::sort(rr.begin(), rr.end(), [](const ResRow& a, const ResRow& b) { return a.mhz < b.mhz; });
                for (auto& r : rr) {
                    if (r.status == "ok") {
                        hr = 0;
                        lastOk = r.mhz;
                    } else if (r.status == "hang" && ++hr >= std::max(1, cfg.scanHangStop) && cfg.scanHangStop > 0) {
                        u32 h = (u32)std::lround(lastOk * 1e6);
                        if (h && (!cap || h < cap)) cap = h;
                        break;
                    }
                }
            }
            int hangRunNow = 0;
            for (u32 hz : steps) {
                if (stop && stop->load()) {
                    *msg = "исследование на паузе — продолжится при следующем запуске";
                    return false;
                }
                double mhz = hz / 1e6;
                bool done = false;
                int doneCount = 0;
                for (auto& r : rows) {
                    if (r.pass == ps.first && r.rep == rep && std::fabs(r.mhz - mhz) < 0.05) {
                        done = true;
                        hangRunNow = r.status == "hang" ? hangRunNow + 1 : (r.status == "ok" ? 0 : hangRunNow);
                    }
                    if (r.status.size()) doneCount++;
                }
                if (done) continue;
                if (cfg.scanHangStop > 0 && hangRunNow >= cfg.scanHangStop) break;  // в этом повторе выше не идём
                if (cap && hz > cap) continue;
                {
                    std::lock_guard<std::recursive_mutex> lk(mtx);
                    researchStatus = strf("Исследование: повтор %d из %d, %s, %s МГц · сделано %d из %d ступеней", rep, reps,
                                          ps.first == "stock" ? "сток" : "гайд", mhzStr(hz).c_str(), doneCount, total);
                }
                {
                    std::string cl;
                    for (auto& f : crashReportNames()) cl += (cl.empty() ? "" : ";") + f;
                    writeFile(resPending(), strf("%.1f|%s|%d|%lld|%s", mhz, ps.first.c_str(), rep, (long long)t.bootEpoch(), cl.c_str()));
                }
                TrialSpec s;
                s.kind = TrialSpec::Scan;
                s.hz = hz;
                s.set = ps.second;
                if (ps.first == "guide") {
                    s.set = ryazhaStockSet();
                    for (auto& kv : presetKnobsAt(c.preset, hz, cfg.communityBands)) s.set[kv.first] = kv.second;
                }
                s.stabSec = cfg.quickSec;
                s.passes = cfg.scanPasses;
                s.desc = strf("Исследование: повтор %d/%d, %s, %s МГц", rep, reps, ps.first == "stock" ? "сток" : "гайд", mhzStr(hz).c_str());
                TrialOut out = runTrial(t, s, stop);
                removeFile(resPending());
                if (out.aborted) {
                    *msg = "исследование на паузе — продолжится при следующем запуске";
                    return false;
                }
                std::string st = out.ok ? "ok" : (failKind(out.reason) == "зависание" ? "hang" : "fail");
                if (st == "hang") hangRunNow++;
                else if (st == "ok") hangRunNow = 0;
                std::string line = strf("%.1f,%s,%d,%s,%.0f,%.0f,%.0f,%.0f,%.1f,%.1f\n", mhz, ps.first.c_str(), rep, st.c_str(), out.b.readMBs,
                                        out.b.writeMBs, out.b.copyMBs, out.b.bw(), out.b.latNs, out.b.lat2Ns);
                readFile(resCsv(), csv);
                writeFile(resCsv(), csv + line);
                rows = parseRes(csv + line);
                histTrial(s, out, "research:" + ps.first);
            }
        }
    }
    std::string csv;
    readFile(resCsv(), csv);
    std::string table = researchTable(csv);
    writeFile(resTxt(), table);
    removeFile(resCfg());
    TLOG("===== Исследование потока закончено: %s =====", resTxt().c_str());
    for (auto& l : split(table, '\n'))
        if (!l.empty()) TLOG("  %s", l.c_str());
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        researchStatus.clear();
    }
    *msg = "Исследование закончено: таблица — config/ram-sweetspot/research.txt, сырые данные — research.csv";
    return true;
}

// Порядок оптимизации: сперва тайминги, сильнее всего влияющие на поток.
static const char* const kOptOrder[] = {"tREFI", "tRFC", "tRCD", "tRP", "tRAS", "tRRD", "tWTR", "tRTW"};

std::vector<int> Tuner::optValuesFor(u32 hz, const std::string& key, int v) const {
    // Найденное значение и до трёх ступеней мягче: немного разжатый тайминг может дать больший поток —
    // максимум достигается не самыми тугими таймингами, а оптимальными. Туже — на ступень, если до известного
    // предела ещё есть место (предел подбора или реестр нестабильных).
    const KnobDef* d = findKnob(key);
    std::vector<int> vals;
    if (!d) return vals;
    int hiOk = d->max;
    // Не туже потолка плана подбора (tREFI выше 7800 нс — только по разрешению): иначе «на ступень туже»
    // за несколько проходов оптимизации уползает за него.
    for (auto& pi : plan)
        if (pi.key == key) hiOk = std::min(hiOk, pi.capMax);
    // Туже — не дальше абсолютного предела минус запас режима.
    // Значение пика уже давало сбой — повторно его пробует только перепроверка подбора, не оптимизация.
    auto lim = limits.find(key);
    if (lim != limits.end()) hiOk = std::min(hiOk, std::min(lim->second - cfg.margin, lim->second - 1));
    auto al = absLimits.find(key);
    if (al != absLimits.end()) hiOk = std::min(hiOk, std::min(al->second - cfg.margin, al->second - 1));
    int bound = 0;
    if (unstableBound(hz, key, &bound)) hiOk = std::min(hiOk, std::min(bound - cfg.margin, bound - 1));
    if (v + 1 <= hiOk) vals.push_back(v + 1);
    for (int x = v; x >= std::max(d->min, v - 3); x--) vals.push_back(x);
    return vals;
}

void Tuner::knobNext() {
    seqPos++;
    const KnobDef* d = kIdx < (int)plan.size() ? findKnob(plan[kIdx].key) : nullptr;
    bool monotonic = d && (d->order == KnobOrder::Up || d->order == KnobOrder::Down);
    // Тайминги и задержки (рискованные) идут до первого сбоя: нужен абсолютный предел, от него — запас режима.
    // «Терпение» — только для безопасных ручек, где сбоя может и не быть.
    if (monotonic && !d->risky && noimp >= cfg.patience) finishKnob();
}

void Tuner::optFinishKey() {
    const std::string key = optKeys[optKey];
    int was = cur.count(key) ? cur[key] : 0;
    if (optHaveBest && optBestVal != was) {
        cur[key] = optBestVal;
        optChanged = true;
        ev("Оптимизация: %s %d→%d — лучшая оценка %.3f, поток %.0f МБ/с (оптимальный, а не самый тугой)", key.c_str(), was, optBestVal, optBestScore,
           optBestBw);
    } else if (!optHaveBest) {
        // Ни одно значение не прошло — и найденное оказалось на краю. Отступаем на ступень мягче.
        const KnobDef* kd = findKnob(key);
        if (kd && was > kd->min) {
            cur[key] = was - 1;
            optChanged = true;
            ev("Оптимизация: %s нестабилен на всех значениях — %d→%d", key.c_str(), was, was - 1);
        }
    } else {
        ev("Оптимизация: %s = %d остаётся — лучший поток", key.c_str(), was);
    }
    optKey++;
    optPrepareKey();
}

double Tuner::optThr(bool softer) const {
    // «Максимум»: любое другое значение — только с приростом от 0,12 %. Безопасный и средний: туже — с приростом больше
    // порога шума, мягче — если не хуже чем на половину шума от самого быстрого (тот же поток при большем запасе).
    if (maxMode()) return kMaxGain;
    return softer ? -eps * 0.5 - optLoss : eps;
}

double Tuner::guardThr() const { return maxMode() ? 0.0 : -eps * 0.5; }

void Tuner::optPrepareKey() {
    optPos = 0;
    optLoss = 0;
    confirmPending = false;
    optHaveBest = false;
    optBestVal = 0;
    optBestBw = 0;
    optBestScore = 0;
    optMaxScore = 0;
    optVals.clear();
    u32 hz = tIdx < (int)targets.size() ? targets[tIdx] : 0;
    while (optKey < (int)optKeys.size()) {
        auto it = cur.find(optKeys[optKey]);
        if (it != cur.end()) {
            optVals = optValuesFor(hz, optKeys[optKey], it->second);
            if (optVals.size() > 1) {
                // Действующее значение — первым: оно эталон, другое берётся только по замеру А/Б.
                auto f = std::find(optVals.begin(), optVals.end(), it->second);
                if (f != optVals.end()) std::rotate(optVals.begin(), f, f + 1);
                return;
            }
        }
        optKey++;
    }
}

void Tuner::enterOptimize() {
    optKeys.clear();
    for (auto k : kOptOrder) {
        auto it = cur.find(k);
        const KnobDef* d = findKnob(k);
        if (it != cur.end() && d && it->second > d->min) optKeys.push_back(k);
    }
    optKey = optPos = optPass = 0;
    optChanged = false;
    optStart = cur;
    optGuard = 0;
    optGuardA = optGuardB = 0;
    if (optKeys.empty()) {
        phase = Phase::Verify;
        return;
    }
    phase = Phase::Optimize;
    optPrepareKey();
    std::string l;
    for (auto& k : optKeys) l += (l.empty() ? "" : ", ") + k;
    ev("%s МГц: оптимизация таймингов по потоку (%s); абсолютные пределы: %s; запас режима %d", mhzStr(targets[tIdx]).c_str(), l.c_str(),
       absLimits.empty() ? "—" : knobSetShort(absLimits).c_str(), cfg.margin);
}

bool Tuner::optimizeCandidate(ITarget& t, int idx, std::string* msg) {
    Candidate c;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size() || cands[idx].baseline) {
            *msg = "выберите кандидата";
            return false;
        }
        c = cands[idx];
        ev("Оптимизация %s после TOTK: перебор таймингов по оценке", c.name().c_str());
    }
    // Как в оптимизации подбора: действующее значение — первым, оно эталон; другое берётся только по замеру А/Б
    // (порог — optThr: в «максимуме» прирост от 0,12 %, иначе туже — больше шума, мягче — не хуже половины шума).
    KnobSet set = c.set;
    std::string steps;
    for (auto k : kOptOrder) {
        auto it = set.find(k);
        const KnobDef* d = findKnob(k);
        if (it == set.end() || !d || it->second <= d->min) continue;
        std::vector<int> vals;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            vals = optValuesFor(c.hz, k, it->second);
        }
        auto f = std::find(vals.begin(), vals.end(), it->second);
        if (f != vals.end()) std::rotate(vals.begin(), f, f + 1);
        int bestVal = it->second;
        double bestScore = 0, loss = 0;
        bool have = false;
        for (int v : vals) {
            TrialSpec s;
            s.kind = TrialSpec::Optimize;
            s.hz = c.hz;
            s.set = set;
            s.set[k] = v;
            s.key = k;
            s.val = v;
            s.stabSec = cfg.quickSec;
            s.passes = std::max(cfg.benchPasses + 1, cfg.timingPasses);
            s.desc = strf("Оптимизация %s: %s = %d", c.name().c_str(), k, v);
            TrialOut out = runTrial(t, s, nullptr);
            TrialSpec q;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx);
                if (!out.ok) {
                    noteUnstable(c.hz, failKind(out.reason), k, v, s.set, out.reason);
                    continue;
                }
                addNoise(out.b);
                if (!have) {
                    have = true;
                    bestVal = v;
                    bestScore = out.score;
                    continue;
                }
                bool softer = v < bestVal;
                double thr = maxMode() ? kMaxGain : softer ? -eps * 0.5 - loss : eps;
                double dlt = out.score / bestScore - 1, sd = std::sqrt(2.0) * trialSigma(s.passes);
                ev("Оптимизация %s: %s = %d — %+.2f %% к %d (порог %+.2f %%)", c.name().c_str(), k, v, dlt * 100, bestVal, thr * 100);
                if (dlt < thr - kScreenK * sd) continue;
                q.kind = TrialSpec::Confirm;
                q.hz = c.hz;
                q.ref = set;
                q.ref[k] = bestVal;
                q.set = s.set;
                q.key = k;
                q.val = v;
                q.passes = kPairPasses;
                q.thr = thr;
                q.desc = strf("Оптимизация %s: %s = %d — подтверждение А/Б против %d", c.name().c_str(), k, v, bestVal);
            }
            TrialOut p = runTrial(t, q, nullptr);
            std::lock_guard<std::recursive_mutex> lk(mtx);
            for (auto& eb : p.extra) addNoise(eb);
            if (!p.ok) {
                if (!p.refFailed && !p.aborted) noteUnstable(c.hz, failKind(p.reason), k, v, q.set, p.reason);
                ev("Оптимизация %s: %s = %d — подтверждение А/Б не прошло (%s)", c.name().c_str(), k, v, p.reason.c_str());
                continue;
            }
            bool acc = p.delta >= q.thr;
            ev("Оптимизация %s: %s = %d против %d — А/Б, блоков %d: %+.3f %% ± %.3f %% — %s", c.name().c_str(), k, v, bestVal, p.blocks, p.delta * 100,
               p.sigma * 100, acc ? "берём" : "не лучше");
            if (acc) {
                loss = std::min(0.0, loss + p.delta);
                bestVal = v;
            }
            bestScore = acc ? p.score : p.refScore;  // выбранное только что замерено заново — сравнение дальше с ним
        }
        if (have && bestVal != set[k]) {
            steps += (steps.empty() ? "" : ", ") + strf("%s %d→%d", k, set[k], bestVal);
            set[k] = bestVal;
        }
    }
    // Финиш (эксперимент, «максимум»): tREFI и tRFC вместе — на этом кипе, после перебора по одному.
    bool rf = refreshSync(t, c.hz, set, &steps);
    flushUnstable(t);
    if (steps.empty()) {
        *msg = c.name() + ": тайминги уже оптимальны по потоку";
        std::lock_guard<std::recursive_mutex> lk(mtx);
        ev("Оптимизация: %s", msg->c_str());
        return true;
    }
    // Итог — длинной проверкой, как любой кандидат.
    TrialSpec s;
    s.kind = TrialSpec::Verify;
    s.hz = c.hz;
    s.set = set;
    s.stabSec = rf ? std::max(cfg.verifySec, 120) : cfg.verifySec;  // после финиша — дольше: длинный tREFI сбоит на прогретой памяти
    s.passes = cfg.benchPasses + 1;
    s.desc = "Оптимизация " + c.name() + ": длинная проверка";
    TrialOut out = runTrial(t, s, nullptr);
    TrialOut g;
    if (out.ok) {
        // Защита итога: новый набор против исходного замером А/Б. Хуже порога — кандидат не создаётся.
        TrialSpec q;
        q.kind = TrialSpec::OptGuard;
        q.hz = c.hz;
        q.ref = c.set;
        q.set = set;
        q.passes = kPairPasses;
        q.thr = guardThr();
        q.desc = "Оптимизация " + c.name() + ": проверка итога — А/Б против исходного";
        g = runTrial(t, q, nullptr);
    }
    std::lock_guard<std::recursive_mutex> lk(mtx);
    for (auto& eb : g.extra) addNoise(eb);
    if (!out.ok || (!g.ok && !g.refFailed)) {
        std::string why = !out.ok ? out.reason : g.reason;
        noteUnstable(c.hz, failKind(why), "", 0, set, "оптимизированный набор: " + why);
        *msg = c.name() + ": оптимизированный набор (" + steps + ") не прошёл " + (!out.ok ? "длинную проверку" : "проверку итога") +
               " — оставляю как был";
        ev("Оптимизация: %s", msg->c_str());
        std::string snap = serialize();
        t.saveText(paths::state(), snap);
        flushUnstable(t);
        return false;
    }
    if (g.ok && g.delta < guardThr()) {
        *msg = strf("%s: оптимизированный набор (%s) хуже исходного — А/Б %+.3f %% ± %.3f %%, новый кандидат не создан", c.name().c_str(),
                    steps.c_str(), g.delta * 100, g.sigma * 100);
        ev("Оптимизация: %s", msg->c_str());
        std::string snap = serialize();
        t.saveText(paths::state(), snap);
        return false;
    }
    addCandidate(c.hz, set, out.b, out.score);
    cands.back().note = "оптимизирован из " + c.name() + ": " + steps;
    *msg = strf("Новый кандидат %s (%s): поток %.0f МБ/с против %.0f, А/Б %+.2f %% к исходному. Прогоните его в TOTK", cands.back().name().c_str(),
                steps.c_str(), out.b.bw(), c.b.bw(), g.ok ? g.delta * 100 : 0.0);
    ev("Оптимизация: %s", msg->c_str());
    computeRanking();
    std::string snap = serialize();
    t.saveText(paths::state(), snap);
    return true;
}

void Tuner::startTiming(const KnobSet& set, double score) {
    limits.clear();
    absLimits.clear();
    cur = set;
    curScore = score;
    kIdx = 0;
    accepted.clear();
    // Значения пресета тоже можно будет ослаблять по ступеням, если длинная проверка не пройдёт.
    for (auto& kv : set) accepted.push_back({kv.first, "inh", false});
    remeasure = false;
    remeasureFails = 0;
    verifyTries = 0;
    loosenRound = 0;
    phase = Phase::Timing;
    resetKnob();
}

void Tuner::nextTarget() {
    tIdx++;
    seedStage = 0;
    cur.clear();
    accepted.clear();
    remeasure = false;
    phase = Phase::Seed;
}

void Tuner::resetKnob() {
    seqPos = 0;
    needRef = true;
    chainAB = chainEnd = false;
    haveBest = false;
    bestVal = 0;
    bestScore = 0;
    startScore = curScore;
    noimp = 0;
    confirmPending = false;
    confirmScore = 0;
    haveFail = false;
    failVal = 0;
    if (kIdx >= (int)plan.size()) return;
    const KnobDef* d = findKnob(plan[kIdx].key);
    auto it = cur.find(plan[kIdx].key);
    if (it != cur.end()) {
        seqStartInh = false;
        seqStart = it->second;
    } else if (d && d->absentIsZero) {
        seqStartInh = false;
        seqStart = 0;
    } else {
        seqStartInh = true;
        seqStart = 0;
    }
}

std::vector<int> Tuner::buildSeq() const {
    std::vector<int> seq;
    if (kIdx >= (int)plan.size()) return seq;
    const PlanItem& pi = plan[kIdx];
    const KnobDef* d = findKnob(pi.key);
    if (!d) return seq;
    int hi = std::min(d->max, pi.capMax), lo = std::max(d->min, pi.capMin);
    int bound = 0;
    u32 hz = (phase == Phase::Timing && tIdx < (int)targets.size()) ? targets[tIdx] : 0;
    if (hz && unstableBound(hz, pi.key, &bound)) {
        if (d->order == KnobOrder::Up) hi = std::min(hi, bound - 1);
        if (d->order == KnobOrder::Down) lo = std::max(lo, bound + 1);
    }
    switch (d->order) {
        case KnobOrder::Up:
            for (int v = seqStartInh ? lo : seqStart + 1; v <= hi; v++) seq.push_back(v);
            break;
        case KnobOrder::Down:
            for (int v = seqStart - 1; v >= lo; v--) seq.push_back(v);
            break;
        case KnobOrder::Explore:
            for (int v = lo; v <= hi; v++)
                if (v != seqStart) seq.push_back(v);
            break;
        case KnobOrder::Toggle:
            if (seqStart != hi) seq.push_back(hi);
            break;
    }
    return seq;
}

std::string Tuner::valueStr(const std::string& key) const {
    auto it = cur.find(key);
    return it == cur.end() ? std::string("inh") : strf("%d", it->second);
}

void Tuner::setValue(KnobSet& s, const std::string& key, const std::string& val) const {
    const KnobDef* d = findKnob(key);
    if (val == "inh") {
        s.erase(key);
        return;
    }
    int v = (int)toLong(val, 0);
    if (d && d->absentIsZero && v == 0)
        s.erase(key);
    else
        s[key] = v;
}

void Tuner::finishKnob() {
    if (kIdx >= (int)plan.size()) return;
    const std::string key = plan[kIdx].key;
    const KnobDef* d = findKnob(key);
    auto seq = buildSeq();
    auto pos = [&](int v) {
        for (size_t i = 0; i < seq.size(); i++)
            if (seq[i] == v) return (int)i;
        return -1;
    };
    bool monotonic = d && (d->order == KnobOrder::Up || d->order == KnobOrder::Down);
    std::string prev = valueStr(key);
    bool changed = false;
    bool edge = false;
    std::string chosenStr;

    (void)pos;
    if (monotonic && d->risky) {
        // Абсолютный предел — последнее стабильное значение перед первым сбоем: сбой этой пробы или известный по
        // реестру нестабильных этой частоты (туда поиск уже не идёт). Запас режима (максимум 0, средний 1,
        // безопасный 2) отсчитывается от него — не от стартового значения и не от пресета.
        bool up = d->order == KnobOrder::Up;
        auto tighter = [up](int a, int b) { return up ? a > b : a < b; };
        u32 hz = tIdx < (int)targets.size() ? targets[tIdx] : 0;
        int fail = 0, bound = 0;
        bool known = false;
        if (haveFail) {
            fail = failVal;
            known = true;
        } else if (hz && unstableBound(hz, key, &bound)) {
            fail = bound;
            known = true;
        }
        int curV = 0;
        bool curHas = cur.count(key) > 0;
        if (curHas)
            curV = cur[key];
        else if (d->absentIsZero)
            curHas = true;  // нет в наборе — ноль
        int base = haveBest ? bestVal : curV;
        int chosen = base;
        bool keep = !haveBest && !curHas;  // общее значение кипа, туже — без прироста: не трогаем
        if (known) {
            // Пик — первое значение со сбоем. Реестр мог записать сбой туже того, что уже прошло, — тогда пик сразу за ним.
            int peak = fail;
            if (curHas && !tighter(fail, curV)) peak = up ? curV + 1 : curV - 1;
            int lastOk = up ? peak - 1 : peak + 1;
            // Режим отсчитывается от самого пика: максимум — пик (перепроверяется, не прошёл — ступень мягче),
            // средний — пик −1, безопасный — пик −2.
            int allowed = up ? peak - cfg.margin : peak + cfg.margin;
            allowed = std::max(d->min, std::min(d->max, allowed));
            // Задержки и растяжки (ноль — сток движка) запасом мягче стока Ryazha-clk не уходят: сток уже проверен.
            if (d->absentIsZero) allowed = up ? std::max(allowed, 0) : std::min(allowed, 0);
            absLimits[key] = peak;
            if (!limits.count(key)) limits[key] = peak;
            if (!keep) chosen = tighter(base, allowed) ? allowed : base;
            edge = !tighter(lastOk, chosen);  // вплотную к пику — первый подозреваемый, если длинная проверка не пройдёт
            ev("%s: пик %d (%s) — %s; режим: пик %s → не туже %d", key.c_str(), peak, knobValueText(*d, peak).c_str(),
               haveFail ? "сбой в этой пробе" : "сбой по реестру нестабильных", cfg.margin ? strf("−%d", cfg.margin).c_str() : "0", allowed);
        } else {
            ev("%s: сбоя до края диапазона нет — предел не найден, берётся лучшее по оценке", key.c_str());
        }
        if (!keep && (!curHas || chosen != curV)) {
            setValue(cur, key, strf("%d", chosen));
            changed = true;
            chosenStr = strf("%d", chosen);
            if (haveBest && chosen == bestVal)
                curScore = bestScore;
            else
                remeasure = true;
        }
    } else if (haveBest) {
        setValue(cur, key, strf("%d", bestVal));
        changed = true;
        chosenStr = strf("%d", bestVal);
        curScore = bestScore;
    }
    if (changed) {
        // Если ручка уже была в списке (значение из пресета) — запоминаем самое первое «прежнее».
        std::string origPrev = prev;
        for (size_t i = 0; i < accepted.size(); i++) {
            if (accepted[i].key == key) {
                origPrev = accepted[i].prev;
                accepted.erase(accepted.begin() + i);
                break;
            }
        }
        accepted.push_back({key, origPrev, edge});
        ev("Принято %s = %s (было %s)%s", key.c_str(), chosenStr.c_str(), prev == "inh" ? "общее" : prev.c_str(),
           remeasure ? " — с запасом, перепроверка" : "");
    } else {
        ev("%s оставлен как был", key.c_str());
    }
    kIdx++;
    resetKnob();
}

// Ослабить одну принятую ручку на ступень (или вернуть к прежнему значению).
bool Tuner::loosenOne(size_t i) {
    if (i >= accepted.size()) return false;
    const std::string key = accepted[i].key;
    const KnobDef* d = findKnob(key);
    if (!d) return false;
    auto it = cur.find(key);
    int now = it != cur.end() ? it->second : 0;
    std::string prev = accepted[i].prev;
    bool toPrev = true;
    int looser = now;
    if (d->order == KnobOrder::Up) {
        looser = now - 1;
        int pv = prev == "inh" ? -1 : (int)toLong(prev, -1);
        toPrev = looser <= pv;
    } else if (d->order == KnobOrder::Down) {
        looser = now + 1;
        int pv = prev == "inh" ? 0 : (int)toLong(prev, 0);
        toPrev = looser >= pv;
    }
    if (toPrev) {
        setValue(cur, key, prev);
        accepted.erase(accepted.begin() + i);
        ev("Ослабление: %s возвращён к %s", key.c_str(), prev == "inh" ? "общему" : prev.c_str());
    } else {
        setValue(cur, key, strf("%d", looser));
        accepted[i].edge = false;
        ev("Ослабление: %s = %d", key.c_str(), looser);
    }
    return true;
}

// Порядок отжима. Тайминги — первыми (tREFI последним: он и есть пропускная способность), по кругу.
// Ручки пропускной способности — только когда тайминги отжаты до конца: от дающих меньше всего к RL.
static const char* const kLoosenTimings[] = {"tRAS", "tRRD", "tRFC", "tRCD", "tRP", "tWTR", "tRTW", "tREFI"};
static const char* const kLoosenBw[] = {"latAllow", "preRef", "arbSafe", "tXP",     "tCKESR", "tW2P",    "tRPab", "rext",  "wext", "tFAW",
                                        "outReq",   "daCovers", "WLset", "daTurns", "arbWin", "ptsaCpu", "ramPD", "WL",    "RL"};

static bool isTimingKey(const std::string& k) {
    for (auto t : kLoosenTimings)
        if (k == t) return true;
    return false;
}

// Одна ступень ослабления ручки в наборе. Тайминги — ступень вниз, с нижней — к общему значению кипа;
// задержки (минус — туже) — на ступень к нулю; режимы — к стоку (убрать из набора).
static bool loosenKeyIn(KnobSet& set, const std::string& key, std::string* desc) {
    auto it = set.find(key);
    const KnobDef* d = findKnob(key);
    if (it == set.end() || !d) return false;
    int v = it->second;
    if (d->order == KnobOrder::Up) {
        if (v <= d->min) return false;  // уже сток Ryazha-clk — мягче некуда (общее значение кипа могло бы оказаться туже)
        it->second = v - 1;
        *desc = strf("%s %d→%d", key.c_str(), v, v - 1);
        return true;
    }
    if (d->order == KnobOrder::Down) {
        if (v >= 0) return false;
        it->second = v + 1;
        *desc = strf("%s %d→%d", key.c_str(), v, v + 1);
        return true;
    }
    if (v == 0) return false;  // режим уже стоковый
    it->second = 0;
    *desc = strf("%s %d→сток", key.c_str(), v);
    return true;
}

// Тайминг по кругу, начиная с round-го. false — таймингов в наборе не осталось.
static bool loosenTimingIn(KnobSet& set, int round, std::string* desc) {
    const int n = (int)(sizeof(kLoosenTimings) / sizeof(kLoosenTimings[0]));
    for (int k = 0; k < n; k++)
        if (loosenKeyIn(set, kLoosenTimings[(round + k) % n], desc)) return true;
    return false;
}

// Длинная проверка не прошла. Сначала разом ослабляем тайминги, принятые вплотную к сбою; потом по одному остальные
// тайминги; и только когда отжимать их больше некуда — ручки пропускной способности, от дающих меньше всего.
bool Tuner::loosenForVerify() {
    bool any = false;
    for (size_t i = accepted.size(); i-- > 0;) {
        if (accepted[i].edge && isTimingKey(accepted[i].key)) {
            loosenOne(i);
            any = true;
        }
    }
    if (any) return true;
    // Тайминги — по кругу: каждая следующая попытка берёт следующий, а не долбит один и тот же.
    std::vector<size_t> tim;
    for (auto key : kLoosenTimings)
        for (size_t i = 0; i < accepted.size(); i++)
            if (accepted[i].key == key) tim.push_back(i);
    if (!tim.empty()) return loosenOne(tim[(size_t)(loosenRound++) % tim.size()]);
    for (auto key : kLoosenBw)
        for (size_t i = 0; i < accepted.size(); i++)
            if (accepted[i].key == key) {
                ev("Тайминги отжаты до конца — уступаю пропускную способность: %s", key);
                return loosenOne(i);
            }
    return false;
}

void Tuner::addCandidate(u32 hz, const KnobSet& set, const BenchResult& b, double score) {
    Candidate c;
    int maxId = 0;
    for (auto& x : cands) maxId = std::max(maxId, x.id);
    c.id = maxId + 1;
    c.hz = hz;
    c.set = set;
    c.b = b;
    c.synth = score;
    cands.push_back(c);
}

// ============================================================== перенос в кип

std::map<int, int> Tuner::kipDesired(const std::vector<std::string>& extra) const {
    std::map<int, int> m;
    for (auto& kv : kipVals) {
        int slot = kipSlotOf(kv.first);
        if (slot < 0) continue;
        bool on = std::find(kipAccepted.begin(), kipAccepted.end(), kv.first) != kipAccepted.end() ||
                  std::find(extra.begin(), extra.end(), kv.first) != extra.end();
        auto o = kipOrig.find(slot);
        m[slot] = on ? kv.second : (o != kipOrig.end() ? o->second : 0);
    }
    return m;
}

KnobSet Tuner::kipResidual(const KnobSet& set, const std::vector<std::string>& inKip) const {
    // Ручки, что уже в кипе, из timings.ini убираем: иначе поправка частоты перекрыла бы кип, и проверялся бы не он.
    KnobSet r = set;
    for (auto& k : inKip) r.erase(k);
    return r;
}

std::vector<int> Tuner::kipCheckCands() const {
    std::vector<int> ids;
    std::vector<u32> seen;
    for (size_t i = 0; i < cands.size(); i++) {
        if (cands[i].baseline || std::find(seen.begin(), seen.end(), cands[i].hz) != seen.end()) continue;
        seen.push_back(cands[i].hz);
        ids.push_back((int)i);
    }
    return ids;
}

static std::string joinKeys(const std::vector<std::string>& keys, const KnobSet& vals) {
    std::string o;
    for (auto& k : keys) {
        auto it = vals.find(k);
        o += (o.empty() ? "" : ", ") + k + (it != vals.end() ? strf("=%d", it->second) : std::string());
    }
    return o.empty() ? std::string("—") : o;
}

void Tuner::enterKip() {
    int best = -1;
    for (size_t i = 0; i < cands.size(); i++)
        if (!cands[i].baseline && (best < 0 || cands[i].synth > cands[best].synth)) best = (int)i;
    if (best < 0) {
        enterGame();
        return;
    }
    kipSrc = cands[best].id;
    kipVals.clear();
    for (auto& kv : cands[best].set)
        if (kipSlotOf(kv.first) >= 0) kipVals[kv.first] = kv.second;
    kipOrig.clear();
    kipAccepted.clear();
    kipQueue.clear();
    kipTesting.clear();
    kipRejected.clear();
    kipFinal = kipDirty = false;
    kipReboots = 0;
    kipCheckIdx = 0;
    kipLog.clear();
    kipStep = KipWrite;
    if (kipVals.empty()) {
        ev("Перенос в кип: у лучшего кандидата нет ручек пропускной способности — пропущен");
        enterGame();
        return;
    }
    std::vector<std::string> all;
    for (auto& kv : kipVals) all.push_back(kv.first);
    kipQueue.push_back(all);
    phase = Phase::Kip;
    kipHangs = 0;
    // Живая проверка до записи: ненулевые значения (ноль — сток движка, он безопасен) на каждой частоте кандидатов,
    // где кандидат с этим значением не проверялся.
    kipPre.clear();
    kipPrePos = 0;
    for (int ci : kipCheckCands())
        for (auto& kv : kipVals) {
            if (kv.second == 0) continue;
            auto it = cands[ci].set.find(kv.first);
            if (it == cands[ci].set.end() || it->second != kv.second) kipPre.push_back({ci, kv.first});
        }
    kipStep = kipPre.empty() ? KipWrite : KipPre;
    ev("Перенос в кип с %s МГц: %s.%s Каждая проверка кипа — перезагрузка; после неё снова откройте RamSweetSpot",
       mhzStr(cands[best].hz).c_str(), knobSetCompact(kipVals).c_str(),
       kipPre.empty() ? "" : strf(" Сначала %zu живых проверок на других частотах кандидатов — без перезагрузок.", kipPre.size()).c_str());
}

int Tuner::kipHangLimit() const {
    // Деление пакета с виновником, вешающим консоль, требует до log2(N)+1 зависаний — даём их и ещё одно про запас,
    // но петлю зависаний обрываем: не меньше 3.
    int n = std::max(1, (int)kipVals.size());
    int lg = 0;
    while ((1 << lg) < n) lg++;
    return std::max(3, lg + 2);
}

bool Tuner::hzDiscarded(u32 hz) const {
    if (!kfDone) return false;
    bool fail = false, ok = false;
    for (auto& p : points)
        if (p.hz == hz && p.pass == 0) {
            if (p.status == 1 || p.status == 3) ok = true;
            if (p.status == 2) fail = true;
        }
    return fail && !ok;
}

bool Tuner::kipFirstAdvance(ITarget& t) {
    std::unique_lock<std::recursive_mutex> lk(mtx);
    auto finish = [&](bool done, const std::string& why) {
        kfTried = true;
        kfDone = done;
        ev("Кип сначала: %s", why.c_str());
        afterBaseline();
        std::string snap = serialize();
        lk.unlock();
        t.saveText(paths::state(), snap);
        return true;
    };
    KnobSet want;
    std::string cut;
    // С полосами — нижняя: одни общие тайминги на все частоты, а верхние полосы у этого набора туже.
    for (auto& kv : presetToKnobs(bandsOn() ? chip.preset->bands[0].t : chip.preset->common)) {
        if (!findKnob(kv.first) || findKnob(kv.first)->absentIsZero) continue;
        // Демон принимает в кип не всё: выше границы — сбросит в 0. Обрезаем, а то, что кипом не задаётся, не трогаем.
        int mx = 0;
        if (t.kipCommonMax(kv.first, &mx)) {
            if (mx <= 0) {
                cut += strf(" %s (кипом не задаётся)", kv.first.c_str());
                continue;
            }
            if (kv.second > mx) {
                cut += strf(" %s %d→%d", kv.first.c_str(), kv.second, mx);
                want[kv.first] = mx;
                continue;
            }
        }
        want[kv.first] = kv.second;
    }
    if (!cut.empty() && kfStep == 0) ev("Кип сначала: по границам демона —%s", cut.c_str());
    if (kfStep == 1) {
        bool rebooted = std::llabs(t.bootEpoch() - (s64)kfBootAt) > 5;
        if (rebooted) {
            KnobSet now;
            std::string err;
            lk.unlock();
            bool okRead = t.kipCommonRead(&now, &err);
            lk.lock();
            bool present = okRead;
            for (auto& kv : kfVals)
                if (!now.count(kv.first) || now[kv.first] != kv.second) present = false;
            if (!present) {
                noteUnstable(stockHz(), "зависание", "", 0, kfVals, "кип сначала: с общими таймингами гайда консоль не загрузилась (откат сторожем)");
                return finish(false, "с общими таймингами гайда консоль не загрузилась — сторож откатил правку. Прогоны — как обычно, на стоке");
            }
            return finish(true, "загрузка прошла, в кипе общие тайминги гайда " + knobSetShort(kfVals) +
                                    ". Прогоны частот — на них, без живых правок; частоты, что не держатся, отбрасываются");
        }
        ev("Кип сначала: перезагрузки не было — повторяю");
        kfStep = 0;
    }
    int wd = t.watchdogState();
    if (wd == kWatchdogNone) return finish(false, "в этой версии Ryazha-clk сторожа нет — пропущено (без него зависание не откатит кип)");
    if (wd == 0) return finish(false, "сторож Ryazha-clk выключен — пропущено (без него зависание не откатит кип)");
    KnobSet cur;
    std::string err;
    lk.unlock();
    bool okRead = t.kipCommonRead(&cur, &err);
    lk.lock();
    if (!okRead) return finish(false, "общие тайминги кипа не читаются (" + err + ") — пропущено");
    // Копия до правки — для «Вернуть как было»; не перезаписываем, если уже есть.
    std::string backup;
    if (!t.loadText(paths::kipCommonBackup(), backup) || backup.empty()) t.saveText(paths::kipCommonBackup(), knobSetToString(cur) + "\n");
    bool same = true;
    for (auto& kv : want)
        if (!cur.count(kv.first) || cur[kv.first] != kv.second) same = false;
    kfVals = want;
    if (same) return finish(true, "общие тайминги гайда уже в кипе — без перезагрузки");
    lk.unlock();
    bool okWrite = t.kipCommonWrite(want, &err);
    lk.lock();
    if (!okWrite) return finish(false, "демон не принял общие тайминги (" + err + ") — пропущено");
    kfStep = 1;
    kfBootAt = (u64)t.bootEpoch();
    status = "Кип сначала: общие тайминги гайда записаны — перезагрузка. После загрузки снова откройте RamSweetSpot";
    ev("Кип сначала: общие тайминги гайда %s записаны в кип — перезагрузка", knobSetShort(want).c_str());
    std::string snap = serialize();
    lk.unlock();
    t.saveText(paths::state(), snap);
    t.idle(4000);  // сторож запоминает правку
    if (!t.reboot(&err)) {
        std::lock_guard<std::recursive_mutex> lk2(mtx);
        status = "Не удалось перезагрузить (" + err + "). Перезагрузите консоль через меню питания и снова откройте RamSweetSpot";
        ev("Кип сначала: %s", status.c_str());
        t.saveText(paths::state(), serialize());
    }
    return false;
}

void Tuner::kipAbort(const std::string& why) {
    // Аварийный откат: всё, что успели принять или собирались проверить, — в отклонённые, кип — к исходному
    // значению одной записью с проверкой после загрузки.
    std::vector<std::string> all = kipAccepted;
    all.insert(all.end(), kipTesting.begin(), kipTesting.end());
    for (auto& g : kipQueue) all.insert(all.end(), g.begin(), g.end());
    for (auto& k : all) kipRejected.push_back({k, kipVals[k], "аварийный откат: " + why});
    kipAccepted.clear();
    kipTesting.clear();
    kipQueue.clear();
    kipDirty = true;
    kipFinal = true;
    kipStep = KipWrite;
    kipCheckIdx = 0;
    kipFixTries = 0;
    std::string l = "аварийный откат кипа к исходному: " + why;
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
}

void Tuner::kipGroupFailed(const std::string& why) {
    std::vector<std::string> g = kipTesting;
    kipTesting.clear();
    kipStep = KipWrite;
    kipCheckIdx = 0;
    kipFixTries = 0;
    if (kipFinal) {
        if (kipAccepted.empty()) {
            // Не держится даже исходный кип — дело не в перенесённых ручках. Дальше не крутим.
            std::string l = "исходный кип тоже не прошёл проверку (" + why + ") — перенос остановлен, кип как до подбора";
            kipLog.push_back(l);
            ev("Кип: %s", l.c_str());
            kipFinal = false;
            kipDirty = false;
            kipQueue.clear();
            return;
        }
        for (auto& k : kipAccepted) kipRejected.push_back({k, kipVals[k], "вместе с остальными не держится: " + why});
        std::string l = "итоговый набор не прошёл (" + why + ") — возвращаю исходные значения кипа";
        kipLog.push_back(l);
        ev("Кип: %s", l.c_str());
        kipAccepted.clear();
        kipDirty = true;
        return;
    }
    kipDirty = true;
    if (g.size() <= 1) {
        if (!g.empty()) {
            kipRejected.push_back({g[0], kipVals[g[0]], why});
            std::string l = strf("виновник: %s = %d — %s", g[0].c_str(), kipVals[g[0]], why.c_str());
            kipLog.push_back(l);
            ev("Кип: %s", l.c_str());
        }
        return;
    }
    size_t half = g.size() / 2;
    std::vector<std::string> a(g.begin(), g.begin() + half), b(g.begin() + half, g.end());
    kipQueue.insert(kipQueue.begin(), b);
    kipQueue.insert(kipQueue.begin(), a);
    std::string l = strf("пакет из %zu не прошёл (%s) — делю пополам: [%s] и [%s]", g.size(), why.c_str(), joinKeys(a, kipVals).c_str(),
                         joinKeys(b, kipVals).c_str());
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
}

void Tuner::kipGroupPassed() {
    kipStep = KipWrite;
    kipCheckIdx = 0;
    kipFixTries = 0;
    if (kipFinal) {
        kipFinal = false;
        kipDirty = false;
        kipLog.push_back("итоговый набор кипа проверен");
        ev("Кип: итоговый набор проверен после перезагрузки");
        return;
    }
    std::string l = "принято в кип: " + joinKeys(kipTesting, kipVals);
    kipAccepted.insert(kipAccepted.end(), kipTesting.begin(), kipTesting.end());
    kipTesting.clear();
    kipDirty = false;
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
}

const char* Tuner::loosenTierName(int tier) {
    switch (tier) {
        case 1: return "тайминги";
        case 2: return "пропускная способность";
        case 3: return "ручка в кипе (перезагрузка)";
        case 4: return "частота ниже";
    }
    return "нечего";
}

Tuner::LoosenPlan Tuner::nextLoosen(const Candidate& c) const {
    LoosenPlan p;
    p.set = c.set;
    p.hz = c.hz;
    if (c.baseline) return p;
    if (loosenTimingIn(p.set, c.loosened, &p.desc)) {
        p.tier = 1;
        return p;
    }
    for (auto key : kLoosenBw) {
        // Ручку, что лежит в кипе, живьём не отжать: поправка timings.ini лишь повторяет кип.
        if (std::find(kipAccepted.begin(), kipAccepted.end(), key) != kipAccepted.end()) continue;
        if (loosenKeyIn(p.set, key, &p.desc)) {
            p.tier = 2;
            p.key = key;
            return p;
        }
    }
    for (auto key : kLoosenBw)
        if (std::find(kipAccepted.begin(), kipAccepted.end(), key) != kipAccepted.end()) {
            p.tier = 3;
            p.key = key;
            auto it = kipVals.find(key);
            p.desc = strf("%s %d из кипа → как до переноса", key, it != kipVals.end() ? it->second : 0);
            return p;
        }
    // Последнее — частота: ближайшая стабильная ступень ниже, тайминги те же.
    u32 below = 0;
    for (auto& pt : points)
        if (pt.status == 1 && pt.hz < c.hz && pt.hz > stockHz() && pt.hz > below && !hzDiscarded(pt.hz)) below = pt.hz;
    if (below) {
        p.tier = 4;
        p.hz = below;
        p.desc = strf("частота %s→%s МГц", mhzStr(c.hz).c_str(), mhzStr(below).c_str());
    }
    return p;
}

void Tuner::kipDrop(const std::string& key, const std::string& why) {
    auto it = std::find(kipAccepted.begin(), kipAccepted.end(), key);
    if (it == kipAccepted.end()) return;
    kipAccepted.erase(it);
    kipRejected.push_back({key, kipVals[key], why});
    // Поправка частоты повторяла кип — уберём и её, иначе ручка осталась бы живьём.
    for (auto& c : cands) c.set.erase(key);
    kipTesting.clear();
    kipQueue.clear();
    kipDirty = true;
    kipFinal = true;
    kipStep = KipWrite;
    kipCheckIdx = 0;
    kipFixTries = 0;
    phase = Phase::Kip;
    std::string l = strf("снимаю из кипа %s: %s", key.c_str(), why.c_str());
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
}

bool Tuner::stabilize(ITarget& t, int idx, std::string* msg) {
    std::string steps;
    std::string from;  // отжимается копия — из какого кандидата
    for (int guard = 0; guard < 40; guard++) {
        LoosenPlan p;
        Candidate c;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            if (idx < 0 || idx >= (int)cands.size()) {
                *msg = "нет такого кандидата";
                return false;
            }
            c = cands[idx];
            p = nextLoosen(c);
            if (p.tier == 0) {
                *msg = c.name() + ": отжимать больше нечего" + (steps.empty() ? "" : " (сделано: " + steps + ")");
                ev("Отжим: %s", msg->c_str());
                return false;
            }
            if (guard == 0 && (c.attempts > 0 || !c.runs.empty())) {
                // Кандидат уже был в игре: его не переписываем (частота, тайминги и прогоны TOTK остаются как есть),
                // отжимаем копию. Много вылетов — исходный помечается «сомнительный», но не пропадает.
                Candidate n = c;
                int maxId = 0;
                for (auto& x : cands) maxId = std::max(maxId, x.id);
                n.id = maxId + 1;
                n.runs.clear();
                n.crashes = n.attempts = n.stutters = n.artifacts = n.longLoads = 0;
                n.game = 0;
                n.optOffered = false;
                n.note = "отжат из " + c.name();
                from = c.name();
                cands.push_back(n);
                if (gameActiveCand == idx) gameActiveCand = -1;
                if (gamePendingCand == idx) gamePendingCand = -1;
                ev("Отжим %s: исходный остаётся в списке%s, отжимается копия К%d", c.name().c_str(),
                   c.doubtful() ? strf(" как сомнительный (сбоев %d из %d)", c.crashes, std::max(c.attempts, c.crashes)).c_str() : "", n.id);
                idx = (int)cands.size() - 1;
                c = cands[idx];
            }
            if (p.tier == 3) {
                kipDrop(p.key, c.name() + " нестабилен в TOTK, тайминги и живые ручки уже отжаты");
                cands[idx].loosened++;
                cands[idx].note = (from.empty() ? std::string("отжат: ") : "отжат из " + from + ": ") + (steps.empty() ? std::string() : steps + "; ") + p.desc;
                *msg = c.name() + ": " + p.desc + " — нужна перезагрузка, подбор продолжится сам";
                std::string snap = serialize();
                t.saveText(paths::state(), snap);
                return true;
            }
            ev("Отжим %s, шаг %d (%s): %s", c.name().c_str(), c.loosened + 1, loosenTierName(p.tier), p.desc.c_str());
        }
        steps += (steps.empty() ? "" : ", ") + p.desc;
        TrialSpec s;
        s.kind = TrialSpec::Verify;
        s.hz = p.hz;
        s.set = p.set;
        s.stabSec = cfg.verifySec;
        s.passes = cfg.benchPasses + 1;
        s.desc = "Отжим " + c.name();
        TrialOut out = runTrial(t, s, nullptr);
        std::lock_guard<std::recursive_mutex> lk(mtx);
        Candidate& cc = cands[idx];
        cc.set = p.set;
        cc.hz = p.hz;
        cc.loosened++;
        if (!out.ok) {
            ev("Отжим %s: после «%s» всё ещё нестабильно (%s) — следующий шаг", cc.name().c_str(), p.desc.c_str(), out.reason.c_str());
            continue;
        }
        cc.b = out.b;
        cc.synth = out.score;
        cc.game = 0;
        cc.note = (from.empty() ? std::string("отжат: ") : "отжат из " + from + ": ") + steps;
        if (gameActiveCand == idx) gameActiveCand = -1;
        *msg = strf("%s отжат (%s): прошёл memtest %d с, поток %.0f МБ/с. Проверьте его в TOTK снова", cc.name().c_str(), steps.c_str(), cfg.verifySec,
                    out.b.bw());
        ev("Отжим: %s", msg->c_str());
        computeRanking();
        std::string snap = serialize();
        t.saveText(paths::state(), snap);
        return true;
    }
    *msg = "отжим: слишком много шагов";
    return false;
}

void Tuner::kipFinish() {
    kipStep = KipWrite;
    std::string l = strf("перенос в кип закончен: принято %zu (%s), отклонено %zu, перезагрузок %d", kipAccepted.size(),
                         joinKeys(kipAccepted, kipVals).c_str(), kipRejected.size(), kipReboots);
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
    enterGame();
}

bool Tuner::kipAdvance(ITarget& t) {
    std::unique_lock<std::recursive_mutex> lk(mtx);
    if (kipStep == KipBoot) {
        // Другая загрузка — другой момент загрузки. Сравниваем с запасом: эпоха и время с загрузки округляются до секунд.
        s64 bootNow = t.bootEpoch();
        bool rebooted = std::llabs(bootNow - (s64)kipUptimeAt) > 5;
        if (!rebooted) {
            ev("Кип: перезагрузки после записи не было — записываю и перезагружаю снова");
            kipReboots = std::max(0, kipReboots - 1);
            kipStep = KipWrite;
        } else {
            std::map<int, int> now;
            std::string err;
            lk.unlock();
            bool okRead = t.kipRead(&now, &err);
            lk.lock();
            if (!okRead) {
                phase = Phase::Failed;
                error = "кип: не удалось прочитать настройки после загрузки — " + err;
                ev("%s", error.c_str());
                std::string snap = serialize();
                lk.unlock();
                t.saveText(paths::state(), snap);
                return false;
            }
            std::vector<std::string> lost;
            for (auto& kv : kipDesired(kipTesting)) {
                auto it = now.find(kv.first);
                if (it == now.end() || it->second != kv.second)
                    for (auto& vk : kipVals)
                        if (kipSlotOf(vk.first) == kv.first) lost.push_back(vk.first);
            }
            if (!lost.empty()) {
                // Демон на загрузке читает настройки из кипа; наших значений там нет — сторож откатил правку,
                // значит консоль с ними зависла (или не дожила до штатной работы демона).
                kipDirty = true;
                kipHangs++;
                if (kipHangs >= kipHangLimit()) {
                    kipAbort(strf("%d зависания за этап", kipHangs));
                    std::string snap = serialize();
                    lk.unlock();
                    t.saveText(paths::state(), snap);
                    return true;
                }
                kipGroupFailed(strf("после перезагрузки %d значений нет в кипе (%s) — сторож Ryazha-clk откатил правку: консоль зависла на загрузке",
                                    kipReboots, joinKeys(lost, kipVals).c_str()));
            } else {
                kipStep = KipCheckRun;
                kipCheckIdx = 0;
                std::string l = strf("загрузка %d прошла: в кипе %s", kipReboots,
                                     kipFinal ? "итоговый набор" : joinKeys(kipTesting.empty() ? kipAccepted : kipTesting, kipVals).c_str());
                kipLog.push_back(l);
                ev("Кип: %s", l.c_str());
            }
            std::string snap = serialize();
            lk.unlock();
            t.saveText(paths::state(), snap);
            return true;
        }
    }

    // Запись в кип и перезагрузка.
    if (kipOrig.empty()) {
        int wd = t.watchdogState();
        if (wd == 0 || wd == kWatchdogNone) {
            // Без сторожа зависание после записи не откатит правку: консоль загрузится с тем же кипом и повиснет снова.
            if (wd == kWatchdogNone)
                ev("Кип: в этой версии Ryazha-clk сторожа нет — перенос в кип пропущен, ручки остаются живыми в timings.ini");
            else
                ev("Кип: сторож Ryazha-clk выключен — перенос в кип пропущен. Включите сторож (оверлей Ryazha-clk → Безопасность → Сторож) "
                   "и запустите подбор снова, чтобы перенести ручки в кип");
            enterGame();
            std::string snap = serialize();
            bool more = isAuto();  // дальше может идти финиш tREFI × tRFC
            lk.unlock();
            t.saveText(paths::state(), snap);
            return more;
        }
        if (wd < 0) ev("Кип: состояние сторожа Ryazha-clk неизвестно — продолжаю, но без сторожа зависание кип не откатит");
        std::map<int, int> orig;
        std::string err;
        lk.unlock();
        bool okRead = t.kipRead(&orig, &err);
        lk.lock();
        if (!okRead) {
            ev("Кип недоступен (%s) — перенос пропущен", err.c_str());
            enterGame();
            std::string snap = serialize();
            lk.unlock();
            t.saveText(paths::state(), snap);
            return false;
        }
        for (auto& kv : kipVals) {
            int slot = kipSlotOf(kv.first);
            kipOrig[slot] = orig.count(slot) ? orig[slot] : 0;
        }
        // Копия исходного кипа — отдельным файлом: её не сотрёт и новый подбор («Вернуть как было»).
        std::string backup;
        if (!t.loadText(paths::kipBackup(), backup) || backup.empty()) {
            for (auto& kv : orig) backup += strf("%d:%d\n", kv.first, kv.second);
            t.saveText(paths::kipBackup(), backup);
        }
        // Что уже стоит в кипе, переносить незачем (в том числе нули стока Ryazha-clk в наборе кандидата).
        for (auto it = kipVals.begin(); it != kipVals.end();) {
            if (kipOrig[kipSlotOf(it->first)] == it->second)
                it = kipVals.erase(it);
            else
                ++it;
        }
        for (auto& g : kipQueue)
            g.erase(std::remove_if(g.begin(), g.end(), [&](const std::string& k) { return !kipVals.count(k); }), g.end());
        ev("Кип до переноса: %s", [&] {
            std::string o;
            for (auto& kv : kipVals) o += strf("%s%s=%d", o.empty() ? "" : ", ", kv.first.c_str(), kipOrig[kipSlotOf(kv.first)]);
            return o;
        }().c_str());
    }
    if (kipTesting.empty() && !kipFinal) {
        while (!kipQueue.empty() && kipQueue.front().empty()) kipQueue.erase(kipQueue.begin());
        if (kipQueue.empty()) {
            if (!kipDirty) {
                kipFinish();
                std::string snap = serialize();
                bool more = isAuto();  // за кипом может идти финиш tREFI × tRFC — тогда подбор продолжается (1.9.4 тут вставал)
                lk.unlock();
                t.saveText(paths::state(), snap);
                return more;
            }
            kipFinal = true;
        } else {
            kipTesting = kipQueue.front();
            kipQueue.erase(kipQueue.begin());
        }
    }
    std::map<int, int> want = kipDesired(kipTesting);
    std::string what = kipFinal ? "итоговый набор (" + joinKeys(kipAccepted, kipVals) + ")" : joinKeys(kipTesting, kipVals);
    std::string err;
    lk.unlock();
    bool okWrite = t.kipWrite(want, &err);
    lk.lock();
    if (!okWrite) {
        phase = Phase::Failed;
        error = "кип: демон не принял запись — " + err;
        ev("%s", error.c_str());
        std::string snap = serialize();
        lk.unlock();
        t.saveText(paths::state(), snap);
        return false;
    }
    kipDirty = !kipTesting.empty();
    kipReboots++;
    kipStep = KipBoot;
    kipCheckIdx = 0;
    kipRebootAt = epochNow();
    kipUptimeAt = (u64)t.bootEpoch();
    status = strf("Перезагрузка %d: в кипе %s. После загрузки снова откройте RamSweetSpot", kipReboots, what.c_str());
    std::string l = strf("записано в кип: %s — перезагрузка %d", what.c_str(), kipReboots);
    kipLog.push_back(l);
    ev("Кип: %s", l.c_str());
    std::string snap = serialize();
    lk.unlock();
    t.saveText(paths::state(), snap);
    // Сторож Ryazha-clk раз в секунду запоминает правку: после зависания он откатит ровно её.
    t.idle(4000);
    if (!t.reboot(&err)) {
        std::lock_guard<std::recursive_mutex> lk2(mtx);
        status = "Не удалось перезагрузить (" + err + "). Перезагрузите консоль через меню питания и снова откройте RamSweetSpot";
        ev("Кип: %s", status.c_str());
        std::string snap2 = serialize();
        t.saveText(paths::state(), snap2);
    }
    return false;
}

double Tuner::rfThr() const { return maxMode() ? kMaxGain : eps; }

void Tuner::enterRefresh() {
    rfQueue.clear();
    for (size_t i = 0; i < cands.size(); i++)
        if (!cands[i].baseline && cands[i].set.count("tREFI") && cands[i].set.count("tRFC")) rfQueue.push_back((int)i);
    rfStage = rfPos = 0;
    rfPairs.clear();
    rfStack.clear();
    rfBad.clear();
    confirmPending = false;
    if (rfQueue.empty()) {
        rfDone = true;
        return;
    }
    phase = Phase::Refresh;
    ev("Финиш (эксперимент): tREFI и tRFC вместе на итоговом кипе — кандидатов %zu. Другая пара — только с приростом от %.2f %% по "
       "замеру А/Б и после длинной проверки",
       rfQueue.size(), rfThr() * 100);
}

void Tuner::rfNext() {
    if (!rfQueue.empty()) rfQueue.erase(rfQueue.begin());
    rfStage = rfPos = 0;
    rfPairs.clear();
    rfStack.clear();
    rfBad.clear();
    rfPickScore = 0;
    confirmPending = false;
}

std::vector<Tuner::RfPair> Tuner::refreshPairs(u32 hz, RfPair start) const {
    const KnobDef* dr = findKnob("tREFI");
    const KnobDef* df = findKnob("tRFC");
    std::vector<RfPair> out;
    if (!dr || !df) return out;
    int rCap = std::min(dr->max, 4), fCap = df->max;
    for (auto& pi : plan) {
        if (pi.key == "tREFI") rCap = std::min(rCap, pi.capMax);
        if (pi.key == "tRFC") fCap = std::min(fCap, pi.capMax);
    }
    // Туже первого сбойного значения этой частоты (реестр нестабильных) — нельзя. Само сбойное — можно ещё раз, если
    // ручки уже в кипе: с обучением памяти на загрузке частота держит больше, чем живьём, где этот предел нашёлся.
    // Это — только в «максимуме»; в безопасном и среднем (финиш там включают вручную) — с запасом режима от сбоя.
    bool retry = !kipAccepted.empty();
    int back = maxMode() ? (retry ? 0 : 1) : std::max(1, cfg.margin);
    int b = 0;
    if (unstableBound(hz, "tREFI", &b)) rCap = std::min(rCap, b - back);
    if (unstableBound(hz, "tRFC", &b)) fCap = std::min(fCap, b - back);
    double s0 = refreshShare(start);
    for (int r = std::max(dr->min, start.first - 1); r <= rCap; r++)
        for (int f = std::max(df->min, start.second - 3); f <= fCap; f++) {
            RfPair p{r, f};
            if (p != start && refreshShare(p) < s0) out.push_back(p);
        }
    // Ближние к исходной доле регенерации — первыми: меньше риск, а дальше идём, только пока есть прирост.
    std::sort(out.begin(), out.end(), [](const RfPair& a, const RfPair& b) { return refreshShare(a) > refreshShare(b); });
    return out;
}

bool Tuner::rfSkip(const RfPair& p) const {
    if (refreshShare(p) >= refreshShare(rfPick)) return true;  // не короче выбранной — прироста не даст
    for (auto& b : rfBad)
        if (p.first >= b.first && p.second >= b.second) return true;  // туже сбойной по обоим
    return false;
}

bool Tuner::refreshSync(ITarget& t, u32 hz, KnobSet& set, std::string* steps) {
    if (!cfg.refreshTune || !set.count("tREFI") || !set.count("tRFC")) return false;
    RfPair start = pairIn(set), pick = start;
    std::vector<RfPair> pairs, bad;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        pairs = refreshPairs(hz, start);
        ev("Финиш (эксперимент): %s — пар на пробу %zu", pairStr(start).c_str(), pairs.size());
    }
    if (pairs.empty()) return false;
    TrialSpec s0;
    s0.kind = TrialSpec::RefTune;
    s0.hz = hz;
    s0.set = set;
    s0.key = "tREFI";
    s0.val = pairCode(start);
    s0.passes = std::max(cfg.benchPasses, cfg.timingPasses);
    s0.desc = "Финиш: эталон (" + pairStr(start) + ")";
    TrialOut o0 = runTrial(t, s0, nullptr);
    if (!o0.ok) return false;
    double pickScore = o0.score;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        addNoise(o0.b);
    }
    for (auto& p : pairs) {
        TrialSpec q;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            bool skip = refreshShare(p) >= refreshShare(pick);
            for (auto& b : bad)
                if (p.first >= b.first && p.second >= b.second) skip = true;
            if (skip) continue;
        }
        TrialSpec s;
        s.kind = TrialSpec::RefTune;
        s.hz = hz;
        s.set = withPair(set, p);
        s.key = "tREFI";
        s.val = pairCode(p);
        s.stabSec = cfg.quickSec;
        s.passes = std::max(cfg.benchPasses, cfg.timingPasses);
        s.desc = "Финиш: " + pairStr(p);
        TrialOut out = runTrial(t, s, nullptr);
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            if (!out.ok) {
                noteUnstable(hz, failKind(out.reason), "", 0, s.set, "финиш: " + out.reason);
                bad.push_back(p);
                ev("Финиш: %s — сбой (%s)", pairStr(p).c_str(), out.reason.c_str());
                continue;
            }
            addNoise(out.b);
            double dlt = out.score / pickScore - 1, sd = std::sqrt(2.0) * trialSigma(s.passes);
            ev("Финиш: %s — %+.2f %% к %s", pairStr(p).c_str(), dlt * 100, pairStr(pick).c_str());
            if (dlt < rfThr() - kScreenK * sd) continue;
            q.kind = TrialSpec::Confirm;
            q.hz = hz;
            q.ref = withPair(set, pick);
            q.set = s.set;
            q.key = "tREFI";
            q.val = s.val;
            q.passes = kPairPasses;
            q.thr = rfThr();
            q.desc = "Финиш: " + pairStr(p) + " — подтверждение А/Б против " + pairStr(pick);
        }
        TrialOut pr = runTrial(t, q, nullptr);
        std::lock_guard<std::recursive_mutex> lk(mtx);
        for (auto& eb : pr.extra) addNoise(eb);
        if (!pr.ok) {
            if (!pr.refFailed && !pr.aborted) {
                noteUnstable(hz, failKind(pr.reason), "", 0, q.set, "финиш: " + pr.reason);
                bad.push_back(p);
            }
            continue;
        }
        bool acc = pr.delta >= rfThr();
        ev("Финиш: %s против %s — А/Б, блоков %d: %+.3f %% ± %.3f %% — %s", pairStr(p).c_str(), pairStr(pick).c_str(), pr.blocks, pr.delta * 100,
           pr.sigma * 100, acc ? "лучше" : "не лучше");
        if (acc) pick = p;
        pickScore = acc ? pr.score : pr.refScore;
    }
    if (pick == start) return false;
    set = withPair(set, pick);
    *steps += (steps->empty() ? "" : ", ") + strf("финиш: tREFI %d→%d, tRFC %d→%d", start.first, pick.first, start.second, pick.second);
    return true;
}

void Tuner::enterGame() {
    if (cfg.refreshTune && !rfDone) {
        enterRefresh();
        if (phase == Phase::Refresh) return;
    }
    bool haveBase = false;
    for (auto& c : cands)
        if (c.baseline) haveBase = true;
    if (!haveBase) {
        Candidate c;
        c.id = 0;
        c.baseline = true;
        cands.insert(cands.begin(), c);
    }
    // Кандидаты — по синтетической оценке.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.baseline != b.baseline) return a.baseline;
        return a.synth > b.synth;
    });
    int real = 0;
    for (auto& c : cands)
        if (!c.baseline) real++;
    if (real == 0) {
        phase = Phase::Failed;
        error = "ни одна частота не прошла длинную проверку";
        ev("%s", error.c_str());
        return;
    }
    phase = Phase::Game;
    status = "Синтетика готова — проверьте кандидатов в TOTK";
    ev("Синтетический подбор закончен: кандидатов %d. Дальше — бенчмарк в лесу короков.", real);
}

// ============================================================== журнал

bool Tuner::saveState(ITarget& t) {
    std::string s;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        s = serialize();
    }
    return t.saveText(paths::state(), s);
}

void Tuner::histSeed() const {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    s64 at = startedEpoch ? startedEpoch : epochNow();
    for (auto& p : points) {
        HistRow h;
        h.at = at;
        h.ver = APP_VER;
        h.dram = chip.dramId;
        h.mhz = p.hz / 1e6;
        h.kind = kfDone ? "scan:kip" : (p.pass ? "scan:guide" : "scan:stock");
        h.status = p.status == 1 ? "ok" : (p.status == 3 ? "slow" : (failKind(p.reason) == "зависание" ? "hang" : "fail"));
        if (p.b.valid && p.status != 2) {
            h.bw = p.b.bw();
            h.lat = p.b.latNs;
        }
        histAppend(h);
    }
    for (auto& c : cands) {
        if (!c.baseline && c.b.valid) {
            HistRow h;
            h.at = at;
            h.ver = APP_VER;
            h.dram = chip.dramId;
            h.mhz = c.hz / 1e6;
            h.kind = "tuned";
            h.status = "ok";
            h.bw = c.b.bw();
            h.lat = c.b.latNs;
            h.set = knobSetCompact(c.set);
            histAppend(h);
        }
        for (auto& r : c.runs) {
            HistRow h;
            h.at = at;
            h.ver = APP_VER;
            h.dram = chip.dramId;
            h.mhz = c.baseline ? 0 : c.hz / 1e6;
            h.kind = "totk";
            h.status = "ok";
            h.frames = r.frames;
            h.fps = r.avg;
            h.low1 = r.low1;
            h.low01 = r.low01;
            h.set = c.baseline ? std::string("как было") : knobSetCompact(c.set);
            histAppend(h);
        }
    }
}

void Tuner::histTrial(const TrialSpec& spec, const TrialOut& out, const std::string& kind) const {
    if (out.aborted) return;
    HistRow h;
    h.ver = APP_VER;
    h.dram = chip.dramId;
    h.mhz = spec.hz / 1e6;
    h.kind = kind;
    h.status = out.ok ? "ok" : (failKind(out.reason) == "зависание" ? "hang" : "fail");
    if (out.ok && out.b.valid) {
        h.bw = out.b.bw();
        h.lat = out.b.latNs;
    }
    h.set = knobSetCompact(spec.set);
    histAppend(h);
}

bool Tuner::loadState(ITarget& t, bool* recoveredHang) {
    if (recoveredHang) *recoveredHang = false;
    std::string text;
    if (!t.loadText(paths::state(), text) || text.empty()) return false;
    std::lock_guard<std::recursive_mutex> lk(mtx);
    loadUnstable(t);
    if (!deserialize(text)) {
        LOGE("Журнал подбора %s повреждён — копия сохранена в state.bad.txt, начинаю с чистого листа", paths::state().c_str());
        writeFile(paths::appDir() + "state.bad.txt", text);
        resetAll();
        return false;
    }
    LOGI("Журнал подбора загружен: этап «%s», проб %d, кандидатов %zu, ждём TOTK: %d, активен: %d", phaseName(phase), trials, cands.size(),
         gamePendingCand, gameActiveCand);
    if (pending.valid()) {
        // Проба не завершилась. Что случилось — по моменту загрузки консоли и отчётам о падениях.
        TrialSpec spec = pending;
        pending = TrialSpec();
        bool rebooted = !pendingBoot || std::llabs(t.bootEpoch() - pendingBoot) > 5;  // журнал прежней версии — как раньше
        std::string crash;
        for (auto& f : crashReportNames())
            if (!std::binary_search(pendingCrash.begin(), pendingCrash.end(), f)) crash = f;
        pendingBoot = 0;
        pendingCrash.clear();
        if (rebooted || !crash.empty()) {
            TrialOut out;
            out.ok = false;
            out.reason = rebooted ? "зависание / перезагрузка во время пробы" : "вылет приложения во время пробы (" + crash + ")";
            ev("Обнаружена прерванная проба: %s — %s", spec.desc.c_str(), rebooted ? "консоль перезагружалась" : "отчёт о падении");
            handleResult(spec, out);
            if (recoveredHang) *recoveredHang = true;
        } else {
            // Консоль не перезагружалась, отчёта о падении нет: приложение закрыли (из HOME, запуском игры) посреди пробы.
            // Это не сбой — проба повторится при продолжении.
            if (trials > 0) trials--;
            ev("Проба «%s» прервана выходом из приложения (без перезагрузки и отчёта о падении) — не сбой, повторю её", spec.desc.c_str());
        }
        flushUnstable(t);
        std::string snap = serialize();
        t.saveText(paths::state(), snap);
    }
    return true;
}

std::string Tuner::serialize() const {
    std::string o;
    o += "v=1\n";
    o += strf("phase=%d\n", (int)phase);
    o += "status=" + clean(status) + "\n";
    o += "error=" + clean(error) + "\n";
    o += strf("trials=%d\n", trials);
    o += strf("started=%lld\n", (long long)startedEpoch);
    o += strf("dram=%d\n", chip.dramId);
    {
        std::string l;
        for (u32 hz : ladder) l += (l.empty() ? "" : ",") + strf("%u", hz);
        o += "ladder=" + l + "\n";
    }
    o += "base=" + base.toString() + "\n";
    o += strf("eps=%.6f\n", eps);
    o += strf("noise=%.9g,%d\n", noiseSS, noiseDf);
    o += strf("needref=%d\n", needRef ? 1 : 0);
    o += strf("benchv=%d\n", benchVer);
    o += strf("rf=%d,%d,%d,%d,%d,%d,%d,%.6f\n", rfDone ? 1 : 0, rfStage, rfPos, rfStart.first, rfStart.second, rfPick.first, rfPick.second,
              rfPickScore);
    {
        std::string q;
        for (int i : rfQueue) q += (q.empty() ? "" : ",") + strf("%d", i);
        o += "rfq=" + (q.empty() ? std::string("-") : q) + "\n";
    }
    o += "rfp=" + pairsToStr(rfPairs) + "\n";
    o += "rfs=" + pairsToStr(rfStack) + "\n";
    o += "rfb=" + pairsToStr(rfBad) + "\n";
    for (auto& b : baseRuns) o += "baserun=" + b.toString() + "\n";
    {
        std::string l;
        for (int i : coarse) l += (l.empty() ? "" : ",") + strf("%d", i);
        o += "coarse=" + l + "\n";
    }
    o += strf("sweep=%d,%d,%d,%.3f,%d\n", scanPos, lastOk, failRun, lastOkBw, hangRun);
    o += strf("sweep2=%d,%d,%d,%u\n", scanPass, stockLastOk, guideChosen ? 1 : 0, freqCap);
    o += strf("single=%u\n", singleHz);
    o += strf("kipfirst=%d,%llu,%d,%d\n", kfStep, (unsigned long long)kfBootAt, kfTried ? 1 : 0, kfDone ? 1 : 0);
    o += "kfvals=" + knobSetToString(kfVals) + "\n";
    o += "guideset=" + knobSetToString(guideSet) + "\n";
    for (auto& p : points)
        o += strf("pt=%u|%d|%s|%.6f|%s|%d\n", p.hz, p.status, p.b.toString().c_str(), p.score, clean(p.reason).c_str(), p.pass);
    {
        std::string l;
        for (u32 hz : targets) l += (l.empty() ? "" : ",") + strf("%u", hz);
        o += "targets=" + l + "\n";
    }
    o += strf("tgt=%d,%d,%.6f\n", tIdx, seedStage, inhScore);
    o += "limits=" + knobSetToString(limits) + "\n";
    o += "abslimits=" + knobSetToString(absLimits) + "\n";
    {
        std::string l, v;
        for (auto& k : optKeys) l += (l.empty() ? "" : ",") + k;
        for (int x : optVals) v += (v.empty() ? "" : ",") + strf("%d", x);
        o += "optkeys=" + l + "\n";
        o += "optvals=" + v + "\n";
        o += strf("opt=%d,%d,%d,%d,%d,%d,%.3f,%.6f,%.6f,%.6f\n", optKey, optPos, optPass, optHaveBest ? 1 : 0, optChanged ? 1 : 0, optBestVal,
                  optBestBw, optBestScore, optMaxScore, optLoss);
        o += "optstart=" + knobSetToString(optStart) + "\n";
        o += strf("optguard=%d,%.6f,%.6f\n", optGuard, optGuardA, optGuardB);
    }
    o += "preset=" + knobSetToString(presetSet) + "\n";
    {
        std::string l;
        for (auto& p : plan) l += (l.empty() ? "" : ",") + strf("%s:%d:%d", p.key.c_str(), p.capMax, p.capMin);
        o += "plan=" + l + "\n";
    }
    o += "cur=" + knobSetToString(cur) + "\n";
    o += strf("tim=%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d\n", curScore, kIdx, seqStart, seqStartInh ? 1 : 0, seqPos, haveBest ? 1 : 0,
              bestVal, bestScore, startScore, noimp, haveFail ? 1 : 0, failVal, remeasure ? 1 : 0, remeasureFails, verifyTries, loosenRound);
    o += strf("conf=%d,%.6f,%d,%d\n", confirmPending ? 1 : 0, confirmScore, chainAB ? 1 : 0, chainEnd ? 1 : 0);
    for (auto& a : accepted) o += "acc=" + a.key + "|" + a.prev + "|" + (a.edge ? "1" : "0") + "\n";
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        o += strf("cand=%d|%d|%u|%s|%s|%.6f|%d|%s|%d|%d|%d|%d|%d|%d|%d\n", c.id, c.baseline ? 1 : 0, c.hz, knobSetToString(c.set).c_str(),
                  c.b.toString().c_str(), c.synth, c.game, clean(c.note).c_str(), c.crashes, c.attempts, c.loosened, c.stutters, c.artifacts,
                  c.longLoads, c.optOffered ? 1 : 0);
        for (auto& r : c.runs) o += strf("run=%zu#", i) + r.toString() + "\n";
    }
    o += strf("gpend=%d\n", gamePendingCand);
    o += strf("gactive=%d\n", gameActiveCand);
    o += "gsnap=" + gameSnapshot + "\n";
    o += strf("kip=%d,%d,%d,%d,%d,%lld,%llu,%d,%d\n", kipStep, kipSrc, kipFinal ? 1 : 0, kipDirty ? 1 : 0, kipReboots, (long long)kipRebootAt,
              (unsigned long long)kipUptimeAt, kipCheckIdx, kipFixTries);
    o += "kipvals=" + knobSetToString(kipVals) + "\n";
    o += strf("kipfix=%d|", kipFixCand) + knobSetToString(kipFixBackup) + "\n";
    {
        std::string l;
        for (auto& pr : kipPre) l += (l.empty() ? "" : ",") + strf("%d:", pr.first) + pr.second;
        o += strf("kippre=%d,%d|", kipPrePos, kipHangs) + l + "\n";
    }
    {
        std::string l;
        for (auto& kv : kipOrig) l += (l.empty() ? "" : ",") + strf("%d:%d", kv.first, kv.second);
        o += "kiporig=" + l + "\n";
    }
    auto keys = [](const std::vector<std::string>& v) {
        std::string l;
        for (auto& k : v) l += (l.empty() ? "" : ",") + k;
        return l;
    };
    o += "kipacc=" + keys(kipAccepted) + "\n";
    o += "kiptest=" + keys(kipTesting) + "\n";
    for (auto& g : kipQueue)
        if (!g.empty()) o += "kipq=" + keys(g) + "\n";
    for (auto& r : kipRejected) o += strf("kiprej=%s|%d|%s\n", r.key.c_str(), r.val, clean(r.reason).c_str());
    for (size_t i = kipLog.size() > 60 ? kipLog.size() - 60 : 0; i < kipLog.size(); i++) o += "kiplog=" + clean(kipLog[i]) + "\n";
    o += "pending=" + (pending.valid() ? pending.serialize() : std::string("-")) + "\n";
    if (pending.valid()) {
        std::string l;
        for (auto& f : pendingCrash) l += (l.empty() ? "" : ";") + f;
        o += strf("pendboot=%lld\n", (long long)pendingBoot);
        o += "pendcrash=" + l + "\n";
    }
    for (auto& e : events) o += "ev=" + e + "\n";
    return o;
}

bool Tuner::deserialize(const std::string& text) {
    Settings keep = cfg;
    std::vector<u32> keepLadder = ladder;
    ChipInfo keepChip = chip;
    resetAll();
    cfg = keep;
    chip = keepChip;
    ladder.clear();
    benchVer = 1;  // журнал до 1.9.4 — без версии замера
    bool sawVersion = false;
    bool oldScan = false;
    for (auto& raw : split(text, '\n')) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "v") sawVersion = v == "1";
        else if (k == "phase") phase = (Phase)toLong(v, 0);
        else if (k == "status") status = v;
        else if (k == "error") error = v;
        else if (k == "trials") trials = (int)toLong(v, 0);
        else if (k == "started") startedEpoch = (s64)strtoll(v.c_str(), nullptr, 10);
        else if (k == "dram") chip.dramId = (int)toLong(v, chip.dramId);
        else if (k == "ladder") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) ladder.push_back((u32)strtoul(x.c_str(), nullptr, 10));
        } else if (k == "base") base.fromString(v);
        else if (k == "eps") eps = toDouble(v, 0.005);
        else if (k == "noise") {
            auto p = split(v, ',');
            noiseSS = p.size() >= 2 ? toDouble(p[0], 0) : 0;
            noiseDf = p.size() >= 2 ? (int)toLong(p[1], 0) : 0;
        } else if (k == "benchv") {
            benchVer = (int)toLong(v, 1);
        } else if (k == "needref") {
            needRef = v == "1";
        } else if (k == "rf") {
            auto p = split(v, ',');
            if (p.size() >= 8) {
                rfDone = p[0] == "1";
                rfStage = (int)toLong(p[1], 0);
                rfPos = (int)toLong(p[2], 0);
                rfStart = {(int)toLong(p[3], 0), (int)toLong(p[4], 0)};
                rfPick = {(int)toLong(p[5], 0), (int)toLong(p[6], 0)};
                rfPickScore = toDouble(p[7], 0);
            }
        } else if (k == "rfq") {
            rfQueue.clear();
            for (auto& x : split(v, ','))
                if (trim(x) != "-" && !trim(x).empty()) rfQueue.push_back((int)toLong(trim(x), 0));
        } else if (k == "rfp") {
            rfPairs = pairsFromStr(v);
        } else if (k == "rfs") {
            rfStack = pairsFromStr(v);
        } else if (k == "rfb") {
            rfBad = pairsFromStr(v);
        }
        else if (k == "baserun") {
            BenchResult b;
            if (b.fromString(v) && b.valid) baseRuns.push_back(b);
        } else if (k == "coarse") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) coarse.push_back((int)toLong(x, 0));
        } else if (k == "sweep2") {
            auto p = split(v, ',');
            if (p.size() >= 3) {
                scanPass = (int)toLong(p[0], 0);
                stockLastOk = (int)toLong(p[1], -1);
                guideChosen = p[2] == "1";
                freqCap = p.size() >= 4 ? (u32)strtoul(p[3].c_str(), nullptr, 10) : 0;
            }
        } else if (k == "guideset") knobSetFromString(v, guideSet);
        else if (k == "single") singleHz = (u32)strtoul(v.c_str(), nullptr, 10);
        else if (k == "kipfirst") {
            auto p = split(v, ',');
            if (p.size() >= 4) {
                kfStep = (int)toLong(p[0], 0);
                kfBootAt = (u64)strtoull(p[1].c_str(), nullptr, 10);
                kfTried = p[2] == "1";
                kfDone = p[3] == "1";
            }
        } else if (k == "kfvals") knobSetFromString(v, kfVals);
        else if (k == "sweep") {
            auto p = split(v, ',');
            if (p.size() >= 4) {
                scanPos = (int)toLong(p[0], 0);
                lastOk = (int)toLong(p[1], -1);
                failRun = (int)toLong(p[2], 0);
                lastOkBw = toDouble(p[3], 0);
                hangRun = p.size() >= 5 ? (int)toLong(p[4], 0) : 0;
            }
        } else if (k == "scan") {
            // Журнал 1.1.0 (грубый + точный поиск): берём только последнюю стабильную ступень.
            auto p = split(v, ',');
            if (p.size() == 6) {
                lastOk = (int)toLong(p[3], -1);
                lastOkBw = toDouble(p[5], 0);
                oldScan = true;
            }
        } else if (k == "pt") {
            auto p = split(v, '|');
            if (p.size() >= 5) {
                FreqPoint fp;
                fp.hz = (u32)strtoul(p[0].c_str(), nullptr, 10);
                fp.status = (int)toLong(p[1], 0);
                fp.b.fromString(p[2]);
                fp.score = toDouble(p[3], 0);
                fp.reason = p[4];
                fp.pass = p.size() >= 6 ? (int)toLong(p[5], 0) : 0;
                points.push_back(fp);
            }
        } else if (k == "targets") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) targets.push_back((u32)strtoul(x.c_str(), nullptr, 10));
        } else if (k == "tgt") {
            auto p = split(v, ',');
            if (p.size() == 3) {
                tIdx = (int)toLong(p[0], 0);
                seedStage = (int)toLong(p[1], 0);
                inhScore = toDouble(p[2], 0);
            }
        } else if (k == "preset") knobSetFromString(v, presetSet);
        else if (k == "limits") knobSetFromString(v, limits);
        else if (k == "abslimits") knobSetFromString(v, absLimits);
        else if (k == "optkeys") {
            optKeys.clear();
            for (auto& x : split(v, ','))
                if (findKnob(trim(x))) optKeys.push_back(trim(x));
        } else if (k == "optvals") {
            optVals.clear();
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) optVals.push_back((int)toLong(x, 0));
        } else if (k == "opt") {
            auto p = split(v, ',');
            if (p.size() >= 7) {
                optKey = (int)toLong(p[0], 0);
                optPos = (int)toLong(p[1], 0);
                optPass = (int)toLong(p[2], 0);
                optHaveBest = p[3] == "1";
                optChanged = p[4] == "1";
                optBestVal = (int)toLong(p[5], 0);
                optBestBw = toDouble(p[6], 0);
                optBestScore = p.size() >= 8 ? toDouble(p[7], 0) : 0;
                optMaxScore = p.size() >= 9 ? toDouble(p[8], 0) : optBestScore;
                optLoss = p.size() >= 10 ? toDouble(p[9], 0) : 0;
            }
        } else if (k == "optstart") {
            knobSetFromString(v, optStart);
        } else if (k == "optguard") {
            auto p = split(v, ',');
            if (p.size() >= 3) {
                optGuard = (int)toLong(p[0], 0);
                optGuardA = toDouble(p[1], 0);
                optGuardB = toDouble(p[2], 0);
            }
        }
        else if (k == "plan") {
            for (auto& x : split(v, ',')) {
                auto q = split(x, ':');
                if (q.size() == 3 && findKnob(q[0])) plan.push_back({q[0], (int)toLong(q[1], 0), (int)toLong(q[2], 0)});
            }
        } else if (k == "cur") knobSetFromString(v, cur);
        else if (k == "tim") {
            auto p = split(v, ',');
            if (p.size() >= 15) {
                curScore = toDouble(p[0], 0);
                kIdx = (int)toLong(p[1], 0);
                seqStart = (int)toLong(p[2], 0);
                seqStartInh = p[3] == "1";
                seqPos = (int)toLong(p[4], 0);
                haveBest = p[5] == "1";
                bestVal = (int)toLong(p[6], 0);
                bestScore = toDouble(p[7], 0);
                startScore = toDouble(p[8], 0);
                noimp = (int)toLong(p[9], 0);
                haveFail = p[10] == "1";
                failVal = (int)toLong(p[11], 0);
                remeasure = p[12] == "1";
                remeasureFails = (int)toLong(p[13], 0);
                verifyTries = (int)toLong(p[14], 0);
                loosenRound = p.size() >= 16 ? (int)toLong(p[15], 0) : 0;
            }
        } else if (k == "conf") {
            auto p = split(v, ',');
            confirmPending = p.size() >= 2 && p[0] == "1";
            confirmScore = p.size() >= 2 ? toDouble(p[1], 0) : 0;
            chainAB = p.size() >= 4 && p[2] == "1";
            chainEnd = p.size() >= 4 && p[3] == "1";
        } else if (k == "acc") {
            auto p = split(v, '|');
            if (p.size() >= 2 && findKnob(p[0])) accepted.push_back({p[0], p[1], p.size() >= 3 && p[2] == "1"});
        } else if (k == "cand") {
            auto p = split(v, '|');
            if (p.size() >= 8) {
                Candidate c;
                c.id = (int)toLong(p[0], 0);
                c.baseline = p[1] == "1";
                c.hz = (u32)strtoul(p[2].c_str(), nullptr, 10);
                knobSetFromString(p[3], c.set);
                c.b.fromString(p[4]);
                c.synth = toDouble(p[5], 0);
                c.game = (int)toLong(p[6], 0);
                c.note = p[7];
                if (p.size() >= 10) {
                    c.crashes = (int)toLong(p[8], 0);
                    c.attempts = (int)toLong(p[9], 0);
                }
                if (p.size() >= 11) c.loosened = (int)toLong(p[10], 0);
                if (p.size() >= 15) {
                    c.stutters = (int)toLong(p[11], 0);
                    c.artifacts = (int)toLong(p[12], 0);
                    c.longLoads = (int)toLong(p[13], 0);
                    c.optOffered = p[14] == "1";
                }
                cands.push_back(c);
            }
        } else if (k == "run") {
            size_t hash = v.find('#');
            if (hash != std::string::npos) {
                size_t idx = (size_t)toLong(v.substr(0, hash), -1);
                TotkRun r;
                if (idx < cands.size() && r.fromString(v.substr(hash + 1))) {
                    cands[idx].runs.push_back(r);
                    if (cands[idx].runs.size() > kMaxTotkRuns) cands[idx].runs.erase(cands[idx].runs.begin());
                }
            }
        } else if (k == "gpend") gamePendingCand = (int)toLong(v, -1);
        else if (k == "gactive") gameActiveCand = (int)toLong(v, -1);
        else if (k == "gsnap") gameSnapshot = v;
        else if (k == "pendboot") pendingBoot = (s64)toLong(v, 0);
        else if (k == "pendcrash") {
            pendingCrash.clear();
            for (auto& f : split(v, ';'))
                if (!trim(f).empty()) pendingCrash.push_back(trim(f));
            std::sort(pendingCrash.begin(), pendingCrash.end());
        } else if (k == "pending") {
            if (v != "-") pending.deserialize(v);
        } else if (k == "ev") events.push_back(v);
        else if (k == "kip") {
            auto p = split(v, ',');
            if (p.size() >= 8) {
                kipStep = (int)toLong(p[0], 0);
                kipSrc = (int)toLong(p[1], -1);
                kipFinal = p[2] == "1";
                kipDirty = p[3] == "1";
                kipReboots = (int)toLong(p[4], 0);
                kipRebootAt = (s64)strtoll(p[5].c_str(), nullptr, 10);
                kipUptimeAt = (u64)strtoull(p[6].c_str(), nullptr, 10);
                kipCheckIdx = (int)toLong(p[7], 0);
                kipFixTries = p.size() >= 9 ? (int)toLong(p[8], 0) : 0;
            }
        } else if (k == "kipvals") knobSetFromString(v, kipVals);
        else if (k == "kippre") {
            size_t bar = v.find('|');
            if (bar != std::string::npos) {
                auto hdr = split(v.substr(0, bar), ',');
                if (hdr.size() >= 2) {
                    kipPrePos = (int)toLong(hdr[0], 0);
                    kipHangs = (int)toLong(hdr[1], 0);
                }
                kipPre.clear();
                for (auto& x : split(v.substr(bar + 1), ',')) {
                    auto q = split(x, ':');
                    if (q.size() == 2 && kipSlotOf(q[1]) >= 0) kipPre.push_back({(int)toLong(q[0], 0), q[1]});
                }
            }
        }
        else if (k == "kipfix") {
            size_t bar = v.find('|');
            if (bar != std::string::npos) {
                kipFixCand = (int)toLong(v.substr(0, bar), -1);
                knobSetFromString(v.substr(bar + 1), kipFixBackup);
            }
        }
        else if (k == "kiporig") {
            for (auto& x : split(v, ',')) {
                auto q = split(x, ':');
                if (q.size() == 2) kipOrig[(int)toLong(q[0], -1)] = (int)toLong(q[1], 0);
            }
        } else if (k == "kipacc" || k == "kiptest" || k == "kipq") {
            std::vector<std::string> g;
            for (auto& x : split(v, ','))
                if (kipSlotOf(trim(x)) >= 0) g.push_back(trim(x));
            if (k == "kipacc") kipAccepted = g;
            else if (k == "kiptest") kipTesting = g;
            else if (!g.empty()) kipQueue.push_back(g);
        } else if (k == "kiprej") {
            auto p = split(v, '|');
            if (p.size() >= 3) kipRejected.push_back({p[0], (int)toLong(p[1], 0), p[2]});
        } else if (k == "kiplog") kipLog.push_back(v);
    }
    if (ladder.empty()) ladder = keepLadder;
    if (!sawVersion) return false;
    // Проверка целостности: индексы должны попадать в лестницу.
    auto badIdx = [&](int i) { return i >= (int)ladder.size(); };
    for (int i : coarse)
        if (i < 0 || badIdx(i)) return false;
    if (badIdx(lastOk) || badIdx(stockLastOk)) return false;
    if (oldScan && phase == Phase::FreqScan) {
        // Поиск 1.1.0 как прогон не продолжить — начинаем прогон заново.
        buildCoarse();
        ev("Журнал прежней версии: прогон частот начат заново");
    }
    if ((phase == Phase::Timing || phase == Phase::Verify || phase == Phase::Optimize) && tIdx >= (int)targets.size()) return false;
    if (phase == Phase::Timing && kIdx > (int)plan.size()) return false;
    if (kipStep < KipWrite || kipStep > KipPre) return false;
    if (benchVer != kBenchVersion && phase != Phase::Idle) {
        // Замер изменился: абсолютные числа другие. Внутри подбора всё сравнивается заново (эталоны, замер А/Б),
        // а оценки кандидатов, замеренные прежней версией, с новыми напрямую не сравнить.
        ev("Замер потока изменён (версия %d → %d): оценки кандидатов до и после — несравнимы. Эталоны меряются заново; для чистого "
           "сравнения кандидатов лучше начать новый подбор",
           benchVer, kBenchVersion);
        if (phase == Phase::Timing) needRef = true;
        benchVer = kBenchVersion;
    }
    computeRanking();
    return true;
}

// ============================================================== проверка в TOTK

// Настройки кандидата (частота РАМ и частоты ЦПУ/ГПУ прогона) всё ещё действуют?
// Сбрасывает их перезагрузка — в том числе сторожем Ryazha-clk после падения игры.
static bool stillApplied(ITarget& t, const Candidate& c) {
    u32 ov = t.overrideHz();
    bool memOk = c.baseline ? ov == 0 : ov == c.hz;
    return memOk && t.gameClocksActive();
}

static std::string baseName(const std::string& path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

bool Tuner::gameApply(ITarget& t, int idx, std::string* err, bool bench) {
    Candidate c;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) {
            if (err) *err = "нет такого кандидата";
            LOGE("TOTK: применение несуществующего кандидата %d (всего %zu)", idx, cands.size());
            return false;
        }
        c = cands[idx];
    }
    LOGI("TOTK: применяю %s: %s", c.name().c_str(), c.baseline ? "исходные настройки" : knobSetShort(c.set).c_str());
    bool ok;
    if (c.baseline) {
        ok = t.applyOriginal(err);
    } else {
        ApplyInfo ai;
        ok = t.apply(c.hz, c.set, &ai, err);
    }
    if (!ok) {
        LOGE("TOTK: не удалось применить %s: %s", c.name().c_str(), err ? err->c_str() : "?");
        return false;
    }
    if (!bench) {
        // Просто применить (поиграть на этой частоте): ЦПУ/ГПУ — как в профиле, результата бенчмарка не ждём.
        t.freeClocks();
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            gamePendingCand = -1;
            gameActiveCand = idx;
            std::string what = c.baseline ? std::string("исходные настройки") : "РАМ " + mhzStr(c.hz) + " МГц и тайминги";
            ev("Применён %s без частот прогона: %s; ЦПУ и ГПУ — как в профиле, прогоны в игре не засчитываются", c.name().c_str(), what.c_str());
        }
        return saveState(t);
    }
    std::string clk;
    if (!t.setGameClocks(&clk)) {
        if (err) *err = "не удалось задать частоты ЦПУ/ГПУ для прогона: " + clk;
        LOGE("TOTK: %s", err ? err->c_str() : clk.c_str());
        return false;
    }
    // Короткий замер «как сейчас» — для отчёта.
    bool aborted = false;
    BenchResult b = t.bench(1, &aborted);
    int runNo;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        runNo = gameRunsDone(cands[idx]) + 1;
    }
    // Подпись в файле бенчмарка — до снимка: в новые прогоны она не попадёт, а глазами видно, чей прогон ниже.
    appendBenchMarker(strf("%s %s · %s · прогон %d из %d · %s", APP_NAME, APP_VER, c.name().c_str(), runNo, cfg.totkRuns, timeStamp().c_str()),
                      c.baseline ? "настройки до подбора · " + clk
                                 : strf("РАМ %s МГц · %s · %s", mhzStr(c.hz).c_str(), clk.c_str(), knobSetShort(c.set).c_str()));
    TLOG("TOTK ▶ %s, прогон %d из %d (%s)", c.name().c_str(), runNo, cfg.totkRuns, clk.c_str());
    BenchSnapshot snap = takeBenchSnapshot();
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (b.valid && c.baseline) {
            cands[idx].b = b;
            cands[idx].synth = scoreOf(b);
        }
        cands[idx].attempts++;
        gamePendingCand = idx;
        gameActiveCand = idx;
        gameSnapshot = snap.serialize();
        ev("TOTK: применён %s (%s) — ждём результат бенчмарка", cands[idx].name().c_str(), clk.c_str());
    }
    return saveState(t);
}

int Tuner::gameCollect(ITarget& t, std::string* msg) {
    int ret = 0;
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        int idx = gamePendingCand >= 0 ? gamePendingCand : gameActiveCand;
        if (idx < 0 || idx >= (int)cands.size()) {
            if (msg) *msg = "Нет применённого кандидата — выберите его и нажмите A";
            return 0;
        }
        Candidate& c = cands[idx];
        if (trim(gameSnapshot).empty()) {
            // Нет точки отсчёта (журнал прошлой версии) — старые прогоны новыми не считаем, отсчёт с этого момента.
            LOGW("TOTK: нет точки отсчёта файлов результата — начинаю отсчёт сейчас");
            gameSnapshot = takeBenchSnapshot().serialize();
            if (msg) *msg = "Отсчёт прогонов начат заново — запустите бенчмарк";
            return 0;
        }
        BenchSnapshot snap;
        snap.deserialize(gameSnapshot);
        std::string file;
        auto runs = newRunsSince(snap, &file);
        auto crashes = newCrashReports(snap);
        bool applied = stillApplied(t, c);
        bool pending = gamePendingCand == idx;
        LOGI("TOTK сверка: %s, ждём результат=%d, настройки действуют=%d, новых прогонов %zu (%s), новых отчётов о падении %zu",
             c.name().c_str(), pending ? 1 : 0, applied ? 1 : 0, runs.size(), file.empty() ? "-" : baseName(file).c_str(), crashes.size());
        for (auto& f : crashes) LOGW("TOTK: отчёт о падении %s", f.c_str());
        std::string crashName = crashes.empty() ? std::string() : baseName(crashes.back());

        if (!runs.empty() && (pending || applied)) {
            // Прогоны засчитываем, если ждали их или настройки кандидата всё ещё действуют
            // (например, после вылета игру перезапустили прямо из меню HOME).
            int korok = 0;
            for (auto& r : runs) {
                c.runs.push_back(r);
                if (r.korok()) korok++;
                HistRow h;
                h.ver = APP_VER;
                h.dram = chip.dramId;
                h.mhz = c.baseline ? 0 : c.hz / 1e6;
                h.kind = "totk";
                h.status = "ok";
                h.bw = c.b.valid ? c.b.bw() : 0;
                h.lat = c.b.valid ? c.b.latNs : 0;
                h.frames = r.frames;
                h.fps = r.avg;
                h.low1 = r.low1;
                h.low01 = r.low01;
                h.set = c.baseline ? std::string("как было") : knobSetCompact(c.set);
                histAppend(h);
            }
            // Не больше 15 прогонов на кандидата: старые уходят, итог — по последним.
            if (c.runs.size() > kMaxTotkRuns) c.runs.erase(c.runs.begin(), c.runs.end() - kMaxTotkRuns);
            c.game = 1;
            if (!crashes.empty()) {
                c.crashes++;
                c.note = "был отчёт о падении: " + crashName;
            } else {
                c.note.clear();
            }
            if (msg)
                *msg = strf("%s: засчитано прогонов %zu (лес короков: %d) из %s", c.name().c_str(), runs.size(), korok, baseName(file).c_str());
            for (auto& r : runs)
                ev("TOTK %s: %s — кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f", c.name().c_str(), r.location.c_str(), r.frames, r.avg, r.low1,
                   r.low01);
            // Статтеры: 0.1% low заметно хуже, чем у «Как было» (относительно среднего FPS), или ниже половины среднего.
            double refRatio = 0;
            int refN = 0;
            for (auto& b : cands)
                if (b.baseline)
                    for (auto& r : b.runs)
                        if (r.avg > 0 && r.low01 > 0) {
                            refRatio += r.low01 / r.avg;
                            refN++;
                        }
            if (refN) refRatio /= refN;
            std::string stut;
            for (auto& r : runs) {
                if (c.baseline || r.avg <= 0 || r.low01 <= 0) continue;
                double ratio = r.low01 / r.avg;
                bool bad = refN ? ratio < refRatio * 0.8 : ratio < 0.5;
                if (bad) {
                    c.stutters++;
                    stut = strf("0.1%% low %.0f при среднем %.0f (%.0f%%%s)", r.low01, r.avg, ratio * 100,
                                refN ? strf(", у «Как было» %.0f%%", refRatio * 100).c_str() : "");
                    TLOG("TOTK ⚠ СТАТТЕРЫ %s: %s", c.name().c_str(), stut.c_str());
                    noteUnstable(c.hz, "статтеры", "", 0, c.set, "по бенчмарку: " + stut);
                }
            }
            if (!stut.empty()) c.note = "статтеры: " + stut;
            int done = gameRunsDone(c);
            if (msg && done < cfg.totkRuns)
                *msg += strf(". Прогонов %d из %d — нужно ещё %d (A — снова)", done, cfg.totkRuns, cfg.totkRuns - done);
            else if (msg)
                *msg += strf(". Прогонов %d из %d — результат усреднён", done, cfg.totkRuns);
            for (auto& r : runs)
                appendBenchNote(strf("  → засчитано %s: прогон %d из %d — кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f%s", c.name().c_str(), done,
                                     cfg.totkRuns, r.frames, r.avg, r.low1, r.low01, stut.empty() ? "" : " (статтеры)"));
            ret = (int)runs.size();
            changed = true;
        } else if (!runs.empty()) {
            // Прогон есть, но настройки кандидата уже сброшены — неизвестно, на чём он сделан.
            LOGW("TOTK: в %s новый прогон, но настройки %s уже не действуют — не засчитан", baseName(file).c_str(), c.name().c_str());
            if (msg)
                *msg = t.overrideHz() == (c.baseline ? 0u : c.hz) && !t.gameClocksActive()
                           ? "Новый прогон сделан без частот ЦПУ/ГПУ прогона (кандидат был только применён) — не засчитан. Для замера — A."
                           : "Новый прогон сделан не на настройках кандидата (они сброшены перезагрузкой) — не засчитан. Примените кандидата заново (A).";
            changed = true;
        } else if (!crashes.empty()) {
            c.game = 2;
            c.crashes++;
            c.note = "игра упала (" + crashName + ")" + (applied ? "" : ", консоль перезапускалась");
            noteUnstable(c.hz, "вылет игры", "", 0, c.set, crashName);
            appendBenchNote("  ✘ " + c.name() + ": игра упала (" + crashName + ")");
            if (msg)
                *msg = c.name() + ": игра упала. " +
                       (applied ? "Настройки всё ещё применены — можно прогнать бенчмарк снова (A или из меню HOME)."
                                : "Настройки сброшены — для повтора нажмите A.");
            ev("TOTK %s: падение игры (%s)", c.name().c_str(), crashName.c_str());
            ret = -1;
            changed = true;
        } else if (pending && !applied) {
            c.game = 2;
            c.crashes++;
            c.note = "консоль перезагружалась во время игры";
            noteUnstable(c.hz, "зависание в игре", "", 0, c.set, "консоль перезагружалась во время TOTK");
            appendBenchNote("  ✘ " + c.name() + ": консоль перезагружалась во время игры");
            if (msg) *msg = c.name() + ": консоль перезагружалась (зависание или сторож Ryazha-clk). Для повтора нажмите A.";
            ev("TOTK %s: перезагрузка во время игры", c.name().c_str());
            ret = -2;
            changed = true;
        } else if (msg) {
            *msg = applied ? "Нового результата бенчмарка пока нет" : "Настройки кандидата сброшены — примените его заново (A)";
        }
        if (changed && pending) gamePendingCand = -1;
        if (!applied && gameActiveCand == idx) {
            LOGI("TOTK: настройки %s больше не действуют", c.name().c_str());
            gameActiveCand = -1;
            changed = true;
        }
        // Новая точка отсчёта: следующие прогоны и падения считаются от этого момента.
        if (changed) gameSnapshot = takeBenchSnapshot().serialize();
        computeRanking();
    }
    if (changed) saveState(t);
    flushUnstable(t);
    return ret;
}

int Tuner::gameRunsDone(const Candidate& c) const {
    int korok = 0;
    for (auto& r : c.runs)
        if (r.korok()) korok++;
    return korok ? korok : (int)c.runs.size();
}

void Tuner::gameFlag(ITarget& t, int idx, const std::string& kind) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) return;
        Candidate& c = cands[idx];
        if (kind == "артефакты") c.artifacts++;
        else if (kind == "долгие загрузки") c.longLoads++;
        else c.stutters++;
        c.note = kind + " (отмечено в игре)";
        noteUnstable(c.hz, kind, "", 0, c.set, "отмечено вручную после игры");
        ev("TOTK %s: %s — отмечено вручную", c.name().c_str(), kind.c_str());
        computeRanking();
    }
    saveState(t);
    flushUnstable(t);
}

void Tuner::gameMarkFailed(ITarget& t, int idx, const std::string& why) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) return;
        cands[idx].game = 2;
        cands[idx].crashes++;
        cands[idx].note = why;
        noteUnstable(cands[idx].hz, "вылет игры", "", 0, cands[idx].set, why);
        if (gamePendingCand == idx) gamePendingCand = -1;
        gameSnapshot = takeBenchSnapshot().serialize();
        ev("TOTK %s: сбой — %s", cands[idx].name().c_str(), why.c_str());
        computeRanking();
    }
    saveState(t);
}

void Tuner::gameClearRuns(ITarget& t, int idx) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) return;
        cands[idx].runs.clear();
        cands[idx].game = 0;
        cands[idx].crashes = 0;
        cands[idx].attempts = 0;
        cands[idx].note.clear();
        LOGI("TOTK: результаты %s очищены", cands[idx].name().c_str());
        computeRanking();
    }
    saveState(t);
}

void Tuner::gameCancelPending(ITarget& t) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        gamePendingCand = -1;
    }
    saveState(t);
}

void Tuner::gameDeactivate(ITarget& t) {
    bool had = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        had = gameActiveCand >= 0 || gamePendingCand >= 0;
        gameActiveCand = -1;
        gamePendingCand = -1;
    }
    if (had) saveState(t);
}

void Tuner::runsMedian(const std::vector<TotkRun>& runs, double* frames, double* fps, double* low1, double* low01) {
    auto med = [&](double TotkRun::*m) {
        std::vector<double> v;
        for (auto& r : runs) v.push_back(r.*m);
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
    };
    if (frames) *frames = med(&TotkRun::frames);
    if (fps) *fps = med(&TotkRun::avg);
    if (low1) *low1 = med(&TotkRun::low1);
    if (low01) *low01 = med(&TotkRun::low01);
}

void Tuner::computeRanking() {
    double maxSynth = 0;
    for (auto& c : cands)
        if (!c.baseline) maxSynth = std::max(maxSynth, c.synth);
    struct Agg {
        double f = 0, l1 = 0, l01 = 0;
        bool ok = false;
    };
    std::vector<Agg> agg(cands.size());
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        if (c.game == 2 || c.runs.empty()) continue;  // последний запуск — сбой: до успешного повтора не участвует
        bool anyKorok = false;
        for (auto& r : c.runs)
            if (r.korok()) anyKorok = true;
        std::vector<TotkRun> use;
        for (auto& r : c.runs) {
            if (anyKorok && !r.korok()) continue;
            TotkRun x = r;
            if (x.frames <= 0) x.frames = r.avg * 100.0;  // кадры точнее целого среднего FPS; если мод их не дал — среднее
            use.push_back(x);
        }
        if (!use.empty()) {
            // Медиана, а не среднее: один холодный прогон (первый после применения часто ниже) не тянет итог.
            double fps = 0;
            runsMedian(use, &agg[i].f, &fps, &agg[i].l1, &agg[i].l01);
            agg[i].ok = true;
        }
    }
    // База для игры — «как было», если прогнан; иначе лучший по кадрам.
    Agg ref;
    for (size_t i = 0; i < cands.size(); i++)
        if (cands[i].baseline && agg[i].ok) ref = agg[i];
    if (!ref.ok)
        for (size_t i = 0; i < cands.size(); i++)
            if (agg[i].ok && agg[i].f > ref.f) ref = agg[i];
    double maxGame = 0;
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        c.gameScore = 0;
        if (!agg[i].ok || !ref.ok) continue;
        auto rel = [](double a, double b) { return (a > 0 && b > 0) ? a / b : 1.0; };
        c.gameScore = std::pow(rel(agg[i].f, ref.f), 0.5) * std::pow(rel(agg[i].l1, ref.l1), 0.3) * std::pow(rel(agg[i].l01, ref.l01), 0.2);
        maxGame = std::max(maxGame, c.gameScore);
    }
    double wg = cfg.wGame / 100.0;
    for (auto& c : cands) {
        double s = (maxSynth > 0 && c.synth > 0) ? c.synth / maxSynth : 0;
        double g = (maxGame > 0 && c.gameScore > 0) ? c.gameScore / maxGame : 0;
        if (c.game == 2)
            c.combined = 0;
        else if (g > 0 && s > 0)
            c.combined = std::pow(s, 1.0 - wg) * std::pow(g, wg);
        else if (g > 0)
            c.combined = g;
        else
            c.combined = s * (1.0 - wg);  // без игры кандидат не может обогнать проверенных
        // Максимум: замечания в игре только штрафуют (по 10 % за каждое). Средний: штрафуют все, кроме вылетов
        // (вылет исключает кандидата, см. bestCandidate).
        if (cfg.ramMode == Settings::ModeMax && !c.baseline) c.combined *= std::pow(0.9, c.issues());
        if (cfg.ramMode == Settings::ModeMedium && !c.baseline) c.combined *= std::pow(0.9, c.issues() - c.crashes);
    }
}

int Tuner::bestCandidate() const {
    // Приоритет: проверен в игре без сбоев > проверен, но сбои были > не проверен. Сбой в последнем запуске — исключён.
    // Проверенным считается кандидат с нужным числом прогонов TOTK (по умолчанию 3, результат — среднее).
    // Безопасный режим: любое замечание в игре (вылет, статтеры, артефакты, долгие загрузки) — не рекомендуется.
    // Средний: вылет / зависание — не рекомендуется, остальное штрафует. Максимум: замечания только штрафуют оценку.
    auto tier = [this](const Candidate& c) {
        bool tested = c.game == 1 && gameRunsDone(c) >= cfg.totkRuns;
        if (c.doubtful()) return 0;  // сомнительный — не рекомендуется, но и не удаляется
        if (cfg.ramMode == Settings::ModeSafe && c.issues() > 0) return 0;
        if (cfg.ramMode == Settings::ModeMedium && c.crashes > 0) return 0;
        if (tested && c.issues() == 0) return 3;
        if (tested) return cfg.ramMode == Settings::ModeSafe ? 2 : 3;
        return 1;
    };
    int best = -1;
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        if (c.baseline || c.game == 2 || tier(c) == 0) continue;
        if (best < 0) {
            best = (int)i;
            continue;
        }
        int tc = tier(c), tb = tier(cands[best]);
        if (tc > tb || (tc == tb && c.combined > cands[best].combined)) best = (int)i;
    }
    return best;
}

int Tuner::sweetCandidate() const {
    std::vector<SweetPoint> pts(cands.size());
    for (size_t i = 0; i < cands.size(); i++) {
        const Candidate& c = cands[i];
        if (c.baseline || c.game != 1 || gameRunsDone(c) < cfg.totkRuns || c.doubtful() || c.crashes > 0 || !c.b.valid) continue;
        if (cfg.ramMode == Settings::ModeSafe && c.issues() > 0) continue;
        double f = 0;
        runsMedian(c.runs, &f, nullptr, nullptr, nullptr);  // медиана прогонов, как в оценке
        pts[i] = {c.hz, c.b.bw(), f};
    }
    return sweetSpot(pts, cfg.wGame / 100.0);
}

bool Tuner::finalize(ITarget& t, int idx, std::string* err) {
    Candidate c;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size() || cands[idx].baseline) {
            if (err) *err = "выберите кандидата";
            return false;
        }
        c = cands[idx];
    }
    if (!t.commit(c.hz, c.set, err)) {
        LOGE("Запись итога %s не удалась: %s", c.name().c_str(), err ? err->c_str() : "?");
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        phase = Phase::Done;
        gameActiveCand = -1;
        gamePendingCand = -1;
        status = strf("Записано: %s МГц, %s", mhzStr(c.hz).c_str(), knobSetShort(c.set).c_str());
        ev("Итог записан в timings.ini: %s МГц — %s", mhzStr(c.hz).c_str(), knobSetShort(c.set).c_str());
    }
    saveState(t);
    t.saveText(paths::report(), report());
    return true;
}

std::string Tuner::report() const {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string o;
    o += strf("%s %s — отчёт подбора памяти\n", APP_NAME, APP_VER);
    o += strf("Режим: %s · прогонов TOTK на кандидата: %d · журнал испытаний: %s\n", Settings::ramModeName(cfg.ramMode), cfg.totkRuns,
              logx::testFile().c_str());
    if (singleHz) o += strf("Подбор одной частоты: %s МГц (без прогонов частот)\n", mhzStr(singleHz).c_str());
    if (kfDone) o += "Кип сначала: общие тайминги гайда в кипе (" + knobSetShort(kfVals) + ") — прогоны и доводка на них\n";
    if (freqCap) o += strf("Потолок частоты: %s МГц — выше прогон закончился зависаниями, дальше проверки не выше\n", mhzStr(freqCap).c_str());
    o += strf("Чип: %s (DRAM ID %d)\n", chip.name.c_str(), chip.dramId);
    o += strf("Этап: %s\n", phaseName(phase));
    if (!error.empty()) o += "Ошибка: " + error + "\n";
    if (maxMode())
        o += strf("Проб выполнено: %d; «максимум»: прирост от %.2f %%, спорное — замер А/Б; проход ±%.3f %% (по %d степ. свободы), шум А/Б до "
                  "%.2f %%\n",
                  trials, kMaxGain * 100, passSigma() * 100, noiseDf, pairNoise() * 100);
    else
        o += strf("Проб выполнено: %d, порог шума %.2f %%\n", trials, eps * 100);
    if (base.valid) o += strf("База (%s МГц): поток %.0f МБ/с, задержка %.1f нс\n\n", mhzStr(stockHz()).c_str(), base.bw(), base.latNs);
    if (!ladder.empty())
        o += strf("Лестница Ryazha-clk: %zu ступеней, %s … %s МГц; прогон: %d … %s МГц\n", ladder.size(), mhzStr(ladder.front()).c_str(),
                  mhzStr(ladder.back()).c_str(), cfg.minMhz, cfg.maxMhz ? strf("%d", cfg.maxMhz).c_str() : "верх лестницы");
    {
        int nOk = 0, nSlow = 0, nFail = 0;
        for (auto& p : points) {
            if (p.status == 1) nOk++;
            else if (p.status == 3) nSlow++;
            else nFail++;
        }
        o += strf("Итог прогона: стабильно %d, медленнее предыдущей %d, сбой %d\n", nOk, nSlow, nFail);
    }
    for (int pass = 0; pass < 2; pass++) {
        auto pk = bandwidthPeaks(pass);
        if (pk.empty()) continue;
        o += strf("Пики потока (%s, падение дальше от %d МБ/с):", pass ? "гайд" : "сток", cfg.peakDropMb);
        for (auto& x : pk) o += strf(" %s (−%.0f)", mhzStr(x.first).c_str(), x.second);
        o += "\n";
    }
    o += guideSet.empty() ? "Частоты (сток Ryazha-clk):\n" : "Частоты (сток Ryazha-clk; «гайд» — " + guideName() + "):\n";
    for (auto& p : points)
        o += strf("  %8s МГц %-5s %-8s  поток %6.0f МБ/с  задержка %6.1f нс  оценка %.3f  %s\n", mhzStr(p.hz).c_str(), p.pass ? "гайд" : "сток",
                  p.status == 1 ? "ок" : (p.status == 3 ? "медленнее" : "сбой"), p.b.bw(), p.b.latNs, p.score, p.reason.c_str());
    o += "\nКандидаты:\n";
    for (auto& c : cands) {
        o += strf("  %s: %s\n", c.name().c_str(), c.baseline ? "настройки до подбора" : knobSetShort(c.set).c_str());
        if (c.b.valid)
            o += strf("     синтетика: чтение %.0f, запись %.0f, копирование %.0f МБ/с, задержка %.1f / %.1f нс, оценка %.3f\n", c.b.readMBs,
                      c.b.writeMBs, c.b.copyMBs, c.b.latNs, c.b.lat2Ns, c.synth);
        for (auto& r : c.runs)
            o += strf("     TOTK %s: кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f\n", r.location.c_str(), r.frames, r.avg, r.low1, r.low01);
        if (c.attempts || c.crashes) o += strf("     запусков игры: %d, сбоев: %d\n", c.attempts, c.crashes);
        if (c.game == 2) o += "     СБОЙ В ПОСЛЕДНЕМ ЗАПУСКЕ: " + c.note + "\n";
        else if (!c.note.empty()) o += "     примечание: " + c.note + "\n";
        if (c.combined > 0) o += strf("     итоговая оценка %.3f\n", c.combined);
        if (!c.baseline && !c.set.empty()) {
            o += "     секция для timings.ini:\n";
            TimingsIni ini;
            ini.setSection(hzToKhz(c.hz), c.set, nullptr);
            for (auto& l : split(ini.serialize(), '\n'))
                if (!l.empty()) o += "       " + l + "\n";
        }
    }
    if (!kipVals.empty()) {
        o += "\nПеренос в кип (ручки пропускной способности, с перезагрузками):\n";
        o += strf("  перезагрузок: %d; принято: %s\n", kipReboots, joinKeys(kipAccepted, kipVals).c_str());
        for (auto& r : kipRejected) o += strf("  отклонено: %s = %d — %s\n", r.key.c_str(), r.val, r.reason.c_str());
        for (auto& l : kipLog) o += "  · " + l + "\n";
    }
    if (!unstable.empty()) {
        o += strf("\nНестабильные значения (реестр %s, последние 40 из %zu):\n", paths::unstable().c_str(), unstable.size());
        for (size_t i = unstable.size() > 40 ? unstable.size() - 40 : 0; i < unstable.size(); i++) {
            auto& e = unstable[i];
            std::string what = e.key.empty() ? "набор " + knobSetShort(e.set) : strf("%s = %d", e.key.c_str(), e.val);
            o += strf("  %8s МГц  %-16s %s%s\n", mhzStr(e.hz).c_str(), e.kind.c_str(), what.c_str(), e.detail.empty() ? "" : (" — " + e.detail).c_str());
        }
    }
    int b = bestCandidate();
    if (b >= 0) o += strf("\nРекомендация: %s — %s\n", cands[b].name().c_str(), knobSetShort(cands[b].set).c_str());
    int sw = sweetCandidate();
    if (sw >= 0) {
        double f = 0;
        runsMedian(cands[sw].runs, &f, nullptr, nullptr, nullptr);
        o += strf("%s%s (поток × кадры TOTK): %s — %.0f МБ/с, %.0f кадров — %s\n", b >= 0 ? "" : "\n", SWEET_MARK, cands[sw].name().c_str(),
                  cands[sw].b.bw(), f, knobSetShort(cands[sw].set).c_str());
    }
    o += "\nПодробный журнал приложения (ошибки, IPC, файлы): " + logx::file() + "\n";
    o += "\nСобытия подбора:\n";
    for (auto& e : events) o += "  " + e + "\n";
    return o;
}
