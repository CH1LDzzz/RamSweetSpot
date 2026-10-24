#include "knobs.hpp"

#include <cmath>
#include <cstring>

// Диапазоны — из config/ryazha-clk/timings.ini.template (Ryazha-clk 3.6.0).
static const std::vector<KnobDef> kKnobs = {
    // key        label                    min  max  order              absent0 risky  prim   adv
    {"tREFI", "tREFI (регенерация)", 0, 6, KnobOrder::Up, false, true, true, false, "как часто освежать ячейки"},
    {"tRCD", "tRCD", 0, 7, KnobOrder::Up, false, true, true, false, "открытие строки -> чтение"},
    {"tRP", "tRP", 0, 7, KnobOrder::Up, false, true, true, false, "предзаряд строки"},
    {"tRAS", "tRAS", 0, 9, KnobOrder::Up, false, true, true, false, "мин. время открытой строки"},
    {"tRRD", "tRRD", 0, 6, KnobOrder::Up, false, true, true, false, "активации в разных банках"},
    {"tRFC", "tRFC", 0, 10, KnobOrder::Up, false, true, true, false, "длительность регенерации"},
    {"tRTW", "tRTW", 0, 7, KnobOrder::Up, false, true, true, false, "разворот чтение -> запись"},
    {"tWTR", "tWTR", 0, 9, KnobOrder::Up, false, true, true, false, "разворот запись -> чтение"},
    {"RL", "RL (задержка чтения)", -3, 1, KnobOrder::Down, true, true, false, false, "CAS: сильнее всего бьёт по задержке"},
    {"WL", "WL (задержка записи)", -3, 1, KnobOrder::Down, true, true, false, false, "задержка записи"},
    {"tFAW", "tFAW (окно активаций)", -4, 4, KnobOrder::Down, true, true, false, false, "окно четырёх активаций"},
    {"daTurns", "Развороты шины", 0, 3, KnobOrder::Explore, true, false, false, false, "R2W 32/48/64"},
    {"arbWin", "Окно арбитра", 0, 8, KnobOrder::Explore, true, false, false, false, "длиннее окно = выше поток"},
    {"ramPD", "HP Mode (сон памяти)", 0, 16, KnobOrder::Toggle, true, true, false, false, "1 = память не засыпает"},
    {"ptsaCpu", "Доля ЦПУ в арбитре", 0, 6, KnobOrder::Explore, true, false, false, true, "баланс ЦПУ/ГПУ — решает TOTK"},
    {"outReq", "Очередь арбитра", 0, 4, KnobOrder::Explore, true, false, false, true, "128 + 32·N"},
    {"rext", "Растяжка чтения", -8, 8, KnobOrder::Down, true, true, false, true, "такты шины после чтения"},
    {"wext", "Растяжка записи", -8, 8, KnobOrder::Down, true, true, false, true, "такты шины после записи"},
    {"arbSafe", "Запас арбитра", -2, 4, KnobOrder::Down, true, false, false, true, "запас на переключение шины"},
    {"tRPab", "tRPab", -3, 6, KnobOrder::Down, true, true, false, true, "предзаряд всех банков"},
    {"tW2P", "tW2P", -6, 6, KnobOrder::Down, true, true, false, true, "запись -> предзаряд"},
    {"latAllow", "Допуск задержки", -10, 10, KnobOrder::Explore, true, false, false, true, "шаг 5 %"},
    {"WLset", "Набор WL", 0, 1, KnobOrder::Explore, true, true, false, true, "0 = A, 1 = B"},
    {"tXP", "tXP", -8, 8, KnobOrder::Down, true, true, false, true, "выход из энергосбережения"},
    {"tCKESR", "tCKESR", -8, 8, KnobOrder::Down, true, true, false, true, "выдержка саморегенерации"},
    {"preRef", "Предзапрос регенерации", -2, 8, KnobOrder::Explore, true, false, false, true, "делитель, база 4"},
    {"daCovers", "Покрытия строк", 0, 2, KnobOrder::Explore, true, false, false, true, "вариант А / Б"},
};

const std::vector<KnobDef>& allKnobs() { return kKnobs; }

const KnobDef* findKnob(const std::string& key) {
    for (auto& k : kKnobs)
        if (key == k.key) return &k;
    return nullptr;
}

