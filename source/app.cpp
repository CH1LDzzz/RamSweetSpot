#include "app.hpp"

#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdlib>

#include "gfx.hpp"
#include "log.hpp"

#ifdef __SWITCH__
static PadState g_pad;

static void workerEntry(void* p) { ((App*)p)->workerMain(); }
static void hookEntry(AppletHookType hook, void* p) { ((App*)p)->onApplet((int)hook); }
#endif

// ============================================================== запуск

bool App::init() {
    logx::begin();
    std::string err;
    if (!gfx::init(&err)) {
        m_initError = err;
        LOGE("Графика не инициализирована: %s", err.c_str());
        return false;
    }
#ifdef __SWITCH__
    appletLockExit();
    appletHook(&m_hook, hookEntry, this);
    appletSetMediaPlaybackState(true);  // не гасить экран во время долгих тестов
    AppletType at = appletGetAppletType();
    m_appMode = at == AppletType_Application || at == AppletType_SystemApplication;
    if (m_appMode) appletSetAutoSleepDisabled(true);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    u64 hw = 0, dram = 0;
    m_psm = R_SUCCEEDED(psmInitialize());  // заряд АКБ для шапки
    if (R_SUCCEEDED(splInitialize())) {
        if (R_SUCCEEDED(splGetConfig(SplConfigItem_HardwareType, &hw))) m_hwType = (int)hw;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem_DramId, &dram))) m_chip = chipFromDramId((int)dram);
        splExit();
    }
#endif
    if (!m_chip.known) m_chip.mariko = isMarikoHw(m_hwType);

    makeDirs(paths::appDir());
    cfg().load();
    m_t.cfg = &cfg();

    LOGI("Консоль: %s, DRAM ID %d (%s%s), запуск %s", consoleName(m_hwType), m_chip.dramId, m_chip.name.c_str(),
         m_chip.preset ? strf(", тир %s", m_chip.preset->tier).c_str() : "", m_appMode ? "через игру" : "через альбом");

    // Стартовое предупреждение — один раз за загрузку консоли: после прогона TOTK и при каждом возврате оно уже принято.
    {
        std::string wb;
        if (readFile(warnedFile(), wb)) {
            s64 prev = toLong(trim(wb), 0);
            s64 boot = m_t.bootEpoch();
            if (prev > 0 && std::llabs(boot - prev) <= 5) m_warned = true;  // момент загрузки считается с точностью до секунд
        }
    }

    m_daemon = rclk::init(&m_svc);
    if (m_daemon) {
        rclk::apiVersion(&m_api);
        rclk::versionString(&m_daemonVer);
        rclk::memFreqList(&m_ladder);
        std::sort(m_ladder.begin(), m_ladder.end());
        rclk::freqList(RclkModule_CPU, &m_cpuList);
        rclk::freqList(RclkModule_GPU, &m_gpuList);
        std::sort(m_cpuList.begin(), m_cpuList.end());
        std::sort(m_gpuList.begin(), m_gpuList.end());
        LOGI("Демон %s %s, API %u. Частоты: РАМ %zu ступ. (%s…%s), ЦПУ %zu (до %s), ГПУ %zu (до %s) МГц", m_svc.c_str(), m_daemonVer.c_str(), m_api,
             m_ladder.size(), m_ladder.empty() ? "-" : mhzStr(m_ladder.front()).c_str(), m_ladder.empty() ? "-" : mhzStr(m_ladder.back()).c_str(),
             m_cpuList.size(), m_cpuList.empty() ? "-" : mhzStr(m_cpuList.back()).c_str(), m_gpuList.size(),
             m_gpuList.empty() ? "-" : mhzStr(m_gpuList.back()).c_str());
        if (m_api != 2) LOGW("API демона %u: приложение сверено с API 2 (Ryazha-clk 3.5.5 и 3.6.0)", m_api);
        // Что сторож Ryazha-clk записал о прошлых перезапусках — пригодится при разборе сбоев.
        std::string wd;
        if (readFile(paths::rclkDir() + "watchdog.log", wd)) {
            auto lines = split(trim(wd), '\n');
            for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); i++)
                if (!trim(lines[i]).empty()) LOGI("watchdog.log Ryazha-clk: %s", trim(lines[i]).c_str());
        }
        int wds = m_t.watchdogState();
        m_noWd = wds == kWatchdogNone;
        m_wdOff = wds == 0 || m_noWd;
        if (m_noWd)
            LOGW("У %s нет сторожа: при зависании консоль не перезагрузится сама, перенос в кип и «Кип сначала» пропускаются",
                 m_daemonVer.empty() ? "Ryazha-clk" : m_daemonVer.c_str());
        else if (m_wdOff)
            LOGW("Сторож Ryazha-clk выключен: при зависании консоль не перезагрузится сама, «Кип сначала» пропускается");
        m_safe = m_t.safeMode();
        if (m_safe) LOGW("Ryazha-clk в безопасном режиме: загрузка после зависания, частоты не ставятся до перезагрузки через меню");
    }
    m_tu.ladder = m_ladder;
    m_tu.chip = m_chip;

    bool hang = false;
    bool have = m_tu.loadState(m_t, &hang);
    m_tu.loadUnstable(m_t);
    // История частот за всё время: если её ещё нет — начать с того, что накоплено в журнале подбора.
    if (have && !fileExists(historyPath())) {
        m_tu.histSeed();
        LOGI("История частот: начата из журнала подбора (%s)", historyPath().c_str());
    }
    m_tu.chip = m_chip;  // чип всегда берём с консоли
    bool inProgress = have && (m_tu.isAuto() || m_tu.phase == Phase::Game);
    m_t.prepareOriginal(!inProgress);
    if (have && !m_ladder.empty() && m_tu.ladder != m_ladder && inProgress)
        toast("Лестница частот демона изменилась с прошлого раза — лучше начать подбор заново", gfx::col::warn);

    m_buffersOk = m_t.initBuffers(&m_bufInfo);

    loadGpuSched();
    m_ucIni = findUltraCamIni();
    m_ucBench = readBenchmarkIndex(m_ucIni);
    m_haveLastRun = lastRun(&m_lastRun, &m_lastRunFile);
    if (m_ucIni.empty())
        LOGW("maxlastbreath.ini мода UltraCam не найден");
    else
        LOGI("UltraCam: %s, Benchmark = %d (%s)", m_ucIni.c_str(), m_ucBench, benchmarkName(m_ucBench));

    // Вернулись из TOTK: частоты прогона (ЦПУ/ГПУ) уже стоят у демона — не сбрасываем их первым же замером.
    if (m_daemon && (m_tu.gameActiveCand >= 0 || m_tu.gamePendingCand >= 0) && m_t.gameClocksActive())
        m_t.assumeClockMode(ConsoleTarget::ClockMode::Game);

    refreshContext(true);
    u32 cur = m_ctx.freqs[RclkModule_MEM];
    for (size_t i = 0; i < m_ladder.size(); i++)
        if (std::fabs((double)m_ladder[i] - cur) < 2e6) m_manFreq = (int)i;
    if (m_manFreq < 0 && !m_ladder.empty()) m_manFreq = (int)m_ladder.size() - 1;

    // Перезагрузка из безопасного режима, сделанная приложением: подбор продолжается так же, как после зависания.
    bool resumeFlag = fileExists(paths::appDir() + "resume.flag");
    if (resumeFlag && !m_safe) removeFile(paths::appDir() + "resume.flag");
    bool busyWork = m_tu.isAuto() || Tuner::researchActive();
    if (m_safe && busyWork) {
        // Пробы сейчас ничего не скажут: демон не ставит частоты. Предлагаем перезагрузку, без выбора — сами через 10 с.
        m_warned = true;
        m_screen = S_Auto;
        offerSafeReboot(10);
        return true;
    }
    // После зависания подбор продолжается сам: сбой уже записан, нажимать A не нужно. Через 5 с — чтобы успеть отменить (B).
    bool resume = (hang || resumeFlag) && !m_safe && cfg().autoResume && m_tu.isAuto() && m_tu.phase != Phase::Kip && m_daemon && m_buffersOk &&
                  !m_ladder.empty() && !Tuner::researchActive();
    if (resume) {
        m_autoResumeAt = nowMs() + 5000;
        m_warned = true;  // предупреждение уже принято в этом подборе
        m_screen = S_Auto;
        LOGI("Зависание записано — подбор продолжится сам через 5 с");
        toast("Прошлая проба зависла — записана как сбой. Подбор продолжится сам через 5 с (B — отмена)", gfx::col::warn);
    } else if (hang) {
        toast("Прошлая проба не завершилась (зависание/перезагрузка) — отмечена как сбой. A — продолжить подбор.", gfx::col::warn);
    } else if (m_safe) {
        offerSafeReboot(0);  // сразу после стартового предупреждения
    } else if (m_noWd) {
        toast("В " + (m_daemonVer.empty() ? std::string("Ryazha-clk") : m_daemonVer) +
                  " нет сторожа: зависшую консоль перезагрузите сами (POWER 12 с) и откройте приложение снова",
              gfx::col::warn);
    } else if (m_wdOff) {
        toast("Сторож Ryazha-clk выключен: зависшая консоль не перезагрузится сама. Включите его в оверлее Ryazha-clk", gfx::col::warn);
    }
    if (m_tu.gamePendingCand >= 0 || m_tu.gameActiveCand >= 0) collectGameOnStart();
    if (m_tu.phase == Phase::Game) m_screen = S_Totk;
    // Исследование потока: после зависания — отметить ступень и продолжить.
    if (Tuner::researchActive()) {
        std::string what;
        if (Tuner::researchRecoverHang(&what, m_t.bootEpoch())) toast("Исследование: зависание на " + what + " записано — продолжаю", gfx::col::warn);
        else toast("Исследование потока: продолжаю", gfx::col::info);
        startResearch(false);
        return true;
    }
    // Перенос в кип: приложение открыли после перезагрузки — разбираем загрузку и продолжаем без лишних нажатий.
    if (m_tu.phase == Phase::Kip && m_daemon && m_buffersOk && !hang) {
        toast("Перенос в кип: проверяю загрузку и продолжаю", gfx::col::info);
        startAuto(false);
    }
    return true;
}

