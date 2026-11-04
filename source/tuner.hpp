// Автоподбор «свитспота» памяти: частота + тайминги + задержки, с журналом на SD.
#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>

#include "common.hpp"
#include "dram.hpp"
#include "knobs.hpp"
#include "membench.hpp"
#include "totk.hpp"

struct Settings {
    // Режим работы с РАМ: 0 — безопасный (стабильность гарантирована: длинные проверки, запас 2, любое замечание в игре
    // исключает кандидата), 2 — средний, околостабильный (каждое значение на ступень мягче предела: запас 1; вылет
    // исключает, остальные замечания штрафуют), 1 — максимум (главное — поток и TOTK, запас 0, всё только штрафует).
    // Коды 0 и 1 — как до 1.6.5. Смена режима переставляет связанные настройки (applyRamMode), их можно подправить.
    enum { ModeSafe = 0, ModeMax = 1, ModeMedium = 2 };
    int ramMode = ModeSafe;
    int totkRuns = 3;       // прогонов TOTK на кандидата: результат — среднее
    // Цель — пропускная способность: тайминги, что дают только задержку, не окупают риск. 100 = задержка не учитывается.
    int wBw = 80;           // вес пропускной способности в синтетике, % (остальное — задержка: её дают tRCD, tRP, tRRD)
    int wGame = 60;         // вес TOTK в итоговой оценке, %
    int quickSec = 6;       // memtest на каждую пробу, с
    int verifySec = 60;     // длинная проверка финального набора, с
    int benchPasses = 2;    // повторов замера (берётся лучший)
    int margin = 1;         // запас стабильности, ступеней
    int patience = 2;       // сколько ступеней без прироста терпеть
    int minMhz = 1300;      // низ прогона частот (ступени ниже не проверяются)
    int maxMhz = 0;         // верх прогона частот (0 = верх лестницы демона)
    int scanFailStop = 0;   // остановить прогон после стольких сбоев подряд (0 = пройти весь диапазон)
    // Зависаний (перезагрузок) подряд, после которых прогон частот заканчивается: выше память физически не берёт,
    // а каждая лишняя ступень — ещё одна перезагрузка. Сбрасывает любая стабильная ступень. 0 — не останавливать.
    int scanHangStop = 2;
    int researchRuns = 3;   // исследование потока: повторов прогона лестницы (сток и гайд в каждом)
    bool autoResume = true; // после зависания подбор продолжается сам (через 5 с, B — отмена)
    // Замеров на пробу таймингов (и на базовый замер); итог — усечённое среднее. В «максимуме» — замеров отбора.
    int timingPasses = 5;
    int numTargets = 2;     // сколько частот доводить таймингами
    int tempLimit = 72;     // °C SoC: выше — пауза на остывание
    bool allowTrefiHigh = false;  // разрешить tREFI 11700 нс
    bool advanced = false;        // расширенные ручки арбитра и шины
    bool usePreset = false;       // стартовать с пресета чипа из гайда Horizon-OC (выкл. — сток Ryazha-clk)
    bool useDaemonLog = true;     // подтверждать применение по логу Ryazha-clk
    bool autoLaunch = true;       // запускать TOTK автоматически
    int cpuTuneMhz = 2397;        // ЦПУ на время подбора РАМ (синтетика, memtest)
    int cpuGameMhz = 2397;        // ЦПУ на время прогона бенчмарка TOTK
    int gpuGameMhz = 1305;        // ГПУ на время прогона бенчмарка TOTK
    bool optimize = true;         // после поиска пределов перебрать тайминги и взять лучший поток, а не самые тугие
    bool scanGuide = true;        // после прогона на стоке — второй прогон на таймингах гайда Horizon-OC для чипа
    // Пики потока: стабильная частота, после которой следующая ступень даёт на peakDropMb МБ/с меньше и хуже.
    // Такие частоты тоже доводятся таймингами: высокий поток на меньшей частоте — шанс для памяти, что выше не идёт.
    int peakDropMb = 100;
    int peakTargets = 2;          // сколько пиков (кроме верхней частоты) добавить в доводку
    int scanPasses = 5;           // замеров на каждой ступени прогонов частот; итог — медиана (с 3 и больше)
#ifdef RAMSS_PUBLIC
    // Сборка под Ryazha-clk 3.5.5: сторожа нет — без него зависание на загрузке кип не откатит, этапы кипа выключены.
    bool kipStage = false;
#else
    bool kipStage = true;         // перенести найденные ручки пропускной способности в кип (с перезагрузками)
#endif
    // Тест: «кип сначала» — до прогонов общие тайминги гайда в кип и перезагрузка: с обучением памяти на загрузке
    // частота может держать то, что живьём (timings.ini, без обучения) не держит. Частоты, что на таком кипе
    // без живых правок нестабильны, отбрасываются.
#ifdef RAMSS_PUBLIC
    bool kipFirst = false;
#else
    bool kipFirst = true;
#endif
    // Тайминги гайда по полосам частот из ручной настройки такой же памяти (есть не у всех чипов). Пока не используется:
    // в настройках не показывается и из settings.ini не читается.
    bool communityBands = false;
    int kipCheckSec = 30;         // memtest на каждой частоте кандидата после загрузки с новым кипом, с
    // Финиш — tREFI и tRFC вместе на итоговом кипе перед TOTK и в оптимизации после TOTK. По умолчанию включён только
    // в «максимуме» (applyRamMode); может ухудшить стабильность и кадры, в остальных режимах — выключен.
    bool refreshTune = false;
    int hidden = 0;               // биты скрытого меню (в настройках не показывается)

