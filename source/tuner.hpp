// Автоподбор «свитспота» памяти: частота + тайминги + задержки, с журналом на SD.
#pragma once

#include <atomic>
#include <mutex>

#include "common.hpp"
#include "dram.hpp"
#include "knobs.hpp"
#include "membench.hpp"
#include "totk.hpp"

struct Settings {
    int wBw = 50;           // вес пропускной способности в синтетике, % (остальное — задержка)
    int wGame = 60;         // вес TOTK в итоговой оценке, %
    int quickSec = 6;       // memtest на каждую пробу, с
    int verifySec = 60;     // длинная проверка финального набора, с
    int benchPasses = 2;    // повторов замера (берётся лучший)
    int margin = 1;         // запас стабильности, ступеней
    int patience = 2;       // сколько ступеней без прироста терпеть
    int minMhz = 1300;      // низ прогона частот (ступени ниже не проверяются)
    int maxMhz = 0;         // верх прогона частот (0 = верх лестницы демона)
    int scanFailStop = 0;   // остановить прогон после стольких сбоев подряд (0 = пройти весь диапазон)
    int numTargets = 2;     // сколько частот доводить таймингами
    int tempLimit = 72;     // °C SoC: выше — пауза на остывание
    bool allowTrefiHigh = false;  // разрешить tREFI 11700 нс
    bool advanced = false;        // расширенные ручки арбитра и шины
    bool usePreset = true;        // стартовать с пресета чипа из гайда
    bool useDaemonLog = true;     // подтверждать применение по логу Ryazha-clk
    bool autoLaunch = true;       // запускать TOTK автоматически
    int cpuTuneMhz = 2397;        // ЦПУ на время подбора РАМ (синтетика, memtest)
    int cpuGameMhz = 2397;        // ЦПУ на время прогона бенчмарка TOTK
    int gpuGameMhz = 1305;        // ГПУ на время прогона бенчмарка TOTK

    bool load();
    bool save() const;
    std::string serialize() const;
    void deserialize(const std::string& text);
};

struct ApplyInfo {
    u32 realHz = 0;                       // реальная частота памяти после применения
    bool confirmed = false;               // демон подтвердил, что тайминги применены
    std::vector<std::string> notApplied;  // "[hw] НЕ ДОШЛО ..."
};

// Всё, что алгоритму нужно от консоли. На Switch — Ryazha-clk + замеры, в тестах — симулятор.
class ITarget {
public:
    virtual ~ITarget() {}
    virtual bool apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) = 0;
    virtual bool applyOriginal(std::string* err) = 0;                         // как было до подбора
    virtual bool commit(u32 hz, const KnobSet& set, std::string* err) = 0;    // записать итог навсегда
    virtual bool stability(int seconds, std::string* err, bool* aborted) = 0;
    virtual BenchResult bench(int passes, bool* aborted) = 0;
    virtual u32 realHz() = 0;
    virtual u32 overrideHz() = 0;
    // Частоты ЦПУ/ГПУ для прогона TOTK (одинаковые для всех кандидатов — иначе сравнение нечестное).
    virtual bool setGameClocks(std::string* note) = 0;
    // Частоты прогона TOTK всё ещё стоят (консоль не перезагружалась, никто их не снял)?
    virtual bool gameClocksActive() = 0;
    virtual int socTempC() = 0;
    virtual bool saveText(const std::string& path, const std::string& text) = 0;
    virtual bool loadText(const std::string& path, std::string& text) = 0;
    virtual void idle(u32 ms) = 0;
};

enum class Phase : int { Idle = 0, Baseline, FreqScan, Seed, Timing, Verify, Game, Done, Failed };
const char* phaseName(Phase p);

struct FreqPoint {
    u32 hz = 0;
    int status = 0;  // 0 не проверялась, 1 стабильна, 2 сбой, 3 стабильна, но медленнее предыдущей (похоже на PLL drop)
    BenchResult b;
    double score = 0;
    std::string reason;
};

struct Candidate {
    int id = 0;
    bool baseline = false;  // «как было до подбора»
    u32 hz = 0;
    KnobSet set;
    BenchResult b;
    double synth = 0;
    int game = 0;  // 0 не проверен в игре, 1 последний запуск успешен, 2 последний запуск — сбой
    int crashes = 0;   // сколько раз игра падала / консоль перезагружалась на этом наборе
    int attempts = 0;  // сколько раз набор применялся для игры
    std::vector<TotkRun> runs;
    std::string note;
    // считается в computeRanking()
    double gameScore = 0, combined = 0;
    std::string name() const;
};

struct TrialSpec {
    enum Kind { None = 0, Base, Scan, SeedInh, SeedPreset, Knob, Remeasure, Verify };
    int kind = None;
    u32 hz = 0;
    KnobSet set;
    int stabSec = 0;
    int passes = 1;
    std::string key;
    int val = 0;
    std::string desc;
    bool valid() const { return kind != None; }
    std::string serialize() const;
    bool deserialize(const std::string& s);
};