void App::collectGameOnStart() {
    std::string msg;
    int r = m_tu.gameCollect(m_t, &msg);
    m_screen = S_Totk;
    if (r > 0) {
        toast(msg, gfx::col::good);
        int idx = m_tu.gamePendingCand >= 0 ? m_tu.gamePendingCand : m_tu.gameActiveCand;
        for (size_t i = 0; i < m_tu.cands.size(); i++)
            if ((int)i == idx || idx < 0) offerOptimize((int)i);
    } else if (r < 0) {
        // Сбой записан, кандидат можно сразу проверить снова (A) — ничего не блокируется. Или отжать (B).
        toast(msg, gfx::col::bad);
        for (size_t i = 0; i < m_tu.cands.size(); i++)
            if (m_tu.cands[i].game == 2 && !m_tu.cands[i].baseline) {
                offerStabilize((int)i, "нестабилен в TOTK");
                break;
            }
    } else {
        toast(msg + ". В игре пройдите бенчмарк в лесу короков и вернитесь сюда (или X — проверить ещё раз).", gfx::col::warn);
    }
    m_haveLastRun = lastRun(&m_lastRun, &m_lastRunFile);
}

// ============================================================== основной цикл

void App::run() {
#ifdef __SWITCH__
    while (appletMainLoop() && !m_quit) {
        padUpdate(&g_pad);
        u64 held = padGetButtons(&g_pad);
        u64 down = navKeys(padGetButtonsDown(&g_pad), held);

        // Проба упёрлась в безопасный режим сторожа — перезагрузка, чтобы вернуть разгон.
        if (m_tu.safeHalt && !busy()) {
            m_tu.safeHalt = false;
            m_safe = true;
            offerSafeReboot(10);
        }
        if (m_safeRebootAt && !busy() && nowMs() >= m_safeRebootAt) {
            m_safeRebootAt = 0;
            writeFile(paths::appDir() + "resume.flag", "1\n");
            m_tu.saveState(m_t);
            LOGI("Безопасный режим: перезагрузка через систему, чтобы вернуть разгон; после неё подбор продолжится");
            startJob("Перезагрузка", [this] {
                std::string err;
                if (!m_t.reboot(&err)) {
                    removeFile(paths::appDir() + "resume.flag");
                    toast("Не удалось перезагрузить: " + err + ". Перезагрузите через меню питания", gfx::col::bad);
                }
            });
        }
        // После зависания — продолжение подбора без нажатий.
        if (m_autoResumeAt && !busy() && nowMs() >= m_autoResumeAt) {
            m_autoResumeAt = 0;
            startAuto(false);
        }
        // Отжим снял ручку из кипа — запускаем этап переноса (запись и перезагрузка).
        if (m_kipResume && !busy()) {
            m_kipResume = false;
            startAuto(false);
        }
        // Поток закончил работу — освобождаем его.
        if (m_threadValid && !m_running.load()) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_threadValid = false;
        }

        if (m_t.measuring.load()) {
            // Идёт замер: экран не перерисовываем, чтобы не отнимать ядро и шину памяти.
            if (down & HidNpadButton_X) pauseAuto();
            stickClicks(down);
            sleepMs(40);
            continue;
        }
        input(down, held);
        refreshContext(false);
        draw();
        if (busy()) sleepMs(200);
    }
#endif
}

void App::shutdown() {
#ifdef __SWITCH__
    if (m_running.load()) {
        m_stopReq = true;
        m_t.abort = true;
    }
    if (m_threadValid) {
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_threadValid = false;
    }
    // Закрыли посреди подбора не через меню — вернём исходные настройки (кроме запуска игры).
    bool systemExit = m_exitRequested.load() && !m_quit;
    if (!m_launchGame && (m_restoreOnExit || (systemExit && m_tu.isAuto()))) {
        std::string err;
        m_t.applyOriginal(&err);
    }
    // Частоты прогона TOTK (ЦПУ/ГПУ) держим, пока ждём результат бенчмарка: игру запускают сразу после выхода
    // (сами или вручную из HOME). Раньше здесь они сбрасывались всегда — и бенчмарк шёл на частотах профиля.
    if (m_t.clockMode() == ConsoleTarget::ClockMode::Game && (m_launchGame || m_tu.gamePendingCand >= 0))
        LOGI("Выход: частоты прогона TOTK оставлены до возврата в приложение");
    else
        m_t.lockCpu(false);
    if (!m_launchGame) m_t.dlog.restore();
    m_tu.saveState(m_t);
    if (m_psm) psmExit();
    rclk::exit();
    gfx::exit();
    appletSetMediaPlaybackState(false);
    if (m_appMode) appletSetAutoSleepDisabled(false);
    appletUnhook(&m_hook);
    appletUnlockExit();
#endif
}

void App::onApplet(int hook) {
#ifdef __SWITCH__
    if (hook == AppletHookType_OnFocusState) {
        bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
        if (!inFocus) {
            m_focusLost = true;
            m_t.abort = true;  // замер вне фокуса недостоверен — повторим
        } else if (m_focusLost.load()) {
            m_focusLost = false;
            if (!m_userPause.load()) m_t.abort = false;
        }
    } else if (hook == AppletHookType_OnExitRequest) {
        m_exitRequested = true;
        m_stopReq = true;
        m_t.abort = true;
    }
#else
    (void)hook;
#endif
}

void App::workerMain() {
    if (m_jobAuto) {
        m_t.lockCpu(true);  // ЦПУ фиксирован на всё время синтетики
        while (!m_stopReq.load()) {
            bool more = m_tu.step(m_t, &m_stopReq);
            if (more) continue;
            // Проба прервана потерей фокуса — ждём возвращения и продолжаем.
            if (m_tu.isAuto() && !m_stopReq.load() && m_t.abort.load()) {
                while (m_t.abort.load() && !m_stopReq.load()) sleepMs(200);
                sleepMs(1500);  // после сна демону нужно поднять частоты обратно
                continue;
            }
            break;
        }
        m_t.lockCpu(false);
        if (m_tu.phase == Phase::Kip) {
            // Сюда попадаем, только если консоль не перезагрузилась сама.
            if (!m_stopReq.load()) toast(m_tu.status + ". A — попробовать снова.", gfx::col::warn);
        } else if (m_tu.phase == Phase::Game) {
            toast("Синтетика готова. Перейдите в «Проверка в TOTK».", gfx::col::good);
        } else if (m_tu.phase == Phase::Failed) {
            toast("Подбор остановлен: " + m_tu.error, gfx::col::bad);
        } else if (m_userPause.load()) {
            toast("Пауза. A — продолжить.", gfx::col::warn);
        }
    } else if (m_jobFn) {
        m_jobFn();
    }
    m_running = false;
}

bool App::startJob(const char* title, std::function<void()> fn) {
    if (busy()) {
        toast("Подождите — идёт другая операция", gfx::col::warn);
        return false;
    }
#ifdef __SWITCH__
    if (m_threadValid) {
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_threadValid = false;
    }
    m_jobAuto = !fn;
    m_jobFn = fn;
    m_jobTitle = title;
    m_stopReq = false;
    m_t.abort = false;
    m_running = true;
    if (R_FAILED(threadCreate(&m_thread, workerEntry, this, nullptr, 0x40000, 0x2C, -2))) {
        m_running = false;
        toast(strf("Не удалось создать рабочий поток для «%s»", title), gfx::col::bad);
        return false;
    }
    m_threadValid = true;
    threadStart(&m_thread);
    return true;
#else
    (void)title;
    if (fn) fn();
    return true;
#endif
}

void App::scheduleSafeReboot(int sec) {
    m_safeRebootAt = nowMs() + (u64)sec * 1000;
    LOGW("Ryazha-clk в безопасном режиме — перезагрузка через %d с (B — отмена)", sec);
    if (sec > 0)
        toast(strf("Ryazha-clk в безопасном режиме: перезагружаю консоль через %d с, подбор продолжится (B — отмена)", sec), gfx::col::warn);
    else
        toast("Перезагружаю консоль через систему — разгон вернётся", gfx::col::info);
}

void App::cancelSafeReboot() {
    if (m_safeRebootAt) LOGI("Перезагрузка из безопасного режима отменена");
    m_safeRebootAt = 0;
    toast("Без перезагрузки разгон не применится: подбор и прогоны TOTK не пойдут. Перезагрузить — «Сервис» или меню питания", gfx::col::warn);
}

void App::offerSafeReboot(int autoSec) {
    std::string text =
        "Консоль загрузилась после зависания, и сторож Ryazha-clk держит её в безопасном режиме: частоты РАМ, ЦПУ и ГПУ не разгоняются, "
        "тайминги и кип не применяются. Подбор, исследование потока и прогоны TOTK в этом режиме ничего не покажут.\n\n"
        "Разгон вернётся после обычной перезагрузки через систему.";
    if (autoSec > 0) {
        text += strf("\n\nИдёт подбор: если ничего не выбрать, через %d с перезагружу сам — после загрузки он продолжится.", autoSec);
        scheduleSafeReboot(autoSec);
    }
    Dialog d;
    d.title = "Безопасный режим Ryazha-clk";
    d.text = text;
    d.yes = [this] { scheduleSafeReboot(0); };
    d.yesLabel = "Перезагрузить";
    d.alt = [this] { cancelSafeReboot(); };
    d.altLabel = "Остаться без разгона";
    d.no = [this] { cancelSafeReboot(); };
    d.danger = true;
    openDialog(d);
}

