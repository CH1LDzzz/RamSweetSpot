#include "tuner.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================== Settings

std::string Settings::serialize() const {
    std::string s;
    s += strf("w_bw=%d\n", wBw);
    s += strf("w_game=%d\n", wGame);
    s += strf("quick_sec=%d\n", quickSec);
    s += strf("verify_sec=%d\n", verifySec);
    s += strf("bench_passes=%d\n", benchPasses);
    s += strf("margin=%d\n", margin);
    s += strf("patience=%d\n", patience);
    s += strf("min_mhz=%d\n", minMhz);
    s += strf("max_mhz=%d\n", maxMhz);
    s += strf("scan_fail_stop=%d\n", scanFailStop);
    s += strf("num_targets=%d\n", numTargets);
    s += strf("temp_limit=%d\n", tempLimit);
    s += strf("allow_trefi_high=%d\n", allowTrefiHigh ? 1 : 0);
    s += strf("advanced=%d\n", advanced ? 1 : 0);
    s += strf("use_preset=%d\n", usePreset ? 1 : 0);
    s += strf("use_daemon_log=%d\n", useDaemonLog ? 1 : 0);
    s += strf("auto_launch=%d\n", autoLaunch ? 1 : 0);
    s += strf("cpu_tune_mhz=%d\n", cpuTuneMhz);
    s += strf("cpu_game_mhz=%d\n", cpuGameMhz);
    s += strf("gpu_game_mhz=%d\n", gpuGameMhz);
    return s;
}

void Settings::deserialize(const std::string& text) {
    for (auto& raw : split(text, '\n')) {
        std::string l = trim(raw);
        size_t eq = l.find('=');
        if (l.empty() || l[0] == ';' || eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq));
        bool ok = false;
        long v = toLong(l.substr(eq + 1), 0, &ok);
        if (!ok) continue;
        auto clampi = [](long x, long lo, long hi) { return (int)std::max(lo, std::min(hi, x)); };
        if (k == "w_bw") wBw = clampi(v, 0, 100);
        else if (k == "w_game") wGame = clampi(v, 0, 100);
        else if (k == "quick_sec") quickSec = clampi(v, 1, 120);
        else if (k == "verify_sec") verifySec = clampi(v, 5, 1800);
        else if (k == "bench_passes") benchPasses = clampi(v, 1, 10);
        else if (k == "margin") margin = clampi(v, 0, 3);
        else if (k == "patience") patience = clampi(v, 1, 5);
        else if (k == "min_mhz") minMhz = clampi(v, 0, 4000);
        else if (k == "max_mhz") maxMhz = clampi(v, 0, 4000);
        else if (k == "scan_fail_stop") scanFailStop = clampi(v, 0, 20);
        else if (k == "num_targets") numTargets = clampi(v, 1, 4);
        else if (k == "temp_limit") tempLimit = clampi(v, 50, 90);
        else if (k == "allow_trefi_high") allowTrefiHigh = v != 0;
        else if (k == "advanced") advanced = v != 0;
        else if (k == "use_preset") usePreset = v != 0;
        else if (k == "use_daemon_log") useDaemonLog = v != 0;
        else if (k == "auto_launch") autoLaunch = v != 0;
        else if (k == "cpu_tune_mhz") cpuTuneMhz = clampi(v, 0, 3100);
        else if (k == "cpu_game_mhz") cpuGameMhz = clampi(v, 0, 3100);
        else if (k == "gpu_game_mhz") gpuGameMhz = clampi(v, 0, 1600);
    }
}

bool Settings::load() {
    std::string text;
    if (!readFile(paths::settings(), text)) return false;
    deserialize(text);
    return true;
}

bool Settings::save() const {
    makeDirs(paths::appDir());
    return writeFileSafe(paths::settings(), serialize());
}

// ============================================================== мелочи

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "не запущен";
        case Phase::Baseline: return "1. Базовый замер";
        case Phase::FreqScan: return "2. Прогон частот";
        case Phase::Seed: return "3. Тайминги: старт";
        case Phase::Timing: return "3. Тайминги";
        case Phase::Verify: return "4. Длинная проверка";
        case Phase::Game: return "5. Проверка в TOTK";
        case Phase::Done: return "Готово";
        case Phase::Failed: return "Остановлен с ошибкой";
    }
    return "?";
}

static std::string clean(const std::string& s) {
    std::string o = s;
    for (auto& c : o)
        if (c == '\n' || c == '\r' || c == '|') c = ' ';
    return o;
}

std::string Candidate::name() const {
    if (baseline) return "Как было";
    return strf("К%d · %s МГц", id, mhzStr(hz).c_str());
}

std::string TrialSpec::serialize() const {
    return strf("%d|%u|%s|%d|%d|%s|%d|%s", kind, hz, knobSetToString(set).c_str(), stabSec, passes, key.empty() ? "-" : key.c_str(), val,
                clean(desc).c_str());
}

bool TrialSpec::deserialize(const std::string& s) {
    auto p = split(s, '|');
    if (p.size() < 8) return false;
    kind = (int)toLong(p[0], 0);
    hz = (u32)strtoul(p[1].c_str(), nullptr, 10);
    if (!knobSetFromString(p[2], set)) return false;
    stabSec = (int)toLong(p[3], 0);
    passes = (int)toLong(p[4], 1);
    key = p[5] == "-" ? std::string() : p[5];
    val = (int)toLong(p[6], 0);
    desc = p[7];
    return kind != None;
}

void Tuner::ev(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrf(fmt, ap);
    va_end(ap);
    events.push_back(clean(s));
    LOGI("[подбор] %s", events.back().c_str());
    if (events.size() > 80) events.erase(events.begin(), events.begin() + (events.size() - 80));
}

double Tuner::scoreOf(const BenchResult& b) const {
    if (!b.valid || !base.valid || b.bw() <= 0 || b.latNs <= 0 || base.bw() <= 0 || base.latNs <= 0) return 0;
    double wb = cfg.wBw / 100.0, wl = 1.0 - wb;
    return std::pow(b.bw() / base.bw(), wb) * std::pow(base.latNs / b.latNs, wl);
}

u32 Tuner::stockHz() const {
    u32 best = 1600000000u;
    u32 bestD = 0xFFFFFFFF;
    for (u32 hz : ladder) {
        u32 d = hz > 1600000000u ? hz - 1600000000u : 1600000000u - hz;
        if (d < bestD) {
            bestD = d;
            best = hz;
        }
    }
    return best;
}

bool Tuner::isAuto() const {
    return phase == Phase::Baseline || phase == Phase::FreqScan || phase == Phase::Seed || phase == Phase::Timing || phase == Phase::Verify;
}

void Tuner::resetAll() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Settings keep = cfg;
    std::vector<u32> keepLadder = ladder;
    ChipInfo keepChip = chip;
    phase = Phase::Idle;
    status.clear();
    error.clear();
    trials = 0;
    startedEpoch = 0;
    events.clear();
    baseRuns.clear();
    base = BenchResult();
    eps = 0.005;
    coarse.clear();
    scanPos = 0;
    lastOk = -1;
    failRun = 0;
    lastOkBw = 0;
    points.clear();
    targets.clear();
    tIdx = 0;
    seedStage = 0;
    inhScore = 0;
    presetSet.clear();
    plan.clear();
    cur.clear();
    curScore = 0;
    kIdx = 0;
    seqStart = 0;
    seqStartInh = true;
    seqPos = 0;
    haveBest = false;
    bestVal = 0;
    bestScore = startScore = 0;
    noimp = 0;
    haveFail = false;
    failVal = 0;
    remeasure = false;
    remeasureFails = 0;
    accepted.clear();
    verifyTries = 0;
    loosenRound = 0;
    cands.clear();
    gamePendingCand = -1;
    gameActiveCand = -1;
    gameSnapshot.clear();
    pending = TrialSpec();
    cfg = keep;
    ladder = keepLadder;
    chip = keepChip;
}

