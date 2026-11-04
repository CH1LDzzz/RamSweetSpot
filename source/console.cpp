#include "console.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>

#include "log.hpp"

bool ConsoleTarget::initBuffers(std::string* info) {
    size_t freeB = freeMemoryBytes();
    const size_t MB = 1024 * 1024;
    // Под замер — до 192 МБ (втрое больше, чем помещается в кэш и TLB), под задержку — 32 МБ.
    size_t bw = std::min<size_t>(192 * MB, std::max<size_t>(48 * MB, freeB / 5));
    size_t lat = 32 * MB;
    bool ok = membench.init(bw, lat);
    if (!ok) ok = membench.init(48 * MB, 16 * MB);
    size_t rest = freeB > (bw + lat + 96 * MB) ? freeB - (bw + lat + 96 * MB) : 0;
    size_t mt = std::min<size_t>(rest, (size_t)2048 * MB);
    bool ok2 = mt >= 64 * MB && memtest.init(mt);
    if (!ok2) ok2 = memtest.init(64 * MB);
    std::string s = strf("свободно %zu МБ: замер %zu+%zu МБ, memtest %zu МБ", freeB / MB, membench.bwBytes() / MB, membench.latBytes() / MB,
                         memtest.size() / MB);
    if (info) *info = s;
    if (ok && ok2)
        LOGI("Буферы: %s", s.c_str());
    else
        LOGE("Не хватило памяти под буферы (%s): замер %s, memtest %s", s.c_str(), ok ? "ок" : "нет", ok2 ? "ок" : "нет");
    return ok && ok2;
}

// Убрать из файла наши пробные секции (если прошлый сеанс оборвался посреди пробы).
static std::string stripTrials(const std::string& text) {
    TimingsIni ini;
    ini.parse(text);
    std::vector<TimingsIni::Section> keep;
    for (auto& s : ini.sections) {
        bool ours = false;
        for (auto& l : s.lines)
            if (l.find(kTagTrial) != std::string::npos) ours = true;
        if (!ours) keep.push_back(s);
    }
    ini.sections = keep;
    return ini.serialize();
}

void ConsoleTarget::prepareOriginal(bool fresh) {
    makeDirs(paths::appDir());
    std::string bak = paths::timingsBackup(), none = bak + ".none";
    if (!fresh) {
        if (fileExists(none)) {
            m_original.clear();
            m_absent = true;
            return;
        }
        if (readFile(bak, m_original)) {
            m_absent = false;
            return;
        }
    }
    std::string cur;
    if (readFile(paths::timings(), cur)) {
        m_original = stripTrials(cur);
        m_absent = false;
        if (!writeFileSafe(bak, m_original)) LOGE("Не удалось сохранить копию timings.ini в %s", bak.c_str());
        removeFile(none);
        LOGI("Копия исходного timings.ini: %zu байт -> %s", m_original.size(), bak.c_str());
    } else {
        m_original.clear();
        m_absent = true;
        writeFile(none, "");
        removeFile(bak);
        LOGI("timings.ini не было — при возврате файл будет удалён");
    }
}

bool ConsoleTarget::writeTimings(u32 hz, const KnobSet& set, ApplyInfo* info) {
    std::string content = composeTimings(m_original, hzToKhz(hz), set, kTagTrial);
    if (m_haveLast && content == m_lastWritten) return true;
    std::string onDisk;
    bool same = readFile(paths::timings(), onDisk) && onDisk == content;
    if (!same) {
        if (cfg && cfg->useDaemonLog) dlog.mark();
        makeDirs(paths::rclkDir());
        if (!writeFile(paths::timings(), content)) {
            lastNote = strf("не удалось записать timings.ini (%s)", strerror(errno));
            LOGE("%s: %s", paths::timings().c_str(), lastNote.c_str());
            return false;
        }
        std::string line;
        std::vector<std::string> na;
        bool confirmed = false;
        if (cfg && cfg->useDaemonLog && dlog.alive()) {
            confirmed = dlog.waitApplied(7000, &na, &line);
            lastNote = confirmed ? line : "демон не подтвердил применение за 7 с";
            if (!confirmed) LOGW("timings.ini для %s МГц: %s", mhzStr(hz).c_str(), lastNote.c_str());
            for (auto& n : na) LOGW("Демон: %s", n.c_str());
        } else {
            // Лога нет — демон перечитывает файл примерно раз в секунду; ждём с запасом.
            sleepMs(3500);
            lastNote = "применено (без подтверждения по логу)";
        }
        if (info) {
            info->confirmed = confirmed;
            info->notApplied = na;
        }
    }
    m_lastWritten = content;
    m_haveLast = true;
    return true;
}