void App::startAuto(bool fresh) {
    if (m_safe) {
        offerSafeReboot(0);
        return;
    }
    if (!m_daemon) {
        toast("Ryazha-clk не найден: нужен запущенный демон (служба ryz:clk)", gfx::col::bad);
        return;
    }
    if (!m_buffersOk) {
        toast("Не хватило памяти под замеры. Запустите через игру (удерживая R), а не через альбом.", gfx::col::bad);
        return;
    }
    if (m_ladder.empty()) {
        toast("Демон не отдал список частот памяти", gfx::col::bad);
        return;
    }
    if (fresh && m_api != 2 && !m_apiConfirmed) {
        confirm("Неизвестная версия демона",
                strf("Служба %s отвечает API %u, а приложение сверено с Ryazha-clk 3.5.5 и 3.6.0 (API 2). Номера команд могли измениться. "
                     "Продолжать только если вы уверены.", m_svc.c_str(), m_api),
                [this] {
                    m_apiConfirmed = true;
                    startAuto(true);
                },
                "Продолжить");
        return;
    }
    if (fresh) {
        m_tu.gameDeactivate(m_t);
        m_t.prepareOriginal(true);
        if (!m_chip.mariko && cfg().maxMhz == 0) {
            cfg().maxMhz = 2100;  // Erista: выше ~2000 МГц Ryazha-clk пока виснет (ERISTA_LIVE_TUNING_STATUS)
            cfg().save();
            toast("Erista: верх прогона ограничен 2100 МГц (см. статус Erista в Ryazha-clk)", gfx::col::warn);
        }
        m_tu.begin(m_ladder, m_chip);
        m_tu.singleHz = m_singleReq;
        if (m_singleReq) LOGI("Подбор одной частоты: %s МГц", mhzStr(m_singleReq).c_str());
        m_singleReq = 0;
        m_tu.saveState(m_t);
    }
    m_autoResumeAt = 0;
    if (cfg().useDaemonLog) m_t.dlog.enable();
    m_userPause = false;
    LOGI("Автоподбор: %s, этап «%s»", fresh ? "новый" : "продолжение", phaseName(m_tu.phase));
    startJob("Автоподбор", nullptr);
}

void App::startResearch(bool fresh) {
    if (m_safe) {
        offerSafeReboot(0);
        return;
    }
    if (!m_daemon || !m_buffersOk || m_ladder.empty()) {
        toast("Исследование: нужен демон Ryazha-clk, список частот и память под замеры (запуск через игру)", gfx::col::bad);
        return;
    }
    if (fresh) Tuner::researchStart(cfg().researchRuns);
    m_screen = S_Auto;
    startJob("Исследование потока", [this] {
        m_t.lockCpu(true);
        std::string msg;
        bool ok = m_tu.research(m_t, m_ladder, m_chip, &m_stopReq, &msg);
        m_t.lockCpu(false);
        toast(msg, ok ? gfx::col::good : gfx::col::warn);
    });
}

void App::startSingle(u32 hz) {
    // Как полный подбор, но после базового замера — сразу доводка этой частоты (частоту берёт startAuto до старта потока).
    m_singleReq = hz;
    startAuto(true);
}

void App::pauseAuto() {
    if (!busy()) return;
    m_userPause = true;
    m_stopReq = true;
    m_t.abort = true;
    toast("Пауза: текущая проба прервана и будет повторена при продолжении", gfx::col::warn);
}

void App::refreshContext(bool force) {
    u64 now = nowMs();
    if (!force && now - m_ctxAt < 1000) return;
    m_ctxAt = now;
    if (m_daemon) {
        RclkContext c;
        if (rclk::context(&c)) m_ctx = c;
    }
#ifdef __SWITCH__
    // Раз в секунду и только при перерисовке: во время замера экран не рисуется, замер это не трогает.
    u32 pct = 0;
    if (m_psm && R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) m_battPct = (int)pct;
#endif
}

void App::toast(const std::string& s, u32 color) {
    if (color == gfx::col::bad)
        LOGE("Сообщение: %s", s.c_str());
    else if (color == gfx::col::warn)
        LOGW("Сообщение: %s", s.c_str());
    else
        LOGI("Сообщение: %s", s.c_str());
    std::lock_guard<std::mutex> lk(m_uiMtx);
    m_toast = s;
    m_toastColor = color ? color : gfx::col::info;
    m_toastUntil = nowMs() + 6000;
}

void App::openDialog(Dialog d) {
    std::lock_guard<std::recursive_mutex> lk(m_modalMtx);
    d.open = true;
    d.seq = ++m_modalSeq;
    m_dlg = std::move(d);
    m_dlgScroll = 0;
}

void App::confirm(const std::string& title, const std::string& text, std::function<void()> yes, const char* yesLabel, std::function<void()> alt,
                  const char* altLabel) {
    Dialog d;
    d.title = title;
    d.text = text;
    d.yes = yes;
    d.alt = alt;
    d.yesLabel = yesLabel ? yesLabel : "Да";
    d.altLabel = altLabel ? altLabel : "";
    openDialog(d);
}

void App::confirmDanger(const std::string& title, const std::string& text, std::function<void()> yes, const char* yesLabel) {
    Dialog d;
    d.title = title;
    d.text = text;
    d.yes = yes;
    d.yesLabel = yesLabel ? yesLabel : "Да";
    d.danger = true;
    openDialog(d);
}

void App::pick(const std::string& title, const std::vector<std::string>& opts, std::function<void(int)> fn, int sel) {
    std::lock_guard<std::recursive_mutex> lk(m_modalMtx);
    m_pick.title = title;
    m_pick.opts = opts;
    m_pick.sel = std::max(0, std::min(sel, (int)opts.size() - 1));
    m_pick.fn = fn;
    m_pick.seq = ++m_modalSeq;
    m_pick.open = true;
}

int App::topModal() {
    std::lock_guard<std::recursive_mutex> lk(m_modalMtx);
    if (m_dlg.open && (!m_pick.open || m_dlg.seq > m_pick.seq)) return 2;
    return m_pick.open ? 1 : 0;
}

std::string App::warnedFile() { return paths::appDir() + "warned.txt"; }

void App::acceptWarning() {
    m_warned = true;
    writeFile(warnedFile(), strf("%lld\n", (long long)m_t.bootEpoch()));
}

void App::requestExit(bool restore) {
    m_restoreOnExit = restore;
    m_quit = true;
}

void App::restoreEverything() {
    m_tu.gameDeactivate(m_t);
    startJob("Возврат исходных настроек", [this] {
        std::string err;
        if (m_t.applyOriginal(&err))
            toast("Исходный timings.ini возвращён, временная частота снята", gfx::col::good);
        else
            toast("Не получилось: " + err, gfx::col::bad);
        m_t.dlog.restore();
        // Общие тайминги кипа до «кип сначала».
        std::string cb;
        if (readFile(paths::kipCommonBackup(), cb) && !trim(cb).empty()) {
            KnobSet orig;
            knobSetFromString(trim(cb), orig);
            if (m_t.kipCommonWrite(orig, &err)) {
                removeFile(paths::kipCommonBackup());
                m_kipNeedsReboot = true;
                toast("Общие тайминги кипа возвращены — подействует после перезагрузки (Сервис → Перезагрузить)", gfx::col::warn);
            } else {
                toast("Общие тайминги кипа вернуть не удалось: " + err, gfx::col::bad);
            }
        }
        // Ручки, перенесённые в кип: вернуть значения до переноса. Действуют они только после перезагрузки.
        std::string backup;
        if (readFile(paths::kipBackup(), backup) && !trim(backup).empty()) {
            std::map<int, int> orig;
            for (auto& l : split(backup, '\n')) {
                auto q = split(trim(l), ':');
                if (q.size() == 2) orig[(int)toLong(q[0], -1)] = (int)toLong(q[1], 0);
            }
            std::map<int, int> now;
            bool differs = !m_t.kipRead(&now, &err) || now != orig;
            if (!differs) {
                removeFile(paths::kipBackup());
            } else if (m_t.kipWrite(orig, &err)) {
                removeFile(paths::kipBackup());
                m_kipNeedsReboot = true;
                toast("Кип возвращён к значениям до переноса — подействует после перезагрузки (Сервис → Перезагрузить)", gfx::col::warn);
            } else {
                toast("Кип вернуть не удалось: " + err + " (копия: " + paths::kipBackup() + ")", gfx::col::bad);
            }
        }
    });
}

void App::launchCandidate(int idx) {
    startJob("Применение кандидата", [this, idx] {
        std::string err;
        if (!m_tu.gameApply(m_t, idx, &err)) {
            toast("Не применилось: " + err, gfx::col::bad);
            return;
        }
        if (!cfg().autoLaunch) {
            toast("Применено. Запустите TOTK, пройдите бенчмарк и вернитесь сюда.", gfx::col::good);
            return;
        }
        m_launchGame = true;
        if (launchTotk(&err)) {
            m_quit = true;  // уступаем место игре; результат заберём при следующем запуске
        } else {
            m_launchGame = false;
            toast("Применено, но " + err, gfx::col::warn);
        }
    });
}

void App::offerOptimize(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size()) return;
    Candidate& c = m_tu.cands[idx];
    if (c.baseline || c.optOffered || c.issues() > 0 || m_tu.gameRunsDone(c) < cfg().totkRuns) return;
    c.optOffered = true;
    m_tu.saveState(m_t);
    // После прогонов — на выбор: оптимизировать сейчас или сразу записать итог. Отказ ничего не закрывает:
    // и то, и другое потом — в «Действиях» (Y) на этом экране.
    confirm("Прогоны TOTK закончены",
            c.name() + " прошёл " + strf("%d", cfg().totkRuns) + " прогона TOTK.\n\nA — оптимизировать тайминги: перебрать найденное и до трёх "
                       "ступеней мягче и собрать новый кандидат с лучшей оценкой (его тоже прогоним в TOTK).\nY — записать этого кандидата "
                       "итогом.\n\nПотом — в любой момент: «Действия» (Y) на экране «Проверка в TOTK».",
            [this, idx] { runOptimize(idx); }, "Оптимизировать", [this, idx] { writeFinal(idx); }, "Записать итог");
}

void App::runOptimize(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size() || m_tu.cands[idx].baseline) return;
    m_tu.cands[idx].optOffered = true;
    m_tu.gameDeactivate(m_t);
    startJob("Оптимизация таймингов", [this, idx] {
        m_t.lockCpu(true);
        std::string msg;
        bool ok = m_tu.optimizeCandidate(m_t, idx, &msg);
        m_t.lockCpu(false);
        toast(msg, ok ? gfx::col::good : gfx::col::warn);
    });
}