    void applyRamMode(int mode);
    static const char* ramModeName(int mode);
    static int nextRamMode(int mode);  // по кругу: безопасный -> средний -> максимум

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

constexpr int kWatchdogNone = -2;  // сторожа у демона нет вовсе (Ryazha-clk 3.5.5 и раньше)

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
    // Снять временные частоты ЦПУ/ГПУ — вернуть профиль (кандидат применён не для замера, а чтобы играть).
    virtual void freeClocks() {}
    virtual int socTempC() = 0;
    virtual bool saveText(const std::string& path, const std::string& text) = 0;
    virtual bool loadText(const std::string& path, std::string& text) = 0;
    virtual void idle(u32 ms) = 0;
    // Ручки пропускной способности в кипе Ryazha-clk: слот tMEH -> значение. Применяются только после перезагрузки.
    virtual bool kipRead(std::map<int, int>* slots, std::string* err) = 0;
    // Записать указанные слоты (остальные не трогаются) и сверить, что демон их принял.
    virtual bool kipWrite(const std::map<int, int>& slots, std::string* err) = 0;
    // Перезагрузка через систему (иначе сторож Ryazha-clk сочтёт её зависанием). На консоли при успехе не возвращается.
    virtual bool reboot(std::string* err) = 0;
    virtual u64 uptimeMs() = 0;  // время с загрузки консоли
    // Момент загрузки консоли (эпоха, с): в пределах одной загрузки постоянен, после перезагрузки — другой.
    virtual s64 bootEpoch() { return epochNow() - (s64)(uptimeMs() / 1000); }
    // Датчики для журнала испытаний: температуры, напряжения, реальные частоты одной строкой.
    virtual std::string sensors() { return std::string(); }
    // Сторож Ryazha-clk: 1 включён, 0 выключен, -1 неизвестно. Без него зависание не откатывает правку кипа.
    virtual int watchdogState() { return -1; }  // 1 — включён, 0 — выключен, -1 — не узнать, kWatchdogNone — его нет
    // Безопасный режим сторожа: консоль загрузилась после зависания, демон не ставит частоты до перезагрузки через меню.
    // Пробы в нём ничего не говорят о стабильности — их нельзя засчитывать.
    virtual bool safeMode() { return false; }
    // Общие тайминги кипа (tRCD…tREFI, те же ступени, что в timings.ini): действуют после перезагрузки, с обучением памяти.
    virtual bool kipCommonRead(KnobSet* vals, std::string* err) { return false; }
    virtual bool kipCommonWrite(const KnobSet& vals, std::string* err) { return false; }
    // Верхняя граница общего тайминга, которую демон принимает в кип (выше — молча сбрасывает в 0). false — без границы.
    virtual bool kipCommonMax(const std::string& key, int* max) { return false; }
};

