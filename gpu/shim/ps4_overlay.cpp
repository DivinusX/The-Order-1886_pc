// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "ps4_settings.h"
#include "ps4_graphics.h"
#include "ps4_perf_stats.h"
#include "ps4_guest_image.h"
#include "ps4_cheats.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderdoc.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global ps4_font_ttf\n"
    "ps4_font_ttf:\n"
    ".incbin \"" PS4_FONT_PATH "\"\n"
    ".global ps4_font_ttf_end\n"
    "ps4_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden ps4_font_ttf\n"
    ".global ps4_font_ttf\n"
    "ps4_font_ttf:\n"
    ".incbin \"" PS4_FONT_PATH "\"\n"
    ".hidden ps4_font_ttf_end\n"
    ".global ps4_font_ttf_end\n"
    "ps4_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char ps4_font_ttf[];
extern "C" const unsigned char ps4_font_ttf_end[];

namespace Ps4Overlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool r3_down = false, l2_down = false;
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
// A short message in a corner (saved screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
int active_tab = 0;     // the tab being drawn; 0 Graphics, 1 Mods
constexpr int TabCount = 2;
const char* const TabNames[TabCount] = {"Graphics", "Mods"};
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<Ps4Png::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

std::mutex presentation_mutex;
Ps4PerfStats::PresentationCounter presentation_counter;
std::chrono::steady_clock::time_point last_ui_frame{};

bool SettingsMenuEnabled() {
    // Opt-in per launch (ORDER1886_SETTINGS_MENU=1): the menu and the settings it edits
    // live are not gameplay-qualified for this title yet, so it stays closed by default.
    return Ps4Settings::MenuEnabled();
}

void SetOpen(bool value) {
    if (value && !SettingsMenuEnabled()) {
        return;
    }
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (value) {
        requested_tab = Ps4Settings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        Ps4Settings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text follow ORDER1886_GLYPHS (run.py sets xbox or nintendo when
// the glyph feature is on); unset is PlayStation. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("ORDER1886_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

// A 0.1x to 100x multiplier with a 1x button.



void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case Ps4Settings::UpscalerFsr3: return "FSR 3.1";
    case Ps4Settings::UpscalerFsr4: return "FSR 4";
    case Ps4Settings::UpscalerFsr411: return "FSR 4.1.1";
    case Ps4Settings::UpscalerTaa: return "TAA";
    case Ps4Settings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = Ps4PerfStats;
    const auto& s = Ps4Settings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance, items);
    }
    P::Sample sample = P::Latest();
    {
        std::scoped_lock lock{presentation_mutex};
        const auto timing = presentation_counter.Latest();
        sample.fps = timing.fps;
        sample.frame_ms = timing.interval_ms;
    }
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

constexpr const char* Unsupported = "Unavailable: game version or code signature mismatch.";


void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("%s", PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", PadText("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel", "Up/Down or D-pad: choose   Enter or A: OK   Esc or B: cancel", "Up/Down or D-pad: choose   Enter or B: OK   Esc or A: cancel"));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

// ---- Performance overlay card -------------------------------------------------------------
// A hardware panel in the style of modern game overlays: the frame rate large and coloured
// against the frame-rate limit, a frame-time graph, then GPU and CPU (load and temperature)
// and memory. Rows this PC cannot measure are left out instead of showing "n/a"; the
// overlay items in the settings choose which rows appear (temperatures always do when known).
constexpr ImVec4 kText{0.93f, 0.94f, 0.97f, 1.0f};
constexpr ImVec4 kDim{0.60f, 0.64f, 0.72f, 1.0f};
constexpr ImVec4 kGpuAccent{0.40f, 0.78f, 1.00f, 1.0f};
constexpr ImVec4 kCpuAccent{0.72f, 0.56f, 1.00f, 1.0f};
constexpr ImVec4 kMemAccent{0.36f, 0.86f, 0.74f, 1.0f};
constexpr ImVec4 kTrack{1.0f, 1.0f, 1.0f, 0.10f};

ImVec4 TierColor(Ps4PerfStats::Tier tier) {
    switch (tier) {
    case Ps4PerfStats::Tier::Good: return {0.38f, 0.88f, 0.56f, 1.0f};
    case Ps4PerfStats::Tier::Warn: return {1.00f, 0.77f, 0.32f, 1.0f};
    default: return {1.00f, 0.40f, 0.40f, 1.0f};
    }
}

// Host frame intervals for the graph (the overlay draws once per presented frame).
struct FrameHistory {
    static constexpr int Size = 120;
    float ms[Size]{};
    int head = 0, count = 0;
    void Push(float value) {
        ms[head] = value;
        head = (head + 1) % Size;
        count = std::min(count + 1, Size);
    }
    float At(int i) const { return ms[(head - count + i + Size) % Size]; } // oldest first
};
FrameHistory frame_history;

void Meter(float fraction, float width, const ImVec4& color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::max(3.0f, ImGui::GetFontSize() * 0.28f);
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(kTrack), h * 0.5f);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0.0f) {
        draw->AddRectFilled(p, ImVec2(p.x + std::max(width * f, h), p.y + h), ImGui::GetColorU32(color),
                            h * 0.5f);
    }
    ImGui::Dummy(ImVec2(width, h));
}

void FrameGraph(float width, float target_ms) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFontSize() * 2.2f;
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.04f)),
                        ImGui::GetFontSize() * 0.3f);
    const int n = frame_history.count;
    float top = target_ms * 2.0f;
    for (int i = 0; i < n; ++i) top = std::max(top, frame_history.At(i));
    const auto y_of = [&](float ms) { return p.y + h - 2.0f - (h - 4.0f) * std::clamp(ms / top, 0.0f, 1.0f); };
    // the frame-time budget of the frame-rate limit
    const float target_y = y_of(target_ms);
    draw->AddLine(ImVec2(p.x, target_y), ImVec2(p.x + width, target_y),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, 0.18f)), 1.0f);
    if (n >= 2) {
        const float step = width / float(FrameHistory::Size - 1);
        const float x0 = p.x + width - step * float(n - 1);
        ImVec2 prev(x0, y_of(frame_history.At(0)));
        for (int i = 1; i < n; ++i) {
            const float ms = frame_history.At(i);
            const ImVec2 cur(x0 + step * float(i), y_of(ms));
            // spikes above 1.5x the budget stand out
            const ImVec4 color = ms > target_ms * 1.5f ? TierColor(Ps4PerfStats::Tier::Bad) : kGpuAccent;
            draw->AddLine(prev, cur, ImGui::GetColorU32(color), std::max(1.0f, ImGui::GetFontSize() * 0.09f));
            prev = cur;
        }
    }
    ImGui::Dummy(ImVec2(width, h));
}

// "GPU  Radeon RX 9070 XT ...... 45%  47°C" then the load meter across the card.
void HardwareRow(const char* tag, const ImVec4& accent, const std::string& name, float percent,
                 float temp_c, float width) {
    const bool has_load = percent >= 0.0f;
    if (name.empty() && !has_load && temp_c < 0.0f) {
        return;
    }
    const float em = ImGui::GetFontSize();
    const float x = ImGui::GetCursorPosX();
    // Line 1: tag and the whole model name (cut only if it is wider than the card).
    ImGui::TextColored(accent, "%s", tag);
    ImGui::SameLine(x + em * 2.6f);
    std::string label = name;
    const float name_room = width - em * 2.6f;
    while (label.size() > 3 && ImGui::CalcTextSize(label.c_str()).x > name_room) {
        label.erase(label.size() - 4);
        label += "...";
    }
    ImGui::TextColored(kDim, "%s", label.c_str());
    // Line 2: load meter, then load and temperature in fixed columns at the right edge, so
    // the GPU and CPU numbers line up.
    char load[16] = "", temp[16] = "";
    if (has_load) std::snprintf(load, sizeof(load), "%.0f%%", percent);
    if (temp_c >= 0.0f) std::snprintf(temp, sizeof(temp), "%.0f\xC2\xB0" "C", temp_c);
    const float temp_col = ImGui::CalcTextSize("100\xC2\xB0" "C").x;
    const float load_col = ImGui::CalcTextSize("100%").x;
    const float gap = em * 0.6f;
    const float numbers_w = temp_col + load_col + gap;
    const float line_h = ImGui::GetTextLineHeight();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (has_load) {
        const float bar_w = width - numbers_w - gap;
        const float h = std::max(3.0f, em * 0.28f);
        const float y = start.y + (line_h - h) * 0.5f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + bar_w, y + h), ImGui::GetColorU32(kTrack),
                            h * 0.5f);
        const float f = std::clamp(percent / 100.0f, 0.0f, 1.0f);
        if (f > 0.0f) {
            draw->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + std::max(bar_w * f, h), y + h),
                                ImGui::GetColorU32(accent), h * 0.5f);
        }
    }
    ImGui::Dummy(ImVec2(1.0f, line_h));
    if (load[0]) {
        ImGui::SameLine(x + width - temp_col - gap - ImGui::CalcTextSize(load).x);
        ImGui::TextUnformatted(load);
    }
    if (temp[0]) {
        ImGui::SameLine(x + width - ImGui::CalcTextSize(temp).x);
        ImGui::TextColored(TierColor(Ps4PerfStats::TempTier(temp_c)), "%s", temp);
    }
}