void App::writeFinal(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size()) return;
    const Candidate& c = m_tu.cands[idx];
    if (c.baseline) {
        confirm("Вернуть как было", "Вернуть исходный timings.ini и частоту профиля?", [this] { restoreEverything(); }, "Вернуть");
        return;
    }
    std::string text = strf("Записать «%s» итогом: в timings.ini секция [%s] (%s), РАМ %s МГц — сразу, до перезагрузки.", c.name().c_str(),
                            mhzStr(c.hz).c_str(), knobSetCompact(c.set).c_str(), mhzStr(c.hz).c_str());
    if (c.game != 1)
        text += "\n\nВнимание: этот кандидат ещё не проверен в TOTK.";
    else if (c.issues())
        text += "\n\nВнимание: у этого кандидата были замечания в игре.";
    text += "\n\nA — ещё и частоту в профиль TOTK у Ryazha-clk (держится после перезагрузки). Y — только timings.ini.";
    auto go = [this, idx](bool profile) {
        startJob("Запись итога", [this, idx, profile] {
            std::string err;
            if (!m_tu.finalize(m_t, idx, &err)) {
                toast("Не получилось: " + err, gfx::col::bad);
                return;
            }
            m_t.dlog.restore();
            if (profile)
                saveProfileForTotk(idx);
            else
                toast("Итог записан в timings.ini. Частоту закрепите в профиле: «Результаты» → X", gfx::col::good);
        });
    };
    confirm("Записать итог", text, [go] { go(true); }, "Записать и в профиль", [go] { go(false); }, "Только timings.ini");
}

void App::archiveSaveCand(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size() || m_tu.cands[idx].baseline) return;
    const Candidate& c = m_tu.cands[idx];
    ArchEntry e;
    e.hz = c.hz;
    e.set = c.set;
    e.dram = m_chip.dramId;
    if (c.b.valid) {
        e.bw = c.b.bw();
        e.lat = c.b.latNs;
    }
    if (!c.runs.empty()) Tuner::runsMedian(c.runs, &e.frames, nullptr, nullptr, nullptr);
    e.name = strf("К%d", c.id) + (c.runs.empty() ? "" : strf(", TOTK %zu пр.", c.runs.size())) +
             (c.doubtful() ? strf(", сомнительный: сбоев %d из %d", c.crashes, std::max(c.attempts, c.crashes)) : (c.issues() ? ", были замечания" : ""));
    if (archAdd(e)) {
        m_archAt = 0;  // сравнение на экране TOTK — с новой записью
        LOGI("Архив: сохранён %s — %s", archLabel(e).c_str(), knobSetCompact(e.set).c_str());
        toast("В архиве: " + archLabel(e) + ". Применить — «Сервис» → «Архив конфигураций»", gfx::col::good);
    } else {
        toast("Не удалось записать " + archivePath(), gfx::col::bad);
    }
}

void App::archiveSaveManual() {
    if (m_ladder.empty() || m_manFreq < 0 || m_manFreq >= (int)m_ladder.size()) return;
    ArchEntry e;
    e.hz = m_ladder[m_manFreq];
    e.set = m_manSet;
    e.dram = m_chip.dramId;
    e.name = "ручная настройка";
    if (archAdd(e)) {
        m_archAt = 0;  // сравнение на экране TOTK — с новой записью
        LOGI("Архив: сохранена ручная настройка %s — %s", mhzStr(e.hz).c_str(), knobSetCompact(e.set).c_str());
        toast("В архиве: " + archLabel(e), gfx::col::good);
    } else {
        toast("Не удалось записать " + archivePath(), gfx::col::bad);
    }
}

void App::archiveMenu() {
    auto all = archLoad();
    std::vector<std::string> names;
    std::string man = m_ladder.empty() || m_manFreq < 0 ? std::string("—")
                                                      : mhzStr(m_ladder[m_manFreq]) + " МГц, " + (m_manSet.empty() ? std::string("общие") : knobSetCompact(m_manSet));
    names.push_back("+ Сохранить ручную настройку (" + man + ")");
    int sweet = archSweet(all, m_chip.dramId, cfg().wGame / 100.0);
    for (size_t i = 0; i < all.size(); i++) {
        const ArchEntry& e = all[i];
        names.push_back(((int)i == sweet ? SWEET_MARK " · " : "") + archLabel(e) + (e.dram >= 0 && e.dram != m_chip.dramId ? " · другой чип" : ""));
    }
    pick(all.empty() ? std::string("Архив конфигураций пуст — сохраните кандидата (TOTK → Y) или ручную настройку")
                     : strf("Архив конфигураций: %zu", all.size()),
         names, [this](int k) {
             if (k == 0)
                 archiveSaveManual();
             else
                 archiveEntryActions((size_t)(k - 1));
         });
}

void App::archiveEntryActions(size_t i) {
    auto all = archLoad();
    if (i >= all.size()) return;
    ArchEntry e = all[i];
    pick(archLabel(e),
         {"Применить сейчас: РАМ и тайминги до перезагрузки, ЦПУ/ГПУ — как в профиле", "Записать итогом: timings.ini и частота (можно и в профиль TOTK)",
          "Подставить в ручную настройку", "Удалить из архива"},
         [this, i, e](int k) {
             if ((k == 0 || k == 1) && m_safe) {
                 offerSafeReboot(0);
                 return;
             }
             if (k == 0) {
                 std::string text = strf("%s МГц: %s\n\nДо перезагрузки; ЦПУ и ГПУ — как в профиле. Навсегда — «Записать итогом».",
                                         mhzStr(e.hz).c_str(), e.set.empty() ? "общие тайминги кипа" : knobSetCompact(e.set).c_str());
                 confirm("Применить из архива", text, [this, e] {
                     m_tu.gameDeactivate(m_t);
                     startJob("Применение из архива", [this, e] {
                         std::string err;
                         ApplyInfo ai;
                         m_t.freeClocks();
                         if (m_t.apply(e.hz, e.set, &ai, &err)) {
                             LOGI("Архив: применено %s — %s", mhzStr(e.hz).c_str(), knobSetCompact(e.set).c_str());
                             toast("Применено из архива: " + mhzStr(e.hz) + " МГц (до перезагрузки). Навсегда — «Записать итогом»", gfx::col::good);
                         } else {
                             toast("Не применилось: " + err, gfx::col::bad);
                         }
                     });
                 }, "Применить");
             } else if (k == 1) {
                 std::string text = strf("Записать в timings.ini секцию [%s] (%s) и поставить %s МГц сейчас?\n\nA — ещё и частоту в профиль TOTK у "
                                         "Ryazha-clk (держится после перезагрузки). Y — только timings.ini.",
                                         mhzStr(e.hz).c_str(), knobSetCompact(e.set).c_str(), mhzStr(e.hz).c_str());
                 if (e.dram >= 0 && e.dram != m_chip.dramId) text += "\n\nВнимание: набор сохранён на другом чипе памяти.";
                 auto go = [this, e](bool profile) {
                     startJob("Запись из архива", [this, e, profile] {
                         std::string err;
                         m_tu.gameDeactivate(m_t);
                         if (!m_t.commit(e.hz, e.set, &err)) {
                             toast("Не получилось: " + err, gfx::col::bad);
                             return;
                         }
                         m_t.dlog.restore();
                         LOGI("Архив: записано итогом %s — %s", mhzStr(e.hz).c_str(), knobSetCompact(e.set).c_str());
                         if (profile)
                             saveProfileHz(e.hz);
                         else
                             toast("Записано в timings.ini: " + mhzStr(e.hz) + " МГц", gfx::col::good);
                     });
                 };
                 confirm("Записать из архива", text, [go] { go(true); }, "Записать и в профиль", [go] { go(false); }, "Только timings.ini");
             } else if (k == 2) {
                 m_manSet = e.set;
                 for (size_t j = 0; j < m_ladder.size(); j++)
                     if (m_ladder[j] == e.hz) m_manFreq = (int)j;
                 m_screen = S_Manual;
                 std::lock_guard<std::mutex> lk(m_uiMtx);
                 m_manNote = "Подставлено из архива: " + mhzStr(e.hz) + " МГц. A — применить";
             } else {
                 confirm("Удалить из архива", "Удалить «" + archLabel(e) + "»?", [this, i] {
                     if (archRemove(i)) {
                         m_archAt = 0;
                         toast("Удалено из архива", gfx::col::good);
                     }
                 });
             }
         });
}

void App::totkActions(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size()) return;
    const Candidate& c = m_tu.cands[idx];
    std::vector<std::string> names;
    std::vector<int> acts;
    if (c.baseline) {
        names.push_back("Вернуть как было (исходный timings.ini и частота профиля)");
        acts.push_back(0);
    } else {
        names.push_back("Записать итогом (timings.ini, частота; можно и в профиль TOTK)");
        acts.push_back(0);
        names.push_back(strf("Оптимизировать тайминги %s МГц (новый кандидат)", mhzStr(c.hz).c_str()));
        acts.push_back(1);
        names.push_back("Сохранить в архив конфигураций (применять потом в любой момент)");
        acts.push_back(3);
    }
    names.push_back("Отметить, что было в игре: вылет, артефакты, загрузки, статтеры");
    acts.push_back(2);
    pick("«" + c.name() + "»: что сделать?", names, [this, idx, acts](int k) {
        int a = acts[k];
        if (a == 3) {
            archiveSaveCand(idx);
        } else if (a == 0) {
            writeFinal(idx);
        } else if (a == 1) {
            const Candidate& cc = m_tu.cands[idx];
            std::string text = strf("Перебрать тайминги «%s» (найденное и до трёх ступеней мягче) и собрать новый кандидат с лучшей оценкой?",
                                    cc.name().c_str());
            if (cc.issues()) text += "\n\nВнимание: у этого кандидата были замечания в игре — лучше сначала отжать (B).";
            confirm("Оптимизировать тайминги", text, [this, idx] { runOptimize(idx); }, "Оптимизировать");
        } else {
            pick("Что было в игре с «" + m_tu.cands[idx].name() + "»?",
                 {"Вылет / зависание игры", "Артефакты (битые текстуры, мерцание)", "Долгие загрузки", "Статтеры (рывки, подвисания)"},
                 [this, idx](int kk) {
                     static const char* kinds[] = {"", "артефакты", "долгие загрузки", "статтеры"};
                     if (kk == 0)
                         m_tu.gameMarkFailed(m_t, idx, "отмечено вручную: вылет / зависание в игре");
                     else
                         m_tu.gameFlag(m_t, idx, kinds[kk]);
                     toast("Записано в реестр нестабильных значений", gfx::col::warn);
                     // Безопасный режим: любое замечание — сначала отжать.
                     if (cfg().ramMode == Settings::ModeSafe || (cfg().ramMode == Settings::ModeMedium && kk == 0))
                         offerStabilize(idx, "замечание в игре");
                 });
        }
    });
}