// Ключ timings.ini -> слот tMEH в кипе (те же значения и знак, сверено с timing_overrides.cpp Ryazha-clk 3.6.0); -1 — не в кипе.
int kipSlotOf(const std::string& key);

// Kip, Optimize, KipFirst — в конце: номер этапа хранится в журнале числом.
enum class Phase : int { Idle = 0, Baseline, FreqScan, Seed, Timing, Verify, Game, Done, Failed, Kip, Optimize, KipFirst, Refresh };
const char* phaseName(Phase p);

struct FreqPoint {
    u32 hz = 0;
    int status = 0;  // 0 не проверялась, 1 стабильна, 2 сбой, 3 стабильна, но медленнее предыдущей (похоже на PLL drop)
    BenchResult b;
    double score = 0;
    std::string reason;
    int pass = 0;  // 0 — прогон на стоке Ryazha-clk, 1 — на таймингах гайда
};

// Запись реестра нестабильных значений: на какой частоте, что именно и почему было нестабильно.
// key/val — ручка, которую проверяли (пусто — весь набор: частота, кип, игра).
struct UnstableEntry {
    u32 hz = 0;
    std::string kind;    // memtest, зависание, PLL drop, вылет игры, артефакты, долгие загрузки, статтеры, …
    std::string key;
    int val = 0;
    KnobSet set;
    std::string detail;
    s64 at = 0;
    std::string toString() const;
    bool fromString(const std::string& s);
};

// Сток Ryazha-clk явно: первичные тайминги — ступень 0 (tREFI 0 = 3900 нс), ручки пропускной способности — 0
// («как считал движок»). Не зависит ни от кипа пользователя, ни от пресетов разгонных гайдов.
KnobSet ryazhaStockSet();

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
    int loosened = 0;  // сколько шагов отжима сделано (по нему же идёт очерёдность таймингов)
    int stutters = 0, artifacts = 0, longLoads = 0;  // замечено в игре (статтеры — и сами по 0.1% low)
    bool optOffered = false;                          // оптимизацию после прогонов TOTK уже предлагали
    int issues() const { return crashes + stutters + artifacts + longLoads; }
    // Сомнительный результат: от двух вылетов и не меньше половины запусков (например, 7 из 7). Кандидат не удаляется
    // и не переписывается — остаётся в списке с пометкой и не рекомендуется; отжимается его копия.
    bool doubtful() const { return crashes >= 2 && crashes * 2 >= std::max(attempts, crashes); }
    std::vector<TotkRun> runs;
    std::string note;
    // считается в computeRanking()
    double gameScore = 0, combined = 0;
    std::string name() const;
};

