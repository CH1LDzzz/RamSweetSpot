// Предпросмотр экранов на ПК: рендер в PPM тем же кодом отрисовки, что и на консоли.
// Запуск: make -C tests preview (нужен прогон ./sim для снимков состояния).
#include <cstdio>

#include "../source/app.hpp"
#include "../source/gfx.hpp"

struct AppPreview {
    static void setup(App& a) {
        a.m_warned = true;
        a.m_daemon = true;
        a.m_svc = "ryz:clk";
        a.m_daemonVer = "3.6.0";
        a.m_api = 2;
        a.m_hwType = 5;
        a.m_chip = chipFromDramId(17);
        a.m_appMode = true;
        a.m_buffersOk = true;
        a.m_bufInfo = "свободно 3120 МБ: замер 192+32 МБ, memtest 2048 МБ";
        a.m_ctx.valid = true;
        a.m_ctx.freqs[2] = 2934400000u;
        a.m_ctx.realFreqs[2] = 2933900000u;
        a.m_ctx.overrideFreqs[2] = 2934400000u;
        a.m_ctx.temps[0] = 47000;
        a.m_ctx.temps[7] = 44000;
        a.m_ctx.freqs[0] = 2397000000u;
        a.m_ctx.freqs[1] = 1305600000u;
        a.m_ctx.overrideFreqs[0] = 2397000000u;
        a.m_ctx.overrideFreqs[1] = 1305600000u;
        a.m_ctx.voltages[1] = 1175000;
        a.m_ctx.voltages[4] = 640000;
        a.m_ucIni = "sdmc:/atmosphere/contents/0100F2C0115B6000/romfs/UltraCam/maxlastbreath.ini";
        a.m_ucBench = 4;
        a.m_haveLastRun = true;
        a.m_lastRunFile = "sdmc:/TOTKBench.txt";
        a.m_lastRun.location = "Korok Forest";
        a.m_lastRun.frames = 3421;
        a.m_lastRun.avg = 57;
        a.m_lastRun.low1 = 44;
        a.m_lastRun.low01 = 36;
    }
    static void load(App& a, const std::string& file) {
        std::string text;
        a.m_tu.resetAll();
        if (readFile(file, text)) a.m_tu.deserialize(text);
        a.m_tu.chip = a.m_chip;
        a.m_ladder = a.m_tu.ladder;
        a.m_manFreq = (int)a.m_ladder.size() - 8;
    }
    static int run(App& a, const std::string& root, const std::string& out);
    static void shot(App& a, const char* name) {
        a.draw();
        gfx::saveFramePPM(name);
        printf("  %s\n", name);
    }
};

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "./sdroot";
    std::string out = argc > 2 ? argv[2] : "./shots";
    setSdRoot(root);
    // Третий аргумент — свой шрифт: широкий (Verdana) ближе к системному шрифту консоли, наложения видны сразу.
    if (argc > 3 && !gfx::initHost(argv[3])) {
        printf("шрифт %s не открылся\n", argv[3]);
        return 1;
    }
    if (argc <= 3 && !gfx::initHost("/Library/Fonts/Arial Unicode.ttf") && !gfx::initHost("/System/Library/Fonts/Supplemental/Arial.ttf") &&
        !gfx::initHost("C:/Windows/Fonts/arial.ttf")) {
        printf("нет шрифта\n");
        return 1;
    }
    makeDirs(out);
    static App a;
    return AppPreview::run(a, root, out);
}

