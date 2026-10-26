// Приложение: состояние, ввод и экраны.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#include "console.hpp"
#include "archive.hpp"
#include "dram.hpp"
#include "history.hpp"
#include "rclk.hpp"
#include "totk.hpp"
#include "tuner.hpp"

enum Screen { S_Auto = 0, S_Totk, S_Manual, S_Results, S_Settings, S_Service, S_Count };

class App {
    friend struct AppPreview;  // предпросмотр экранов на ПК (tests/preview.cpp)

public:
    bool init();
    void run();
    void shutdown();

    // ---- вызывается из потоков
    void workerMain();
    void onApplet(int hook);

private:
    // ---- кадр
    void input(u64 down, u64 held);
    u64 navKeys(u64 down, u64& held);           // стики -> крестовина, автоповтор вверх/вниз
    int wrapStep(int sel, int n, int d) const;  // шаг по списку: по кругу, при автоповторе — до края
    void draw();
    void drawWarning();
    void drawHeader();
    void drawTabs();
    void drawFooter();
    void drawDialog();
    void drawToast();
    void drawAuto(int x, int y, int w, int h);
    void drawTotk(int x, int y, int w, int h);
    void drawManual(int x, int y, int w, int h);
    void drawResults(int x, int y, int w, int h);
    void drawSettings(int x, int y, int w, int h);
    void drawService(int x, int y, int w, int h);

    // ---- ввод по экранам
    void inputAuto(u64 down);
    void inputTotk(u64 down);
    void inputManual(u64 down);
    void inputResults(u64 down);
    void inputSettings(u64 down);
    void inputService(u64 down);

    // ---- действия
    void startAuto(bool fresh);
    void pauseAuto();
    bool startJob(const char* title, std::function<void()> fn);
    bool busy() const { return m_running.load(); }
    void refreshContext(bool force);
    void toast(const std::string& s, u32 color = 0);
    void confirm(const std::string& title, const std::string& text, std::function<void()> yes, const char* yesLabel = "Да",
                 std::function<void()> alt = nullptr, const char* altLabel = nullptr);
    void collectGameOnStart();
    void saveProfileForTotk(int candIdx);
    void restoreEverything();
    void requestExit(bool restore);
    void acceptWarning();            // предупреждение принято: до перезагрузки консоли больше не показывать
    static std::string warnedFile();  // момент загрузки, в которую предупреждение принято

    // ---- данные
    ConsoleTarget m_t;
    Tuner m_tu;
    Settings& cfg() { return m_tu.cfg; }

    bool m_daemon = false;
    std::string m_svc, m_daemonVer;
    u32 m_api = 0;
    bool m_apiConfirmed = false;
    bool m_kipNeedsReboot = false;
    bool m_kipResume = false;  // отжим перевёл подбор на этап «Перенос в кип»
    void offerStabilize(int idx, const char* why);
    void stabilizeCand(int idx);  // кип вернули к значениям до переноса — нужна перезагрузка
    std::vector<u32> m_ladder;
    std::vector<u32> m_cpuList, m_gpuList;
    int m_hwType = -1;
    ChipInfo m_chip;
    bool m_appMode = false;
    bool m_buffersOk = false;
    std::string m_bufInfo;
    std::string m_initError;

    RclkContext m_ctx;
    bool m_psm = false;  // служба psm (заряд АКБ) открыта
    int m_battPct = -1;  // заряд АКБ, % (-1 — неизвестно)
    u64 m_ctxAt = 0;

    // ---- интерфейс
    int m_screen = S_Auto;
    bool m_warned = false;
    int m_sel[S_Count] = {};
    std::string m_toast;
    u32 m_toastColor = 0;
    u64 m_toastUntil = 0;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_launchGame{false};
    bool m_restoreOnExit = false;
    std::mutex m_uiMtx;  // строки, которые пишет рабочий поток (тост, заметки ручного режима)

