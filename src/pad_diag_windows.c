/* Windows gamepad diagnostics - the analogue of the Linux runtime wrapper's "pad check".
 *
 * The Linux wrapper (packaging/linux/order1886-runtime-wrapper.sh) scans /sys/bus/usb,
 * /sys/class/hidraw and /sys/class/input to explain why a pad is present but unusable:
 * a missing kernel driver, an unreadable /dev node (udev group), or Steam grabbing the
 * pad. None of that exists on Windows. Here the same failure modes are detected through
 * the Windows APIs:
 *
 *   1. XInput slots (XInputGetState) - an Xbox 360/One/Series pad bound to the in-box
 *      driver shows here.
 *   2. HID devices via SetupAPI - the "visible on the bus" check by vendor id
 *      (045E Microsoft, 054C Sony), the analogue of the wrapper's USB/hidraw scan.
 *   3. SDL's own enumeration - the list the runtime actually uses, the ground truth.
 *
 * Steam Input is the common Windows cause of "pad works everywhere but not here": when
 * enabled it can own the device, so the bus still shows it but the game's SDL open sees
 * nothing. The advice mirrors QUICKSTART's CONTROLLERS section, without udev or sysfs.
 *
 * Best-effort and never fatal: every line is prefixed "Order1886: pad check:" so the
 * launcher log stays greppable, and the scan is skipped when ORDER1886_PAD_DIAG=0.
 * The caller runs it only for a real boot (first argument is the prepared image, not a
 * --flag), exactly like the wrapper's guard.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <hidclass.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "SDL3/SDL.h"

/* XInput is loaded dynamically: xinput1_4.dll is absent on very old Windows and the
 * game must still run without a pad; a load-time import would fail the whole exe. */
typedef struct {
    unsigned short wButtons;
    unsigned char bLeftTrigger, bRightTrigger;
    short sThumbLX, sThumbLY, sRX, sRY;
} Order1886XinputGamepad;
typedef struct { Order1886XinputGamepad gamepad; unsigned int dwPacketNumber; } Order1886XinputState;
typedef DWORD(WINAPI *Order1886XinputGetState)(DWORD, Order1886XinputState *);

static int xinput_connected_slots(int *out, int cap) {
    HMODULE x = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!x) x = LoadLibraryExW(L"xinput1_3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!x) x = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!x) return -1; /* no XInput at all */
    Order1886XinputGetState get_state =
        (Order1886XinputGetState)(void *)GetProcAddress(x, "XInputGetState");
    int found = 0;
    if (get_state) {
        for (DWORD slot = 0; slot < 4 && found < cap; ++slot) {
            Order1886XinputState st;
            if (get_state(slot, &st) == ERROR_SUCCESS) out[found++] = (int)slot;
        }
    }
    FreeLibrary(x);
    return found;
}

/* HID-class device instances carry the vendor id in their path (...&vid_045e&pid_...). */
static void hid_vendors_seen(int *xbox, int *sony) {
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface;
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_HID, index, &iface)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, NULL, 0, &needed, NULL);
        if (!needed || needed > 8192) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed, NULL, NULL)) {
            WCHAR path[1024];
            lstrcpynW(path, detail->DevicePath, 1024);
            for (int i = 0; path[i]; ++i) path[i] = (WCHAR)towlower(path[i]);
            if (wcsstr(path, L"vid_045e")) *xbox = 1;
            if (wcsstr(path, L"vid_054c")) *sony = 1;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(info);
}

void order1886_pad_check_windows(void) {
    const char *skip = getenv("ORDER1886_PAD_DIAG");
    if (skip && !strcmp(skip, "0")) return;

    int sdl_pads = 0;
    const bool sdl_ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (sdl_ready) {
        for (int waited = 0; waited < 20; ++waited) { /* pads appear after a pump */
            SDL_PumpEvents();
            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            sdl_pads = count;
            SDL_free(ids);
            if (count > 0) break;
            SDL_Delay(10);
        }
    }

    int slots[4];
    const int xinput = xinput_connected_slots(slots, 4);
    int xbox_on_bus = 0, sony_on_bus = 0;
    hid_vendors_seen(&xbox_on_bus, &sony_on_bus);

    if (xinput <= 0 && !xbox_on_bus && !sony_on_bus && sdl_pads == 0) {
        puts("Order1886: pad check: no gamepad seen by XInput, HID or SDL; "
             "keyboard (space/cross) still plays. Connect a pad and press its Xbox/guide button.");
    } else if (sdl_pads == 0 && (xinput > 0 || xbox_on_bus || sony_on_bus)) {
        puts("Order1886: pad check: a pad is connected but the runtime cannot open it. "
             "If Steam is running it may own the controller: quit Steam while playing, or "
             "disable Steam Input for that controller family in Settings > Controllers.");
    } else {
        printf("Order1886: pad check: %d gamepad(s) ready", sdl_pads);
        if (xinput > 0) {
            fputs(", XInput slot(s):", stdout);
            for (int i = 0; i < xinput; ++i) printf(" %d", slots[i]);
        }
        putchar('\n');
    }
    if (sdl_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

#endif /* _WIN32 */
