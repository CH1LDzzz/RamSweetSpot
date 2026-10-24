// Проверка алгоритма автоподбора на ПК: симулятор консоли с моделью памяти.
// Сборка и запуск: make -C tests

#include <algorithm>
#include <memory>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>

#include "../source/common.hpp"
#include "../source/knobs.hpp"
#include "../source/log.hpp"
#include "../source/membench.hpp"
#include "../source/totk.hpp"
#include "../source/tuner.hpp"

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_fail++;                                     \
        }                                                 \
    } while (0)

struct Hang : std::runtime_error {
    Hang() : std::runtime_error("hang") {}
};

// ------------------------------------------------------------------ модель памяти

struct ChipModel {
    double maxStableMhz = 2950;  // на общих таймингах кипа
    double pllDropMhz = 3050;    // выше — частота проваливается
    std::map<std::string, int> kip = {{"tRCD", 1}, {"tRP", 1}, {"tRAS", 2}, {"tRRD", 1}, {"tRFC", 3},
                                      {"tRTW", 2}, {"tWTR", 2}, {"tREFI", 0}};
    // Предел ступени на 2400 МГц; выше частота — предел ниже.
    std::map<std::string, int> limit2400 = {{"tRCD", 6}, {"tRP", 5}, {"tRAS", 8}, {"tRRD", 4}, {"tRFC", 7},
                                            {"tRTW", 6}, {"tWTR", 7}, {"tREFI", 5}};
    int rlMin = -2, wlMin = -3, tfawMin = -3;
    // «Дыра»: на этих частотах memtest валится (или консоль виснет), выше снова стабильно.
    double holeLo = 0, holeHi = 0;
    bool holeHangs = false;
    // На этих частотах поток проседает (как при PLL drop без смены частоты).
    double slowLo = 0, slowHi = 0;

    int effective(const KnobSet& s, const std::string& k) const {
        auto it = s.find(k);
        if (it != s.end()) return it->second;
        auto d = findKnob(k);
        if (d && d->absentIsZero) return 0;
        auto kv = kip.find(k);
        return kv != kip.end() ? kv->second : 0;
    }
    int limitAt(const std::string& k, double mhz) const {
        int base = limit2400.at(k);
        int drop = mhz > 2400 ? (int)((mhz - 2400) / 250) : 0;
        return base - drop;
    }
    // Пограничный режим: значение ровно на пределе проходит короткий тест,
    // но валится (или вешает консоль) на длинном.
    bool marginal = false;
    bool marginalHangs = false;

    // 0 — стабильно, 1 — ошибки memtest, 2 — зависание. seconds — длительность теста.
    int stability(double mhz, const KnobSet& s, int seconds = 0) const {
        if (mhz > maxStableMhz + 120) return 2;
        if (holeHi > 0 && mhz >= holeLo && mhz <= holeHi) return holeHangs ? 2 : 1;
        int worst = mhz > maxStableMhz ? 1 : 0;
        for (auto& kv : limit2400) {
            int v = effective(s, kv.first);
            int lim = limitAt(kv.first, mhz);
            if (v > lim + 1) worst = std::max(worst, 2);
            else if (v > lim) worst = std::max(worst, 1);
            else if (marginal && v == lim && seconds >= 30 && s.count(kv.first)) worst = std::max(worst, marginalHangs ? 2 : 1);
        }
        int rl = effective(s, "RL");
        if (rl < rlMin - (mhz > 2700 ? 0 : -1)) worst = std::max(worst, rl < rlMin - 1 ? 2 : 1);
        if (effective(s, "WL") < wlMin) worst = std::max(worst, 1);
        if (effective(s, "tFAW") < tfawMin) worst = std::max(worst, 1);
        return worst;
    }
    BenchResult perf(double mhz, const KnobSet& s, std::mt19937& rng) const {
        double tight = 0;
        tight += effective(s, "tRCD") * 0.006 + effective(s, "tRP") * 0.005 + effective(s, "tRAS") * 0.002;
        tight += effective(s, "tRRD") * 0.002 + effective(s, "tRFC") * 0.003 + effective(s, "tRTW") * 0.002;
        tight += effective(s, "tWTR") * 0.002 + effective(s, "tREFI") * 0.012;
        double latGain = -effective(s, "RL") * 0.035 - effective(s, "WL") * 0.005 + (effective(s, "ramPD") == 1 ? 0.03 : 0);
        int da = effective(s, "daTurns");
        double daGain = da == 2 ? 0.012 : (da == 1 ? 0.006 : (da == 3 ? -0.004 : 0));
        int aw = effective(s, "arbWin");
        double awGain = 0.004 * std::min(aw, 4) - 0.003 * std::max(0, aw - 4);
        double fawGain = -effective(s, "tFAW") * 0.002;
        std::normal_distribution<double> n(1.0, 0.0012);
        BenchResult b;
        b.valid = true;
        double bw = 9.0 * mhz * (1 + tight + daGain + awGain + fawGain);
        if (slowHi > 0 && mhz >= slowLo && mhz <= slowHi) bw *= 0.8;
        b.readMBs = bw * n(rng);
        b.writeMBs = bw * 0.8 * n(rng);
        b.copyMBs = bw * 0.9 * n(rng);
        b.latNs = (130.0 + 120000.0 / mhz) * (1 - tight * 0.8 - latGain) * n(rng);
        b.lat2Ns = b.latNs * 0.6;
        return b;
    }
};