void Tuner::begin(const std::vector<u32>& lad, const ChipInfo& c) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    resetAll();
    ladder = lad;
    std::sort(ladder.begin(), ladder.end());
    ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
    chip = c;
    startedEpoch = epochNow();
    plan.clear();
    for (auto& ak : autoKnobPlan(cfg.allowTrefiHigh, cfg.advanced)) plan.push_back({ak.def->key, ak.capMax, ak.capMin});
    if (cfg.usePreset && chip.preset) presetSet = presetToKnobs(chip.preset->common);
    phase = Phase::Baseline;
    ev("Старт подбора: %s, ступеней частоты %zu", chip.name.c_str(), ladder.size());
    if (!ladder.empty()) {
        ev("Лестница Ryazha-clk: %s … %s МГц", mhzStr(ladder.front()).c_str(), mhzStr(ladder.back()).c_str());
        // Лестницу строит загрузчик Ryazha-clk при загрузке, до «Макс. частоты РАМ» из оверлея. Если она короче
        // запрошенного верха, выше прогон не пойдёт — частот там у демона просто нет.
        u32 want = cfg.maxMhz > 0 ? (u32)cfg.maxMhz : 2900u;
        if (ladder.back() / 1000000u + 40 < want)
            ev("Внимание: лестница кончается на %s МГц — выше прогон не пойдёт. Поднимите «Макс. частоту РАМ» в Ryazha-clk и "
               "перезагрузите консоль",
               mhzStr(ladder.back()).c_str());
    }
    if (chip.preset)
        ev("Пресет «Common» для %s (тир %s): %s, ориентир %u–%u МГц", chip.preset->chip, chip.preset->tier, knobSetShort(presetSet).c_str(),
           chip.preset->minMhz, chip.preset->maxMhz);
}

// ============================================================== шаг

bool Tuner::step(ITarget& t, std::atomic<bool>* stop) {
    TrialSpec spec;
    std::string snap;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (!isAuto()) return false;
    }
    // Остывание до начала пробы (пока ничего не применено — журнал чистый).
    int temp = t.socTempC();
    if (temp >= cfg.tempLimit) {
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            status = strf("Остывание: SoC %d °C (порог %d °C)", temp, cfg.tempLimit);
        }
        while (temp >= cfg.tempLimit - 5 && !(stop && stop->load())) {
            t.idle(2000);
            temp = t.socTempC();
            std::lock_guard<std::recursive_mutex> lk(mtx);
            status = strf("Остывание: SoC %d °C (ждём %d °C)", temp, cfg.tempLimit - 5);
        }
        return !(stop && stop->load());
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        spec = nextTrial();
        if (!spec.valid()) {
            snap = serialize();
        } else {
            pending = spec;
            trials++;
            status = spec.desc;
            snap = serialize();
        }
    }
    t.saveText(paths::state(), snap);
    if (!spec.valid()) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        return isAuto();
    }

    TrialOut out = runTrial(t, spec, stop);

    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        pending = TrialSpec();
        if (out.aborted) {
            trials--;
            status = "Пауза — проба будет повторена";
        } else {
            handleResult(spec, out);
        }
        snap = serialize();
    }
    t.saveText(paths::state(), snap);
    return !out.aborted;
}

TrialOut Tuner::runTrial(ITarget& t, const TrialSpec& spec, std::atomic<bool>* stop) {
    TrialOut out;
    std::string err;
    if (!t.apply(spec.hz, spec.set, &out.ai, &err)) {
        out.reason = err.empty() ? "не удалось применить" : err;
        LOGE("Проба «%s»: применение не удалось: %s", spec.desc.c_str(), out.reason.c_str());
        return out;
    }
    auto offFreq = [&](u32 real) {
        if (real == 0 || spec.hz == 0) return false;
        double d = std::fabs((double)real - (double)spec.hz) / (double)spec.hz;
        return d > 0.03;
    };
    if (offFreq(out.ai.realHz)) {
        out.reason = strf("PLL drop: реально %s МГц вместо %s", mhzStr(out.ai.realHz).c_str(), mhzStr(spec.hz).c_str());
        return out;
    }
    if (stop && stop->load()) {
        out.aborted = true;
        return out;
    }
    if (spec.stabSec > 0) {
        bool aborted = false;
        if (!t.stability(spec.stabSec, &err, &aborted)) {
            if (aborted) {
                out.aborted = true;
                return out;
            }
            out.reason = "memtest: " + err;
            return out;
        }
    }
    bool aborted = false;
    out.b = t.bench(spec.passes, &aborted);
    if (aborted) {
        out.aborted = true;
        return out;
    }
    if (!out.b.valid) {
        out.reason = "замер не удался";
        return out;
    }
    u32 real2 = t.realHz();
    if (offFreq(real2)) {
        out.reason = strf("PLL drop под нагрузкой: %s МГц", mhzStr(real2).c_str());
        return out;
    }
    out.score = scoreOf(out.b);
    out.ok = true;
    return out;
}

// ============================================================== выбор следующей пробы

TrialSpec Tuner::nextTrial() {
    for (int guard = 0; guard < 200; guard++) {
        TrialSpec s;
        switch (phase) {
            case Phase::Baseline: {
                if (baseRuns.size() >= 3) {
                    finishBaseline();
                    continue;
                }
                s.kind = TrialSpec::Base;
                s.hz = stockHz();
                s.stabSec = 0;
                s.passes = cfg.benchPasses;
                s.desc = strf("Базовый замер %zu/3 на %s МГц", baseRuns.size() + 1, mhzStr(s.hz).c_str());
                return s;
            }
            case Phase::FreqScan: {
                int idx = scanNextIdx();
                if (idx < 0) {
                    finishScan();
                    continue;
                }
                s.kind = TrialSpec::Scan;
                s.hz = ladder[idx];
                s.stabSec = cfg.quickSec;
                s.passes = cfg.benchPasses;
                s.desc = strf("Частота %s МГц (прогон: ступень %d из %zu)", mhzStr(s.hz).c_str(), scanPos + 1, coarse.size());
                return s;
            }
            case Phase::Seed: {
                if (tIdx >= (int)targets.size()) {
                    enterGame();
                    continue;
                }
                s.hz = targets[tIdx];
                s.stabSec = cfg.quickSec;
                s.passes = cfg.benchPasses;
                if (seedStage == 0) {
                    s.kind = TrialSpec::SeedInh;
                    s.desc = strf("%s МГц: общие тайминги кипа", mhzStr(s.hz).c_str());
                } else {
                    s.kind = TrialSpec::SeedPreset;
                    s.set = presetSet;
                    s.desc = strf("%s МГц: пресет чипа %s", mhzStr(s.hz).c_str(), knobSetShort(presetSet).c_str());
                }
                return s;
            }
            case Phase::Timing: {
                u32 hz = targets[tIdx];
                if (remeasure) {
                    s.kind = TrialSpec::Remeasure;
                    s.hz = hz;
                    s.set = cur;
                    s.stabSec = cfg.quickSec;
                    s.passes = cfg.benchPasses;
                    s.desc = strf("%s МГц: перепроверка с запасом", mhzStr(hz).c_str());
                    return s;
                }
                if (kIdx >= (int)plan.size()) {
                    phase = Phase::Verify;
                    verifyTries = 0;
                    ev("%s МГц: подбор ручек закончен — %s", mhzStr(hz).c_str(), knobSetShort(cur).c_str());
                    continue;
                }
                auto seq = buildSeq();
                if (seqPos >= (int)seq.size()) {
                    finishKnob();
                    continue;
                }
                const KnobDef* d = findKnob(plan[kIdx].key);
                s.kind = TrialSpec::Knob;
                s.hz = hz;
                s.set = cur;
                s.key = plan[kIdx].key;
                s.val = seq[seqPos];
                if (d->absentIsZero && s.val == 0)
                    s.set.erase(s.key);
                else
                    s.set[s.key] = s.val;
                s.stabSec = cfg.quickSec;
                s.passes = cfg.benchPasses;
                s.desc = strf("%s МГц: %s = %d (%s)", mhzStr(hz).c_str(), s.key.c_str(), s.val, knobValueText(*d, s.val).c_str());
                return s;
            }
            case Phase::Verify: {
                s.kind = TrialSpec::Verify;
                s.hz = targets[tIdx];
                s.set = cur;
                s.stabSec = cfg.verifySec;
                s.passes = cfg.benchPasses + 1;
                s.desc = strf("%s МГц: длинная проверка %d с", mhzStr(s.hz).c_str(), cfg.verifySec);
                return s;
            }
            default:
                return s;
        }
    }
    phase = Phase::Failed;
    error = "внутренняя ошибка: зацикливание выбора пробы";
    return TrialSpec();
}