std::string knobValueText(const KnobDef& k, int v) {
    static const int tRCD[] = {18, 17, 16, 15, 14, 13, 12, 11};
    static const int tRAS[] = {42, 36, 34, 32, 30, 28, 26, 24, 22, 20};
    static const char* tRRD[] = {"7.5", "6.0", "5.0", "4.0", "3.0", "2.0", "1.0"};
    static const int tRFC[] = {140, 130, 120, 110, 100, 90, 80, 70, 60, 50, 40};
    static const int tWTR[] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    static const int tREFI[] = {3900, 5850, 7800, 11700, 15600, 99999};
    std::string key = k.key;
    if ((key == "tRCD" || key == "tRP") && v >= 0 && v < 8) return strf("%d нс", tRCD[v]);
    if (key == "tRAS" && v >= 0 && v < 10) return strf("%d нс", tRAS[v]);
    if (key == "tRRD" && v >= 0 && v < 7) return strf("%s нс", tRRD[v]);
    if (key == "tRFC" && v >= 0 && v < 11) return strf("%d нс", tRFC[v]);
    if (key == "tWTR" && v >= 0 && v < 10) return strf("%d нс", tWTR[v]);
    if (key == "tREFI") return v >= 0 && v < 6 ? strf("%d нс", tREFI[v]) : std::string("не менять");
    if (key == "tRTW") return strf("-%d такт.", v);
    if (key == "ramPD") return v == 0 ? "сток" : (v == 1 ? "HP Mode" : strf("нибл %d", v - 1));
    if (key == "daTurns") return v == 0 ? "сток" : strf("R2W %d", 16 + 16 * v);
    if (key == "ptsaCpu") return v == 0 ? "как было" : (v == 1 ? "по частоте" : strf("+%d %%", (v - 1) * 10));
    if (key == "tFAW") return strf("x%.1f", 4.0 + 0.5 * v);
    if (key == "RL" || key == "WL") return v == 0 ? std::string("сток") : strf("%+d ступ. MR2", v);
    if (key == "arbWin") return v == 0 ? std::string("сток") : strf("делитель %d", 1 + v);
    return strf("%+d", v);
}

std::vector<AutoKnob> autoKnobPlan(bool allowTrefiHigh, bool advanced) {
    std::vector<AutoKnob> plan;
    auto add = [&](const char* key, int capMax, int capMin) {
        const KnobDef* d = findKnob(key);
        if (d) plan.push_back({d, capMax, capMin});
    };
    // Порядок — раздел 10.6 руководства: tREFI, первичные по одному, развороты,
    // задержки RL/WL, пропускная способность, HP Mode последним.
    add("tREFI", allowTrefiHigh ? 3 : 2, 0);  // 7800 нс — норма JEDEC; выше — только по разрешению
    add("tRCD", 7, 0);
    add("tRP", 7, 0);
    add("tRAS", 9, 0);
    add("tRRD", 6, 0);
    add("tRFC", 10, 0);
    add("tRTW", 7, 0);
    add("tWTR", 9, 0);
    add("RL", 0, -3);
    add("WL", 0, -3);
    add("tFAW", 0, -4);
    add("daTurns", 3, 0);
    add("arbWin", 8, 0);
    if (advanced) {
        add("ptsaCpu", 6, 0);
        add("outReq", 4, 0);
        add("rext", 0, -4);
        add("wext", 0, -4);
        add("arbSafe", 0, -2);
        add("tRPab", 0, -3);
        add("tW2P", 0, -3);
    }
    add("ramPD", 1, 0);  // HP Mode — последним: он съедает запас
    return plan;
}

std::string knobSetToString(const KnobSet& s) {
    if (s.empty()) return "-";
    std::string out;
    for (auto& kv : s) {
        if (!out.empty()) out += ',';
        out += strf("%s:%d", kv.first.c_str(), kv.second);
    }
    return out;
}

bool knobSetFromString(const std::string& str, KnobSet& out) {
    out.clear();
    std::string t = trim(str);
    if (t.empty() || t == "-") return true;
    for (auto& part : split(t, ',')) {
        auto kv = split(part, ':');
        if (kv.size() != 2) return false;
        bool ok = false;
        long v = toLong(kv[1], 0, &ok);
        if (!ok || !findKnob(trim(kv[0]))) return false;
        out[trim(kv[0])] = (int)v;
    }
    return true;
}

std::string knobSetShort(const KnobSet& s) {
    if (s.empty()) return "общие (из кипа)";
    std::string out;
    // В порядке плана, чтобы читалось как в меню.
    for (auto& k : kKnobs) {
        auto it = s.find(k.key);
        if (it == s.end()) continue;
        if (!out.empty()) out += ' ';
        if (k.order == KnobOrder::Up || k.order == KnobOrder::Toggle || k.order == KnobOrder::Explore)
            out += strf("%s%d", k.key, it->second);
        else
            out += strf("%s%+d", k.key, it->second);
    }
    return out;
}