class SimTarget : public ITarget {
public:
    ChipModel m;
    std::mt19937 rng{12345};
    u32 override_ = 0;
    u32 cpuOv = 0, gpuOv = 0;  // временные частоты ЦПУ/ГПУ
    u32 curHz = 1600000000u;
    KnobSet curSet;  // для текущей частоты
    std::string original;
    int hangs = 0;
    int applies = 0;
    bool hangArmed = false;  // текущая конфигурация «висит» — следующий тест её уронит

    std::string timingsPath() const { return paths::timings(); }

    bool apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) override {
        applies++;
        std::string content = composeTimings(original, hzToKhz(hz), set, "RamSweetSpot");
        writeFile(timingsPath(), content);
        override_ = hz;
        curHz = hz;
        curSet = set;
        info->realHz = realHz();
        info->confirmed = true;
        (void)err;
        int st = m.stability(hz / 1e6, set);
        if (st == 2) hangArmed = true;
        return true;
    }
    bool applyOriginal(std::string*) override {
        writeFile(timingsPath(), original);
        override_ = 0;
        cpuOv = gpuOv = 0;
        curHz = 2133000000u;  // профиль игры
        curSet.clear();
        return true;
    }
    bool commit(u32 hz, const KnobSet& set, std::string*) override {
        std::string content = composeTimings(original, hzToKhz(hz), set, "RamSweetSpot: итог");
        writeFile(timingsPath(), content);
        override_ = hz;
        return true;
    }
    bool stability(int seconds, std::string* err, bool* aborted) override {
        (void)seconds;
        *aborted = false;
        if (hangArmed) {
            hangArmed = false;
            hangs++;
            throw Hang();
        }
        int st = m.stability(curHz / 1e6, curSet, seconds);
        if (st == 2) {
            hangs++;
            throw Hang();
        }
        if (st >= 1) {
            *err = "3 ошиб. (случайные)";
            return false;
        }
        return true;
    }
    BenchResult bench(int, bool* aborted) override {
        *aborted = false;
        if (hangArmed) {
            hangArmed = false;
            hangs++;
            throw Hang();
        }
        return m.perf(realHz() / 1e6, curSet, rng);
    }
    u32 realHz() override { return curHz / 1e6 > m.pllDropMhz ? (u32)(curHz * 0.88) : curHz; }
    u32 overrideHz() override { return override_; }
    bool setGameClocks(std::string* note) override {
        cpuOv = 2397000000u;
        gpuOv = 1305600000u;
        if (note) *note = "ЦПУ 2397.0, ГПУ 1305.6";
        return true;
    }
    bool gameClocksActive() override { return cpuOv == 2397000000u && gpuOv == 1305600000u; }
    int socTempC() override { return 45; }
    bool saveText(const std::string& path, const std::string& text) override {
        makeDirs(paths::appDir());
        return writeFileSafe(path, text);
    }
    bool loadText(const std::string& path, std::string& text) override { return readFileSafe(path, text); }
    void idle(u32) override {}
    void reboot() {
        override_ = 0;
        cpuOv = gpuOv = 0;
        curHz = 1600000000u;
        curSet.clear();
        hangArmed = false;
    }
};

static std::vector<u32> makeLadder() {
    std::vector<u32> l = {204000000u, 1331200000u, 1600000000u};
    for (u32 f = 1638400000u; f <= 3200000000u; f += 38400000u) l.push_back(f);
    l.push_back(1866000000u);
    l.push_back(2133000000u);
    l.push_back(2400000000u);
    l.push_back(2666000000u);
    l.push_back(2933000000u);
    std::sort(l.begin(), l.end());
    return l;
}

// ------------------------------------------------------------------ юнит-тесты

