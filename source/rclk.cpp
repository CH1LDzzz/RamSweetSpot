#include "rclk.hpp"

#include "log.hpp"

#include <cstring>
#include <mutex>

#ifdef __SWITCH__

namespace {
Service g_srv;
bool g_open = false;
std::mutex g_ipc;  // интерфейс и рабочий поток ходят в демон одновременно

// Ошибка IPC — в журнал. Одинаковые подряд пишем не чаще раза в 30 с (контекст опрашивается каждую секунду).
void logRc(const char* what, Result rc) {
    static std::string lastWhat;
    static Result lastRc = 0;
    static u64 lastAt = 0;
    u64 now = nowMs();
    if (rc == lastRc && lastWhat == what && now - lastAt < 30000) return;
    lastWhat = what;
    lastRc = rc;
    lastAt = now;
    LOGE("IPC %s: ошибка 0x%X (%04u-%04u)", what, rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
}

enum Cmd : u32 {
    Cmd_GetApiVersion = 0,
    Cmd_GetVersionString = 1,
    Cmd_GetCurrentContext = 2,
    Cmd_GetProfiles = 5,
    Cmd_SetProfiles = 6,
    Cmd_SetOverride = 8,
    Cmd_GetFreqList = 11,
};

struct SetOverrideArgs {
    u32 module;
    u32 hz;
};
struct GetFreqListArgs {
    u32 module;
    u32 maxCount;
};
struct SetProfilesArgs {
    u64 tid;
    u32 mhz[25];
};
static_assert(sizeof(SetProfilesArgs) == 0x70, "SetProfiles: демон ждёт 0x70 байт");

// Служба зарегистрирована? Как в оверлеях sys-clk/hoc-clk: если зарегистрировать имя удалось — её нет.
// smGetService на незарегистрированное имя в Atmosphère не возвращает ошибку, а ждёт вечно,
// поэтому «запущена» — только при явном ответе sm «уже зарегистрировано» (2021-0004, 0x815).
bool serviceRunning(const char* name) {
    Handle h = INVALID_HANDLE;
    SmServiceName sn = smEncodeName(name);
    Result rc = smRegisterService(&h, sn, false, 1);
    if (R_SUCCEEDED(rc)) {
        svcCloseHandle(h);
        smUnregisterService(sn);
        return false;
    }
    const Result kSmAlreadyRegistered = MAKERESULT(21, 4);  // модуль sm = 21
    return rc == kSmAlreadyRegistered;
}
}  // namespace

namespace rclk {

bool init(std::string* svcName) {
    if (g_open) return true;
    static const char* names[] = {"ryz:clk", "hoc:clk"};
    for (auto n : names) {
        if (!serviceRunning(n)) {
            LOGI("Служба %s не зарегистрирована", n);
            continue;
        }
        Result rc = smGetService(&g_srv, n);
        if (R_SUCCEEDED(rc)) {
            u32 ver = 0;
            g_open = true;
            if (!apiVersion(&ver)) {
                LOGE("Служба %s не ответила на GetApiVersion", n);
                serviceClose(&g_srv);
                g_open = false;
                continue;
            }
            if (svcName) *svcName = n;
            LOGI("Подключено к %s, API %u", n, ver);
            return true;
        }
        logRc(n, rc);
    }
    LOGE("Демон Ryazha-clk / Horizon-OC не найден");
    return false;
}

void exit() {
    if (g_open) serviceClose(&g_srv);
    g_open = false;
}

bool connected() { return g_open; }

bool apiVersion(u32* out) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    Result rc = serviceDispatchOut(&g_srv, Cmd_GetApiVersion, *out);
    if (R_FAILED(rc)) logRc("GetApiVersion", rc);
    return R_SUCCEEDED(rc);
}

bool versionString(std::string* out) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    char buf[0x40] = {};
    Result rc = serviceDispatch(&g_srv, Cmd_GetVersionString, .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
                                .buffers = {{buf, sizeof(buf) - 1}}, );
    if (R_FAILED(rc)) {
        logRc("GetVersionString", rc);
        return false;
    }
    *out = buf;
    return true;
}