void App::offerStabilize(int idx, const char* why) {
    if (idx < 0 || idx >= (int)m_tu.cands.size() || m_tu.cands[idx].baseline) return;
    Tuner::LoosenPlan p = m_tu.nextLoosen(m_tu.cands[idx]);
    if (p.tier == 0) {
        toast(m_tu.cands[idx].name() + ": отжимать больше нечего", gfx::col::warn);
        return;
    }
    std::string text = m_tu.cands[idx].name() + " — " + why + ".\n\nСначала отжимаются тайминги, потом ручки пропускной способности, потом "
                       "ручки в кипе (с перезагрузкой), последней — частота. Следующий шаг: " + p.desc + " (" + Tuner::loosenTierName(p.tier) +
                       "). После каждого шага — memtest " + strf("%d", cfg().verifySec) + " с; не прошёл — следующий шаг.";
    confirm("Отжать кандидата", text, [this, idx] { stabilizeCand(idx); }, "Отжать");
}

void App::stabilizeCand(int idx) {
    m_tu.gameDeactivate(m_t);
    startJob("Отжим кандидата", [this, idx] {
        m_t.lockCpu(true);
        std::string msg;
        size_t before = m_tu.cands.size();
        bool ok = m_tu.stabilize(m_t, idx, &msg);
        m_t.lockCpu(false);
        if (m_tu.cands.size() > before) m_sel[S_Totk] = (int)m_tu.cands.size() - 1;  // отжата копия — курсор на неё
        toast(msg, ok ? gfx::col::good : gfx::col::bad);
        if (ok && m_tu.phase == Phase::Kip) m_kipResume = true;  // ручку снимаем из кипа — дальше перезагрузка
    });
}

void App::saveProfileForTotk(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size()) return;
    saveProfileHz(m_tu.cands[idx].hz);
}

void App::saveProfileHz(u32 hz) {
    u32 mhz[25];
    if (!rclk::getProfiles(TOTK_TITLE_ID, mhz)) {
        toast("Не удалось прочитать профиль TOTK у демона", gfx::col::bad);
        return;
    }
    // Профили хранят целые МГц; округляем вверх, демон сам встанет на ближайшую ступень.
    u32 m = (hz + 999999u) / 1000000u;
    for (int p = 0; p < 5; p++) mhz[p * 5 + RclkModule_MEM] = m;
    if (!rclk::setProfiles(TOTK_TITLE_ID, mhz)) {
        toast("Демон не принял профиль", gfx::col::bad);
        return;
    }
    u32 back[25];
    if (rclk::getProfiles(TOTK_TITLE_ID, back) && back[4 * 5 + RclkModule_MEM] == m)
        toast(strf("Профиль TOTK: РАМ %u МГц во всех режимах питания", m), gfx::col::good);
    else
        toast("Профиль записан, но перечитать его не удалось — проверьте в оверлее Ryazha-clk", gfx::col::warn);
}

// ============================================================== ввод

#ifdef __SWITCH__
#define BTN(x) HidNpadButton_##x
#else
#define BTN(x) 0
#endif

// Стики работают как крестовина. Вверх/вниз, зажатые крестовиной или стиком, повторяются: через 400 мс, потом
// каждые 70 мс — длинные списки листаются удержанием. Влево/вправо не повторяются: в настройках они переключают флажки.
u64 App::navKeys(u64 down, u64& held) {
    static const u64 kDirs[4][2] = {{BTN(AnyUp), BTN(Up)}, {BTN(AnyDown), BTN(Down)}, {BTN(AnyLeft), BTN(Left)}, {BTN(AnyRight), BTN(Right)}};
    for (auto& d : kDirs) {
        if (down & d[0]) down |= d[1];
        if (held & d[0]) held |= d[1];
    }
    m_navRep = false;
    const u64 vert = BTN(Up) | BTN(Down);
    u64 hv = held & vert, now = nowMs();
    if (down & vert) {
        m_navRepAt = now + 400;
    } else if (!hv || hv == vert) {
        m_navRepAt = 0;  // отпущено или зажаты обе стороны
    } else if (m_navRepAt && now >= m_navRepAt) {
        down |= hv;
        m_navRepAt = now + 70;
        m_navRep = true;
    }
    return down;
}

// Нажатия левого стика подряд. Считаются и во время замера, когда экран не рисуется и ввод не разбирается: иначе они
// терялись, и всё нужное открывалось только между замерами.
void App::stickClicks(u64 down) {
    if (!(down & BTN(StickL))) return;
    u64 now = nowMs();
    if (now - m_slAt > 800) m_slCount = 0;
    m_slCount++;
    m_slAt = now;
}

// Шаг по списку по кругу; при автоповторе — до края, чтобы удержание не перескакивало с конца в начало.
int App::wrapStep(int sel, int n, int d) const {
    if (n <= 0) return 0;
    int s = sel + d;
    if (s < 0) return m_navRep ? 0 : n - 1;
    if (s >= n) return m_navRep ? n - 1 : 0;
    return s;
}

// Кнопки верхнему окну и только ему: экран под окнами в этот кадр не получает ничего. Обработчик вызывается копией и
// без блокировки: внутри он может открыть следующее окно (архив: выбор внутри выбора; планировщик ГПУ: «выключить?» →
// «перезагрузить?»). Закрылось верхнее, а под ним было ещё одно — кнопки ему со следующего кадра.
bool App::inputModal(u64 down) {
    std::function<void()> call;
    {
        std::lock_guard<std::recursive_mutex> lk(m_modalMtx);
        int top = topModal();
        if (!top) return false;
        if (top == 1) {
            int n = (int)m_pick.opts.size();
            if (down & BTN(Up)) m_pick.sel = wrapStep(m_pick.sel, n, -1);
            if (down & BTN(Down)) m_pick.sel = wrapStep(m_pick.sel, n, 1);
            if (down & BTN(A)) {
                m_pick.open = false;
                auto fn = m_pick.fn;
                int sel = m_pick.sel;
                if (fn) call = [fn, sel] { fn(sel); };
            } else if (down & BTN(B)) {
                m_pick.open = false;
            }
        } else {
            if (down & BTN(Up)) m_dlgScroll = std::max(0, m_dlgScroll - 1);
            if (down & BTN(Down)) m_dlgScroll++;  // верхний предел зажмёт textScroll
            if (down & BTN(A)) {
                m_dlg.open = false;
                call = m_dlg.yes;
            } else if ((down & BTN(Y)) && m_dlg.alt) {
                m_dlg.open = false;
                call = m_dlg.alt;
            } else if (down & BTN(B)) {
                m_dlg.open = false;
                call = m_dlg.no;
            }
        }
    }
    if (call) call();
    return true;
}

void App::input(u64 down, u64 held) {
    (void)held;
    // Открытое окно важнее отложенных действий: B в окне закрывает окно, а не отменяет перезагрузку или продолжение.
    bool modal = topModal() != 0;
    if (m_safeRebootAt && !modal && (down & BTN(B))) {
        cancelSafeReboot();
        return;
    }
    if (m_autoResumeAt && !modal && (down & BTN(B))) {
        m_autoResumeAt = 0;
        LOGI("Автопродолжение после зависания отменено");
        toast("Автопродолжение отменено. A — продолжить подбор", gfx::col::info);
        return;
    }
    // Окна, открытые при входе (итог прогонов TOTK, безопасный режим…), ждут, пока предупреждение не принято:
    // иначе A уходило окну поверх него.
    if (!m_warned) {
        if (down & BTN(A)) acceptWarning();
        if (down & BTN(Plus)) requestExit(false);
        return;
    }
    if (m_hmOpen) {
        if (down & BTN(Up)) m_hmSel = wrapStep(m_hmSel, 4, -1);
        if (down & BTN(Down)) m_hmSel = wrapStep(m_hmSel, 4, 1);
        if (down & BTN(A)) {
            cfg().hidden ^= 1 << m_hmSel;
            cfg().save();
        }
        if (down & BTN(B)) m_hmOpen = false;
        return;
    }
    // Левый стик нажать (не сдвинуть) столько раз подряд, сколько частот доводится таймингами, и подождать.
    if (!m_dlg.open && !m_pick.open) {
        stickClicks(down);
        if (!(down & BTN(StickL)) && m_slCount && nowMs() - m_slAt > 800) {
            if (m_slCount == std::max(1, cfg().numTargets)) {
                m_hmOpen = true;
                m_hmSel = 0;
            }
            m_slCount = 0;
        }
    }
    if (inputModal(down)) return;
    if (down & BTN(Plus)) {
        if (busy()) {
            toast("Сначала пауза (X) и дождитесь конца пробы", gfx::col::warn);
            return;
        }
        if (m_tu.gamePendingCand >= 0 || m_tu.gameActiveCand >= 0) {
            confirm("Выход",
                    m_t.clockMode() == ConsoleTarget::ClockMode::Game ? "Кандидат применён для прогона TOTK (РАМ + ЦПУ/ГПУ). Выйти, оставив его настройки?"
                                                                       : "Кандидат применён (РАМ и тайминги). Выйти, оставив его настройки?",
                    [this] { requestExit(false); },
                    "Выйти", [this] { requestExit(true); }, "Выйти и вернуть как было");
        } else {
            confirm("Выход", "Вернуть исходный timings.ini и частоту профиля?", [this] { requestExit(true); }, "Вернуть и выйти",
                    [this] { requestExit(false); }, "Выйти, оставив как есть");
        }
        return;
    }
    if (down & BTN(L)) m_screen = (m_screen + S_Count - 1) % S_Count;
    if (down & BTN(R)) m_screen = (m_screen + 1) % S_Count;
    if (down & (BTN(L) | BTN(R))) return;
    switch (m_screen) {
        case S_Auto: inputAuto(down); break;
        case S_Totk: inputTotk(down); break;
        case S_Manual: inputManual(down); break;
        case S_Results: inputResults(down); break;
        case S_Settings: inputSettings(down); break;
        case S_Service: inputService(down); break;
    }
}

