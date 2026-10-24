// Бенчмарк TOTK (мод UltraCam): разбор результата, отслеживание новых прогонов, запуск игры.
#pragma once

#include "common.hpp"

#define TOTK_TITLE_ID 0x0100F2C0115B6000ULL

struct TotkRun {
    std::string location;  // "Korok Forest"
    double frames = 0;     // всего кадров (точнее среднего FPS: тот целый)
    double avg = 0;        // средний FPS
    double low1 = 0;       // 1% low
    double low01 = 0;      // 0.1% low
    bool korok() const;
    std::string toString() const;
    bool fromString(const std::string& s);
};

// Разбор текста файла: все блоки "BENCHMARK FOR <место> COMPLETED!" + строка с кадрами.
std::vector<TotkRun> parseTotkBench(const std::string& text);

// Снимок файлов результата до запуска игры, чтобы потом понять, что появилось нового.
struct BenchSnapshot {
    struct File {
        std::string path;
        bool exists = false;
        u64 size = 0;
        s64 mtime = 0;
        u64 hash = 0;
        int runs = 0;
    };
    std::vector<File> files;
    std::vector<std::string> crashFiles;  // отчёты о падениях на момент снимка
    s64 epoch = 0;
    std::string serialize() const;
    bool deserialize(const std::string& s);
};

// Имена файлов, которые ищем в корне карты. Пользователь назвал TOTKBench.txt,
// а мод в архиве пишет sd:/TOTKBenchmark.txt — понимаем оба.
std::vector<std::string> benchFileCandidates();
BenchSnapshot takeBenchSnapshot();
// Новые прогоны после снимка. latestFile — откуда взяты.
std::vector<TotkRun> newRunsSince(const BenchSnapshot& before, std::string* latestFile);
// Последний прогон из самого свежего файла (для экрана статуса).
bool lastRun(TotkRun* out, std::string* file);
// Новые отчёты о падениях Atmosphère с момента снимка.
std::vector<std::string> newCrashReports(const BenchSnapshot& before);

// maxlastbreath.ini мода UltraCam: где лежит и какой бенчмарк выбран (4 = Korok Forest).
std::string findUltraCamIni();
int readBenchmarkIndex(const std::string& iniPath);
bool writeBenchmarkIndex(const std::string& iniPath, int index);
const char* benchmarkName(int index);

// Запуск TOTK. false + текст ошибки, если система не дала.
bool launchTotk(std::string* err);