// ============================================================== обработка результата

void Tuner::handleResult(const TrialSpec& spec, const TrialOut& out) {
    if (!out.ai.notApplied.empty())
        for (auto& n : out.ai.notApplied) ev("Демон: %s", n.c_str());

    switch (spec.kind) {
        case TrialSpec::Base: {
            if (!out.ok) {
                phase = Phase::Failed;
                error = "базовый замер на стоковой частоте не удался: " + out.reason;
                ev("%s", error.c_str());
                return;
            }
            baseRuns.push_back(out.b);
            ev("Базовый замер: поток %.0f МБ/с, задержка %.1f нс", out.b.bw(), out.b.latNs);
            return;
        }
        case TrialSpec::Scan: {
            int idx = -1;
            for (size_t i = 0; i < ladder.size(); i++)
                if (ladder[i] == spec.hz) idx = (int)i;
            FreqPoint fp;
            fp.hz = spec.hz;
            fp.b = out.b;
            fp.score = out.score;
            bool ok = out.ok;
            std::string reason = out.reason;
            bool slower = false;
            if (ok && lastOk >= 0 && out.b.bw() < lastOkBw * (1.0 - std::max(0.03, eps * 2))) {
                // Не сбой: прогон идёт дальше, но в цели такая ступень не попадёт.
                slower = true;
                reason = strf("медленнее предыдущей стабильной ступени (%.0f < %.0f МБ/с) — похоже на PLL drop", out.b.bw(), lastOkBw);
            }
            fp.status = slower ? 3 : (ok ? 1 : 2);
            fp.reason = reason;
            points.erase(std::remove_if(points.begin(), points.end(), [&](const FreqPoint& p) { return p.hz == spec.hz; }), points.end());
            points.push_back(fp);
            std::sort(points.begin(), points.end(), [](const FreqPoint& a, const FreqPoint& b) { return a.hz < b.hz; });
            scanPos++;
            if (slower) {
                failRun = 0;
                ev("%s МГц — %s", mhzStr(spec.hz).c_str(), reason.c_str());
            } else if (ok) {
                ev("%s МГц — стабильно: поток %.0f МБ/с, задержка %.1f нс, оценка %.3f", mhzStr(spec.hz).c_str(), out.b.bw(), out.b.latNs,
                   out.score);
                failRun = 0;
                if (idx > lastOk) lastOk = idx;
                lastOkBw = out.b.bw();
            } else {
                failRun++;
                ev("%s МГц — сбой: %s", mhzStr(spec.hz).c_str(), reason.c_str());
                // Сбой не останавливает прогон: следующая ступень проверяется с нуля. Остановка — только по настройке.
                if (cfg.scanFailStop > 0 && failRun >= cfg.scanFailStop && scanPos < (int)coarse.size()) {
                    ev("%d сбоев подряд — прогон остановлен на %s МГц (настройка «Сбоев подряд до остановки»)", failRun,
                       mhzStr(spec.hz).c_str());
                    scanPos = (int)coarse.size();
                }
            }
            return;
        }
        case TrialSpec::SeedInh: {
            if (!out.ok) {
                ev("%s МГц больше не держится на общих таймингах (%s) — цель пропущена", mhzStr(spec.hz).c_str(), out.reason.c_str());
                nextTarget();
                return;
            }
            inhScore = out.score;
            if (!presetSet.empty()) {
                seedStage = 1;
            } else {
                startTiming(KnobSet(), inhScore);
            }
            return;
        }
        case TrialSpec::SeedPreset: {
            if (out.ok && out.score >= inhScore * (1.0 - eps)) {
                ev("Пресет чипа держится (оценка %.3f против %.3f у общих) — стартуем с него", out.score, inhScore);
                startTiming(presetSet, std::max(out.score, 0.0));
            } else {
                if (out.ok)
                    ev("Общие тайминги кипа быстрее пресета (%.3f > %.3f) — стартуем с них", inhScore, out.score);
                else
                    ev("Пресет чипа не прошёл (%s) — стартуем с общих таймингов", out.reason.c_str());
                startTiming(KnobSet(), inhScore);
            }
            return;
        }
        case TrialSpec::Knob: {
            const KnobDef* d = findKnob(spec.key);
            if (!out.ok) {
                haveFail = true;
                failVal = spec.val;
                ev("%s = %d — сбой: %s", spec.key.c_str(), spec.val, out.reason.c_str());
                finishKnob();
                return;
            }
            double ref = std::max(startScore, haveBest ? bestScore : 0.0);
            std::string note;
            if (!out.ai.notApplied.empty()) note = " (демон: не дошло до регистров)";
            if (out.score > ref * (1.0 + eps)) {
                haveBest = true;
                bestVal = spec.val;
                bestScore = out.score;
                noimp = 0;
                ev("%s = %d: оценка %.3f — лучше%s", spec.key.c_str(), spec.val, out.score, note.c_str());
            } else {
                if (out.score >= startScore * (1.0 - eps)) noimp++;
                ev("%s = %d: оценка %.3f — без прироста%s", spec.key.c_str(), spec.val, out.score, note.c_str());
            }
            seqPos++;
            bool monotonic = d && (d->order == KnobOrder::Up || d->order == KnobOrder::Down);
            if (monotonic && noimp >= cfg.patience) finishKnob();
            return;
        }
        case TrialSpec::Remeasure: {
            if (out.ok) {
                curScore = out.score;
                startScore = curScore;
                remeasure = false;
                remeasureFails = 0;
                return;
            }
            remeasureFails++;
            ev("Перепроверка не прошла (%s)", out.reason.c_str());
            if (remeasureFails > 3 || accepted.empty()) {
                ev("%s МГц: цель пропущена — не удаётся вернуть стабильность", mhzStr(spec.hz).c_str());
                remeasure = false;
                nextTarget();
                return;
            }
            auto last = accepted.back();
            accepted.pop_back();
            setValue(cur, last.key, last.prev);
            ev("Откат %s к %s", last.key.c_str(), last.prev == "inh" ? "общему" : last.prev.c_str());
            remeasure = true;
            return;
        }
        case TrialSpec::Verify: {
            if (out.ok) {
                addCandidate(spec.hz, cur, out.b, out.score);
                ev("%s МГц прошла длинную проверку: поток %.0f МБ/с, задержка %.1f нс, оценка %.3f", mhzStr(spec.hz).c_str(), out.b.bw(),
                   out.b.latNs, out.score);
                nextTarget();
                return;
            }
            verifyTries++;
            ev("Длинная проверка %s МГц не прошла (%s), попытка %d", mhzStr(spec.hz).c_str(), out.reason.c_str(), verifyTries);
            if (verifyTries > 16) {
                ev("%s МГц пропущена — нестабильна даже после ослабления", mhzStr(spec.hz).c_str());
                nextTarget();
                return;
            }
            if (!loosenForVerify()) {
                if (!cur.empty()) {
                    cur.clear();
                    ev("Сброс к общим таймингам кипа");
                } else {
                    ev("%s МГц пропущена — нестабильна на общих таймингах", mhzStr(spec.hz).c_str());
                    nextTarget();
                }
            }
            return;
        }
    }
}