void MemoryRow(const char* tag, double used, double total, float width) {
    if (used < 0.0) {
        return;
    }
    const float x = ImGui::GetCursorPosX();
    ImGui::TextColored(kMemAccent, "%s", tag);
    char value[48];
    if (total > 0.0) std::snprintf(value, sizeof(value), "%.1f / %.0f GB", used, total);
    else std::snprintf(value, sizeof(value), "%.1f GB", used);
    ImGui::SameLine(x + width - ImGui::CalcTextSize(value).x);
    ImGui::TextColored(kDim, "%s", value);
    if (total > 0.0) Meter(float(used / total), width, kMemAccent);
}

void Divider(float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + ImGui::GetFontSize() * 0.25f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(p.x + width, y),
                                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), 1.0f);
    ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() * 0.5f));
}

void FpsCounter() {
    namespace P = Ps4PerfStats;
    const auto& s = Ps4Settings::Get();
    const unsigned items = s.overlay_items;
    if (!items) {
        return;
    }
    // The sampler also feeds temperatures, so it runs whenever the card is shown.
    if (overlay_instance) {
        P::Start(*overlay_instance, items);
    }
    P::Sample sample = P::Latest();
    {
        std::scoped_lock lock{presentation_mutex};
        const auto timing = presentation_counter.Latest();
        sample.fps = timing.fps;
        sample.frame_ms = timing.interval_ms;
    }
    const float dt_ms = ImGui::GetIO().DeltaTime * 1000.0f;
    if (dt_ms > 0.0f && dt_ms < 1000.0f) frame_history.Push(dt_ms);

    // The rate the game aims for: its own frame interval (2 = 30 FPS, 1 = 60 FPS) caps it below
    // the presentation limit, so a 30 FPS session is not shown in red against a 60 FPS limit.
    float target_fps = s.fps_limit > 0 ? float(s.fps_limit.load()) : 60.0f;
    if (volatile uint32_t* interval = Ps4GuestImage::IntervalGlobal(); interval && *interval > 0) {
        target_fps = std::min(target_fps, 60.0f / float(*interval));
    }
    // The game's own frame rate (guest flips); host presents when the game has not flipped yet.
    const float fps = sample.game_fps >= 0.0f ? sample.game_fps : sample.fps;
    const float frame_ms = sample.game_frame_ms >= 0.0f ? sample.game_frame_ms : sample.frame_ms;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(std::max(s.overlay_opacity / 100.0f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.05f, 0.065f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * base_scale, 9.0f * base_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * base_scale, 3.0f * base_scale));
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const float scale = s.overlay_scale / 100.0f;
    ImGui::SetWindowFontScale(scale);
    const float em = ImGui::GetFontSize();
    const bool row = s.overlay_layout == 0;
    const ImVec4 fps_color = fps >= 0.0f ? TierColor(P::FpsTier(fps, target_fps)) : kDim;

    if (row) {
        // One compact line: 60 FPS 16.7 ms | GPU 45% 47°C | CPU 12% 45°C | VRAM 5.1 GB
        const auto sep = [&] {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 1, 1, 0.20f), "|");
            ImGui::SameLine();
        };
        if (items & P::Fps) {
            if (fps >= 0.0f) ImGui::TextColored(fps_color, "%.0f", fps);
            else ImGui::TextColored(kDim, "--");
            ImGui::SameLine();
            ImGui::TextColored(kDim, "FPS");
        }
        if ((items & P::FrameTime) && frame_ms >= 0.0f) {
            if (items & P::Fps) ImGui::SameLine();
            ImGui::TextColored(kDim, "%.1f ms", frame_ms);
        }
        const auto hw = [&](const char* tag, const ImVec4& accent, float pct, float temp) {
            if (pct < 0.0f && temp < 0.0f) return;
            sep();
            ImGui::TextColored(accent, "%s", tag);
            if (pct >= 0.0f) {
                ImGui::SameLine();
                ImGui::Text("%.0f%%", pct);
            }
            if (temp >= 0.0f) {
                ImGui::SameLine();
                ImGui::TextColored(TierColor(P::TempTier(temp)), "%.0f\xC2\xB0", temp);
            }
        };
        hw("GPU", kGpuAccent, sample.gpu_percent, sample.gpu_temp_c);
        hw("CPU", kCpuAccent, sample.system_cpu_percent, sample.cpu_temp_c);
        if ((items & P::Vram) && sample.vram_used_gb >= 0.0) {
            sep();
            ImGui::TextColored(kMemAccent, "VRAM");
            ImGui::SameLine();
            ImGui::Text("%.1f GB", sample.vram_used_gb);
        }
        if ((items & P::Ram) && sample.system_ram_used_gb >= 0.0) {
            sep();
            ImGui::TextColored(kMemAccent, "RAM");
            ImGui::SameLine();
            ImGui::Text("%.1f GB", sample.system_ram_used_gb);
        }
    } else {
        const float width = em * 15.0f;
        // Header: big frame rate, unit and frame time beside it.
        if (items & (P::Fps | P::FrameTime)) {
            const float x = ImGui::GetCursorPosX();
            if (items & P::Fps) {
                ImGui::SetWindowFontScale(scale * 2.1f);
                if (fps >= 0.0f) ImGui::TextColored(fps_color, "%.0f", fps);
                else ImGui::TextColored(kDim, "--");
                const float big_w = ImGui::GetItemRectSize().x;
                const float big_h = ImGui::GetItemRectSize().y;
                ImGui::SetWindowFontScale(scale);
                ImGui::SameLine(x + big_w + em * 0.35f);
                const float y = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(y + big_h - em * 1.25f);
                ImGui::TextColored(kDim, "FPS");
            }
            char info[64] = "";
            if ((items & P::FrameTime) && frame_ms >= 0.0f) {
                std::snprintf(info, sizeof(info), "%.1f ms", frame_ms);
            }
            if (info[0]) {
                ImGui::SameLine(x + width - ImGui::CalcTextSize(info).x);
                ImGui::TextColored(kDim, "%s", info);
            }
            if ((items & P::Fps) && sample.fps >= 0.0f && sample.game_fps >= 0.0f &&
                std::abs(sample.fps - sample.game_fps) > 1.5f) {
                // frame generation / presentation rate above the game's own
                ImGui::TextColored(kDim, "Presented %.0f FPS", sample.fps);
            }
            if (items & P::FrameTime) {
                FrameGraph(width, 1000.0f / target_fps);
            }
        }
        // GPU and CPU always appear with whatever this PC can report (load, temperature).
        const bool gpu_any = sample.gpu_temp_c >= 0.0f || sample.gpu_percent >= 0.0f;
        const bool cpu_any = sample.cpu_temp_c >= 0.0f || sample.system_cpu_percent >= 0.0f;
        if (gpu_any || cpu_any) {
            Divider(width);
            if (gpu_any) {
                HardwareRow("GPU", kGpuAccent, P::ShortGpuName(sample.gpu_name), sample.gpu_percent,
                            sample.gpu_temp_c, width);
            }
            if (cpu_any) {
                HardwareRow("CPU", kCpuAccent, sample.cpu_name, sample.system_cpu_percent,
                            sample.cpu_temp_c, width);
            }
        }
        const bool vram = (items & P::Vram) && sample.vram_used_gb >= 0.0;
        const bool ram = (items & P::Ram) && sample.system_ram_used_gb >= 0.0;
        if (vram || ram) {
            Divider(width);
            if (vram) MemoryRow("VRAM", sample.vram_used_gb, sample.vram_total_gb, width);
            if (ram) MemoryRow("RAM", sample.system_ram_used_gb, sample.ram_total_gb, width);
        }
        if (items & P::Upscaler) {
            const std::string upscaler = UpscalerLabel(s.upscaler);
            Divider(width);
            ImGui::TextColored(kDim, "%s", upscaler.empty() ? "Native" : upscaler.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(3);
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

void Notice() {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}


// ---- In-game settings menu (F1) -------------------------------------------------------------
// Laid out like Longshot's (rows of label | value, a description panel, key hints), coloured
// with the launcher's palette, which follows the game's: charcoal, parchment and gold.
namespace Menu {

constexpr ImU32 Rgba(u32 rgb, float alpha = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(alpha * 255.0f));
}
constexpr ImU32 Backdrop = IM_COL32(4, 5, 7, 165);
constexpr ImU32 BackdropLow = IM_COL32(0, 0, 0, 220);
constexpr ImU32 Panel = Rgba(0x0d0f12, 0.97f);
constexpr ImU32 PanelTop = Rgba(0x171a1f, 0.97f);
constexpr ImU32 Surface = Rgba(0x15181c);
constexpr ImU32 Surface2 = Rgba(0x1b1f24);
constexpr ImU32 SurfaceHi = Rgba(0x232830);
constexpr ImU32 Line = IM_COL32(255, 255, 255, 16);
constexpr ImU32 LineStrong = IM_COL32(255, 255, 255, 34);
constexpr ImU32 Ink = Rgba(0xf3efe6);
constexpr ImU32 Muted = Rgba(0x8f939a);
constexpr ImU32 Dim = Rgba(0x5d6168);
constexpr ImU32 Accent = Rgba(0xd9b766);
constexpr ImU32 AccentSoft = Rgba(0xd9b766, 0.14f);
constexpr ImU32 AccentLine = Rgba(0xd9b766, 0.55f);
constexpr ImU32 AccentInk = Rgba(0x16130b);
constexpr ImU32 Focus = Rgba(0xf4d88f);
constexpr ImU32 RowFocus = Rgba(0x241f16);
constexpr ImU32 Good = Rgba(0x7fcf8e);
constexpr ImU32 GoodSoft = Rgba(0x7fcf8e, 0.14f);
constexpr ImU32 Warn = Rgba(0xe3a95c);
constexpr ImU32 WarnSoft = Rgba(0xe3a95c, 0.14f);
constexpr ImU32 Bad = Rgba(0xdc7d70);
constexpr ImU32 BadSoft = Rgba(0xdc7d70, 0.14f);

enum class Kind { Choice, Toggle };
struct Row {
    const char* section; // non-null: a heading drawn above this row
    const char* label;
    Kind kind;
    std::vector<std::string> values; // Toggle: {"Off", "On"}
    int current;
    bool available;
    const char* title;
    const char* body;
    std::string note;          // shown under the body with a gold bar (current effect)
    void (*apply)(int index);  // stores the new value
    int cheat = -1;            // Mods rows: index into Ps4Cheats::Table(), apply stays null
    const char* badge = nullptr;  // short chip after the label ("Active", "Restart", ...)
    int badge_tone = 0;           // 0 gold, 1 green, 2 amber, 3 red
};

int focused = 0;
// 0: the game decides its own interval; otherwise the interval the menu set, plus one (the
// interval itself can be 0), kept every frame.
std::atomic<u32> chosen_interval{0};

const int OutputChoices[] = {Ps4Settings::OutputAutomatic, 0, 1, 2, 3};
// One frame-rate row. 30 and 60 are the engine's own intervals (2 and 1); every higher rate runs
// the engine uncapped (interval 0) and lets the presentation cap pace it, so no executable code
// is changed live. Unlocked removes the cap: V-Sync or the GPU sets the limit.
constexpr int Rates[] = {30, 60, 90, 120, 144, 165, 0};
constexpr int RateCount = int(sizeof(Rates) / sizeof(Rates[0]));

void SetGameFrameRate(int index) {
    const int rate = Rates[index];
    const u32 interval = rate == 30 ? 2u : rate == 60 ? 1u : 0u;
    chosen_interval = interval + 1;
    if (volatile uint32_t* global = Ps4GuestImage::IntervalGlobal()) {
        *global = interval;
    }
    Ps4Settings::Get().fps_limit = rate;
}

// ---- Quality (shadows, textures, anisotropy, presets) ---------------------------------------
constexpr int QualityAnisotropies[] = {0, 1, 2, 4, 8, 16};
constexpr int QualityAnisotropyCount = int(sizeof(QualityAnisotropies) / sizeof(QualityAnisotropies[0]));

int RateIndex(int fps) {
    for (int i = 0; i < RateCount; ++i) {
        if (Rates[i] == fps) return i;
    }
    return 0;
}

// A single choice made by hand turns the preset into Custom.
void MarkCustom() {
    Ps4Settings::Get().quality_preset = Ps4Settings::QualityCustom;
}

void ApplyQualityPreset(int index) {
    auto& s = Ps4Settings::Get();
    s.quality_preset = index;
    Ps4Settings::QualityRecipe recipe{};
    if (!Ps4Settings::PresetRecipe(index, recipe)) {
        Ps4Settings::SaveQuality();
        return;
    }
    s.shadows = recipe.shadows;
    s.texture_quality = recipe.texture_quality;
    s.quality_anisotropy = recipe.anisotropy;
    s.quality_generation.fetch_add(1);
    if (Ps4GuestImage::IntervalGlobal()) {
        SetGameFrameRate(RateIndex(recipe.frame_rate));
    }
    Ps4Settings::SaveQuality();
    // The launcher prepares the game with these at the next Play (the render resolution is
    // part of the prepared image and cannot change while the game runs).
    const bool stored = Ps4Settings::StoreForNextLaunch(recipe.render_width, recipe.frame_rate,
                                                        recipe.anisotropy);
    std::string text = std::string(Ps4Settings::QualityPresetName(index)) +
                       " preset applied. Restart the game to apply all preset changes.";
    if (recipe.render_width > 0 && !stored) {
        text += " Set the render resolution to " + std::to_string(recipe.render_width) +
                " in the launcher.";
    }
    ShowNotice(text, 8000);
}

// Choosing a preset in the row opens a confirmation dialog (Apply / Cancel) instead of
// applying it at once; -1 while no dialog is open.
int pending_preset = -1;
int dialog_focus = 0; // 0 Apply, 1 Cancel

void OpenPresetDialog(int index) {
    if (index == Ps4Settings::QualityCustom) {
        ApplyQualityPreset(index); // Custom changes nothing: no dialog
        return;
    }
    pending_preset = index;
    dialog_focus = 0;
}

void ApplyShadows(int index) {
    Ps4Settings::Get().shadows = index == 1;
    MarkCustom();
    Ps4Settings::SaveQuality();
}

void ApplyTemporalAA(int index) {
    Ps4Settings::Get().temporal_aa = index == 1;
    MarkCustom();
    Ps4Settings::SaveQuality();
}

void ApplyQualityAnisotropy(int index) {
    auto& s = Ps4Settings::Get();
    s.quality_anisotropy = QualityAnisotropies[std::clamp(index, 0, QualityAnisotropyCount - 1)];
    s.quality_generation.fetch_add(1);
    MarkCustom();
    Ps4Settings::SaveQuality();
}

// ---- Mods (cheats; issue #24) ---------------------------------------------------------------
// Toggling a cheat patches the loaded image right away: every site's expected bytes are
// checked first (Ps4Cheats), so a build that differs from the verified one is refused and
// the row says why. The selection is kept in graphics.ini and reapplied at the next start.
void ApplyCheatToggle(int cheat, int index) {
    const Ps4Cheats::Cheat& entry = Ps4Cheats::Table()[cheat];
    std::string problem;
    if (!Ps4Cheats::SetEnabled(entry, index == 1, problem)) {
        std::printf("Cheats: %s: %s\n", entry.id, problem.c_str());
        std::fflush(stdout);
        return;  // the image was not touched; the row re-reads its own state next draw
    }
    unsigned mask = Ps4Settings::Get().cheat_mask.load(std::memory_order_relaxed);
    if (index == 1) {
        mask |= 1u << cheat;
    } else {
        mask &= ~(1u << cheat);
    }
    Ps4Settings::Get().cheat_mask = mask;
    Ps4Settings::SaveQuality();
}

std::vector<Row> BuildModsRows() {
    std::vector<Row> rows;
    for (size_t i = 0; i < Ps4Cheats::Count(); ++i) {
        const Ps4Cheats::Cheat& entry = Ps4Cheats::Table()[i];
        const Ps4Cheats::State state = Ps4Cheats::Of(entry);
        const bool known = state != Ps4Cheats::State::Unknown;
        const bool on = state == Ps4Cheats::State::On;
        rows.push_back({i == 0 ? "Cheats" : nullptr, entry.title, Kind::Toggle, {"Off", "On"}, on ? 1 : 0,
                        known, entry.title, entry.body,
                        known ? (on ? "Active now. It is applied automatically every time the game starts, "
                                      "until you switch it off here."
                                    : "Switch it on and it takes effect immediately: no restart and no "
                                      "Apply button. It then stays on for every future start.")
                              : "Not available: the instruction bytes at these sites are not the "
                                "ones verified for this cheat, so this build is refused.",
                        nullptr, static_cast<int>(i)});
        if (!known) {
            rows.back().badge = "Unavailable";
            rows.back().badge_tone = 3;
        } else if (on) {
            rows.back().badge = "Active";
            rows.back().badge_tone = 1;
        }
    }
    return rows;
}

std::vector<Row> BuildRows(int tab) {
    if (tab == 1) {
        return BuildModsRows();
    }
    auto& s = Ps4Settings::Get();
    std::vector<Row> rows;
    volatile uint32_t* global = Ps4GuestImage::IntervalGlobal();
    const u32 interval = global ? *global : 2u;
    const int limit = s.fps_limit.load();
    int rate = interval >= 2 ? 0 : interval == 1 ? 1 : RateCount - 1;
    if (interval == 0) {
        for (int i = 2; i < RateCount - 1; ++i) {
            if (Rates[i] == limit) {
                rate = i;
            }
        }
    }
    std::vector<std::string> labels;
    for (int i = 0; i < RateCount; ++i) {
        labels.push_back(Rates[i] == 0    ? "Unlocked \xe2\x80\x94 experimental"
                         : Rates[i] == 30 ? "30 FPS \xe2\x80\x94 native"
                                          : std::to_string(Rates[i]) + " FPS \xe2\x80\x94 experimental");
    }
    std::string note = "Not available: this executable is not the verified 01.02 build.";
    if (global) {
        note = rate == 0 ? "Running at 30 FPS, as on the console."
               : rate == RateCount - 1
                   ? "Running uncapped: V-Sync or the GPU sets the limit."
                   : "Running at " + std::to_string(Rates[rate]) + " FPS.";
    }
    rows.push_back({"Frame rate", "Game frame rate", Kind::Choice, labels, rate, global != nullptr,
                    "Game frame rate",
                    "How many frames the game runs per second. Changes right away. 30 FPS is native; "
                    "the rest are experimental: the game clock follows measured time, but some "
                    "quick-time events in Chapters 6 and 9 can get stuck, so go back to 30 there if one "
                    "does. With V-Sync on, the display's refresh rate is the most you will see.",
                    note, SetGameFrameRate});
    if (global && rate != 0) {
        rows.back().badge = "Experimental";
        rows.back().badge_tone = 2;
    }
    const int mode = std::clamp(s.display_mode.load(), 0, int(Ps4Graphics::DisplayModeCount) - 1);
    rows.push_back({"Screen", "Window mode", Kind::Choice, {"Window", "Borderless", "Fullscreen"}, mode,
                    true, "Window mode",
                    "Window keeps the desktop around the game. Borderless fills the screen without a "
                    "border and switches instantly. Fullscreen takes the display for itself.",
                    "", [](int index) { Ps4Settings::Get().display_mode = index; }});
    rows.push_back({nullptr, "V-Sync", Kind::Toggle, {"Off", "On"}, s.vsync ? 1 : 0, true, "V-Sync",
                    "Waits for the monitor before showing each frame: no tearing, a little more "
                    "input delay. Off shows frames as soon as they are ready.",
                    "", [](int index) { Ps4Settings::Get().vsync = index == 1; }});
    int output = 0;
    for (int i = 0; i < 5; ++i) {
        if (OutputChoices[i] == s.output_res) {
            output = i;
        }
    }
    std::string size_note;
    if (const int w = s.image_width, h = s.image_height; w > 0 && h > 0) {
        size_note = "The game's picture is " + std::to_string(w) + "x" + std::to_string(h) + ".";
    }
    rows.push_back({nullptr, "Output resolution", Kind::Choice,
                    {"Automatic (window)", "1280x720", "1920x1080", "2560x1440", "3840x2160"}, output, true,
                    "Output resolution",
                    "The size the finished picture is scaled to before it is fitted to the window. "
                    "Automatic follows the window. The game still renders its scene at its own size.",
                    size_note, [](int index) { Ps4Settings::Get().output_res = OutputChoices[index]; }});
    const bool ultrawide_ok = s.ultrawide_available.load();
    rows.push_back({nullptr, "Ultrawide (21:9)", Kind::Toggle, {"Off", "Fill screen"},
                    s.ultrawide && ultrawide_ok ? 1 : 0, ultrawide_ok, "Ultrawide (21:9)",
                    "The game's picture is 2.4:1, the shape of a 21:9 monitor; normally it sits inside a "
                    "16:9 frame with black bars above and below, and a 21:9 screen then adds bars at the "
                    "sides too. Fill screen shows only the picture, scaled to the width of the window: "
                    "no bars on 21:9 displays. Menus, subtitles and prompts stay inside the picture. "
                    "Changes right away.",
                    ultrawide_ok ? std::string()
                                 : std::string("Not available: choose Original framing in the launcher. "
                                               "With 16:9 framing there are no bars to remove."),
                    [](int index) {
                        Ps4Settings::Get().ultrawide = index == 1;
                        Ps4Settings::SaveQuality();
                    }});
    rows.push_back({"Overlay", "FPS counter", Kind::Toggle, {"Off", "On"}, s.show_fps ? 1 : 0, true,
                    "FPS counter", "The performance card in a corner of the screen: frame rate, frame "
                    "time, GPU and CPU load and temperature, memory. F9 also shows or hides it.",
                    "", [](int index) { Ps4Settings::Get().show_fps = index == 1; }});

    // Quality: everything here changes right away except the render resolution of the Steam
    // Deck preset, which the launcher applies at the next start.
    const int preset = std::clamp(s.quality_preset.load(), 0, int(Ps4Settings::QualityCustom));
    rows.push_back({"Quality", "Preset", Kind::Choice, {"Steam Deck", "Low", "Medium", "High", "Custom"},
                    preset, true, "Quality preset",
                    "Sets shadows, anisotropic filtering and the frame rate together. Steam Deck: "
                    "shadows off, 2x filtering, 30 FPS and a 1280-wide render resolution. Low: the "
                    "same at your render resolution. Medium: shadows on, 8x, 60 FPS. High: everything "
                    "at full quality, 16x, 60 FPS. Changing any option below turns the preset into "
                    "Custom.",
                    preset != Ps4Settings::QualityCustom
                        ? "Restart the game to apply all preset changes (render resolution and "
                          "frame-rate profile). Shadows and filtering already changed."
                        : "",
                    OpenPresetDialog});
    rows.back().badge = "Restart";
    rows.back().badge_tone = 0;
    rows.push_back({nullptr, "Temporal AA", Kind::Toggle, {"Off", "On"}, s.temporal_aa ? 1 : 0,
                    true, "Temporal AA",
                    "On blends the previous frame into the game's multisample resolve: the edges "
                    "soften and the picture smears at high resolutions. Off resolves the same "
                    "samples with the game's own weights and no blend, so native-resolution "
                    "rendering is sharp and only thin edges shimmer. Changes right away.",
                    "", ApplyTemporalAA});
    rows.push_back({nullptr, "Shadows", Kind::Toggle, {"Off", "On"}, s.shadows ? 1 : 0, true, "Shadows",
                    "Off skips drawing the sun, lamp and torch shadow maps: the scene is lit as if "
                    "nothing cast a shadow. It saves GPU time and CPU time on the GPU thread, which "
                    "helps most on handhelds and older PCs. Changes right away.",
                    "", ApplyShadows});
    int aniso = 0;
    for (int i = 0; i < QualityAnisotropyCount; ++i) {
        if (QualityAnisotropies[i] == s.quality_anisotropy) aniso = i;
    }
    rows.push_back({nullptr, "Anisotropic filtering", Kind::Choice,
                    {"Launcher setting", "Off", "2x", "4x", "8x", "16x"}, aniso, true,
                    "Anisotropic filtering",
                    "Keeps floors and walls sharp at a glancing angle. Higher costs more memory "
                    "bandwidth; 2x is a good balance on the Steam Deck. Launcher setting uses the "
                    "value chosen in the launcher. Changes right away.",
                    "", ApplyQualityAnisotropy});
    return rows;
}

ImVec2 TextSize(float size, const char* text) {
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}
void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text, float wrap = 0.0f) {
    dl->AddText(ImGui::GetFont(), size, pos, color, text, nullptr, wrap);
}
void TextCentered(ImDrawList* dl, float size, ImVec2 a, ImVec2 b, ImU32 color, const char* text) {
    const ImVec2 t = TextSize(size, text);
    Text(dl, size, ImVec2((a.x + b.x - t.x) * 0.5f, (a.y + b.y - t.y) * 0.5f), color, text);
}
void Arrow(ImDrawList* dl, ImVec2 center, float r, bool right, ImU32 color) {
    if (right) {
        dl->AddTriangleFilled(ImVec2(center.x - r * 0.6f, center.y - r), ImVec2(center.x - r * 0.6f, center.y + r),
                              ImVec2(center.x + r * 0.8f, center.y), color);
    } else {
        dl->AddTriangleFilled(ImVec2(center.x + r * 0.6f, center.y - r), ImVec2(center.x + r * 0.6f, center.y + r),
                              ImVec2(center.x - r * 0.8f, center.y), color);
    }
}
void KeyCap(ImDrawList* dl, float s, ImVec2& pos, const char* key, float h = 34.0f) {
    const float size = 20.0f * s;
    h *= s;
    const ImVec2 t = TextSize(size, key);
    const ImVec2 a = pos, b = ImVec2(pos.x + std::max(t.x + 18.0f * s, h), pos.y + h);
    dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f * s), ImVec2(b.x, b.y + 2.0f * s), IM_COL32(0, 0, 0, 120), 6.0f * s);
    dl->AddRectFilled(a, b, SurfaceHi, 6.0f * s);
    dl->AddRect(a, b, LineStrong, 6.0f * s);
    TextCentered(dl, size, a, b, Ink, key);
    pos.x = b.x + 6.0f * s;
}
// A soft drop shadow: a few widening translucent layers behind a rounded rectangle.
void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float spread, float s) {
    const int layers = 8;
    for (int i = layers; i >= 1; --i) {
        const float g = spread * s * float(i) / float(layers);
        const float drop = spread * 0.35f * s;
        dl->AddRectFilled(ImVec2(a.x - g, a.y - g + drop), ImVec2(b.x + g, b.y + g + drop),
                          IM_COL32(0, 0, 0, int(26.0f * (1.0f - float(i - 1) / float(layers)))), rounding + g);
    }
}
ImU32 ToneInk(int tone) {
    return tone == 1 ? Good : tone == 2 ? Warn : tone == 3 ? Bad : Accent;
}
ImU32 ToneSoft(int tone) {
    return tone == 1 ? GoodSoft : tone == 2 ? WarnSoft : tone == 3 ? BadSoft : AccentSoft;
}
// A small rounded chip; returns its width.
float Chip(ImDrawList* dl, float s, ImVec2 pos, const char* text, int tone, float size = 17.0f) {
    const ImVec2 t = TextSize(size * s, text);
    const ImVec2 a = pos, b(pos.x + t.x + 18.0f * s, pos.y + t.y + 8.0f * s);
    dl->AddRectFilled(a, b, ToneSoft(tone), (b.y - a.y) * 0.5f);
    dl->AddRect(a, b, (ToneInk(tone) & 0x00ffffffu) | 0x80000000u, (b.y - a.y) * 0.5f);
    Text(dl, size * s, ImVec2(a.x + 9.0f * s, a.y + 4.0f * s), ToneInk(tone), text);
    return b.x - a.x;
}
// iOS-style switch: a pill track with a round knob.
void Switch(ImDrawList* dl, float s, ImVec2 center, bool on, bool focus, bool available) {
    const float w = 62.0f * s, h = 32.0f * s, r = h * 0.5f;
    const ImVec2 a(center.x - w * 0.5f, center.y - r), b(center.x + w * 0.5f, center.y + r);
    const ImU32 track = !available ? Surface2 : on ? Accent : SurfaceHi;
    dl->AddRectFilled(a, b, track, r);
    dl->AddRect(a, b, on && available ? (focus ? Focus : AccentLine) : (focus ? AccentLine : LineStrong), r, 0,
                focus ? 2.0f * s : 1.0f);
    const float kx = on ? b.x - r : a.x + r;
    dl->AddCircleFilled(ImVec2(kx, center.y + 1.5f * s), r - 3.0f * s, IM_COL32(0, 0, 0, 90), 24);
    dl->AddCircleFilled(ImVec2(kx, center.y), r - 4.0f * s, available ? (on ? AccentInk : Ink) : Dim, 24);
}
void Chevron(ImDrawList* dl, ImVec2 c, float r, bool right, ImU32 color, float thickness) {
    const float d = right ? 1.0f : -1.0f;
    const ImVec2 pts[3] = {ImVec2(c.x - d * r * 0.5f, c.y - r), ImVec2(c.x + d * r * 0.5f, c.y),
                           ImVec2(c.x - d * r * 0.5f, c.y + r)};
    dl->AddPolyline(pts, 3, color, 0, thickness);
}
std::string Upper(const char* text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    }
    return out;
}