struct TrialSpec {
    // Confirm — парный замер А/Б: эталон ref (А) против кандидата set (Б) по очереди, без memtest; OptGuard с ref —
    // так же (набор до оптимизации против итога). Ref — эталон ручки заново: замер набора без memtest.
    // RefTune / RefVerify — финиш tREFI × tRFC: проба пары (val = tREFI * 100 + tRFC) и длинная проверка выбранной.
    enum Kind {
        None = 0, Base, Scan, SeedInh, SeedPreset, Knob, Remeasure, Verify, KipCheck, Optimize, KipPre, OptGuard, Confirm, Ref, RefTune, RefVerify
    };
    int kind = None;
    u32 hz = 0;
    KnobSet set;
    KnobSet ref;      // А/Б: эталон А
    double thr = 0;   // А/Б: Б «лучше», если разница не меньше этого (в журнал не пишется: проба строится заново)
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
    // Парный замер А/Б (Confirm): score и b — кандидат Б по всем блокам.
    double delta = 0;      // средняя по блокам разница Б к А (доля)
    double sigma = 0;      // её σ
    double refScore = 0;   // оценка эталона А по всем блокам
    int blocks = 0;
    bool refFailed = false;              // сбой у эталона, а не у кандидата
    std::vector<BenchResult> extra;      // замеры половин блоков — для оценки шума
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
    // Первый замер после запуска приложения занижен (на консоли 1.8.2: 11542 против 11668 / 11647 МБ/с) — перед
    // первой пробой сеанса делается разогревочный замер, он ни во что не идёт. Не сохраняется: нужен в каждом сеансе.
    bool sessionWarm = false;
    BenchResult base;
    double eps = 0.005;  // порог шума замера (доля)
    // «Максимум»: прирост засчитывается от 0,12 % (шум разницы 0,10 % + 0,02 %). Решает парный замер А/Б: блоки
    // «эталон, кандидат» по kPairPasses проходов, пока σ разницы не дойдёт до 0,04 % (шум = 2,5σ = 0,10 %) или
    // решение не станет очевидным; отбор на обычной пробе отсеивает явно худшие значения (ниже 0,12 % − 1,5σ).
    static constexpr double kMaxGain = 0.0012;
    static constexpr double kPairSigmaTarget = 0.0004;
    static constexpr double kScreenK = 1.5;
    static constexpr int kPairPasses = 5, kPairMinBlocks = 2, kPairMaxBlocks = 8;
    // Шум одного прохода — по всем пробам сеанса: сумма квадратов относительных отклонений проходов от итога своей
    // пробы и число степеней свободы. Пока данных мало — осторожная оценка 0,15 %.
    double noiseSS = 0;
    int noiseDf = 0;
    bool maxMode() const { return cfg.ramMode == Settings::ModeMax; }
    double passSigma() const;
    double trialSigma(int passes) const;  // σ итога пробы из passes проходов
    double pairNoise() const;             // шум разницы А/Б (2,5σ) при наибольшем числе блоков
    void addNoise(const BenchResult& b);
    // thr — граница решения: блоки добавляются, пока разница не отойдёт от неё на 2,5σ или σ не дойдёт до цели.
    TrialOut pairedConfirm(ITarget& t, u32 hz, const KnobSet& a, const KnobSet& b, std::atomic<bool>* stop, double thr);
    // Эталон ручки заново: прошлый принятый замер — отобранный как лучший из шумных, он завышен, и с ним все значения
    // следующей ручки казались хуже (оставался сток). Ставится в начале ручки и после принятого без А/Б значения.
    bool needRef = false;

