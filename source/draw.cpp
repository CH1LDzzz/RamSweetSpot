#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

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
    setTheme(fun(2) ? 1 : 0);
    beginFrame();
    clear(col::bg);
    if (!m_warned) {
        drawWarning();
        drawToast();  // окна выбора — только после предупреждения (см. input)
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
    drawHidden();
    endFrame();
}

void App::drawWarning() {
    rect(0, 0, W, 6, col::accent);
    text(80, 60, 40, col::accent, "RamSweetSpot");
    text(80, 112, 22, col::dim, strf("v%s · автоподбор частоты, таймингов и задержек памяти для %s", APP_VER, RCLK_TARGET APP_BUILD));
    int y = 170;
    rect(80, y, W - 160, 380, col::panel);
    rect(80, y, 6, 380, col::warn);
    text(106, y + 18, 26, col::warn, "Прочитайте перед началом");
    std::string t =
        "• Разгон памяти может вызвать зависания, порчу сохранений и данных на SD-карте и NAND. Сделайте резервные копии "
        "NAND, PRODINFO, emuMMC и SD. Тестируйте на emuMMC.\n"
        "• Приложение меняет только «живые» настройки Ryazha-clk: временную частоту РАМ и секции config/ryazha-clk/timings.ini. "
        "Кип не трогается — после перезагрузки консоль стартует как раньше.\n" +
        std::string(m_noWd ? "• Если консоль зависнет, держите POWER 12 с и включите её снова — в этой версии Ryazha-clk сторожа нет. "
                           : "• Если консоль зависнет, сторож Ryazha-clk перезапустит её. ") +
        "Откройте приложение снова — подбор продолжится, а пробу, на которой случилось зависание, оно учтёт как сбой.\n"
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
    // Левее — АКБ: заряд и мощность по контроллеру батареи (Ryazha-clk, среднее): минус — разряд (потребление), плюс — заряд.
    std::string batt;
    if (m_battPct >= 0) batt = strf("АКБ %d %%", m_battPct);
    if (m_daemon && m_ctx.valid) {
        double w = m_ctx.power[1] / 1000.0;
        std::string p = w < 0 ? strf("потребление %.1f Вт", -w) : strf("заряд +%.1f Вт", w);
        batt += (batt.empty() ? "" : " · ") + p;
    }
    if (!batt.empty()) {
        u32 bc = m_battPct >= 0 && m_battPct < 15 ? col::bad : m_battPct >= 0 && m_battPct < 30 ? col::warn : col::dim;
        textRight(W - 20 - textWidth(18, d) - 28, 6, 18, bc, batt);
    }
    std::string chip = strf("%s · %s", consoleName(m_hwType), m_chip.name.c_str());
    if (m_chip.preset) chip += strf(" · тир %s", m_chip.preset->tier);
    textRight(W - 20, 30, 16, col::dim, chip);

    // Строка состояния памяти.
    rect(0, 58, W, 40, col::bg);
    int y = 66;
    x = 20;
    // Справа — что сейчас идёт; датчики слева занимают только место до него. Что не влезло — не рисуется
    // (последними уходят напряжения), а не ложится поверх.
    int limit = W - 20;
    std::string job;
    if (busy()) {
        job = "● " + (m_jobAuto ? std::string("автоподбор идёт") : m_jobTitle + "…");
        limit -= textWidth(18, job) + 24;
    }
    bool full = false;
    auto put = [&](int gap, int size, int dy, u32 c, const std::string& s) {
        if (full) return;
        if (x + gap + textWidth(size, s) > limit) {
            full = true;
            return;
        }
        x += text(x + gap, y + dy, size, c, s) + gap;
    };
    if (m_ctx.valid) {
        u32 set = m_ctx.freqs[RclkModule_MEM], real = m_ctx.realFreqs[RclkModule_MEM], ov = m_ctx.overrideFreqs[RclkModule_MEM];
        put(0, 20, 0, col::text, strf("РАМ %s МГц", mhzStr(set).c_str()));
        if (real) put(8, 18, 2, col::dim, strf("реально %s", mhzStr(real).c_str()));
        put(16, 18, 2, ov ? col::warn : col::dim, ov ? strf("временно %s", mhzStr(ov).c_str()) : std::string("без временной"));
        u32 cpu = m_ctx.freqs[RclkModule_CPU], gpu = m_ctx.freqs[RclkModule_GPU];
        bool cpuOv = m_ctx.overrideFreqs[RclkModule_CPU] != 0, gpuOv = m_ctx.overrideFreqs[RclkModule_GPU] != 0;
        put(16, 18, 2, cpuOv ? col::warn : col::dim, strf("ЦПУ %u", cpu / 1000000));
        put(12, 18, 2, gpuOv ? col::warn : col::dim, strf("ГПУ %u", gpu / 1000000));
        put(16, 18, 2, col::dim, strf("SoC %d °C", m_ctx.temps[0] / 1000));
        if (m_ctx.temps[7]) put(16, 18, 2, col::dim, strf("MEM %d °C", m_ctx.temps[7] / 1000));
        if (m_ctx.voltages[1]) put(16, 18, 2, col::dim, strf("VDD2 %d мВ", voltMv(m_ctx.voltages[1])));
        if (m_ctx.voltages[4]) put(16, 18, 2, col::dim, strf("VDDQ %d мВ", voltMv(m_ctx.voltages[4])));
    } else {
        text(x, y, 20, col::dim, m_daemon ? "нет данных от демона" : "Установите Ryazha-clk (входит в Ряженку) и перезагрузите консоль", limit - x);
    }
    if (!job.empty()) textRight(W - 20, y + 2, 18, col::accent, job);
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
    // Название этапа — в ширину панели, длинное переносится на вторую строку («4. Оптимизация таймингов»).
    int ph = textWrap(24, y + 36, 20, phaseColor(m_tu.phase), phaseName(m_tu.phase), 210, 24);
    if (m_tu.trials) text(24, y + 36 + ph + 4, 16, col::dim, strf("проб: %d", m_tu.trials));
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
            x += hint(x, y, "↑↓", m_tu.phase == Phase::Idle ? "Прокрутка" : "Журнал");
            break;
        case S_Totk:
            x += hint(x, y, "A", cfg().autoLaunch ? "Прогон TOTK" : "Прогон (запуск вручную)");
            x += hint(x, y, "ZL", "Только применить");
            x += hint(x, y, "X", "Забрать результат");
            x += hint(x, y, "Y", "Действия");
            x += hint(x, y, "B", "Отжать");
            x += hint(x, y, "-", "Очистить");
            if (!m_ucIni.empty() && m_ucBench != 4) x += hint(x, y, "ZR", "Лес короков");
            break;
        case S_Manual:
            x += hint(x, y, "ZL/ZR", "Частота");
            x += hint(x, y, "←→", "Значение");
            x += hint(x, y, "A", "Применить");
            x += hint(x, y, "Y", "Замер");
            x += hint(x, y, "X", "memtest");
            x += hint(x, y, "-", "Подставить найденное");
            x += hint(x, y, "B", "Очистить");
            break;
        case S_Results:
            if (m_resAll) {
                x += hint(x, y, "ZR", "Этот подбор");
                x += hint(x, y, "↑↓", "Частота");
                x += hint(x, y, "Y", "Сохранить отчёт");
                break;
            }
            x += hint(x, y, "A", "Записать лучший");
            x += hint(x, y, "X", "В профиль TOTK");
            x += hint(x, y, "Y", "Сохранить отчёт");
            x += hint(x, y, "ZR", "За всё время");
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

// ============================================================== скрытое меню

void App::drawHidden() {
    if (!m_hmOpen) return;
    for (int yy = 0; yy < H; yy += 2) hline(0, yy, W, col::bg);
    const int rowH = 46;
    int w = 760, h = 92 + 4 * rowH + 44, x = (W - w) / 2, y = (H - h) / 2;
    rect(x, y, w, h, col::panel);
    rect(x, y, w, 4, col::accent);
    for (int i = 0; i < 4; i++) {
        int ry = y + 34 + i * rowH;
        bool on = fun(i), cur = i == m_hmSel;
        if (cur) rect(x + 16, ry - 8, w - 32, rowH - 6, col::sel);
        int tw = text(x + 34, ry, 22, cur ? col::accent : col::text, strf("Функция %d", i + 1));
        if (i == 3) text(x + 34 + tw + 14, ry + 4, 17, col::dim, "67R mode");
        textRight(x + w - 34, ry, 22, on ? col::good : col::dim, on ? "вкл." : "выкл.");
    }
    textWrap(x + 34, y + 34 + 4 * rowH + 6, 16, col::dim,
             "thx: Dimasick-git, CHILDzzz (эт я), Айдару, моему коту Дрочиле и карте памяти смартбай на 64 гигабута", w - 68, 22);
    int bx = x + 34, by = y + h - 40;
    bx += hint(bx, by, "A", "вкл. / выкл.");
    hint(bx, by, "B", "закрыть");
}

std::string App::funEvent(const std::string& e) const {
    std::string s = e;
    if (fun(0)) {
        // Не каждая строка, а примерно каждая пятая — и всегда одна и та же (по самой строке), без мерцания.
        static const char* kPhrases[] = {"бурмалда", "жареный шашлык", "Астана", "ты зачем меня сломал?", "я заебался это крутить",
                                         "ОСТАНОВИСЬ!!!", "в ультре круче", "порвали туз", "ээ не придумал", "тут не работаю, я на перекуре",
                                         "у нас поминки фиксика, отъебись", "сам покрути, мне лень"};
        size_t hsh = std::hash<std::string>()(e);
        if (hsh % 5 == 0) return kPhrases[(hsh / 5) % (sizeof(kPhrases) / sizeof(kPhrases[0]))];
    }
    return funNums(s);
}

std::string App::funNums(const std::string& e) const {
    std::string s = e;
    if (fun(3)) {
        // «N МБ/с» → 228 Тб/с, «кадров N» → N + 228.
        for (size_t p = s.find(" МБ/с"); p != std::string::npos; p = s.find(" МБ/с", p + 1)) {
            size_t b = p;
            while (b > 0 && (isdigit((unsigned char)s[b - 1]) || s[b - 1] == '.')) b--;
            if (b == p) continue;
            s.replace(b, p + strlen(" МБ/с") - b, "228 Тб/с");
            p = b;
        }
        for (const std::string fr : {"кадров ", "TOTK "}) {
            for (size_t p = s.find(fr); p != std::string::npos; p = s.find(fr, p + 1)) {
                size_t b = p + fr.size(), q = b;
                while (q < s.size() && isdigit((unsigned char)s[q])) q++;
                if (q == b || (q < s.size() && s[q] == ' ' && s.compare(q, strlen(" пр"), " пр") == 0)) continue;  // «TOTK 3 пр.» — не кадры
                std::string n = strf("%ld", atol(s.substr(b, q - b).c_str()) + 228);
                s.replace(b, q - b, n);
            }
        }
    }
    return s;
}

int App::drawFunText(int x, int y, int size, u32 c, const std::string& raw, int maxW) {
    std::string s = funNums(raw);
    // Кадры TOTK в строке (архив конфигураций: «… · TOTK 2639 · …») — числом и надписью, как на экране TOTK.
    const std::string fr = "TOTK ";
    size_t p = fun(1) ? s.find(fr) : std::string::npos;
    while (p != std::string::npos) {
        size_t b = p + fr.size(), q = b;
        while (q < s.size() && isdigit((unsigned char)s[q])) q++;
        double v = q > b ? atof(s.substr(b, q - b).c_str()) : 0;
        bool runs = q < s.size() && s.compare(q, strlen(" пр"), " пр") == 0;
        if (q > b && !runs && v >= 2500 && v < 2900) {
            int w = text(x, y, size, c, s.substr(0, b), maxW);
            if (maxW > 0 && w >= maxW) return w;
            w += drawFrames(x + w, y, size, v, maxW > 0 ? maxW - w : 0);
            if (q < s.size() && (maxW <= 0 || w < maxW)) w += text(x + w, y, size, c, s.substr(q), maxW > 0 ? maxW - w : 0);
            return w;
        }
        p = s.find(fr, p + 1);
    }
    return text(x, y, size, c, s, maxW);
}

std::string App::bwStr(double mbs) const { return fun(3) ? std::string("228 Тб/с") : strf("%.0f МБ/с", mbs); }

double App::framesShown(double f) const { return fun(3) && f > 0 ? f + 228 : f; }

int App::drawFrames(int x, int y, int size, double frames, int maxW) {
    std::string n = strf("%.0f", frames);
    if (frames >= 2800) {
        // Радуга: у каждой буквы свой цвет, без повторов; раз в секунду — другая комбинация.
        static const u32 kRainbow[7] = {rgb(0xFF, 0x3B, 0x30), rgb(0xFF, 0x95, 0x00), rgb(0xFF, 0xD6, 0x0A), rgb(0x34, 0xC7, 0x59),
                                        rgb(0x5A, 0xC8, 0xFA), rgb(0x0A, 0x64, 0xFF), rgb(0xAF, 0x52, 0xDE)};
        int ord[7] = {0, 1, 2, 3, 4, 5, 6};
        u64 seed = nowMs() / 1000 * 2654435761ull + 12345;
        for (int i = 6; i > 0; i--) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            std::swap(ord[i], ord[(seed >> 33) % (u64)(i + 1)]);
        }
        std::string s = n + " · LGBTQ+";
        int cx = x, k = 0;
        for (size_t i = 0; i < s.size();) {
            size_t len = 1;
            unsigned char ch = (unsigned char)s[i];
            if (ch >= 0xF0) len = 4;
            else if (ch >= 0xE0) len = 3;
            else if (ch >= 0xC0) len = 2;
            std::string g = s.substr(i, len);
            bool colored = g != " " && g != "·";
            cx += text(cx, y, size, colored ? kRainbow[ord[k++ % 7]] : col::dim, g);
            i += len;
            if (cx - x > maxW) break;
        }
        return cx - x;
    }
    u32 c = col::text;
    const char* label = "мокроух";
    if (frames >= 2700) {
        c = rgb(0xFF, 0x7A, 0xC8);
        label = "хранитель семени";
    } else if (frames >= 2600) {
        c = rgb(0x5C, 0xC8, 0xFF);
        label = "клевая попка";
    }
    return text(x, y, size, c, n + " · " + label, maxW);
}