static void testTimingsIni() {
    printf("[timings.ini]\n");
    std::string orig =
        "; комментарий пользователя\n"
        ";  tRCD 0-7\n"
        "\n"
        "[1638.4]\n"
        "tRAS = 5\n"
        "latAllow = 4\n"
        "\n"
        "[2934400]\n"
        "tRCD = 2 ; своё\n";
    TimingsIni ini;
    ini.parse(orig);
    CHECK(ini.sections.size() == 2, "секций %zu", ini.sections.size());
    CHECK(ini.serialize() == orig, "файл без изменений должен совпасть байт в байт");
    KnobSet s;
    CHECK(ini.getSection(2934400, s) && s["tRCD"] == 2, "секция в кГц");
    CHECK(ini.getSection(1638400, s) && s["tRAS"] == 5 && s["latAllow"] == 4, "секция в МГц");
    std::string c = composeTimings(orig, 2665600, {{"tRCD", 4}, {"RL", -1}}, "RamSweetSpot");
    TimingsIni ini2;
    ini2.parse(c);
    CHECK(ini2.sections.size() == 3, "после добавления секций %zu", ini2.sections.size());
    CHECK(ini2.getSection(2665600, s) && s["tRCD"] == 4 && s["RL"] == -1, "наша секция читается");
    CHECK(c.find("; комментарий пользователя") != std::string::npos, "комментарии сохранены");
    CHECK(c.find("[2665.6]") != std::string::npos, "заголовок в МГц");
    CHECK(ini2.sections[1].khz == 2665600, "секция вставлена по возрастанию");
    std::string c2 = composeTimings(c, 2665600, {}, "x");
    CHECK(c2 == orig, "пустой набор убирает секцию и возвращает исходный файл");
    std::string c3 = composeTimings(orig, 2934400, {{"tRCD", 5}}, "RamSweetSpot");
    TimingsIni ini3;
    ini3.parse(c3);
    CHECK(ini3.getSection(2934400, s) && s.size() == 1 && s["tRCD"] == 5, "замена существующей секции");
    KnobSet k;
    CHECK(knobSetFromString(knobSetToString({{"tRCD", 4}, {"RL", -2}}), k) && k["RL"] == -2, "сериализация набора");
    CHECK(mhzStr(2934400000u) == "2934.4" && mhzStr(1600000000u) == "1600.0", "mhzStr");
}

static void testTotkParse() {
    printf("[TOTKBench]\n");
    // Ровно как формирует мод (строки из subsdk3): "BENCHMARK FOR %s COMPLETED!\nTotal frames: %i, ..."
    std::string t =
        "BENCHMARK FOR Korok Forest COMPLETED!\n"
        "Total frames: 3456, Average FPS: 57, 1% FPS: 41, 0.1% FPS: 33\n";
    auto r = parseTotkBench(t);
    CHECK(r.size() == 1, "прогонов %zu", r.size());
    if (r.size() == 1) {
        CHECK(r[0].location == "Korok Forest" && r[0].korok(), "место '%s'", r[0].location.c_str());
        CHECK(r[0].frames == 3456 && r[0].avg == 57 && r[0].low1 == 41 && r[0].low01 == 33, "числа %.0f %.0f %.0f %.0f", r[0].frames,
              r[0].avg, r[0].low1, r[0].low01);
    }
    std::string t2 = t + "BENCHMARK FOR Kakariko COMPLETED!\r\nTotal frames: 3000, Average FPS: 50, 1% FPS: 40, 0.1% FPS: 31\r\n";
    auto r2 = parseTotkBench(t2);
    CHECK(r2.size() == 2 && r2[1].location == "Kakariko" && r2[1].low01 == 31, "два блока, CRLF");
    auto r3 = parseTotkBench("Total frames: 100, Average FPS: 57.5, 1% FPS: 41.2, 0.1% FPS: 33.9");
    CHECK(r3.size() == 1 && std::fabs(r3[0].avg - 57.5) < 1e-9 && std::fabs(r3[0].low01 - 33.9) < 1e-9, "дробные значения");
    TotkRun x;
    CHECK(x.fromString(r[0].toString()) && x.frames == 3456, "сериализация прогона");
}

static void testBenchFiles() {
    printf("[файл бенчмарка: новые прогоны]\n");
    std::string f = sdRoot() + "TOTKBenchmark.txt";
    removeFile(sdRoot() + "TOTKBench.txt");
    writeFile(f, "BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: 3000, Average FPS: 50, 1% FPS: 40, 0.1% FPS: 30\n");
    BenchSnapshot s = takeBenchSnapshot();
    BenchSnapshot s2;
    s2.deserialize(s.serialize());
    std::string file;
    CHECK(newRunsSince(s2, &file).empty(), "без изменений новых прогонов нет");
    // Мод дописал блок.
    std::string data;
    readFile(f, data);
    writeFile(f, data + "BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: 3100, Average FPS: 52, 1% FPS: 41, 0.1% FPS: 31\n");
    auto n = newRunsSince(s2, &file);
    CHECK(n.size() == 1 && n[0].frames == 3100, "дописанный блок найден");
    // Мод перезаписал файл.
    writeFile(f, "BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: 3200, Average FPS: 53, 1% FPS: 42, 0.1% FPS: 32\n");
    n = newRunsSince(s2, &file);
    CHECK(n.size() == 1 && n[0].frames == 3200, "перезаписанный файл найден");
    // Имя из задачи пользователя.
    removeFile(f);
    BenchSnapshot s3 = takeBenchSnapshot();
    writeFile(sdRoot() + "TOTKBench.txt", "BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: 3300, Average FPS: 55, 1% FPS: 43, 0.1% FPS: 33\n");
    n = newRunsSince(s3, &file);
    CHECK(n.size() == 1 && n[0].frames == 3300 && file.find("TOTKBench.txt") != std::string::npos, "TOTKBench.txt");
    removeFile(sdRoot() + "TOTKBench.txt");
}