void App::inputAuto(u64 down) {
    if (down & BTN(X)) {
        pauseAuto();
        return;
    }
    if (busy()) return;
    if (down & BTN(A)) {
        Phase p = m_tu.phase;
        if (m_tu.isAuto()) {
            startAuto(false);
        } else if (p == Phase::Game) {
            m_screen = S_Totk;
        } else {
            confirm("Новый подбор",
                    std::string(m_noWd ? "В этой версии Ryazha-clk сторожа нет. Зависшая консоль сама не перезагрузится — держите POWER 12 с "
                                         "и включите её снова. «Кип сначала» и перенос в кип будут пропущены: без сторожа зависание "
                                         "на загрузке кип не откатит.\n\n"
                                : m_wdOff ? "ВНИМАНИЕ: сторож Ryazha-clk выключен. Зависшая консоль сама не перезагрузится — придётся держать "
                                            "POWER 12 с, а «Кип сначала» будет пропущен. Включите сторож в оверлее Ryazha-clk "
                                            "(раздел «Безопасность и общие»).\n\n"
                                          : "") +
                        "Приложение будет менять частоту памяти и тайминги на лету через Ryazha-clk, гонять memtest и замеры. " +
                        (m_wdOff ? "Если консоль зависнет, перезагрузите её и просто откройте RamSweetSpot снова: "
                                 : "Если консоль зависнет, сторож Ryazha-clk перезапустит её — после этого просто откройте RamSweetSpot снова: ") +
                        "сбой запишется, и подбор продолжится сам через 5 с.\n\nA — полный подбор: прогоны всех частот и доводка лучших. "
                    "Y — одна частота: выберите её, и приложение подберёт для неё тайминги и ручки (без прогонов частот).",
                    [this] {
                        m_singleReq = 0;
                        startAuto(true);
                    },
                    "Полный подбор",
                    [this] {
                        // Одна частота: список ступеней лестницы выше стока, курсор — на частоте ручной настройки.
                        std::vector<std::string> names;
                        auto hzs = std::make_shared<std::vector<u32>>();
                        int sel = 0;
                        for (u32 hz : m_ladder) {
                            if (hz <= 1600000000u) continue;
                            if (m_manFreq >= 0 && m_manFreq < (int)m_ladder.size() && m_ladder[m_manFreq] == hz) sel = (int)hzs->size();
                            std::string extra;
                            {
                                std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
                                for (auto& p : m_tu.points)
                                    if (p.hz == hz && p.status == 1) extra = strf(" · прогон: %.0f МБ/с", p.b.bw());
                            }
                            if (m_tu.freqCap && hz > m_tu.freqCap) extra += " · выше потолка: прогон зависал";
                            if (m_tu.hzDiscarded(hz)) extra += " · отброшена: на кипе не держится";
                            names.push_back(mhzStr(hz) + " МГц" + extra);
                            hzs->push_back(hz);
                        }
                        if (hzs->empty()) {
                            toast("Демон не отдал разгонных частот", gfx::col::bad);
                            return;
                        }
                        pick("Какую частоту подобрать?", names, [this, hzs](int i) { startSingle((*hzs)[i]); }, sel);
                    },
                    "Одна частота");
        }
    }
    if (down & BTN(Y)) {
        confirm("Сбросить прогресс", "Журнал подбора будет удалён, исходный timings.ini и частота профиля — возвращены.", [this] {
            m_tu.resetAll();
            removeFile(paths::state());
            restoreEverything();
        });
    }
    if (down & BTN(Up)) m_sel[S_Auto] = std::max(0, m_sel[S_Auto] - 1);
    if (down & BTN(Down))
        m_sel[S_Auto] = m_tu.phase == Phase::Idle ? m_sel[S_Auto] + 1  // прокрутка «Что будет сделано»: предел зажмёт textScroll
                                                  : std::min((int)m_tu.events.size(), m_sel[S_Auto] + 1);
}

void App::inputTotk(u64 down) {
    if (busy()) return;
    // В безопасном режиме кандидата не применить — сразу предлагаем перезагрузку.
    if (m_safe && (down & (BTN(A) | BTN(ZL)))) {
        offerSafeReboot(0);
        return;
    }
    int n = (int)m_tu.cands.size();
    int& sel = m_sel[S_Totk];
    if (n > 0) sel = std::min(sel, n - 1);
    if (down & BTN(Up)) sel = std::max(0, sel - 1);
    if (down & BTN(Down)) sel = std::min(std::max(0, n - 1), sel + 1);
    if ((down & BTN(ZR)) && !m_ucIni.empty() && m_ucBench != 4) {
        if (writeBenchmarkIndex(m_ucIni, 4)) {
            m_ucBench = readBenchmarkIndex(m_ucIni);
            toast("maxlastbreath.ini: Benchmark = 4 (Korok Forest)", gfx::col::good);
        } else {
            toast("Не удалось изменить maxlastbreath.ini", gfx::col::bad);
        }
    }
    if (n == 0) return;
    if (down & BTN(A)) {
        int idx = sel;
        std::string runs = strf("Прогон %d из %d для «%s».", m_tu.gameRunsDone(m_tu.cands[idx]) + 1, cfg().totkRuns, m_tu.cands[idx].name().c_str());
        confirmDanger("ВНИМАНИЕ: РИСК ДЕСИНХРОНИЗАЦИИ АКБ",
                      "Вылет или зависание игры на разгоне памяти может сбить контроллер аккумулятора: неверный процент заряда, "
                      "внезапное выключение «на 30 %», долгая зарядка. Лечится: Ryazha-clk → Питание → Зарядка и батарея → "
                      "«Сбросить калибровку», затем полный цикл заряда.\n\nДержите консоль на зарядке, сохраняйтесь заранее. " + runs +
                          " Нужно " + strf("%d", cfg().totkRuns) + " прогона — результат усредняется.",
                      [this, idx] { launchCandidate(idx); }, "Понимаю, запустить");
    }
    if (down & BTN(ZL)) {
        int idx = sel;
        // Только применить — поиграть на этой частоте: РАМ и тайминги, ЦПУ/ГПУ как в профиле (без лишнего расхода).
        startJob("Применение кандидата", [this, idx] {
            std::string err;
            if (m_tu.gameApply(m_t, idx, &err, false))
                toast("Применено: РАМ и тайминги, ЦПУ/ГПУ — как в профиле. Для замера в бенчмарке — A", gfx::col::good);
            else
                toast("Не применилось: " + err, gfx::col::bad);
        });
    }
    if (down & BTN(X)) {
        if (m_tu.gamePendingCand < 0 && m_tu.gameActiveCand < 0)
            toast("Нет применённого кандидата — выберите его и нажмите A", gfx::col::warn);
        else
            collectGameOnStart();
        m_haveLastRun = lastRun(&m_lastRun, &m_lastRunFile);
    }
    if (down & BTN(Y)) totkActions(sel);
    if (down & BTN(B)) offerStabilize(sel, "отжать вручную");
    if (down & BTN(Minus)) {
        int idx = sel;
        confirm("Очистить прогоны", "Удалить результаты TOTK у «" + m_tu.cands[idx].name() + "»?", [this, idx] { m_tu.gameClearRuns(m_t, idx); });
    }
}