bool ConsoleTarget::waitFreq(u32 hz, u32* realOut, std::string* err) {
    u64 deadline = nowMs() + 10000;
    int good = 0;
    RclkContext ctx;
    u32 lastReal = 0, lastSet = 0;
    while (nowMs() < deadline) {
        sleepMs(150);
        if (!rclk::context(&ctx)) continue;
        lastSet = ctx.freqs[RclkModule_MEM];
        lastReal = ctx.realFreqs[RclkModule_MEM] ? ctx.realFreqs[RclkModule_MEM] : lastSet;
        bool setOk = std::fabs((double)lastSet - hz) < 2e6;
        bool realOk = std::fabs((double)lastReal - hz) / hz < 0.02;
        good = (setOk && realOk) ? good + 1 : 0;
        if (good >= 3) break;
    }
    if (realOut) *realOut = lastReal;
    if (std::fabs((double)lastSet - hz) >= 2e6) {
        std::string e = strf("pcv не перешёл на %s МГц (стоит %s)", mhzStr(hz).c_str(), mhzStr(lastSet).c_str());
        // Частая причина: после зависания сторож Ryazha-clk держит демон без разгона до обычной перезагрузки.
        std::string wd;
        if (readFile(paths::rclkDir() + "watchdog.log", wd) && !wd.empty()) {
            auto lines = split(trim(wd), '\n');
            LOGW("watchdog.log Ryazha-clk: %s", trim(lines.back()).c_str());
        }
        e += ". Если консоль перезапускал сторож — перезагрузите её через меню питания";
        if (err) *err = e;
        LOGE("%s (override %s, реально %s)", e.c_str(), mhzStr(hz).c_str(), mhzStr(lastReal).c_str());
        return false;
    }
    sleepMs(300);
    return true;
}

bool ConsoleTarget::setFreq(u32 hz, u32* realOut, std::string* err) {
    RclkContext ctx;
    bool need = true;
    if (rclk::context(&ctx) && ctx.overrideFreqs[RclkModule_MEM] == hz && std::fabs((double)ctx.freqs[RclkModule_MEM] - hz) < 2e6) need = false;
    if (need && !rclk::setMemOverride(hz)) {
        if (err) *err = "демон отклонил временную частоту";
        LOGE("SetOverride(MEM, %u) отклонён", hz);
        return false;
    }
    return waitFreq(hz, realOut, err);
}

bool ConsoleTarget::apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) {
    // Сначала тайминги (демон перестраивает таблицы всей лестницы), потом переход на частоту.
    if (!writeTimings(hz, set, info)) {
        if (err) *err = lastNote;
        return false;
    }
    u32 real = 0;
    if (!setFreq(hz, &real, err)) return false;
    if (info) info->realHz = real;
    return true;
}

bool ConsoleTarget::applyOriginal(std::string* err) {
    setClockMode(ClockMode::Free);
    bool ok = m_absent ? (removeFile(paths::timings()) || !fileExists(paths::timings())) : writeFile(paths::timings(), m_original);
    if (!ok) LOGE("Не удалось вернуть исходный timings.ini (%s)", strerror(errno));
    m_haveLast = false;
    if (!rclk::setMemOverride(0)) {
        if (err) *err = "не удалось снять временную частоту";
        LOGE("Не удалось снять временную частоту РАМ");
        return false;
    }
    sleepMs(3500);
    if (!ok && err) *err = "не удалось вернуть timings.ini";
    if (ok) LOGI("Возвращены исходные настройки: timings.ini и частота профиля");
    return ok;
}