void Tuner::finishBaseline() {
    BenchResult avg;
    avg.valid = true;
    for (auto& b : baseRuns) {
        avg.readMBs += b.readMBs;
        avg.writeMBs += b.writeMBs;
        avg.copyMBs += b.copyMBs;
        avg.latNs += b.latNs;
        avg.lat2Ns += b.lat2Ns;
    }
    double n = (double)baseRuns.size();
    avg.readMBs /= n;
    avg.writeMBs /= n;
    avg.copyMBs /= n;
    avg.latNs /= n;
    avg.lat2Ns /= n;
    base = avg;
    double lo = 1e9, hi = 0;
    for (auto& b : baseRuns) {
        double s = scoreOf(b);
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    double spread = hi > 0 ? (hi - lo) : 0;
    eps = std::max(0.003, std::min(0.02, spread * 1.5));
    ev("Шум замера %.2f %% — порог значимого прироста %.2f %%", spread * 100, eps * 100);
    phase = Phase::FreqScan;
    buildCoarse();
}

void Tuner::buildCoarse() {
    coarse.clear();
    scanPos = 0;
    lastOk = -1;
    failRun = 0;
    lastOkBw = 0;
    if (ladder.empty()) return;
    u64 minHz = cfg.minMhz > 0 ? (u64)cfg.minMhz * 1000000ULL - 500000ULL : 0;
    u64 maxHz = cfg.maxMhz > 0 ? (u64)cfg.maxMhz * 1000000ULL + 500000ULL : ~0ULL;
    // Каждая ступень лестницы демона в диапазоне — по возрастанию, без пропусков и без остановки на первом сбое.
    for (size_t i = 0; i < ladder.size(); i++)
        if (ladder[i] >= minHz && ladder[i] <= maxHz) coarse.push_back((int)i);
    if (coarse.empty()) {
        ev("В диапазоне %d…%s МГц нет ни одной ступени лестницы", cfg.minMhz, cfg.maxMhz ? strf("%d", cfg.maxMhz).c_str() : "верх");
        return;
    }
    ev("Прогон частот: %zu ступеней от %s до %s МГц, каждая — memtest и замер", coarse.size(), mhzStr(ladder[coarse.front()]).c_str(),
       mhzStr(ladder[coarse.back()]).c_str());
}

int Tuner::scanNextIdx() const { return scanPos < (int)coarse.size() ? coarse[scanPos] : -1; }

void Tuner::finishScan() {
    u32 stock = stockHz();
    if (lastOk < 0 || ladder[lastOk] <= stock) {
        phase = Phase::Failed;
        error = "ни одна разгонная частота не прошла проверку — проверьте VDD2/VDDQ и потолок РАМ в Ryazha-clk";
        ev("%s", error.c_str());
        return;
    }
    u32 fmax = ladder[lastOk];
    ev("Максимальная стабильная частота на общих таймингах: %s МГц", mhzStr(fmax).c_str());
    targets.clear();
    targets.push_back(fmax);
    // Остальные цели: сперва «JEDEC-частоты» (гайд Horizon-OC: часто держат таймингы туже), потом соседние ступени.
    static const double jedec[] = {1866, 1996.8, 2133, 2400, 2666, 2933, 3200};
    std::vector<u32> ok;
    for (auto& p : points)
        if (p.status == 1 && p.hz < fmax && p.hz > stock && p.hz + 450000000u >= fmax) ok.push_back(p.hz);
    std::sort(ok.rbegin(), ok.rend());
    auto isJedec = [&](u32 hz) {
        for (double j : jedec)
            if (std::fabs(hz / 1e6 - j) / j < 0.006) return true;
        return false;
    };
    for (u32 hz : ok)
        if ((int)targets.size() < cfg.numTargets && isJedec(hz)) targets.push_back(hz);
    for (u32 hz : ok)
        if ((int)targets.size() < cfg.numTargets && std::find(targets.begin(), targets.end(), hz) == targets.end()) targets.push_back(hz);
    std::string list;
    for (u32 hz : targets) list += (list.empty() ? "" : ", ") + mhzStr(hz);
    ev("Доводим тайминги на частотах: %s МГц", list.c_str());
    phase = Phase::Seed;
    tIdx = 0;
    seedStage = 0;
}

void Tuner::startTiming(const KnobSet& set, double score) {
    cur = set;
    curScore = score;
    kIdx = 0;
    accepted.clear();
    // Значения пресета тоже можно будет ослаблять по ступеням, если длинная проверка не пройдёт.
    for (auto& kv : set) accepted.push_back({kv.first, "inh", false});
    remeasure = false;
    remeasureFails = 0;
    verifyTries = 0;
    loosenRound = 0;
    phase = Phase::Timing;
    resetKnob();
}

void Tuner::nextTarget() {
    tIdx++;
    seedStage = 0;
    cur.clear();
    accepted.clear();
    remeasure = false;
    phase = Phase::Seed;
}

void Tuner::resetKnob() {
    seqPos = 0;
    haveBest = false;
    bestVal = 0;
    bestScore = 0;
    startScore = curScore;
    noimp = 0;
    haveFail = false;
    failVal = 0;
    if (kIdx >= (int)plan.size()) return;
    const KnobDef* d = findKnob(plan[kIdx].key);
    auto it = cur.find(plan[kIdx].key);
    if (it != cur.end()) {
        seqStartInh = false;
        seqStart = it->second;
    } else if (d && d->absentIsZero) {
        seqStartInh = false;
        seqStart = 0;
    } else {
        seqStartInh = true;
        seqStart = 0;
    }
}

std::vector<int> Tuner::buildSeq() const {
    std::vector<int> seq;
    if (kIdx >= (int)plan.size()) return seq;
    const PlanItem& pi = plan[kIdx];
    const KnobDef* d = findKnob(pi.key);
    if (!d) return seq;
    int hi = std::min(d->max, pi.capMax), lo = std::max(d->min, pi.capMin);
    switch (d->order) {
        case KnobOrder::Up:
            for (int v = seqStartInh ? lo : seqStart + 1; v <= hi; v++) seq.push_back(v);
            break;
        case KnobOrder::Down:
            for (int v = seqStart - 1; v >= lo; v--) seq.push_back(v);
            break;
        case KnobOrder::Explore:
            for (int v = lo; v <= hi; v++)
                if (v != seqStart) seq.push_back(v);
            break;
        case KnobOrder::Toggle:
            if (seqStart != hi) seq.push_back(hi);
            break;
    }
    return seq;
}

std::string Tuner::valueStr(const std::string& key) const {
    auto it = cur.find(key);
    return it == cur.end() ? std::string("inh") : strf("%d", it->second);
}

void Tuner::setValue(KnobSet& s, const std::string& key, const std::string& val) const {
    const KnobDef* d = findKnob(key);
    if (val == "inh") {
        s.erase(key);
        return;
    }
    int v = (int)toLong(val, 0);
    if (d && d->absentIsZero && v == 0)
        s.erase(key);
    else
        s[key] = v;
}

void Tuner::finishKnob() {
    if (kIdx >= (int)plan.size()) return;
    const std::string key = plan[kIdx].key;
    const KnobDef* d = findKnob(key);
    auto seq = buildSeq();
    auto pos = [&](int v) {
        for (size_t i = 0; i < seq.size(); i++)
            if (seq[i] == v) return (int)i;
        return -1;
    };
    bool monotonic = d && (d->order == KnobOrder::Up || d->order == KnobOrder::Down);
    std::string prev = valueStr(key);
    bool changed = false;
    bool edge = false;
    std::string chosenStr;

    if (haveBest) {
        int chosen = bestVal;
        bool keepStart = false;
        // Лучшее значение — последнее стабильное перед сбоем?
        bool adjacent = haveFail && pos(failVal) == pos(bestVal) + 1;
        if (adjacent && d->risky && monotonic && cfg.margin > 0) {
            int p = pos(bestVal) - cfg.margin;  // оставляем запас
            if (p >= 0)
                chosen = seq[p];
            else
                keepStart = true;
        } else if (adjacent && d->risky) {
            edge = true;  // без запаса — первый подозреваемый, если длинная проверка не пройдёт
        }
        if (!keepStart) {
            setValue(cur, key, strf("%d", chosen));
            changed = true;
            chosenStr = strf("%d", chosen);
            if (chosen == bestVal)
                curScore = bestScore;
            else
                remeasure = true;
        }
    } else if (haveFail && d->risky && monotonic && cfg.margin > 0 && !seqStartInh && !d->absentIsZero && pos(failVal) == 0) {
        // Уже первая ступень туже стартовой упала — стартовое значение на краю, отступаем на запас.
        int backed = d->order == KnobOrder::Up ? seqStart - cfg.margin : seqStart + cfg.margin;
        backed = std::max(d->min, std::min(d->max, backed));
        if (backed != seqStart) {
            setValue(cur, key, strf("%d", backed));
            changed = true;
            chosenStr = strf("%d", backed);
            remeasure = true;
        }
    }
    if (changed) {
        // Если ручка уже была в списке (значение из пресета) — запоминаем самое первое «прежнее».
        std::string origPrev = prev;
        for (size_t i = 0; i < accepted.size(); i++) {
            if (accepted[i].key == key) {
                origPrev = accepted[i].prev;
                accepted.erase(accepted.begin() + i);
                break;
            }
        }
        accepted.push_back({key, origPrev, edge});
        ev("Принято %s = %s (было %s)%s", key.c_str(), chosenStr.c_str(), prev == "inh" ? "общее" : prev.c_str(),
           remeasure ? " — с запасом, перепроверка" : "");
    } else {
        ev("%s оставлен как был", key.c_str());
    }
    kIdx++;
    resetKnob();
}

// Ослабить одну принятую ручку на ступень (или вернуть к прежнему значению).
bool Tuner::loosenOne(size_t i) {
    if (i >= accepted.size()) return false;
    const std::string key = accepted[i].key;
    const KnobDef* d = findKnob(key);
    if (!d) return false;
    auto it = cur.find(key);
    int now = it != cur.end() ? it->second : 0;
    std::string prev = accepted[i].prev;
    bool toPrev = true;
    int looser = now;
    if (d->order == KnobOrder::Up) {
        looser = now - 1;
        int pv = prev == "inh" ? -1 : (int)toLong(prev, -1);
        toPrev = looser <= pv;
    } else if (d->order == KnobOrder::Down) {
        looser = now + 1;
        int pv = prev == "inh" ? 0 : (int)toLong(prev, 0);
        toPrev = looser >= pv;
    }
    if (toPrev) {
        setValue(cur, key, prev);
        accepted.erase(accepted.begin() + i);
        ev("Ослабление: %s возвращён к %s", key.c_str(), prev == "inh" ? "общему" : prev.c_str());
    } else {
        setValue(cur, key, strf("%d", looser));
        accepted[i].edge = false;
        ev("Ослабление: %s = %d", key.c_str(), looser);
    }
    return true;
}

// Длинная проверка не прошла. Сначала разом ослабляем ручки, принятые вплотную к сбою;
// потом по одной в порядке из гайда Horizon-OC (t8 -> t1 -> t2 -> t3 -> t6 -> t7 -> t4 -> t5),
// а HP Mode и задержки RL/WL — ещё раньше: они «съедают запас» сильнее всего.
bool Tuner::loosenForVerify() {
    bool any = false;
    for (size_t i = accepted.size(); i-- > 0;) {
        const KnobDef* d = findKnob(accepted[i].key);
        if (accepted[i].edge && d && d->risky) {
            loosenOne(i);
            any = true;
        }
    }
    if (any) return true;
    static const char* order[] = {"ramPD", "RL", "WL", "tFAW", "rext", "wext", "tRPab", "tW2P", "tXP", "tCKESR", "WLset",
                                  "tREFI", "tRCD", "tRP", "tRAS", "tRTW", "tWTR", "tRRD", "tRFC"};
    // По кругу: каждая следующая попытка берёт следующую ручку, а не долбит одну и ту же.
    std::vector<size_t> cand;
    for (auto key : order)
        for (size_t i = 0; i < accepted.size(); i++)
            if (accepted[i].key == key) cand.push_back(i);
    if (cand.empty()) return false;
    return loosenOne(cand[(size_t)(loosenRound++) % cand.size()]);
}

void Tuner::addCandidate(u32 hz, const KnobSet& set, const BenchResult& b, double score) {
    Candidate c;
    int maxId = 0;
    for (auto& x : cands) maxId = std::max(maxId, x.id);
    c.id = maxId + 1;
    c.hz = hz;
    c.set = set;
    c.b = b;
    c.synth = score;
    cands.push_back(c);
}

void Tuner::enterGame() {
    bool haveBase = false;
    for (auto& c : cands)
        if (c.baseline) haveBase = true;
    if (!haveBase) {
        Candidate c;
        c.id = 0;
        c.baseline = true;
        cands.insert(cands.begin(), c);
    }
    // Кандидаты — по синтетической оценке.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.baseline != b.baseline) return a.baseline;
        return a.synth > b.synth;
    });
    int real = 0;
    for (auto& c : cands)
        if (!c.baseline) real++;
    if (real == 0) {
        phase = Phase::Failed;
        error = "ни одна частота не прошла длинную проверку";
        ev("%s", error.c_str());
        return;
    }
    phase = Phase::Game;
    status = "Синтетика готова — проверьте кандидатов в TOTK";
    ev("Синтетический подбор закончен: кандидатов %d. Дальше — бенчмарк в лесу короков.", real);
}

