// SPDX-License-Identifier: GPL-2.0-or-later
// user settings changed at run time from the in-game menu (ps4_overlay.h) and kept
// in order1886.ini (ORDER1886_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once


#include <atomic>
#include <cstdint>
#include "ps4_perf_stats.h"

namespace Ps4Settings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
/// -1: the presentation frame follows the window. Otherwise the frame is created at the
/// selected size (fit to the game's aspect) and the swapchain blit fits it to the window.
inline constexpr int OutputAutomatic = -1;
inline constexpr int OutputDefault = OutputAutomatic;

/// Quality presets of the menu, in menu order.
enum QualityPreset : int { QualityDeck, QualityLow, QualityMedium, QualityHigh, QualityCustom };

struct Values {
    std::atomic<int> upscaler{UpscalerOff};
    /// The upscaler runtime.ini or ORDER1886_UPSCALER asked for, before Load forces it off.
    std::atomic<int> requested_upscaler{UpscalerOff};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{false};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{false};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{false};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{true};
    /// Performance overlay (ps4_perf_stats.h): items shown, font size and background opacity
    /// in percent, corner (0 top left, 1 top right, 2 bottom left, 3 bottom right), layout
    /// (0 one line, 1 one item per line). menu_tab: the menu's last tab.
    std::atomic<unsigned> overlay_items{Ps4PerfStats::DefaultItems};
    std::atomic<int> overlay_scale{100}, overlay_corner{1}, overlay_opacity{50}, overlay_layout{1};
    std::atomic<int> menu_tab{0};
    /// The game's code reads these floats directly.
    // Graphics (ps4_graphics.h). Live: applied on the next frame.
    std::atomic<int> display_mode{0};
    std::atomic<bool> vsync{true};
    std::atomic<int> fps_limit{30};
    /// Pipelines compile off the GPU thread; a draw waiting for one is skipped meanwhile
    /// (docs/verification/2026-10-24-async-pipeline-compile.md).
    std::atomic<bool> async_shaders{true};
    std::atomic<bool> fsr1{false};
    std::atomic<bool> rcas{false};
    std::atomic<int> rcas_strength{75};
    /// Set by the presenter: sharpening ran on the last frame (it needs FSR 1 when the game's
    /// image is smaller than the window).
    std::atomic<bool> rcas_applied{false};
    /// Set by the presenter: the game's image and the window it is shown in, in pixels.
    std::atomic<int> image_width{0}, image_height{0}, window_width{0}, window_height{0};
    std::atomic<int> frames_queued{1};
    /// Controller vibration strength in percent; 0 turns it off (runtime_pad.c). Live.
    std::atomic<int> vibration{100};
    std::atomic<bool> mouse_look{true}, mouse_invert_y{false};
    std::atomic<int> mouse_sensitivity{100};
    // Startup: run.py turns them into patches and environment variables; "Apply and restart".
    std::atomic<bool> deferred_readback{false};
    std::atomic<bool> stale_readback{false};
    /// Ps4Graphics::Anisotropies; the samplers read startup_anisotropy.
    std::atomic<int> anisotropy{0};
    int startup_anisotropy = 0;
    /// Set before an "Apply and restart"; cleared once the new launch shows the game. run.py
    /// restores the previous startup options when a launch never cleared it.
    std::atomic<bool> restart_unconfirmed{false};
    /// The engine rate this launch really runs at (run.py's ORDER1886_ENGINE_FPS): 60 when the frame
    /// rate patch does not apply to this game build.
    bool startup_deferred_readback = false, startup_stale_readback = false;
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{false};
    std::atomic<bool> fsr4_invert_jitter{false};
    /// Sun shadow readback mode (theorder1886-port#26): 0 off, 1 staging (default), 2 legacy
    /// drain. Staging copies the sun ranges to host staging at the EVSM copy dispatch and
    /// serves CPU write faults from the newest completed batch - no mid-frame queue drain,
    /// which is what made the sun's shadows flicker every other frame in The Chase.
    std::atomic<int> sun_sync{1};
    /// Draw-time HTILE expand for depth-writing passes (the decisive half of #26 on RDNA4).
    std::atomic<bool> htile_expand{true};
    std::atomic<int> active_render_width{0}, active_render_height{0};
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths, or OutputAutomatic
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (ps4_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit ORDER1886_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerOff;
    /// The provider requested_upscaler held at startup: the renderer picks it once, so a menu
    /// change takes effect at the next launch.
    int startup_requested_upscaler = UpscalerOff;
    /// Bit (1 << Upscaler) per temporal provider that loads on this device, set by the renderer.
    std::atomic<unsigned> upscaler_available_mask{0};
    /// The launcher's render width for the next launch (settings.json), 0 when unknown.
    std::atomic<int> launcher_render_width{0};
    bool startup_object_motion = false;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;

    // Quality (the menu's "Quality" section): live, saved in graphics.ini next to runtime.ini
    // (the launcher rewrites runtime.ini from its own settings on every Play).
    /// Off: draws into shadow maps (depth-only, square power-of-two targets) are skipped, the
    /// maps keep their clear, and everything is lit as if unshadowed.
    std::atomic<bool> shadows{true};
    /// Off: the game's temporal resolve (its MSAA resolve blended with the previous frame, the
    /// blur in issue #29) is replaced by the engine resolving the same samples without the
    /// history term, in place of the game's dispatch. The game sends no projection jitter, so
    /// this costs the temporal smoothing but not stability. Live; graphics.ini.
    std::atomic<bool> temporal_aa{true};
    /// 0 low, 1 medium, 2 high: mipmapped textures start that many mip levels down.
    std::atomic<int> texture_quality{2};
    /// 0: the launcher's anisotropy (startup_anisotropy); otherwise the menu's 1-16.
    std::atomic<int> quality_anisotropy{0};
    std::atomic<int> quality_preset{QualityCustom};
    /// Bumped when a sampler-affecting choice changes, so the sampler cache makes new ones.
    std::atomic<unsigned> quality_generation{0};
    /// Ultrawide (21:9): present only the game's 2.4:1 picture band, scaled to fill the window
    /// width, instead of the whole 16:9 frame with its black bars. Live; graphics.ini.
    std::atomic<bool> ultrawide{false};
    /// The prepared image uses Original (2.4:1) framing: the band exists (prepared.json).
    std::atomic<bool> ultrawide_available{false};
    /// Cheats (issue #24): bit i is Ps4Cheats::Table()[i]. Toggled live in the menu's Mods
    /// section; the selection persists in graphics.ini and is applied again at start.
    std::atomic<unsigned> cheat_mask{0};
};

struct QualityRecipe {
    bool shadows;
    int texture_quality;
    int anisotropy;
    int frame_rate;   ///< 30 or 60
    int render_width; ///< 0: keep the launcher's render resolution
};
/// What a preset sets; Custom has no recipe (returns false).
bool PresetRecipe(int preset, QualityRecipe& out);
const char* QualityPresetName(int preset);

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// The in-game menu is opt-in through ORDER1886_SETTINGS_MENU=1. The menu, its tabs and
/// the live settings it edits are not gameplay-qualified for this title, so a launch that
/// does not ask for it behaves exactly as before.
bool MenuEnabled();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1. `problem`
/// is recorded whenever FSR 4 is unavailable, so the reason exists even if nobody asked for it.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, const char* problem = nullptr);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
/// A startup graphics option differs from the one this launch started with.
bool GraphicsNeedRestart();
/// Writes the file (menu changes).
void Save();
/// graphics.ini (quality section): read by Load, written by the menu.
void LoadQuality();
void SaveQuality();
/// Stores the render width, frame rate and anisotropy in the launcher's settings.json so the
/// next Play prepares the game with them (the render resolution cannot change while it runs).
/// Returns false when the file is missing or has no such keys.
bool StoreForNextLaunch(int render_width, int frame_rate, int anisotropy);
/// One numeric launcher setting (settings.json key) for the next launch; false when the file
/// or the key is missing.
bool StoreLauncherNumber(const char* key, int value);

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace Ps4Settings
