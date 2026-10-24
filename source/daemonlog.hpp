// Лог демона Ryazha-clk (config/ryazha-clk/log.txt): подтверждение, что тайминги применены.
#pragma once

#include "common.hpp"

class DaemonLog {
public:
    // Включить лог (пустой log.flag). Флаг, созданный нами, уберём в restore().
    void enable();
    void restore();
    bool createdFlag() const { return m_created; }

    // Запомнить текущий конец лога.
    void mark();
    // Ждать после mark() строки о применении таймингов. false — подтверждения не было.
    bool waitApplied(int timeoutMs, std::vector<std::string>* notApplied, std::string* lastLine);
    // Лог растёт (демон его пишет)?
    bool alive();
    // Последние строки лога (для экрана диагностики).
    std::vector<std::string> tail(int lines);

private:
    std::string readSince(u64 offset, u64* newEnd);
    u64 m_mark = 0;
    bool m_created = false;
    u64 m_lastSize = 0;
    u64 m_lastCheck = 0;
    bool m_alive = false;
};