bool ConsoleTarget::commit(u32 hz, const KnobSet& set, std::string* err) {
    setClockMode(ClockMode::Free);
    std::string content = composeTimings(m_original, hzToKhz(hz), set, kTagFinal);
    if (!writeFile(paths::timings(), content)) {
        if (err) *err = strf("не удалось записать timings.ini (%s)", strerror(errno));
        LOGE("Запись итога: %s", err ? err->c_str() : "");
        return false;
    }
    LOGI("Итог записан: [%s] %s", mhzStr(hz).c_str(), knobSetToString(set).c_str());
    m_lastWritten = content;
    m_haveLast = true;
    u32 real = 0;
    std::string e2;
    setFreq(hz, &real, &e2);  // временная частота — до перезагрузки; постоянную задаёт профиль
    return true;
}

bool ConsoleTarget::stability(int seconds, std::string* err, bool* aborted) {
    // memtest и замер — всегда на частоте ЦПУ подбора, затем возвращаем прежний режим.
    ClockMode prev = m_mode;
    if (prev != ClockMode::Tuning) setClockMode(ClockMode::Tuning);
    measuring = true;
    MemTestResult r = memtest.run(seconds, &abort);
    measuring = false;
    if (prev != ClockMode::Tuning) setClockMode(prev);
    *aborted = r.aborted || abort.load();
    if (*aborted) return false;
    if (!r.ok) {
        if (err) *err = r.detail;
        LOGW("memtest %d с: %s", seconds, r.detail.c_str());
    }
    return r.ok;
}

BenchResult ConsoleTarget::bench(int passes, bool* aborted) {
    // Замер всегда при одной и той же частоте ЦПУ — иначе результаты разных проб несравнимы.
    ClockMode prev = m_mode;
    if (prev != ClockMode::Tuning) setClockMode(ClockMode::Tuning);
    measuring = true;
    BenchResult r = membench.run(passes, &abort);
    measuring = false;
    if (prev != ClockMode::Tuning) setClockMode(prev);
    *aborted = abort.load();
    if (!r.valid && !*aborted) LOGE("Замер памяти не удался (буферы не выделены?)");
    return r;
}

u32 ConsoleTarget::realHz() {
    // Замер реальной частоты у демона гуляет; берём самую близкую к записанной из трёх выборок.
    u32 best = 0;
    double bestD = 1e18;
    for (int i = 0; i < 3; i++) {
        RclkContext ctx;
        if (rclk::context(&ctx)) {
            u32 set = ctx.freqs[RclkModule_MEM];
            u32 real = ctx.realFreqs[RclkModule_MEM] ? ctx.realFreqs[RclkModule_MEM] : set;
            double d = std::fabs((double)real - (double)set);
            if (d < bestD) {
                bestD = d;
                best = real;
            }
        }
        if (i < 2) sleepMs(60);
    }
    return best;
}

u32 ConsoleTarget::pickHz(u32 module, int mhz, std::string* note) {
    if (mhz <= 0 || module > RclkModule_GPU) return 0;
    std::vector<u32>& list = m_lists[module];
    if (list.empty() && !rclk::freqList(module, &list)) {
        if (note) *note = strf("демон не отдал список частот %s", module == RclkModule_CPU ? "ЦПУ" : "ГПУ");
        LOGE("%s", note ? note->c_str() : "freqList");
        return 0;
    }
    u64 want = (u64)mhz * 1000000ULL + 999999ULL;  // 2397 -> до 2397.99 МГц
    u32 pick = 0, lowest = 0;
    for (u32 hz : list) {
        if (hz <= want && hz > pick) pick = hz;
        if (!lowest || hz < lowest) lowest = hz;
    }
    if (!pick) pick = lowest;
    const char* name = module == RclkModule_CPU ? "ЦПУ" : "ГПУ";
    if (pick && std::fabs(pick / 1e6 - mhz) > 2.0) {
        u32 top = list.empty() ? 0 : *std::max_element(list.begin(), list.end());
        LOGW("%s %d МГц нет в списке демона (максимум %s МГц) — взята %s МГц. Поднимите «Макс. частоту» %s в Ryazha-clk.", name, mhz,
             mhzStr(top).c_str(), mhzStr(pick).c_str(), name);
        if (note) *note = strf("%s %s вместо %d", name, mhzStr(pick).c_str(), mhz);
    }
    return pick;
}

