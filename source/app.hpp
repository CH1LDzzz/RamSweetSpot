// Приложение: состояние, ввод и экраны.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#include "console.hpp"
#include "dram.hpp"
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

    // ---- данные
    ConsoleTarget m_t;
    Tuner m_tu;
    Settings& cfg() { return m_tu.cfg; }

    bool m_daemon = false;
    std::string m_svc, m_daemonVer;
    u32 m_api = 0;
    bool m_apiConfirmed = false;
    std::vector<u32> m_ladder;
    std::vector<u32> m_cpuList, m_gpuList;
    int m_hwType = -1;
    ChipInfo m_chip;
    bool m_appMode = false;
    bool m_buffersOk = false;
    std::string m_bufInfo;
    std::string m_initError;

    RclkContext m_ctx;
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
        std::function<void()> yes, alt;
        std::string yesLabel, altLabel;
    } m_dlg;

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