void App::drawPicker() {
    if (!m_pick.open) return;
    for (int yy = 0; yy < H; yy += 2) hline(0, yy, W, col::bg);
    int n = (int)m_pick.opts.size();
    const int rowH = 40;
    int visible = std::min(n, (H - 220) / rowH);  // длинный список — прокручивается за выбором
    int first = std::max(0, std::min(m_pick.sel - visible / 2, n - visible));
    int w = 820, h = 120 + visible * rowH, x = (W - w) / 2, y = (H - h) / 2;
    rect(x, y, w, h, col::panel);
    rect(x, y, w, 4, col::warn);
    drawFunText(x + 28, y + 20, 24, col::text, m_pick.title, w - 56);
    for (int i = first; i < first + visible; i++) {
        int ry = y + 66 + (i - first) * rowH;
        if (i == m_pick.sel) rect(x + 16, ry - 6, w - 40, rowH - 4, col::sel);
        drawFunText(x + 34, ry, 20, i == m_pick.sel ? col::accent : col::text, m_pick.opts[i], w - 80);
    }
    if (visible < n) {
        int bh = visible * rowH, th = std::max(16, bh * visible / n);
        rect(x + w - 16, y + 60, 4, bh, col::line);
        rect(x + w - 16, y + 60 + (bh - th) * first / std::max(1, n - visible), 4, th, col::accent);
    }
    int bx = x + 28, by = y + h - 40;
    bx += hint(bx, by, "A", "Выбрать");
    hint(bx, by, "B", "Отмена");
}

// Окна — в порядке открытия: верхним рисуется то, которому идут кнопки (topModal).
void App::drawDialog() {
    std::lock_guard<std::recursive_mutex> lk(m_modalMtx);
    if (topModal() == 1) {
        drawDialogBox();
        drawPicker();
    } else {
        drawPicker();
        drawDialogBox();
    }
}