    // --- прогон частот: каждая ступень лестницы от minMhz до maxMhz
    std::vector<int> coarse;  // индексы ступеней прогона в ladder
    int scanPos = 0;
    int lastOk = -1;          // самая высокая стабильная ступень
    int failRun = 0;          // сбоев подряд
    int hangRun = 0;          // зависаний подряд (стабильная ступень сбрасывает)
    // Потолок частоты: крайняя стабильная частота, после которой прогон закончился зависаниями. Всё дальше —
    // второй прогон, цели, пики, доводка — не выше него. 0 — потолка нет.
    u32 freqCap = 0;
    // Второй прогон — на таймингах гайда Horizon-OC «Common» для чипа (поверх стока Ryazha-clk): на стоке память может
    // упереться в частоту раньше. Выигрывает прогон с более высокой стабильной частотой.
    int scanPass = 0;         // 0 — сток Ryazha-clk, 1 — тайминги гайда
    int stockLastOk = -1;     // предел стока (индекс в ladder), пока идёт второй прогон
    bool guideChosen = false; // цели взяты из прогона на таймингах гайда
    KnobSet guideSet;         // сток Ryazha-clk + значения гайда (с полосами — нижней полосы, для журнала)
    bool bandsOn() const;     // тайминги гайда — по полосам проверенного набора
    KnobSet guideAt(u32 hz) const;   // сток Ryazha-clk + тайминги гайда для этой частоты
    KnobSet presetAt(u32 hz) const;  // стартовый набор цели: presetSet, а с полосами — набор её частоты
    KnobSet kipScanSet(u32 hz) const;  // «кип сначала»: без живых таймингов, задержка записи — по полосе
    std::string guideName() const;
    // Подбор одной частоты: прогоны пропускаются, цель — только она (старт — сток или гайд, что держится лучше).
    u32 singleHz = 0;
    // «Кип сначала»: общие тайминги гайда в кипе до прогонов.
    int kfStep = 0;           // 0 — записать и перезагрузить, 1 — ждём загрузку
    u64 kfBootAt = 0;         // момент загрузки при запросе перезагрузки (bootEpoch)
    bool kfTried = false;     // этап пройден (удачно или нет)
    bool kfDone = false;      // общие тайминги гайда стоят в кипе — прогоны и старт таймингов на них
    KnobSet kfVals;           // что записали в общие тайминги кипа
    bool kipFirstAdvance(ITarget& t);
    void afterBaseline();
    bool hzDiscarded(u32 hz) const;  // «кип сначала»: на кипе без живых правок частота не держится
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
    // Спорный прирост: та же проба ещё раз, решение — по среднему двух. В «максимуме» — подтверждение А/Б
    // (и в оптимизации: значение optVals[optPos] против выбранного).
    bool confirmPending = false;
    // «Максимум», рискованный тайминг: лучшая по отбору ступень цепочки ждёт одного замера А/Б против старта
    // (chainEnd — цепочка уже закончилась сбоем, замер — следующей пробой).
    bool chainAB = false, chainEnd = false;
    bool chainKey(const std::string& key) const;  // «максимум», рискованный тайминг: цепочка с одним замером А/Б
    // Прогрев перед первой пробой сеанса: kWarmSec memtest на рабочей частоте и проверенном наборе, замер не в счёт,
    // потом эталон — заново. false — прервали (пауза).
    static constexpr int kWarmSec = 30;
    bool warmUp(ITarget& t, const TrialSpec& spec);
    int benchVer = kBenchVersion;  // версия замера, которой замерены оценки журнала
    double confirmScore = 0;
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

    // --- оптимизация таймингов: не самые тугие, а дающие наибольший поток
    KnobSet limits;                  // первое сбойное значение каждой ручки на этой цели
    KnobSet absLimits;               // пик: первое значение со сбоем (memtest или перезагрузка); запас режима — от него
    std::vector<std::string> optKeys;
    int optKey = 0, optPos = 0, optPass = 0;
    std::vector<int> optVals;
    bool optHaveBest = false, optChanged = false;
    int optBestVal = 0;
    double optBestBw = 0;     // поток лучшего значения — для экрана и журнала
    double optBestScore = 0;  // его оценка: по ней и выбираем (поток и задержка — с весами настроек)
    double optMaxScore = 0;   // прежний выбор от максимума (до 1.8.8) — только для журнала прежней версии
    // Насколько выбранное значение медленнее самого быстрого подтверждённого (≤ 0): мягче берётся, только пока
    // потеря в сумме не больше половины шума — иначе выбор сползал бы вниз по ступени за ступенью.
    double optLoss = 0;
    double optThr(bool softer) const;  // порог А/Б для значения в оптимизации

