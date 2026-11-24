// In-game (F1) menu choices survive the launcher's next launch (ps4_settings.cpp).
//
// The launcher's worker rewrites runtime.ini from settings.json on every launch. A menu save
// therefore has to write the launcher's own keys back into settings.json, keeping each value's
// JSON type, and keep the menu-only keys in runtime-menu.ini, which the worker never writes.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "ps4_settings.h"

#ifndef _WIN32
static int _putenv_s(const char* name, const char* value) {
    return setenv(name, value, 1);
}
#endif

// ps4_settings.cpp carries the cheat table; no game image is loaded here.
extern "C" int runtime_guest_peek(uint64_t, unsigned char*, size_t) { return -2; }
extern "C" int runtime_guest_patch(uint64_t, const unsigned char*, const unsigned char*, size_t) {
    return -2;
}

static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

static void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

static bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "order1886-menu-persistence-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto ini = dir / "runtime.ini";
    const auto json = dir / "settings.json";
    // The launcher's schema: bools, strings and numbers, plus a string value equal to a key.
    Write(json, "{\n  \"display_mode\": \"borderless\",\n  \"vsync\": true,\n"
                "  \"show_fps\": false,\n  \"presentation_filter\": \"fsr4\",\n"
                "  \"note\": \"vsync\",\n  \"mouse_sensitivity\": 100,\n"
                "  \"mouse_invert_y\": false,\n  \"frames_queued\": 1,\n  \"sun_sync\": 1,\n"
                "  \"htile_expand\": true,\n  \"frame_generation\": false\n}\n");
    // What the worker writes at launch: no vibration or overlay placement keys.
    const std::string launch_ini = "display_mode=borderless\nvsync=1\nshow_fps=0\n"
                                   "presentation_filter=fsr1\nupscaler=fsr4\nmouse_sensitivity=100\n"
                                   "frames_queued=1\nsun_sync=1\nhtile_expand=1\n";
    Write(ini, launch_ini);
    const std::string ini_path = ini.string();
    _putenv_s("ORDER1886_CONFIG", ini_path.c_str());

    auto& v = Ps4Settings::Get();
    Ps4Settings::Load();
    assert(v.display_mode == 1 && v.vsync && !v.show_fps);

    // Menu changes, then the save on menu close.
    v.display_mode = 2;
    v.vsync = false;
    v.show_fps = true;
    v.mouse_sensitivity = 140;
    v.mouse_invert_y = true;
    v.frames_queued = 2;
    v.htile_expand = false;
    v.vibration = 30;
    v.overlay_corner = 3;
    v.overlay_opacity = 80;
    Ps4Settings::Save();

    const std::string saved = Read(json);
    assert(Has(saved, "\"display_mode\": \"fullscreen\""));
    assert(Has(saved, "\"vsync\": false"));
    assert(Has(saved, "\"show_fps\": true"));
    assert(Has(saved, "\"note\": \"vsync\"")); // a string value equal to a key is not a key
    assert(Has(saved, "\"mouse_sensitivity\": 140"));
    assert(Has(saved, "\"mouse_invert_y\": true"));
    assert(Has(saved, "\"frames_queued\": 2"));
    assert(Has(saved, "\"htile_expand\": false"));
    assert(Has(saved, "\"presentation_filter\": \"fsr4\""));
    assert(Has(saved, "\"sun_sync\": 1"));

    // frame_generation is a JSON bool in the launcher's schema: the menu's number used to fail.
    assert(Ps4Settings::StoreLauncherNumber("frame_generation", 1));
    assert(Has(Read(json), "\"frame_generation\": true"));

    // Next launch: the worker rewrites runtime.ini; the menu-only choices come back anyway.
    Write(ini, launch_ini);
    v.vibration = 100;
    v.overlay_corner = 1;
    v.overlay_opacity = 50;
    Ps4Settings::Load();
    assert(v.vibration == 30 && v.overlay_corner == 3 && v.overlay_opacity == 80);

    std::filesystem::remove_all(dir);
    std::puts("menu persistence: ok");
    return 0;
}
