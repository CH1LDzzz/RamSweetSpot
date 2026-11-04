// Проверка алгоритма автоподбора на ПК: симулятор консоли с моделью памяти.
// Сборка и запуск: make -C tests

#include <algorithm>
#include <memory>
#include <set>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>

#include "../source/archive.hpp"
#include "../source/common.hpp"
#include "../source/history.hpp"
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
    // «Стена стока»: на стоковых таймингах Ryazha-clk (все первичные на ступени 0) выше этой частоты — ошибки memtest.
    double stockWallMhz = 0;
    // Ручка (живьём или из кипа) с ненулевым значением выше этой частоты вешает память — как HP Mode на 2934.4.
    std::map<std::string, double> hangAbove;
    // Обученные общие тайминги из кипа (не заданы живьём): выше этой частоты — ошибки memtest. 0 — нет предела.
    double trainedWallMhz = 0;
    // Кип Ryazha-clk: ручки пропускной способности (слот tMEH -> значение), действующие с этой загрузки.
    std::map<int, int> tmeh;
    std::set<int> kipBootBad;  // ненулевое значение слота в кипе вешает консоль на загрузке
    std::set<int> kipMemBad;   // ненулевое значение слота в кипе даёт ошибки memtest
    std::set<int> kipHangBad;  // ненулевое значение слота в кипе вешает память под нагрузкой (после загрузки)

    int effective(const KnobSet& s, const std::string& k) const {
        auto it = s.find(k);
        if (it != s.end()) return it->second;
        int slot = kipSlotOf(k);
        if (slot >= 0) {
            auto t = tmeh.find(slot);
            if (t != tmeh.end()) return t->second;
        }
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
        for (auto& hb : hangAbove)
            if (mhz > hb.second && effective(s, hb.first) != 0) return 2;
        if (trainedWallMhz > 0 && mhz > trainedWallMhz)
            for (auto& kv : kip)
                if (!s.count(kv.first) && kv.second > 0) return 1;
        if (stockWallMhz > 0 && mhz > stockWallMhz && s.count("tRCD") && s.at("tRCD") == 0 && s.count("tRAS") && s.at("tRAS") == 0) return 1;
        for (int slot : kipMemBad) {
            auto t = tmeh.find(slot);
            if (t != tmeh.end() && t->second != 0) return 1;
        }
        for (int slot : kipHangBad) {
            auto t = tmeh.find(slot);
            if (t != tmeh.end() && t->second != 0) return 2;
        }
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
    BenchResult perf(double mhz, const KnobSet& s, std::mt19937& rng, double sigma = 0.0012) const {
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
        std::normal_distribution<double> n(1.0, sigma);
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
        virtMs += 500;
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
        virtMs += (u64)seconds * 1000;
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
    // Проходы как на консоли (1.8.4): шум прохода по оценке ~0,14 % и медленный дрейф (нагрев, фон) около 0,1 %,
    // меняющийся за несколько проб. Выкл. — как раньше: один замер на любое число проходов.
    bool realNoise = false;
    double drift = 0;
    BenchResult bench(int passes, bool* aborted) override {
        *aborted = false;
        if (hangArmed) {
            hangArmed = false;
            hangs++;
            throw Hang();
        }
        if (!realNoise) return m.perf(realHz() / 1e6, curSet, rng);
        std::normal_distribution<double> n(0.0, 1.0);
        std::vector<BenchPass> ps;
        for (int i = 0; i < std::max(1, passes); i++) {
            virtMs += 1000;  // проход на консоли — около секунды
            drift = drift * 0.98 + 0.0002 * n(rng);
            BenchResult b = m.perf(realHz() / 1e6, curSet, rng, 0.0027);
            double k = 1 + drift;
            BenchPass p;
            p.r = b.readMBs * k;
            p.w = b.writeMBs * k;
            p.c = b.copyMBs * k;
            p.l = b.latNs / k;
            p.l2 = b.lat2Ns / k;
            ps.push_back(p);
        }
        return aggregatePasses(ps);
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
    void freeClocks() override { cpuOv = gpuOv = 0; }
    int socTempC() override { return 45; }
    bool saveText(const std::string& path, const std::string& text) override {
        makeDirs(paths::appDir());
        return writeFileSafe(path, text);
    }
    bool loadText(const std::string& path, std::string& text) override { return readFileSafe(path, text); }
    u64 virtMs = 600000;  // виртуальные часы: консоль работает уже 10 минут
    u64 bootVirt = 0;
    void idle(u32 ms) override { virtMs += ms; }

    // --- общие тайминги кипа: kipCommonCfg — записано, m.kip — действует с загрузки (обучено)
    bool commonSupported = false;  // по умолчанию как в старых тестах: «не читаются» — этап «кип сначала» пропускается
    KnobSet kipCommonCfg;
    bool kipCommonRead(KnobSet* vals, std::string*) override {
        if (!commonSupported) return false;
        *vals = kipCommonCfg;
        return true;
    }
    bool kipCommonWrite(const KnobSet& vals, std::string*) override {
        for (auto& kv : vals) kipCommonCfg[kv.first] = kv.second;
        return true;
    }

    // --- кип: config — что записано (демон отдаёт его), m.tmeh — что действует с последней загрузки
    std::map<int, int> kipConfig;
    bool rebootRequested = false;
    int boots = 0, bootHangs = 0;
    bool kipRead(std::map<int, int>* slots, std::string*) override {
        slots->clear();
        for (int i = 0; i < 28; i++) (*slots)[i] = kipConfig.count(i) ? kipConfig[i] : 0;
        return true;
    }
    bool kipWrite(const std::map<int, int>& slots, std::string*) override {
        for (auto& kv : slots) kipConfig[kv.first] = kv.second;
        return true;
    }
    bool reboot(std::string*) override {
        rebootRequested = true;
        return true;
    }
    u64 uptimeMs() override { return virtMs - bootVirt; }
    int hangBoots = 0;
    s64 bootEpoch() override { return 1000000 + (s64)(boots + hangBoots) * 600; }  // часы симулятора виртуальные — загрузку считаем сами
    // Загрузка после перезагрузки через систему: лоадер берёт кип; виснет — сторож откатывает правку.
    void systemBoot() {
        rebootRequested = false;
        boots++;
        powerCycle();
        bool hang = false;
        for (int slot : m.kipBootBad)
            if (kipConfig.count(slot) && kipConfig[slot] != 0) hang = true;
        if (hang) {
            bootHangs++;
            kipConfig = m.tmeh;  // на правку назад — к тому, с чем консоль грузилась прежде
        } else {
            m.tmeh = kipConfig;
            for (auto& kv : kipCommonCfg) m.kip[kv.first] = kv.second;
        }
        virtMs += 30000;  // загрузка
        bootVirt = virtMs;
    }
    void powerCycle() {
        hangBoots++;  // зависание и перезапуск сторожем — другая загрузка
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
            if (!tu.step(t, &stop)) {
                if (!t.rebootRequested) return true;
                // Приложение перезагрузило консоль: загрузка, затем пользователь снова открывает RamSweetSpot.
                t.systemBoot();
                tu.resetAll();
                tu.ladder.clear();
                bool rec = false;
                if (!tu.loadState(t, &rec)) {
                    printf("  не удалось загрузить журнал после перезагрузки (кип)\n");
                    return false;
                }
                steps++;
                continue;
            }
            steps++;
            // Снимок посреди подбора таймингов — для предпросмотра экранов.
            if (tu.phase == Phase::Timing && tu.kIdx >= 3 && !fileExists(sdRoot() + "preview_timing.txt"))
                writeFile(sdRoot() + "preview_timing.txt", tu.serialize());
        } catch (Hang&) {
            // «Перезагрузка»: приложение умерло посреди пробы, журнал остался на карте.
            (*reboots)++;
            t.powerCycle();
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
    bool opt = false;
    for (auto& e : tu.events)
        if (e.find("оптимизация закончена") != std::string::npos || e.find("Оптимизация:") != std::string::npos) opt = true;
    if (tu.cfg.optimize) CHECK(opt, "этап оптимизации таймингов пройден");
    std::string reg;
    CHECK(readFile(paths::unstable(), reg) && split(reg, '\n').size() > 2, "сбои записаны в реестр нестабильных");
    std::string tl;
    CHECK(readFile(logx::testFile(), tl) && tl.find("✔ стабильно") != std::string::npos && tl.find("memtest") != std::string::npos,
          "журнал испытаний пишется");
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
// Второй прогон на таймингах гайда: на стоке память упирается в 2400 МГц, с гайдом идёт выше.
static void testGuideSweep() {
    printf("[второй прогон на таймингах гайда]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    t.m.stockWallMhz = 2400;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.cfg.kipStage = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    u32 topStock = 0, topGuide = 0, firstGuide = 0;
    for (auto& p : tu.points) {
        if (p.status != 1) {
            if (p.pass == 1 && !firstGuide) firstGuide = p.hz;
            continue;
        }
        if (p.pass == 0) topStock = std::max(topStock, p.hz);
        if (p.pass == 1) {
            topGuide = std::max(topGuide, p.hz);
            if (!firstGuide) firstGuide = p.hz;
        }
    }
    for (auto& p : tu.points)
        if (p.pass == 1 && (!firstGuide || p.hz < firstGuide)) firstGuide = p.hz;
    printf("  сток до %s МГц, гайд до %s МГц (гайд начат с %s), цели: ", mhzStr(topStock).c_str(), mhzStr(topGuide).c_str(), mhzStr(firstGuide).c_str());
    for (u32 hz : tu.targets) printf("%s ", mhzStr(hz).c_str());
    printf("\n");
    CHECK(topStock / 1000000u <= 2400 && topStock / 1000000u >= 2300, "сток упёрся в стену ~2400 (%s)", mhzStr(topStock).c_str());
    CHECK(topGuide > topStock, "на гайде частота выше");
    CHECK(tu.guideChosen, "цели взяты из прогона на гайде");
    CHECK(firstGuide == 1331200000u, "второй прогон — по всему диапазону, с 1331.2 МГц (%s)", mhzStr(firstGuide).c_str());
    CHECK(!tu.targets.empty() && tu.targets[0] == topGuide, "первая цель — предел гайда");
    CHECK(tu.phase == Phase::Game, "дошли до TOTK: %s (%s)", phaseName(tu.phase), tu.error.c_str());
    bool real = false;
    for (auto& c : tu.cands)
        if (!c.baseline && c.hz > topStock) real = true;
    CHECK(real, "кандидат выше стены стока");
    std::string rep = tu.report();
    CHECK(rep.find("гайд") != std::string::npos, "прогон на гайде в отчёте");

    // Выключено — только сток.
    removeFile(paths::state());
    SimTarget t2;
    t2.m.stockWallMhz = 2400;
    Tuner tu2;
    tu2.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu2.cfg.kipStage = false;
    tu2.cfg.scanGuide = false;
    tu2.begin(makeLadder(), chipFromDramId(17));
    runToGame(t2, tu2, &reboots, 8000);
    bool anyGuide = false;
    for (auto& p : tu2.points)
        if (p.pass == 1) anyGuide = true;
    CHECK(!anyGuide && !tu2.guideChosen, "без второго прогона — только сток");
    removeFile(paths::unstable());
}

// Предохранитель кипа: значение, принятое на одной частоте, проверяется живьём на других частотах кандидатов до записи.
static void testKipGuard() {
    printf("[кип: живая проверка до записи]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(paths::kipBackup());
    SimTarget t;
    t.m.hangAbove["ramPD"] = 2900;  // HP Mode держится на 2600, вешает 2933
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.ladder = makeLadder();
    tu.chip = chipFromDramId(17);
    tu.base.valid = true;
    tu.base.readMBs = tu.base.writeMBs = tu.base.copyMBs = 13000;
    tu.base.latNs = 200;
    Candidate k1;
    k1.id = 1;
    k1.hz = 2598400000u;
    k1.set = {{"tRCD", 3}, {"ramPD", 1}, {"daTurns", 2}};
    k1.synth = 1.6;
    Candidate k2;
    k2.id = 2;
    k2.hz = 2933000000u;
    k2.set = {{"tRCD", 3}};
    k2.synth = 1.5;
    tu.cands = {k1, k2};
    tu.phase = Phase::Seed;  // цели кончились — следующий шаг войдёт в перенос в кип
    tu.tIdx = 0;
    tu.saveState(t);
    int reboots = 0;
    runToGame(t, tu, &reboots, 2000);
    bool rejectedLive = false;
    for (auto& r : tu.kipRejected)
        if (r.key == "ramPD" && r.reason.find("живьём") != std::string::npos) rejectedLive = true;
    for (auto& l : tu.kipLog) printf("    · %s\n", l.c_str());
    printf("  зависаний (живьём): %d, перезагрузок для кипа: %d, в кипе: ramPD=%d daTurns=%d\n", reboots, t.boots, t.m.tmeh[14], t.m.tmeh[26]);
    CHECK(rejectedLive, "HP Mode отклонён живой проверкой на 2933 до записи в кип");
    CHECK(t.m.tmeh[14] == 0, "HP Mode в кип не попал");
    CHECK(t.m.tmeh[26] == 2, "daTurns прошёл живую проверку и записан в кип");
    CHECK(tu.phase == Phase::Game, "этап: %s (%s)", phaseName(tu.phase), tu.error.c_str());

    // Зависание после загрузки с новым кипом: виноват кип — откат сразу, тайминги не отжимаются.
    {
        removeFile(paths::state());
        removeFile(paths::kipBackup());
        SimTarget t3;
        t3.m.kipHangBad = {26};  // daTurns из кипа вешает память
        Tuner tu3;
        tu3.cfg.communityBands = false;  // модель чипа — под Common гайда
        tu3.ladder = makeLadder();
        tu3.chip = chipFromDramId(17);
        tu3.base = tu.base;
        Candidate k4 = k1;
        k4.set = {{"tRCD", 3}, {"daTurns", 2}};
        tu3.cands = {k4};
        tu3.phase = Phase::Seed;
        int rb = 0;
        runToGame(t3, tu3, &rb, 2000);
        bool loosened = false, culprit = false;
        for (auto& l : tu3.kipLog) {
            if (l.find("отжимаю тайминги") != std::string::npos) loosened = true;
            if (l.find("виновник: daTurns") != std::string::npos) culprit = true;
        }
        printf("  зависание после загрузки: зависаний %d, перезагрузок %d, виновник найден %d, отжим таймингов %d\n", rb, t3.boots, culprit, loosened);
        CHECK(culprit && !loosened, "зависание с кипом — сразу виновник, без отжима таймингов");
        CHECK(t3.m.tmeh[26] == 0 && tu3.phase == Phase::Game, "daTurns убран из кипа, этап закончен");
    }

    // Сторож выключен — перенос в кип пропускается, кип не трогаем.
    struct NoWd : SimTarget {
        int watchdogState() override { return 0; }
    } t2;
    removeFile(paths::state());
    removeFile(paths::kipBackup());
    Tuner tu2;
    tu2.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu2.ladder = makeLadder();
    tu2.chip = chipFromDramId(17);
    tu2.base = tu.base;
    Candidate k3 = k1;
    k3.set = {{"tRCD", 3}, {"daTurns", 2}};
    tu2.cands = {k3};
    tu2.phase = Phase::Seed;
    runToGame(t2, tu2, &reboots, 2000);
    CHECK(tu2.phase == Phase::Game && t2.boots == 0 && t2.kipConfig.empty(), "без сторожа кип не трогается");
    removeFile(paths::unstable());
    removeFile(paths::kipBackup());
}

// «Кип сначала»: общие тайминги гайда в кип до прогонов; частоты, что на них не держатся, отбрасываются.
// Кип сначала с границами демона Ryazha-clk 3.6.0: tRRD/tRFC ≤ 3, tRTW/tWTR/tREFI кипом не задаются.
static void testKipFirstLimits() {
    printf("[кип сначала: границы демона]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(paths::kipCommonBackup());
    struct Lim : SimTarget {
        bool kipCommonMax(const std::string& k, int* mx) override {
            static const std::map<std::string, int> lim = {{"tRCD", 4}, {"tRP", 4}, {"tRAS", 8}, {"tRRD", 3},
                                                           {"tRFC", 3}, {"tRTW", 0}, {"tWTR", 0}, {"tREFI", 0}};
            auto it = lim.find(k);
            if (it == lim.end()) return false;
            *mx = it->second;
            return true;
        }
    } t;
    t.commonSupported = true;
    t.kipCommonCfg = {{"tRCD", 0}, {"tRP", 0}, {"tRAS", 0}, {"tRRD", 0}, {"tRFC", 0}, {"tRTW", 0}, {"tWTR", 0}, {"tREFI", 0}};
    t.m.kip.clear();
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.kipStage = false;
    tu.cfg.scanHangStop = 0;
    tu.begin(makeLadder(), chipFromDramId(17));  // гайд AA: tRCD4 tRP4 tRAS5 tRRD4 tRFC5 tRTW5 tWTR6
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    printf("  в кипе: %s; kfDone %d\n", knobSetShort(t.kipCommonCfg).c_str(), tu.kfDone ? 1 : 0);
    CHECK(tu.kfDone, "кип сначала прошёл (раньше: «демон не принял общие тайминги»)");
    CHECK(t.kipCommonCfg["tRFC"] == 3 && t.kipCommonCfg["tRRD"] == 3 && t.kipCommonCfg["tRCD"] == 4 && t.kipCommonCfg["tRAS"] == 5,
          "обрезано по границам: %s", knobSetShort(t.kipCommonCfg).c_str());
    CHECK(t.kipCommonCfg["tRTW"] == 0 && t.kipCommonCfg["tWTR"] == 0 && !tu.kfVals.count("tRTW") && !tu.kfVals.count("tWTR"),
          "tRTW/tWTR кипом не задаются — не тронуты");
    removeFile(paths::unstable());
}

static void testKipFirst() {
    printf("[кип сначала]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(paths::kipCommonBackup());
    SimTarget t;
    t.commonSupported = true;
    t.kipCommonCfg = {{"tRCD", 0}, {"tRP", 0}, {"tRAS", 0}, {"tRRD", 0}, {"tRFC", 0}, {"tRTW", 0}, {"tWTR", 0}, {"tREFI", 0}};
    t.m.kip.clear();
    t.m.trainedWallMhz = 2700;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.cfg.kipStage = false;
    tu.cfg.scanHangStop = 0;
    tu.begin(makeLadder(), chipFromDramId(17));
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    bool livePrimary = false;
    for (auto& e : tu.events)
        if (e.find("Частота") != std::string::npos && e.find("tRCD") != std::string::npos) livePrimary = true;
    u32 topOk = 0;
    for (auto& p : tu.points)
        if (p.status == 1) topOk = std::max(topOk, p.hz);
    bool overTarget = false;
    for (u32 hz : tu.targets)
        if (hz > 2700000000u) overTarget = true;
    KnobSet guide;
    for (auto& kv : presetToKnobs(chipFromDramId(17).preset->common)) guide[kv.first] = kv.second;
    printf("  в кипе: %s; перезагрузок %d; стабильно до %s МГц; цели:", knobSetShort(t.m.kip).c_str(), t.boots, mhzStr(topOk).c_str());
    for (u32 hz : tu.targets) printf(" %s", mhzStr(hz).c_str());
    printf("\n");
    CHECK(tu.kfDone && t.boots == 1, "общие тайминги гайда в кипе, одна перезагрузка");
    CHECK(t.m.kip["tRCD"] == guide["tRCD"] && t.m.kip["tWTR"] == guide["tWTR"], "в кипе — значения гайда");
    CHECK(!livePrimary, "прогон без живых первичных таймингов");
    CHECK(topOk <= 2700000000u && tu.hzDiscarded(2752000000u) && !overTarget, "частоты выше 2700 отброшены, цели не выше");
    CHECK(tu.phase == Phase::Game, "этап: %s (%s)", phaseName(tu.phase), tu.error.c_str());
    bool startKip = false;
    for (auto& c : tu.cands)
        if (!c.baseline && c.set.count("tRCD") && c.set.at("tRCD") >= guide["tRCD"]) startKip = true;
    CHECK(startKip, "доводка стартовала с таймингов кипа");
    std::string bk;
    CHECK(readFile(paths::kipCommonBackup(), bk) && bk.find("tRCD:0") != std::string::npos, "копия общих таймингов до правки");

    // Без сторожа — пропуск.
    struct NoWd : SimTarget {
        int watchdogState() override { return 0; }
    } t2;
    t2.commonSupported = true;
    removeFile(paths::state());
    Tuner tu2;
    tu2.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu2.cfg.kipStage = false;
    tu2.begin(makeLadder(), chipFromDramId(17));
    runToGame(t2, tu2, &reboots, 8000);
    CHECK(!tu2.kfDone && tu2.kfTried && t2.boots == 0, "без сторожа «кип сначала» пропущен");
    removeFile(paths::unstable());
    removeFile(paths::kipCommonBackup());
}

// Исследование потока: повторные прогоны, зависание посередине — продолжение после «перезагрузки», таблица.
static void testBands() {
    printf("[проверенный набор Samsung AA по полосам частот]\n");
    const ChipInfo aa = chipFromDramId(17);
    CHECK(aa.preset && aa.preset->bands, "у AA-MGCL есть проверенный набор");
    // WL: 1333 при автоматике 16 (до 1862.4) — −2, при 18 — −3; 1600 — −2; 1866 — −1; сток и 1600 МГц — 0.
    CHECK(wlStepFor(1638400000u, 1333) == -2, "WL 1638.4: %d", wlStepFor(1638400000u, 1333));
    CHECK(wlStepFor(1996800000u, 1333) == -3, "WL 1996.8: %d", wlStepFor(1996800000u, 1333));
    CHECK(wlStepFor(2368000000u, 1600) == -2 && wlStepFor(2560000000u, 1866) == -1, "WL 2368 / 2560");
    CHECK(wlStepFor(1600000000u, 1333) == 0 && wlStepFor(2828800000u, 0) == 0, "WL сток");
    struct {
        u32 hz;
        int tRAS, tRRD, tRTW, tWTR, wl;
    } cases[] = {{1638400000u, 8, 5, 5, 6, -2}, {2291200000u, 8, 5, 5, 6, -3}, {2329600000u, 8, 5, 6, 7, -3},
                 {2521600000u, 8, 5, 6, 7, -2}, {2560000000u, 8, 5, 6, 7, -1}, {2790400000u, 8, 5, 6, 7, -1},
                 {2828800000u, 8, 5, 6, 7, 0}};
    for (auto& c : cases) {
        KnobSet k = presetKnobsAt(aa.preset, c.hz, true);
        CHECK(k["tRCD"] == 4 && k["tRP"] == 4 && k["tRFC"] == 5 && k["tRAS"] == c.tRAS && k["tRRD"] == c.tRRD && k["tRTW"] == c.tRTW &&
                  k["tWTR"] == c.tWTR && k["WL"] == c.wl && !k.count("tREFI"),
              "%s МГц: %s", mhzStr(c.hz).c_str(), knobSetShort(k).c_str());
    }
    KnobSet common = presetKnobsAt(aa.preset, 2560000000u, false);
    CHECK(common == presetToKnobs(aa.preset->common) && !common.count("WL"), "выключено — Common гайда");
    CHECK(!chipFromDramId(20).preset->bands && presetKnobsAt(chipFromDramId(20).preset, 2560000000u, true) ==
                                                     presetToKnobs(chipFromDramId(20).preset->common),
          "у чипов без набора — Common");
    Tuner tu;
    tu.cfg.communityBands = true;  // набор пока выключен по умолчанию — проверяем его явно
    tu.begin(makeLadder(), aa);
    CHECK(tu.bandsOn() && tu.guideAt(2560000000u)["WL"] == -1 && tu.guideAt(2560000000u)["RL"] == 0, "guideAt с полосами");
    KnobSet ks = tu.kipScanSet(2000000000u);
    CHECK(ks["WL"] == -3 && !ks.count("tRAS"), "кип сначала: живьём только WL по полосе");
    tu.cfg.communityBands = false;
    CHECK(!tu.bandsOn() && tu.guideAt(2560000000u)["WL"] == 0 && tu.guideAt(2560000000u)["tRAS"] == 5, "guideAt без полос");
    printf("  %s\n", bandsText(aa.preset).c_str());

    // Полный прогон с полосами: память, что держит проверенный набор (tRRD 5 на 2400), сток упирается в 2400.
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    t.m.stockWallMhz = 2400;
    t.m.limit2400["tRRD"] = 5;
    writeFile(paths::timings(), "");
    Tuner tb;
    tb.cfg.communityBands = true;
    tb.cfg.kipStage = false;
    tb.cfg.kipFirst = false;
    tb.begin(makeLadder(), aa);
    int reboots = 0;
    runToGame(t, tb, &reboots, 8000);
    u32 topStock = 0, topGuide = 0;
    for (auto& p : tb.points)
        if (p.status == 1) (p.pass ? topGuide : topStock) = std::max(p.pass ? topGuide : topStock, p.hz);
    printf("  с полосами: сток до %s МГц, гайд до %s МГц, цели:", mhzStr(topStock).c_str(), mhzStr(topGuide).c_str());
    for (u32 hz : tb.targets) printf(" %s", mhzStr(hz).c_str());
    printf("\n");
    CHECK(topGuide > topStock && tb.guideChosen, "на полосах частота выше стока");
    CHECK(tb.phase == Phase::Game, "дошли до TOTK: %s (%s)", phaseName(tb.phase), tb.error.c_str());
    CHECK(!tb.targets.empty() && tb.presetAt(tb.targets[0]) == tb.guideAt(tb.targets[0]) &&
              tb.presetAt(tb.targets[0])["WL"] == presetKnobsAt(aa.preset, tb.targets[0], true)["WL"],
          "старт цели — набор её полосы");
    CHECK(tb.report().find("по полосам частот") != std::string::npos, "в отчёте — какой набор взят");
}

static void testResearch() {
    printf("[исследование потока]\n");
    SimTarget t;
    t.m.holeLo = 2135;  // в дыре только 2137.6 (2133.0 — ниже)
    t.m.holeHi = 2140;
    t.m.holeHangs = true;
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.cfg.minMhz = 2000;
    tu.cfg.maxMhz = 2300;
    tu.cfg.researchRuns = 2;
    tu.cfg.scanHangStop = 0;
    Tuner::researchStart(2);
    std::atomic<bool> stop{false};
    std::string msg;
    bool ok = false;
    int hangs = 0;
    for (int guard = 0; guard < 10 && !ok; guard++) {
        try {
            ok = tu.research(t, makeLadder(), chipFromDramId(17), &stop, &msg);
        } catch (Hang&) {
            hangs++;
            t.powerCycle();
            std::string what;
            CHECK(Tuner::researchRecoverHang(&what), "зависание отмечено при перезапуске");
        }
    }
    CHECK(ok && !Tuner::researchActive(), "исследование закончено: %s", msg.c_str());
    std::string csv, txt;
    readFile(paths::researchDir() + "research.csv", csv);
    readFile(paths::researchDir() + "research.txt", txt);
    int okRows = 0, hangRows = 0;
    for (auto& l : split(csv, '\n')) {
        if (l.find(",ok,") != std::string::npos) okRows++;
        if (l.find(",hang,") != std::string::npos) hangRows++;
    }
    int steps = 0;
    for (u32 hz : makeLadder())
        if (hz >= 1999500000u && hz <= 2300500000u) steps++;
    printf("  ступеней %d, строк ok %d, зависаний %d (перезапусков %d)\n", steps, okRows, hangRows, hangs);
    CHECK(hangRows == 4 && hangs == 4, "зависшая ступень — в каждом повторе и прогоне, продолжение после перезапуска");
    CHECK(okRows == 2 * 2 * (steps - 1), "все остальные ступени сделаны: %d", okRows);
    CHECK(txt.find("сток Ryazha-clk") != std::string::npos && txt.find("гайда") != std::string::npos && txt.find("σ") != std::string::npos,
          "таблица: сток, гайд, разброс");
    printf("%s", txt.c_str());
}

// Подбор одной частоты: без прогонов, цель — только она; сток там не держится — старт с гайда.
static void testSingle() {
    printf("[подбор одной частоты]\n");
    for (int wall = 0; wall < 2; wall++) {
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        if (wall) t.m.stockWallMhz = 2300;  // 2400 на стоке не держится
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.communityBands = false;  // модель чипа — под Common гайда
        tu.cfg.kipStage = false;
        tu.begin(makeLadder(), chipFromDramId(17));
        tu.singleHz = 2400000000u;
        int reboots = 0;
        runToGame(t, tu, &reboots, 8000);
        int real = 0;
        u32 hz = 0;
        for (auto& c : tu.cands)
            if (!c.baseline) {
                real++;
                hz = c.hz;
            }
        // Журнал событий хранит последние 80 строк — старт ищем в журнале испытаний.
        std::string tl, tlOld;
        readFile(logx::testFile(), tl);
        readFile(paths::appDir() + "logs/test.old.log", tlOld);  // журнал мог уйти в ротацию
        tl = tlOld + tl;
        size_t from = tl.rfind("Подбор одной частоты: 2400.0");
        bool guideStart = from != std::string::npos && tl.find("пробую тайминги гайда", from) != std::string::npos;
        printf("  %s: проб %d, кандидат %s МГц, точек прогона %zu\n", wall ? "стена стока 2300" : "сток держит", tu.trials, mhzStr(hz).c_str(),
               tu.points.size());
        CHECK(tu.points.empty(), "прогонов частот не было");
        CHECK(tu.targets.size() == 1 && tu.targets[0] == 2400000000u, "цель — только выбранная частота");
        CHECK(tu.phase == Phase::Game && real == 1 && hz == 2400000000u, "кандидат на выбранной частоте (%s, %s)", phaseName(tu.phase), tu.error.c_str());
        if (wall) CHECK(guideStart, "сток на 2400 не держится — старт с таймингов гайда");
        // Журнал: режим переживает перезапуск приложения.
        Tuner tu2;
        tu2.cfg.communityBands = false;  // модель чипа — под Common гайда
        bool rec = false;
        tu2.loadState(t, &rec);
        CHECK(tu2.singleHz == 2400000000u, "режим одной частоты в журнале");
    }
    removeFile(paths::unstable());
}

// Пробы таймингов — с 5 замерами; спорный прирост (между половиной порога и порогом) замеряется ещё раз.
// Смотрит журнал испытаний после testAbsLimits: там у tRAS прирост за ступень ~0.2 % — около порога шума.
static void testConfirmGain() {
    printf("[5 замеров на пробу и повтор спорного прироста]\n");
    std::string tl, tlOld;
    readFile(logx::testFile(), tl);
    readFile(paths::appDir() + "logs/test.old.log", tlOld);
    tl = tlOld + tl;
    size_t knob = tl.find("[ручка]");
    size_t line = knob == std::string::npos ? std::string::npos : tl.find('\n', knob);
    std::string first = knob == std::string::npos ? std::string() : tl.substr(knob, line - knob);
    CHECK(first.find("замеров 5") != std::string::npos, "проба ручки — 5 замеров: %s", first.c_str());
    size_t sp = tl.find("спорный прирост, замеряю ещё раз");
    CHECK(sp != std::string::npos, "спорный прирост встретился и замерен ещё раз");
    size_t rep = sp == std::string::npos ? std::string::npos : tl.find("повтор спорного прироста", sp);
    size_t avg = sp == std::string::npos ? std::string::npos : tl.find("среднее двух", sp);
    CHECK(rep != std::string::npos && avg != std::string::npos && rep < avg, "повторная проба, затем решение по среднему двух");
    int n = 0;
    for (size_t p = tl.find("спорный прирост, замеряю"); p != std::string::npos; p = tl.find("спорный прирост, замеряю", p + 1)) n++;
    printf("  спорных приростов в журнале: %d\n", n);
}

// Прерванная проба: перезагрузка — зависание; та же загрузка + отчёт о падении — вылет; иначе — приложение закрыли.
static void testExitVsCrash() {
    printf("[выход из приложения посреди пробы — не сбой]\n");
    for (int mode = 0; mode < 3; mode++) {  // 0 — закрыли, 1 — вылет с отчётом, 2 — зависание (перезагрузка)
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.kipFirst = false;
        tu.begin(makeLadder(), chipFromDramId(17));
        for (int i = 0; i < 8 && tu.phase != Phase::FreqScan; i++) tu.step(t, nullptr);
        tu.step(t, nullptr);  // одна ступень прогона
        size_t pts = tu.points.size();
        TrialSpec s;
        s.kind = TrialSpec::Scan;
        s.hz = tu.ladder[tu.coarse[tu.scanPos]];
        s.desc = "проба для теста";
        tu.pending = s;
        tu.pendingBoot = t.bootEpoch();
        tu.pendingCrash = crashReportNames();
        tu.saveState(t);
        std::string crashDir = sdRoot() + "atmosphere/crash_reports";
        makeDirs(crashDir);
        if (mode == 1) writeFile(crashDir + strf("/0179%07d_0100000000001000.log", 1234567 + (int)pts), "crash");
        if (mode == 2) t.powerCycle();
        Tuner tu2;
        bool rec = false;
        CHECK(tu2.loadState(t, &rec), "журнал читается");
        bool counted = tu2.points.size() == pts + 1;
        const char* name[] = {"закрыли", "вылет с отчётом", "зависание"};
        printf("  %s: засчитано как сбой — %s\n", name[mode], counted ? "да" : "нет");
        if (mode == 0)
            CHECK(!rec && !counted && !tu2.pending.valid(), "закрыли приложение — проба не засчитана, повторится");
        else
            CHECK(rec && counted && tu2.points.back().status == 2, "%s — сбой засчитан", name[mode]);
        if (mode == 1) removeFile(crashDir + strf("/0179%07d_0100000000001000.log", 1234567 + (int)pts));
    }
    removeFile(paths::unstable());
}

// Кандидат, вылетевший 7 из 7: не удаляется и не переписывается отжимом — помечен «сомнительный», отжимается копия.
static void testDoubtful() {
    printf("[вылетел 7 из 7 — сомнительный, не удаляется]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.kipFirst = false;
    tu.cfg.kipStage = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    tu.singleHz = 2400000000u;
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    int i = -1;
    for (size_t k = 0; k < tu.cands.size(); k++)
        if (!tu.cands[k].baseline) i = (int)k;
    CHECK(i >= 0, "есть кандидат");
    if (i < 0) return;
    Candidate& c = tu.cands[i];
    c.crashes = c.attempts = 7;
    c.game = 2;
    TotkRun r;
    r.frames = 2600;
    r.avg = 43;
    c.runs.push_back(r);
    Candidate before = c;
    size_t n = tu.cands.size();
    std::string msg;
    tu.stabilize(t, i, &msg);
    const Candidate& o = tu.cands[i];
    printf("  кандидатов %zu → %zu; исходный: %s, сомнительный %d; копия: %s\n", n, tu.cands.size(), knobSetShort(o.set).c_str(), o.doubtful() ? 1 : 0,
           tu.cands.size() > n ? tu.cands.back().note.c_str() : "-");
    CHECK(o.hz == before.hz && o.set == before.set && o.runs.size() == 1 && o.crashes == 7, "исходный не тронут: частота, набор, прогоны");
    CHECK(o.doubtful() && tu.bestCandidate() != i, "помечен сомнительным и не рекомендуется");
    CHECK(tu.cands.size() == n + 1 && tu.cands.back().note.find("отжат из") != std::string::npos && tu.cands.back().runs.empty(),
          "отжата копия");
    removeFile(paths::unstable());
}

// Разогрев перед первой пробой сеанса, медиана прогонов TOTK, повтор при ошибке записи timings.ini.
static void testWarmMedianRetry() {
    printf("[разогрев, медиана TOTK, повтор записи timings.ini]\n");
    // Медиана: холодный первый прогон (2617) не тянет итог — как на консоли 1.8.2.
    std::vector<TotkRun> runs(3);
    runs[0].frames = 2617;
    runs[1].frames = 2754;
    runs[2].frames = 2753;
    double f = 0;
    Tuner::runsMedian(runs, &f, nullptr, nullptr, nullptr);
    CHECK(f == 2753, "медиана 2617 / 2754 / 2753 = 2753 (%.0f)", f);

    // Разогрев: перед первой пробой сеанса — замер не в счёт, базовых замеров по-прежнему 3.
    removeFile(paths::state());
    removeFile(paths::unstable());
    std::string tl0;
    readFile(logx::testFile(), tl0);
    SimTarget t;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.kipFirst = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    for (int i = 0; i < 8 && tu.phase == Phase::Baseline; i++) tu.step(t, nullptr);
    std::string tl;
    readFile(logx::testFile(), tl);
    std::string fresh = tl.size() > tl0.size() ? tl.substr(tl0.size()) : tl;
    size_t warm = fresh.find("▶ прогрев 30 с на 1600.0 МГц перед первой пробой сеанса");
    size_t base = fresh.find("Базовый замер 1/3");
    CHECK(warm != std::string::npos && base != std::string::npos && warm < base, "разогрев — до первого базового замера");
    CHECK(tu.baseRuns.size() == 3 && fresh.find("▶ прогрев", warm + 10) == std::string::npos && fresh.find("разогревочный замер") == std::string::npos,
          "прогрев один (на рабочей частоте, не прежний разогрев), базовых 3");

    // Ошибка записи timings.ini: два раза — повтор и проба засчитывается; всё время — пауза, а не сбой ручки.
    for (int fails : {2, 100}) {
        struct Io : SimTarget {
            int left = 0;
            bool apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) override {
                if (left > 0) {
                    left--;
                    if (err) *err = "не удалось записать timings.ini (I/O error)";
                    return false;
                }
                return SimTarget::apply(hz, set, info, err);
            }
        } io;
        io.left = fails;
        size_t pts = tu.points.size();
        int trials = tu.trials;
        std::string reg0, reg1;
        readFile(paths::unstable(), reg0);
        bool more = tu.step(io, nullptr);
        readFile(paths::unstable(), reg1);
        if (fails == 2)
            CHECK(more && tu.points.size() == pts + 1 && tu.points.back().status == 1, "после двух ошибок записи проба прошла");
        else
            CHECK(!more && tu.points.size() == pts && tu.trials == trials && reg1 == reg0, "карта не пишет — пауза, сбой не засчитан");
    }
    removeFile(paths::unstable());
}

// Максимум: 9 замеров на отбор, прирост — от 0,12 %, порог не растёт; прежние значения по умолчанию переводятся.
static void testMaxNoise() {
    printf("[максимум: отбор 9 замеров, прирост от 0,12 %%]\n");
    Settings m;
    m.applyRamMode(Settings::ModeMax);
    Settings old;
    old.deserialize(strf("settings_v=4\nram_mode=%d\ntiming_passes=5\n", (int)Settings::ModeMax));
    Settings was15;
    was15.deserialize(strf("settings_v=5\nram_mode=%d\ntiming_passes=15\n", (int)Settings::ModeMax));
    Settings mine;
    mine.deserialize(strf("settings_v=5\nram_mode=%d\ntiming_passes=7\n", (int)Settings::ModeMax));
    Settings many;
    many.deserialize(strf("settings_v=6\nram_mode=%d\ntiming_passes=25\n", (int)Settings::ModeMax));
    CHECK(m.timingPasses == 9 && old.timingPasses == 9 && was15.timingPasses == 9 && mine.timingPasses == 7 && many.timingPasses == 25,
          "9 замеров; прежние 5 и 15 переведены, свой выбор сохранён, до 30 не урезается (%d, %d, %d, %d, %d)", m.timingPasses, old.timingPasses,
          was15.timingPasses, mine.timingPasses, many.timingPasses);
    removeFile(paths::state());
    SimTarget t;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg = m;
    tu.cfg.kipFirst = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    for (int i = 0; i < 8 && tu.phase == Phase::Baseline; i++) tu.step(t, nullptr);
    std::string ev;
    for (auto& e : tu.events)
        if (e.find("Шум замера") != std::string::npos) ev = e;
    printf("  %s\n", ev.c_str());
    CHECK(std::fabs(tu.eps - Tuner::kMaxGain) < 1e-9, "порог — ровно 0,12 %% (%.3f %%)", tu.eps * 100);
    CHECK(ev.find("замер А/Б") != std::string::npos, "в журнале — правило порога");
    Tuner mid;
    mid.cfg.applyRamMode(Settings::ModeMedium);
    mid.cfg.kipFirst = false;
    mid.begin(makeLadder(), chipFromDramId(17));
    for (int i = 0; i < 8 && mid.phase == Phase::Baseline; i++) mid.step(t, nullptr);
    CHECK(mid.eps >= 0.003 - 1e-9, "у среднего порог прежний, от 0,3 %% (%.3f %%)", mid.eps * 100);
}

// Максимум при шуме как на консоли: отбор + замер А/Б. Ручки без влияния на поток не принимаются, настоящие
// приросты принимаются, оптимизация не перескакивает туда-обратно, шум прохода копится и переживает перезапуск.
static void testMaxPaired() {
    printf("[максимум: отбор и подтверждение А/Б при шуме как на консоли]\n");
    std::vector<BenchPass> ps(5);
    const double rr[] = {99, 100, 101, 40, 160};
    for (int i = 0; i < 5; i++) {
        ps[i].r = rr[i];
        ps[i].w = ps[i].c = 100;
        ps[i].l = ps[i].l2 = 50;
    }
    BenchResult agg = aggregatePasses(ps);
    CHECK(std::fabs(agg.readMBs - 100) < 1e-9 && agg.passes.size() == 5, "усечённое среднее 40/99/100/101/160 = 100 (%.2f)", agg.readMBs);

    TrialSpec s;
    s.kind = TrialSpec::Confirm;
    s.hz = 2400000000u;
    s.set = {{"tRCD", 5}};
    s.ref = {{"tRCD", 4}};
    s.key = "tRCD";
    s.val = 5;
    s.passes = 5;
    s.desc = "проба";
    TrialSpec s2, old;
    CHECK(s2.deserialize(s.serialize()) && s2.kind == TrialSpec::Confirm && s2.ref == s.ref && s2.set == s.set, "эталон А/Б в журнале");
    CHECK(old.deserialize("5|2400000000|tRCD:5|6|5|tRCD|5|старая проба") && old.ref.empty() && old.desc == "старая проба", "журнал до 1.8.7 читается");

    // Журнал испытаний — с чистого листа: по нему считаем решения.
    removeFile(logx::testFile());
    removeFile(paths::appDir() + "logs/test.old.log");
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    t.realNoise = true;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.applyRamMode(Settings::ModeMax);
    tu.cfg.kipFirst = false;
    tu.cfg.kipStage = false;
    tu.cfg.numTargets = 1;
    tu.begin(makeLadder(), chipFromDramId(17));
    u64 v0 = t.virtMs;
    int reboots = 0;
    bool ok = runToGame(t, tu, &reboots, 30000);
    CHECK(ok && tu.phase == Phase::Game, "дошли до TOTK: %s (%s)", phaseName(tu.phase), tu.error.c_str());
    double minutes = (t.virtMs - v0) / 60000.0;

    std::string tl, tlOld;
    readFile(logx::testFile(), tl);
    readFile(paths::appDir() + "logs/test.old.log", tlOld);
    tl = tlOld + tl;
    static const std::set<std::string> zero = {"ptsaCpu", "outReq", "rext", "wext",  "arbSafe", "tRPab",   "tW2P",
                                               "latAllow", "WLset", "tXP",  "tCKESR", "preRef", "daCovers"};
    const std::string evMark = "· ", pairMark = ": А/Б, блоков ", optMark = "Оптимизация ";
    int screened = 0, pairs = 0, acc = 0, falseAcc = 0, blocks = 0, badBlocks = 0, probes = 0, chains = 0;
    std::map<std::string, std::vector<std::pair<int, int>>> moves;  // оптимизация: ручка -> переходы
    for (auto& l : split(tl, '\n')) {
        if (l.find("▶ проба") != std::string::npos) probes++;
        size_t e = l.find(evMark);
        if (e == std::string::npos) continue;
        std::string body = l.substr(e + evMark.size());
        if (body.find("подтверждаю замером А/Б") != std::string::npos) screened++;
        if (startsWith(body, "Оптимизация: ")) {
            std::string r = body.substr(strlen("Оптимизация: "));
            int a = 0, b = 0;
            char key[32] = {0};
            if (sscanf(r.c_str(), "%31s %d→%d", key, &a, &b) == 3) moves[key].push_back({a, b});
        }
        size_t p = body.find(pairMark);
        // Цепочка рискованного тайминга: «tRAS: цепочка, 6 против старта — А/Б, блоков 3: …».
        bool chain = p == std::string::npos && body.find(": цепочка, ") != std::string::npos && body.find("— А/Б, блоков ") != std::string::npos;
        if (p == std::string::npos && !chain) continue;
        pairs++;
        chains += chain;
        int n = chain ? atoi(body.c_str() + body.find("— А/Б, блоков ") + strlen("— А/Б, блоков ")) : atoi(body.c_str() + p + pairMark.size());
        blocks += n;
        if (n < Tuner::kPairMinBlocks || n > Tuner::kPairMaxBlocks) badBlocks++;
        bool better = body.find("— лучше") != std::string::npos;
        if (better) acc++;
        std::string head = startsWith(body, optMark.c_str()) ? body.substr(optMark.size()) : body;
        std::string key = head.substr(0, std::min(head.find(" = "), head.find(':')));
        if (better && zero.count(key)) {
            falseAcc++;
            printf("  ложный прирост: %s\n", body.c_str());
        }
    }
    int flips = 0;
    for (auto& kv : moves)
        for (size_t i = 1; i < kv.second.size(); i++)
            if (kv.second[i].second == kv.second[i - 1].first) flips++;
    const Candidate* cand = nullptr;
    for (auto& c : tu.cands)
        if (!c.baseline) cand = &c;
    printf("  проб %d, на подтверждение %d, замеров А/Б %d (из них цепочек %d, в среднем %.1f блока), принято %d; проход ±%.3f %% (%d степ.), "
           "шум А/Б до %.2f %%; время %.0f мин\n",
           probes, screened, pairs, chains, pairs ? (double)blocks / pairs : 0.0, acc, tu.passSigma() * 100, tu.noiseDf, tu.pairNoise() * 100,
           minutes);
    if (cand) printf("  кандидат %s МГц: %s\n", mhzStr(cand->hz).c_str(), knobSetShort(cand->set).c_str());
    CHECK(pairs > 0 && screened + chains >= pairs - 1, "спорное подтверждается замером А/Б");
    CHECK(chains > 0, "у рискованных таймингов — один замер А/Б на цепочку (%d)", chains);
    CHECK(badBlocks == 0, "блоков А/Б от %d до %d", Tuner::kPairMinBlocks, Tuner::kPairMaxBlocks);
    CHECK(falseAcc == 0, "ручки без влияния на поток не приняты (%d)", falseAcc);
    CHECK(acc >= 3, "настоящие приросты приняты (%d)", acc);
    CHECK(flips == 0, "оптимизация не возвращает ручку туда, откуда сдвинула (%d)", flips);
    CHECK(tu.passSigma() > 0.0010 && tu.passSigma() < 0.0018, "шум прохода оценён около модельных 0,14 %% (%.3f %%)", tu.passSigma() * 100);
    if (cand) {
        // Сильные тайминги (0,5–1,2 % за ступень) должны дойти до предела модели: запас у «максимума» 0.
        for (const char* k : {"tRCD", "tRP", "tREFI"}) {
            int lim = t.m.limitAt(k, cand->hz / 1e6);
            int v = cand->set.count(k) ? cand->set.at(k) : -1;
            CHECK(v == lim, "%s = %d, предел модели %d", k, v, lim);
        }
    }
    Tuner tu2;
    tu2.cfg = tu.cfg;
    bool rec = false;
    CHECK(tu2.loadState(t, &rec) && tu2.noiseDf == tu.noiseDf && std::fabs(tu2.noiseSS - tu.noiseSS) < 1e-9 * (1 + tu.noiseSS),
          "шум прохода в журнале (%d степ.)", tu2.noiseDf);

    // Сам замер А/Б на наборе кандидата: одинаковые наборы — не «лучше»; на ступень лучше tRAS (+0,2 %) — «лучше».
    if (cand) {
        int same = 0, gain = 0, loss = 0, sb = 0;
        const int runs = 40;
        KnobSet a = cand->set, z = cand->set, soft = cand->set, softer = cand->set;
        z["ptsaCpu"] = 3;  // в модели не влияет на поток
        soft["tRAS"] = cand->set.at("tRAS") - 1;
        softer["tRCD"] = cand->set.at("tRCD") - 1;
        for (int i = 0; i < runs; i++) {
            TrialOut o = tu.pairedConfirm(t, cand->hz, a, z, nullptr, Tuner::kMaxGain);
            same += o.ok && o.delta >= Tuner::kMaxGain;
            sb += o.blocks;
            TrialOut g = tu.pairedConfirm(t, cand->hz, soft, a, nullptr, Tuner::kMaxGain);
            gain += g.ok && g.delta >= Tuner::kMaxGain;
            TrialOut w = tu.pairedConfirm(t, cand->hz, a, softer, nullptr, Tuner::kMaxGain);
            loss += w.ok && w.delta >= Tuner::kMaxGain;
        }
        printf("  А/Б %d раз: без влияния «лучше» %d (в среднем %.1f блока), tRAS +0,2 %% «лучше» %d, tRCD −0,6 %% «лучше» %d\n", runs, same,
               (double)sb / runs, gain, loss);
        CHECK(same == 0 && loss == 0, "без прироста и с потерей — не «лучше» (%d, %d)", same, loss);
        CHECK(gain >= runs * 8 / 10, "прирост 0,2 %% виден в 80 %% замеров и чаще (%d из %d)", gain, runs);
    }
    removeFile(paths::unstable());
}

// Эталон ручки заново: прежний принятый замер отобран как лучший из шумных и завышен — с ним все значения следующей
// ручки выходили «хуже», и оставался сток (1.8.4: после WL = -1 у девяти ручек подряд почти все значения в минусе).
static void testFreshRef() {
    printf("[эталон ручки заново: завышенный прежний замер не оставляет сток]\n");
    for (int mode : {(int)Settings::ModeSafe, (int)Settings::ModeMax}) {
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        t.realNoise = mode == Settings::ModeMax;
        writeFile(paths::timings(), "");
        Tuner g;
        if (mode == Settings::ModeMax) g.cfg.applyRamMode(Settings::ModeMax);
        g.cfg.kipFirst = false;
        g.begin(makeLadder(), chipFromDramId(17));
        g.singleHz = 2400000000u;
        // Ручка с настоящим приростом: tRCD (0,6 % за ступень) в безопасном, tRAS (0,2 %) в «максимуме».
        const char* key = mode == Settings::ModeMax ? "tRAS" : "tRCD";
        std::vector<Tuner::PlanItem> only;
        for (auto& p : g.plan)
            if (p.key == key) only.push_back(p);
        g.plan = only;
        for (int i = 0; i < 40 && g.phase != Phase::Timing; i++) g.step(t, nullptr);
        CHECK(g.phase == Phase::Timing, "дошли до таймингов (%s)", phaseName(g.phase));
        KnobSet st = g.cur;
        // Прежний принятый замер завышен на 1 % — больше прироста ступени (так бывает с отобранным лучшим из шумных).
        g.curScore *= 1.01;
        g.startScore = g.curScore;
        for (int i = 0; i < 200 && g.phase == Phase::Timing && g.kIdx < (int)g.plan.size(); i++) g.step(t, nullptr);
        bool ref = false;
        for (auto& e : g.events)
            if (e.find("эталон заново") != std::string::npos) ref = true;
        int v = g.cur.count(key) ? g.cur[key] : 0;
        printf("  %s: %s = %d (сток %d)\n", Settings::ramModeName(mode), key, v, st[key]);
        CHECK(ref, "%s: эталон замерен заново", Settings::ramModeName(mode));
        CHECK(v > st[key], "%s: %s ужесточён, а не оставлен стоковым (%d)", Settings::ramModeName(mode), key, v);
    }
    removeFile(paths::unstable());
}

// Финиш (эксперимент, «максимум»): tREFI и tRFC вместе перед TOTK. Этап проходит и не ломает кандидата; сбойное значение
// с итоговым кипом пробуется ещё раз и отклоняется; выключенный в настройках — пропускается.
static void testRefreshStage() {
    printf("[финиш: tREFI × tRFC вместе перед TOTK]\n");
    // По умолчанию — только «максимум»; настройки до 2.0 (стояло включённым везде) переводятся, свой выбор — сохраняется.
    Settings mx, safe, med, oldMax, oldSafe, off;
    mx.applyRamMode(Settings::ModeMax);
    safe.applyRamMode(Settings::ModeSafe);
    med.applyRamMode(Settings::ModeMedium);
    oldMax.deserialize("settings_v=6\nram_mode=1\nrefresh_tune=1\n");
    oldSafe.deserialize("settings_v=6\nram_mode=0\nrefresh_tune=1\n");
    off.deserialize("settings_v=7\nram_mode=1\nrefresh_tune=0\n");
    CHECK(mx.refreshTune && !safe.refreshTune && !med.refreshTune && oldMax.refreshTune && !oldSafe.refreshTune && !off.refreshTune,
          "по умолчанию включён только в «максимуме», выключается, старые настройки переведены");
    for (int on = 1; on >= 0; on--) {
        removeFile(logx::testFile());
        removeFile(paths::appDir() + "logs/test.old.log");
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        t.realNoise = true;
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.applyRamMode(Settings::ModeMax);
        tu.cfg.kipFirst = false;
        tu.cfg.kipStage = false;
        tu.cfg.numTargets = 1;
        tu.cfg.refreshTune = on != 0;
        tu.begin(makeLadder(), chipFromDramId(17));
        tu.singleHz = 2400000000u;
        int reboots = 0;
        bool ok = runToGame(t, tu, &reboots, 30000);
        CHECK(ok && tu.phase == Phase::Game, "дошли до TOTK: %s (%s)", phaseName(tu.phase), tu.error.c_str());
        std::string tl;
        readFile(logx::testFile(), tl);
        bool stage = tl.find("Финиш (эксперимент)") != std::string::npos;
        if (!on) {
            CHECK(!stage && !tu.rfDone, "выключен — этапа нет");
            continue;
        }
        CHECK(stage && tu.rfDone, "этап прошёл");
        int ci = -1;
        for (size_t i = 0; i < tu.cands.size(); i++)
            if (!tu.cands[i].baseline) ci = (int)i;
        CHECK(ci >= 0, "кандидат есть");
        if (ci < 0) continue;
        KnobSet before = tu.cands[ci].set;
        CHECK(t.m.stability(tu.cands[ci].hz / 1e6, before, 600) == 0, "кандидат стабилен в модели: %s", knobSetShort(before).c_str());

        // Ручки уже в кипе: сбойное значение tRFC пробуется ещё раз — в модели оно сбоит и отклоняется, кандидат цел.
        tu.kipAccepted = {"RL"};
        tu.rfDone = false;
        tu.rfQueue = {ci};
        tu.rfStage = 0;
        tu.phase = Phase::Refresh;
        size_t mark = tl.size();
        ok = runToGame(t, tu, &reboots, 3000);
        readFile(logx::testFile(), tl);
        std::string fresh = tl.size() > mark ? tl.substr(mark) : tl;
        int tried = 0;
        for (size_t p = fresh.find("[финиш: tREFI × tRFC]"); p != std::string::npos; p = fresh.find("[финиш: tREFI × tRFC]", p + 1)) tried++;
        printf("  набор %s; повтор на кипе: проб пар %d, перезагрузок %d; итог %s\n", knobSetShort(before).c_str(), tried, reboots,
               knobSetShort(tu.cands[ci].set).c_str());
        CHECK(ok && tu.phase == Phase::Game, "после повтора — снова TOTK (%s)", phaseName(tu.phase));
        CHECK(tried >= 1, "сбойное значение с кипом попробовано ещё раз");
        CHECK(t.m.stability(tu.cands[ci].hz / 1e6, tu.cands[ci].set, 600) == 0, "кандидат после финиша стабилен: %s",
              knobSetShort(tu.cands[ci].set).c_str());
        Tuner tu2;
        tu2.cfg = tu.cfg;
        bool rec = false;
        CHECK(tu2.loadState(t, &rec) && tu2.serialize() == tu.serialize(), "журнал финиша читается без потерь");
    }
    // С переносом в кип: после проверки загрузки финиш идёт сам, подбор не встаёт (1.9.4: вставал сразу после кипа).
    {
        removeFile(paths::state());
        removeFile(paths::unstable());
        removeFile(paths::kipBackup());
        std::string tl0;
        readFile(logx::testFile(), tl0);
        SimTarget t;
        t.realNoise = true;
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.applyRamMode(Settings::ModeMax);
        tu.cfg.kipFirst = false;
        tu.cfg.kipStage = true;
        tu.begin(makeLadder(), chipFromDramId(17));
        tu.singleHz = 2400000000u;
        int reboots = 0;
        bool ok = runToGame(t, tu, &reboots, 30000);
        std::string tl;
        readFile(logx::testFile(), tl);
        std::string fresh = tl.size() > tl0.size() ? tl.substr(tl0.size()) : tl;
        bool kip = fresh.find("Кип: перенос в кип закончен") != std::string::npos;
        bool fin = fresh.find("[финиш: tREFI × tRFC]") != std::string::npos;
        printf("  с кипом: перезагрузок %d, кип %s, финиш %s, этап %s\n", t.boots, kip ? "да" : "нет", fin ? "прошёл" : "нет", phaseName(tu.phase));
        CHECK(ok && kip && fin && tu.rfDone && tu.phase == Phase::Game, "после кипа финиш идёт сам и подбор доходит до TOTK (%s)", phaseName(tu.phase));
    }
    // Ряженка 8.1.0 (Ryazha-clk 3.5.5): сторожа нет — «кип сначала» и перенос в кип пропускаются, финиш всё равно идёт.
    {
        removeFile(paths::state());
        removeFile(paths::unstable());
        removeFile(paths::kipBackup());
        removeFile(paths::kipCommonBackup());
        struct Pub : SimTarget {
            int watchdogState() override { return kWatchdogNone; }
        } t;
        t.realNoise = true;
        t.commonSupported = true;
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.applyRamMode(Settings::ModeMax);
        tu.cfg.kipFirst = true;
        tu.cfg.kipStage = true;
        tu.begin(makeLadder(), chipFromDramId(17));
        tu.singleHz = 2400000000u;
        std::string tl0;
        readFile(logx::testFile(), tl0);
        int reboots = 0;
        bool ok = runToGame(t, tu, &reboots, 30000);
        std::string tl;
        readFile(logx::testFile(), tl);
        std::string fresh = tl.size() > tl0.size() ? tl.substr(tl0.size()) : tl;
        size_t first = fresh.find("сторожа нет");
        bool skipped = first != std::string::npos && fresh.find("сторожа нет", first + 1) != std::string::npos;  // оба этапа кипа
        printf("  без сторожа: перезагрузок %d, кип %s, финиш %s, этап %s\n", t.boots, t.kipConfig.empty() ? "не тронут" : "ИЗМЕНЁН",
               tu.rfDone ? "прошёл" : "нет", phaseName(tu.phase));
        CHECK(ok && skipped && t.boots == 0 && t.kipConfig.empty() && tu.rfDone && tu.phase == Phase::Game,
              "без сторожа кип не трогается, финиш идёт, подбор доходит до TOTK (%s)", phaseName(tu.phase));
    }
    // Безопасный режим с финишем, включённым вручную: идёт с запасом режима, кандидат остаётся стабильным.
    {
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        writeFile(paths::timings(), "");
        Tuner tu;
        tu.cfg.applyRamMode(Settings::ModeSafe);
        tu.cfg.kipFirst = false;
        tu.cfg.kipStage = false;
        tu.cfg.refreshTune = true;
        tu.begin(makeLadder(), chipFromDramId(17));
        tu.singleHz = 2400000000u;
        int reboots = 0;
        bool ok = runToGame(t, tu, &reboots, 30000);
        int ci = -1;
        for (size_t i = 0; i < tu.cands.size(); i++)
            if (!tu.cands[i].baseline) ci = (int)i;
        CHECK(ok && tu.phase == Phase::Game && tu.rfDone && ci >= 0, "безопасный, финиш вручную: дошли до TOTK");
        if (ci >= 0)
            CHECK(t.m.stability(tu.cands[ci].hz / 1e6, tu.cands[ci].set, 600) == 0, "безопасный: кандидат стабилен (%s)",
                  knobSetShort(tu.cands[ci].set).c_str());
    }
    removeFile(paths::unstable());
}

// Прогрев перед первой пробой сеанса: на частоте пробы и проверенном наборе, без memtest; после него эталон — заново.
// Журнал прежнего замера (без версии) — предупреждение и эталон заново.
static void testWarmRestart() {
    printf("[прогрев после перезапуска и смена версии замера]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(logx::testFile());
    removeFile(paths::appDir() + "logs/test.old.log");
    SimTarget t;
    t.realNoise = true;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.applyRamMode(Settings::ModeMax);
    tu.cfg.kipFirst = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    tu.singleHz = 2400000000u;
    for (int i = 0; i < 60 && !(tu.phase == Phase::Timing && tu.kIdx >= 2); i++) tu.step(t, nullptr);
    CHECK(tu.phase == Phase::Timing && tu.kIdx >= 2, "дошли до третьей ручки (%s, %d)", phaseName(tu.phase), tu.kIdx);
    tu.saveState(t);
    // «Перезапуск приложения»: новый объект, журнал с карты; первая проба — прогрев на 2400, затем эталон заново.
    std::string tl0;
    readFile(logx::testFile(), tl0);
    Tuner tu2;
    tu2.cfg = tu.cfg;
    bool rec = false;
    CHECK(tu2.loadState(t, &rec), "журнал читается");
    tu2.step(t, nullptr);
    std::string tl;
    readFile(logx::testFile(), tl);
    std::string fresh = tl.size() > tl0.size() ? tl.substr(tl0.size()) : tl;
    size_t warm = fresh.find("▶ прогрев 30 с на 2400.0 МГц"), ref = fresh.find("[эталон заново]");
    CHECK(warm != std::string::npos && fresh.find("memtest", warm) > fresh.find("прогрев:", warm), "прогрев на частоте пробы, без memtest");
    CHECK(ref != std::string::npos && warm < ref, "после прогрева — эталон заново");
    // Журнал прежнего замера: без версии — при загрузке предупреждение, эталон заново, версия — текущая.
    std::string st;
    t.loadText(paths::state(), st);
    size_t p = st.find("benchv=");
    if (p != std::string::npos) st.erase(p, st.find('\n', p) - p + 1);
    t.saveText(paths::state(), st);
    Tuner tu3;
    tu3.cfg = tu.cfg;
    CHECK(tu3.loadState(t, &rec) && tu3.benchVer == kBenchVersion && tu3.needRef, "журнал без версии замера — эталон заново");
    bool warned = false;
    for (auto& e : tu3.events)
        if (e.find("Замер потока изменён") != std::string::npos) warned = true;
    CHECK(warned, "предупреждение о смене замера");
    removeFile(paths::unstable());
}

// Прогоны TOTK на кандидата: хранятся все, но не больше 15 — старые уходят, итог по последним; журнал это переживает.
static void testTotkRunCap() {
    printf("[TOTK: до 15 прогонов на кандидата]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.kipFirst = false;
    tu.cfg.kipStage = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    tu.singleHz = 2400000000u;
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    int i = -1;
    for (size_t k = 0; k < tu.cands.size(); k++)
        if (!tu.cands[k].baseline) i = (int)k;
    CHECK(i >= 0, "есть кандидат");
    if (i < 0) return;
    removeFile(sdRoot() + "TOTKBenchmark.txt");
    std::string err, msg;
    for (int k = 0; k < 18; k++) {
        CHECK(tu.gameApply(t, i, &err), "gameApply: %s", err.c_str());
        std::string old;
        readFile(sdRoot() + "TOTKBenchmark.txt", old);
        writeFile(sdRoot() + "TOTKBenchmark.txt",
                  old + strf("BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: %d, Average FPS: 45, 1%% FPS: 40, 0.1%% FPS: 38\n", 2600 + k));
        tu.gameCollect(t, &msg);
    }
    const Candidate& c = tu.cands[i];
    printf("  прогонов %zu: первый %.0f, последний %.0f\n", c.runs.size(), c.runs.empty() ? 0.0 : c.runs.front().frames,
           c.runs.empty() ? 0.0 : c.runs.back().frames);
    CHECK(c.runs.size() == Tuner::kMaxTotkRuns && c.runs.front().frames == 2603 && c.runs.back().frames == 2617, "18 прогонов — остались 15 последних");
    tu.saveState(t);
    Tuner tu2;
    tu2.cfg = tu.cfg;
    bool rec = false;
    CHECK(tu2.loadState(t, &rec) && tu2.cands.size() > (size_t)i && tu2.cands[i].runs.size() == Tuner::kMaxTotkRuns, "журнал: 15 прогонов");
    removeFile(paths::unstable());
}

// Правка system_settings.ini (планировщик ГПУ): ставится один ключ, всё остальное — байт в байт.
static void testIniEdit() {
    printf("[system_settings.ini: планировщик ГПУ]\n");
    const std::string S = "am.gpu", K = "gpu_scheduling_enabled", OFF = "u8!0x0";
    std::string a = iniSet("", S, K, OFF);
    CHECK(a == "[am.gpu]\ngpu_scheduling_enabled = u8!0x0\n" && iniGet(a, S, K) == OFF, "пустой файл: %s", a.c_str());
    std::string b0 = "; мои настройки\r\n[atmosphere]\r\nfatal_auto_reboot_interval = u64!0x0\r\n";
    std::string b = iniSet(b0, S, K, OFF);
    CHECK(startsWith(b, b0.c_str()) && b.find("\r\n[am.gpu]\r\ngpu_scheduling_enabled = u8!0x0\r\n") != std::string::npos,
          "другие секции на месте, CRLF сохранён");
    std::string c0 = "[am.gpu]\ngpu_scheduling_enabled=u8!0x1 \n; комментарий\n[usb]\nusb30_force_enabled = u8!0x1\n";
    std::string c = iniSet(c0, S, K, OFF);
    CHECK(c == "[am.gpu]\ngpu_scheduling_enabled = u8!0x0\n; комментарий\n[usb]\nusb30_force_enabled = u8!0x1\n", "ключ заменён на месте: %s",
          c.c_str());
    CHECK(iniGet(c0, S, K) == "u8!0x1" && iniGet(c0, "usb", "usb30_force_enabled") == "u8!0x1" && iniGet(c0, "usb", K).empty(),
          "чтение по секциям");
    std::string d0 = "[am.gpu]\nother = u8!0x1\n\n[usb]\nx = 1\n";
    std::string d = iniSet(d0, S, K, "u8!0x1");
    CHECK(d == "[am.gpu]\nother = u8!0x1\ngpu_scheduling_enabled = u8!0x1\n\n[usb]\nx = 1\n", "дописан в свою секцию, до следующей: %s", d.c_str());
    CHECK(iniSet(iniSet(c0, S, K, OFF), S, K, "u8!0x1") == iniSet(c0, S, K, "u8!0x1"), "повторная смена — тот же файл");
}

// Архив конфигураций: сохранить, не дублировать тот же набор, удалить.
static void testArchive() {
    printf("[архив успешных конфигураций]\n");
    removeFile(archivePath());
    ArchEntry a;
    a.hz = 2934400000u;
    a.set = {{"tRCD", 5}, {"tRP", 5}, {"RL", -1}, {"ramPD", 1}};
    a.bw = 18601;
    a.frames = 2639;
    a.name = "К1, TOTK 3 пр.";
    ArchEntry b = a;
    b.hz = 2828800000u;
    b.name = "ручная настройка";
    b.frames = 0;
    CHECK(archAdd(a) && archAdd(b), "записано");
    ArchEntry a2 = a;
    a2.bw = 18650;
    a2.frames = 0;  // повторное сохранение без TOTK не должно стереть кадры
    CHECK(archAdd(a2), "повторное сохранение");
    auto all = archLoad();
    CHECK(all.size() == 2, "тот же набор не дублируется (%zu)", all.size());
    CHECK(all.size() == 2 && all[0].hz == a.hz && all[0].set == a.set && all[0].bw == 18650 && all[0].frames == 2639, "обновлён, кадры сохранены");
    printf("  %s\n", all.empty() ? "-" : archLabel(all[0]).c_str());
    CHECK(archRemove(0) && archLoad().size() == 1 && archLoad()[0].hz == b.hz, "удаление");
    removeFile(archivePath());
}

// История частот: копится из прогонов, доводки и TOTK; сводка — по частотам.
static void testHistory() {
    printf("[история частот за всё время]\n");
    auto rows = histLoad();
    int scan = 0, tuned = 0, totk = 0;
    for (auto& r : rows) {
        if (startsWith(r.kind, "scan:")) scan++;
        if (r.kind == "tuned") tuned++;
        if (r.kind == "totk") totk++;
    }
    HistFreq base;
    auto sum = histSummary(rows, &base);
    double bestTuned = 0;
    for (auto& f : sum) bestTuned = std::max(bestTuned, f.tunedBest);
    printf("  записей %zu: прогон %d, доводка %d, TOTK %d; частот %zu, «Как было» в TOTK — %d пр.\n", rows.size(), scan, tuned, totk, sum.size(),
           base.totkRuns);
    CHECK(scan > 0 && tuned > 0 && totk > 0, "в истории прогоны, доводка и TOTK");
    CHECK(!sum.empty() && bestTuned > 0 && std::is_sorted(sum.begin(), sum.end(), [](const HistFreq& a, const HistFreq& b) { return a.mhz < b.mhz; }),
          "сводка по частотам по возрастанию, с доведённым потоком");
}

// Оптимизация: другое значение — только по замеру А/Б, мягче — без сползания; итог сверяется с набором до оптимизации.
static void testOptChoice() {
    printf("[оптимизация: пороги А/Б, без сползания и защита итога]\n");
    Tuner tu;
    tu.eps = 0.004;
    tu.optLoss = 0;
    double tight = tu.optThr(false), soft0 = tu.optThr(true);
    tu.optLoss = -0.0015;  // уже взяли мягче с потерей 0,15 %
    double soft1 = tu.optThr(true);
    CHECK(std::fabs(tight - 0.004) < 1e-12 && std::fabs(soft0 + 0.002) < 1e-12 && std::fabs(soft1 + 0.0005) < 1e-12,
          "безопасный: туже — от шума, мягче — до половины шума от самого быстрого в сумме (%.4f, %.4f, %.4f)", tight, soft0, soft1);
    Tuner mx;
    mx.cfg.applyRamMode(Settings::ModeMax);
    CHECK(mx.optThr(false) == Tuner::kMaxGain && mx.optThr(true) == Tuner::kMaxGain && mx.guardThr() == 0.0,
          "максимум: любое другое значение — от 0,12 %%, итог — не хуже набора до оптимизации");

    // Защита итога: набор «после» заметно хуже «до» — возвращается прежний; лучше — остаётся.
    for (int worse = 1; worse >= 0; worse--) {
        removeFile(paths::state());
        removeFile(paths::unstable());
        SimTarget t;
        writeFile(paths::timings(), "");
        Tuner g;
        g.cfg.kipFirst = false;
        g.cfg.communityBands = false;
        g.begin(makeLadder(), chipFromDramId(17));
        for (int i = 0; i < 8 && g.phase == Phase::Baseline; i++) g.step(t, nullptr);
        u32 hz = 0;
        for (u32 h : g.ladder)
            if (!hz || std::fabs(h / 1e6 - 2400) < std::fabs(hz / 1e6 - 2400)) hz = h;
        KnobSet tight = ryazhaStockSet(), loose = ryazhaStockSet();
        tight["tRCD"] = 6;
        tight["tRP"] = 5;
        loose["tRCD"] = 1;
        loose["tRP"] = 1;
        g.targets = {hz};
        g.tIdx = 0;
        g.phase = Phase::Optimize;
        g.optKeys.clear();
        g.optKey = 0;
        g.optPass = 1;
        g.optGuard = 0;
        g.optGuardA = g.optGuardB = 0;
        g.optStart = worse ? tight : loose;
        g.cur = worse ? loose : tight;
        KnobSet after = g.cur, before = g.optStart;
        for (int i = 0; i < 4 && g.optGuard < 4; i++) g.step(t, nullptr);
        printf("  итог %s до оптимизации: осталось %s\n", worse ? "хуже" : "лучше", knobSetShort(g.cur).c_str());
        if (worse)
            CHECK(g.cur == before && g.optGuard >= 4, "итог хуже — возвращён набор до оптимизации");
        else
            CHECK(g.cur == after && g.optGuard >= 4, "итог лучше — оставлен");
        g.step(t, nullptr);
        CHECK(g.phase != Phase::Optimize, "оптимизация закончена, подбор пошёл дальше (%s)", phaseName(g.phase));
    }
    removeFile(paths::unstable());
}

// Безопасный режим сторожа: демон не ставит частоты — проба не засчитывается и подбор встаёт до перезагрузки.
static void testSafeMode() {
    printf("[безопасный режим сторожа: пробы не засчитываются]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    struct Safe : SimTarget {
        bool safe = false;
        bool safeMode() override { return safe; }
        bool apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) override {
            if (safe && hz != 1600000000u) {
                if (err) *err = "pcv не перешёл на " + mhzStr(hz) + " МГц (стоит 1600.0)";
                return false;
            }
            return SimTarget::apply(hz, set, info, err);
        }
    } t;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.kipFirst = false;
    tu.cfg.kipStage = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    for (int i = 0; i < 6 && tu.phase != Phase::FreqScan; i++) tu.step(t, nullptr);  // база
    size_t pts = tu.points.size();
    int trials = tu.trials;
    std::string reg0;
    readFile(paths::unstable(), reg0);
    t.safe = true;
    bool more = tu.step(t, nullptr);
    std::string reg1;
    readFile(paths::unstable(), reg1);
    CHECK(!more && tu.safeHalt, "подбор встал (safeHalt)");
    CHECK(tu.points.size() == pts && tu.trials == trials && reg1 == reg0, "проба не засчитана: точек %zu→%zu, проб %d→%d", pts, tu.points.size(), trials,
          tu.trials);
    t.safe = false;
    tu.safeHalt = false;
    CHECK(tu.step(t, nullptr) && tu.points.size() == pts + 1, "после перезагрузки та же ступень проверяется заново");
    removeFile(paths::unstable());
}

// Как на консоли 25.10: сток держит весь диапазон, набор гайда виснет на 1638.4 и 1676.8 — это вина набора, не частоты.
static void testGuideHangNoCap() {
    printf("[зависания на наборе гайда не ставят потолок частоте]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    SimTarget t;
    t.m.hangAbove["tRAS"] = 1600;  // любой tRAS туже стока выше 1600 вешает память — у гайда tRAS 5
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.communityBands = false;
    tu.cfg.kipStage = false;
    tu.cfg.kipFirst = false;
    tu.begin(makeLadder(), chipFromDramId(17));
    int reboots = 0;
    runToGame(t, tu, &reboots, 8000);
    u32 top = 0;  // верх стока в модели — около 2950 МГц
    for (auto& p : tu.points)
        if (p.pass == 0 && p.status == 1) top = std::max(top, p.hz);
    printf("  потолок %s, цели:", tu.freqCap ? mhzStr(tu.freqCap).c_str() : "нет");
    for (u32 hz : tu.targets) printf(" %s", mhzStr(hz).c_str());
    printf(", перезагрузок %d\n", reboots);
    CHECK(tu.freqCap == 0, "потолка нет (%s)", mhzStr(tu.freqCap).c_str());
    CHECK(top > 2900000000u && !tu.guideChosen && !tu.targets.empty() && tu.targets[0] == top, "первая цель — верх стока %s", mhzStr(top).c_str());
    removeFile(paths::unstable());
}

// Абсолютные пределы: тайминги идут до первого сбоя при любом пресете, запас режима — от предела.
static void testAbsLimits() {
    printf("[абсолютные пределы таймингов и запас режима]\n");
    for (int preset = 0; preset < 2; preset++)
        for (int margin = 0; margin <= 2; margin++) {
            removeFile(paths::state());
            removeFile(paths::unstable());
            SimTarget t;
            writeFile(paths::timings(), "");
            Tuner tu;
            tu.cfg.communityBands = false;  // модель чипа — под Common гайда
            tu.cfg.kipStage = false;
            tu.cfg.kipFirst = false;
            tu.cfg.usePreset = preset == 1;
            tu.cfg.margin = margin;
            tu.cfg.patience = 1;  // «терпение» не должно обрывать поиск предела таймингов
            tu.begin(makeLadder(), chipFromDramId(17));
            tu.singleHz = 2400000000u;
            int reboots = 0;
            runToGame(t, tu, &reboots, 8000);
            KnobSet fin;
            for (auto& c : tu.cands)
                if (!c.baseline) fin = c.set;
            // Последнее рабочее в модели на 2400: tRCD 6, tRP 5, tRAS 8, tRRD 4, tRFC 7, tRTW 6, tWTR 7 — пик на ступень туже.
            printf("  пресет %s, запас %d: пики %s; итог %s\n", preset ? "гайд" : "нет", margin, knobSetShort(tu.absLimits).c_str(),
                   knobSetShort(fin).c_str());
            CHECK(tu.absLimits.count("tRCD") && tu.absLimits["tRCD"] == 7 && tu.absLimits["tRP"] == 6 && tu.absLimits["tWTR"] == 8,
                  "пики найдены (%s)", knobSetShort(tu.absLimits).c_str());
            // tRCD сильнее всего даёт поток — встаёт ровно на границу режима. Максимум: пик 7 перепроверяется, не держится —
            // ступень мягче (6); средний — пик −1 = 6; безопасный — пик −2 = 5. Остальные: оптимизация может взять мягче
            // (тот же поток в пределах шума), но туже границы — никогда; задержки не уходят мягче стока.
            int wantRcd = margin == 0 ? 6 : 7 - margin;
            CHECK(fin.count("tRCD") && fin["tRCD"] == wantRcd, "запас %d от пика: tRCD %d (ждали %d)", margin, fin.count("tRCD") ? fin["tRCD"] : -1,
                  wantRcd);
            CHECK(!fin.count("tREFI") || fin["tREFI"] <= 2, "tREFI %d — выше 7800 нс без разрешения", fin["tREFI"]);
            for (auto& kv : tu.absLimits) {
                const KnobDef* d = findKnob(kv.first);
                bool up = d->order == KnobOrder::Up;
                int v = fin.count(kv.first) ? fin[kv.first] : 0;
                int allowed = up ? kv.second - margin : kv.second + margin;
                int lastOk = up ? kv.second - 1 : kv.second + 1;
                if (up ? allowed > lastOk : allowed < lastOk) allowed = lastOk;  // сам пик в модели не держится
                if (d->absentIsZero) allowed = up ? std::max(allowed, 0) : std::min(allowed, 0);
                CHECK(up ? v <= allowed : v >= allowed, "%s = %d туже границы %d (пик %d, запас %d)", kv.first.c_str(), v, allowed, kv.second, margin);
                if (d->absentIsZero) CHECK(up ? v >= 0 : v <= 0, "%s = %d мягче стока", kv.first.c_str(), v);
            }
        }
    removeFile(paths::unstable());
}

// Порядок отжима: тайминги -> пропускная способность живьём -> кип -> частота.
static void testLoosenOrder() {
    printf("[порядок отжима]\n");
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.ladder = makeLadder();
    Candidate c;
    c.id = 1;
    c.hz = 2400000000u;
    c.set = {{"tRCD", 4}, {"tREFI", 2}, {"RL", -1}, {"daTurns", 1}, {"ramPD", 1}};
    tu.cands.push_back(c);
    tu.kipAccepted = {"ramPD"};
    tu.kipVals = {{"ramPD", 1}};
    for (u32 hz : {2133000000u, 2400000000u}) {
        FreqPoint fp;
        fp.hz = hz;
        fp.status = 1;
        tu.points.push_back(fp);
    }
    std::vector<int> tiers;
    std::string seq;
    for (int guard = 0; guard < 40; guard++) {
        Tuner::LoosenPlan p = tu.nextLoosen(tu.cands[0]);
        if (p.tier == 0) break;
        tiers.push_back(p.tier);
        seq += (seq.empty() ? "" : " | ") + p.desc;
        if (p.tier == 3) {
            tu.kipAccepted.clear();
            tu.cands[0].set.erase(p.key);
        } else {
            tu.cands[0].set = p.set;
            tu.cands[0].hz = p.hz;
        }
        tu.cands[0].loosened++;
    }
    printf("  %s\n", seq.c_str());
    CHECK(std::is_sorted(tiers.begin(), tiers.end()), "тайминги -> пропускная способность -> кип -> частота");
    CHECK(std::count(tiers.begin(), tiers.end(), 1) == 6, "тайминги отжаты до стока Ryazha-clk (%d шагов)", (int)std::count(tiers.begin(), tiers.end(), 1));
    CHECK(std::count(tiers.begin(), tiers.end(), 3) == 1 && std::count(tiers.begin(), tiers.end(), 4) == 1, "кип и частота — по шагу");
    CHECK(seq.find("daTurns") < seq.find("RL"), "RL сдаётся последним из живых ручек");
    CHECK(tu.cands[0].hz == 2133000000u, "частота ниже — последней");

    Settings a, b, c3, c4;
    a.deserialize("w_bw=50\n");
    b.deserialize("settings_v=2\nw_bw=50\n");
    c3.deserialize("settings_v=3\nw_bw=100\n");  // прежнее значение по умолчанию — на новое (80 %)
    c4.deserialize("settings_v=4\nw_bw=100\n");  // выбрано после 1.7.6 — как есть
    CHECK(a.wBw == 80 && b.wBw == 50 && c3.wBw == 80 && c4.wBw == 100, "вес потока: до 1.5.0 и прежние 100 %% — на 80 %%, свой выбор — как есть (%d, %d, %d, %d)",
          a.wBw, b.wBw, c3.wBw, c4.wBw);
    Settings m;
    m.applyRamMode(Settings::ModeMedium);
    Settings m2;
    m2.deserialize(m.serialize());
    CHECK(m.margin == 1 && m2.ramMode == Settings::ModeMedium && m2.margin == 1, "средний режим: запас 1, сохраняется");
    Settings pk1, pk2, pk3;
    pk1.deserialize("settings_v=2\nram_mode=0\npeak_drop_mb=150\n");
    pk2.deserialize("settings_v=2\nram_mode=0\npeak_drop_mb=200\n");
    pk3.deserialize(pk1.serialize());
    CHECK(Settings().peakDropMb == 100 && pk1.peakDropMb == 100 && pk2.peakDropMb == 200 && pk3.peakDropMb == 100,
          "порог пика 100 МБ/с: старое 150 переведено, своё значение не тронуто");
    CHECK(Settings::nextRamMode(Settings::ModeSafe) == Settings::ModeMedium && Settings::nextRamMode(Settings::ModeMedium) == Settings::ModeMax &&
              Settings::nextRamMode(Settings::ModeMax) == Settings::ModeSafe,
          "переключение по кругу");
}

// Перенос в кип: пакет разом, одна ручка вешает загрузку, другая портит память — деление находит обе.
static void testKipBisect() {
    printf("[перенос в кип: пакет + деление]\n");
    removeFile(paths::state());
    removeFile(paths::kipBackup());
    SimTarget t;
    t.m.kipBootBad = {14};  // HP Mode в кипе: консоль не загружается
    t.m.kipMemBad = {0};    // RL в кипе: ошибки memtest после обучения на загрузке
    t.original = "";
    writeFile(paths::timings(), t.original);
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.cfg.wBw = 50;  // поровну с задержкой: в модели RL/WL/HP Mode дают только задержку, а здесь нужны в пакете
    tu.cfg.scanHangStop = 0;  // траектория прогона как раньше: тут проверяется механика кипа
    tu.begin(makeLadder(), chipFromDramId(17));
    int reboots = 0;
    bool ok = runToGame(t, tu, &reboots, 8000);
    CHECK(ok && tu.phase == Phase::Game, "дошли до TOTK: %s (%s)", phaseName(tu.phase), tu.error.c_str());
    printf("  ручки: %s\n  перезагрузок через систему: %d (висла на загрузке %d), принято: %zu, отклонено: %zu\n", knobSetShort(tu.kipVals).c_str(),
           t.boots, t.bootHangs, tu.kipAccepted.size(), tu.kipRejected.size());
    for (auto& l : tu.kipLog) printf("    · %s\n", l.c_str());
    bool rejHp = false, rejRl = false;
    for (auto& r : tu.kipRejected) {
        if (r.key == "ramPD") rejHp = true;
        if (r.key == "RL") rejRl = true;
    }
    CHECK(tu.kipVals.count("ramPD") && tu.kipVals.count("RL"), "в пакете есть ramPD и RL");
    CHECK(rejHp && rejRl, "оба виновника найдены");
    CHECK(tu.kipRejected.size() == 2, "отклонены только виновники (%zu)", tu.kipRejected.size());
    CHECK(tu.kipAccepted.size() + 2 == tu.kipVals.size(), "остальные приняты");
    CHECK(t.bootHangs >= 1, "зависание на загрузке случилось");
    // В кипе ровно принятый набор, виновников нет.
    CHECK(t.m.tmeh[14] == 0 && t.m.tmeh[0] == 0, "виновников в кипе нет");
    for (auto& k : tu.kipAccepted) CHECK(t.m.tmeh[kipSlotOf(k)] == tu.kipVals[k], "%s в кипе", k.c_str());
    CHECK(t.boots <= 12, "перезагрузок не слишком много: %d", t.boots);
    std::string bk;
    CHECK(readFile(paths::kipBackup(), bk) && bk.find("14:0") != std::string::npos, "копия исходного кипа сохранена");
    std::string rep = tu.report();
    CHECK(rep.find("Перенос в кип") != std::string::npos && rep.find("виновник") != std::string::npos, "перенос в кип в отчёте");
    bool restored = false;
    for (auto& l : tu.kipLog)
        if (l.find("тайминги не помогли") != std::string::npos) restored = true;
    CHECK(restored, "RL в кипе: сначала отжали тайминги, не помогло — вернули их и нашли виновника");

    // Сбой в игре: отжим кандидата — первым шагом тайминг, ручки пропускной способности не тронуты.
    int idx = -1;
    for (size_t i = 0; i < tu.cands.size(); i++)
        if (!tu.cands[i].baseline) idx = (int)i;
    KnobSet before = tu.cands[idx].set;
    std::string msg;
    CHECK(tu.stabilize(t, idx, &msg), "отжим: %s", msg.c_str());
    int changedBw = 0, changedTim = 0;
    for (auto& kv : before) {
        auto it = tu.cands[idx].set.find(kv.first);
        bool same = it != tu.cands[idx].set.end() && it->second == kv.second;
        if (!same) (kipSlotOf(kv.first) >= 0 ? changedBw : changedTim)++;
    }
    printf("  отжим после сбоя в игре: %s\n", msg.c_str());
    CHECK(changedTim == 1 && changedBw == 0, "отжат один тайминг (тайминги %d, пропускная способность %d)", changedTim, changedBw);

    // Выключенный перенос: перезагрузок нет.
    removeFile(paths::state());
    SimTarget t2;
    writeFile(paths::timings(), "");
    Tuner tu2;
    tu2.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu2.cfg.kipStage = false;
    tu2.begin(makeLadder(), chipFromDramId(17));
    runToGame(t2, tu2, &reboots, 8000);
    CHECK(tu2.phase == Phase::Game && t2.boots == 0, "без переноса в кип перезагрузок нет (%d)", t2.boots);
    removeFile(paths::kipBackup());
}

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
    cfg.scanHangStop = 0;  // здесь проверяем, что прогон проходит дыру с зависаниями насквозь
    int reboots = 0;
    auto tu = run(m, cfg, &reboots);
    int nOk = 0, nFail = 0, nSlow = 0;
    u32 topInRange = 0;
    for (u32 hz : makeLadder())
        if (hz <= 2900500000u) topInRange = hz;
    bool sawLow = false, saw1331 = false, sawTop = false;
    u32 topOk = 0;
    for (auto& p : tu->points) {
        if (p.pass != 0) continue;  // второй прогон (тайминги гайда) — отдельно
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
    size_t stockPts = 0;
    for (auto& p : tu->points)
        if (p.pass == 0) stockPts++;
    CHECK(stockPts == want, "проверены все ступени диапазона: %zu из %zu", stockPts, want);
    CHECK(!sawLow && saw1331, "низ прогона — 1331.2 МГц, 204 не проверяется");
    CHECK(sawTop, "прогон дошёл до верха диапазона");
    CHECK(reboots >= 2, "зависания в дыре должны были случиться");
    CHECK(topOk / 1000000u >= 2600 && topOk / 1000000u <= 2700, "стабильная частота выше дыры найдена: %s", mhzStr(topOk).c_str());
    CHECK(nSlow >= 1, "ступень с просевшим потоком помечена «медленнее»");
    // Пик потока: ступень перед просадкой (2060.8 медленнее) — 2022.4 МГц; она в целях доводки.
    int usePass = tu->guideChosen ? 1 : 0;
    auto peaks = tu->bandwidthPeaks(usePass);
    bool peak2022 = false;
    for (auto& pk : peaks) {
        printf("  пик потока: %s МГц, дальше −%.0f МБ/с\n", mhzStr(pk.first).c_str(), pk.second);
        if (pk.first == 2022400000u) peak2022 = true;
    }
    CHECK(peak2022, "пик перед просадкой (2022.4 МГц) найден");
    CHECK(std::find(tu->targets.begin(), tu->targets.end(), 2022400000u) != tu->targets.end(), "пик потока добавлен в цели доводки");
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

    // По умолчанию: 2 зависания подряд — прогон закончен (выше память не берёт), подбор идёт дальше.
    Settings c3 = cfg;
    c3.scanHangStop = 2;
    m.holeHangs = true;
    auto tu3 = run(m, c3, &reboots);
    u32 maxStock = 0;
    for (auto& p : tu3->points)
        if (p.pass == 0) maxStock = std::max(maxStock, p.hz);
    bool said = false;
    for (auto& e : tu3->events)
        if (e.find("зависания подряд") != std::string::npos) said = true;
    std::string tl3;
    readFile(logx::testFile(), tl3);
    if (tl3.find("зависания подряд") != std::string::npos) said = true;
    printf("  2 зависания подряд: прогон на стоке закончен на %s МГц, подбор дошёл до «%s»\n", mhzStr(maxStock).c_str(), phaseName(tu3->phase));
    CHECK(maxStock / 1000000u < 1950 && said, "после 2 зависаний подряд прогон закончен (%s)", mhzStr(maxStock).c_str());
    CHECK(tu3->phase == Phase::Game, "и подбор пошёл дальше");
    // Крайняя стабильная частота — потолок для всего дальше: второй прогон, цели.
    bool overCap = false;
    for (auto& p : tu3->points)
        if (p.pass == 1 && p.hz > tu3->freqCap) overCap = true;
    for (u32 hz : tu3->targets)
        if (hz > tu3->freqCap) overCap = true;
    printf("  потолок: %s МГц, цели:", mhzStr(tu3->freqCap).c_str());
    for (u32 hz : tu3->targets) printf(" %s", mhzStr(hz).c_str());
    printf("\n");
    CHECK(tu3->freqCap && tu3->freqCap < 1860000000u && !overCap, "второй прогон и цели не выше крайней стабильной частоты");

    // Настройки пишутся и читаются.
    Settings s3;
    s3.deserialize(cfg.serialize());
    CHECK(s3.minMhz == 1300 && s3.maxMhz == 2900 && s3.scanFailStop == 0, "min_mhz/max_mhz/scan_fail_stop");
}

// TOTK: подписи и итоги в файле бенчмарка, три прогона, статтеры по 0.1% low, реестр нестабильных значений.
static void testTotkMarks() {
    printf("[TOTK: подписи в файле, 3 прогона, статтеры]\n");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(sdRoot() + "TOTKBench.txt");
    removeFile(sdRoot() + "TOTKBenchmark.txt");
    std::string crashDir = sdRoot() + "atmosphere/crash_reports";
    for (auto& f : listDir(crashDir)) removeFile(crashDir + "/" + f);
    SimTarget t;
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.ladder = makeLadder();
    tu.phase = Phase::Game;
    Candidate b;
    b.id = 0;
    b.baseline = true;
    Candidate k1;
    k1.id = 1;
    k1.hz = 2934400000u;
    k1.set = {{"tRCD", 4}};
    k1.synth = 1.6;
    tu.cands = {b, k1};
    auto run = [&](int idx, double avg, double low01) {
        std::string err, msg;
        CHECK(tu.gameApply(t, idx, &err), "gameApply: %s", err.c_str());
        std::string old;
        readFile(sdRoot() + "TOTKBench.txt", old);
        writeFile(sdRoot() + "TOTKBench.txt",
                  old + strf("BENCHMARK FOR Korok Forest COMPLETED!\nTotal frames: %.0f, Average FPS: %.0f, 1%% FPS: %.0f, 0.1%% FPS: %.0f\n",
                             avg * 60, avg, avg * 0.8, low01));
        int n = tu.gameCollect(t, &msg);
        CHECK(n == 1, "прогон засчитан (%s)", msg.c_str());
        return msg;
    };
    run(0, 50, 33);  // «Как было»: 0.1% low = 66 % среднего
    std::string m1 = run(1, 55, 36);
    CHECK(m1.find("1 из 3") != std::string::npos, "после первого прогона — «1 из 3» (%s)", m1.c_str());
    run(1, 55, 37);
    std::string m3 = run(1, 55, 20);  // 36 % среднего — статтеры
    CHECK(tu.gameRunsDone(tu.cands[1]) == 3 && m3.find("усреднён") != std::string::npos, "три прогона, среднее (%s)", m3.c_str());
    CHECK(tu.cands[1].stutters == 1, "статтеры распознаны по 0.1%% low (%d)", tu.cands[1].stutters);
    std::string f;
    readFile(sdRoot() + "TOTKBench.txt", f);
    CHECK(f.find("------") != std::string::npos && f.find("К1 · 2934.4 МГц · прогон 2 из 3") != std::string::npos, "подпись с разделителем");
    CHECK(f.find("→ засчитано К1 · 2934.4 МГц: прогон 3 из 3") != std::string::npos && f.find("(статтеры)") != std::string::npos, "итог прогона в файле");
    // Подписи не мешают разбору: прогонов в файле ровно четыре.
    CHECK(parseTotkBench(f).size() == 4, "разбор файла с подписями: %zu прогонов", parseTotkBench(f).size());
    tu.computeRanking();
    CHECK(tu.bestCandidate() < 0, "безопасный режим: кандидат со статтерами не рекомендуется");
    tu.cfg.ramMode = Settings::ModeMedium;
    tu.computeRanking();
    CHECK(tu.bestCandidate() == 1, "средний: статтеры только штрафуют");
    tu.cfg.ramMode = 1;
    tu.computeRanking();
    CHECK(tu.bestCandidate() == 1, "максимум: рекомендуется со штрафом");
    std::string reg;
    CHECK(readFile(paths::unstable(), reg) && reg.find("статтеры") != std::string::npos, "статтеры в реестре нестабильных");
    tu.gameFlag(t, 1, "артефакты");
    CHECK(tu.cands[1].artifacts == 1, "отметка «артефакты»");
    printf("  файл бенчмарка:\n");
    for (auto& l : split(f, '\n'))
        if (!l.empty()) printf("    %s\n", l.c_str());
    removeFile(paths::unstable());
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
        tu.cfg.communityBands = false;  // модель чипа — под Common гайда
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
    t.powerCycle();
    delete tu;
    tu = newTuner();
    CHECK(tu->gameCollect(t, &msg) == -2, "перезагрузка распознана без диалога (%s)", msg.c_str());
    CHECK(tu->cands[2].game == 2 && tu->gamePendingCand == -1 && tu->gameActiveCand == -1, "К2: сбой, ожидание снято");
    // Прогон на стоковых настройках после перезагрузки не должен засчитаться К2.
    bench(2900);
    CHECK(tu->gameCollect(t, &msg) == 0 && tu->cands[2].runs.empty(), "прогон не на настройках К2 не засчитан");

    // 4б. «Только применить» (поиграть): РАМ и тайминги — да, ЦПУ/ГПУ — как в профиле; прогон в игре не засчитывается.
    CHECK(tu->gameApply(t, 2, &err, false), "только применить К2: %s", err.c_str());
    CHECK(t.override_ == tu->cands[2].hz && !t.gameClocksActive() && t.cpuOv == 0 && t.gpuOv == 0, "частота РАМ стоит, ЦПУ/ГПУ не тронуты");
    CHECK(tu->gamePendingCand == -1 && tu->gameActiveCand == 2, "результата не ждём, кандидат применён");
    bench(3040);
    CHECK(tu->gameCollect(t, &msg) == 0 && tu->cands[2].runs.empty() && msg.find("без частот ЦПУ/ГПУ") != std::string::npos,
          "прогон без частот прогона не засчитан (%s)", msg.c_str());

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
    CHECK(tu->bestCandidate() < 0, "безопасный режим: кандидаты со сбоями в игре не рекомендуются");
    tu->cfg.ramMode = Settings::ModeMedium;
    tu->computeRanking();
    CHECK(tu->bestCandidate() < 0, "средний режим: вылет в игре исключает кандидата");
    tu->cfg.ramMode = 1;  // максимум: сбои только штрафуют
    tu->computeRanking();
    int best = tu->bestCandidate();
    CHECK(best >= 0, "режим «Максимум»: есть лучший кандидат");
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

// Разбор настоящего журнала с консоли: sim <sdroot> --replay <state.txt> [слот, что вешает память из кипа].
// Консоль «перезапускает» приложение с этим журналом; в кипе — значения из журнала (kipDesired текущей проверки).
static int replay(const std::string& stateFile, int hangSlot) {
    std::string text;
    if (!readFile(stateFile, text)) {
        printf("нет файла %s\n", stateFile.c_str());
        return 1;
    }
    writeFile(paths::state(), text);
    SimTarget t;
    Tuner probe;
    probe.cfg.communityBands = false;  // модель чипа — под Common гайда
    bool rec = false;
    probe.loadState(t, &rec);
    for (auto& kv : probe.kipDesired(probe.kipTesting)) t.kipConfig[kv.first] = kv.second;
    t.m.tmeh = t.kipConfig;
    if (hangSlot >= 0) t.m.kipHangBad = {hangSlot};
    printf("журнал: этап «%s», в кипе: %s\n", phaseName(probe.phase), knobSetShort(probe.kipVals).c_str());
    Tuner tu;
    tu.cfg.communityBands = false;  // модель чипа — под Common гайда
    tu.loadState(t, &rec);
    printf("прерванная проба засчитана: %s\n", rec ? "да" : "нет");
    int reboots = 0;
    runToGame(t, tu, &reboots, 3000);
    for (auto& l : tu.kipLog) printf("  · %s\n", l.c_str());
    printf("итог: этап «%s», перезагрузок %d, в кипе ramPD=%d\n", phaseName(tu.phase), t.boots, t.m.tmeh[14]);
    return tu.phase == Phase::Game && t.m.tmeh[14] == 0 ? 0 : 1;
}

#ifdef RAMSS_PUBLIC
// Сборка под Ryazha-clk 3.5.5: этапы кипа выключены по умолчанию — даже на демоне со сторожем кип не трогается.
static void testPublicDefaults() {
    printf("[сборка под Ryazha-clk 3.5.5]\n");
    Settings d;
    CHECK(!d.kipStage && !d.kipFirst, "перенос в кип и «кип сначала» по умолчанию выключены");
    removeFile(paths::state());
    removeFile(paths::unstable());
    removeFile(paths::kipBackup());
    removeFile(paths::kipCommonBackup());
    struct Wd : SimTarget {
        int watchdogState() override { return 1; }
    } t;
    t.commonSupported = true;
    writeFile(paths::timings(), "");
    Tuner tu;
    tu.cfg.applyRamMode(Settings::ModeMax);
    tu.begin(makeLadder(), chipFromDramId(17));
    tu.singleHz = 2400000000u;
    int reboots = 0;
    bool ok = runToGame(t, tu, &reboots, 30000);
    printf("  перезагрузок %d, кип %s, финиш %s, этап %s\n", t.boots, t.kipConfig.empty() ? "не тронут" : "ИЗМЕНЁН", tu.rfDone ? "прошёл" : "нет",
           phaseName(tu.phase));
    CHECK(ok && t.boots == 0 && t.kipConfig.empty() && !tu.kfTried && tu.rfDone && tu.phase == Phase::Game,
          "кип не трогается, финиш идёт, подбор доходит до TOTK (%s)", phaseName(tu.phase));
    removeFile(paths::unstable());
}
#endif

// Свитспот: самая выгодная частота по потоку и кадрам TOTK — от двух частот, нижняя в пределах допуска выгоднее.
static void testSweetSpot() {
    printf("[свитспот]\n");
    // Одна частота — сравнивать нечего.
    CHECK(sweetSpot({{2828800000u, 22200, 2772}, {2828800000u, 22100, 2775}}, 0.6) == -1, "одна частота — свитспота нет");
    CHECK(sweetSpot({{2828800000u, 22200, 2772}, {2934400000u, 0, 2765}}, 0.6) == -1, "без потока точка не считается");
    // Выше частота — ощутимо больше потока и кадров: она и есть.
    CHECK(sweetSpot({{2400000000u, 20000, 2600}, {2828800000u, 22200, 2772}}, 0.6) == 1, "явно лучшая частота");
    // 2934 почти не обгоняет 2828 (в пределах 0,3 %), а кадров меньше — выгоднее 2828 (как на консоли в 1.9.4).
    CHECK(sweetSpot({{2934400000u, 22300, 2765}, {2828800000u, 22200, 2772}, {2400000000u, 20000, 2600}}, 0.6) == 1,
          "почти равные — выгоднее нижняя частота");
    // Ниже допуска — верхняя: поток +3 %, кадры +1 %.
    CHECK(sweetSpot({{2828800000u, 21550, 2745}, {2934400000u, 22200, 2772}}, 0.6) == 1, "за пределами допуска — верхняя");
    // Вес TOTK 0: только поток.
    CHECK(sweetSpot({{2828800000u, 22200, 2800}, {2934400000u, 22600, 2700}}, 0.0) == 1, "вес TOTK 0 — по потоку");

    // Архив: чужой чип и «сомнительный» не участвуют.
    std::vector<ArchEntry> a(4);
    a[0].hz = 2828800000u, a[0].bw = 22200, a[0].frames = 2772, a[0].dram = 17, a[0].name = "К1, TOTK 3 пр.";
    a[1].hz = 2934400000u, a[1].bw = 23500, a[1].frames = 2850, a[1].dram = 9, a[1].name = "К2";
    a[2].hz = 3000000000u, a[2].bw = 23600, a[2].frames = 2860, a[2].dram = 17, a[2].name = "К3, сомнительный: сбоев 2 из 3";
    a[3].hz = 2400000000u, a[3].bw = 20000, a[3].frames = 2600, a[3].dram = 17, a[3].name = "ручная настройка";
    CHECK(archSweet(a, 17, 0.6) == 0, "архив: свитспот среди своего чипа (%d)", archSweet(a, 17, 0.6));
    CHECK(archSweet({a[0]}, 17, 0.6) == -1, "архив: одна частота — нет");

    // Кандидаты: только проверенные всеми прогонами без вылетов.
    auto mk = [](int id, u32 hz, double bw, double frames, int runs) {
        Candidate c;
        c.id = id;
        c.hz = hz;
        c.b.valid = true;
        c.b.readMBs = c.b.writeMBs = c.b.copyMBs = bw;
        c.game = runs ? 1 : 0;
        for (int i = 0; i < runs; i++) {
            TotkRun r;
            r.location = "Korok Forest";
            r.frames = frames;
            r.avg = frames / 100;
            r.low1 = r.low01 = 25;
            c.runs.push_back(r);
        }
        return c;
    };
    Tuner tu;
    Candidate base = mk(0, 1600000000u, 12000, 2400, 3);
    base.baseline = true;
    tu.cands = {base, mk(1, 2934400000u, 22300, 2765, 3), mk(2, 2828800000u, 22200, 2772, 3), mk(3, 3000000000u, 23000, 2900, 1)};
    CHECK(tu.sweetCandidate() == 2, "кандидаты: свитспот К2 на 2828.8 (%d)", tu.sweetCandidate());
    tu.cands[2].crashes = 1;
    CHECK(tu.sweetCandidate() == -1, "кандидат с вылетом не в счёт, одной частоты мало (%d)", tu.sweetCandidate());
    tu.cands[3] = mk(3, 3000000000u, 23000, 2900, 3);
    CHECK(tu.sweetCandidate() == 3, "дождались всех прогонов — верхняя (%d)", tu.sweetCandidate());
    std::string rep = tu.report();
    CHECK(rep.find(SWEET_MARK) != std::string::npos, "свитспот в отчёте");
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "./sdroot";
    setSdRoot(root);
    makeDirs(paths::rclkDir());
    makeDirs(paths::appDir());
    if (argc > 3 && std::string(argv[2]) == "--replay") return replay(argv[3], argc > 4 ? atoi(argv[4]) : -1);
    if (argc > 3 && std::string(argv[2]) == "--table") {
        std::string csv;
        readFile(argv[3], csv);
        printf("%s", Tuner::researchTable(csv).c_str());
        return 0;
    }

    testTimingsIni();
    testTotkParse();
    testBenchFiles();
    testUltraCamIni();
    testLog();
    testGameCrashFlow();
    testSweep();
#ifndef RAMSS_PUBLIC  // в сборке под 3.5.5 этапы кипа выключены по умолчанию — их проверяет основная сборка
    testKipBisect();
#endif
    testTotkMarks();
    testLoosenOrder();
    testGuideSweep();
    testSingle();
#ifndef RAMSS_PUBLIC
    testKipGuard();
    testKipFirst();
    testKipFirstLimits();
#else
    testPublicDefaults();
#endif
    testResearch();
    testBands();
    testAbsLimits();
    testConfirmGain();
    testGuideHangNoCap();
    testSafeMode();
    testExitVsCrash();
    testOptChoice();
    testSweetSpot();

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
    testHistory();
    testArchive();
    testDoubtful();
    testMaxNoise();
    testWarmMedianRetry();
    testMaxPaired();
    testFreshRef();
    testRefreshStage();
    testIniEdit();
    testTotkRunCap();
    testWarmRestart();

    printf(g_fail ? "\nИТОГ: ошибок %d\n" : "\nИТОГ: все проверки пройдены\n", g_fail);
    return g_fail ? 1 : 0;
}