    // --- финиш (эксперимент, «максимум»): tREFI и tRFC вместе, на итоговом кипе, перед TOTK. Оба — про регенерацию:
    // её доля во времени памяти ≈ tRFC / tREFI, оптимум у пары общий, а перебор по одному таких пар не находит.
    typedef std::pair<int, int> RfPair;  // tREFI, tRFC
    bool rfDone = false;                 // этап пройден в этом подборе
    std::vector<int> rfQueue;            // индексы кандидатов; первый — текущий
    int rfStage = 0;                     // 0 — эталон, 1 — перебор пар, 2 — длинная проверка выбранной
    int rfPos = 0;
    std::vector<RfPair> rfPairs, rfStack, rfBad;  // пары по очереди; принятые до выбранной; сбойные
    RfPair rfStart{0, 0}, rfPick{0, 0};
    double rfPickScore = 0;
    void enterRefresh();
    void rfNext();
    std::vector<RfPair> refreshPairs(u32 hz, RfPair start) const;
    bool rfSkip(const RfPair& p) const;  // не короче выбранной по доле регенерации или туже сбойной
    double rfThr() const;                // порог прироста финиша: «максимум» — 0,12 %, остальные — порог шума
    // Финиш для оптимизации после TOTK — те же пары и пороги, подряд. true — пара поменялась (set, steps).
    bool refreshSync(ITarget& t, u32 hz, KnobSet& set, std::string* steps);
    double guardThr() const;           // порог А/Б защиты итога: итог против набора до оптимизации
    // Защита итога: набор до оптимизации и замеры «после / до» по очереди (4 пробы); итог хуже — возвращаем прежний.
    KnobSet optStart;
    int optGuard = 0;
    double optGuardA = 0, optGuardB = 0;
    void enterOptimize();
    void optPrepareKey();
    std::vector<int> optValuesFor(u32 hz, const std::string& key, int cur) const;
    void optFinishKey();                                  // перебор ручки закончен: выбранное — в набор
    void knobNext();                                      // проба ручки учтена: следующее значение или конец
    // Оптимизация готового кандидата (после TOTK): новый кандидат «оптимизирован», его снова прогоняют в игре.
    bool optimizeCandidate(ITarget& t, int idx, std::string* msg);
    // Прогоны TOTK кандидата одной точкой: медиана кадров, FPS, 1 % и 0.1 % low (один холодный прогон не тянет итог).
    static void runsMedian(const std::vector<TotkRun>& runs, double* frames, double* fps, double* low1, double* low01);
    static constexpr size_t kMaxTotkRuns = 15;  // прогонов TOTK на кандидата храним не больше (старые уходят)

    // --- кандидаты и проверка в игре
    std::vector<Candidate> cands;
    int gamePendingCand = -1;  // запущена игра, ждём результат этого кандидата
    int gameActiveCand = -1;   // настройки этого кандидата сейчас применены (живут и после сбоя игры)
    std::string gameSnapshot;  // состояние файлов результата и отчётов о падениях на момент последней сверки