struct TrialOut {
    bool ok = false;
    bool aborted = false;
    std::string reason;
    BenchResult b;
    double score = 0;
    ApplyInfo ai;
};

class Tuner {
public:
    Settings cfg;
    mutable std::recursive_mutex mtx;

    // --- входные данные
    std::vector<u32> ladder;  // частоты памяти от демона, Гц, по возрастанию
    ChipInfo chip;

    // --- общее состояние
    Phase phase = Phase::Idle;
    std::string status;
    std::string error;
    int trials = 0;
    s64 startedEpoch = 0;
    std::vector<std::string> events;

    // --- базовый замер
    std::vector<BenchResult> baseRuns;
    BenchResult base;
    double eps = 0.005;  // порог шума замера (доля)

    // --- прогон частот: каждая ступень лестницы от minMhz до maxMhz
    std::vector<int> coarse;  // индексы ступеней прогона в ladder
    int scanPos = 0;
    int lastOk = -1;          // самая высокая стабильная ступень
    int failRun = 0;          // сбоев подряд
    double lastOkBw = 0;      // поток предыдущей стабильной ступени
    std::vector<FreqPoint> points;

    // --- цели (частоты, которые доводим таймингами)
    std::vector<u32> targets;
    int tIdx = 0;
    int seedStage = 0;
    double inhScore = 0;
    KnobSet presetSet;

    // --- тайминги текущей цели
    struct PlanItem {
        std::string key;
        int capMax = 0, capMin = 0;
    };
    std::vector<PlanItem> plan;
    KnobSet cur;
    double curScore = 0;
    int kIdx = 0;
    int seqStart = 0;
    bool seqStartInh = true;
    int seqPos = 0;
    bool haveBest = false;
    int bestVal = 0;
    double bestScore = 0, startScore = 0;
    int noimp = 0;
    bool haveFail = false;
    int failVal = 0;
    bool remeasure = false;
    int remeasureFails = 0;
    struct Accepted {
        std::string key;
        std::string prev;   // прежнее значение: "inh" или число
        bool edge = false;  // принято вплотную к сбою — первый подозреваемый при длинной проверке
    };
    std::vector<Accepted> accepted;
    int verifyTries = 0;
    int loosenRound = 0;

    // --- кандидаты и проверка в игре
    std::vector<Candidate> cands;
    int gamePendingCand = -1;  // запущена игра, ждём результат этого кандидата
    int gameActiveCand = -1;   // настройки этого кандидата сейчас применены (живут и после сбоя игры)
    std::string gameSnapshot;  // состояние файлов результата и отчётов о падениях на момент последней сверки

    // --- незавершённая проба (если консоль зависла — она будет засчитана как сбой)
    TrialSpec pending;

    // Начать новый подбор.
    void begin(const std::vector<u32>& ladder, const ChipInfo& chip);
    // Один шаг автоподбора. false — автоматическая часть закончилась (или пауза/ошибка).
    bool step(ITarget& t, std::atomic<bool>* stop);
    bool isAuto() const;

    bool saveState(ITarget& t);
    // Загрузить журнал. Если в нём осталась незавершённая проба — она засчитывается как зависание.
    bool loadState(ITarget& t, bool* recoveredHang);
    void resetAll();

    double scoreOf(const BenchResult& b) const;
    u32 stockHz() const;

    // --- проверка в TOTK
    bool gameApply(ITarget& t, int idx, std::string* err);
    // >0 — сколько прогонов собрано; 0 — нового нет; -1 — игра упала; -2 — консоль перезагружалась.
    int gameCollect(ITarget& t, std::string* msg);
    // Настройки кандидата больше не применены (ручной режим, новый подбор, возврат как было).
    void gameDeactivate(ITarget& t);
    void gameMarkFailed(ITarget& t, int idx, const std::string& why);
    void gameClearRuns(ITarget& t, int idx);
    void gameCancelPending(ITarget& t);
    void computeRanking();
    int bestCandidate() const;
    bool finalize(ITarget& t, int idx, std::string* err);

    std::string report() const;
    std::string serialize() const;
    bool deserialize(const std::string& text);

private:
    void ev(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    TrialSpec nextTrial();
    TrialOut runTrial(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop);
    void handleResult(const TrialSpec& spec, const TrialOut& out);

    void finishBaseline();
    void buildCoarse();
    int scanNextIdx() const;
    void finishScan();
    void startTiming(const KnobSet& set, double score);
    void nextTarget();
    void resetKnob();
    std::vector<int> buildSeq() const;
    void finishKnob();
    bool loosenOne(size_t accIdx);
    bool loosenForVerify();
    void enterGame();
    void addCandidate(u32 hz, const KnobSet& set, const BenchResult& b, double score);
    std::string valueStr(const std::string& key) const;
    void setValue(KnobSet& s, const std::string& key, const std::string& val) const;
};
