// RamSweetSpot — автоподбор «свитспота» памяти Nintendo Switch для Ryazha-clk (Ряженка CFW).
#include <cstdio>

#include "app.hpp"

#ifdef __SWITCH__

// Приложение ставит свои буферы под memtest — стандартной кучи newlib достаточно.
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static App app;  // большой объект — не на стеке
    if (!app.init()) {
        // Графика не поднялась — показываем ошибку текстовой консолью.
        consoleInit(nullptr);
        printf("RamSweetSpot: не удалось инициализировать графику.\nPress + to exit.\n");
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        return 0;
    }
    app.run();
    app.shutdown();
    return 0;
}

#endif