bool ConsoleTarget::setClockMode(ClockMode m, std::string* note) {
    if (!cfg) return false;
    std::string n1, n2;
    u32 cpu = 0, gpu = 0;
    if (m == ClockMode::Tuning) cpu = pickHz(RclkModule_CPU, cfg->cpuTuneMhz, &n1);
    if (m == ClockMode::Game) {
        cpu = pickHz(RclkModule_CPU, cfg->cpuGameMhz, &n1);
        gpu = pickHz(RclkModule_GPU, cfg->gpuGameMhz, &n2);
    }
    bool ok = true;
    if (!rclk::setOverride(RclkModule_CPU, cpu)) {
        ok = false;
        LOGE("SetOverride(CPU, %u) отклонён", cpu);
    }
    if (!rclk::setOverride(RclkModule_GPU, gpu)) {
        ok = false;
        LOGE("SetOverride(GPU, %u) отклонён", gpu);
    }
    static const char* names[] = {"свободно", "подбор РАМ", "прогон TOTK"};
    if (m != m_mode) LOGI("Частоты: режим «%s» — ЦПУ %s, ГПУ %s", names[(int)m], cpu ? (mhzStr(cpu) + " МГц").c_str() : "как в профиле",
                          gpu ? (mhzStr(gpu) + " МГц").c_str() : "как в профиле");
    m_mode = m;
    if (note) {
        *note = strf("ЦПУ %s, ГПУ %s", cpu ? mhzStr(cpu).c_str() : "проф.", gpu ? mhzStr(gpu).c_str() : "проф.");
        if (!n1.empty()) *note += "; " + n1;
        if (!n2.empty()) *note += "; " + n2;
    }
    sleepMs(300);
    return ok;
}

bool ConsoleTarget::setGameClocks(std::string* note) { return setClockMode(ClockMode::Game, note); }

bool ConsoleTarget::gameClocksActive() {
    if (!cfg) return true;
    RclkContext ctx;
    if (!rclk::context(&ctx)) return false;
    u32 cpu = pickHz(RclkModule_CPU, cfg->cpuGameMhz, nullptr);
    u32 gpu = pickHz(RclkModule_GPU, cfg->gpuGameMhz, nullptr);
    return ctx.overrideFreqs[RclkModule_CPU] == cpu && ctx.overrideFreqs[RclkModule_GPU] == gpu;
}

u32 ConsoleTarget::overrideHz() {
    RclkContext ctx;
    if (!rclk::context(&ctx)) return 0;
    return ctx.overrideFreqs[RclkModule_MEM];
}

int ConsoleTarget::socTempC() {
    RclkContext ctx;
    if (!rclk::context(&ctx)) return 0;
    return ctx.temps[0] / 1000;
}

bool ConsoleTarget::saveText(const std::string& path, const std::string& text) {
    makeDirs(paths::appDir());
    return writeFileSafe(path, text);
}

bool ConsoleTarget::loadText(const std::string& path, std::string& text) { return readFileSafe(path, text); }

void ConsoleTarget::idle(u32 ms) { sleepMs(ms); }

// ============================================================== кип

static const u64 kCustRev = 35;  // ревизия таблицы кипа Ryazha-clk 3.6.0 и 3.5.5 (CUST_REV; кип у них один)

static bool readKipConfig(std::vector<u64>* v, std::string* err) {
    if (!rclk::getConfig(v)) {
        if (err) *err = "демон не отдал настройки (GetConfigValues)";
        return false;
    }
    if ((*v)[rclk::kCfgCustRev] != kCustRev) {
        if (err) *err = strf("ревизия таблицы кипа %llu, приложение знает только %llu (Ryazha-clk 3.5.5 / 3.6.0)", (unsigned long long)(*v)[rclk::kCfgCustRev],
                             (unsigned long long)kCustRev);
        return false;
    }
    return true;
}

bool ConsoleTarget::kipRead(std::map<int, int>* slots, std::string* err) {
    std::vector<u64> v;
    if (!readKipConfig(&v, err)) return false;
    slots->clear();
    // Знаковые: отрицательное лежит дополнительным кодом в младших 32 битах.
    for (int i = 0; i < 28; i++) (*slots)[i] = (int)(s32)(u32)v[rclk::kCfgTmehFirst + i];
    return true;
}