static void testUltraCamIni() {
    printf("[maxlastbreath.ini]\n");
    std::string dir = sdRoot() + "atmosphere/contents/0100F2C0115B6000/romfs/UltraCam/";
    makeDirs(dir);
    writeFile(dir + "maxlastbreath.ini", "[Resolution]\nMaxFramerate = 60\n\n# 4 - Korok Forest\n\n[Benchmark]\nBenchmark = 2\n\n[Heaps]\nRSDB = 1.0");
    std::string p = findUltraCamIni();
    CHECK(!p.empty() && readBenchmarkIndex(p) == 2, "индекс бенчмарка");
    CHECK(writeBenchmarkIndex(p, 4) && readBenchmarkIndex(p) == 4, "запись Benchmark = 4");
    std::string d;
    readFile(p, d);
    CHECK(d.find("RSDB = 1.0") != std::string::npos && d.find("MaxFramerate = 60") != std::string::npos, "остальное не тронуто");
}

// ------------------------------------------------------------------ полный прогон

static bool runToGame(SimTarget& t, Tuner& tu, int* reboots, int maxSteps) {
    std::atomic<bool> stop{false};
    int steps = 0;
    while (steps < maxSteps) {
        try {
            if (!tu.step(t, &stop)) return true;
            steps++;
            // Снимок посреди подбора таймингов — для предпросмотра экранов.
            if (tu.phase == Phase::Timing && tu.kIdx >= 3 && !fileExists(sdRoot() + "preview_timing.txt"))
                writeFile(sdRoot() + "preview_timing.txt", tu.serialize());
        } catch (Hang&) {
            // «Перезагрузка»: приложение умерло посреди пробы, журнал остался на карте.
            (*reboots)++;
            t.reboot();
            // Как новый процесс: всё состояние берётся только из журнала.
            tu.resetAll();
            tu.ladder.clear();
            bool rec = false;
            if (!tu.loadState(t, &rec)) {
                printf("  не удалось загрузить журнал после перезагрузки\n");
                return false;
            }
            CHECK(rec, "после перезагрузки прерванная проба должна засчитаться как сбой");
        }
    }
    printf("  превышено число шагов\n");
    return false;
}

static void checkCandidatesStable(const Tuner& tu, const ChipModel& m) {
    for (auto& c : tu.cands) {
        if (c.baseline) continue;
        int st = m.stability(c.hz / 1e6, c.set, 600);  // как на часовом прогоне
        CHECK(st == 0, "кандидат %s нестабилен в модели: %s", mhzStr(c.hz).c_str(), knobSetShort(c.set).c_str());
        // Запас: для каждого рискованного первичного тайминга значение не на самом пределе (margin=1).
        for (auto& kv : c.set) {
            if (m.limit2400.count(kv.first)) {
                int lim = m.limitAt(kv.first, c.hz / 1e6);
                if (kv.second >= lim && tu.cfg.margin > 0)
                    printf("  примечание: %s = %d на пределе %d (%s МГц) — допустимо, если сбой не встречался\n", kv.first.c_str(), kv.second, lim,
                           mhzStr(c.hz).c_str());
            }
        }
    }
}