void App::drawDialogBox() {
    if (!m_dlg.open) return;
    // Затемнение.
    for (int yy = 0; yy < H; yy += 2) hline(0, yy, W, col::bg);
    int w = m_dlg.danger ? 1000 : 820, h = m_dlg.danger ? 470 : 360, x = (W - w) / 2, y = (H - h) / 2;
    rect(x, y, w, h, col::panel);
    if (m_dlg.danger) {
        // Рамка в 6 px: такое не пролистнёшь.
        rect(x, y, w, 6, col::bad);
        rect(x, y + h - 6, w, 6, col::bad);
        rect(x, y, 6, h, col::bad);
        rect(x + w - 6, y, 6, h, col::bad);
        text(x + 28, y + 24, 30, col::bad, "⚠ " + m_dlg.title, w - 56);
        textScroll(x + 28, y + 84, 21, col::text, m_dlg.text, w - 56, 31, h - 84 - 64, &m_dlgScroll);
    } else {
        rect(x, y, w, 4, col::accent);
        text(x + 28, y + 22, 28, col::text, m_dlg.title, w - 56);
        textScroll(x + 28, y + 76, 20, col::text, m_dlg.text, w - 56, 30, h - 76 - 64, &m_dlgScroll);
    }
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
    static const char* steps[] = {"База", "Частоты", "Тайминги", "Оптимизация", "Проверка", "Кип", "TOTK"};
    const int ns = 7;
    int cur = 0;
    switch (t.phase) {
        case Phase::Baseline:
        case Phase::KipFirst: cur = 0; break;
        case Phase::FreqScan: cur = 1; break;
        case Phase::Seed:
        case Phase::Timing: cur = 2; break;
        case Phase::Optimize: cur = 3; break;
        case Phase::Verify: cur = 4; break;
        case Phase::Kip:
        case Phase::Refresh: cur = 5; break;
        case Phase::Game: cur = 6; break;
        case Phase::Done: cur = 7; break;
        default: cur = -1;
    }
    int sw = (w - 40) / ns;
    for (int i = 0; i < ns; i++) {
        int sx = x + 20 + i * sw;
        u32 c = i < cur ? col::good : (i == cur ? col::accent : col::line);
        rect(sx, y + 18, sw - 12, 6, c);
        // Подпись обрезается по своей ячейке — соседние больше не слипаются.
        text(sx, y + 34, 17, i == cur ? col::text : col::dim, strf("%d. %s", i + 1, steps[i]), sw - 14);
    }
    int yy = y + 90;

    if (!t.researchStatus.empty()) {
        card(x, yy, w, 120, "Исследование потока");
        text(x + 18, yy + 46, 20, col::accent, t.researchStatus, w - 36);
        text(x + 18, yy + 78, 16, col::dim, "X — пауза. Итог: config/ram-sweetspot/research.txt · подробно — logs/test.log", w - 36);
        return;
    }
    if (t.phase == Phase::Idle) {
        card(x, yy, w, h - 90, "Что будет сделано");
        std::string s =
            std::string("Режим работы с РАМ: ") + Settings::ramModeName(cfg().ramMode) + " (Настройки, первая строка). ↑↓ — прокрутка.\n" +
            "A — полный подбор (ниже) или, через Y в диалоге, подбор одной частоты: выберите её — прогоны пропускаются, "
            "старт со стока Ryazha-clk или гайда, дальше те же тайминги, оптимизация, проверка, кип и TOTK.\n" +
            "1. Базовый замер на стоковых 1600 МГц: поток и задержка памяти, оценка шума.\n"
            "2. Прогон частот на стоке Ryazha-clk: каждая ступень лестницы от 1331 МГц до верха — memtest и замер. Сбой (memtest, PLL drop, "
            "зависание) прогон не останавливает. Ступени медленнее предыдущей помечаются жёлтым и в доводку не идут.\n"
            "3. Второй прогон — на таймингах гайда Horizon-OC «Common» для вашего чипа, тоже по всему диапазону. Дальше работаем от "
            "прогона с более высокой стабильной частотой. На каждой ступени — 5 замеров, итог — медиана. Пики потока (дальше поток "
            "падает на 100 МБ/с и больше) тоже идут в доводку.\n"
            "4. Тайминги на лучших частотах по одной ручке: каждая ужесточается до первого сбоя (memtest или перезагрузка) — это пик. "
            "Режим считается от пика: максимум — пик (перепроверка), средний — пик −1, безопасный — пик −2. Внутри — лучшее по потоку.\n"
            "5. Оптимизация: по каждому таймингу найденное, ступень туже и до трёх мягче — найденное остаётся эталоном, другое берётся "
            "только по замеру А/Б (эталон и кандидат по очереди).\n"
            "6. Длинная проверка. Не прошла — отжим: сначала тайминги, потом ручки пропускной способности.\n" +
            (m_noWd ? std::string("7. Перенос в кип — пропускается: в этой версии Ryazha-clk сторожа нет, и зависание на загрузке кип не "
                                  "откатит. Найденные ручки остаются живыми в timings.ini.\n")
             : !cfg().kipStage ? std::string("7. Перенос в кип — выключен в настройках: найденные ручки остаются живыми в timings.ini.\n")
                               : std::string("7. Перенос ручек пропускной способности в кип Ryazha-clk: пакет разом, перезагрузка, memtest и "
                                             "замер; не прошёл — пакет делится пополам до виновника. После каждой перезагрузки снова "
                                             "откройте приложение.\n")) +
            "8. TOTK в лесу короков: перед запуском — предупреждение о десинхронизации АКБ, 3 прогона на кандидата, результат — среднее.";
        int iy = yy + h - 90 - 124;
        // Текст — только в своей области над нижним блоком; что не влезло — прокручивается.
        textScroll(x + 18, yy + 48, 19, col::text, s, w - 36, 27, iy - (yy + 48) - 10, &m_sel[S_Auto]);
        hline(x + 18, iy, w - 36, col::line);
        const int mw = w - 36;
        text(x + 18, iy + 10, 17, col::dim, strf("Чип: %s", m_chip.name.c_str()), mw);
        if (m_chip.preset)
            text(x + 18, iy + 34, 17, col::dim,
                 strf("Гайд Horizon-OC «Common» (прогон 2): %s · типично %u–%u МГц · VDD2 %d мВ, VDDQ %d мВ",
                      knobSetShort(presetToKnobs(m_chip.preset->common)).c_str(), m_chip.preset->minMhz, m_chip.preset->maxMhz, m_chip.preset->vdd2,
                      m_chip.preset->vddq),
                 mw);
        else
            text(x + 18, iy + 34, 17, col::dim, "Гайда Horizon-OC для этого чипа нет — второго прогона не будет", mw);
        text(x + 18, iy + 58, 17, col::dim,
             strf("Ступеней частоты у демона: %zu · память под тест: %s", m_ladder.size(), m_bufInfo.empty() ? "—" : m_bufInfo.c_str()), mw);
        text(x + 18, iy + 82, 17, m_appMode ? col::dim : col::warn,
             m_appMode ? "Запуск через игру — памяти для memtest достаточно" : "Запуск через альбом — памяти меньше; лучше через игру (удерживая R)", mw);
        if (m_noWd)
            text(x + 18, iy + 106, 17, col::warn, "Сторожа в этой версии Ryazha-clk нет: зависшую консоль перезагрузите сами, кип не трогается", mw);
        else if (m_wdOff)
            text(x + 18, iy + 106, 17, col::bad, "Сторож Ryazha-clk выключен: зависшая консоль не перезагрузится сама — включите его в оверлее", mw);
        return;
    }

    // Текущее состояние.
    card(x, yy, w, 170, "Сейчас");
    u32 sc = t.phase == Phase::Failed ? col::bad : col::text;
    text(x + 18, yy + 46, 22, sc, t.phase == Phase::Failed ? t.error : (t.status.empty() ? phaseName(t.phase) : t.status), w - 36);
    std::string noise = t.maxMode() ? strf("проход ±%.3f %%, шум А/Б до %.2f %%", t.passSigma() * 100, t.pairNoise() * 100)
                                    : strf("шум замера %.2f %%", t.eps * 100);
    text(x + 18, yy + 80, 18, col::dim,
         strf("Проб: %d · время: %s · %s · база: %s, %.1f нс", t.trials, elapsed(t.startedEpoch).c_str(), noise.c_str(), bwStr(t.base.bw()).c_str(),
              t.base.latNs));
    std::string line2, line3;
    if (t.phase == Phase::FreqScan) {
        line2 = t.lastOk >= 0 && t.lastOk < (int)t.ladder.size() ? strf("Самая высокая стабильная частота: %s МГц", mhzStr(t.ladder[t.lastOk]).c_str())
                                                                   : std::string("Стабильных частот пока нет");
        line3 = strf("Прогон %d (%s): ступень %d из %zu (%s … %s МГц)", std::min(t.scanPass, 1) + 1,
                     t.scanPass == 1 ? "тайминги гайда" : "сток Ryazha-clk", std::min(t.scanPos + 1, (int)t.coarse.size()), t.coarse.size(),
                     t.coarse.empty() ? "—" : mhzStr(t.ladder[t.coarse.front()]).c_str(), t.coarse.empty() ? "—" : mhzStr(t.ladder[t.coarse.back()]).c_str());
    } else if (t.phase == Phase::Optimize && t.tIdx < (int)t.targets.size()) {
        std::string key = t.optKey < (int)t.optKeys.size() ? t.optKeys[t.optKey] : std::string("—");
        line2 = strf("Оптимизация %s МГц: %s (%d из %zu), круг %d · выбранное: %s", mhzStr(t.targets[t.tIdx]).c_str(), key.c_str(), t.optKey + 1,
                     t.optKeys.size(), t.optPass + 1, bwStr(t.optBestBw).c_str());
        line3 = "Найденное — эталон, другое значение — только по замеру А/Б; итог сверяется с набором до этапа";
    } else if (t.phase == Phase::Refresh && !t.rfQueue.empty() && t.rfQueue[0] < (int)t.cands.size()) {
        line2 = strf("Финиш %s: tREFI %d, tRFC %d · пара %d из %zu", t.cands[t.rfQueue[0]].name().c_str(), t.rfPick.first, t.rfPick.second,
                     std::min(t.rfPos + 1, (int)t.rfPairs.size()), t.rfPairs.size());
        line3 = "tREFI и tRFC вместе на итоговом кипе: другая пара — только с приростом от 0,12 % по замеру А/Б и после длинной проверки";
    } else if (t.phase == Phase::Kip) {
        size_t queued = 0;
        for (auto& g : t.kipQueue) queued += g.size();
        line2 = strf("Кип: перезагрузок %d · принято %zu · на проверке %zu · в очереди %zu · отклонено %zu", t.kipReboots, t.kipAccepted.size(),
                     t.kipTesting.size(), queued, t.kipRejected.size());
        line3 = t.kipLog.empty() ? std::string("Ручки: ") + knobSetShort(t.kipVals) : t.kipLog.back();
    } else if ((t.phase == Phase::Seed || t.phase == Phase::Timing || t.phase == Phase::Verify) && t.tIdx < (int)t.targets.size()) {
        std::string knob = t.kIdx < (int)t.plan.size() ? t.plan[t.kIdx].key : std::string("—");
        line2 = strf("Частота %d из %zu: %s МГц · ручка %s (%d/%zu) · оценка %.3f", t.tIdx + 1, t.targets.size(), mhzStr(t.targets[t.tIdx]).c_str(),
                     knob.c_str(), std::min(t.kIdx + 1, (int)t.plan.size()), t.plan.size(), t.curScore);
        line3 = "Набор: " + knobSetCompact(t.cur);
    } else if (t.phase == Phase::Game || t.phase == Phase::Done) {
        line2 = strf("Кандидатов: %zu — проверьте их в TOTK на вкладке «Проверка в TOTK»", t.cands.size() ? t.cands.size() - 1 : 0);
    }
    if (m_safeRebootAt) {
        u64 now = nowMs();
        int left = m_safeRebootAt > now ? (int)((m_safeRebootAt - now + 999) / 1000) : 0;
        line3 = strf("Ryazha-clk в безопасном режиме — перезагружаю через %d с, подбор продолжится · B — отмена", left);
    } else if (m_autoResumeAt) {
        u64 now = nowMs();
        int left = m_autoResumeAt > now ? (int)((m_autoResumeAt - now + 999) / 1000) : 0;
        line3 = strf("Зависание записано — продолжаю сам через %d с · B — отмена", left);
    }
    // Коротко — справа от заголовка карточки; подробности — в строке ниже и в журнале.
    if (m_safe) textRight(x + w - 18, yy + 14, 16, col::bad, "Безопасный режим Ryazha-clk");
    else if (m_noWd) textRight(x + w - 18, yy + 14, 16, col::warn, "Без сторожа Ryazha-clk");
    else if (m_wdOff) textRight(x + w - 18, yy + 14, 16, col::bad, "Сторож Ryazha-clk выключен");
    text(x + 18, yy + 106, 18, col::text, line2, w - 36);
    text(x + 18, yy + 132, 18, (m_autoResumeAt || m_safeRebootAt) ? col::warn : col::accent, line3, w - 36);

    // Журнал.
    int ly = yy + 184;
    int lh = h - (ly - y);
    card(x, ly, w, lh, "Журнал");
    int rows = (lh - 50) / 26;
    int total = (int)t.events.size();
    int off = std::min(m_sel[S_Auto], std::max(0, total - rows));
    int start = std::max(0, total - rows - off);
    for (int i = 0; i < rows && start + i < total; i++) {
        const std::string e = funEvent(t.events[start + i]);
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
             strf("maxlastbreath.ini: Benchmark = %d (%s)%s", m_ucBench, benchmarkName(m_ucBench), m_ucBench == 4 ? "" : " — нужен 4, нажмите ZR"),
             w - 36);
    }
    if (m_haveLastRun)
        text(x + 18, y + 72, 18, col::dim,
             strf("Последний результат: %s — кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f", m_lastRun.location.c_str(),
                  framesShown(m_lastRun.frames), m_lastRun.avg, m_lastRun.low1, m_lastRun.low01),
             w - 36);
    else
        text(x + 18, y + 72, 18, col::dim, "Файла TOTKBench.txt / TOTKBenchmark.txt в корне карты пока нет", w - 36);
    if (m_tu.gamePendingCand >= 0 && m_tu.gamePendingCand < (int)m_tu.cands.size())
        text(x + 18, y + 94, 18, col::accent,
             "Ждём результат для «" + m_tu.cands[m_tu.gamePendingCand].name() + "»: пройдите бенчмарк, выйдите и откройте RamSweetSpot", w - 36);
    else
        text(x + 18, y + 94, 18, m_gpuSched == 0 ? col::warn : col::dim,
             strf("В игре — ЦПУ %d МГц и ГПУ %d МГц для всех кандидатов (Настройки)%s", cfg().cpuGameMhz, cfg().gpuGameMhz,
                  m_gpuSched == 0 ? " · планировщик ГПУ выкл." : ""),
             w - 36);

    int ty = y + 132;
    // Под таблицей — выбранный кандидат: все прогоны TOTK, среднее и сравнение с другими наборами той же частоты.
    const int detH = 140;
    int tableH = h - 132 - 70 - detH;
    card(x, ty, w, tableH, nullptr);
    if (m_tu.cands.empty()) {
        textWrap(x + 18, ty + 20, 20, col::dim,
                 "Кандидатов пока нет. Сначала пройдите автоподбор: после длинной проверки здесь появятся лучшие наборы "
                 "и строка «Как было» для сравнения.",
                 w - 36, 30);
        return;
    }
    // Колонки: каждая надпись ограничена своей шириной (на консоли шрифт шире, чем на ПК). Второе число каждой
    // колонки — второй строкой: задержка под потоком, доли кадров под кадрами.
    const int c0 = x + 18, c1 = x + 228, c2 = x + 308, c3 = x + 456, c4 = x + 714, c5 = x + 792;
    text(c0, ty + 12, 17, col::dim, "Кандидат", c1 - c0 - 10);
    text(c1, ty + 12, 17, col::dim, "Синт.", c2 - c1 - 8);
    text(c2, ty + 12, 17, col::dim, "Поток · задержка", c3 - c2 - 10);
    text(c3, ty + 12, 17, col::dim, "TOTK: кадры · ср. FPS", c4 - c3 - 10);
    text(c4, ty + 12, 17, col::dim, "Итог", c5 - c4 - 8);
    text(c5, ty + 12, 17, col::dim, "Статус", x + w - c5 - 12);
    hline(x + 14, ty + 38, w - 28, col::line);
    int best = m_tu.bestCandidate();
    int sweet = m_tu.sweetCandidate();
    int sel = std::min(m_sel[S_Totk], (int)m_tu.cands.size() - 1);
    int nRows = std::max(1, (tableH - 46) / 52), nc = (int)m_tu.cands.size();
    int first = std::max(0, std::min(sel - nRows + 1, nc - nRows));  // список прокручивается за выбором
    for (int i = first; i < nc && i < first + nRows; i++) {
        const Candidate& c = m_tu.cands[i];
        int ry = ty + 46 + (i - first) * 52;
        if ((int)i == sel) rect(x + 6, ry - 4, w - 12, 50, col::sel);
        if ((int)i == best) rect(x + 6, ry - 4, 4, 50, col::good);
        bool active = (int)i == m_tu.gameActiveCand;
        text(c0, ry, 19, active ? col::accent : col::text, active ? "● " + c.name() : c.name(), c1 - c0 - 10);
        text(c0, ry + 24, 15, col::dim, c.baseline ? "настройки до подбора" : knobSetCompact(c.set), c2 - c0 - 14);
        if (c.synth > 0) text(c1, ry, 19, col::text, strf("%.3f", c.synth), c2 - c1 - 8);
        if (c.b.valid) {
            text(c2, ry, 17, col::text, bwStr(c.b.bw()), c3 - c2 - 10);
            text(c2, ry + 24, 15, col::dim, strf("%.1f нс", c.b.latNs), c3 - c2 - 10);
        }
        if (!c.runs.empty()) {
            double f = 0, a = 0, l1 = 0, l01 = 0;
            Tuner::runsMedian(c.runs, &f, &a, &l1, &l01);  // медиана прогонов, как в оценке
            f = framesShown(f);
            int done = m_tu.gameRunsDone(c);
            if (fun(1) && done >= cfg().totkRuns && f >= 2500 && f < 2900) {
                drawFrames(c3, ry, 17, f, c4 - c3 - 10);
                text(c3, ry + 24, 15, col::dim, strf("%.0f FPS · 1%% %.0f · 0.1%% %.0f · %d/%d", a, l1, l01, done, cfg().totkRuns), c4 - c3 - 10);
            } else {
                text(c3, ry, 17, col::text, strf("%.0f кадров · %.0f FPS", f, a), c4 - c3 - 10);
                text(c3, ry + 24, 15, done >= cfg().totkRuns ? col::dim : col::warn,
                     strf("1%% %.0f · 0.1%% %.0f · прогонов %d/%d", l1, l01, done, cfg().totkRuns), c4 - c3 - 10);
            }
        }
        if (c.combined > 0) text(c4, ry, 19, (int)i == best ? col::good : col::text, strf("%.3f", c.combined), c5 - c4 - 8);
        std::string st;
        u32 sc = col::dim;
        if ((int)i == m_tu.gamePendingCand) {
            st = "ждём бенчмарк";
            sc = col::accent;
        } else if (c.doubtful()) {
            st = strf("сомнительный: сбоев %d из %d", c.crashes, std::max(c.attempts, c.crashes));
            sc = col::bad;
        } else if (c.game == 2) {
            st = "сбой: " + c.note;
            sc = col::bad;
        } else if (c.game == 1 && m_tu.gameRunsDone(c) < cfg().totkRuns) {
            st = strf("ещё %d прогона", cfg().totkRuns - m_tu.gameRunsDone(c));
            sc = col::warn;
        } else if ((int)i == sweet) {
            st = c.issues() ? SWEET_MARK ", были замечания" : SWEET_MARK;
            sc = col::warn;
        } else if (c.game == 1) {
            st = c.issues() ? "проверен, были замечания" : "проверен";
            sc = c.issues() ? col::warn : col::good;
        } else {
            st = "не проверен";
        }
        text(c5, ry, 17, sc, st, x + w - c5 - 12);
        std::string sub;
        if (active) sub = "применён сейчас";
        if (c.crashes) sub += (sub.empty() ? "" : " · ") + strf("сбоев %d из %d", c.crashes, std::max(c.attempts, c.crashes));
        if (c.stutters) sub += (sub.empty() ? "" : " · ") + strf("статтеры %d", c.stutters);
        if (c.artifacts) sub += (sub.empty() ? "" : " · ") + strf("артефакты %d", c.artifacts);
        if (c.longLoads) sub += (sub.empty() ? "" : " · ") + strf("загрузки %d", c.longLoads);
        if (!sub.empty()) text(c5, ry + 24, 15, active ? col::accent : col::dim, sub, x + w - c5 - 12);
    }
    if (nc > nRows) {
        int bh = nRows * 52, th = std::max(16, bh * nRows / nc);
        rect(x + w - 8, ty + 46, 4, bh, col::line);
        rect(x + w - 8, ty + 46 + (bh - th) * first / std::max(1, nc - nRows), 4, th, col::accent);
    }
    drawTotkDetails(x, ty + tableH + 8, w, detH - 8, sel);
    int ry = y + h - 60;
    card(x, ry, w, 60);
    // Свитспот — справа: самая выгодная частота по потоку и кадрам TOTK (если сравнивать есть что — от двух частот).
    int sw = 0;
    if (sweet >= 0 && sweet != best) {
        const Candidate& s = m_tu.cands[sweet];
        double f = 0;
        Tuner::runsMedian(s.runs, &f, nullptr, nullptr, nullptr);
        std::string l1 = SWEET_MARK ": " + s.name(), l2 = bwStr(s.b.bw()) + strf(" · %.0f кадров", framesShown(f));
        textRight(x + w - 18, ry + 8, 19, col::warn, l1);
        textRight(x + w - 18, ry + 34, 16, col::dim, l2);
        sw = std::max(textWidth(19, l1), textWidth(16, l2)) + 24;
    }
    if (best >= 0) {
        const Candidate& b = m_tu.cands[best];
        int tx = x + 18 + text(x + 18, ry + 8, 19, col::good, "Рекомендация: " + b.name() + (b.game == 1 ? "" : " (без проверки в игре)"), w - 36 - sw);
        if (sweet == best) text(tx, ry + 8, 19, col::warn, " · " SWEET_MARK);
        text(x + 18, ry + 34, 16, col::dim, knobSetCompact(b.set), w - 36 - sw);
    } else {
        text(x + 18, ry + 18, 19, col::dim, "Рекомендации пока нет", w - 36 - sw);
    }
}

