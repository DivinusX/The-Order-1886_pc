// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_perf_stats.h"
#include "ps4_frame_trace.h"

#include <chrono>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800 // Windows 8+; older headers lack it
#endif
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#endif

namespace Ps4PerfStats {

namespace {

std::mutex mutex;
Sample latest;
PresentationCounter guest_output;
std::once_flag started;
std::atomic<unsigned> requested_items{DefaultItems};

FrameTrace& Trace() {
    static FrameTrace trace(std::getenv("ORDER1886_FRAME_TRACE"));
    return trace;
}

constexpr double GiB = 1024.0 * 1024.0 * 1024.0;

// NVIDIA's management library, loaded at run time when the driver ships it (any NVIDIA
// driver on Linux and Windows). Gives the temperature and load of the card Vulkan uses; the
// proprietary Linux driver exposes neither through sysfs.
class Nvml {
public:
    Nvml(uint32_t vendor_id, uint32_t device_id) {
        if (vendor_id != 0x10de) {
            return;
        }
#ifdef _WIN32
        lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto sym = [&](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
        };
#else
        lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        const auto sym = [&](const char* name) { return dlsym(lib, name); };
#endif
        if (!lib) {
            return;
        }
        auto init = reinterpret_cast<int (*)()>(sym("nvmlInit_v2"));
        auto count = reinterpret_cast<int (*)(unsigned*)>(sym("nvmlDeviceGetCount_v2"));
        auto by_index = reinterpret_cast<int (*)(unsigned, void**)>(sym("nvmlDeviceGetHandleByIndex_v2"));
        auto pci = reinterpret_cast<int (*)(void*, PciInfo*)>(sym("nvmlDeviceGetPciInfo_v3"));
        temperature = reinterpret_cast<int (*)(void*, int, unsigned*)>(sym("nvmlDeviceGetTemperature"));
        utilization = reinterpret_cast<int (*)(void*, Utilization*)>(sym("nvmlDeviceGetUtilizationRates"));
        unsigned n = 0;
        if (!init || !count || !by_index || !temperature || init() != 0 || count(&n) != 0) {
            temperature = nullptr;
            return;
        }
        for (unsigned i = 0; i < n && !device; ++i) {
            void* handle = nullptr;
            PciInfo info{};
            if (by_index(i, &handle) != 0) continue;
            // pciDeviceId is (device << 16) | vendor. Without the PCI query take the first card.
            if (!pci || pci(handle, &info) != 0 || (info.pci_device_id >> 16) == device_id) {
                device = handle;
            }
        }
    }

    float Temperature() const {
        unsigned value = 0;
        return device && temperature && temperature(device, 0, &value) == 0 ? float(value) : -1.0f;
    }

    float Load() const {
        Utilization value{};
        return device && utilization && utilization(device, &value) == 0 ? float(value.gpu) : -1.0f;
    }

private:
    struct PciInfo {
        char bus_id_legacy[16];
        unsigned domain, bus, device, pci_device_id, pci_sub_system_id;
        char bus_id[32];
    };
    struct Utilization {
        unsigned gpu, memory;
    };
    void* lib = nullptr;
    void* device = nullptr;
    int (*temperature)(void*, int, unsigned*) = nullptr;
    int (*utilization)(void*, Utilization*) = nullptr;
};

#ifdef _WIN32
uint64_t Ticks(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// This process's use of the GPU's 3D engines, as Task Manager shows it ("GPU Engine" counters,
// any vendor). Negative until the second sample or without the counters.
class GpuUsage {
public:
    GpuUsage() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) !=
            ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        PdhCollectQueryData(query);
        pid = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    }

    float Percent() {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return -1.0f;
        }
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         nullptr) != PDH_MORE_DATA) {
            return -1.0f;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         items) != ERROR_SUCCESS) {
            return -1.0f;
        }
        double total = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            const wchar_t* name = items[i].szName;
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && wcsstr(name, pid.c_str()) &&
                wcsstr(name, L"engtype_3D")) {
                total += items[i].FmtValue.doubleValue;
            }
        }
        return std::clamp(float(total), 0.0f, 100.0f);
    }

private:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::wstring pid;
    std::vector<unsigned char> buffer;
};

std::string CpuName() {
    char name[256]{};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
        return ShortCpuName(name);
    }
    return {};
}

