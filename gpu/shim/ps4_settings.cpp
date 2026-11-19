// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_settings.h"
#include "ps4_cheats.h"
#include "ps4_graphics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Ps4Settings {

namespace {

const char* Path() {
    const char* env = std::getenv("ORDER1886_CONFIG");
    return env && env[0] ? env : "order1886.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 3 ? i : 0;
        return;
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = Ps4Graphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = Ps4Graphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "presentation_filter") {
        v.fsr1 = value == "fsr1";
    } else if (key == "fsr1") {
        // Legacy engine switches do not override the explicit presentation filter.
        return;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = Ps4Graphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = Ps4Graphics::ParseFramesQueued(value.empty() ? -1 : i);
    }   else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "anisotropy") {
        v.anisotropy = Ps4Graphics::ParseAnisotropy(i);
    } else if (key == "vibration") {
        v.vibration = value.empty() ? 100 : std::clamp(i, 0, 100);
    } else if (key == "mouse_look") {
        v.mouse_look = i != 0;
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y = i != 0;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity = value.empty() ? 100 : std::clamp(i, 10, 500);
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;

    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "sun_sync") {
        // Words or numbers: off=0, staging=1 (default), drain=2 (the v0.2.9 synchronous path).
        if (value == "off") {
            v.sun_sync = 0;
        } else if (value == "drain") {
            v.sun_sync = 2;
        } else if (value == "staging") {
            v.sun_sync = 1;
        } else {
            v.sun_sync = std::clamp(i, 0, 2);
        }
    } else if (key == "htile_expand") {
        v.htile_expand = i != 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        v.output_res = OutputAutomatic;
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    v.fsr1 = false;
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("ORDER1886_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"ORDER1886_FSR_SHARPNESS", "sharpness"},        {"ORDER1886_JITTER", "jitter"},
        {"ORDER1886_REACTIVE", "reactive"},              {"ORDER1886_REACTIVE_SCALE", "reactive_scale"},
        {"ORDER1886_REACTIVE_THRESHOLD", "reactive_threshold"}, {"ORDER1886_REACTIVE_MAX", "reactive_max"},
        {"ORDER1886_UPSCALE_PRESET", "preset"},            {"ORDER1886_OBJECT_MOTION", "object_motion"},
        {"ORDER1886_ASYNC_SHADERS", "async_shaders"},      {"ORDER1886_DEFERRED_READBACK", "deferred_readback"},
        {"ORDER1886_STALE_READBACK", "stale_readback"},     {"ORDER1886_FSR1", "fsr1"},
        {"ORDER1886_RCAS", "rcas"},                        {"ORDER1886_FRAMES_AHEAD", "frames_queued"},
        {"ORDER1886_ANISOTROPY", "anisotropy"},            {"ORDER1886_VIBRATION", "vibration"},
        {"ORDER1886_SUN_SYNC", "sun_sync"},                {"ORDER1886_HTILE_DRAW_EXPAND", "htile_expand"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.requested_upscaler = v.upscaler.load();
    // No inherited engine reconstruction, cheats, HUD, or speculative readbacks. Sharpening
    // (rcas) and asynchronous shaders are launcher settings and keep the values read above.
    v.upscaler = UpscalerOff;
    v.object_motion = false;
    v.jitter = false;
    v.live_resolution = 0;
    v.deferred_readback = false;
    v.stale_readback = false;
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_requested_upscaler = v.requested_upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    v.startup_anisotropy = v.anisotropy;
    const int out = v.output_res;
    std::printf("Settings: output %s, sharpening %s (%d), anisotropy %d, async shaders %d, "
                "frames queued %d, overlay items 0x%x, sun sync %d, htile expand %d\n",
                out == OutputAutomatic ? "window"
                                       : (std::to_string(OutputWidths[out]) + "x" +
                                          std::to_string(OutputHeights[out])).c_str(),
                v.rcas ? "on" : "off", v.rcas_strength.load(), v.anisotropy.load(),
                int(v.async_shaders.load()), v.frames_queued.load(), v.overlay_items.load(),
                v.sun_sync.load(), int(v.htile_expand.load()));
    LoadQuality();
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback ||
           v.anisotropy != v.startup_anisotropy;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411, const char* problem) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    if (!fsr4) {
        static std::string kept;
        kept = problem ? problem : "GPU does not support the FSR 4 shaders";
        v.fsr4_problem = kept.c_str();
    } else {
        v.fsr4_problem = nullptr;
    }
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
        // The launcher's choice drives provider selection; keeping the old request would make
        // vk_order_resolve look for an FSR 4 provider the device cannot run.
        v.requested_upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "ps4_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("ORDER1886_RENDER_RES");
    return size && size[0];
}

bool MenuEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ORDER1886_SETTINGS_MENU");
        return value && value[0] == '1';
    }();
    return enabled;
}