// ============================================================== журнал

bool Tuner::saveState(ITarget& t) {
    std::string s;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        s = serialize();
    }
    return t.saveText(paths::state(), s);
}

bool Tuner::loadState(ITarget& t, bool* recoveredHang) {
    if (recoveredHang) *recoveredHang = false;
    std::string text;
    if (!t.loadText(paths::state(), text) || text.empty()) return false;
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!deserialize(text)) {
        LOGE("Журнал подбора %s повреждён — копия сохранена в state.bad.txt, начинаю с чистого листа", paths::state().c_str());
        writeFile(paths::appDir() + "state.bad.txt", text);
        resetAll();
        return false;
    }
    LOGI("Журнал подбора загружен: этап «%s», проб %d, кандидатов %zu, ждём TOTK: %d, активен: %d", phaseName(phase), trials, cands.size(),
         gamePendingCand, gameActiveCand);
    if (pending.valid()) {
        // Проба не завершилась: консоль зависла и перезагрузилась (или приложение закрыли посреди пробы).
        TrialSpec spec = pending;
        pending = TrialSpec();
        TrialOut out;
        out.ok = false;
        out.reason = "зависание / перезагрузка во время пробы";
        ev("Обнаружена прерванная проба: %s", spec.desc.c_str());
        handleResult(spec, out);
        if (recoveredHang) *recoveredHang = true;
        std::string snap = serialize();
        t.saveText(paths::state(), snap);
    }
    return true;
}