void Change(std::vector<Row>& rows, int row, int step) {
    Row& r = rows[row];
    if (!r.available || r.values.empty()) {
        return;
    }
    const int count = int(r.values.size());
    const int next = r.kind == Kind::Toggle ? (step == 0 ? 1 - r.current : std::clamp(r.current + step, 0, 1))
                                            : (r.current + step + count) % count;
    if (next != r.current) {
        if (r.cheat >= 0) {
            ApplyCheatToggle(r.cheat, next);
        } else {
            r.apply(next);
        }
        r.current = next;
        dirty = true;
    }
}

void Draw() {
    if (requested_tab >= 0) {
        const int tab = std::clamp(requested_tab, 0, TabCount - 1);
        requested_tab = -1;
        if (tab != active_tab) {
            active_tab = tab;
            focused = 0;
            Ps4Settings::Get().menu_tab = tab;
            dirty = true;
        }
    }
    std::vector<Row> rows = BuildRows(active_tab);
    const int count = int(rows.size());
    focused = std::clamp(focused, 0, count - 1);

    // A dialog left open when the menu closed does not come back on the next open.
    static int last_frame = -2;
    if (ImGui::GetFrameCount() - last_frame > 1) {
        pending_preset = -1;
    }
    last_frame = ImGui::GetFrameCount();

    // Keyboard and controller (forwarded to ImGui while the menu is open).
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    bool close = false;
    // Set while the preset dialog takes the input; the buttons' mouse clicks are checked when
    // they are drawn.
    const bool dialog = pending_preset >= 0;
    if (dialog) {
        if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft) ||
            pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight) ||
            pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp) ||
            pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown)) {
            dialog_focus = 1 - dialog_focus;
        }
        if (confirm) {
            if (dialog_focus == 0) {
                ApplyQualityPreset(pending_preset);
                dirty = true;
            }
            pending_preset = -1;
        } else if (back) {
            pending_preset = -1;
        }
    } else {
        if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp)) {
            focused = (focused + count - 1) % count;
        }
        if (pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown)) {
            focused = (focused + 1) % count;
        }
        if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft)) {
            Change(rows, focused, -1);
        }
        if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight)) {
            Change(rows, focused, 1);
        }
        if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_Q)) {
            requested_tab = (active_tab + TabCount - 1) % TabCount;
        }
        if (pressed(ImGuiKey_GamepadR1) || pressed(ImGuiKey_E)) {
            requested_tab = (active_tab + 1) % TabCount;
        }
        if (confirm) {
            Change(rows, focused, rows[focused].kind == Kind::Toggle ? 0 : 1);
        }
        close = back;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float s = std::max(H / 1080.0f, 0.6f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##settings_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = io.MousePos;
    const auto inside = [&](ImVec2 p, ImVec2 q) {
        return mouse.x >= p.x && mouse.x < q.x && mouse.y >= p.y && mouse.y < q.y;
    };
    dl->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, Backdrop, Backdrop, BackdropLow, BackdropLow);

    // Panel: shadow, vertical gradient, a gold hairline along the top edge.
    const float margin = std::min(56.0f * s, W * 0.03f);
    const float round = 16.0f * s;
    const ImVec2 p0(margin, margin), p1(W - margin, H - margin);
    Shadow(dl, p0, p1, round, 26.0f, s);
    dl->AddRectFilled(p0, p1, Panel, round);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 96.0f * s), PanelTop, round, ImDrawFlags_RoundCornersTop);
    dl->AddRect(p0, p1, LineStrong, round);
    dl->AddLine(ImVec2(p0.x + round * 2, p0.y + 1.0f), ImVec2(p1.x - round * 2, p0.y + 1.0f), AccentLine, 1.5f * s);

    // Header: emblem and title on the left, tab pills in the middle-right with Q/E (L1/R1) caps.
    const float bar = 96.0f * s;
    const ImVec2 emblem(p0.x + 50.0f * s, p0.y + bar * 0.5f);
    dl->AddCircle(emblem, 22.0f * s, Accent, 32, 2.0f * s);
    dl->AddCircle(emblem, 15.0f * s, AccentLine, 32, 1.0f * s);
    for (int k = 0; k < 4; ++k) {
        const float ang = 3.14159265f * 0.5f * float(k) + 3.14159265f * 0.25f;
        const ImVec2 dir(std::cos(ang), std::sin(ang));
        dl->AddLine(ImVec2(emblem.x + dir.x * 15.0f * s, emblem.y + dir.y * 15.0f * s),
                    ImVec2(emblem.x + dir.x * 22.0f * s, emblem.y + dir.y * 22.0f * s), Accent, 2.0f * s);
    }
    dl->AddCircleFilled(emblem, 5.0f * s, Accent, 16);
    Text(dl, 27.0f * s, ImVec2(p0.x + 88.0f * s, p0.y + 22.0f * s), Ink, "THE ORDER: 1886");
    Text(dl, 18.0f * s, ImVec2(p0.x + 89.0f * s, p0.y + 54.0f * s), Muted, "Settings and mods");

    unsigned active_mods = 0;
    for (unsigned m = Ps4Settings::Get().cheat_mask.load(std::memory_order_relaxed); m; m &= m - 1) {
        ++active_mods;
    }
    const float pill_h = 50.0f * s, pill_w = 196.0f * s, pill_pad = 5.0f * s;
    const float tabs_w = pill_w * TabCount + pill_pad * 2;
    const float tabs_x = std::max(p0.x + 420.0f * s, (p0.x + p1.x - tabs_w) * 0.5f);
    const float tabs_y = p0.y + (bar - pill_h - pill_pad * 2) * 0.5f;
    const ImVec2 ta(tabs_x, tabs_y), tb(tabs_x + tabs_w, tabs_y + pill_h + pill_pad * 2);
    dl->AddRectFilled(ta, tb, Surface, (tb.y - ta.y) * 0.5f);
    dl->AddRect(ta, tb, Line, (tb.y - ta.y) * 0.5f);
    for (int t = 0; t < TabCount; ++t) {
        const ImVec2 a(ta.x + pill_pad + pill_w * float(t), ta.y + pill_pad), b(a.x + pill_w, a.y + pill_h);
        const bool active = t == active_tab;
        const bool hover = !dialog && inside(a, b);
        if (active) {
            dl->AddRectFilled(a, b, Accent, pill_h * 0.5f);
        } else if (hover) {
            dl->AddRectFilled(a, b, SurfaceHi, pill_h * 0.5f);
        }
        const char* name = TabNames[t];
        const ImVec2 nt = TextSize(25.0f * s, name);
        std::string count_text;
        float extra = 0.0f;
        if (t == 1 && active_mods > 0) {
            count_text = std::to_string(active_mods);
            extra = 34.0f * s;
        }
        const float tx = (a.x + b.x - nt.x - extra) * 0.5f, ty = (a.y + b.y - nt.y) * 0.5f;
        Text(dl, 25.0f * s, ImVec2(tx, ty), active ? AccentInk : (hover ? Ink : Muted), name);
        if (!count_text.empty()) {
            const ImVec2 c(tx + nt.x + 22.0f * s, (a.y + b.y) * 0.5f);
            dl->AddCircleFilled(c, 13.0f * s, active ? AccentInk : Good, 20);
            TextCentered(dl, 17.0f * s, ImVec2(c.x - 13.0f * s, c.y - 13.0f * s), ImVec2(c.x + 13.0f * s, c.y + 13.0f * s),
                         active ? Accent : AccentInk, count_text.c_str());
        }
        if (hover && !active && ImGui::IsMouseClicked(0)) {
            requested_tab = t;
        }
    }
    {
        ImVec2 q(ta.x - 46.0f * s, ta.y + (tb.y - ta.y - 34.0f * s) * 0.5f);
        KeyCap(dl, s, q, "Q");
        ImVec2 e(tb.x + 12.0f * s, q.y);
        KeyCap(dl, s, e, "E");
    }
    dl->AddLine(ImVec2(p0.x + 24.0f * s, p0.y + bar), ImVec2(p1.x - 24.0f * s, p0.y + bar), Line);

    // Content area: the list on the left, the description card on the right.
    const float left = p0.x + 40.0f * s, inner = p1.x - p0.x - 80.0f * s;
    const float list_w = inner * 0.57f;
    float top = p0.y + bar + 30.0f * s;
    const float bottom = p1.y - 84.0f * s;

    // Mods: a banner that says how mods work, above the list.
    if (active_tab == 1) {
        const char* head = "Mods apply automatically";
        const char* body = "Switching a mod changes the game the moment you press it: no restart and no Apply "
                           "button. Your choice is saved and applied again every time the game starts.";
        const float wrap = list_w - 110.0f * s;
        const float body_h = ImGui::GetFont()->CalcTextSizeA(21.0f * s, FLT_MAX, wrap, body).y;
        const float bh = std::max(64.0f * s, 34.0f * s + body_h + 30.0f * s);
        const ImVec2 a(left, top), b(left + list_w, top + bh);
        dl->AddRectFilled(a, b, GoodSoft, 12.0f * s);
        dl->AddRect(a, b, (Good & 0x00ffffffu) | 0x66000000u, 12.0f * s);
        const ImVec2 c(a.x + 42.0f * s, a.y + 40.0f * s);
        dl->AddCircleFilled(c, 19.0f * s, Good, 24);
        const ImVec2 tick[3] = {ImVec2(c.x - 8.0f * s, c.y + 0.5f * s), ImVec2(c.x - 2.5f * s, c.y + 6.0f * s),
                                ImVec2(c.x + 8.5f * s, c.y - 6.0f * s)};
        dl->AddPolyline(tick, 3, AccentInk, 0, 3.0f * s);
        Text(dl, 24.0f * s, ImVec2(a.x + 80.0f * s, a.y + 16.0f * s), Ink, head);
        Text(dl, 21.0f * s, ImVec2(a.x + 80.0f * s, a.y + 48.0f * s), Muted, body, wrap);
        top = b.y + 26.0f * s;
    }

    const float row_h = 62.0f * s, gap = 10.0f * s, rr = 12.0f * s;
    const float label_w = list_w * 0.52f;
    const float text = 25.0f * s;
    std::vector<float> ys(count), heads(count, 0.0f);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (rows[i].section) {
            const float h = (i > 0 ? 18.0f * s : 0.0f) + 20.0f * s + 14.0f * s;
            heads[i] = h;
            y += h;
        }
        ys[i] = y;
        y += row_h + gap;
    }
    const float content = y - gap;
    const float avail = bottom - top;
    float shift = 0.0f;
    if (content > avail) {
        shift = std::clamp(ys[focused] + row_h - avail, 0.0f, content - avail);
    }
    dl->PushClipRect(ImVec2(left - 8.0f * s, top - 4.0f * s), ImVec2(left + list_w + 24.0f * s, bottom + 4.0f * s), true);
    for (int i = 0; i < count; ++i) {
        Row& r = rows[i];
        const float ry = top + ys[i] - shift;
        if (r.section) {
            const float hy = ry - heads[i] + (i > 0 ? 18.0f * s : 0.0f);
            const std::string head = Upper(r.section);
            const ImVec2 t = TextSize(19.0f * s, head.c_str());
            const ImVec2 d(left + 6.0f * s, hy + t.y * 0.5f);
            dl->AddQuadFilled(ImVec2(d.x, d.y - 5.0f * s), ImVec2(d.x + 5.0f * s, d.y), ImVec2(d.x, d.y + 5.0f * s),
                              ImVec2(d.x - 5.0f * s, d.y), Accent);
            Text(dl, 19.0f * s, ImVec2(left + 20.0f * s, hy), Accent, head.c_str());
            dl->AddLine(ImVec2(left + 34.0f * s + t.x, hy + t.y * 0.55f), ImVec2(left + list_w, hy + t.y * 0.55f), Line);
        }
        if (ry + row_h < top || ry > bottom) {
            continue;  // scrolled out of the list area: not drawn, not clickable
        }
        const bool focus = i == focused;
        const ImVec2 a(left, ry), b(left + list_w, ry + row_h);
        const ImVec2 va(left + label_w, ry), vb = b;
        const bool hover = !dialog && inside(a, b) && ry >= top && ry + row_h <= bottom;
        // Mouse: a click focuses the row; on the value it changes it (left half back for choices).
        if (hover && ImGui::IsMouseClicked(0)) {
            focused = i;
            if (mouse.x >= va.x) {
                if (r.kind == Kind::Toggle) {
                    Change(rows, i, 0);
                } else {
                    Change(rows, i, mouse.x < (va.x + vb.x) * 0.5f ? -1 : 1);
                }
            }
        }
        dl->AddRectFilled(a, b, focus ? RowFocus : (hover ? SurfaceHi : Surface2), rr);
        dl->AddRect(a, b, focus ? AccentLine : Line, rr, 0, focus ? 1.5f * s : 1.0f);
        if (focus) {
            dl->AddRectFilled(ImVec2(a.x + 6.0f * s, a.y + 14.0f * s), ImVec2(a.x + 10.0f * s, b.y - 14.0f * s), Accent,
                              2.0f * s);
        }
        const float ly = a.y + (row_h - text) * 0.5f - 1.0f * s;
        Text(dl, text, ImVec2(a.x + 26.0f * s, ly), r.available ? Ink : Muted, r.label);
        if (r.badge) {
            const float lx = a.x + 26.0f * s + TextSize(text, r.label).x + 12.0f * s;
            if (lx + 90.0f * s < va.x) {
                Chip(dl, s, ImVec2(lx, a.y + (row_h - 25.0f * s) * 0.5f), r.badge, r.badge_tone, 16.0f);
            }
        }
        if (r.kind == Kind::Toggle) {
            const bool on = r.current == 1;
            const ImVec2 sc(vb.x - 52.0f * s, (a.y + b.y) * 0.5f);
            Switch(dl, s, sc, on, focus, r.available);
            const char* state = r.values[r.current].c_str();
            const ImVec2 t = TextSize(text, state);
            Text(dl, text, ImVec2(sc.x - 48.0f * s - t.x, (a.y + b.y - t.y) * 0.5f),
                 !r.available ? Dim : on ? Ink : Muted, state);
        } else {
            const ImVec2 ca(va.x + 8.0f * s, a.y + 8.0f * s), cb(vb.x - 8.0f * s, b.y - 8.0f * s);
            dl->AddRectFilled(ca, cb, focus && r.available ? AccentSoft : Surface, 9.0f * s);
            if (focus) {
                dl->AddRect(ca, cb, r.available ? AccentLine : LineStrong, 9.0f * s);
            }
            const ImU32 ink = r.available ? (focus ? Focus : Ink) : Muted;
            const ImVec2 t = TextSize(text, r.values[r.current].c_str());
            const int n = int(r.values.size());
            const bool dots = n > 1 && n <= 8;
            const float cyv = (ca.y + cb.y) * 0.5f - (dots ? 4.0f * s : 0.0f);
            Text(dl, text, ImVec2((ca.x + cb.x - t.x) * 0.5f, cyv - t.y * 0.5f), ink, r.values[r.current].c_str());
            if (dots) {
                const float step = 10.0f * s, dx0 = (ca.x + cb.x) * 0.5f - step * float(n - 1) * 0.5f;
                for (int k = 0; k < n; ++k) {
                    dl->AddCircleFilled(ImVec2(dx0 + step * float(k), cb.y - 8.0f * s), k == r.current ? 3.0f * s : 2.0f * s,
                                        k == r.current ? (r.available ? Accent : Muted) : Dim, 12);
                }
            }
            if (r.available) {
                const ImU32 arrow = focus ? Accent : Muted;
                Chevron(dl, ImVec2(ca.x + 22.0f * s, (ca.y + cb.y) * 0.5f), 7.0f * s, false, arrow, 2.5f * s);
                Chevron(dl, ImVec2(cb.x - 22.0f * s, (ca.y + cb.y) * 0.5f), 7.0f * s, true, arrow, 2.5f * s);
            }
        }
    }
    dl->PopClipRect();
    if (content > avail) {
        // Scroll indicator beside the list.
        const float tx = left + list_w + 12.0f * s;
        dl->AddRectFilled(ImVec2(tx, top), ImVec2(tx + 4.0f * s, bottom), Surface2, 2.0f * s);
        const float h = std::max(avail * avail / content, 40.0f * s);
        const float y0 = top + (avail - h) * (shift / (content - avail));
        dl->AddRectFilled(ImVec2(tx, y0), ImVec2(tx + 4.0f * s, y0 + h), AccentLine, 2.0f * s);
    }

    // Description card for the focused row.
    const Row& r = rows[focused];
    const ImVec2 da(left + list_w + 44.0f * s, p0.y + bar + 30.0f * s), db(p1.x - 40.0f * s, bottom);
    dl->AddRectFilled(da, db, Surface, 14.0f * s);
    dl->AddRectFilled(da, ImVec2(db.x, da.y + 92.0f * s), Surface2, 14.0f * s, ImDrawFlags_RoundCornersTop);
    dl->AddRect(da, db, Line, 14.0f * s);
    const float pad = 32.0f * s, wrap = db.x - da.x - pad * 2;
    float dy = da.y + pad - 4.0f * s;
    Text(dl, 31.0f * s, ImVec2(da.x + pad, dy), Focus, r.title);
    dy += 46.0f * s;
    dl->AddRectFilled(ImVec2(da.x + pad, dy), ImVec2(da.x + pad + 46.0f * s, dy + 3.0f * s), Accent, 1.5f * s);
    dy += 20.0f * s;
    {
        // Status chips: what the row is doing now.
        float cx = da.x + pad;
        if (r.cheat >= 0) {
            cx += Chip(dl, s, ImVec2(cx, dy), !r.available ? "Unavailable" : r.current ? "Active" : "Off",
                       !r.available ? 3 : r.current ? 1 : 0) + 10.0f * s;
            Chip(dl, s, ImVec2(cx, dy), "Applies automatically", 0);
            dy += 46.0f * s;
        } else if (r.badge) {
            Chip(dl, s, ImVec2(cx, dy), r.badge, r.badge_tone);
            dy += 46.0f * s;
        } else if (!r.available) {
            Chip(dl, s, ImVec2(cx, dy), "Unavailable", 3);
            dy += 46.0f * s;
        }
    }
    Text(dl, 24.0f * s, ImVec2(da.x + pad, dy), Ink, r.body, wrap);  // wrap is a width
    dy += ImGui::GetFont()->CalcTextSizeA(24.0f * s, FLT_MAX, wrap, r.body).y + 26.0f * s;
    if (!r.note.empty()) {
        const float nw = wrap - 44.0f * s;
        const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(22.0f * s, FLT_MAX, nw, r.note.c_str());
        const int tone = !r.available ? 3 : (r.cheat >= 0 && r.current) ? 1 : 0;
        const ImVec2 na(da.x + pad, dy), nb(db.x - pad, dy + t.y + 30.0f * s);
        dl->AddRectFilled(na, nb, ToneSoft(tone), 10.0f * s);
        dl->AddRectFilled(ImVec2(na.x, na.y), ImVec2(na.x + 4.0f * s, nb.y), ToneInk(tone), 2.0f * s);
        Text(dl, 22.0f * s, ImVec2(na.x + 24.0f * s, na.y + 15.0f * s), Ink, r.note.c_str(), nw);
    }

    // Footer: a reminder on the left, key hints on the right.
    const float hy = p1.y - 60.0f * s;
    dl->AddLine(ImVec2(p0.x + 24.0f * s, hy - 16.0f * s), ImVec2(p1.x - 24.0f * s, hy - 16.0f * s), Line);
    Text(dl, 19.0f * s, ImVec2(p0.x + 40.0f * s, hy + 7.0f * s), Dim,
         active_tab == 1 ? "Mods are saved as soon as you switch them." : "Settings are saved when you close the menu.");
    struct Hint { const char* label; const char* keys[2]; };
    const Hint hints[] = {{"Choose", {"\xe2\x96\xb2", "\xe2\x96\xbc"}}, {"Change", {"\xe2\x97\x80", "\xe2\x96\xb6"}},
                          {"Close", {"F1", "Esc"}}};
    float width = 0.0f;
    for (const Hint& h : hints) {
        width += TextSize(21.0f * s, h.label).x + 12.0f * s;
        for (const char* k : h.keys) {
            width += std::max(TextSize(20.0f * s, k).x + 18.0f * s, 34.0f * s) + 6.0f * s;
        }
        width += 26.0f * s;
    }
    ImVec2 hp(p1.x - 40.0f * s - width + 26.0f * s, hy);
    for (const Hint& h : hints) {
        Text(dl, 21.0f * s, ImVec2(hp.x, hp.y + 5.0f * s), Muted, h.label);
        hp.x += TextSize(21.0f * s, h.label).x + 12.0f * s;
        KeyCap(dl, s, hp, h.keys[0]);
        KeyCap(dl, s, hp, h.keys[1]);
        hp.x += 26.0f * s;
    }

    // Preset confirmation dialog: what the preset changes, then Apply / Cancel.
    Ps4Settings::QualityRecipe recipe{};
    if (pending_preset >= 0 && Ps4Settings::PresetRecipe(pending_preset, recipe)) {
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 170), round);
        const float dw = std::min(800.0f * s, W - 2 * margin - 40.0f * s);
        const float dh = 540.0f * s;
        const ImVec2 a((W - dw) * 0.5f, (H - dh) * 0.5f), b(a.x + dw, a.y + dh);
        Shadow(dl, a, b, 16.0f * s, 30.0f, s);
        dl->AddRectFilled(a, b, Panel, 16.0f * s);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + 100.0f * s), PanelTop, 16.0f * s, ImDrawFlags_RoundCornersTop);
        dl->AddRect(a, b, AccentLine, 16.0f * s, 0, 1.5f * s);
        const float dpad = 36.0f * s;
        float ty = a.y + dpad;
        const std::string title =
            std::string("Apply the ") + Ps4Settings::QualityPresetName(pending_preset) + " preset?";
        Text(dl, 32.0f * s, ImVec2(a.x + dpad, ty), Focus, title.c_str());
        ty += 58.0f * s;
        std::string aniso_text = recipe.anisotropy <= 1 ? std::string("Off")
                                                        : std::to_string(recipe.anisotropy) + "x";
        const std::string width_text = recipe.render_width > 0
                                           ? std::to_string(recipe.render_width) + " wide (after restart)"
                                           : std::string("Unchanged");
        const std::pair<const char*, std::string> lines[] = {
            {"Shadows", recipe.shadows ? "On" : "Off"},
            {"Anisotropic filtering", aniso_text},
            {"Frame rate", std::to_string(recipe.frame_rate) + " FPS"},
            {"Render resolution", width_text},
        };
        for (const auto& [label, value] : lines) {
            const ImVec2 la(a.x + dpad, ty), lb(b.x - dpad, ty + 44.0f * s);
            dl->AddRectFilled(la, lb, Surface2, 8.0f * s);
            Text(dl, 23.0f * s, ImVec2(la.x + 18.0f * s, la.y + 10.0f * s), Muted, label);
            const ImVec2 vt = TextSize(23.0f * s, value.c_str());
            Text(dl, 23.0f * s, ImVec2(lb.x - 18.0f * s - vt.x, la.y + 10.0f * s), Ink, value.c_str());
            ty += 52.0f * s;
        }
        ty += 8.0f * s;
        const char* note = "Shadows, filtering and the frame rate change right away. "
                           "Restart the game to apply all preset changes.";
        const float nw = dw - dpad * 2 - 40.0f * s;
        const float nh = ImGui::GetFont()->CalcTextSizeA(21.0f * s, FLT_MAX, nw, note).y + 26.0f * s;
        dl->AddRectFilled(ImVec2(a.x + dpad, ty), ImVec2(b.x - dpad, ty + nh), AccentSoft, 10.0f * s);
        dl->AddRectFilled(ImVec2(a.x + dpad, ty), ImVec2(a.x + dpad + 4.0f * s, ty + nh), Accent, 2.0f * s);
        Text(dl, 21.0f * s, ImVec2(a.x + dpad + 22.0f * s, ty + 13.0f * s), Ink, note, nw);
        // Buttons: arrows move between them, Enter or the south button presses, east cancels.
        const float bw = 200.0f * s, bh = 58.0f * s, by = b.y - dpad - bh;
        const ImVec2 apply_a(b.x - dpad - bw * 2 - 16.0f * s, by), apply_b(apply_a.x + bw, by + bh);
        const ImVec2 cancel_a(b.x - dpad - bw, by), cancel_b(cancel_a.x + bw, by + bh);
        if (inside(apply_a, apply_b)) dialog_focus = 0;
        if (inside(cancel_a, cancel_b)) dialog_focus = 1;
        const auto button = [&](ImVec2 p, ImVec2 q, const char* label, bool focus, bool primary) {
            const float br = (q.y - p.y) * 0.5f;
            dl->AddRectFilled(p, q, focus ? (primary ? Accent : SurfaceHi) : Surface2, br);
            dl->AddRect(p, q, focus ? Focus : LineStrong, br, 0, focus ? 2.0f * s : 1.0f);
            TextCentered(dl, 25.0f * s, p, q, focus && primary ? AccentInk : Ink, label);
        };
        button(apply_a, apply_b, "Apply", dialog_focus == 0, true);
        button(cancel_a, cancel_b, "Cancel", dialog_focus == 1, false);
        if (ImGui::IsMouseClicked(0)) {
            if (inside(apply_a, apply_b)) {
                ApplyQualityPreset(pending_preset);
                dirty = true;
                pending_preset = -1;
            } else if (inside(cancel_a, cancel_b)) {
                pending_preset = -1;
            }
        }
    }
    ImGui::End();
    if (close) {
        SetOpen(false);
    }
}