    // --- перенос ручек пропускной способности в кип (каждая проверка — перезагрузка консоли)
    // Пакет ручек ставится в кип разом; не загрузилась или не прошла проверку — пакет делится пополам,
    // пока не останется один виновник. Ручки из одной группы проверяются одной перезагрузкой.
    // KipPre — до записи: каждое значение для кипа живьём (timings.ini) на каждой частоте кандидатов. Кип действует на
    // всю лестницу, а ручку подбирали на одной частоте: на другой она может повесить память (так HP Mode с 2828.8
    // повесил 2934.4). Живая проверка без перезагрузки — зависание тут не трогает кип.
    enum KipStep { KipWrite = 0, KipBoot = 1, KipCheckRun = 2, KipPre = 3 };
    struct KipReject {
        std::string key;
        int val = 0;
        std::string reason;
    };
    int kipStep = KipWrite;
    int kipSrc = -1;                              // id кандидата, с которого берутся значения
    KnobSet kipVals;                              // ключ -> значение, которое переносим (с кандидата)
    std::map<int, int> kipOrig;                   // слот -> значение кипа до этапа (для «Вернуть как было»)
    std::vector<std::string> kipAccepted;         // уже в кипе, проверены
    std::vector<std::vector<std::string>> kipQueue;  // группы на проверку
    std::vector<std::string> kipTesting;          // группа, с которой ушли в перезагрузку
    bool kipFinal = false;                        // проверка итогового набора (после сбоя в кипе остались чужие значения)
    bool kipDirty = false;                        // в кипе сейчас не принятый набор
    std::vector<KipReject> kipRejected;
    int kipReboots = 0;
    s64 kipRebootAt = 0;                          // когда просили перезагрузку (эпоха, с)
    u64 kipUptimeAt = 0;                          // момент загрузки консоли на тот момент (bootEpoch)
    int kipCheckIdx = 0;                          // какой кандидат проверяем после загрузки
    int kipFixTries = 0;                          // сколько раз отжали тайминги на этой загрузке
    std::vector<std::pair<int, std::string>> kipPre;  // живая проверка до записи: (кандидат, ручка)
    int kipPrePos = 0;
    int kipHangs = 0;                             // зависаний за этап: 3 — аварийный откат кипа к исходному
    void kipAbort(const std::string& why);
    int kipHangLimit() const;  // сколько зависаний за этап до аварийного отката
    int kipFixCand = -1;                          // чьи тайминги отжимаем (индекс в cands)
    KnobSet kipFixBackup;                         // его тайминги до отжима: не помогло — вернём
    std::vector<std::string> kipLog;              // ход этапа для отчёта
    bool kipTouched() const { return !kipOrig.empty() && (kipReboots > 0 || kipDirty || !kipAccepted.empty()); }
    std::map<int, int> kipDesired(const std::vector<std::string>& extra) const;

    // --- реестр нестабильных значений (config/ram-sweetspot/unstable.txt, живёт между подборами)
    std::vector<UnstableEntry> unstable;
    bool unstableLoaded = false, unstableDirty = false;
    void noteUnstable(u32 hz, const std::string& kind, const std::string& key, int val, const KnobSet& set, const std::string& detail);
    void loadUnstable(ITarget& t);
    void flushUnstable(ITarget& t);
    // Значение ручки на частоте, туже которого уже был сбой (memtest / зависание): Up — верхняя граница, Down — нижняя.
    bool unstableBound(u32 hz, const std::string& key, int* bound) const;

    // --- незавершённая проба (если консоль зависла — она будет засчитана как сбой)
    TrialSpec pending;