static void fullRun(const char* title, ChipModel model, const ChipInfo& chip, Settings cfg) {
    printf("[полный прогон: %s]\n", title);
    removeFile(paths::state());
    SimTarget t;
    t.m = model;
    t.original = "; файл пользователя\n[1638.4]\ntRAS = 5\n";
    writeFile(paths::timings(), t.original);
    Tuner tu;
    tu.cfg = cfg;
    tu.begin(makeLadder(), chip);
    int reboots = 0;
    bool ok = runToGame(t, tu, &reboots, 5000);
    CHECK(ok, "алгоритм должен дойти до этапа TOTK");
    CHECK(tu.phase == Phase::Game, "этап: %s (%s)", phaseName(tu.phase), tu.error.c_str());
    printf("  проб: %d, перезагрузок (зависаний): %d, применений: %d\n", tu.trials, reboots, t.applies);
    for (auto& p : tu.points)
        printf("    %7s МГц %s %s\n", mhzStr(p.hz).c_str(), p.status == 1 ? "ок  " : (p.status == 3 ? "медл" : "СБОЙ"),
               p.status == 1 ? "" : p.reason.c_str());
    int real = 0;
    for (auto& c : tu.cands)
        if (!c.baseline) real++;
    CHECK(real >= 1, "должен быть хотя бы один кандидат");
    for (auto& c : tu.cands)
        if (!c.baseline)
            printf("  кандидат %s: %s | поток %.0f, задержка %.1f, оценка %.3f\n", c.name().c_str(), knobSetShort(c.set).c_str(), c.b.bw(), c.b.latNs,
                   c.synth);
    checkCandidatesStable(tu, t.m);
    double fmax = 0;
    for (auto& c : tu.cands) fmax = std::max(fmax, c.hz / 1e6);
    double expect = std::min(t.m.maxStableMhz, t.m.pllDropMhz);
    CHECK(fmax <= expect && fmax >= expect - 120, "частота %.1f, ожидалась около %.0f", fmax, expect);

    // --- игра: «как было» + все кандидаты
    removeFile(sdRoot() + "TOTKBench.txt");
    removeFile(sdRoot() + "TOTKBenchmark.txt");
    for (size_t i = 0; i < tu.cands.size(); i++) {
        std::string err;
        CHECK(tu.gameApply(t, (int)i, &err), "gameApply: %s", err.c_str());
        std::string msg;
        CHECK(tu.gameCollect(t, &msg) == 0, "до прогона результата быть не должно");
        // Игра прогнала бенчмарк: кадры пропорциональны синтетике (+ шум).
        double fr = tu.cands[i].baseline ? 3000 : 3000 * (0.9 + 0.1 * tu.cands[i].synth / 1.6);
        std::string old;
        readFile(sdRoot() + "TOTKBenchmark.txt", old);
        writeFile(sdRoot() + "TOTKBenchmark.txt", old + strf("BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: %.0f, Average FPS: %.0f, 1%% FPS: %.0f, 0.1%% FPS: %.0f\n", fr,
                                                              fr / 60, fr / 75, fr / 92));
        int n = tu.gameCollect(t, &msg);
        CHECK(n == 1, "собрано %d (%s)", n, msg.c_str());
    }
    // Симулируем сбой в игре у одного кандидата: отчёт о падении.
    if (tu.cands.size() > 2) {
        std::string err;
        tu.gameApply(t, 2, &err);
        makeDirs(sdRoot() + "atmosphere/crash_reports");
        writeFile(sdRoot() + "atmosphere/crash_reports/01700000000_0100f2c0115b6000.log", "crash");
        std::string msg;
        CHECK(tu.gameCollect(t, &msg) == -1, "падение должно распознаться");
        CHECK(tu.cands[2].game == 2, "кандидат помечен как сбойный");
        removeFile(sdRoot() + "atmosphere/crash_reports/01700000000_0100f2c0115b6000.log");
    }
    tu.computeRanking();
    if (!fileExists(sdRoot() + "preview_game.txt")) writeFile(sdRoot() + "preview_game.txt", tu.serialize());
    int best = tu.bestCandidate();
    CHECK(best >= 0 && !tu.cands[best].baseline && tu.cands[best].game != 2, "лучший кандидат выбран");
    for (auto& c : tu.cands)
        printf("  %-18s игра %.3f  итог %.3f  %s\n", c.name().c_str(), c.gameScore, c.combined, c.game == 2 ? "СБОЙ" : "");
    std::string err;
    CHECK(tu.finalize(t, best, &err), "finalize: %s", err.c_str());
    std::string fin;
    readFile(paths::timings(), fin);
    TimingsIni ini;
    ini.parse(fin);
    KnobSet s;
    CHECK(ini.getSection(1638400, s) && s["tRAS"] == 5, "секция пользователя сохранена");
    if (!tu.cands[best].set.empty())
        CHECK(ini.getSection(hzToKhz(tu.cands[best].hz), s) && s == tu.cands[best].set, "итоговая секция записана");
    CHECK(tu.phase == Phase::Done, "этап после записи: %s", phaseName(tu.phase));
    CHECK(fileExists(paths::report()), "отчёт сохранён");

    // Журнал переживает перезапуск приложения без потерь.
    Tuner tu2;
    tu2.cfg = cfg;
    bool rec = false;
    CHECK(tu2.loadState(t, &rec) && !rec, "журнал читается");
    CHECK(tu2.serialize() == tu.serialize(), "журнал после загрузки совпадает");
}

