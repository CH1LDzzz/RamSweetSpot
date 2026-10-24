#include "app.hpp"

#include <algorithm>
#include <cmath>

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
        if (m_api != 2) LOGW("API демона %u: приложение сверено с API 2 (Ryazha-clk 3.6.0)", m_api);
        // Что сторож Ryazha-clk записал о прошлых перезапусках — пригодится при разборе сбоев.
        std::string wd;
        if (readFile(paths::rclkDir() + "watchdog.log", wd)) {
            auto lines = split(trim(wd), '\n');
            for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); i++)
                if (!trim(lines[i]).empty()) LOGI("watchdog.log Ryazha-clk: %s", trim(lines[i]).c_str());
        }
    }
    m_tu.ladder = m_ladder;
    m_tu.chip = m_chip;

    bool hang = false;
    bool have = m_tu.loadState(m_t, &hang);
    m_tu.chip = m_chip;  // чип всегда берём с консоли
    bool inProgress = have && (m_tu.isAuto() || m_tu.phase == Phase::Game);
    m_t.prepareOriginal(!inProgress);
    if (have && !m_ladder.empty() && m_tu.ladder != m_ladder && inProgress)
        toast("Лестница частот демона изменилась с прошлого раза — лучше начать подбор заново", gfx::col::warn);

    m_buffersOk = m_t.initBuffers(&m_bufInfo);

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

    if (hang)
        toast("Прошлая проба не завершилась (зависание/перезагрузка) — отмечена как сбой. A — продолжить подбор.", gfx::col::warn);
    if (m_tu.gamePendingCand >= 0 || m_tu.gameActiveCand >= 0) collectGameOnStart();
    if (m_tu.phase == Phase::Game) m_screen = S_Totk;
    return true;
}