int AppPreview::run(App& a, const std::string& root, const std::string& out) {
    AppPreview::setup(a);
    auto path = [&](const char* n) { return out + "/" + n; };

    a.m_warned = false;
    AppPreview::shot(a, path("0_warning.ppm").c_str());
    // Безопасный режим сторожа — окно сразу при входе, поверх предупреждения.
    a.m_safe = true;
    a.offerSafeReboot(0);
    AppPreview::shot(a, path("0a_warning_safe.ppm").c_str());
    a.m_dlg.open = false;
    a.m_safe = false;
    a.m_toastUntil = 0;
    a.m_warned = true;

    a.m_screen = S_Auto;
    AppPreview::shot(a, path("1_auto_idle.ppm").c_str());

    AppPreview::load(a, root + "/preview_timing.txt");
    AppPreview::shot(a, path("2_auto_timing.ppm").c_str());
    // Идёт автоподбор: значок справа в строке датчиков не должен ложиться поверх них.
    a.m_running = true;
    a.m_jobAuto = true;
    AppPreview::shot(a, path("2a_auto_busy.ppm").c_str());
    // Безопасный режим сторожа: отсчёт перезагрузки.
    a.m_running = false;
    a.m_safe = true;
    a.m_safeRebootAt = nowMs() + 7000;
    AppPreview::shot(a, path("2b_auto_safe.ppm").c_str());
    a.m_safe = false;
    a.m_safeRebootAt = 0;
    // Длинное название этапа в левой панели.
    Phase keep = a.m_tu.phase;
    a.m_tu.phase = Phase::Optimize;
    AppPreview::shot(a, path("2c_auto_optimize.ppm").c_str());
    a.m_tu.phase = keep;

    AppPreview::load(a, root + "/preview_game.txt");
    a.m_tu.gamePendingCand = -1;
    a.m_tu.gameActiveCand = 1;
    if (a.m_tu.cands.size() > 2) {
        a.m_tu.cands[2].game = 1;
        a.m_tu.cands[2].crashes = 1;
        a.m_tu.cands[2].attempts = 2;
        a.m_tu.cands[2].note.clear();
        a.m_tu.computeRanking();
    }
    a.m_screen = S_Totk;
    AppPreview::shot(a, path("3_totk.ppm").c_str());
    a.totkActions(1);
    AppPreview::shot(a, path("3a_totk_actions.ppm").c_str());
    a.m_pick.open = false;
    a.writeFinal(1);
    AppPreview::shot(a, path("3b_totk_final.ppm").c_str());
    a.m_dlg.open = false;

    // Свитспот: та же оценка на частоте ниже — кадров больше, потока чуть меньше; рекомендация остаётся своей.
    if (a.m_tu.cands.size() > 1 && a.m_tu.cands[1].b.valid) {
        auto keepCands = a.m_tu.cands;
        Candidate s = a.m_tu.cands[1];
        s.id = (int)a.m_tu.cands.size();
        s.hz = 2828800000u;
        s.crashes = s.attempts = 0;
        s.b.readMBs *= 0.996;
        s.b.writeMBs *= 0.996;
        s.b.copyMBs *= 0.996;
        for (auto& r : s.runs) r.frames += 6;
        while (a.m_tu.gameRunsDone(s) < a.cfg().totkRuns) s.runs.push_back(s.runs.empty() ? TotkRun{"Korok Forest", 2772, 27.7, 25, 23} : s.runs[0]);
        if (a.m_tu.gameRunsDone(a.m_tu.cands[1]) < a.cfg().totkRuns) a.m_tu.cands[1].runs = s.runs;
        for (auto& r : a.m_tu.cands[1].runs) r.frames = s.runs[0].frames - 6;
        a.m_tu.cands[1].game = s.game = 1;
        a.m_tu.cands.push_back(s);
        a.m_tu.computeRanking();
        a.m_screen = S_Totk;
        a.m_sel[S_Totk] = (int)a.m_tu.cands.size() - 1;
        AppPreview::shot(a, path("3c_totk_sweet.ppm").c_str());
        a.m_screen = S_Results;
        AppPreview::shot(a, path("4b_results_sweet.ppm").c_str());
        // Свитспот ниже рекомендации: кадров чуть меньше, а потока почти столько же — та же выгода на частоте ниже.
        for (auto& r : a.m_tu.cands.back().runs) r.frames -= 8;
        a.m_tu.computeRanking();
        a.m_screen = S_Totk;
        AppPreview::shot(a, path("3d_totk_sweet_other.ppm").c_str());
        a.m_screen = S_Results;
        AppPreview::shot(a, path("4c_results_sweet_other.ppm").c_str());
        // Архив: две частоты — свитспот помечается в списке.
        removeFile(archivePath());
        a.archiveSaveCand(1);
        a.archiveSaveCand((int)a.m_tu.cands.size() - 1);
        a.m_toastUntil = 0;
        a.archiveMenu();
        AppPreview::shot(a, path("7c_archive_sweet.ppm").c_str());
        a.m_pick.open = false;
        removeFile(archivePath());
        a.m_tu.cands = keepCands;
        a.m_tu.computeRanking();
        a.m_sel[S_Totk] = 0;
    }

    a.m_screen = S_Results;
    AppPreview::shot(a, path("4_results.ppm").c_str());
    // За всё время — история из прогона ./sim.
    a.m_resAll = true;
    a.loadHistory();
    a.m_histSel = (int)a.m_hist.size() / 2;
    AppPreview::shot(a, path("4a_results_history.ppm").c_str());
    a.m_resAll = false;

    a.m_screen = S_Manual;
    a.m_manSet = {{"tRCD", 4}, {"tRP", 3}, {"RL", -1}, {"ramPD", 1}};
    a.m_manKnob = 1;
    a.m_manPrev.valid = true;
    a.m_manPrev.readMBs = 24100; a.m_manPrev.writeMBs = 19800; a.m_manPrev.copyMBs = 21900; a.m_manPrev.latNs = 158.2; a.m_manPrev.lat2Ns = 96.1;
    a.m_manLast = a.m_manPrev;
    a.m_manLast.readMBs = 24600; a.m_manLast.latNs = 152.4;
    a.m_manNote = "Применено: 2934.4 МГц (реально 2933.9). [gen] тайминги применены живьём: таблиц 41 из 41; 2934.4 MHz";
    a.m_manAppliedHz = 2934400000u;
    a.m_manAppliedSet = a.m_manSet;
    AppPreview::shot(a, path("5_manual.ppm").c_str());

    a.m_screen = S_Settings;
    AppPreview::shot(a, path("6_settings.ppm").c_str());

    a.m_screen = S_Service;
    a.m_logTitle = "Журнал приложения: sdmc:/config/ram-sweetspot/logs/ramss.log";
    a.m_logTail = {"2026-10-24 18:02:11.204 [I] Частоты: режим «прогон TOTK» — ЦПУ 2397.0 МГц, ГПУ 1305.6 МГц",
                   "2026-10-24 18:09:40.917 [W] TOTK: отчёт о падении sdmc:/atmosphere/crash_reports/01800000001_0100f2c0115b6000.log",
                   "2026-10-24 18:09:41.002 [E] Сообщение: К1 · 2934.4 МГц: игра упала. Настройки всё ещё применены — можно прогнать бенчмарк снова","[mgr] RAM clock change: 2934.4 MHz", "[tune] поправки таймингов перечитаны: точек 1",
                   "[gen] тайминги применены живьём: таблиц 41 из 41; 2934.4 MHz; 812 мкс; emc_cfg 0000F00D (сон памяти вкл)",
                   "[hw] 2934.4 MHz: регистров арбитра сверено 21, совпало 21 — всё дошло"};
    AppPreview::shot(a, path("7_service.ppm").c_str());
    // Архив конфигураций: два кандидата сохранены, открыт список и действия с записью.
    removeFile(archivePath());
    if (a.m_tu.cands.size() > 2) {
        a.archiveSaveCand(1);
        a.archiveSaveCand(2);
    }
    a.m_toastUntil = 0;
    a.archiveMenu();
    AppPreview::shot(a, path("7a_archive.ppm").c_str());
    a.m_pick.open = false;
    a.archiveEntryActions(0);
    AppPreview::shot(a, path("7b_archive_entry.ppm").c_str());
    a.m_pick.open = false;
    removeFile(archivePath());

    a.m_screen = S_Auto;
    a.toast("Прошлая проба не завершилась (зависание/перезагрузка) — отмечена как сбой. A — продолжить подбор.", gfx::col::warn);
    a.confirm("Консоль перезагружалась",
              "К1 · 2944.0 МГц: временная частота сброшена — похоже, консоль перезагружалась.\nЕсли игра зависла на этом кандидате — "
              "отметьте его как сбойный. Если вы перезагрузили консоль сами — прогоните бенчмарк ещё раз.",
              [] {}, "Отметить сбой", [] {}, "Повторить");
    AppPreview::shot(a, path("8_dialog.ppm").c_str());
    return 0;
}