bool ConsoleTarget::kipWrite(const std::map<int, int>& slots, std::string* err) {
    std::vector<u64> v;
    if (!readKipConfig(&v, err)) return false;
    std::string what;
    for (auto& kv : slots) {
        if (kv.first < 0 || kv.first >= 28) continue;
        u64& cell = v[rclk::kCfgTmehFirst + kv.first];
        u64 nv = (u64)(u32)(s32)kv.second;
        if (cell != nv) what += strf(" t%d:%d→%d", kv.first, (int)(s32)(u32)cell, kv.second);
        cell = nv;
    }
    LOGI("Кип: запись ручек пропускной способности:%s", what.empty() ? " без изменений" : what.c_str());
    if (!rclk::setConfig(v)) {
        if (err) *err = "демон не принял настройки (SetConfigValues)";
        return false;
    }
    if (!rclk::setKipData()) {
        if (err) *err = "демон не записал кип (SetKipData)";
        return false;
    }
    sleepMs(500);
    std::map<int, int> back;
    if (!kipRead(&back, err)) return false;
    for (auto& kv : slots) {
        if (back[kv.first] != kv.second) {
            if (err) *err = strf("после записи слот %d = %d вместо %d (демон отбросил значение вне диапазона?)", kv.first, back[kv.first], kv.second);
            LOGE("Кип: %s", err ? err->c_str() : "");
            return false;
        }
    }
    return true;
}

bool ConsoleTarget::reboot(std::string* err) {
#ifdef __SWITCH__
    // Только через систему: psc сообщает демону о выключении, сторож гасится и возвращает настройки в кип.
    // Перезагрузку мимо HOS (bpcRebootSystem) сторож не отличит от зависания и загрузит консоль в стоке с откатом.
    setClockMode(ClockMode::Free);
    LOGI("Кип: перезагрузка консоли через spsm");
    Result rc = spsmInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = spsmShutdown(true);
        spsmExit();
    }
    if (R_SUCCEEDED(rc)) {
        // Система приняла запрос: перезагрузка идёт сама, процесс просто доживает последние секунды.
        // Раньше здесь ошибочно сообщалось «не удалась» с кодом 0.
        LOGI("Кип: система приняла перезагрузку — ждём");
        for (int i = 0; i < 600; i++) sleepMs(100);
        if (err) *err = "система приняла перезагрузку, но за 60 с консоль не перезагрузилась";
        LOGE("Кип: %s", err ? err->c_str() : "");
        return false;
    }
    if (err) *err = strf("spsmShutdown: 0x%X", rc);
    LOGE("Кип: перезагрузка не удалась: 0x%X", rc);
    return false;
#else
    if (err) *err = "не консоль";
    return false;
#endif
}

// Общие тайминги кипа в списке настроек демона (pMEH / sMEH, как в оверлее «Общие · нужна перезагрузка»).
// max — что демон принимает (ryazhaclkValidConfigValue в Ryazha-clk 3.6.0: pMEH[8..10] ≤ 4/4/8, sMEH[14,15] ≤ 3,
// sMEH[18..20] = 0); выше — сбрасывает в 0. tRTW, tWTR и tREFI через кип не задаются.
static const struct {
    const char* key;
    u32 idx;
    int max;
} kCommon[] = {{"tRCD", 237, 4}, {"tRP", 238, 4}, {"tRAS", 239, 8}, {"tRRD", 267, 3},
               {"tRFC", 268, 3}, {"tRTW", 271, 0}, {"tWTR", 272, 0}, {"tREFI", 273, 0}};

bool ConsoleTarget::kipCommonMax(const std::string& key, int* max) {
    for (auto& c : kCommon)
        if (key == c.key) {
            *max = c.max;
            return true;
        }
    return false;
}

bool ConsoleTarget::kipCommonRead(KnobSet* vals, std::string* err) {
    std::vector<u64> v;
    if (!readKipConfig(&v, err)) return false;
    vals->clear();
    for (auto& c : kCommon) (*vals)[c.key] = (int)(s32)(u32)v[c.idx];
    return true;
}