void Run(const Vulkan::Instance* instance) {
    std::unique_ptr<GpuUsage> gpu;
    const Nvml nvml(instance->GetVendorID(), instance->GetDeviceID());
    const std::string cpu_name = CpuName();
    FILETIME created, ended, kernel, user, sys_idle, sys_kernel, sys_user;
    uint64_t last_process = 0, last_wall = 0, last_idle = 0, last_busy_total = 0;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    while (true) {
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        s.cpu_name = cpu_name;
        if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
            const uint64_t process = Ticks(kernel) + Ticks(user);
            const uint64_t wall = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()) / 100;
            if (last_wall && wall > last_wall && process >= last_process) {
                s.cpu_percent = CpuPercent(process - last_process, wall - last_wall, cores);
            }
            last_process = process;
            last_wall = wall;
        }
        // Whole-system CPU load (kernel time includes idle time).
        if (GetSystemTimes(&sys_idle, &sys_kernel, &sys_user)) {
            const uint64_t idle = Ticks(sys_idle), total = Ticks(sys_kernel) + Ticks(sys_user);
            if (last_busy_total && total > last_busy_total && idle >= last_idle) {
                s.system_cpu_percent = CpuBusyPercent({last_idle, last_busy_total}, {idle, total});
            }
            last_idle = idle;
            last_busy_total = total;
        }
        PROCESS_MEMORY_COUNTERS memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
            s.ram_used_gb = double(memory.WorkingSetSize) / GiB;
        }
        MEMORYSTATUSEX system{.dwLength = sizeof(MEMORYSTATUSEX)};
        if (GlobalMemoryStatusEx(&system)) {
            s.ram_total_gb = double(system.ullTotalPhys) / GiB;
            s.system_ram_used_gb = double(system.ullTotalPhys - system.ullAvailPhys) / GiB;
        }
        if (instance->CanReportMemoryUsage()) {
            s.vram_used_gb = double(instance->GetDeviceMemoryUsage()) / GiB;
            s.vram_total_gb = double(instance->GetDeviceMemoryBudgetNow()) / GiB;
        }
        // The card always shows GPU load when it can be measured, like on Linux.
        if (!gpu) gpu = std::make_unique<GpuUsage>();
        s.gpu_percent = gpu->Percent();
        if (s.gpu_percent < 0.0f) s.gpu_percent = nvml.Load();
        s.gpu_temp_c = nvml.Temperature();
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
#else
std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