std::string Tuner::serialize() const {
    std::string o;
    o += "v=1\n";
    o += strf("phase=%d\n", (int)phase);
    o += "status=" + clean(status) + "\n";
    o += "error=" + clean(error) + "\n";
    o += strf("trials=%d\n", trials);
    o += strf("started=%lld\n", (long long)startedEpoch);
    o += strf("dram=%d\n", chip.dramId);
    {
        std::string l;
        for (u32 hz : ladder) l += (l.empty() ? "" : ",") + strf("%u", hz);
        o += "ladder=" + l + "\n";
    }
    o += "base=" + base.toString() + "\n";
    o += strf("eps=%.6f\n", eps);
    for (auto& b : baseRuns) o += "baserun=" + b.toString() + "\n";
    {
        std::string l;
        for (int i : coarse) l += (l.empty() ? "" : ",") + strf("%d", i);
        o += "coarse=" + l + "\n";
    }
    o += strf("sweep=%d,%d,%d,%.3f\n", scanPos, lastOk, failRun, lastOkBw);
    for (auto& p : points) o += strf("pt=%u|%d|%s|%.6f|%s\n", p.hz, p.status, p.b.toString().c_str(), p.score, clean(p.reason).c_str());
    {
        std::string l;
        for (u32 hz : targets) l += (l.empty() ? "" : ",") + strf("%u", hz);
        o += "targets=" + l + "\n";
    }
    o += strf("tgt=%d,%d,%.6f\n", tIdx, seedStage, inhScore);
    o += "preset=" + knobSetToString(presetSet) + "\n";
    {
        std::string l;
        for (auto& p : plan) l += (l.empty() ? "" : ",") + strf("%s:%d:%d", p.key.c_str(), p.capMax, p.capMin);
        o += "plan=" + l + "\n";
    }
    o += "cur=" + knobSetToString(cur) + "\n";
    o += strf("tim=%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d\n", curScore, kIdx, seqStart, seqStartInh ? 1 : 0, seqPos, haveBest ? 1 : 0,
              bestVal, bestScore, startScore, noimp, haveFail ? 1 : 0, failVal, remeasure ? 1 : 0, remeasureFails, verifyTries, loosenRound);
    for (auto& a : accepted) o += "acc=" + a.key + "|" + a.prev + "|" + (a.edge ? "1" : "0") + "\n";
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        o += strf("cand=%d|%d|%u|%s|%s|%.6f|%d|%s|%d|%d\n", c.id, c.baseline ? 1 : 0, c.hz, knobSetToString(c.set).c_str(),
                  c.b.toString().c_str(), c.synth, c.game, clean(c.note).c_str(), c.crashes, c.attempts);
        for (auto& r : c.runs) o += strf("run=%zu#", i) + r.toString() + "\n";
    }
    o += strf("gpend=%d\n", gamePendingCand);
    o += strf("gactive=%d\n", gameActiveCand);
    o += "gsnap=" + gameSnapshot + "\n";
    o += "pending=" + (pending.valid() ? pending.serialize() : std::string("-")) + "\n";
    for (auto& e : events) o += "ev=" + e + "\n";
    return o;
}

bool Tuner::deserialize(const std::string& text) {
    Settings keep = cfg;
    std::vector<u32> keepLadder = ladder;
    ChipInfo keepChip = chip;
    resetAll();
    cfg = keep;
    chip = keepChip;
    ladder.clear();
    bool sawVersion = false;
    bool oldScan = false;
    for (auto& raw : split(text, '\n')) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "v") sawVersion = v == "1";
        else if (k == "phase") phase = (Phase)toLong(v, 0);
        else if (k == "status") status = v;
        else if (k == "error") error = v;
        else if (k == "trials") trials = (int)toLong(v, 0);
        else if (k == "started") startedEpoch = (s64)strtoll(v.c_str(), nullptr, 10);
        else if (k == "dram") chip.dramId = (int)toLong(v, chip.dramId);
        else if (k == "ladder") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) ladder.push_back((u32)strtoul(x.c_str(), nullptr, 10));
        } else if (k == "base") base.fromString(v);
        else if (k == "eps") eps = toDouble(v, 0.005);
        else if (k == "baserun") {
            BenchResult b;
            if (b.fromString(v) && b.valid) baseRuns.push_back(b);
        } else if (k == "coarse") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) coarse.push_back((int)toLong(x, 0));
        } else if (k == "sweep") {
            auto p = split(v, ',');
            if (p.size() == 4) {
                scanPos = (int)toLong(p[0], 0);
                lastOk = (int)toLong(p[1], -1);
                failRun = (int)toLong(p[2], 0);
                lastOkBw = toDouble(p[3], 0);
            }
        } else if (k == "scan") {
            // Журнал 1.1.0 (грубый + точный поиск): берём только последнюю стабильную ступень.
            auto p = split(v, ',');
            if (p.size() == 6) {
                lastOk = (int)toLong(p[3], -1);
                lastOkBw = toDouble(p[5], 0);
                oldScan = true;
            }
        } else if (k == "pt") {
            auto p = split(v, '|');
            if (p.size() >= 5) {
                FreqPoint fp;
                fp.hz = (u32)strtoul(p[0].c_str(), nullptr, 10);
                fp.status = (int)toLong(p[1], 0);
                fp.b.fromString(p[2]);
                fp.score = toDouble(p[3], 0);
                fp.reason = p[4];
                points.push_back(fp);
            }
        } else if (k == "targets") {
            for (auto& x : split(v, ','))
                if (!trim(x).empty()) targets.push_back((u32)strtoul(x.c_str(), nullptr, 10));
        } else if (k == "tgt") {
            auto p = split(v, ',');
            if (p.size() == 3) {
                tIdx = (int)toLong(p[0], 0);
                seedStage = (int)toLong(p[1], 0);
                inhScore = toDouble(p[2], 0);
            }
        } else if (k == "preset") knobSetFromString(v, presetSet);
        else if (k == "plan") {
            for (auto& x : split(v, ',')) {
                auto q = split(x, ':');
                if (q.size() == 3 && findKnob(q[0])) plan.push_back({q[0], (int)toLong(q[1], 0), (int)toLong(q[2], 0)});
            }
        } else if (k == "cur") knobSetFromString(v, cur);
        else if (k == "tim") {
            auto p = split(v, ',');
            if (p.size() >= 15) {
                curScore = toDouble(p[0], 0);
                kIdx = (int)toLong(p[1], 0);
                seqStart = (int)toLong(p[2], 0);
                seqStartInh = p[3] == "1";
                seqPos = (int)toLong(p[4], 0);
                haveBest = p[5] == "1";
                bestVal = (int)toLong(p[6], 0);
                bestScore = toDouble(p[7], 0);
                startScore = toDouble(p[8], 0);
                noimp = (int)toLong(p[9], 0);
                haveFail = p[10] == "1";
                failVal = (int)toLong(p[11], 0);
                remeasure = p[12] == "1";
                remeasureFails = (int)toLong(p[13], 0);
                verifyTries = (int)toLong(p[14], 0);
                loosenRound = p.size() >= 16 ? (int)toLong(p[15], 0) : 0;
            }
        } else if (k == "acc") {
            auto p = split(v, '|');
            if (p.size() >= 2 && findKnob(p[0])) accepted.push_back({p[0], p[1], p.size() >= 3 && p[2] == "1"});
        } else if (k == "cand") {
            auto p = split(v, '|');
            if (p.size() >= 8) {
                Candidate c;
                c.id = (int)toLong(p[0], 0);
                c.baseline = p[1] == "1";
                c.hz = (u32)strtoul(p[2].c_str(), nullptr, 10);
                knobSetFromString(p[3], c.set);
                c.b.fromString(p[4]);
                c.synth = toDouble(p[5], 0);
                c.game = (int)toLong(p[6], 0);
                c.note = p[7];
                if (p.size() >= 10) {
                    c.crashes = (int)toLong(p[8], 0);
                    c.attempts = (int)toLong(p[9], 0);
                }
                cands.push_back(c);
            }
        } else if (k == "run") {
            size_t hash = v.find('#');
            if (hash != std::string::npos) {
                size_t idx = (size_t)toLong(v.substr(0, hash), -1);
                TotkRun r;
                if (idx < cands.size() && r.fromString(v.substr(hash + 1))) cands[idx].runs.push_back(r);
            }
        } else if (k == "gpend") gamePendingCand = (int)toLong(v, -1);
        else if (k == "gactive") gameActiveCand = (int)toLong(v, -1);
        else if (k == "gsnap") gameSnapshot = v;
        else if (k == "pending") {
            if (v != "-") pending.deserialize(v);
        } else if (k == "ev") events.push_back(v);
    }
    if (ladder.empty()) ladder = keepLadder;
    if (!sawVersion) return false;
    // Проверка целостности: индексы должны попадать в лестницу.
    auto badIdx = [&](int i) { return i >= (int)ladder.size(); };
    for (int i : coarse)
        if (i < 0 || badIdx(i)) return false;
    if (badIdx(lastOk)) return false;
    if (oldScan && phase == Phase::FreqScan) {
        // Поиск 1.1.0 как прогон не продолжить — начинаем прогон заново.
        buildCoarse();
        ev("Журнал прежней версии: прогон частот начат заново");
    }
    if ((phase == Phase::Timing || phase == Phase::Verify) && tIdx >= (int)targets.size()) return false;
    if (phase == Phase::Timing && kIdx > (int)plan.size()) return false;
    computeRanking();
    return true;
}