void App::inputManual(u64 down) {
    if (busy()) return;
    const auto& knobs = allKnobs();
    int& ks = m_manKnob;
    if (down & BTN(Up)) ks = std::max(0, ks - 1);
    if (down & BTN(Down)) ks = std::min((int)knobs.size() - 1, ks + 1);
    if (!m_ladder.empty()) {
        if (down & BTN(ZL)) m_manFreq = std::max(0, m_manFreq - 1);
        if (down & BTN(ZR)) m_manFreq = std::min((int)m_ladder.size() - 1, m_manFreq + 1);
    }
    const KnobDef& k = knobs[ks];
    auto it = m_manSet.find(k.key);
    if (down & BTN(Right)) {
        if (it == m_manSet.end())
            m_manSet[k.key] = k.absentIsZero ? std::min(1, k.max) : k.min;
        else if (it->second < k.max)
            it->second++;
        if (k.absentIsZero && m_manSet[k.key] == 0) m_manSet.erase(k.key);
    }
    if (down & BTN(Left)) {
        if (it == m_manSet.end()) {
            if (k.absentIsZero) m_manSet[k.key] = std::max(-1, k.min);
        } else if (it->second > k.min) {
            it->second--;
            if (k.absentIsZero && it->second == 0) m_manSet.erase(it);
        } else if (!k.absentIsZero) {
            m_manSet.erase(it);  // ниже минимума — «как в общих»
        }
    }
    if (m_ladder.empty()) return;
    u32 hz = m_ladder[m_manFreq];
    if ((down & BTN(A)) && m_safe) {
        offerSafeReboot(0);
        return;
    }
    if (down & BTN(A)) {
        KnobSet set = m_manSet;
        m_tu.gameDeactivate(m_t);
        startJob("Применение", [this, hz, set] {
            m_t.setClockMode(ConsoleTarget::ClockMode::Free);
            ApplyInfo ai;
            std::string err;
            bool ok = m_t.apply(hz, set, &ai, &err);
            std::lock_guard<std::mutex> lk(m_uiMtx);
            if (ok) {
                m_manAppliedHz = hz;
                m_manAppliedSet = set;
                m_manNote = strf("Применено: %s МГц (реально %s). %s", mhzStr(hz).c_str(), mhzStr(ai.realHz).c_str(), m_t.lastNote.c_str());
                for (auto& n : ai.notApplied) m_manNote += " | " + n;
            } else {
                m_manNote = "Ошибка: " + err;
            }
        });
    }
    if (down & BTN(Y)) {
        startJob("Замер", [this] {
            bool ab = false;
            BenchResult r = m_t.bench(3, &ab);
            std::lock_guard<std::mutex> lk(m_uiMtx);
            if (!ab && r.valid) {
                m_manPrev = m_manLast;
                m_manLast = r;
            }
        });
    }
    if (down & BTN(X)) {
        startJob("memtest 30 с", [this] {
            std::string err;
            bool ab = false;
            bool ok = m_t.stability(30, &err, &ab);
            std::lock_guard<std::mutex> lk(m_uiMtx);
            if (ab)
                m_manNote = "memtest прерван";
            else
                m_manNote = ok ? "memtest 30 с: ошибок нет" : "memtest: " + err;
        });
    }
    if (down & BTN(Minus)) {
        // Подставить значения, найденные подбором: каждый чип уникален, и доводить руками удобнее от своих чисел.
        struct Src {
            std::string name;
            u32 hz;  // 0 — частоту не менять
            KnobSet set;
        };
        auto srcs = std::make_shared<std::vector<Src>>();
        srcs->push_back({"Сток Ryazha-clk (ступень 0, ручки пропускной способности — 0)", 0, ryazhaStockSet()});
        if (m_chip.preset) {
            KnobSet g = ryazhaStockSet();
            for (auto& kv : presetToKnobs(m_chip.preset->common)) g[kv.first] = kv.second;
            srcs->push_back({std::string("Гайд Horizon-OC «Common» для ") + m_chip.preset->chip, 0, g});
            if (cfg().communityBands && m_chip.preset->bands) {
                KnobSet b = ryazhaStockSet();
                for (auto& kv : presetKnobsAt(m_chip.preset, hz, true)) b[kv.first] = kv.second;
                srcs->push_back({std::string("Проверенный набор (") + m_chip.preset->bandsSrc + ") для " + mhzStr(hz) + " МГц: " + knobSetCompact(b), 0, b});
            }
        }
        {
            std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
            for (auto& c : m_tu.cands)
                if (!c.baseline) srcs->push_back({c.name() + " — найденный набор: " + knobSetCompact(c.set), c.hz, c.set});
            bool tuning = m_tu.phase == Phase::Timing || m_tu.phase == Phase::Optimize || m_tu.phase == Phase::Verify;
            if (tuning && m_tu.tIdx < (int)m_tu.targets.size())
                srcs->push_back({"Текущий набор подбора (" + mhzStr(m_tu.targets[m_tu.tIdx]) + " МГц): " + knobSetCompact(m_tu.cur),
                                 m_tu.targets[m_tu.tIdx], m_tu.cur});
        }
        {
            TimingsIni ini;
            ini.parse(m_t.original());
            KnobSet orig;
            if (ini.getSection(hzToKhz(hz), orig)) srcs->push_back({"Секция " + mhzStr(hz) + " из исходного timings.ini", 0, orig});
        }
        auto arch = archLoad();
        int sweet = archSweet(arch, m_chip.dramId, cfg().wGame / 100.0);
        for (size_t i = 0; i < arch.size(); i++)
            srcs->push_back({std::string((int)i == sweet ? SWEET_MARK " · " : "") + "Архив: " + archLabel(arch[i]), arch[i].hz, arch[i].set});
        std::vector<std::string> names;
        for (auto& sItem : *srcs) names.push_back(sItem.name);
        pick("Подставить значения", names, [this, srcs](int i) {
            const Src& sItem = (*srcs)[i];
            m_manSet = sItem.set;
            if (sItem.hz)
                for (size_t k = 0; k < m_ladder.size(); k++)
                    if (m_ladder[k] == sItem.hz) m_manFreq = (int)k;
            std::lock_guard<std::mutex> lk(m_uiMtx);
            m_manNote = "Подставлено: " + sItem.name + ". A — применить";
        });
    }
    if (down & BTN(B)) {
        m_manSet.clear();
        m_manNote = "Набор очищен (все ручки — общие из кипа)";
    }
}

void App::loadHistory() {
    auto rows = histLoad();
    m_histRows = rows.size();
    m_hist = histSummary(rows, &m_histBase);
    m_histSel = std::min(m_histSel, std::max(0, (int)m_hist.size() - 1));
}

void App::inputResults(u64 down) {
    if (busy()) return;
    if (down & BTN(ZR)) {
        m_resAll = !m_resAll;
        if (m_resAll) loadHistory();
    }
    if (m_resAll) {
        if (down & BTN(Up)) m_histSel = std::max(0, m_histSel - 1);
        if (down & BTN(Down)) m_histSel = std::min(std::max(0, (int)m_hist.size() - 1), m_histSel + 1);
        if (down & BTN(Y)) {
            makeDirs(paths::appDir());
            if (writeFileSafe(paths::report(), m_tu.report()))
                toast("Отчёт: config/ram-sweetspot/report.txt · история: config/ram-sweetspot/history.csv", gfx::col::good);
        }
        return;
    }
    int& sel = m_sel[S_Results];
    if (down & BTN(Up)) sel = std::max(0, sel - 1);
    if (down & BTN(Down)) sel = std::min(std::max(0, (int)m_tu.points.size() - 1), sel + 1);
    int best = m_tu.bestCandidate();
    if (down & BTN(Y)) {
        makeDirs(paths::appDir());
        if (writeFileSafe(paths::report(), m_tu.report()))
            toast("Отчёт: config/ram-sweetspot/report.txt", gfx::col::good);
        else
            toast("Не удалось сохранить отчёт", gfx::col::bad);
    }
    if (best < 0) return;
    const Candidate& c = m_tu.cands[best];
    if (down & BTN(A)) {
        std::string text = strf("Записать в timings.ini секцию [%s] (%s) и поставить %s МГц до перезагрузки?", mhzStr(c.hz).c_str(),
                                knobSetCompact(c.set).c_str(), mhzStr(c.hz).c_str());
        if (c.game != 1) text += "\n\nВнимание: этот кандидат ещё не проверен в TOTK.";
        confirm("Записать итог", text, [this, best] {
            startJob("Запись итога", [this, best] {
                std::string err;
                if (m_tu.finalize(m_t, best, &err))
                    toast("Итог записан в timings.ini. Частоту закрепите кнопкой X (профиль TOTK).", gfx::col::good);
                else
                    toast("Не получилось: " + err, gfx::col::bad);
                m_t.dlog.restore();
            });
        });
    }
    if (down & BTN(X)) {
        confirm("Профиль TOTK", strf("Записать РАМ %s МГц в профиль TOTK (все режимы питания) у Ryazha-clk?", mhzStr(c.hz).c_str()),
                [this, best] { saveProfileForTotk(best); });
    }
}

static const int kSettingCount = 35;  // строки — в drawSettings()

void App::inputSettings(u64 down) {
    int& sel = m_sel[S_Settings];
    if (down & BTN(Up)) sel = wrapStep(sel, kSettingCount, -1);
    if (down & BTN(Down)) sel = wrapStep(sel, kSettingCount, 1);
    int d = (down & BTN(Right)) ? 1 : (down & BTN(Left)) ? -1 : 0;
    if (!d && !(down & BTN(A))) return;
    if (!d) d = 1;
    Settings& c = cfg();
    auto step = [&](int& v, int s, int lo, int hi) { v = std::max(lo, std::min(hi, v + d * s)); };
    switch (sel) {
        case 0:
            c.applyRamMode(d < 0 ? Settings::nextRamMode(Settings::nextRamMode(c.ramMode)) : Settings::nextRamMode(c.ramMode));
            if (c.ramMode == Settings::ModeSafe)
                toast("Безопасный: запас 2 ступени, memtest 10 / 300 с, после загрузки 120 с; любое замечание в игре исключает кандидата",
                      gfx::col::good);
            else if (c.ramMode == Settings::ModeMedium)
                toast("Средний (около стабильного): запас 1 ступень, memtest 8 / 120 с, после загрузки 60 с; вылет исключает, остальное штрафует",
                      gfx::col::info);
            else
                toast("Максимум: запас 0, memtest 6 / 60 с, прирост от 0,12 % по замеру А/Б, финиш tREFI × tRFC, расширенные ручки и tREFI 11700",
                      gfx::col::warn);
            break;
        case 1: step(c.wBw, 10, 0, 100); break;
        case 2: step(c.wGame, 10, 0, 100); break;
        case 3: step(c.quickSec, c.quickSec >= 10 ? 5 : 1, 2, 60); break;
        case 4: step(c.verifySec, 15, 15, 900); break;
        case 5: step(c.benchPasses, 1, 1, 5); break;
        case 6: step(c.margin, 1, 0, 2); break;
        case 7: step(c.patience, 1, 1, 4); break;
        case 8: {
            // Низ прогона: ступени лестницы демона (без 204 МГц); без лестницы — шаг 100 МГц.
            std::vector<int> opts;
            for (u32 hz : m_ladder)
                if (hz >= 665000000u) opts.push_back((int)(hz / 1000000u));
            if (opts.empty()) {
                step(c.minMhz, 100, 600, 2400);
                break;
            }
            int pos = 0;
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] <= c.minMhz + 1) pos = (int)i;
            if (opts[pos] < c.minMhz - 1 && d < 0) pos++;  // значение между ступенями: шаг влево — на ближайшую ниже
            pos = std::max(0, std::min((int)opts.size() - 1, pos + d));
            c.minMhz = opts[pos];
            break;
        }
        case 9: {
            // Листаем по ступеням лестницы демона.
            std::vector<int> opts = {0};
            for (u32 hz : m_ladder)
                if (hz >= 1600000000u) opts.push_back((int)(hz / 1000000u));
            int pos = 0;
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] == c.maxMhz) pos = (int)i;
            pos = std::max(0, std::min((int)opts.size() - 1, pos + d));
            c.maxMhz = opts[pos];
            break;
        }
        case 10: step(c.scanFailStop, 1, 0, 10); break;
        case 11: step(c.numTargets, 1, 1, 4); break;
        case 12: step(c.tempLimit, 1, 60, 85); break;
        case 13: c.usePreset = !c.usePreset; break;
        case 14: c.allowTrefiHigh = !c.allowTrefiHigh; break;
        case 15: c.advanced = !c.advanced; break;
        case 16: c.useDaemonLog = !c.useDaemonLog; break;
        case 17: c.autoLaunch = !c.autoLaunch; break;
        case 18:
        case 19:
        case 20: {
            // Листаем по частотам демона; 0 — не фиксировать.
            const std::vector<u32>& list = sel == 20 ? m_gpuList : m_cpuList;
            int& v = sel == 18 ? c.cpuTuneMhz : (sel == 19 ? c.cpuGameMhz : c.gpuGameMhz);
            std::vector<int> opts = {0};
            for (u32 hz : list) opts.push_back((int)(hz / 1000000u));
            if (opts.size() == 1) {
                // Демон не отдал список — шаг 50 МГц.
                v = std::max(0, std::min(sel == 20 ? 1600 : 3100, v + d * 50));
                break;
            }
            int pos = 0;
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] <= v) pos = (int)i;
            pos = std::max(0, std::min((int)opts.size() - 1, pos + d));
            v = opts[pos];
            break;
        }
        case 21: c.kipStage = !c.kipStage; break;
        case 22: step(c.kipCheckSec, 10, 10, 300); break;
        case 23: step(c.totkRuns, 1, 1, 5); break;
        case 24: c.optimize = !c.optimize; break;
        case 25: c.scanGuide = !c.scanGuide; break;
        case 26: step(c.peakDropMb, 25, 25, 1000); break;
        case 27: step(c.peakTargets, 1, 0, 4); break;
        case 28: step(c.scanPasses, 1, 1, 10); break;
        case 29: step(c.scanHangStop, 1, 0, 5); break;
        case 30: c.kipFirst = !c.kipFirst; break;
        case 31: step(c.researchRuns, 1, 1, 10); break;
        case 32: c.autoResume = !c.autoResume; break;
        case 33: step(c.timingPasses, 1, 1, 30); break;
        case 34: c.refreshTune = !c.refreshTune; break;
    }
    c.save();
    m_tu.computeRanking();
}