// Keep the interval the menu chose: the game writes its own at a few scripted moments.
void EnforceFrameRate() {
    const u32 chosen = chosen_interval.load(std::memory_order_relaxed);
    if (!chosen) {
        return;
    }
    const u32 interval = chosen - 1;
    if (volatile uint32_t* global = Ps4GuestImage::IntervalGlobal(); global && *global != interval) {
        *global = interval;
    }
}

} // namespace Menu

} // namespace

void OnPresented(bool loading_frame) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    std::scoped_lock lock{presentation_mutex};
    presentation_counter.Present(u64(now_ns), loading_frame);
    Ps4PerfStats::OnHostPresented(u64(now_ns), loading_frame);
}

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ps4";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(ps4_font_ttf),
                                   int(ps4_font_ttf_end - ps4_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: %s\n", SettingsMenuEnabled()
                    ? "menu ready (Insert or R3+L2)"
                    : "settings menu disabled; system dialogs and screenshots available");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const Ps4Png::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    const bool settings_menu_enabled = SettingsMenuEnabled();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.key == SDLK_F9) {
            if (down && !event.key.repeat) {
                auto& settings = Ps4Settings::Get();
                settings.show_fps = !settings.show_fps.load();
            }
            return true;
        }
        static const bool capture_key = [] {
            const char* env = std::getenv("ORDER1886_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (event.key.key == SDLK_F12) {
            if (down && !event.key.repeat) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            return true;
        }
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        // F1 (and Insert) open and close the menu; Esc closes it.
        const bool toggle_key = event.key.key == SDLK_F1 || event.key.key == SDLK_INSERT;
        if (settings_menu_enabled && down && !event.key.repeat &&
            (toggle_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(toggle_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 opens the optional host settings menu.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (settings_menu_enabled && down && l2_down) {
                SetOpen(!is_open);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (settings_menu_enabled && down && !l2_down && r3_down) {
                l2_down = true;
                SetOpen(!is_open);
                return true;
            }
            l2_down = down;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = Ps4Settings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    Ps4Settings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

bool Visible() {
    const auto& s = Ps4Settings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           choice_retiring || s.show_fps || NoticeActive());
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Ps4Png::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // ImGui needs a UI delta even when no presentation sample exists yet.
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_ui_frame).count();
    last_ui_frame = now;
    ConfirmRestart();
    Menu::EnforceFrameRate();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }

    if (Ps4Settings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (NoticeActive()) {
        Notice();
    }

    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    if (menu_open) {
        Menu::Draw();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace Ps4Overlay