void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) { std::printf("Settings: cannot write %s\n", Path()); return; }
    std::fprintf(file,
        "# The Order: 1886 runtime settings\n"
        "display_mode=%s\nvsync=%d\nfps_limit=%d\nframes_queued=%d\npresentation_filter=%s\n"
        "vibration=%d\nanisotropy=%d\nshow_fps=%d\n"
        "overlay_scale=%d\noverlay_corner=%d\noverlay_opacity=%d\noverlay_layout=%d\n",
        Ps4Graphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
        int(v.vsync.load()), v.fps_limit.load(), v.frames_queued.load(),
        v.fsr1.load() ? "fsr1" : "linear",
        v.vibration.load(), v.anisotropy.load(), int(v.show_fps.load()),
        v.overlay_scale.load(), v.overlay_corner.load(), v.overlay_opacity.load(), v.overlay_layout.load());
    std::fprintf(file, "mouse_look=%d\nmouse_sensitivity=%d\nmouse_invert_y=%d\n",
        int(v.mouse_look.load()), v.mouse_sensitivity.load(), int(v.mouse_invert_y.load()));
    // The sun-shadow chain (theorder1886-port#26): an in-game save must keep the launcher's
    // readback mode and expand switch, or the next launch silently reverts the flicker fix.
    std::fprintf(file, "sun_sync=%d\nhtile_expand=%d\n",
        v.sun_sync.load(), int(v.htile_expand.load()));
    // The launcher's FSR 3.1 choice arrives as this key; dropping it on an in-game save would
    // switch the player back to the game's own resolve at the next launch.
    if (v.requested_upscaler != UpscalerOff) {
        std::fprintf(file, "upscaler=%s\n", UpscalerName(v.requested_upscaler));
    }
    std::fclose(file);
}

namespace {
// graphics.ini and the launcher's settings.json sit next to runtime.ini (the state folder).
std::string SiblingPath(const char* name) {
    const std::string config = Path();
    const size_t slash = config.find_last_of("/\\");
    return slash == std::string::npos ? std::string(name) : config.substr(0, slash + 1) + name;
}
} // namespace

bool PresetRecipe(int preset, QualityRecipe& out) {
    switch (preset) {
    // Textures stay High in every preset: the texture-quality clamp also reached the game's
    // button-prompt icons (drawn like scene geometry), so it is not offered yet.
    case QualityDeck: out = {false, 2, 2, 30, 1280}; return true;
    case QualityLow: out = {false, 2, 2, 30, 0}; return true;
    case QualityMedium: out = {true, 2, 8, 60, 0}; return true;
    case QualityHigh: out = {true, 2, 16, 60, 0}; return true;
    default: return false;
    }
}

const char* QualityPresetName(int preset) {
    static constexpr const char* names[] = {"Steam Deck", "Low", "Medium", "High", "Custom"};
    return names[std::clamp(preset, 0, int(QualityCustom))];
}