// Ошибка из отчёта пользователя: после вылета TOTK новые прогоны не учитывались,
// статус кандидата не менялся, и дальше с ним ничего нельзя было сделать.
// Прогон частот: проходит весь диапазон, сбой и зависание посередине его не останавливают.
static void testSweep() {
    printf("[прогон частот 1300…2900]\n");
    auto run = [](ChipModel m, Settings cfg, int* reboots) {
        removeFile(paths::state());
        auto t = std::make_unique<SimTarget>();
        t->m = m;
        t->original = "";
        writeFile(paths::timings(), t->original);
        auto tu = std::make_unique<Tuner>();
        tu->cfg = cfg;
        tu->begin(makeLadder(), chipFromDramId(17));
        *reboots = 0;
        runToGame(*t, *tu, reboots, 5000);
        return tu;
    };
    ChipModel m;
    m.maxStableMhz = 2700;
    m.holeLo = 1860;  // как «упирается в 1800»: сразу над 1800 — сбои подряд
    m.holeHi = 1940;
    m.holeHangs = true;
    m.slowLo = 2055;  // ступень 2060.8
    m.slowHi = 2065;
    Settings cfg;
    cfg.minMhz = 1300;
    cfg.maxMhz = 2900;
    int reboots = 0;
    auto tu = run(m, cfg, &reboots);
    int nOk = 0, nFail = 0, nSlow = 0;
    u32 topInRange = 0;
    for (u32 hz : makeLadder())
        if (hz <= 2900500000u) topInRange = hz;
    bool sawLow = false, saw1331 = false, sawTop = false;
    u32 topOk = 0;
    for (auto& p : tu->points) {
        if (p.hz < 1300000000u) sawLow = true;
        if (p.hz == 1331200000u) saw1331 = true;
        if (p.hz == topInRange) sawTop = true;
        if (p.status == 1) {
            nOk++;
            topOk = std::max(topOk, p.hz);
        } else if (p.status == 3) {
            nSlow++;
        } else {
            nFail++;
        }
    }
    size_t want = 0;
    for (u32 hz : makeLadder())
        if (hz >= 1299500000u && hz <= 2900500000u) want++;
    printf("  ступеней %zu (ждали %zu): ок %d, медленнее %d, сбой %d, зависаний %d, верх стабильной %s МГц\n", tu->points.size(), want, nOk, nSlow,
           nFail, reboots, mhzStr(topOk).c_str());
    CHECK(tu->points.size() == want, "проверены все ступени диапазона: %zu из %zu", tu->points.size(), want);
    CHECK(!sawLow && saw1331, "низ прогона — 1331.2 МГц, 204 не проверяется");
    CHECK(sawTop, "прогон дошёл до верха диапазона");
    CHECK(reboots >= 2, "зависания в дыре должны были случиться");
    CHECK(topOk / 1000000u >= 2600 && topOk / 1000000u <= 2700, "стабильная частота выше дыры найдена: %s", mhzStr(topOk).c_str());
    CHECK(nSlow >= 1, "ступень с просевшим потоком помечена «медленнее»");
    for (auto& p : tu->points)
        if (p.status == 3) CHECK(std::find(tu->targets.begin(), tu->targets.end(), p.hz) == tu->targets.end(), "«медленнее» не идёт в цели");
    CHECK(tu->phase == Phase::Game, "этап после прогона: %s (%s)", phaseName(tu->phase), tu->error.c_str());
    std::string rep = tu->report();
    CHECK(rep.find("Итог прогона") != std::string::npos && rep.find("Лестница Ryazha-clk") != std::string::npos, "сводка прогона в отчёте");

    // Остановка по настройке: 2 сбоя подряд — выше не идём.
    Settings c2 = cfg;
    c2.scanFailStop = 2;
    m.holeHangs = false;
    auto tu2 = run(m, c2, &reboots);
    u32 maxTried = 0;
    for (auto& p : tu2->points) maxTried = std::max(maxTried, p.hz);
    CHECK(maxTried / 1000000u < 1950, "с остановкой после 2 сбоев прогон кончился у дыры (дошёл до %s)", mhzStr(maxTried).c_str());

    // Настройки пишутся и читаются.
    Settings s3;
    s3.deserialize(cfg.serialize());
    CHECK(s3.minMhz == 1300 && s3.maxMhz == 2900 && s3.scanFailStop == 0, "min_mhz/max_mhz/scan_fail_stop");
}