bool ConsoleTarget::kipCommonWrite(const KnobSet& vals, std::string* err) {
    std::vector<u64> v;
    if (!readKipConfig(&v, err)) return false;
    std::string what;
    for (auto& c : kCommon) {
        auto it = vals.find(c.key);
        if (it == vals.end()) continue;
        if (v[c.idx] != (u64)(u32)it->second) what += strf(" %s:%d→%d", c.key, (int)v[c.idx], it->second);
        v[c.idx] = (u64)(u32)it->second;
    }
    LOGI("Кип: общие тайминги:%s", what.empty() ? " без изменений" : what.c_str());
    if (!rclk::setConfig(v)) {
        if (err) *err = "демон не принял настройки (SetConfigValues)";
        return false;
    }
    if (!rclk::setKipData()) {
        if (err) *err = "демон не записал кип (SetKipData)";
        return false;
    }
    sleepMs(500);
    KnobSet back;
    if (!kipCommonRead(&back, err)) return false;
    for (auto& kv : vals)
        if (back.count(kv.first) && back[kv.first] != kv.second) {
            if (err) *err = strf("после записи %s = %d вместо %d", kv.first.c_str(), back[kv.first], kv.second);
            return false;
        }
    return true;
}

bool ConsoleTarget::safeMode() {
    // Метка сеанса сторожа (Ryazha-clk 3.6.0, pwr/watchdog.cpp): {u32 magic "RWDS", u8 safe, 3 байта, u64 время}.
    // Пишется при взводе таймера; safe = 1 — эта загрузка после зависания, частоты демон не ставит.
    std::string b;
    if (!readFile(paths::rclkDir() + "watchdog/session.bin", b) || b.size() < 5) return false;
    u32 magic = (u8)b[0] | ((u8)b[1] << 8) | ((u8)b[2] << 16) | ((u32)(u8)b[3] << 24);
    return magic == 0x53445752u && b[4] != 0;
}

// Первая версия вида x.y.z в строке демона («Ryazha-clk 3.5.5») числом x·10000 + y·100 + z; 0 — не нашлась.
static int daemonVerNum(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i]) || (i && isdigit((unsigned char)s[i - 1]))) continue;
        int part[3] = {0, 0, 0}, k = 0;
        size_t j = i;
        while (k < 3 && j < s.size() && isdigit((unsigned char)s[j])) {
            while (j < s.size() && isdigit((unsigned char)s[j])) part[k] = part[k] * 10 + (s[j++] - '0');
            k++;
            if (k < 3 && j + 1 < s.size() && s[j] == '.' && isdigit((unsigned char)s[j + 1])) j++;
            else break;
        }
        if (k >= 2) return part[0] * 10000 + part[1] * 100 + part[2];
    }
    return 0;
}

int ConsoleTarget::watchdogState() {
    // Сторож появился в Ryazha-clk 3.6.0. Демон постарше — его нет, даже если список настроек не прочитать.
    std::string ver;
    if (rclk::versionString(&ver)) {
        int n = daemonVerNum(ver);
        if (n > 0 && n < 30600) {
            LOGI("Сторожа у Ryazha-clk нет (%s — до 3.6.0)", ver.c_str());
            return kWatchdogNone;
        }
    }
    std::vector<u64> v;
    if (!rclk::getConfig(&v) || v.size() <= rclk::kCfgWatchdog) return -1;
    if (rclk::configCount() <= rclk::kCfgWatchdog) {
        LOGI("Сторожа у Ryazha-clk нет (настроек %u — до 3.6.0)", rclk::configCount());
        return kWatchdogNone;
    }
    int on = v[rclk::kCfgWatchdog] != 0 ? 1 : 0;
    LOGI("Сторож Ryazha-clk: %s", on ? "включён" : "выключен");
    return on;
}

std::string ConsoleTarget::sensors() {
    RclkContext c;
    if (!rclk::context(&c)) return "датчики: демон не ответил";
    return strf("SoC %d °C, MEM %d °C, VDD2 %d мВ, VDDQ %d мВ, SoC %d мВ, РАМ %s, ЦПУ %s, ГПУ %s МГц, %d мВт", c.temps[0] / 1000, c.temps[7] / 1000,
                voltMv(c.voltages[1]), voltMv(c.voltages[4]), voltMv(c.voltages[0]), mhzStr(c.realFreqs[RclkModule_MEM]).c_str(),
                mhzStr(c.realFreqs[RclkModule_CPU]).c_str(), mhzStr(c.realFreqs[RclkModule_GPU]).c_str(), c.power[0]);
}

u64 ConsoleTarget::uptimeMs() {
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
#else
    return nowMs();
#endif
}