    // --- исследование потока: повторные прогоны лестницы на стоке и гайде, без подбора и без кипа.
    // Своя запись (research.cfg / .csv / .pending), журнал подбора не трогает; после зависания продолжается.
    std::string researchStatus;  // для экрана, пока идёт
    bool safeHalt = false;       // проба не применилась, а демон в безопасном режиме: подбор встал до перезагрузки
    // Незавершённая проба: момент загрузки консоли и отчёты о падениях на её старте. При следующем запуске:
    // консоль перезагружалась — зависание; не перезагружалась, но есть новый отчёт — вылет приложения;
    // ни того ни другого — приложение просто закрыли (свернули и закрыли, запустили игру): проба повторяется.
    s64 pendingBoot = 0;
    std::vector<std::string> pendingCrash;
    void histTrial(const TrialSpec& spec, const TrialOut& out, const std::string& kind) const;  // строка в историю частот
    static bool researchActive();
    static void researchStart(int reps);
    static std::string researchStop();  // прервать: таблица по собранному, запись закрыта
    // Отметить зависшую ступень (если осталась метка) — при запуске приложения.
    // bootNow — момент загрузки консоли сейчас: та же загрузка и нет отчёта о падении — исследование просто закрыли,
    // ступень не засчитывается и повторяется.
    static bool researchRecoverHang(std::string* what, s64 bootNow = 0);
    bool research(ITarget& t, const std::vector<u32>& ladder, const ChipInfo& chip, std::atomic<bool>* stop, std::string* msg);
    static std::string researchTable(const std::string& csv);  // средние и разброс по ступеням

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
    // bench — прогон бенчмарка: ставятся частоты ЦПУ/ГПУ прогона и ждём результат. Без него — только РАМ и тайминги,
    // ЦПУ/ГПУ как в профиле (не тратим батарею), прогоны в игре не засчитываются.
    bool gameApply(ITarget& t, int idx, std::string* err, bool bench = true);
    // >0 — сколько прогонов собрано; 0 — нового нет; -1 — игра упала; -2 — консоль перезагружалась.
    int gameCollect(ITarget& t, std::string* msg);
    // Настройки кандидата больше не применены (ручной режим, новый подбор, возврат как было).
    void gameDeactivate(ITarget& t);
    void gameMarkFailed(ITarget& t, int idx, const std::string& why);
    void gameClearRuns(ITarget& t, int idx);
    // Отметка из игры: «артефакты», «долгие загрузки», «статтеры» (вылет — gameMarkFailed). В реестр нестабильных.
    void gameFlag(ITarget& t, int idx, const std::string& kind);
    int gameRunsDone(const Candidate& c) const;  // прогоны TOTK, что идут в среднее (лес короков, если он есть)
    void gameCancelPending(ITarget& t);
    void computeRanking();
    int bestCandidate() const;
    // Свитспот среди кандидатов (sweetSpot в archive.hpp): проверенные в TOTK всеми прогонами, без вылетов и сомнений,
    // в безопасном режиме — и без замечаний. Нужны минимум две частоты; -1 — сравнивать нечего.
    int sweetCandidate() const;
    bool finalize(ITarget& t, int idx, std::string* err);

    // --- отжим нестабильного набора. Порядок: тайминги -> ручки пропускной способности живьём -> ручки в кипе
    // (с перезагрузкой) -> частота ниже. Пропускная способность сдаётся последней.
    struct LoosenPlan {
        int tier = 0;  // 0 нечего, 1 тайминги, 2 пропускная способность живьём, 3 ручка в кипе, 4 частота ниже
        std::string key;
        std::string desc;
        KnobSet set;
        u32 hz = 0;
    };
    LoosenPlan nextLoosen(const Candidate& c) const;
    static const char* loosenTierName(int tier);
    // Отжать кандидата и проверить (memtest длинной проверки + замер); ступени — пока не станет стабильным.
    // Ручка в кипе снимается через этап «Перенос в кип» (перезагрузка): тогда этап становится Kip.
    bool stabilize(ITarget& t, int idx, std::string* msg);
    void kipDrop(const std::string& key, const std::string& why);

    std::string report() const;
    std::string serialize() const;
    bool deserialize(const std::string& text);

private:
    void ev(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    TrialSpec nextTrial();
    TrialOut runTrial(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop);
    TrialOut runTrialCore(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop);
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
    void enterKip();
    bool kipAdvance(ITarget& t);  // запись + перезагрузка / разбор загрузки; false — ждём перезапуска приложения
    void kipGroupFailed(const std::string& why);
    void kipGroupPassed();
    void kipFinish();
    std::vector<int> kipCheckCands() const;  // индексы кандидатов, которые проверяем после загрузки
public:
    // Пики потока в прогоне pass: частота -> насколько упал поток на следующей проверенной ступени (МБ/с).
    std::vector<std::pair<u32, double>> bandwidthPeaks(int pass) const;
    // История частот ещё не велась (первый запуск 1.8.1): перенести в неё то, что есть в журнале подбора.
    void histSeed() const;
private:
    KnobSet kipResidual(const KnobSet& set, const std::vector<std::string>& inKip) const;
    void addCandidate(u32 hz, const KnobSet& set, const BenchResult& b, double score);
    std::string valueStr(const std::string& key) const;
    void setValue(KnobSet& s, const std::string& key, const std::string& val) const;
};
