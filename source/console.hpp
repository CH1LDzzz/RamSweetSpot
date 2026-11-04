// Реализация ITarget на консоли: Ryazha-clk + замеры + memtest.
#pragma once

#include <atomic>

#include "daemonlog.hpp"
#include "membench.hpp"
#include "rclk.hpp"
#include "tuner.hpp"

class ConsoleTarget : public ITarget {
public:
    MemBench membench;
    MemTest memtest;
    DaemonLog dlog;
    Settings* cfg = nullptr;

    std::atomic<bool> abort{false};      // пауза / потеря фокуса — прервать замер
    std::atomic<bool> measuring{false};  // идёт замер — интерфейс не рисует, чтобы не мешать
    std::string lastNote;                // последнее сообщение о применении (для экрана)

    // Выделить буферы. Возвращает текст о том, сколько памяти взято.
    bool initBuffers(std::string* info);
    // Исходный timings.ini пользователя. fresh — новый подбор: снять копию заново.
    void prepareOriginal(bool fresh);
    const std::string& original() const { return m_original; }
    bool originalAbsent() const { return m_absent; }

    bool apply(u32 hz, const KnobSet& set, ApplyInfo* info, std::string* err) override;
    bool applyOriginal(std::string* err) override;
    bool commit(u32 hz, const KnobSet& set, std::string* err) override;
    bool stability(int seconds, std::string* err, bool* aborted) override;
    BenchResult bench(int passes, bool* aborted) override;
    u32 realHz() override;
    u32 overrideHz() override;
    int socTempC() override;
    bool saveText(const std::string& path, const std::string& text) override;
    bool loadText(const std::string& path, std::string& text) override;
    void idle(u32 ms) override;
    bool kipRead(std::map<int, int>* slots, std::string* err) override;
    bool kipWrite(const std::map<int, int>& slots, std::string* err) override;
    bool reboot(std::string* err) override;
    u64 uptimeMs() override;
    std::string sensors() override;
    int watchdogState() override;
    bool safeMode() override;
    bool kipCommonRead(KnobSet* vals, std::string* err) override;
    bool kipCommonWrite(const KnobSet& vals, std::string* err) override;
    bool kipCommonMax(const std::string& key, int* max) override;

    // Частоты ЦПУ/ГПУ, которые держит приложение через временные значения Ryazha-clk:
    //   Free   — ничего не фиксируем;
    //   Tuning — ЦПУ на частоте подбора РАМ (по умолчанию 2397 МГц), иначе говернёр и буст дают шум;
    //   Game   — ЦПУ и ГПУ на частотах прогона TOTK (2397 / 1305 МГц), одинаково для всех кандидатов.
    enum class ClockMode { Free, Tuning, Game };
    bool setClockMode(ClockMode m, std::string* note = nullptr);
    ClockMode clockMode() const { return m_mode; }
    // После перезапуска приложения: режим уже выставлен демону (например, вернулись из TOTK).
    void assumeClockMode(ClockMode m) { m_mode = m; }
    void lockCpu(bool on) { setClockMode(on ? ClockMode::Tuning : ClockMode::Free); }
    // Ближайшая доступная у демона частота не выше заданной (МГц). 0 — модуль не трогаем.
    u32 pickHz(u32 module, int mhz, std::string* note);
    bool setGameClocks(std::string* note) override;
    void freeClocks() override { setClockMode(ClockMode::Free); }
    bool gameClocksActive() override;

    // Для ручного режима: только частота / только тайминги.
    bool setFreq(u32 hz, u32* realOut, std::string* err);
    bool writeTimings(u32 hz, const KnobSet& set, ApplyInfo* info);

private:
    bool waitFreq(u32 hz, u32* realOut, std::string* err);
    std::string m_original;
    bool m_absent = false;
    std::string m_lastWritten;
    bool m_haveLast = false;
    ClockMode m_mode = ClockMode::Free;
    std::vector<u32> m_lists[2];  // частоты ЦПУ и ГПУ от демона
};

static const char* const kTagTrial = "RamSweetSpot: проба";
static const char* const kTagFinal = "RamSweetSpot: итог подбора";