// ============================================================== проверка в TOTK

// Настройки кандидата (частота РАМ и частоты ЦПУ/ГПУ прогона) всё ещё действуют?
// Сбрасывает их перезагрузка — в том числе сторожем Ryazha-clk после падения игры.
static bool stillApplied(ITarget& t, const Candidate& c) {
    u32 ov = t.overrideHz();
    bool memOk = c.baseline ? ov == 0 : ov == c.hz;
    return memOk && t.gameClocksActive();
}

static std::string baseName(const std::string& path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

bool Tuner::gameApply(ITarget& t, int idx, std::string* err) {
    Candidate c;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) {
            if (err) *err = "нет такого кандидата";
            LOGE("TOTK: применение несуществующего кандидата %d (всего %zu)", idx, cands.size());
            return false;
        }
        c = cands[idx];
    }
    LOGI("TOTK: применяю %s: %s", c.name().c_str(), c.baseline ? "исходные настройки" : knobSetShort(c.set).c_str());
    bool ok;
    if (c.baseline) {
        ok = t.applyOriginal(err);
    } else {
        ApplyInfo ai;
        ok = t.apply(c.hz, c.set, &ai, err);
    }
    if (!ok) {
        LOGE("TOTK: не удалось применить %s: %s", c.name().c_str(), err ? err->c_str() : "?");
        return false;
    }
    std::string clk;
    if (!t.setGameClocks(&clk)) {
        if (err) *err = "не удалось задать частоты ЦПУ/ГПУ для прогона: " + clk;
        LOGE("TOTK: %s", err ? err->c_str() : clk.c_str());
        return false;
    }
    // Короткий замер «как сейчас» — для отчёта.
    bool aborted = false;
    BenchResult b = t.bench(1, &aborted);
    BenchSnapshot snap = takeBenchSnapshot();
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (b.valid && c.baseline) {
            cands[idx].b = b;
            cands[idx].synth = scoreOf(b);
        }
        cands[idx].attempts++;
        gamePendingCand = idx;
        gameActiveCand = idx;
        gameSnapshot = snap.serialize();
        ev("TOTK: применён %s (%s) — ждём результат бенчмарка", cands[idx].name().c_str(), clk.c_str());
    }
    return saveState(t);
}

int Tuner::gameCollect(ITarget& t, std::string* msg) {
    int ret = 0;
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        int idx = gamePendingCand >= 0 ? gamePendingCand : gameActiveCand;
        if (idx < 0 || idx >= (int)cands.size()) {
            if (msg) *msg = "Нет применённого кандидата — выберите его и нажмите A";
            return 0;
        }
        Candidate& c = cands[idx];
        if (trim(gameSnapshot).empty()) {
            // Нет точки отсчёта (журнал прошлой версии) — старые прогоны новыми не считаем, отсчёт с этого момента.
            LOGW("TOTK: нет точки отсчёта файлов результата — начинаю отсчёт сейчас");
            gameSnapshot = takeBenchSnapshot().serialize();
            if (msg) *msg = "Отсчёт прогонов начат заново — запустите бенчмарк";
            return 0;
        }
        BenchSnapshot snap;
        snap.deserialize(gameSnapshot);
        std::string file;
        auto runs = newRunsSince(snap, &file);
        auto crashes = newCrashReports(snap);
        bool applied = stillApplied(t, c);
        bool pending = gamePendingCand == idx;
        LOGI("TOTK сверка: %s, ждём результат=%d, настройки действуют=%d, новых прогонов %zu (%s), новых отчётов о падении %zu",
             c.name().c_str(), pending ? 1 : 0, applied ? 1 : 0, runs.size(), file.empty() ? "-" : baseName(file).c_str(), crashes.size());
        for (auto& f : crashes) LOGW("TOTK: отчёт о падении %s", f.c_str());
        std::string crashName = crashes.empty() ? std::string() : baseName(crashes.back());

        if (!runs.empty() && (pending || applied)) {
            // Прогоны засчитываем, если ждали их или настройки кандидата всё ещё действуют
            // (например, после вылета игру перезапустили прямо из меню HOME).
            int korok = 0;
            for (auto& r : runs) {
                c.runs.push_back(r);
                if (r.korok()) korok++;
            }
            c.game = 1;
            if (!crashes.empty()) {
                c.crashes++;
                c.note = "был отчёт о падении: " + crashName;
            } else {
                c.note.clear();
            }
            if (msg)
                *msg = strf("%s: засчитано прогонов %zu (лес короков: %d) из %s", c.name().c_str(), runs.size(), korok, baseName(file).c_str());
            for (auto& r : runs)
                ev("TOTK %s: %s — кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f", c.name().c_str(), r.location.c_str(), r.frames, r.avg, r.low1,
                   r.low01);
            ret = (int)runs.size();
            changed = true;
        } else if (!runs.empty()) {
            // Прогон есть, но настройки кандидата уже сброшены — неизвестно, на чём он сделан.
            LOGW("TOTK: в %s новый прогон, но настройки %s уже не действуют — не засчитан", baseName(file).c_str(), c.name().c_str());
            if (msg) *msg = "Новый прогон сделан не на настройках кандидата (они сброшены перезагрузкой) — не засчитан. Примените кандидата заново (A).";
            changed = true;
        } else if (!crashes.empty()) {
            c.game = 2;
            c.crashes++;
            c.note = "игра упала (" + crashName + ")" + (applied ? "" : ", консоль перезапускалась");
            if (msg)
                *msg = c.name() + ": игра упала. " +
                       (applied ? "Настройки всё ещё применены — можно прогнать бенчмарк снова (A или из меню HOME)."
                                : "Настройки сброшены — для повтора нажмите A.");
            ev("TOTK %s: падение игры (%s)", c.name().c_str(), crashName.c_str());
            ret = -1;
            changed = true;
        } else if (pending && !applied) {
            c.game = 2;
            c.crashes++;
            c.note = "консоль перезагружалась во время игры";
            if (msg) *msg = c.name() + ": консоль перезагружалась (зависание или сторож Ryazha-clk). Для повтора нажмите A.";
            ev("TOTK %s: перезагрузка во время игры", c.name().c_str());
            ret = -2;
            changed = true;
        } else if (msg) {
            *msg = applied ? "Нового результата бенчмарка пока нет" : "Настройки кандидата сброшены — примените его заново (A)";
        }
        if (changed && pending) gamePendingCand = -1;
        if (!applied && gameActiveCand == idx) {
            LOGI("TOTK: настройки %s больше не действуют", c.name().c_str());
            gameActiveCand = -1;
            changed = true;
        }
        // Новая точка отсчёта: следующие прогоны и падения считаются от этого момента.
        if (changed) gameSnapshot = takeBenchSnapshot().serialize();
        computeRanking();
    }
    if (changed) saveState(t);
    return ret;
}

void Tuner::gameMarkFailed(ITarget& t, int idx, const std::string& why) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) return;
        cands[idx].game = 2;
        cands[idx].crashes++;
        cands[idx].note = why;
        if (gamePendingCand == idx) gamePendingCand = -1;
        gameSnapshot = takeBenchSnapshot().serialize();
        ev("TOTK %s: сбой — %s", cands[idx].name().c_str(), why.c_str());
        computeRanking();
    }
    saveState(t);
}

void Tuner::gameClearRuns(ITarget& t, int idx) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size()) return;
        cands[idx].runs.clear();
        cands[idx].game = 0;
        cands[idx].crashes = 0;
        cands[idx].attempts = 0;
        cands[idx].note.clear();
        LOGI("TOTK: результаты %s очищены", cands[idx].name().c_str());
        computeRanking();
    }
    saveState(t);
}

void Tuner::gameCancelPending(ITarget& t) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        gamePendingCand = -1;
    }
    saveState(t);
}

void Tuner::gameDeactivate(ITarget& t) {
    bool had = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        had = gameActiveCand >= 0 || gamePendingCand >= 0;
        gameActiveCand = -1;
        gamePendingCand = -1;
    }
    if (had) saveState(t);
}

