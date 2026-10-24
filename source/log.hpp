// Журнал приложения: внутренние ошибки, предупреждения и ход работы.
// Файл: sdmc:/config/ram-sweetspot/logs/ramss.log (больше 1 МБ — уходит в ramss.old.log).
#pragma once

#include "common.hpp"

namespace logx {

// Начать сеанс: заголовок с версией и временем.
void begin();
void write(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
std::string file();     // полный путь к журналу
std::string oldFile();  // предыдущий журнал после ротации
// Последние строки журнала (для экрана «Сервис»).
std::vector<std::string> tail(int lines);
// Сколько ошибок записано за этот сеанс.
int errorsThisSession();

}  // namespace logx

#define LOGI(...) logx::write('I', __VA_ARGS__)
#define LOGW(...) logx::write('W', __VA_ARGS__)
#define LOGE(...) logx::write('E', __VA_ARGS__)
