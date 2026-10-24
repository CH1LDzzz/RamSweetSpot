#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "gfx.hpp"
#include "log.hpp"

using namespace gfx;

namespace {
const int CX = 266, CY = 106, CW = 1280 - CX - 16, CH = 684 - CY - 6;

// Кнопка-подсказка: кружок с буквой + подпись. Возвращает ширину.
int hint(int x, int y, const char* btn, const std::string& label) {
    int bw = std::max(26, textWidth(16, btn) + 14);
    rect(x, y, bw, 24, col::panel2);
    frame(x, y, bw, 24, col::line);
    textCenter(x + bw / 2, y + 2, 16, col::text, btn);
    int w = text(x + bw + 6, y + 1, 18, col::dim, label);
    return bw + 6 + w + 18;
}

void card(int x, int y, int w, int h, const char* title = nullptr) {
    rect(x, y, w, h, col::panel);
    if (title) {
        text(x + 14, y + 8, 18, col::dim, title);
        hline(x + 14, y + 34, w - 28, col::line);
    }
}

std::string elapsed(s64 since) {
    if (since <= 0) return "—";
    s64 s = epochNow() - since;
    if (s < 0) s = 0;
    return strf("%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
}

u32 phaseColor(Phase p) {
    switch (p) {
        case Phase::Failed: return col::bad;
        case Phase::Done: return col::good;
        case Phase::Game: return col::info;
        case Phase::Idle: return col::dim;
        default: return col::accent;
    }
}
}  // namespace

void App::draw() {
    beginFrame();
    clear(col::bg);
    if (!m_warned) {
        drawWarning();
        endFrame();
        return;
    }
    drawHeader();
    drawTabs();
    switch (m_screen) {
        case S_Auto: drawAuto(CX, CY, CW, CH); break;
        case S_Totk: drawTotk(CX, CY, CW, CH); break;
        case S_Manual: drawManual(CX, CY, CW, CH); break;
        case S_Results: drawResults(CX, CY, CW, CH); break;
        case S_Settings: drawSettings(CX, CY, CW, CH); break;
        case S_Service: drawService(CX, CY, CW, CH); break;
    }
    drawFooter();
    drawToast();
    drawDialog();
    endFrame();
}

void App::drawWarning() {
    rect(0, 0, W, 6, col::accent);
    text(80, 60, 40, col::accent, "RamSweetSpot");
    text(80, 112, 22, col::dim, strf("v%s · автоподбор частоты, таймингов и задержек памяти для Ryazha-clk", APP_VER));
    int y = 170;
    rect(80, y, W - 160, 380, col::panel);
    rect(80, y, 6, 380, col::warn);
    text(106, y + 18, 26, col::warn, "Прочитайте перед началом");
    std::string t =
        "• Разгон памяти может вызвать зависания, порчу сохранений и данных на SD-карте и NAND. Сделайте резервные копии "
        "NAND, PRODINFO, emuMMC и SD. Тестируйте на emuMMC.\n"
        "• Приложение меняет только «живые» настройки Ryazha-clk: временную частоту РАМ и секции config/ryazha-clk/timings.ini. "
        "Кип не трогается — после перезагрузки консоль стартует как раньше.\n"
        "• Если консоль зависнет, сторож Ryazha-clk перезапустит её. Откройте приложение снова — подбор продолжится, "
        "а пробу, на которой случилось зависание, оно учтёт как сбой.\n"
        "• Лучше запускать через игру (удерживая R) — так доступно больше памяти для теста. Перед подбором поставьте "
        "VDD2/VDDQ по гайду для своего чипа и поднимите потолок РАМ в Ryazha-clk.";
    textWrap(106, y + 64, 20, col::text, t, W - 220, 30);
    if (!m_initError.empty()) text(106, y + 340, 20, col::bad, m_initError);
    int x = 80;
    x += hint(x, 600, "A", "Понятно, продолжить");
    hint(x, 600, "+", "Выход");
}

void App::drawHeader() {
    rect(0, 0, W, 56, col::panel);
    rect(0, 56, W, 2, col::accent);
    int x = text(20, 10, 30, col::accent, "RamSweetSpot");
    text(20 + x + 12, 20, 18, col::dim, "v" APP_VER);
    // Справа — демон и чип.
    std::string d = m_daemon ? strf("Ryazha-clk %s · %s · API %u", m_daemonVer.c_str(), m_svc.c_str(), m_api) : std::string("Ryazha-clk не найден");
    textRight(W - 20, 6, 18, m_daemon ? col::good : col::bad, d);
    std::string chip = strf("%s · %s", consoleName(m_hwType), m_chip.name.c_str());
    if (m_chip.preset) chip += strf(" · тир %s", m_chip.preset->tier);
    textRight(W - 20, 30, 16, col::dim, chip);

    // Строка состояния памяти.
    rect(0, 58, W, 40, col::bg);
    int y = 66;
    x = 20;
    if (m_ctx.valid) {
        u32 set = m_ctx.freqs[RclkModule_MEM], real = m_ctx.realFreqs[RclkModule_MEM], ov = m_ctx.overrideFreqs[RclkModule_MEM];
        x += text(x, y, 20, col::text, strf("РАМ %s МГц", mhzStr(set).c_str()));
        if (real) x += text(x + 8, y + 2, 18, col::dim, strf("реально %s", mhzStr(real).c_str())) + 8;
        x += text(x + 16, y + 2, 18, ov ? col::warn : col::dim, ov ? strf("временно %s", mhzStr(ov).c_str()) : std::string("без временной")) + 16;
        u32 cpu = m_ctx.freqs[RclkModule_CPU], gpu = m_ctx.freqs[RclkModule_GPU];
        bool cpuOv = m_ctx.overrideFreqs[RclkModule_CPU] != 0, gpuOv = m_ctx.overrideFreqs[RclkModule_GPU] != 0;
        x += text(x + 16, y + 2, 18, cpuOv ? col::warn : col::dim, strf("ЦПУ %u", cpu / 1000000)) + 16;
        x += text(x + 12, y + 2, 18, gpuOv ? col::warn : col::dim, strf("ГПУ %u", gpu / 1000000)) + 12;
        x += text(x + 16, y + 2, 18, col::dim, strf("SoC %d °C", m_ctx.temps[0] / 1000)) + 16;
        if (m_ctx.temps[7]) x += text(x + 16, y + 2, 18, col::dim, strf("MEM %d °C", m_ctx.temps[7] / 1000)) + 16;
        if (m_ctx.voltages[1]) x += text(x + 16, y + 2, 18, col::dim, strf("VDD2 %d мВ", voltMv(m_ctx.voltages[1]))) + 16;
        if (m_ctx.voltages[4]) x += text(x + 16, y + 2, 18, col::dim, strf("VDDQ %d мВ", voltMv(m_ctx.voltages[4]))) + 16;
    } else {
        text(x, y, 20, col::dim, m_daemon ? "нет данных от демона" : "Установите Ryazha-clk (входит в Ряженку) и перезагрузите консоль");
    }
    if (busy()) textRight(W - 20, y + 2, 18, col::accent, "● " + (m_jobAuto ? std::string("автоподбор идёт") : m_jobTitle + "…"));
}

void App::drawTabs() {
    static const char* names[S_Count] = {"Автоподбор", "Проверка в TOTK", "Ручная настройка", "Результаты", "Настройки", "Сервис"};
    rect(0, 98, 250, 586, col::panel);
    for (int i = 0; i < S_Count; i++) {
        int y = 116 + i * 54;
        bool on = i == m_screen;
        if (on) {
            rect(0, y - 8, 250, 48, col::sel);
            rect(0, y - 8, 5, 48, col::accent);
        }
        text(24, y, 22, on ? col::text : col::dim, names[i]);
    }
    std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
    int y = 116 + S_Count * 54 + 16;
    hline(20, y, 210, col::line);
    text(24, y + 14, 16, col::dim, "Этап подбора");
    text(24, y + 36, 20, phaseColor(m_tu.phase), phaseName(m_tu.phase));
    if (m_tu.trials) text(24, y + 64, 16, col::dim, strf("проб: %d", m_tu.trials));
    text(24, 652, 16, col::dim, "L / R — разделы");
}

void App::drawFooter() {
    rect(0, 684, W, 36, col::panel);
    int x = 20, y = 690;
    switch (m_screen) {
        case S_Auto:
            if (busy()) {
                x += hint(x, y, "X", "Пауза");
            } else {
                x += hint(x, y, "A", m_tu.isAuto() ? "Продолжить" : (m_tu.phase == Phase::Game ? "К проверке в TOTK" : "Начать подбор"));
                x += hint(x, y, "Y", "Сбросить");
            }
            x += hint(x, y, "↑↓", "Журнал");
            break;
        case S_Totk:
            x += hint(x, y, "A", cfg().autoLaunch ? "Применить и запустить TOTK" : "Применить");
            x += hint(x, y, "ZL", "Только применить");
            x += hint(x, y, "X", "Забрать результат");
            x += hint(x, y, "Y", "Сбой");
            x += hint(x, y, "-", "Очистить");
            if (!m_ucIni.empty() && m_ucBench != 4) x += hint(x, y, "ZR", "Лес короков");
            break;
        case S_Manual:
            x += hint(x, y, "ZL/ZR", "Частота");
            x += hint(x, y, "←→", "Значение");
            x += hint(x, y, "A", "Применить");
            x += hint(x, y, "Y", "Замер");
            x += hint(x, y, "X", "memtest");
            x += hint(x, y, "-", "Загрузить");
            x += hint(x, y, "B", "Очистить");
            break;
        case S_Results:
            x += hint(x, y, "A", "Записать лучший");
            x += hint(x, y, "X", "В профиль TOTK");
            x += hint(x, y, "Y", "Сохранить отчёт");
            break;
        case S_Settings:
            x += hint(x, y, "←→", "Изменить");
            break;
        case S_Service:
            x += hint(x, y, "A", "Выполнить");
            break;
    }
    textRight(W - 20, y + 1, 18, col::dim, "+ выход");
}

void App::drawToast() {
    std::string t;
    u32 c;
    {
        std::lock_guard<std::mutex> lk(m_uiMtx);
        if (m_toast.empty() || nowMs() > m_toastUntil) return;
        t = m_toast;
        c = m_toastColor;
    }
    int w = std::min(W - 300, textWidth(20, t) + 40);
    int x = CX + (CW - w) / 2, y = 626;
    rect(x, y, w, 46, col::panel2);
    rect(x, y, 5, 46, c);
    text(x + 20, y + 11, 20, col::text, t, w - 30);
}

void App::drawDialog() {
    if (!m_dlg.open) return;
    // Затемнение.
    for (int yy = 0; yy < H; yy += 2) hline(0, yy, W, col::bg);
    int w = 820, h = 360, x = (W - w) / 2, y = (H - h) / 2;
    rect(x, y, w, h, col::panel);
    rect(x, y, w, 4, col::accent);
    text(x + 28, y + 22, 28, col::text, m_dlg.title);
    textWrap(x + 28, y + 76, 20, col::text, m_dlg.text, w - 56, 30);
    int bx = x + 28, by = y + h - 50;
    bx += hint(bx, by, "A", m_dlg.yesLabel);
    if (m_dlg.alt) bx += hint(bx, by, "Y", m_dlg.altLabel);
    hint(bx, by, "B", "Отмена");
}

// ============================================================== Автоподбор

void App::drawAuto(int x, int y, int w, int h) {
    std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
    const Tuner& t = m_tu;
    // Шаги.
    card(x, y, w, 76);
    static const char* steps[] = {"Базовый замер", "Частота", "Тайминги", "Длинная проверка", "TOTK"};
    int cur = 0;
    switch (t.phase) {
        case Phase::Baseline: cur = 0; break;
        case Phase::FreqScan: cur = 1; break;
        case Phase::Seed:
        case Phase::Timing: cur = 2; break;
        case Phase::Verify: cur = 3; break;
        case Phase::Game: cur = 4; break;
        case Phase::Done: cur = 5; break;
        default: cur = -1;
    }
    int sw = (w - 40) / 5;
    for (int i = 0; i < 5; i++) {
        int sx = x + 20 + i * sw;
        u32 c = i < cur ? col::good : (i == cur ? col::accent : col::line);
        rect(sx, y + 18, sw - 12, 6, c);
        text(sx, y + 34, 18, i == cur ? col::text : col::dim, strf("%d. %s", i + 1, steps[i]));
    }
    int yy = y + 90;

    if (t.phase == Phase::Idle) {
        card(x, yy, w, h - 90, "Что будет сделано");
        std::string s =
            "1. Базовый замер на стоковых 1600 МГц: поток и задержка памяти, оценка шума.\n"
            "2. Прогон частот: каждая ступень лестницы Ryazha-clk в диапазоне из настроек (по умолчанию от 1331 МГц до верха) — "
            "memtest и замер. Сбой (memtest, PLL drop, зависание) прогон не останавливает: проверяется следующая ступень. "
            "Ступени медленнее предыдущей помечаются жёлтым и в доводку не идут.\n"
            "3. Тайминги на 1–2 лучших частотах по одной ручке в порядке руководства Ryazha-clk: tREFI → tRCD → tRP → tRAS → "
            "tRRD → tRFC → tRTW → tWTR → RL/WL → пропускная способность → HP Mode. Старт — с пресета вашего чипа из гайда Horizon-OC. "
            "Ужесточение принимается, только если даёт измеримый прирост; у сбоя оставляется запас.\n"
            "4. Длинная проверка финала, при сбое — ослабление по порядку из гайда.\n"
            "5. Проверка кандидатов бенчмарком TOTK в лесу короков (файл TOTKBench.txt / TOTKBenchmark.txt в корне карты).";
        textWrap(x + 18, yy + 48, 19, col::text, s, w - 36, 28);
        int iy = yy + h - 90 - 150;
        hline(x + 18, iy, w - 36, col::line);
        text(x + 18, iy + 12, 18, col::dim, strf("Чип: %s", m_chip.name.c_str()));
        if (m_chip.preset)
            text(x + 18, iy + 38, 18, col::dim,
                 strf("Пресет «Common»: %s · типично %u–%u МГц · VDD2 %d мВ, VDDQ %d мВ", knobSetShort(presetToKnobs(m_chip.preset->common)).c_str(),
                      m_chip.preset->minMhz, m_chip.preset->maxMhz, m_chip.preset->vdd2, m_chip.preset->vddq));
        else
            text(x + 18, iy + 38, 18, col::dim, "Пресета для этого чипа нет — старт с общих таймингов кипа");
        text(x + 18, iy + 64, 18, col::dim,
             strf("Ступеней частоты у демона: %zu · память под тест: %s", m_ladder.size(), m_bufInfo.empty() ? "—" : m_bufInfo.c_str()));
        text(x + 18, iy + 90, 18, m_appMode ? col::dim : col::warn,
             m_appMode ? "Запуск через игру — памяти для memtest достаточно" : "Запуск через альбом — памяти меньше; лучше через игру (удерживая R)");
        return;
    }

    // Текущее состояние.
    card(x, yy, w, 170, "Сейчас");
    u32 sc = t.phase == Phase::Failed ? col::bad : col::text;
    text(x + 18, yy + 46, 22, sc, t.phase == Phase::Failed ? t.error : (t.status.empty() ? phaseName(t.phase) : t.status), w - 36);
    text(x + 18, yy + 80, 18, col::dim,
         strf("Проб: %d · время: %s · шум замера %.2f %% · база: %.0f МБ/с, %.1f нс", t.trials, elapsed(t.startedEpoch).c_str(), t.eps * 100,
              t.base.bw(), t.base.latNs));
    std::string line2, line3;
    if (t.phase == Phase::FreqScan) {
        line2 = t.lastOk >= 0 && t.lastOk < (int)t.ladder.size() ? strf("Самая высокая стабильная частота: %s МГц", mhzStr(t.ladder[t.lastOk]).c_str())
                                                                   : std::string("Стабильных частот пока нет");
        line3 = strf("Прогон: ступень %d из %zu (%s … %s МГц)", std::min(t.scanPos + 1, (int)t.coarse.size()), t.coarse.size(),
                     t.coarse.empty() ? "—" : mhzStr(t.ladder[t.coarse.front()]).c_str(), t.coarse.empty() ? "—" : mhzStr(t.ladder[t.coarse.back()]).c_str());
    } else if ((t.phase == Phase::Seed || t.phase == Phase::Timing || t.phase == Phase::Verify) && t.tIdx < (int)t.targets.size()) {
        std::string knob = t.kIdx < (int)t.plan.size() ? t.plan[t.kIdx].key : std::string("—");
        line2 = strf("Частота %d из %zu: %s МГц · ручка %s (%d/%zu) · оценка %.3f", t.tIdx + 1, t.targets.size(), mhzStr(t.targets[t.tIdx]).c_str(),
                     knob.c_str(), std::min(t.kIdx + 1, (int)t.plan.size()), t.plan.size(), t.curScore);
        line3 = "Набор: " + knobSetShort(t.cur);
    } else if (t.phase == Phase::Game || t.phase == Phase::Done) {
        line2 = strf("Кандидатов: %zu — проверьте их в TOTK на вкладке «Проверка в TOTK»", t.cands.size() ? t.cands.size() - 1 : 0);
    }
    text(x + 18, yy + 106, 18, col::text, line2, w - 36);
    text(x + 18, yy + 132, 18, col::accent, line3, w - 36);

    // Журнал.
    int ly = yy + 184;
    int lh = h - (ly - y);
    card(x, ly, w, lh, "Журнал");
    int rows = (lh - 50) / 26;
    int total = (int)t.events.size();
    int off = std::min(m_sel[S_Auto], std::max(0, total - rows));
    int start = std::max(0, total - rows - off);
    for (int i = 0; i < rows && start + i < total; i++) {
        const std::string& e = t.events[start + i];
        u32 c = col::text;
        if (e.find("сбой") != std::string::npos || e.find("СБОЙ") != std::string::npos || e.find("не прош") != std::string::npos) c = col::bad;
        else if (e.find("Принято") != std::string::npos || e.find("стабильно") != std::string::npos || e.find("прошла") != std::string::npos) c = col::good;
        else if (e.find("без прироста") != std::string::npos || e.find("оставлен") != std::string::npos) c = col::dim;
        text(x + 18, ly + 46 + i * 26, 17, c, e, w - 36);
    }
}

// ============================================================== TOTK

void App::drawTotk(int x, int y, int w, int h) {
    std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
    card(x, y, w, 118, "Бенчмарк TOTK (UltraCam)");
    if (m_ucIni.empty()) {
        text(x + 18, y + 46, 18, col::warn, "maxlastbreath.ini не найден в atmosphere/contents/0100F2C0115B6000 — установлен ли мод UltraCam?");
    } else {
        u32 c = m_ucBench == 4 ? col::good : col::warn;
        text(x + 18, y + 46, 18, c,
             strf("maxlastbreath.ini: Benchmark = %d (%s)%s", m_ucBench, benchmarkName(m_ucBench), m_ucBench == 4 ? "" : " — нужен 4, нажмите ZR"));
    }
    if (m_haveLastRun)
        text(x + 18, y + 72, 18, col::dim,
             strf("Последний результат (%s): %s — кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f", m_lastRunFile.substr(m_lastRunFile.rfind('/') + 1).c_str(),
                  m_lastRun.location.c_str(), m_lastRun.frames, m_lastRun.avg, m_lastRun.low1, m_lastRun.low01));
    else
        text(x + 18, y + 72, 18, col::dim, "Файла TOTKBench.txt / TOTKBenchmark.txt в корне карты пока нет");
    if (m_tu.gamePendingCand >= 0 && m_tu.gamePendingCand < (int)m_tu.cands.size())
        text(x + 18, y + 94, 18, col::accent,
             "Ждём результат для «" + m_tu.cands[m_tu.gamePendingCand].name() + "»: в игре пройдите бенчмарк, выйдите и откройте RamSweetSpot");
    else
        text(x + 18, y + 94, 18, col::dim,
             strf("Прогон в игре идёт на ЦПУ %d МГц и ГПУ %d МГц для всех кандидатов (Настройки)", cfg().cpuGameMhz, cfg().gpuGameMhz));

    int ty = y + 132;
    card(x, ty, w, h - 132 - 70, nullptr);
    if (m_tu.cands.empty()) {
        textWrap(x + 18, ty + 20, 20, col::dim,
                 "Кандидатов пока нет. Сначала пройдите автоподбор: после длинной проверки здесь появятся лучшие наборы "
                 "и строка «Как было» для сравнения.",
                 w - 36, 30);
        return;
    }
    const int c0 = x + 18, c1 = x + 236, c2 = x + 320, c3 = x + 500, c4 = x + 716, c5 = x + 786;
    text(c0, ty + 12, 17, col::dim, "Кандидат");
    text(c1, ty + 12, 17, col::dim, "Синт.");
    text(c2, ty + 12, 17, col::dim, "Поток / задержка");
    text(c3, ty + 12, 17, col::dim, "TOTK: кадры·ср·1%·0.1%");
    text(c4, ty + 12, 17, col::dim, "Итог");
    text(c5, ty + 12, 17, col::dim, "Статус");
    hline(x + 14, ty + 38, w - 28, col::line);
    int best = m_tu.bestCandidate();
    int sel = std::min(m_sel[S_Totk], (int)m_tu.cands.size() - 1);
    for (size_t i = 0; i < m_tu.cands.size() && i < 12; i++) {
        const Candidate& c = m_tu.cands[i];
        int ry = ty + 46 + (int)i * 52;
        if ((int)i == sel) rect(x + 6, ry - 4, w - 12, 50, col::sel);
        if ((int)i == best) rect(x + 6, ry - 4, 4, 50, col::good);
        bool active = (int)i == m_tu.gameActiveCand;
        text(c0, ry, 19, active ? col::accent : col::text, active ? "● " + c.name() : c.name(), c1 - c0 - 10);
        text(c0, ry + 24, 15, col::dim, c.baseline ? "настройки до подбора" : knobSetShort(c.set), c3 - c0 - 20);
        if (c.synth > 0) text(c1, ry, 19, col::text, strf("%.3f", c.synth));
        if (c.b.valid) text(c2, ry, 17, col::text, strf("%.0f МБ/с · %.1f нс", c.b.bw(), c.b.latNs));
        if (!c.runs.empty()) {
            double f = 0, a = 0, l1 = 0, l01 = 0;
            for (auto& r : c.runs) {
                f += r.frames;
                a += r.avg;
                l1 += r.low1;
                l01 += r.low01;
            }
            double n = (double)c.runs.size();
            text(c3, ry, 17, col::text, strf("%.0f · %.0f · %.0f · %.0f", f / n, a / n, l1 / n, l01 / n));
            text(c3, ry + 24, 15, col::dim, strf("прогонов: %zu", c.runs.size()));
        }
        if (c.combined > 0) text(c4, ry, 19, (int)i == best ? col::good : col::text, strf("%.3f", c.combined));
        std::string st;
        u32 sc = col::dim;
        if ((int)i == m_tu.gamePendingCand) {
            st = "ждём бенчмарк";
            sc = col::accent;
        } else if (c.game == 2) {
            st = "сбой: " + c.note;
            sc = col::bad;
        } else if (c.game == 1) {
            st = c.crashes ? "проверен, был сбой" : "проверен";
            sc = c.crashes ? col::warn : col::good;
        } else {
            st = "не проверен";
        }
        text(c5, ry, 17, sc, st, x + w - c5 - 12);
        std::string sub;
        if (active) sub = "применён сейчас";
        if (c.crashes) sub += (sub.empty() ? "" : " · ") + strf("сбоев %d из %d", c.crashes, std::max(c.attempts, c.crashes));
        if (!sub.empty()) text(c5, ry + 24, 15, active ? col::accent : col::dim, sub, x + w - c5 - 12);
    }
    int ry = y + h - 60;
    card(x, ry, w, 60);
    if (best >= 0) {
        const Candidate& b = m_tu.cands[best];
        text(x + 18, ry + 8, 19, col::good, "Рекомендация: " + b.name() + (b.game == 1 ? "" : " (без проверки в игре)"));
        text(x + 18, ry + 34, 16, col::dim, knobSetShort(b.set), w - 36);
    } else {
        text(x + 18, ry + 18, 19, col::dim, "Рекомендации пока нет");
    }
}

// ============================================================== Ручная настройка

void App::drawManual(int x, int y, int w, int h) {
    const auto& knobs = allKnobs();
    int lw = 560;
    card(x, y, lw, h);
    u32 hz = (m_manFreq >= 0 && m_manFreq < (int)m_ladder.size()) ? m_ladder[m_manFreq] : 0;
    text(x + 18, y + 12, 18, col::dim, "Частота (ZL / ZR)");
    text(x + 18, y + 36, 28, col::accent, hz ? "< " + mhzStr(hz) + " МГц >" : std::string("нет списка частот"));
    if (hz && hz <= 1600000000u) text(x + 300, y + 44, 16, col::warn, "сток: ручки лестницы не действуют");
    hline(x + 14, y + 76, lw - 28, col::line);
    int rows = (h - 96) / 30;
    int first = std::max(0, std::min(m_manKnob - rows / 2, (int)knobs.size() - rows));
    for (int i = 0; i < rows && first + i < (int)knobs.size(); i++) {
        int k = first + i;
        const KnobDef& d = knobs[k];
        int ry = y + 86 + i * 30;
        if (k == m_manKnob) rect(x + 6, ry - 3, lw - 12, 29, col::sel);
        text(x + 18, ry, 18, col::text, d.label, 250);
        auto it = m_manSet.find(d.key);
        std::string v = it == m_manSet.end() ? (d.absentIsZero ? std::string("0 (сток)") : std::string("общ. (кип)"))
                                             : strf("%d · %s", it->second, knobValueText(d, it->second).c_str());
        u32 vc = it == m_manSet.end() ? col::dim : col::accent;
        text(x + 290, ry, 18, vc, v, lw - 300);
    }

    int rx = x + lw + 14, rw = w - lw - 14;
    card(rx, y, rw, 240, "Замер (Y)");
    BenchResult last, prev;
    std::string note;
    {
        std::lock_guard<std::mutex> lk(m_uiMtx);
        last = m_manLast;
        prev = m_manPrev;
        note = m_manNote;
    }
    if (last.valid) {
        auto rowv = [&](int ry, const char* name, double v, double pv, const char* unit, bool lowerBetter) {
            text(rx + 18, ry, 19, col::dim, name);
            text(rx + 200, ry, 19, col::text, strf("%.1f %s", v, unit));
            if (pv > 0) {
                double d = (v - pv) / pv * 100;
                bool good = lowerBetter ? d < 0 : d > 0;
                if (std::fabs(d) >= 0.05) text(rx + 370, ry, 18, good ? col::good : col::bad, strf("%+.2f %%", d));
            }
        };
        rowv(y + 46, "Чтение", last.readMBs, prev.valid ? prev.readMBs : 0, "МБ/с", false);
        rowv(y + 76, "Запись", last.writeMBs, prev.valid ? prev.writeMBs : 0, "МБ/с", false);
        rowv(y + 106, "Копирование", last.copyMBs, prev.valid ? prev.copyMBs : 0, "МБ/с", false);
        rowv(y + 136, "Задержка", last.latNs, prev.valid ? prev.latNs : 0, "нс", true);
        rowv(y + 166, "Задержка ×2", last.lat2Ns, prev.valid ? prev.lat2Ns : 0, "нс", true);
        if (m_tu.base.valid) text(rx + 18, y + 200, 17, col::dim, strf("Оценка относительно базы 1600 МГц: %.3f", m_tu.scoreOf(last)));
    } else {
        text(rx + 18, y + 50, 18, col::dim, "Замеров ещё не было");
    }
    card(rx, y + 254, rw, h - 254, "Подсказка");
    const KnobDef& d = knobs[m_manKnob];
    text(rx + 18, y + 300, 20, col::text, d.label);
    text(rx + 18, y + 328, 18, col::dim, d.hint, rw - 36);
    text(rx + 18, y + 354, 17, col::dim, strf("Диапазон %d…%d · %s", d.min, d.max, d.risky ? "может портить данные — проверяйте memtest" : "данные не портит"),
         rw - 36);
    if (m_manAppliedHz)
        text(rx + 18, y + 386, 17, col::dim, strf("Применено: %s МГц, %s", mhzStr(m_manAppliedHz).c_str(), knobSetShort(m_manAppliedSet).c_str()), rw - 36);
    if (!note.empty()) textWrap(rx + 18, y + 416, 17, col::accent, note, rw - 36, 24);
}

// ============================================================== Результаты

void App::drawResults(int x, int y, int w, int h) {
    std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
    const Tuner& t = m_tu;
    card(x, y, w, 270, "Частоты: поток (столбцы) и задержка (линия)");
    if (t.points.empty()) {
        text(x + 18, y + 50, 18, col::dim, "Данных пока нет — запустите автоподбор");
    } else {
        int gx = x + 60, gy = y + 50, gw = w - 90, gh = 180;
        double maxBw = 1, minLat = 1e9, maxLat = 0;
        for (auto& p : t.points) {
            if (!p.b.valid) continue;
            maxBw = std::max(maxBw, p.b.bw());
            minLat = std::min(minLat, p.b.latNs);
            maxLat = std::max(maxLat, p.b.latNs);
        }
        hline(gx, gy + gh, gw, col::line);
        int n = (int)t.points.size();
        int bw = std::max(6, std::min(40, gw / std::max(1, n) - 6));
        int px = -1, py = -1;
        for (int i = 0; i < n; i++) {
            const FreqPoint& p = t.points[i];
            int cx = gx + (int)((i + 0.5) * gw / n);
            if (p.b.valid) {
                int bh = (int)(gh * p.b.bw() / maxBw);
                rect(cx - bw / 2, gy + gh - bh, bw, bh, p.status == 1 ? col::good : (p.status == 3 ? col::warn : col::bad));
                double span = std::max(1.0, maxLat - minLat);
                int ly = gy + 10 + (int)((p.b.latNs - minLat) / span * (gh - 40));
                if (px >= 0) line(px, py, cx, ly, col::info);
                rect(cx - 3, ly - 3, 6, 6, col::info);
                px = cx;
                py = ly;
            } else {
                rect(cx - bw / 2, gy + gh - 8, bw, 8, col::bad);
            }
            if (n <= 16 || i % (n > 30 ? 3 : 2) == 0) textCenter(cx, gy + gh + 6, 14, col::dim, mhzStr(p.hz));
        }
        text(x + 14, gy - 4, 14, col::dim, strf("%.0f", maxBw));
    }
    // Таблица точек.
    int ty = y + 284;
    int tw = 600;
    card(x, ty, tw, h - 284, "Точки частоты");
    int rows = (h - 284 - 50) / 26;
    int first = std::max(0, std::min(m_sel[S_Results], (int)t.points.size() - rows));
    for (int i = 0; i < rows && first + i < (int)t.points.size(); i++) {
        const FreqPoint& p = t.points[first + i];
        int ry = ty + 46 + i * 26;
        text(x + 18, ry, 17, col::text, mhzStr(p.hz) + " МГц");
        if (p.status == 1)
            text(x + 150, ry, 17, col::good, strf("%.0f МБ/с · %.1f нс · %.3f", p.b.bw(), p.b.latNs, p.score));
        else if (p.status == 3)
            text(x + 150, ry, 17, col::warn, strf("%.0f МБ/с · %.1f нс · медленнее предыдущей", p.b.bw(), p.b.latNs), tw - 170);
        else
            text(x + 150, ry, 17, col::bad, p.reason, tw - 170);
    }
    // Итог.
    int bx = x + tw + 14, bw2 = w - tw - 14;
    card(bx, ty, bw2, h - 284, "Итог");
    int best = t.bestCandidate();
    if (best < 0) {
        text(bx + 18, ty + 50, 18, col::dim, "Кандидатов пока нет");
        return;
    }
    const Candidate& c = t.cands[best];
    text(bx + 18, ty + 48, 24, col::good, mhzStr(c.hz) + " МГц");
    textWrap(bx + 18, ty + 84, 18, col::text, knobSetShort(c.set), bw2 - 36, 26);
    if (c.b.valid)
        text(bx + 18, ty + 150, 18, col::dim, strf("Поток %.0f МБ/с · задержка %.1f нс · оценка %.3f", c.b.bw(), c.b.latNs, c.synth), bw2 - 36);
    text(bx + 18, ty + 178, 18, c.game == 1 ? col::good : col::warn,
         c.game == 1 ? strf("Проверен в TOTK, итоговая оценка %.3f", c.combined) : std::string("Ещё не проверен в TOTK"));
    if (t.phase == Phase::Done) text(bx + 18, ty + 210, 18, col::good, "Записан в timings.ini");
    text(bx + 18, ty + h - 284 - 40, 15, col::dim, "Отчёт: config/ram-sweetspot/report.txt", bw2 - 36);
}

// ============================================================== Настройки

void App::drawSettings(int x, int y, int w, int h) {
    (void)h;
    static const char* labels[] = {
        "Вес пропускной способности", "Вес TOTK в итоговой оценке", "memtest на каждую пробу", "Длинная проверка финала", "Повторов замера",
        "Запас стабильности", "Терпение", "Низ прогона частот", "Верх прогона частот", "Сбоев подряд до остановки", "Частот для доводки таймингов",
        "Порог температуры SoC", "Стартовать с пресета чипа", "tREFI 11700 нс", "Расширенные ручки", "Подтверждение по логу демона",
        "Запускать TOTK автоматически", "ЦПУ при подборе РАМ", "ЦПУ в бенчмарке TOTK", "ГПУ в бенчмарке TOTK"};
    static const char* hints[] = {
        "остальное — вес задержки; 50 = поровну", "остальное — синтетика", "секунд; больше — надёжнее и дольше", "секунд на каждую итоговую частоту",
        "берётся лучший результат", "на сколько ступеней отступить от сбоя", "сколько ступеней без прироста пробовать дальше",
        "с этой ступени лестницы начинается прогон", "0 = до верха лестницы Ryazha-clk", "0 = пройти весь диапазон, сбой не останавливает",
        "сколько лучших частот доводить",
        "выше — пауза на остывание", "Common-тайминги из гайда Horizon-OC", "риск порчи данных при нагреве", "арбитр, растяжки шины, tRPab, tW2P",
        "ждать «тайминги применены живьём»", "после применения кандидата", "фиксируется на время memtest и замеров",
        "одинаково для всех кандидатов", "одинаково для всех кандидатов"};
    const Settings& c = m_tu.cfg;
    auto onoff = [](bool b) { return std::string(b ? "вкл." : "выкл."); };
    std::string vals[] = {
        strf("%d %% / задержка %d %%", c.wBw, 100 - c.wBw), strf("%d %%", c.wGame), strf("%d с", c.quickSec), strf("%d с", c.verifySec),
        strf("%d", c.benchPasses), strf("%d ступ.", c.margin), strf("%d", c.patience), strf("%d МГц", c.minMhz),
        c.maxMhz ? strf("%d МГц", c.maxMhz) : std::string("верх лестницы"), c.scanFailStop ? strf("%d", c.scanFailStop) : std::string("не останавливать"),
        strf("%d", c.numTargets), strf("%d °C", c.tempLimit), onoff(c.usePreset), onoff(c.allowTrefiHigh),
        onoff(c.advanced), onoff(c.useDaemonLog), onoff(c.autoLaunch),
        c.cpuTuneMhz ? strf("%d МГц", c.cpuTuneMhz) : std::string("не фиксировать"),
        c.cpuGameMhz ? strf("%d МГц", c.cpuGameMhz) : std::string("не фиксировать"),
        c.gpuGameMhz ? strf("%d МГц", c.gpuGameMhz) : std::string("не фиксировать")};
    const int n = 20;
    const int rh = 26;
    card(x, y, w, 20 + n * rh);
    for (int i = 0; i < n; i++) {
        int ry = y + 10 + i * rh;
        bool on = i == m_sel[S_Settings];
        if (on) rect(x + 6, ry - 3, w - 12, rh - 1, col::sel);
        text(x + 18, ry, 17, col::text, labels[i]);
        text(x + 420, ry + 2, 15, col::dim, hints[i], 420);
        textRight(x + w - 18, ry, 17, on ? col::accent : col::text, vals[i]);
    }
    text(x + 18, y + 20 + n * rh + 6, 15, col::dim, "Порядок ручек и запас применяются к новому подбору, частоты ЦПУ/ГПУ — сразу. Сохраняется автоматически.");
}

// ============================================================== Сервис

void App::drawService(int x, int y, int w, int h) {
    static const char* rows[] = {"Вернуть как было (исходный timings.ini, без временных частот)", "Сбросить прогресс подбора",
                                 "Лог демона Ryazha-clk: включить / выключить", "Показать хвост лога демона", "Перечитать список частот у демона",
                                 "Показать журнал приложения (ошибки и ход работы)"};
    const int nr = 6;
    card(x, y, w, 24 + nr * 32);
    for (int i = 0; i < nr; i++) {
        int ry = y + 12 + i * 32;
        if (i == m_sel[S_Service]) rect(x + 6, ry - 4, w - 12, 30, col::sel);
        text(x + 18, ry, 19, col::text, rows[i]);
    }
    int iy = y + 24 + nr * 32 + 12;
    card(x, iy, w, h - (iy - y), "Состояние");
    int ly = iy + 46;
    auto row = [&](const std::string& s, u32 c = col::dim) {
        text(x + 18, ly, 17, c, s, w - 36);
        ly += 24;
    };
    row(strf("Служба: %s · API %u · версия %s", m_daemon ? m_svc.c_str() : "нет", m_api, m_daemonVer.c_str()));
    row(strf("Ступеней частоты: %zu (%s … %s МГц)", m_ladder.size(), m_ladder.empty() ? "—" : mhzStr(m_ladder.front()).c_str(),
             m_ladder.empty() ? "—" : mhzStr(m_ladder.back()).c_str()));
    row(strf("Память под тесты: %s", m_bufInfo.c_str()));
    row(strf("Режим запуска: %s", m_appMode ? "через игру (приложение)" : "через альбом (апплет)"));
    row(strf("Копия исходного timings.ini: %s", m_t.originalAbsent() ? "файла не было" : paths::timingsBackup().c_str()));
    row(strf("Лог демона: %s", fileExists(paths::rclkLogFlag()) ? "включён" : "выключен"));
    int errs = logx::errorsThisSession();
    row(strf("Журнал приложения: %s (ошибок за сеанс: %d)", logx::file().c_str(), errs), errs ? col::warn : col::dim);
    if (!m_logTitle.empty()) row(m_logTitle, col::accent);
    for (auto& l : m_logTail) {
        u32 c = l.find("[E]") != std::string::npos ? col::bad : (l.find("[W]") != std::string::npos ? col::warn : col::text);
        row(l, c);
        if (ly > y + h - 24) break;
    }
}