void LoadQuality() {
    auto& v = Get();
    // Ultrawide needs the game's Original (2.4:1) framing: with 16:9 framing there is no band
    // and cropping would cut the picture. The prepared image records which one it uses.
    {
        std::string prepared;
        if (FILE* file = std::fopen(SiblingPath("prepared/prepared.json").c_str(), "rb")) {
            char buffer[4096];
            for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) prepared.append(buffer, n);
            std::fclose(file);
        }
        v.ultrawide_available = prepared.find("\"aspect_ratio\": \"original\"") != std::string::npos &&
                                prepared.find("\"aspect_ratio\": \"16:9\"") == std::string::npos;
    }
    // The render width the launcher will prepare next (Custom stores 0 and the width beside it).
    {
        std::string saved;
        if (FILE* file = std::fopen(SiblingPath("settings.json").c_str(), "rb")) {
            char buffer[4096];
            for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) saved.append(buffer, n);
            std::fclose(file);
        }
        const auto number = [&](const char* key) {
            const size_t at = saved.find(std::string("\"") + key + "\"");
            if (at == std::string::npos) return -1;
            const size_t colon = saved.find(':', at);
            return colon == std::string::npos ? -1 : std::atoi(saved.c_str() + colon + 1);
        };
        int width = number("render_width");
        if (width == 0) width = number("custom_render_width");
        v.launcher_render_width = width > 0 ? width : 0;
    }
    const std::string path = SiblingPath("graphics.ini");
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return; // defaults: shadows on, high textures, the launcher's anisotropy
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
        const size_t eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos) continue;
        const std::string key = text.substr(0, eq);
        const int value = std::atoi(text.c_str() + eq + 1);
        if (key == "shadows") v.shadows = value != 0;
        else if (key == "temporal_aa") v.temporal_aa = value != 0;
        // texture_quality is not offered yet (see PresetRecipe): an older graphics.ini that
        // lowered it is ignored and textures stay at full quality.
        else if (key == "anisotropy") v.quality_anisotropy = std::clamp(value, 0, 16);
        else if (key == "preset") v.quality_preset = std::clamp(value, 0, int(QualityCustom));
        else if (key == "ultrawide") v.ultrawide = value != 0;
        else if (key == "cheats") v.cheat_mask = Ps4Cheats::MaskFromIds(text.substr(eq + 1));
    }
    std::fclose(file);
    v.quality_generation.fetch_add(1);
    std::printf("Quality: %s preset, shadows %s, temporal %s, textures %d, anisotropy %d, ultrawide %s, cheats %s (%s)\n",
                QualityPresetName(v.quality_preset), v.shadows ? "on" : "off",
                v.temporal_aa ? "on" : "off",
                v.texture_quality.load(), v.quality_anisotropy.load(),
                v.ultrawide ? (v.ultrawide_available ? "on" : "on, unavailable: 16:9 framing") : "off",
                v.cheat_mask ? Ps4Cheats::IdsFromMask(v.cheat_mask).c_str() : "none",
                path.c_str());
    std::fflush(stdout);
}

void SaveQuality() {
    const auto& v = Get();
    const std::string path = SiblingPath("graphics.ini");
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) {
        std::printf("Quality: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(file,
                 "# The Order: 1886 quality settings (in-game menu, F1 > Quality and Mods)\n"
                 "preset=%d\nshadows=%d\ntemporal_aa=%d\ntexture_quality=%d\nanisotropy=%d\nultrawide=%d\ncheats=%s\n",
                 v.quality_preset.load(), int(v.shadows.load()), int(v.temporal_aa.load()),
                 v.texture_quality.load(),
                 v.quality_anisotropy.load(), int(v.ultrawide.load()),
                 Ps4Cheats::IdsFromMask(v.cheat_mask.load()).c_str());
    std::fclose(file);
}

namespace {
// Replace the numbers after "key": in place; the launcher owns the rest of the file.
bool EditLauncherNumbers(std::initializer_list<std::pair<const char*, int>> values) {
    const std::string path = SiblingPath("settings.json");
    std::string text;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, n);
        std::fclose(file);
    }
    if (text.empty()) {
        return false;
    }
    const auto set = [&](const char* key, int value) {
        const std::string quoted = std::string("\"") + key + "\"";
        size_t at = text.find(quoted);
        if (at == std::string::npos || value < 0) return false;
        at = text.find(':', at + quoted.size());
        if (at == std::string::npos) return false;
        const size_t start = text.find_first_not_of(" \t", at + 1);
        if (start == std::string::npos) return false;
        size_t end = start;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-')) {
            ++end;
        }
        if (end == start) return false;
        text.replace(start, end - start, std::to_string(value));
        return true;
    };
    bool changed = false;
    for (const auto& [key, value] : values) {
        changed |= set(key, value);
    }
    if (!changed) {
        return false;
    }
    const std::string temp = path + ".tmp";
    FILE* out = std::fopen(temp.c_str(), "wb");
    if (!out) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    std::fclose(out);
    if (!ok) {
        std::remove(temp.c_str());
        return false;
    }
#ifdef _WIN32
    // The Windows C runtime's rename() refuses an existing destination.
    std::remove(path.c_str());
#endif
    return std::rename(temp.c_str(), path.c_str()) == 0;
}
} // namespace

bool StoreForNextLaunch(int render_width, int frame_rate, int anisotropy) {
    // A key the caller leaves out (0 width or rate, negative anisotropy) is not touched.
    const bool ok = EditLauncherNumbers({{"render_width", render_width > 0 ? render_width : -1},
                                         {"frame_rate", frame_rate > 0 ? frame_rate : -1},
                                         {"anisotropy", anisotropy}});
    if (ok && render_width > 0) {
        Get().launcher_render_width = render_width;
    }
    return ok;
}

bool StoreLauncherNumber(const char* key, int value) {
    return EditLauncherNumbers({{key, value}});
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace Ps4Settings