std::vector<std::string> List(const std::string& dir, const char* prefix) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            if (std::string_view(e->d_name).starts_with(prefix)) out.push_back(dir + "/" + e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The temp*_input of a hwmon directory to read: the driver's preferred label, else the first.
std::string PickTemperature(const std::string& hwmon) {
    const std::string driver = Trim(ReadFile(hwmon + "/name"));
    std::string first;
    std::vector<std::pair<std::string, std::string>> labelled;
    for (const auto& input : List(hwmon, "temp")) {
        if (!input.ends_with("_input")) continue;
        if (first.empty()) first = input;
        const std::string base = input.substr(0, input.size() - 6);
        labelled.emplace_back(Trim(ReadFile(base + "_label")), input);
    }
    for (const char* const* want = PreferredTempLabels(driver); *want; ++want) {
        for (const auto& [label, input] : labelled) {
            if (label == *want) return input;
        }
    }
    return first;
}

float ReadCelsius(const std::string& path) {
    if (path.empty()) return -1.0f;
    const std::string text = ReadFile(path);
    if (text.empty()) return -1.0f;
    const long milli = std::strtol(text.c_str(), nullptr, 10);
    return milli > 0 && milli < 150000 ? float(milli) / 1000.0f : -1.0f;
}

// Sysfs files of this PC, located once: the DRM card Vulkan renders on (matched by PCI
// vendor and device ID, so an iGPU next to a discrete card is not picked), its load and
// temperature, and the CPU package temperature.
struct LinuxSensors {
    std::string gpu_busy, gpu_temp, cpu_temp, cpu_name;

    LinuxSensors(uint32_t vendor_id, uint32_t device_id) {
        for (const auto& card : List("/sys/class/drm", "card")) {
            if (card.find('-') != std::string::npos) continue; // connectors (card1-DP-1)
            const std::string dev = card + "/device";
            if (std::strtoul(ReadFile(dev + "/vendor").c_str(), nullptr, 16) != vendor_id ||
                std::strtoul(ReadFile(dev + "/device").c_str(), nullptr, 16) != device_id) {
                continue;
            }
            if (!ReadFile(dev + "/gpu_busy_percent").empty()) gpu_busy = dev + "/gpu_busy_percent";
            for (const auto& hwmon : List(dev + "/hwmon", "hwmon")) {
                gpu_temp = PickTemperature(hwmon);
                if (!gpu_temp.empty()) break;
            }
            break;
        }
        // CPU package sensor: known drivers first, then a generic thermal zone.
        static const char* const cpu_drivers[] = {"k10temp", "zenpower", "coretemp", "cpu_thermal"};
        const auto hwmons = List("/sys/class/hwmon", "hwmon");
        for (const char* driver : cpu_drivers) {
            for (const auto& hwmon : hwmons) {
                if (Trim(ReadFile(hwmon + "/name")) == driver) {
                    cpu_temp = PickTemperature(hwmon);
                    break;
                }
            }
            if (!cpu_temp.empty()) break;
        }
        if (cpu_temp.empty()) {
            for (const auto& zone : List("/sys/class/thermal", "thermal_zone")) {
                const std::string type = Trim(ReadFile(zone + "/type"));
                if (type == "x86_pkg_temp" || type == "cpu-thermal" || type == "cpu_thermal") {
                    cpu_temp = zone + "/temp";
                    break;
                }
            }
        }
        std::istringstream info(ReadFile("/proc/cpuinfo"));
        for (std::string line; std::getline(info, line);) {
            if (line.starts_with("model name")) {
                if (const size_t colon = line.find(':'); colon != std::string::npos) {
                    cpu_name = ShortCpuName(line.substr(colon + 1));
                }
                break;
            }
        }
    }
};

void Run(const Vulkan::Instance* instance) {
    const LinuxSensors sensors(instance->GetVendorID(), instance->GetDeviceID());
    const Nvml nvml(instance->GetVendorID(), instance->GetDeviceID());
    CpuTimes last_cpu{};
    while (true) {
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        s.cpu_name = sensors.cpu_name;
        {
            std::istringstream stat(ReadFile("/proc/stat"));
            std::string line;
            CpuTimes now{};
            if (std::getline(stat, line) && ParseCpuLine(line, now)) {
                if (last_cpu.total) s.system_cpu_percent = CpuBusyPercent(last_cpu, now);
                last_cpu = now;
            }
        }
        double total = 0.0, available = 0.0;
        if (ParseMeminfo(ReadFile("/proc/meminfo"), total, available)) {
            s.ram_total_gb = total;
            s.system_ram_used_gb = total - available;
        }
        if (instance->CanReportMemoryUsage()) {
            s.vram_used_gb = double(instance->GetDeviceMemoryUsage()) / GiB;
            s.vram_total_gb = double(instance->GetDeviceMemoryBudgetNow()) / GiB;
        }
        if (!sensors.gpu_busy.empty()) {
            const std::string busy = ReadFile(sensors.gpu_busy);
            if (!busy.empty()) s.gpu_percent = std::clamp(float(std::atoi(busy.c_str())), 0.0f, 100.0f);
        }
        if (s.gpu_percent < 0.0f) s.gpu_percent = nvml.Load();
        s.gpu_temp_c = ReadCelsius(sensors.gpu_temp);
        if (s.gpu_temp_c < 0.0f) s.gpu_temp_c = nvml.Temperature();
        s.cpu_temp_c = ReadCelsius(sensors.cpu_temp);
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
#endif

} // namespace

void Start(const Vulkan::Instance& instance, unsigned items) {
    requested_items.store(items, std::memory_order_relaxed);
    // ponytail: never joined; a thread that ended while the game ran crashed in ntdll
    // (vk_pipeline_serialization.cpp, StopWarmUp). It sleeps between samples.
    std::call_once(started, [&] { std::thread(Run, &instance).detach(); });
}

void OnGuestFlip(uint64_t render_serial, uint64_t buffer_address, bool blank) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    std::scoped_lock lock{mutex};
    guest_output.Present(uint64_t(now_ns), false);
    Trace().Record("guest_flip", uint64_t(now_ns), render_serial, buffer_address, blank);
}

void OnHostPresented(uint64_t timestamp_ns, bool loading) {
    Trace().Record("host_present", timestamp_ns, 0, 0, loading);
}

Sample Latest() {
    std::scoped_lock lock{mutex};
    auto sample = latest;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto timing = guest_output.Latest(uint64_t(now_ns));
    sample.game_fps = timing.fps;
    sample.game_frame_ms = timing.interval_ms;
    sample.game_stale = timing.stale;
    return sample;
}

} // namespace Ps4PerfStats
