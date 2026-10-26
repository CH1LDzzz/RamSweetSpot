# Проверка алгоритма на ПК (macOS/Linux): make -C tests run
CXX ?= c++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -pthread
SRC := ../source/util.cpp ../source/log.cpp ../source/knobs.cpp ../source/dram.cpp ../source/membench.cpp \
       ../source/totk.cpp ../source/tuner.cpp ../source/history.cpp ../source/archive.cpp sim.cpp
SDROOT ?= ./sdroot

sim: $(SRC) $(wildcard ../source/*.hpp)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

run: sim
	rm -rf $(SDROOT) && mkdir -p $(SDROOT)
	./sim $(SDROOT)

clean:
	rm -rf sim $(SDROOT)

.PHONY: run clean

# Предпросмотр экранов: сначала make run (снимки состояния), потом make preview
PREVIEW_SRC := ../source/util.cpp ../source/log.cpp ../source/knobs.cpp ../source/dram.cpp ../source/membench.cpp ../source/totk.cpp \
               ../source/tuner.cpp ../source/gfx.cpp ../source/draw.cpp ../source/app.cpp ../source/console.cpp \
               ../source/rclk.cpp ../source/daemonlog.cpp preview.cpp
SHOTS ?= ./shots
previewbin: $(PREVIEW_SRC) $(wildcard ../source/*.hpp)
	$(CXX) $(CXXFLAGS) -Wno-unused-function $(PREVIEW_SRC) -o $@
preview: previewbin
	./previewbin $(SDROOT) $(SHOTS)
	for f in $(SHOTS)/*.ppm; do sips -s format png "$$f" --out "$${f%.ppm}.png" >/dev/null; done
.PHONY: preview