// ---------------------------------------------------------------- timings.ini

u32 TimingsIni::parseHeaderKhz(const std::string& innerRaw, bool* ok) {
    std::string inner = trim(innerRaw);
    *ok = false;
    if (inner.empty()) return 0;
    for (char c : inner)
        if (!(c == '.' || (c >= '0' && c <= '9'))) return 0;
    if (inner.find('.') != std::string::npos) {
        double mhz = toDouble(inner, 0, ok);
        if (!*ok || mhz <= 0) {
            *ok = false;
            return 0;
        }
        return (u32)llround(mhz * 1000.0);
    }
    long v = toLong(inner, 0, ok);
    if (!*ok || v <= 0) {
        *ok = false;
        return 0;
    }
    // Ryazha-clk понимает и МГц ([1600]), и кГц ([1600000]).
    return v < 100000 ? (u32)(v * 1000) : (u32)v;
}

std::string TimingsIni::headerFor(u32 khz) {
    if (khz % 100 == 0) return strf("[%u.%u]", khz / 1000, (khz % 1000) / 100);
    return strf("[%u]", khz);
}

bool TimingsIni::parse(const std::string& text) {
    preamble.clear();
    sections.clear();
    Section* cur = nullptr;
    for (auto& rawLine : split(text, '\n')) {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            size_t close = t.find(']');
            if (close != std::string::npos) {
                Section s;
                s.header = line;
                bool ok = false;
                s.khz = parseHeaderKhz(t.substr(1, close - 1), &ok);
                if (!ok) s.khz = 0;
                sections.push_back(s);
                cur = &sections.back();
                continue;
            }
        }
        if (cur)
            cur->lines.push_back(line);
        else
            preamble.push_back(line);
    }
    // split() на файле, заканчивающемся '\n', даёт пустую последнюю строку — уберём её.
    std::vector<std::string>& last = sections.empty() ? preamble : sections.back().lines;
    if (!last.empty() && last.back().empty()) last.pop_back();
    return true;
}

std::string TimingsIni::serialize() const {
    std::string out;
    for (auto& l : preamble) out += l + "\n";
    for (auto& s : sections) {
        out += s.header + "\n";
        for (auto& l : s.lines) out += l + "\n";
    }
    return out;
}

void TimingsIni::removeSection(u32 khz) {
    for (size_t i = 0; i < sections.size();) {
        if (sections[i].khz == khz && khz != 0)
            sections.erase(sections.begin() + i);
        else
            i++;
    }
}

void TimingsIni::setSection(u32 khz, const KnobSet& set, const char* tag) {
    removeSection(khz);
    if (set.empty()) return;
    Section s;
    s.khz = khz;
    s.header = headerFor(khz);
    if (tag && *tag) s.lines.push_back(strf("; %s", tag));
    // Ключи в порядке таблицы ручек — так файл удобно читать руками.
    for (auto& k : allKnobs()) {
        auto it = set.find(k.key);
        if (it != set.end()) s.lines.push_back(strf("%s = %d", k.key, it->second));
    }
    s.lines.push_back("");
    // Вставляем по возрастанию частоты, чтобы не путать порядок секций пользователя.
    size_t pos = sections.size();
    for (size_t i = 0; i < sections.size(); i++) {
        if (sections[i].khz > khz) {
            pos = i;
            break;
        }
    }
    sections.insert(sections.begin() + pos, s);
}

bool TimingsIni::getSection(u32 khz, KnobSet& out) const {
    out.clear();
    bool found = false;
    for (auto& s : sections) {
        if (s.khz != khz) continue;
        found = true;
        for (auto& l : s.lines) {
            std::string t = trim(l);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(t.substr(0, eq));
            std::string val = trim(t.substr(eq + 1));
            size_t sc = val.find(';');
            if (sc != std::string::npos) val = trim(val.substr(0, sc));
            bool ok = false;
            long v = toLong(val, 0, &ok);
            const KnobDef* d = findKnob(key);
            if (ok && d) out[key] = (int)v;
        }
    }
    return found;
}

bool TimingsIni::hasSection(u32 khz) const {
    for (auto& s : sections)
        if (s.khz == khz) return true;
    return false;
}

std::string composeTimings(const std::string& original, u32 khz, const KnobSet& set, const char* tag) {
    TimingsIni ini;
    ini.parse(original);
    if (set.empty())
        ini.removeSection(khz);
    else
        ini.setSection(khz, set, tag);
    return ini.serialize();
}