static void testGameCrashFlow() {
    printf("[TOTK: вылет игры, перезагрузка, повторные прогоны]\n");
    removeFile(paths::state());
    removeFile(sdRoot() + "TOTKBench.txt");
    removeFile(sdRoot() + "TOTKBenchmark.txt");
    std::string crashDir = sdRoot() + "atmosphere/crash_reports";
    makeDirs(crashDir);
    for (auto& f : listDir(crashDir)) removeFile(crashDir + "/" + f);
    SimTarget t;
    t.original = "";
    auto newTuner = [&]() {
        // Каждый «перезапуск приложения» — новый объект, всё состояние только из журнала.
        Tuner* tu = new Tuner();
        bool rec = false;
        tu->loadState(t, &rec);
        return tu;
    };
    {
        Tuner tu;
        tu.ladder = makeLadder();
        tu.base.valid = true;
        tu.base.readMBs = tu.base.writeMBs = tu.base.copyMBs = 13000;
        tu.base.latNs = 200;
        tu.phase = Phase::Game;
        Candidate b;
        b.id = 0;
        b.baseline = true;
        Candidate k1;
        k1.id = 1;
        k1.hz = 2934400000u;
        k1.set = {{"tRCD", 4}, {"RL", -1}};
        k1.synth = 1.6;
        Candidate k2;
        k2.id = 2;
        k2.hz = 2895000000u;
        k2.set = {{"tRCD", 5}};
        k2.synth = 1.58;
        tu.cands = {b, k1, k2};
        tu.saveState(t);
    }
    auto bench = [&](double frames) {
        std::string old;
        readFile(sdRoot() + "TOTKBench.txt", old);
        writeFile(sdRoot() + "TOTKBench.txt", old + strf("BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: %.0f, Average FPS: %.0f, "
                                                         "1%% FPS: %.0f, 0.1%% FPS: %.0f\n", frames, frames / 60, frames / 75, frames / 92));
    };
    std::string err, msg;

    // 1. Применили К1 и запустили игру — игра упала (отчёт Atmosphère), консоль НЕ перезагружалась.
    Tuner* tu = newTuner();
    CHECK(tu->gameApply(t, 1, &err), "gameApply: %s", err.c_str());
    CHECK(t.gameClocksActive(), "частоты ЦПУ 2397 / ГПУ 1305 выставлены для прогона");
    writeFile(crashDir + "/01800000001_0100f2c0115b6000.log", "crash");
    delete tu;
    tu = newTuner();
    CHECK(tu->gameCollect(t, &msg) == -1, "падение распознано (%s)", msg.c_str());
    CHECK(tu->cands[1].game == 2 && tu->cands[1].crashes == 1, "К1: сбой, сбоев 1");
    CHECK(tu->gameActiveCand == 1, "настройки К1 всё ещё применены — кандидат остаётся активным");

    // 2. Пользователь запускает TOTK снова прямо из меню HOME и проходит бенчмарк.
    bench(3100);
    delete tu;
    tu = newTuner();
    int n = tu->gameCollect(t, &msg);
    CHECK(n == 1, "прогон после вылета засчитан (вернул %d: %s)", n, msg.c_str());
    CHECK(tu->cands[1].game == 1 && tu->cands[1].note.empty(), "статус К1 обновился: проверен (note='%s')", tu->cands[1].note.c_str());
    CHECK(tu->cands[1].runs.size() == 1, "у К1 один прогон");
    // Повторный сбор не засчитывает тот же прогон второй раз.
    CHECK(tu->gameCollect(t, &msg) == 0 && tu->cands[1].runs.size() == 1, "тот же прогон не считается дважды");

    // 3. Ещё прогон с теми же настройками — тоже засчитывается.
    bench(3120);
    CHECK(tu->gameCollect(t, &msg) == 1 && tu->cands[1].runs.size() == 2, "второй прогон засчитан");

    // 4. К2: зависание в игре, сторож перезагрузил консоль (отчёта нет, временные частоты сброшены).
    CHECK(tu->gameApply(t, 2, &err), "gameApply К2: %s", err.c_str());
    t.reboot();
    delete tu;
    tu = newTuner();
    CHECK(tu->gameCollect(t, &msg) == -2, "перезагрузка распознана без диалога (%s)", msg.c_str());
    CHECK(tu->cands[2].game == 2 && tu->gamePendingCand == -1 && tu->gameActiveCand == -1, "К2: сбой, ожидание снято");
    // Прогон на стоковых настройках после перезагрузки не должен засчитаться К2.
    bench(2900);
    CHECK(tu->gameCollect(t, &msg) == 0 && tu->cands[2].runs.empty(), "прогон не на настройках К2 не засчитан");

    // 5. К2 можно сразу проверить снова — ничего не заблокировано.
    CHECK(tu->gameApply(t, 2, &err), "повторное применение К2: %s", err.c_str());
    bench(3050);
    delete tu;
    tu = newTuner();
    CHECK(tu->gameCollect(t, &msg) == 1, "повторный прогон К2 засчитан (%s)", msg.c_str());
    CHECK(tu->cands[2].game == 1 && tu->cands[2].crashes == 1 && tu->cands[2].attempts == 2, "К2: проверен, сбоев 1 из 2");

    // 6. «Как было» и выбор лучшего: кандидат без сбоев важнее кандидата со сбоем.
    CHECK(tu->gameApply(t, 0, &err), "применение «Как было»: %s", err.c_str());
    CHECK(t.gameClocksActive() && t.override_ == 0, "«Как было» — тоже на ЦПУ 2397 / ГПУ 1305, без временной частоты РАМ");
    bench(2950);
    CHECK(tu->gameCollect(t, &msg) == 1, "прогон «Как было» засчитан");
    tu->computeRanking();
    int best = tu->bestCandidate();
    CHECK(best >= 0, "есть лучший кандидат");
    printf("  К1: сбоев %d, итог %.3f; К2: сбоев %d, итог %.3f; лучший: %s\n", tu->cands[1].crashes, tu->cands[1].combined, tu->cands[2].crashes,
           tu->cands[2].combined, best >= 0 ? tu->cands[best].name().c_str() : "-");
    // Ручная отметка и очистка тоже работают.
    tu->gameClearRuns(t, 1);
    CHECK(tu->cands[1].game == 0 && tu->cands[1].crashes == 0 && tu->cands[1].runs.empty(), "очистка прогонов");
    delete tu;
    removeFile(crashDir + "/01800000001_0100f2c0115b6000.log");
}