static std::string sysSettingsPath() { return sdRoot() + "atmosphere/config/system_settings.ini"; }
static std::string gpuSchedFile() { return paths::appDir() + "gpusched.txt"; }  // момент загрузки, в которую меняли

void App::loadGpuSched() {
    std::string t;
    m_gpuSched = -1;
    if (readFile(sysSettingsPath(), t)) {
        std::string v = iniGet(t, "am.gpu", "gpu_scheduling_enabled");  // «u8!0x1» / «u8!0x0»
        size_t bang = v.find('!');
        if (!v.empty()) m_gpuSched = strtol(v.substr(bang == std::string::npos ? 0 : bang + 1).c_str(), nullptr, 0) ? 1 : 0;
    }
    m_gpuSchedRclk = 0;
    if (readFile(paths::rclkDir() + "config.ini", t)) {
        std::string v = iniGet(t, "values", "gpu_scheduling");
        if (!v.empty()) m_gpuSchedRclk = (int)toLong(v, 0);
    }
    std::string p;
    m_gpuSchedPending = readFile(gpuSchedFile(), p) && std::llabs(toLong(trim(p), 0) - m_t.bootEpoch()) <= 5;
}

bool App::setGpuSched(bool enabled, std::string* err) {
    std::string path = sysSettingsPath(), t;
    bool had = readFile(path, t);
    std::string bak = paths::appDir() + "system_settings.orig.ini";
    if (!fileExists(bak)) writeFile(bak, had ? t : std::string("; system_settings.ini не было\n"));
    makeDirs(sdRoot() + "atmosphere/config");
    if (!writeFileSafe(path, iniSet(t, "am.gpu", "gpu_scheduling_enabled", enabled ? "u8!0x1" : "u8!0x0"))) {
        *err = "не удалось записать " + path;
        return false;
    }
    writeFile(gpuSchedFile(), strf("%lld\n", (long long)m_t.bootEpoch()));
    LOGI("Планировщик ГПУ: %s в %s ([am.gpu] gpu_scheduling_enabled) — подействует после перезагрузки", enabled ? "включён" : "выключен",
         path.c_str());
    loadGpuSched();
    return true;
}

std::string App::gpuSchedText() const {
    std::string s = m_gpuSched == 0 ? "выкл." : m_gpuSched == 1 ? "вкл." : "вкл. (как в системе)";
    if (m_gpuSchedPending) s += " (после перезагрузки)";
    return s;
}

void App::inputService(u64 down) {
    int& sel = m_sel[S_Service];
    const int rows = 11;
    if (down & BTN(Up)) sel = wrapStep(sel, rows, -1);
    if (down & BTN(Down)) sel = wrapStep(sel, rows, 1);
    if (busy() || !(down & BTN(A))) return;
    switch (sel) {
        case 0:
            confirm("Вернуть как было", "Записать исходный timings.ini (копия до подбора) и снять временную частоту памяти?",
                    [this] { restoreEverything(); });
            break;
        case 1:
            confirm("Сбросить прогресс", "Удалить журнал подбора? Результаты и кандидаты будут потеряны.", [this] {
                m_tu.resetAll();
                removeFile(paths::state());
                toast("Журнал удалён", gfx::col::good);
            });
            break;
        case 2:
            if (fileExists(paths::rclkLogFlag())) {
                m_t.dlog.restore();
                if (fileExists(paths::rclkLogFlag())) removeFile(paths::rclkLogFlag());
                toast("Лог демона выключен", gfx::col::good);
            } else {
                m_t.dlog.enable();
                toast("Лог демона включён (config/ryazha-clk/log.flag)", gfx::col::good);
            }
            break;
        case 3:
            m_logTail = m_t.dlog.tail(14);
            m_logTitle = "Хвост лога демона: config/ryazha-clk/log.txt";
            if (m_logTail.empty()) toast("Лог пуст или выключен", gfx::col::warn);
            break;
        case 5:
            m_logTail = logx::tail(14);
            m_logTitle = "Журнал приложения: " + logx::file();
            if (m_logTail.empty()) toast("Журнал приложения пуст", gfx::col::warn);
            break;
        case 9:
            archiveMenu();
            break;
        case 10: {
            // Планировщик ГПУ — системная настройка HOS, не оверлей и не демон: система читает её при загрузке.
            bool turnOff = m_gpuSched != 0;
            std::string t = turnOff ? "Выключить планировщик ГПУ? Система перестанет делить время ГПУ с игрой — кадров в TOTK может стать "
                                      "больше, но меню HOME, скриншоты, запись видео и оверлеи могут подтормаживать или сбоить."
                                    : "Включить планировщик ГПУ обратно, как в системе?";
            t += "\n\nПишется в atmosphere/config/system_settings.ini ([am.gpu] gpu_scheduling_enabled), копия файла до первой смены — "
                 "config/ram-sweetspot/system_settings.orig.ini. Действует после перезагрузки. Прогоны TOTK до и после смены между собой не "
                 "сравнивайте.";
            if (m_gpuSchedRclk)
                t += strf("\n\nВ config.ini Ryazha-clk задан gpu_scheduling=%d — демон при загрузке поставит планировщик сам и перекроет эту "
                          "настройку.",
                          m_gpuSchedRclk);
            confirm(
                "Планировщик ГПУ", t,
                [this, turnOff] {
                    std::string err;
                    if (!setGpuSched(!turnOff, &err)) {
                        toast("Планировщик ГПУ: " + err, gfx::col::bad);
                        return;
                    }
                    confirm("Нужна перезагрузка",
                            strf("Планировщик ГПУ %s. Система читает эту настройку при загрузке — перезагрузить консоль сейчас?",
                                 turnOff ? "выключен" : "включён"),
                            [this] {
                                std::string e;
                                if (!m_t.reboot(&e)) toast("Перезагрузка не удалась: " + e + ". Перезагрузите через меню питания.", gfx::col::bad);
                            },
                            "Перезагрузить");
                },
                turnOff ? "Выключить" : "Включить");
            break;
        }
        case 8:
            if (Tuner::researchActive()) {
                confirm("Исследование идёт", "Продолжить с того места или прервать (таблица по уже собранному — research.txt)?",
                        [this] { startResearch(false); }, "Продолжить", [this] { toast(Tuner::researchStop(), gfx::col::warn); }, "Прервать");
                break;
            }
            confirm("Исследование потока",
                    strf("%d повторных прогонов всей лестницы: в каждом — сток Ryazha-clk и тайминги гайда, на каждой ступени memtest и %d "
                         "замеров (медиана). Подбор и кип не трогаются. Займёт порядка %d мин. После зависания просто откройте приложение — "
                         "продолжит само. Итог — config/ram-sweetspot/research.txt (среднее и разброс по ступеням) и research.csv.",
                         cfg().researchRuns, cfg().scanPasses, cfg().researchRuns * 2 * (int)m_ladder.size() * (cfg().quickSec + cfg().scanPasses * 2 + 2) / 60),
                    [this] { startResearch(true); }, "Начать");
            break;
        case 7:
            confirm("Очистить реестр", strf("Удалить %zu записей о нестабильных значениях? Подбор снова будет пробовать значения, которые уже сбоили.",
                                            m_tu.unstable.size()),
                    [this] {
                        std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
                        m_tu.unstable.clear();
                        m_tu.unstableDirty = true;
                        removeFile(paths::unstable());
                        toast("Реестр нестабильных значений очищен", gfx::col::good);
                    });
            break;
        case 6:
            confirm("Перезагрузить консоль",
                    m_kipNeedsReboot ? "Перезагрузить через систему, чтобы кип вернулся к значениям до переноса?"
                                     : m_noWd ? "Перезагрузить консоль через систему?"
                                              : "Перезагрузить консоль через систему (сторож Ryazha-clk не сочтёт это зависанием)?",
                    [this] {
                        std::string err;
                        if (!m_t.reboot(&err)) toast("Перезагрузка не удалась: " + err + ". Перезагрузите через меню питания.", gfx::col::bad);
                    },
                    "Перезагрузить");
            break;
        case 4:
            if (rclk::memFreqList(&m_ladder)) {
                std::sort(m_ladder.begin(), m_ladder.end());
                toast(strf("Ступеней частоты: %zu", m_ladder.size()), gfx::col::good);
            } else {
                toast("Демон не ответил", gfx::col::bad);
            }
            break;
    }
}