void App::drawTotkDetails(int x, int y, int w, int h, int sel) {
    card(x, y, w, h);
    if (sel < 0 || sel >= (int)m_tu.cands.size()) return;
    const Candidate& c = m_tu.cands[sel];
    int ly = y + 8;
    auto meanFrames = [](const std::vector<TotkRun>& runs) {
        double s = 0;
        for (auto& r : runs) s += r.frames;
        return runs.empty() ? 0.0 : s / runs.size();
    };
    double mine = meanFrames(c.runs);
    if (c.runs.empty()) {
        text(x + 18, ly, 17, col::dim, c.name() + ": прогонов TOTK пока нет", w - 36);
        ly += 48;
    } else {
        double fps = 0, med = 0;
        for (auto& r : c.runs) fps += r.avg;
        Tuner::runsMedian(c.runs, &med, nullptr, nullptr, nullptr);
        text(x + 18, ly, 17, col::text,
             strf("%s — прогонов TOTK %zu из %zu · среднее %.0f кадров (%.1f FPS) · медиана %.0f", c.name().c_str(), c.runs.size(), Tuner::kMaxTotkRuns,
                  framesShown(mine), fps / c.runs.size(), framesShown(med)),
             w - 36);
        // Все прогоны по порядку; «*» — не лес короков (в итог идут только они). Больше всех кадров на этой частоте —
        // зелёным, меньше всех — красным.
        double hi = 0, lo = 1e18;
        for (auto& r : c.runs) {
            hi = std::max(hi, r.frames);
            lo = std::min(lo, r.frames);
        }
        int rx = x + 18 + text(x + 18, ly + 22, 16, col::dim, "Прогоны: ");
        for (size_t i = 0; i < c.runs.size(); i++) {
            const TotkRun& r = c.runs[i];
            std::string v = strf("%.0f", framesShown(r.frames)) + (r.korok() ? "" : "*");
            u32 vc = hi > lo && r.frames == hi ? col::good : hi > lo && r.frames == lo ? col::bad : col::text;
            if (i) rx += text(rx, ly + 22, 16, col::dim, " · ");
            if (rx + textWidth(16, v) > x + w - 18) break;
            rx += text(rx, ly + 22, 16, vc, v);
        }
        ly += 48;
    }
    if (c.baseline) return;

    // Та же частота: другие кандидаты и записи архива — поток, кадры TOTK к этому набору и чем наборы отличаются.
    auto delta = [](double other, double me) { return me > 0 && other > 0 ? strf(" (%+.1f %%)", (other / me - 1) * 100) : std::string(); };
    auto diff = [&](const KnobSet& o) {
        std::string d;
        std::set<std::string> keys;
        for (auto& kv : o) keys.insert(kv.first);
        for (auto& kv : c.set) keys.insert(kv.first);
        for (auto& k : keys) {
            auto a = o.find(k), b = c.set.find(k);
            int va = a != o.end() ? a->second : 0, vb = b != c.set.end() ? b->second : 0;
            if (va != vb) d += (d.empty() ? "" : ", ") + strf("%s %d", k.c_str(), va);
        }
        return d.empty() ? std::string("тот же набор") : "у него: " + d;
    };
    std::vector<std::string> lines;
    for (size_t i = 0; i < m_tu.cands.size(); i++) {
        const Candidate& o = m_tu.cands[i];
        if ((int)i == sel || o.baseline || o.hz != c.hz) continue;
        double of = meanFrames(o.runs);
        std::string l = o.name() + ": " + (o.b.valid ? bwStr(o.b.bw()) + delta(o.b.bw(), c.b.bw()) : std::string("поток —"));
        l += of > 0 ? strf(", TOTK %.0f", framesShown(of)) + delta(of, mine) : std::string(", TOTK —");
        lines.push_back(l + " · " + diff(o.set));
    }
    if (nowMs() - m_archAt > 3000) {
        m_archCache = archLoad();
        m_archAt = nowMs();
    }
    int archSw = archSweet(m_archCache, m_chip.dramId, cfg().wGame / 100.0);
    for (int ai = (int)m_archCache.size() - 1; ai >= 0; ai--) {
        const ArchEntry& e = m_archCache[ai];
        if (e.hz != c.hz || (e.dram >= 0 && e.dram != m_chip.dramId)) continue;
        std::string l = std::string(ai == archSw ? SWEET_MARK " " : "") + "архив " + (e.name.empty() ? std::string("—") : e.name) + ": " +
                        (e.bw > 0 ? bwStr(e.bw) + delta(e.bw, c.b.bw()) : "поток —");
        l += e.frames > 0 ? strf(", TOTK %.0f", framesShown(e.frames)) + delta(e.frames, mine) : std::string(", TOTK —");
        lines.push_back(l + " · " + diff(e.set));
    }
    text(x + 18, ly, 16, col::dim,
         lines.empty() ? strf("На %s МГц других наборов нет — ни среди кандидатов, ни в архиве", mhzStr(c.hz).c_str())
                       : strf("Та же частота, %s МГц — к этому набору:", mhzStr(c.hz).c_str()),
         w - 36);
    ly += 20;
    const int maxLines = 3;
    for (size_t i = 0; i < lines.size() && (int)i < maxLines; i++) {
        bool last = (int)i == maxLines - 1 && lines.size() > (size_t)maxLines;
        text(x + 18, ly, 16, col::text, last ? strf("… и ещё %zu", lines.size() - i) : lines[i], w - 36);
        ly += 20;
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
    // Что прогоны знают об этой частоте.
    std::string scan;
    {
        std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
        for (auto& p : m_tu.points) {
            if (p.hz != hz) continue;
            bool peak = false;
            for (auto& pk : m_tu.bandwidthPeaks(p.pass))
                if (pk.first == hz) peak = true;
            scan += (scan.empty() ? "" : " · ") + std::string(p.pass ? "гайд: " : "сток: ") +
                    (p.status == 1 ? bwStr(p.b.bw()) + (peak ? ", пик" : "") : p.status == 3 ? std::string("медленнее") : std::string("сбой"));
        }
    }
    text(x + 18, y + 74, 15, scan.empty() ? col::dim : col::info, scan.empty() ? "прогонами не проверялась" : "Прогоны: " + scan, lw - 36);
    hline(x + 14, y + 96, lw - 28, col::line);
    int rows = (h - 116) / 30;
    int first = std::max(0, std::min(m_manKnob - rows / 2, (int)knobs.size() - rows));
    for (int i = 0; i < rows && first + i < (int)knobs.size(); i++) {
        int k = first + i;
        const KnobDef& d = knobs[k];
        int ry = y + 106 + i * 30;
        if (k == m_manKnob) rect(x + 6, ry - 3, lw - 12, 29, col::sel);
        text(x + 18, ry, 18, col::text, d.label, 230);
        auto it = m_manSet.find(d.key);
        std::string v = it == m_manSet.end() ? (d.absentIsZero ? std::string("0 (сток)") : std::string("общ. (кип)"))
                                             : strf("%d · %s", it->second, knobValueText(d, it->second).c_str());
        // Найденный предел на этой частоте: туже него уже был сбой (реестр нестабильных и текущий подбор).
        int bound = 0;
        bool haveBound = false;
        {
            std::lock_guard<std::recursive_mutex> lk(m_tu.mtx);
            haveBound = hz && m_tu.unstableBound(hz, d.key, &bound);
            bool cur = m_tu.tIdx < (int)m_tu.targets.size() && m_tu.targets[m_tu.tIdx] == hz && m_tu.limits.count(d.key);
            if (cur) {
                int l = m_tu.limits.at(d.key);
                if (!haveBound || (d.order == KnobOrder::Up ? l < bound : l > bound)) bound = l;
                haveBound = true;
            }
        }
        bool over = haveBound && it != m_manSet.end() &&
                    (d.order == KnobOrder::Up ? it->second >= bound : d.order == KnobOrder::Down ? it->second <= bound : false);
        u32 vc = over ? col::bad : (it == m_manSet.end() ? col::dim : col::accent);
        text(x + 260, ry, 18, vc, v, lw - 260 - (haveBound ? 110 : 18));
        if (haveBound) textRight(x + lw - 18, ry + 2, 15, over ? col::bad : col::warn, strf("сбой с %d", bound));
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
        // Изменение к прошлому замеру — по правому краю карточки: с отступом от значения оно вылезало за рамку.
        auto rowv = [&](int ry, const char* name, double v, double pv, const char* unit, bool lowerBetter) {
            std::string pct;
            u32 pc = col::text;
            if (pv > 0) {
                double d = (v - pv) / pv * 100;
                bool good = lowerBetter ? d < 0 : d > 0;
                if (std::fabs(d) >= 0.05) {
                    pct = strf("%+.2f %%", d);
                    pc = good ? col::good : col::bad;
                }
            }
            int pw = pct.empty() ? 0 : textWidth(18, pct) + 12;
            text(rx + 18, ry, 19, col::dim, name, 150);
            text(rx + 172, ry, 19, col::text, fun(3) && std::string(unit) == "МБ/с" ? bwStr(v) : strf("%.1f %s", v, unit), rw - 172 - 18 - pw);
            if (!pct.empty()) textRight(rx + rw - 18, ry + 1, 18, pc, pct);
        };
        rowv(y + 46, "Чтение", last.readMBs, prev.valid ? prev.readMBs : 0, "МБ/с", false);
        rowv(y + 76, "Запись", last.writeMBs, prev.valid ? prev.writeMBs : 0, "МБ/с", false);
        rowv(y + 106, "Копирование", last.copyMBs, prev.valid ? prev.copyMBs : 0, "МБ/с", false);
        rowv(y + 136, "Задержка", last.latNs, prev.valid ? prev.latNs : 0, "нс", true);
        rowv(y + 166, "Задержка ×2", last.lat2Ns, prev.valid ? prev.lat2Ns : 0, "нс", true);
        if (m_tu.base.valid) text(rx + 18, y + 200, 17, col::dim, strf("Оценка относительно базы 1600 МГц: %.3f", m_tu.scoreOf(last)), rw - 36);
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

void App::drawHistory(int x, int y, int w, int h) {
    card(x, y, w, 270, "За всё время: поток по частотам");
    if (m_hist.empty()) {
        text(x + 18, y + 50, 18, col::dim, "История пуста — она копится с каждым прогоном частот, доводкой и прогоном TOTK", w - 36);
        card(x, y + 284, w, h - 284, "Частоты за всё время");
        return;
    }
    // Столбец — лучший поток на частоте (после доводки таймингов, если была), белая риска — медиана прогона
    // на общих таймингах, цвет — доля стабильных проверок. Оранжевые точки — кадры TOTK.
    int gx = x + 60, gy = y + 50, gw = w - 90, gh = 180;
    double maxBw = 1, maxFr = 0;
    for (auto& f : m_hist) {
        maxBw = std::max(maxBw, f.best());
        maxFr = std::max(maxFr, f.totkFrames);
    }
    hline(gx, gy + gh, gw, col::line);
    int n = (int)m_hist.size();
    int slot = gw / std::max(1, n);
    int bw = std::max(3, std::min(36, slot - 4));
    int labelEvery = std::max(1, (64 + slot - 1) / std::max(1, slot));
    int px = -1, py = -1;
    for (int i = 0; i < n; i++) {
        const HistFreq& f = m_hist[i];
        int cx = gx + (int)((i + 0.5) * gw / n);
        u32 c = f.stability() >= 0.9 ? col::good : (f.stability() >= 0.6 ? col::warn : col::bad);
        if (f.best() > 0) {
            int bh = (int)(gh * f.best() / maxBw);
            rect(cx - bw / 2, gy + gh - bh, bw, bh, c);
            if (f.scanMedian > 0) rect(cx - bw / 2, gy + gh - (int)(gh * f.scanMedian / maxBw), bw, 3, col::text);
        } else {
            rect(cx - bw / 2, gy + gh - 8, bw, 8, col::bad);
        }
        if (i == m_histSel) rect(cx - bw / 2, gy + gh + 2, bw, 3, col::accent);
        if (f.totkRuns && maxFr > 0) {
            int ly = gy + gh - (int)(gh * 0.95 * f.totkFrames / maxFr);
            if (px >= 0) line(px, py, cx, ly, col::accent);
            rect(cx - 3, ly - 3, 7, 7, col::accent);
            px = cx;
            py = ly;
        }
        if (i % labelEvery == 0) textCenter(cx, gy + gh + 6, 14, col::dim, strf("%.1f", f.mhz));
    }
    text(x + 14, gy - 4, 14, col::dim, strf("%.0f", maxBw));
    textRight(x + w - 18, y + 12, 14, col::dim, "цвет — стабильность · риска — медиана прогона · точки — TOTK");

    // Таблица: по частоте — проверки, стабильность, поток прогона → с таймингами, TOTK.
    int ty = y + 284;
    int tw = 640;
    card(x, ty, tw, h - 284, strf("Частоты за всё время · записей %zu", m_histRows).c_str());
    const int c0 = x + 18, c1 = x + 110, c2 = x + 230, c3 = x + 318, c4 = x + 530;
    text(c0, ty + 44, 15, col::dim, "МГц", c1 - c0 - 6);
    text(c1, ty + 44, 15, col::dim, "проверок", c2 - c1 - 6);
    text(c2, ty + 44, 15, col::dim, "стабильно", c3 - c2 - 6);
    text(c3, ty + 44, 15, col::dim, "поток: прогон → с таймингами", c4 - c3 - 6);
    text(c4, ty + 44, 15, col::dim, "TOTK", x + tw - c4 - 12);
    int rows = (h - 284 - 78) / 26;
    int first = std::max(0, std::min(m_histSel - rows / 2, n - rows));
    for (int i = 0; i < rows && first + i < n; i++) {
        const HistFreq& f = m_hist[first + i];
        int ry = ty + 70 + i * 26;
        if (first + i == m_histSel) rect(x + 6, ry - 3, tw - 12, 25, col::sel);
        u32 c = f.stability() >= 0.9 ? col::good : (f.stability() >= 0.6 ? col::warn : col::bad);
        text(c0, ry, 17, col::text, strf("%.1f", f.mhz), c1 - c0 - 6);
        text(c1, ry, 17, col::dim, strf("%d", f.tests), c2 - c1 - 6);
        text(c2, ry, 17, c, strf("%.0f %%%s", f.stability() * 100, f.hang ? strf(" · завис. %d", f.hang).c_str() : ""), c3 - c2 - 6);
        std::string bwTxt = f.scanMedian > 0 ? strf("%.0f", f.scanMedian) : std::string("—");
        if (f.tunedBest > 0)
            bwTxt += strf(" → %.0f", f.tunedBest) + (f.scanMedian > 0 ? strf(" (%+.1f %%)", (f.tunedBest / f.scanMedian - 1) * 100) : std::string());
        text(c3, ry, 17, col::text, bwTxt, c4 - c3 - 6);
        text(c4, ry, 17, f.totkRuns ? col::accent : col::dim, f.totkRuns ? strf("%.0f · %d пр.", f.totkFrames, f.totkRuns) : std::string("—"),
             x + tw - c4 - 12);
    }
    // Выводы: лучшая по потоку среди надёжных, лучшая в TOTK, база.
    int bx = x + tw + 14, bw2 = w - tw - 14;
    card(bx, ty, bw2, h - 284, "Что видно");
    const HistFreq* fast = nullptr;
    const HistFreq* game = nullptr;
    for (auto& f : m_hist) {
        if (f.tests >= 3 && f.stability() >= 0.9 && (!fast || f.best() > fast->best())) fast = &f;
        if (f.totkRuns && (!game || f.totkFrames > game->totkFrames)) game = &f;
    }
    int yy = ty + 48;
    if (fast) {
        text(bx + 18, yy, 17, col::dim, "Быстрее всех из надёжных (от 90 %):", bw2 - 36);
        text(bx + 18, yy + 24, 20, col::good, strf("%.1f МГц — ", fast->mhz) + bwStr(fast->best()), bw2 - 36);
        yy += 62;
    }
    if (game) {
        text(bx + 18, yy, 17, col::dim, "Лучшая в TOTK:", bw2 - 36);
        std::string g = strf("%.1f МГц — %.0f кадров", game->mhz, framesShown(game->totkFrames));
        if (m_histBase.totkRuns && m_histBase.totkFrames > 0) g += strf(" (%+.0f %%)", (game->totkFrames / m_histBase.totkFrames - 1) * 100);
        text(bx + 18, yy + 24, 20, col::accent, g, bw2 - 36);
        yy += 62;
    }
    if (m_histBase.totkRuns)
        text(bx + 18, yy, 16, col::dim, strf("«Как было» в TOTK: %.0f кадров (%d пр.)", framesShown(m_histBase.totkFrames), m_histBase.totkRuns), bw2 - 36);
    if (m_histSel < n && !m_hist[m_histSel].bestSet.empty()) {
        int setScroll = 0;
        text(bx + 18, ty + h - 284 - 92, 15, col::dim, strf("Лучший набор на %.1f МГц:", m_hist[m_histSel].mhz), bw2 - 36);
        textScroll(bx + 18, ty + h - 284 - 68, 15, col::text, m_hist[m_histSel].bestSet, bw2 - 36, 20, 40, &setScroll);
    }
    text(bx + 18, ty + h - 284 - 24, 14, col::dim, "Сырые данные: config/ram-sweetspot/history.csv", bw2 - 36);
}

void App::drawResults(int x, int y, int w, int h) {
    if (m_resAll) {
        drawHistory(x, y, w, h);
        return;
    }
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
        // Одна позиция на частоту: сток и гайд — парой столбцов (зелёный / голубой), линия задержки — по стоку.
        std::vector<u32> hzs;
        bool twoPass = false;
        for (auto& p : t.points) {
            if (std::find(hzs.begin(), hzs.end(), p.hz) == hzs.end()) hzs.push_back(p.hz);
            if (p.pass) twoPass = true;
        }
        int n = (int)hzs.size();
        int slot = gw / std::max(1, n);
        int bw = std::max(3, std::min(36, (twoPass ? slot / 2 : slot) - 3));
        auto pk0 = t.bandwidthPeaks(0), pk1 = t.bandwidthPeaks(1);
        auto isPeak = [&](const FreqPoint& p) {
            for (auto& x : p.pass ? pk1 : pk0)
                if (x.first == p.hz) return true;
            return false;
        };
        int labelEvery = std::max(1, (64 + slot - 1) / std::max(1, slot));  // подписи не ближе 64 px
        int px = -1, py = -1;
        for (int i = 0; i < n; i++) {
            int cx = gx + (int)((i + 0.5) * gw / n);
            for (auto& p : t.points) {
                if (p.hz != hzs[i]) continue;
                int bx = twoPass ? (p.pass ? cx : cx - bw) : cx - bw / 2;
                if (p.b.valid) {
                    int bh = (int)(gh * p.b.bw() / maxBw);
                    rect(bx, gy + gh - bh, bw, bh, p.status == 1 ? (p.pass ? col::info : col::good) : (p.status == 3 ? col::warn : col::bad));
                    if (isPeak(p)) rect(bx, gy + gh - bh - 6, bw, 4, col::accent);  // пик потока — оранжевая шапка
                    if (p.pass == 0) {
                        double span = std::max(1.0, maxLat - minLat);
                        int ly = gy + 10 + (int)((p.b.latNs - minLat) / span * (gh - 40));
                        if (px >= 0) line(px, py, cx, ly, col::dim);
                        rect(cx - 2, ly - 2, 5, 5, col::text);
                        px = cx;
                        py = ly;
                    }
                } else {
                    rect(bx, gy + gh - 8, bw, 8, col::bad);
                }
            }
            if (i % labelEvery == 0) textCenter(cx, gy + gh + 6, 14, col::dim, mhzStr(hzs[i]));
        }
        text(x + 14, gy - 4, 14, col::dim, strf("%.0f", maxBw));
        if (twoPass) text(x + w - 380, y + 12, 14, col::dim, "зелёный — сток · голубой — гайд · оранжевый — пик");
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
        text(x + 18, ry, 17, p.pass ? col::info : col::text, mhzStr(p.hz) + (p.pass ? " гайд" : " МГц"));
        bool peak = false;
        for (auto& pk : t.bandwidthPeaks(p.pass))
            if (pk.first == p.hz) peak = true;
        if (p.status == 1)
            text(x + 150, ry, 17, peak ? col::accent : col::good,
                 bwStr(p.b.bw()) + strf(" · %.1f нс · %.3f%s", p.b.latNs, p.score, peak ? " · пик" : ""), tw - 170);
        else if (p.status == 3)
            text(x + 150, ry, 17, col::warn, bwStr(p.b.bw()) + strf(" · %.1f нс · медленнее предыдущей", p.b.latNs), tw - 170);
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
    int setScroll = 0;  // не больше двух строк; полный набор — в отчёте
    textScroll(bx + 18, ty + 84, 18, col::text, knobSetCompact(c.set), bw2 - 36, 26, 52, &setScroll);
    if (c.b.valid)
        text(bx + 18, ty + 150, 18, col::dim, "Поток " + bwStr(c.b.bw()) + strf(" · задержка %.1f нс · оценка %.3f", c.b.latNs, c.synth), bw2 - 36);
    text(bx + 18, ty + 178, 18, c.game == 1 ? col::good : col::warn,
         c.game == 1 ? strf("Проверен в TOTK, итоговая оценка %.3f", c.combined) : std::string("Ещё не проверен в TOTK"));
    int yy = ty + 210;
    if (t.phase == Phase::Done) {
        text(bx + 18, yy, 18, col::good, "Записан в timings.ini");
        yy += 26;
    }
    // Свитспот: самая выгодная частота по потоку и кадрам TOTK — от двух проверенных частот. Он же итог — пометка у частоты.
    int sweet = t.sweetCandidate();
    if (sweet == best) {
        textRight(bx + bw2 - 18, ty + 54, 18, col::warn, SWEET_MARK);
    } else if (sweet >= 0) {
        const Candidate& s = t.cands[sweet];
        double f = 0;
        Tuner::runsMedian(s.runs, &f, nullptr, nullptr, nullptr);
        text(bx + 18, yy, 17, col::warn, SWEET_MARK ": " + s.name() + strf(" · %.0f кадров", framesShown(f)), bw2 - 36);
        yy += 26;
    }
    if (yy + 18 <= ty + h - 284 - 40) text(bx + 18, ty + h - 284 - 40, 15, col::dim, "Отчёт: config/ram-sweetspot/report.txt", bw2 - 36);
}

// ============================================================== Настройки

void App::drawSettings(int x, int y, int w, int h) {
    (void)h;
    static const char* labels[] = {
        "Режим работы с РАМ", "Вес пропускной способности", "Вес TOTK в итоговой оценке", "memtest на каждую пробу", "Длинная проверка финала", "Повторов замера",
        "Запас стабильности", "Терпение", "Низ прогона частот", "Верх прогона частот", "Сбоев подряд до остановки", "Частот для доводки таймингов",
        "Порог температуры SoC", "Стартовать с пресета чипа", "tREFI 11700 нс", "Расширенные ручки", "Подтверждение по логу демона",
        "Запускать TOTK автоматически", "ЦПУ при подборе РАМ", "ЦПУ в бенчмарке TOTK", "ГПУ в бенчмарке TOTK",
        "Перенос ручек в кип", "memtest после загрузки", "Прогонов TOTK на кандидата", "Оптимизация таймингов", "Второй прогон на гайде",
        "Пик потока: падение дальше", "Пиков потока в доводку", "Замеров на ступень прогона", "Зависаний подряд до конца прогона",
        "Кип сначала (тест)", "Повторов исследования", "Продолжать после зависания", "Замеров на пробу таймингов", "Финиш: tREFI × tRFC (тест)"};
    static const char* hints[] = {
        "A / ←→ — безопасный, средний, максимум", "цель — поток; 100 = задержка не в счёт, 50 = поровну", "остальное — синтетика", "секунд; больше — надёжнее и дольше", "секунд на каждую итоговую частоту",
        "берётся лучший результат", "на сколько ступеней отступить от сбоя", "сколько ступеней без прироста пробовать дальше",
        "с этой ступени лестницы начинается прогон", "0 = до верха лестницы Ryazha-clk", "0 = пройти весь диапазон, сбой не останавливает",
        "сколько лучших частот доводить",
        "выше — пауза на остывание", "Common-тайминги из гайда Horizon-OC", "риск порчи данных при нагреве", "арбитр, растяжки шины, tRPab, tW2P",
        "ждать «тайминги применены живьём»", "после применения кандидата", "фиксируется на время memtest и замеров",
        "одинаково для всех кандидатов", "одинаково для всех кандидатов", "пропускная способность; с перезагрузками",
        "секунд на каждой частоте кандидата", "результат — среднее", "после пределов — лучший поток, не самые тугие",
        "после стока — тайминги гайда Horizon-OC для чипа", "МБ/с: на сколько медленнее следующая ступень",
        "частоты с пиком потока — тоже доводятся", "итог — усечённое среднее (с 3 замеров)", "выше память не берёт; 0 = не останавливать",
        "тайминги гайда в кип до прогонов (перезагрузка)", "Сервис → Исследование потока", "сам через 5 с после запуска, B — отмена",
        "в «максимуме» — отбор, спорное подтверждает замер А/Б", "может ухудшить стабильность и кадры · по умолчанию — только «максимум»"};
    const Settings& c = m_tu.cfg;
    auto onoff = [](bool b) { return std::string(b ? "вкл." : "выкл."); };
    std::string vals[] = {
        Settings::ramModeName(c.ramMode), strf("%d %% / задержка %d %%", c.wBw, 100 - c.wBw), strf("%d %%", c.wGame), strf("%d с", c.quickSec), strf("%d с", c.verifySec),
        strf("%d", c.benchPasses), strf("%d ступ.", c.margin), strf("%d", c.patience), strf("%d МГц", c.minMhz),
        c.maxMhz ? strf("%d МГц", c.maxMhz) : std::string("верх лестницы"), c.scanFailStop ? strf("%d", c.scanFailStop) : std::string("не останавливать"),
        strf("%d", c.numTargets), strf("%d °C", c.tempLimit), onoff(c.usePreset), onoff(c.allowTrefiHigh),
        onoff(c.advanced), onoff(c.useDaemonLog), onoff(c.autoLaunch),
        c.cpuTuneMhz ? strf("%d МГц", c.cpuTuneMhz) : std::string("не фиксировать"),
        c.cpuGameMhz ? strf("%d МГц", c.cpuGameMhz) : std::string("не фиксировать"),
        c.gpuGameMhz ? strf("%d МГц", c.gpuGameMhz) : std::string("не фиксировать"), onoff(c.kipStage), strf("%d с", c.kipCheckSec),
        strf("%d", c.totkRuns), onoff(c.optimize), onoff(c.scanGuide), strf("%d МБ/с", c.peakDropMb), strf("%d", c.peakTargets),
        strf("%d", c.scanPasses), c.scanHangStop ? strf("%d", c.scanHangStop) : std::string("не останавливать"),
        onoff(c.kipFirst), strf("%d", c.researchRuns), onoff(c.autoResume), strf("%d", c.timingPasses), onoff(c.refreshTune)};
    const int n = 35;
    const int rh = 24;
    // Список прокручивается вслед за выбором: строки не сжимаются и не наползают на подпись снизу.
    const int visible = std::min(n, (h - 20 - 30) / rh);
    int first = std::max(0, std::min(m_sel[S_Settings] - visible / 2, n - visible));
    card(x, y, w, 20 + visible * rh);
    if (visible < n) {
        int bh = visible * rh, th = std::max(16, bh * visible / n);
        rect(x + w - 8, y + 10, 4, bh, col::line);
        rect(x + w - 8, y + 10 + (bh - th) * first / std::max(1, n - visible), 4, th, col::accent);
    }
    for (int i = first; i < first + visible; i++) {
        int ry = y + 10 + (i - first) * rh;
        bool on = i == m_sel[S_Settings];
        if (on) rect(x + 6, ry - 3, w - 12, rh - 1, col::sel);
        u32 lc = i == 0 ? (c.ramMode == Settings::ModeSafe ? col::good : c.ramMode == Settings::ModeMedium ? col::info : col::warn) : col::text;
        text(x + 18, ry, 16, lc, labels[i]);
        // Без сторожа (Ryazha-clk 3.5.5) этапы с записью в кип пропускаются, что бы ни стояло в настройке.
        bool noKip = m_noWd && (i == 21 || i == 22 || i == 30);
        text(x + 420, ry + 2, 14, noKip ? col::warn : col::dim, noKip ? "нужен сторож Ryazha-clk 3.6.0 — здесь пропускается" : hints[i], 420);
        textRight(x + w - 22, ry, 16, noKip ? col::dim : i == 0 ? lc : (on ? col::accent : col::text), vals[i]);
    }
    text(x + 18, y + 20 + visible * rh + 6, 15, col::dim, "Порядок ручек и запас применяются к новому подбору, частоты ЦПУ/ГПУ — сразу. Сохраняется автоматически.");
}

// ============================================================== Сервис

void App::drawService(int x, int y, int w, int h) {
    static const char* rows[] = {"Вернуть как было (исходный timings.ini, без временных частот)", "Сбросить прогресс подбора",
                                 "Лог демона Ryazha-clk: включить / выключить", "Показать хвост лога демона", "Перечитать список частот у демона",
                                 "Показать журнал приложения (ошибки и ход работы)", "Перезагрузить консоль (через систему)",
                                 "Очистить реестр нестабильных значений", "Исследование потока: повторные прогоны частот (сток и гайд)",
                                 "Архив конфигураций: применить, записать, сохранить ручную настройку"};
    const int nr = 11;
    card(x, y, w, 24 + nr * 32);
    for (int i = 0; i < nr; i++) {
        int ry = y + 12 + i * 32;
        if (i == m_sel[S_Service]) rect(x + 6, ry - 4, w - 12, 30, col::sel);
        std::string r = i < 10 ? std::string(rows[i]) : "Планировщик ГПУ: " + gpuSchedText() + " · A — переключить, нужна перезагрузка";
        text(x + 18, ry, 19, i == 10 && m_gpuSched == 0 ? col::warn : col::text, r, w - 36);
    }
    int iy = y + 24 + nr * 32 + 12;
    card(x, iy, w, h - (iy - y), "Состояние");
    int ly = iy + 46;
    auto row = [&](const std::string& s, u32 c = col::dim) {
        text(x + 18, ly, 17, c, s, w - 36);
        ly += 24;
    };
    row(strf("Служба: %s · API %u · версия %s", m_daemon ? m_svc.c_str() : "нет", m_api, m_daemonVer.c_str()));
    if (m_gpuSchedRclk)
        row(strf("Планировщик ГПУ: %s · в config.ini Ryazha-clk gpu_scheduling=%d — демон при загрузке перекроет", gpuSchedText().c_str(),
                 m_gpuSchedRclk),
            col::warn);
    else
        row("Планировщик ГПУ (system_settings.ini, [am.gpu]): " + gpuSchedText(), m_gpuSchedPending ? col::warn : col::dim);
    row(strf("Ступеней частоты: %zu (%s … %s МГц)", m_ladder.size(), m_ladder.empty() ? "—" : mhzStr(m_ladder.front()).c_str(),
             m_ladder.empty() ? "—" : mhzStr(m_ladder.back()).c_str()));
    row(strf("Память под тесты: %s", m_bufInfo.c_str()));
    row(strf("Режим запуска: %s", m_appMode ? "через игру (приложение)" : "через альбом (апплет)"));
    row(strf("Копия исходного timings.ini: %s", m_t.originalAbsent() ? "файла не было" : paths::timingsBackup().c_str()));
    row(strf("Лог демона: %s", fileExists(paths::rclkLogFlag()) ? "включён" : "выключен"));
    int errs = logx::errorsThisSession();
    row(strf("Журнал приложения: %s (ошибок за сеанс: %d)", logx::file().c_str(), errs), errs ? col::warn : col::dim);
    row(strf("Журнал испытаний: %s · нестабильных значений в реестре: %zu (%s)", logx::testFile().c_str(), m_tu.unstable.size(),
             paths::unstable().c_str()));
    if (!m_logTitle.empty()) row(m_logTitle, col::accent);
    for (auto& l : m_logTail) {
        u32 c = l.find("[E]") != std::string::npos ? col::bad : (l.find("[W]") != std::string::npos ? col::warn : col::text);
        row(l, c);
        if (ly > y + h - 24) break;
    }
}