void Tuner::computeRanking() {
    double maxSynth = 0;
    for (auto& c : cands)
        if (!c.baseline) maxSynth = std::max(maxSynth, c.synth);
    struct Agg {
        double f = 0, l1 = 0, l01 = 0;
        bool ok = false;
    };
    std::vector<Agg> agg(cands.size());
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        if (c.game == 2 || c.runs.empty()) continue;  // последний запуск — сбой: до успешного повтора не участвует
        bool anyKorok = false;
        for (auto& r : c.runs)
            if (r.korok()) anyKorok = true;
        int n = 0;
        for (auto& r : c.runs) {
            if (anyKorok && !r.korok()) continue;
            // Кадры точнее целого среднего FPS; если мод их не дал — берём среднее.
            agg[i].f += r.frames > 0 ? r.frames : r.avg * 100.0;
            agg[i].l1 += r.low1;
            agg[i].l01 += r.low01;
            n++;
        }
        if (n > 0) {
            agg[i].f /= n;
            agg[i].l1 /= n;
            agg[i].l01 /= n;
            agg[i].ok = true;
        }
    }
    // База для игры — «как было», если прогнан; иначе лучший по кадрам.
    Agg ref;
    for (size_t i = 0; i < cands.size(); i++)
        if (cands[i].baseline && agg[i].ok) ref = agg[i];
    if (!ref.ok)
        for (size_t i = 0; i < cands.size(); i++)
            if (agg[i].ok && agg[i].f > ref.f) ref = agg[i];
    double maxGame = 0;
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        c.gameScore = 0;
        if (!agg[i].ok || !ref.ok) continue;
        auto rel = [](double a, double b) { return (a > 0 && b > 0) ? a / b : 1.0; };
        c.gameScore = std::pow(rel(agg[i].f, ref.f), 0.5) * std::pow(rel(agg[i].l1, ref.l1), 0.3) * std::pow(rel(agg[i].l01, ref.l01), 0.2);
        maxGame = std::max(maxGame, c.gameScore);
    }
    double wg = cfg.wGame / 100.0;
    for (auto& c : cands) {
        double s = (maxSynth > 0 && c.synth > 0) ? c.synth / maxSynth : 0;
        double g = (maxGame > 0 && c.gameScore > 0) ? c.gameScore / maxGame : 0;
        if (c.game == 2)
            c.combined = 0;
        else if (g > 0 && s > 0)
            c.combined = std::pow(s, 1.0 - wg) * std::pow(g, wg);
        else if (g > 0)
            c.combined = g;
        else
            c.combined = s * (1.0 - wg);  // без игры кандидат не может обогнать проверенных
    }
}

int Tuner::bestCandidate() const {
    // Приоритет: проверен в игре без сбоев > проверен, но сбои были > не проверен. Сбой в последнем запуске — исключён.
    auto tier = [](const Candidate& c) {
        bool tested = c.game == 1 && !c.runs.empty();
        if (tested && c.crashes == 0) return 3;
        if (tested) return 2;
        return 1;
    };
    int best = -1;
    for (size_t i = 0; i < cands.size(); i++) {
        auto& c = cands[i];
        if (c.baseline || c.game == 2) continue;
        if (best < 0) {
            best = (int)i;
            continue;
        }
        int tc = tier(c), tb = tier(cands[best]);
        if (tc > tb || (tc == tb && c.combined > cands[best].combined)) best = (int)i;
    }
    return best;
}

bool Tuner::finalize(ITarget& t, int idx, std::string* err) {
    Candidate c;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (idx < 0 || idx >= (int)cands.size() || cands[idx].baseline) {
            if (err) *err = "выберите кандидата";
            return false;
        }
        c = cands[idx];
    }
    if (!t.commit(c.hz, c.set, err)) {
        LOGE("Запись итога %s не удалась: %s", c.name().c_str(), err ? err->c_str() : "?");
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        phase = Phase::Done;
        gameActiveCand = -1;
        gamePendingCand = -1;
        status = strf("Записано: %s МГц, %s", mhzStr(c.hz).c_str(), knobSetShort(c.set).c_str());
        ev("Итог записан в timings.ini: %s МГц — %s", mhzStr(c.hz).c_str(), knobSetShort(c.set).c_str());
    }
    saveState(t);
    t.saveText(paths::report(), report());
    return true;
}

std::string Tuner::report() const {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string o;
    o += strf("%s %s — отчёт подбора памяти\n", APP_NAME, APP_VER);
    o += strf("Чип: %s (DRAM ID %d)\n", chip.name.c_str(), chip.dramId);
    o += strf("Этап: %s\n", phaseName(phase));
    if (!error.empty()) o += "Ошибка: " + error + "\n";
    o += strf("Проб выполнено: %d, порог шума %.2f %%\n", trials, eps * 100);
    if (base.valid) o += strf("База (%s МГц): поток %.0f МБ/с, задержка %.1f нс\n\n", mhzStr(stockHz()).c_str(), base.bw(), base.latNs);
    if (!ladder.empty())
        o += strf("Лестница Ryazha-clk: %zu ступеней, %s … %s МГц; прогон: %d … %s МГц\n", ladder.size(), mhzStr(ladder.front()).c_str(),
                  mhzStr(ladder.back()).c_str(), cfg.minMhz, cfg.maxMhz ? strf("%d", cfg.maxMhz).c_str() : "верх лестницы");
    {
        int nOk = 0, nSlow = 0, nFail = 0;
        for (auto& p : points) {
            if (p.status == 1) nOk++;
            else if (p.status == 3) nSlow++;
            else nFail++;
        }
        o += strf("Итог прогона: стабильно %d, медленнее предыдущей %d, сбой %d\n", nOk, nSlow, nFail);
    }
    o += "Частоты (общие тайминги кипа):\n";
    for (auto& p : points)
        o += strf("  %8s МГц  %-8s  поток %6.0f МБ/с  задержка %6.1f нс  оценка %.3f  %s\n", mhzStr(p.hz).c_str(),
                  p.status == 1 ? "ок" : (p.status == 3 ? "медленнее" : "сбой"), p.b.bw(), p.b.latNs, p.score, p.reason.c_str());
    o += "\nКандидаты:\n";
    for (auto& c : cands) {
        o += strf("  %s: %s\n", c.name().c_str(), c.baseline ? "настройки до подбора" : knobSetShort(c.set).c_str());
        if (c.b.valid)
            o += strf("     синтетика: чтение %.0f, запись %.0f, копирование %.0f МБ/с, задержка %.1f / %.1f нс, оценка %.3f\n", c.b.readMBs,
                      c.b.writeMBs, c.b.copyMBs, c.b.latNs, c.b.lat2Ns, c.synth);
        for (auto& r : c.runs)
            o += strf("     TOTK %s: кадров %.0f, средний %.0f, 1%% %.0f, 0.1%% %.0f\n", r.location.c_str(), r.frames, r.avg, r.low1, r.low01);
        if (c.attempts || c.crashes) o += strf("     запусков игры: %d, сбоев: %d\n", c.attempts, c.crashes);
        if (c.game == 2) o += "     СБОЙ В ПОСЛЕДНЕМ ЗАПУСКЕ: " + c.note + "\n";
        else if (!c.note.empty()) o += "     примечание: " + c.note + "\n";
        if (c.combined > 0) o += strf("     итоговая оценка %.3f\n", c.combined);
        if (!c.baseline && !c.set.empty()) {
            o += "     секция для timings.ini:\n";
            TimingsIni ini;
            ini.setSection(hzToKhz(c.hz), c.set, nullptr);
            for (auto& l : split(ini.serialize(), '\n'))
                if (!l.empty()) o += "       " + l + "\n";
        }
    }
    int b = bestCandidate();
    if (b >= 0) o += strf("\nРекомендация: %s — %s\n", cands[b].name().c_str(), knobSetShort(cands[b].set).c_str());
    o += "\nПодробный журнал приложения (ошибки, IPC, файлы): " + logx::file() + "\n";
    o += "\nСобытия подбора:\n";
    for (auto& e : events) o += "  " + e + "\n";
    return o;
}