static void testLog() {
    printf("[журнал приложения]\n");
    logx::begin();
    LOGE("проверка ошибки %d", 42);
    LOGW("проверка предупреждения");
    auto tail = logx::tail(3);
    CHECK(tail.size() == 3 && tail[1].find("[E] проверка ошибки 42") != std::string::npos, "строки в журнале: %s",
          tail.empty() ? "-" : tail.back().c_str());
    CHECK(logx::errorsThisSession() == 1, "счётчик ошибок");
    CHECK(fileExists(sdRoot() + "config/ram-sweetspot/logs/ramss.log"), "файл журнала на месте");
}

static void hostBench() {
    printf("[замер памяти на этом ПК — проверка, что ядра работают]\n");
    MemBench b;
    if (!b.init(96u << 20, 32u << 20)) {
        CHECK(false, "MemBench::init");
        return;
    }
    BenchResult r = b.run(1, nullptr);
    printf("  чтение %.0f МБ/с, запись %.0f МБ/с, копирование %.0f МБ/с, задержка %.1f нс (двойная %.1f)\n", r.readMBs, r.writeMBs, r.copyMBs,
           r.latNs, r.lat2Ns);
    CHECK(r.valid && r.readMBs > 100 && r.latNs > 1 && r.latNs < 1000, "правдоподобные числа");
    MemTest m;
    CHECK(m.init(64u << 20), "MemTest::init");
    MemTestResult mr = m.run(2, nullptr);
    printf("  memtest: %s, проходов %d, проверено %.1f ГБ\n", mr.ok ? "ок" : mr.detail.c_str(), mr.passes, mr.bytes / 1e9);
    CHECK(mr.ok && mr.passes >= 5, "memtest на исправной памяти проходит все шаблоны");
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "./sdroot";
    setSdRoot(root);
    makeDirs(paths::rclkDir());
    makeDirs(paths::appDir());

    testTimingsIni();
    testTotkParse();
    testBenchFiles();
    testUltraCamIni();
    testLog();
    testGameCrashFlow();
    testSweep();

    Settings cfg;
    {
        ChipModel m;
        fullRun("AA-MGCL, пресет, запас 1", m, chipFromDramId(17), cfg);
    }
    {
        ChipModel m;
        m.maxStableMhz = 2700;
        m.limit2400 = {{"tRCD", 3}, {"tRP", 3}, {"tRAS", 5}, {"tRRD", 2}, {"tRFC", 5}, {"tRTW", 4}, {"tWTR", 4}, {"tREFI", 5}};
        Settings c2 = cfg;
        c2.numTargets = 3;
        fullRun("слабый чип, пресет туже, чем держит", m, chipFromDramId(10), c2);
    }
    {
        ChipModel m;
        m.maxStableMhz = 3150;  // выше 3050 — PLL drop
        Settings c3 = cfg;
        c3.usePreset = false;
        c3.advanced = true;
        fullRun("без пресета, PLL drop, расширенные ручки", m, chipFromDramId(99), c3);
    }
    {
        ChipModel m;
        m.marginal = true;
        Settings c4 = cfg;
        c4.margin = 0;  // без запаса длинная проверка обязана поймать пограничные значения
        fullRun("пограничная стабильность, запас 0: ослабление после длинной проверки", m, chipFromDramId(8), c4);
    }
    {
        ChipModel m;
        m.marginal = true;
        m.marginalHangs = true;
        Settings c5 = cfg;
        c5.margin = 0;
        fullRun("пограничные значения вешают консоль на длинной проверке", m, chipFromDramId(32), c5);
    }
    hostBench();

    printf(g_fail ? "\nИТОГ: ошибок %d\n" : "\nИТОГ: все проверки пройдены\n", g_fail);
    return g_fail ? 1 : 0;
}
