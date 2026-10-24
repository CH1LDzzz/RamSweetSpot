// Ручки таймингов Ryazha-clk (timings.ini) и работа с файлом.
#pragma once

#include "common.hpp"

// Набор значений ручек для одной частоты. Ключа нет — берётся общее значение из кипа.
typedef std::map<std::string, int> KnobSet;

enum class KnobOrder {
    Up,       // выше = туже (первичные тайминги: номер ступени)
    Down,     // ниже = быстрее (знаковые задержки: RL, WL, tFAW ...)
    Explore,  // направление заранее неизвестно — перебор всех значений
    Toggle,   // переключатель (HP Mode)
};

struct KnobDef {
    const char* key;      // ключ в timings.ini
    const char* label;    // подпись в интерфейсе
    int min, max;         // допустимый диапазон в файле
    KnobOrder order;
    bool absentIsZero;    // нет ключа == 0 (знаковые поправки); иначе неизвестное значение кипа
    bool risky;           // может портить данные — нужен запас при подборе
    bool primary;         // первичный тайминг
    bool advanced;        // только в расширенном режиме автоподбора
    const char* hint;     // короткая подсказка
};

const std::vector<KnobDef>& allKnobs();
const KnobDef* findKnob(const std::string& key);

// Человеческое значение ступени: "tRCD 4" -> "14 нс".
std::string knobValueText(const KnobDef& k, int v);

// Ручки для автоподбора в рекомендованном порядке (раздел 10.6 руководства Ryazha-clk).
struct AutoKnob {
    const KnobDef* def;
    int capMax;   // для Up: максимальный номер ступени при автоподборе
    int capMin;   // для Down: минимальное значение при автоподборе
};
std::vector<AutoKnob> autoKnobPlan(bool allowTrefiHigh, bool advanced);

std::string knobSetToString(const KnobSet& s);          // "tRCD:4,tRP:3" или "-"
bool knobSetFromString(const std::string& str, KnobSet& out);
std::string knobSetShort(const KnobSet& s);              // для таблиц: "tRCD4 tRP3 RL-1"

// timings.ini: разбор и запись с сохранением комментариев и чужих секций.
class TimingsIni {
public:
    struct Section {
        std::string header;              // строка заголовка как в файле
        u32 khz = 0;                     // частота секции
        std::vector<std::string> lines;  // строки внутри секции (без заголовка)
    };

    bool parse(const std::string& text);
    std::string serialize() const;

    // Заменить/создать секцию частоты. Пустой набор удаляет секцию.
    void setSection(u32 khz, const KnobSet& set, const char* tag);
    void removeSection(u32 khz);
    bool getSection(u32 khz, KnobSet& out) const;
    bool hasSection(u32 khz) const;

    static u32 parseHeaderKhz(const std::string& inner, bool* ok);
    static std::string headerFor(u32 khz);

    std::vector<std::string> preamble;
    std::vector<Section> sections;
};

// Исходный timings.ini пользователя + наша секция для частоты (пустой набор — секции этой частоты нет).
std::string composeTimings(const std::string& original, u32 khz, const KnobSet& set, const char* tag);