void App::collectGameOnStart() {
    std::string msg;
    int r = m_tu.gameCollect(m_t, &msg);
    m_screen = S_Totk;
    if (r > 0) {
        toast(msg, gfx::col::good);
    } else if (r < 0) {
        // Сбой записан, кандидат можно сразу проверить снова (A) — ничего не блокируется.
        toast(msg, gfx::col::bad);
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
        u64 down = padGetButtonsDown(&g_pad);
        u64 held = padGetButtons(&g_pad);

        // Поток закончил работу — освобождаем его.
        if (m_threadValid && !m_running.load()) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_threadValid = false;
        }

        if (m_t.measuring.load()) {
            // Идёт замер: экран не перерисовываем, чтобы не отнимать ядро и шину памяти.
            if (down & HidNpadButton_X) pauseAuto();
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
    m_t.lockCpu(false);
    if (!m_launchGame) m_t.dlog.restore();
    m_tu.saveState(m_t);
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
        if (m_tu.phase == Phase::Game) {
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

void App::startAuto(bool fresh) {
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
                strf("Служба %s отвечает API %u, а приложение сверено с Ryazha-clk 3.6.0 (API 2). Номера команд могли измениться. "
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
        m_tu.saveState(m_t);
    }
    if (cfg().useDaemonLog) m_t.dlog.enable();
    m_userPause = false;
    LOGI("Автоподбор: %s, этап «%s»", fresh ? "новый" : "продолжение", phaseName(m_tu.phase));
    startJob("Автоподбор", nullptr);
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

void App::confirm(const std::string& title, const std::string& text, std::function<void()> yes, const char* yesLabel, std::function<void()> alt,
                  const char* altLabel) {
    m_dlg.open = true;
    m_dlg.title = title;
    m_dlg.text = text;
    m_dlg.yes = yes;
    m_dlg.alt = alt;
    m_dlg.yesLabel = yesLabel ? yesLabel : "Да";
    m_dlg.altLabel = altLabel ? altLabel : "";
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
    });
}

void App::saveProfileForTotk(int idx) {
    if (idx < 0 || idx >= (int)m_tu.cands.size()) return;
    u32 hz = m_tu.cands[idx].hz;
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

void App::input(u64 down, u64 held) {
    (void)held;
    if (!m_warned) {
        if (down & BTN(A)) m_warned = true;
        if (down & BTN(Plus)) requestExit(false);
        return;
    }
    if (m_dlg.open) {
        if (down & BTN(A)) {
            m_dlg.open = false;
            if (m_dlg.yes) m_dlg.yes();
        } else if ((down & BTN(Y)) && m_dlg.alt) {
            m_dlg.open = false;
            m_dlg.alt();
        } else if (down & BTN(B)) {
            m_dlg.open = false;
        }
        return;
    }
    if (down & BTN(Plus)) {
        if (busy()) {
            toast("Сначала пауза (X) и дождитесь конца пробы", gfx::col::warn);
            return;
        }
        if (m_tu.gamePendingCand >= 0 || m_tu.gameActiveCand >= 0) {
            confirm("Выход", "Кандидат применён для прогона TOTK (РАМ + ЦПУ/ГПУ). Выйти, оставив его настройки?", [this] { requestExit(false); },
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
                    "Приложение будет менять частоту памяти и тайминги на лету через Ryazha-clk, гонять memtest и замеры. "
                    "Если консоль зависнет, сторож Ryazha-clk перезапустит её — после этого просто откройте RamSweetSpot снова, "
                    "подбор продолжится с учётом сбоя.\n\nНачать?",
                    [this] { startAuto(true); }, "Начать");
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
    if (down & BTN(Down)) m_sel[S_Auto] = std::min((int)m_tu.events.size(), m_sel[S_Auto] + 1);
}

void App::inputTotk(u64 down) {
    if (busy()) return;
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
    if (down & BTN(ZL)) {
        int idx = sel;
        startJob("Применение кандидата", [this, idx] {
            std::string err;
            if (m_tu.gameApply(m_t, idx, &err))
                toast("Применено. Запустите TOTK вручную; выход — «+» → «Выйти»", gfx::col::good);
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
    if (down & BTN(Y)) {
        int idx = sel;
        confirm("Сбой в игре", "Отметить «" + m_tu.cands[idx].name() + "» как нестабильный в TOTK?",
                [this, idx] { m_tu.gameMarkFailed(m_t, idx, "отмечено вручную: нестабильно в игре"); });
    }
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
        // Подставить набор лучшего кандидата или секцию из исходного файла для этой частоты.
        int b = m_tu.bestCandidate();
        if (b >= 0 && m_tu.cands[b].hz == hz) {
            m_manSet = m_tu.cands[b].set;
            m_manNote = "Загружен набор лучшего кандидата";
        } else {
            TimingsIni ini;
            ini.parse(m_t.original());
            ini.getSection(hzToKhz(hz), m_manSet);
            m_manNote = "Загружена секция из исходного timings.ini";
        }
    }
    if (down & BTN(B)) {
        m_manSet.clear();
        m_manNote = "Набор очищен (все ручки — общие из кипа)";
    }
}

void App::inputResults(u64 down) {
    if (busy()) return;
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
                                knobSetShort(c.set).c_str(), mhzStr(c.hz).c_str());
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

static const int kSettingCount = 20;  // строки — в drawSettings()

void App::inputSettings(u64 down) {
    int& sel = m_sel[S_Settings];
    if (down & BTN(Up)) sel = (sel + kSettingCount - 1) % kSettingCount;
    if (down & BTN(Down)) sel = (sel + 1) % kSettingCount;
    int d = (down & BTN(Right)) ? 1 : (down & BTN(Left)) ? -1 : 0;
    if (!d && !(down & BTN(A))) return;
    if (!d) d = 1;
    Settings& c = cfg();
    auto step = [&](int& v, int s, int lo, int hi) { v = std::max(lo, std::min(hi, v + d * s)); };
    switch (sel) {
        case 0: step(c.wBw, 10, 0, 100); break;
        case 1: step(c.wGame, 10, 0, 100); break;
        case 2: step(c.quickSec, c.quickSec >= 10 ? 5 : 1, 2, 60); break;
        case 3: step(c.verifySec, 15, 15, 900); break;
        case 4: step(c.benchPasses, 1, 1, 5); break;
        case 5: step(c.margin, 1, 0, 2); break;
        case 6: step(c.patience, 1, 1, 4); break;
        case 7: {
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
        case 8: {
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
        case 9: step(c.scanFailStop, 1, 0, 10); break;
        case 10: step(c.numTargets, 1, 1, 4); break;
        case 11: step(c.tempLimit, 1, 60, 85); break;
        case 12: c.usePreset = !c.usePreset; break;
        case 13: c.allowTrefiHigh = !c.allowTrefiHigh; break;
        case 14: c.advanced = !c.advanced; break;
        case 15: c.useDaemonLog = !c.useDaemonLog; break;
        case 16: c.autoLaunch = !c.autoLaunch; break;
        case 17:
        case 18:
        case 19: {
            // Листаем по частотам демона; 0 — не фиксировать.
            const std::vector<u32>& list = sel == 19 ? m_gpuList : m_cpuList;
            int& v = sel == 17 ? c.cpuTuneMhz : (sel == 18 ? c.cpuGameMhz : c.gpuGameMhz);
            std::vector<int> opts = {0};
            for (u32 hz : list) opts.push_back((int)(hz / 1000000u));
            if (opts.size() == 1) {
                // Демон не отдал список — шаг 50 МГц.
                v = std::max(0, std::min(sel == 19 ? 1600 : 3100, v + d * 50));
                break;
            }
            int pos = 0;
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] <= v) pos = (int)i;
            pos = std::max(0, std::min((int)opts.size() - 1, pos + d));
            v = opts[pos];
            break;
        }
    }
    c.save();
    m_tu.computeRanking();
}

void App::inputService(u64 down) {
    int& sel = m_sel[S_Service];
    const int rows = 6;
    if (down & BTN(Up)) sel = (sel + rows - 1) % rows;
    if (down & BTN(Down)) sel = (sel + 1) % rows;
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