    struct Dialog {
        bool open = false;
        std::string title, text;
        std::function<void()> yes, alt, no;  // no — на B (закрыть), если нужно что-то сделать при отказе
        std::string yesLabel, altLabel;
        bool danger = false;  // предупреждение, которое нельзя не заметить
    } m_dlg;
    int m_dlgScroll = 0;  // прокрутка текста диалога
    // Скрытое меню: нажатия левого стика подряд (их число — «Частот для доводки таймингов»).
    bool m_hmOpen = false;
    int m_hmSel = 0;
    int m_slCount = 0;
    u64 m_slAt = 0;
    // Планировщик ГПУ — системная настройка HOS [am.gpu] gpu_scheduling_enabled в atmosphere/config/system_settings.ini
    // (тот же ключ, что у Ryazha-clk со способом INI). Система читает её при загрузке: после смены нужна перезагрузка.
    int m_gpuSched = -1;             // в файле: 1 вкл., 0 выкл., -1 не задано (как в системе)
    int m_gpuSchedRclk = 0;          // config.ini Ryazha-clk gpu_scheduling: 0 не менять, 1 вкл., 2 выкл. — перекрывает при загрузке
    bool m_gpuSchedPending = false;  // меняли в эту загрузку — подействует после перезагрузки
    void loadGpuSched();
    bool setGpuSched(bool enabled, std::string* err);
    std::string gpuSchedText() const;
    bool fun(int i) const { return (m_tu.cfg.hidden >> i) & 1; }
    // Экран TOTK: выбранный кандидат — все прогоны, среднее, сравнение с другими наборами той же частоты.
    void drawTotkDetails(int x, int y, int w, int h, int sel);
    std::vector<ArchEntry> m_archCache;  // архив для сравнения (перечитывается раз в 3 с и после правки архива)
    u64 m_archAt = 0;
    void stickClicks(u64 down);
    void drawHidden();
    std::string funEvent(const std::string& e) const;  // строка журнала на экране
    std::string funNums(const std::string& e) const;   // поток и кадры в строке
    int drawFunText(int x, int y, int size, u32 c, const std::string& s, int maxW);  // строка окна выбора
    std::string bwStr(double mbs) const;                // «N МБ/с» на экране
    double framesShown(double f) const;                 // кадры TOTK на экране
    // Кадры TOTK после всех прогонов: число и надпись. Возвращает ширину.
    int drawFrames(int x, int y, int size, double frames, int maxW);
    u64 m_navRepAt = 0;     // когда повторить зажатое вверх/вниз (0 — не зажато)
    bool m_navRep = false;  // нажатие этого кадра — от автоповтора
    // Выбор из списка (отметка после игры и т. п.).
    struct Picker {
        bool open = false;
        std::string title;
        std::vector<std::string> opts;
        int sel = 0;
        std::function<void(int)> fn;
    } m_pick;
    void pick(const std::string& title, const std::vector<std::string>& opts, std::function<void(int)> fn, int sel = 0);
    void startSingle(u32 hz);
    void startResearch(bool fresh);
    u32 m_singleReq = 0;  // подбор одной частоты: какую (0 — полный подбор)
    u64 m_autoResumeAt = 0;  // после зависания: когда продолжить подбор самому (0 — не продолжать)
    // «Результаты» за всё время: сводка истории частот (history.csv), перечитывается при включении вида.
    bool m_resAll = false;
    std::vector<HistFreq> m_hist;
    HistFreq m_histBase;
    size_t m_histRows = 0;
    int m_histSel = 0;
    void loadHistory();
    void drawHistory(int x, int y, int w, int h);
    bool m_wdOff = false;    // сторож Ryazha-clk выключен или его нет: зависание не перезагрузит консоль
#ifdef RAMSS_PUBLIC
    bool m_noWd = true;      // сторожа нет вовсе (Ryazha-clk 3.5.5) — до связи с демоном считаем по сборке
#else
    bool m_noWd = false;
#endif
    bool m_safe = false;     // безопасный режим сторожа: демон не ставит частоты до перезагрузки через меню
    u64 m_safeRebootAt = 0;  // когда перезагрузить консоль, чтобы вернуть разгон (0 — не перезагружать)
    void scheduleSafeReboot(int sec);
    void cancelSafeReboot();
    // Окно «Безопасный режим Ryazha-clk»: перезагрузить или остаться без разгона. autoSec > 0 — идёт подбор:
    // без выбора перезагрузка сама через столько секунд.
    void offerSafeReboot(int autoSec);
    void confirmDanger(const std::string& title, const std::string& text, std::function<void()> yes, const char* yesLabel);
    void drawPicker();
    void launchCandidate(int idx);
    void offerOptimize(int idx);
    void runOptimize(int idx);   // оптимизация таймингов кандидата — в любой момент после прогонов
    void writeFinal(int idx);    // записать кандидата итогом: timings.ini (+ профиль TOTK)
    void totkActions(int idx);   // меню «Действия» на экране TOTK
    // Архив успешных конфигураций (archive.txt): сохранить кандидата или ручную настройку, применять в любой момент.
    void archiveSaveCand(int idx);
    void archiveSaveManual();
    void archiveMenu();
    void archiveEntryActions(size_t i);
    void saveProfileHz(u32 hz);  // частота РАМ — в профиль TOTK у Ryazha-clk

    // ---- рабочий поток
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopReq{false};
    std::atomic<bool> m_userPause{false};
    std::atomic<bool> m_focusLost{false};
    std::atomic<bool> m_exitRequested{false};
    bool m_jobAuto = false;
    std::function<void()> m_jobFn;
    std::string m_jobTitle;
#ifdef __SWITCH__
    Thread m_thread;
    bool m_threadValid = false;
    AppletHookCookie m_hook;
#endif

    // ---- ручной режим
    int m_manFreq = -1;   // индекс в m_ladder
    KnobSet m_manSet;
    int m_manKnob = 0;
    BenchResult m_manLast, m_manPrev;
    std::string m_manNote;
    u32 m_manAppliedHz = 0;
    KnobSet m_manAppliedSet;

    // ---- служебное
    std::vector<std::string> m_logTail;
    std::string m_logTitle;
    std::string m_ucIni;
    int m_ucBench = -1;
    TotkRun m_lastRun;
    bool m_haveLastRun = false;
    std::string m_lastRunFile;
};
