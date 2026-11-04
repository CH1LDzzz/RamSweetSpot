#include "daemonlog.hpp"

#include <cstdio>

// Метка «log.flag создали мы» лежит у нас в папке — переживает перезапуск приложения.
static std::string ownMarker() { return paths::appDir() + "log.flag.ours"; }

void DaemonLog::enable() {
    if (fileExists(paths::rclkLogFlag())) return;
    makeDirs(paths::rclkDir());
    m_created = writeFile(paths::rclkLogFlag(), "");
    if (m_created) {
        makeDirs(paths::appDir());
        writeFile(ownMarker(), "");
    }
}

void DaemonLog::restore() {
    if (m_created || fileExists(ownMarker())) {
        removeFile(paths::rclkLogFlag());
        removeFile(ownMarker());
    }
    m_created = false;
}

std::string DaemonLog::readSince(u64 offset, u64* newEnd) {
    std::string out;
    FILE* f = fopen(paths::rclkLog().c_str(), "rb");
    if (!f) {
        *newEnd = 0;
        return out;
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end < 0) end = 0;
    if ((u64)end < offset) offset = 0;  // лог начат заново
    // Больше 256 КБ за раз не читаем — нужен только свежий хвост.
    if ((u64)end - offset > 256 * 1024) offset = (u64)end - 256 * 1024;
    fseek(f, (long)offset, SEEK_SET);
    out.resize((size_t)((u64)end - offset));
    size_t n = out.empty() ? 0 : fread(&out[0], 1, out.size(), f);
    out.resize(n);
    fclose(f);
    *newEnd = offset + n;
    return out;
}

void DaemonLog::mark() {
    u64 size = 0;
    if (fileStat(paths::rclkLog(), &size, nullptr))
        m_mark = size;
    else
        m_mark = 0;
}

bool DaemonLog::alive() {
    u64 now = nowMs();
    if (now - m_lastCheck < 2000 && m_lastCheck) return m_alive;
    u64 size = 0;
    bool ok = fileStat(paths::rclkLog(), &size, nullptr);
    if (ok && m_lastCheck && size != m_lastSize) m_alive = true;
    m_lastSize = size;
    m_lastCheck = now;
    return m_alive;
}

bool DaemonLog::waitApplied(int timeoutMs, std::vector<std::string>* notApplied, std::string* lastLine) {
    u64 deadline = nowMs() + (u64)timeoutMs;
    u64 off = m_mark;
    bool reread = false;
    u64 rereadAt = 0;
    std::string carry;
    while (nowMs() < deadline) {
        sleepMs(250);
        u64 end = 0;
        std::string chunk = readSince(off, &end);
        if (end < off) carry.clear();
        off = end;
        if (chunk.empty()) {
            if (reread && nowMs() - rereadAt > 2500) return true;  // перечитал, но строки «применены» в этой версии нет
            continue;
        }
        m_alive = true;
        carry += chunk;
        size_t nl;
        while ((nl = carry.find('\n')) != std::string::npos) {
            std::string line = trim(carry.substr(0, nl));
            carry.erase(0, nl + 1);
            if (line.empty()) continue;
            if (line.find("[hw]") != std::string::npos && line.find("НЕ ДОШЛО") != std::string::npos) {
                if (notApplied) notApplied->push_back(line.substr(line.find("[hw]")));
            }
            bool applied = line.find("тайминги применены живьём") != std::string::npos ||
                           line.find("[gen] записано") != std::string::npos ||
                           line.find("[ergen] живые тайминги") != std::string::npos ||
                           line.find("это стоковая таблица") != std::string::npos;
            if (applied) {
                if (lastLine) *lastLine = line;
                // Дадим демону полсекунды дописать сверку регистров "[hw] ...".
                sleepMs(600);
                u64 e2 = 0;
                std::string more = readSince(off, &e2);
                for (auto& l : split(more, '\n'))
                    if (l.find("[hw]") != std::string::npos && l.find("НЕ ДОШЛО") != std::string::npos && notApplied)
                        notApplied->push_back(trim(l.substr(l.find("[hw]"))));
                return true;
            }
            if (line.find("поправки таймингов перечитаны") != std::string::npos || line.find("кнобы изменились") != std::string::npos) {
                reread = true;
                rereadAt = nowMs();
                if (lastLine) *lastLine = line;
            }
        }
    }
    return reread;
}

std::vector<std::string> DaemonLog::tail(int lines) {
    std::vector<std::string> out;
    u64 size = 0;
    if (!fileStat(paths::rclkLog(), &size, nullptr)) return out;
    u64 end = 0;
    std::string data = readSince(size > 16384 ? size - 16384 : 0, &end);
    auto all = split(data, '\n');
    for (int i = (int)all.size() - 1; i >= 0 && (int)out.size() < lines; i--) {
        std::string l = trim(all[i]);
        if (!l.empty()) out.insert(out.begin(), l);
    }
    return out;
}