bool context(RclkContext* out) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    alignas(8) static u8 raw[0x500];
    memset(raw, 0, sizeof(raw));
    Result rc = serviceDispatch(&g_srv, Cmd_GetCurrentContext, .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
                                .buffers = {{raw, sizeof(raw)}}, );
    if (R_FAILED(rc)) {
        logRc("GetCurrentContext", rc);
        return false;
    }
    // Раскладка «стабильной» структуры hoc-clk: проверена по записи частоты ЦПУ в смещения 0x00 и 0xC4.
    memcpy(out->freqs, raw + 0x00, sizeof(out->freqs));
    memcpy(out->realFreqs, raw + 0x14, sizeof(out->realFreqs));
    memcpy(out->overrideFreqs, raw + 0x28, sizeof(out->overrideFreqs));
    memcpy(out->temps, raw + 0x3C, sizeof(out->temps));
    memcpy(out->power, raw + 0x68, sizeof(out->power));
    memcpy(out->voltages, raw + 0x98, sizeof(out->voltages));
    memcpy(&out->applicationId, raw + 0xB8, sizeof(out->applicationId));
    memcpy(&out->profile, raw + 0xC0, sizeof(out->profile));
    out->valid = true;
    return true;
}

bool freqList(u32 module, std::vector<u32>* out) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    static u32 list[64];
    GetFreqListArgs args = {module, 64};
    u32 count = 0;
    Result rc = serviceDispatchInOut(&g_srv, Cmd_GetFreqList, args, count, .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
                                     .buffers = {{list, sizeof(list)}}, );
    if (R_FAILED(rc)) {
        logRc(module == RclkModule_MEM ? "GetFreqList(MEM)" : (module == RclkModule_CPU ? "GetFreqList(CPU)" : "GetFreqList(GPU)"), rc);
        return false;
    }
    out->clear();
    for (u32 i = 0; i < count && i < 64; i++)
        if (list[i]) out->push_back(list[i]);
    return !out->empty();
}

bool setOverride(u32 module, u32 hz) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open || module > RclkModule_MEM) return false;
    SetOverrideArgs args = {module, hz};
    Result rc = serviceDispatchIn(&g_srv, Cmd_SetOverride, args);
    if (R_FAILED(rc)) logRc(module == RclkModule_MEM ? "SetOverride(MEM)" : (module == RclkModule_CPU ? "SetOverride(CPU)" : "SetOverride(GPU)"), rc);
    return R_SUCCEEDED(rc);
}

bool getProfiles(u64 tid, u32 mhz[25]) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    u32 buf[25] = {};
    Result rc = serviceDispatchIn(&g_srv, Cmd_GetProfiles, tid, .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
                                  .buffers = {{buf, sizeof(buf)}}, );
    if (R_FAILED(rc)) {
        logRc("GetProfiles", rc);
        return false;
    }
    memcpy(mhz, buf, sizeof(buf));
    return true;
}

bool setProfiles(u64 tid, const u32 mhz[25]) {
    std::lock_guard<std::mutex> lk(g_ipc);
    if (!g_open) return false;
    SetProfilesArgs args;
    memset(&args, 0, sizeof(args));
    args.tid = tid;
    memcpy(args.mhz, mhz, sizeof(args.mhz));
    Result rc = serviceDispatchIn(&g_srv, Cmd_SetProfiles, args);
    if (R_FAILED(rc)) logRc("SetProfiles", rc);
    return R_SUCCEEDED(rc);
}

}  // namespace rclk

#else  // ПК: демона нет

namespace rclk {
bool init(std::string*) { return false; }
void exit() {}
bool connected() { return false; }
bool apiVersion(u32*) { return false; }
bool versionString(std::string*) { return false; }
bool context(RclkContext*) { return false; }
bool freqList(u32, std::vector<u32>*) { return false; }
bool setOverride(u32, u32) { return false; }
bool getProfiles(u64, u32*) { return false; }
bool setProfiles(u64, const u32*) { return false; }
}  // namespace rclk

#endif
