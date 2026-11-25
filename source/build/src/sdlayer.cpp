// SDL interface layer for the Build Engine
// Use SDL 1.2 or 2.0 from http://www.libsdl.org

#include <signal.h>

#include "a.h"
#include "build.h"
#include "cache1d.h"
#include "compat.h"
#include "build_cpuid.h"
#include "engine_priv.h"
#include "osd.h"
#include "palette.h"
#include "renderlayer.h"
#include "sdl_inc.h"
#include "softsurface.h"

#ifdef EDUKE32_GAMESTATION_GO
# include "scancodes.h"
# include <errno.h>
# include <linux/input.h>
# include <fcntl.h>
# include <sys/ioctl.h>
# include <sys/mman.h>
# include <sys/poll.h>
# include <unistd.h>
# include "SDL_syswm.h"
#endif

#if SDL_MAJOR_VERSION >= 2
# include "imgui.h"
# include "imgui_impl_sdl2.h"
#ifdef USE_OPENGL
# include "imgui_impl_opengl3.h"
#endif
#endif

#ifdef USE_OPENGL
# include "glad/glad.h"
# include "glbuild.h"
# include "glsurface.h"
#endif

#if defined HAVE_GTK2
# include "gtkbits.h"
#endif

#ifdef __ANDROID__
# include <android/log.h>
#elif defined __APPLE__
# include "osxbits.h"
# include <mach/mach.h>
# include <mach/mach_time.h>
#elif defined GEKKO
# include "wiibits.h"
# include <ogc/lwp.h>
# include <ogc/lwp_watchdog.h>
#elif defined _WIN32
# include "winbits.h"
#endif

#include "vfs.h"
#include "communityapi.h"

#define MICROPROFILE_IMPL
#include "microprofile.h"

#if SDL_MAJOR_VERSION >= 2
static SDL_version linked;
#else
#define SDL_JoystickNameForIndex(x) SDL_JoystickName(x)
#endif

#if !defined STARTUP_SETUP_WINDOW
int32_t startwin_open(void) { return 0; }
int32_t startwin_close(void) { return 0; }
int32_t startwin_puts(const char *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_idle(void *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_settitle(const char *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_run(void) { return 0; }
bool startwin_isopen(void) { return false; }
#endif

/// These can be useful for debugging sometimes...
//#define SDL_WM_GrabInput(x) SDL_WM_GrabInput(SDL_GRAB_OFF)
//#define SDL_ShowCursor(x) SDL_ShowCursor(SDL_ENABLE)

// undefine to restrict windowed resolutions to conventional sizes
#define ANY_WINDOWED_SIZE

// fix for mousewheel
int32_t inputchecked = 0;

char quitevent=0, appactive=1, novideo=0;

// video
static SDL_Surface *sdl_surface/*=NULL*/;

static vec2_t sdl_resize;
static int sdl_minimized;

#if SDL_MAJOR_VERSION >= 2
static SDL_Window *sdl_window;

#ifdef EDUKE32_GAMESTATION_GO
// The KMSDRM window-surface blit path (SDL_UpdateWindowSurface) crashes in the
// GameStation Go's Mali r7p0 blob when driving the HDMI connector at
// 1920x1080. Present through an SDL software renderer + streaming texture
// instead; that path is stable on the device (validated with a standalone
// KMSDRM test program). gsg_presenter_surface backs sdl_surface when active.
static SDL_Renderer *gsg_presenter_renderer;
static SDL_Texture *gsg_presenter_texture;
static SDL_Surface *gsg_presenter_surface;
static int32_t gsg_presenter_w, gsg_presenter_h;
static bool gsg_presenter_active;
static bool gsg_presenter_disabled;
static bool gsg_fps_log;

static void gsg_presenter_destroy(void)
{
    if (gsg_presenter_texture)
        SDL_DestroyTexture(gsg_presenter_texture);
    gsg_presenter_texture = NULL;

    if (gsg_presenter_renderer)
        SDL_DestroyRenderer(gsg_presenter_renderer);
    gsg_presenter_renderer = NULL;

    if (gsg_presenter_surface)
    {
        if (sdl_surface == gsg_presenter_surface)
            sdl_surface = NULL;
        SDL_FreeSurface(gsg_presenter_surface);
        gsg_presenter_surface = NULL;
    }

    gsg_presenter_active = false;
}

// ---- Direct DRM dumb-buffer presenter -----------------------------------
// The vendor SDL's KMSDRM presentation funnels every frame through a CPU copy
// into write-combined Mali/GBM memory (~130 ms per 1080p frame). The
// GameStation's working native launchers (RetroArch et al.) instead render
// into their own GEM dumb buffers and page-flip them, which is cheap. We do
// the same, borrowing the DRM fd SDL already owns so there is no master
// conflict. No libdrm dependency: the few ioctls used are stable kernel UAPI.

struct gsg_drm_modeinfo
{
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct gsg_drm_card_res
{
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct gsg_drm_crtc
{
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct gsg_drm_modeinfo mode;
};

struct gsg_drm_encoder
{
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct gsg_drm_connector
{
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id;
    uint32_t connector_type, connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height, subpixel, pad;
};

struct gsg_drm_fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct gsg_drm_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct gsg_drm_create_dumb { uint32_t height, width, bpp, flags; uint32_t handle, pitch; uint64_t size; };
struct gsg_drm_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct gsg_drm_destroy_dumb { uint32_t handle; };
struct gsg_drm_event { uint32_t type, length; };
struct gsg_drm_client_cap { uint64_t capability, value; };
struct gsg_drm_plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };
struct gsg_drm_get_plane
{
    uint32_t plane_id, crtc_id, fb_id;
    uint32_t possible_crtcs, gamma_size, count_format_types;
    uint64_t format_type_ptr;
};
struct gsg_drm_set_plane
{
    uint32_t plane_id, crtc_id, fb_id, flags;
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t src_x, src_y, src_h, src_w; // 16.16 fixed point; note h before w (UAPI)
};
struct gsg_drm_wait_vblank { int32_t type; uint32_t sequence; int32_t tval_sec; int32_t tval_usec; };

#define GSG_DRM_IOWR(nr, t) _IOWR('d', nr, t)
#define GSG_DRM_IOCTL_GETRESOURCES GSG_DRM_IOWR(0xA0, struct gsg_drm_card_res)
#define GSG_DRM_IOCTL_GETCRTC      GSG_DRM_IOWR(0xA1, struct gsg_drm_crtc)
#define GSG_DRM_IOCTL_SETCRTC      GSG_DRM_IOWR(0xA2, struct gsg_drm_crtc)
#define GSG_DRM_IOCTL_GETENCODER   GSG_DRM_IOWR(0xA6, struct gsg_drm_encoder)
#define GSG_DRM_IOCTL_GETCONNECTOR GSG_DRM_IOWR(0xA7, struct gsg_drm_connector)
#define GSG_DRM_IOCTL_ADDFB        GSG_DRM_IOWR(0xAE, struct gsg_drm_fb_cmd)
#define GSG_DRM_IOCTL_RMFB         GSG_DRM_IOWR(0xAF, uint32_t)
#define GSG_DRM_IOCTL_PAGE_FLIP    GSG_DRM_IOWR(0xB0, struct gsg_drm_page_flip)
#define GSG_DRM_IOCTL_CREATE_DUMB  GSG_DRM_IOWR(0xB2, struct gsg_drm_create_dumb)
#define GSG_DRM_IOCTL_MAP_DUMB     GSG_DRM_IOWR(0xB3, struct gsg_drm_map_dumb)
#define GSG_DRM_IOCTL_DESTROY_DUMB GSG_DRM_IOWR(0xB4, struct gsg_drm_destroy_dumb)
#define GSG_DRM_IOCTL_SET_CLIENT_CAP _IOW('d', 0x0d, struct gsg_drm_client_cap)
#define GSG_DRM_IOCTL_GETPLANERES  GSG_DRM_IOWR(0xB5, struct gsg_drm_plane_res)
#define GSG_DRM_IOCTL_GETPLANE     GSG_DRM_IOWR(0xB6, struct gsg_drm_get_plane)
#define GSG_DRM_IOCTL_SETPLANE     GSG_DRM_IOWR(0xB7, struct gsg_drm_set_plane)
#define GSG_DRM_IOCTL_WAIT_VBLANK  GSG_DRM_IOWR(0x3a, struct gsg_drm_wait_vblank)

#define GSG_DRM_EVENT_FLIP_COMPLETE 0x02
#define GSG_DRM_PAGE_FLIP_EVENT     0x01
#define GSG_DRM_MODE_CONNECTED      1
#define GSG_DRM_MODE_TYPE_PREFERRED 8
#define GSG_DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define GSG_DRM_FORMAT_RGB565       0x36314752 // fourcc('R','G','1','6')

static int gsg_drm_fd = -1; // borrowed from SDL, never closed by us
static uint32_t gsg_drm_crtc_id, gsg_drm_connector_id;
static uint32_t gsg_drm_crtc_index;
static struct gsg_drm_modeinfo gsg_drm_mode;
static struct gsg_drm_crtc gsg_drm_saved_crtc;
static struct { uint32_t handle, fb_id, pitch; uint64_t size; void *map; } gsg_drm_bufs[2];
static int32_t gsg_drm_back = 1;
static bool gsg_drm_flip_pending;
static bool gsg_drm_active;
static bool gsg_drm_disabled;
static int32_t gsg_drm_failed_x, gsg_drm_failed_y; // last size with no matching connector (retry on change)

// Hardware-scaled overlay plane: present at engine resolution and let the VOP
// stretch it to the display mode, quartering our write-combined traffic.
static uint32_t gsg_drm_overlay_plane;
static struct { uint32_t handle, fb_id, pitch; uint64_t size; void *map; } gsg_drm_overlay_bufs[2];
static int32_t gsg_drm_overlay_back;
static vec2_t gsg_drm_overlay_res;
static bool gsg_drm_overlay_active;
static bool gsg_drm_overlay_failed;

int32_t g_gameStationHomePressed;
int32_t g_gameStationShowMenu;
int32_t g_gameStationMenuActive;
static bool gsg_input_debug;

// The bundled SDK SDL2's KMSDRM input never surfaces the GameStation's USB
// trackball ("XC Track Ball", a mouse-class evdev device); read it directly
// and inject SDL mouse events. The launcher/vendor SDL handle it natively.
static int gsg_trackball_fd = -1;
static uint32_t gsg_trackball_nextscan;
static int32_t gsg_trackball_x = -1, gsg_trackball_y = -1; // virtual cursor, menus read absolute position

// mact globals; the bundled SDL reports no mouse, so mouse support would
// otherwise stay disabled even though the trackball is present.
extern bool CONTROL_MousePresent;
extern bool CONTROL_MouseEnabled;

// Probing every /dev/input/event* node stalls the main loop for ~180 ms on
// this device, so the periodic rescans below only probe when the kernel's
// input device list changes (plus a few settle cycles for late /dev nodes).
struct gsg_devscan_t
{
    uint32_t hash;
    int settle;
};

static bool gsg_input_devices_changed(gsg_devscan_t *scan)
{
    int const fd = open("/proc/bus/input/devices", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return true;

    uint32_t hash = 2166136261u;
    char buf[1024];
    ssize_t got;
    while ((got = read(fd, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < got; i++)
            hash = (hash ^ (uint8_t)buf[i]) * 16777619u;
    close(fd);

    if (!scan->hash || scan->hash != hash)
    {
        scan->hash = hash ? hash : 1;
        scan->settle = 3;
        return true;
    }
    if (scan->settle > 0)
    {
        scan->settle--;
        return true;
    }
    return false;
}

static void gsg_trackball_try_open(void)
{
    if (gsg_trackball_fd >= 0)
        return;

    uint32_t const now = SDL_GetTicks();
    if ((int32_t)(now - gsg_trackball_nextscan) < 0)
        return;
    gsg_trackball_nextscan = now + 2000;

    static gsg_devscan_t devscan;
    if (!gsg_input_devices_changed(&devscan))
        return;

    char const *forced = getenv("EDUKE32_GSG_TRACKBALL_DEVICE");

    for (int i = 0; i < 32; i++)
    {
        char path[40];
        if (forced && forced[0])
        {
            Bstrncpyz(path, forced, sizeof(path));
            i = 32; // single attempt
        }
        else
            Bsprintf(path, "/dev/input/event%d", i);

        int const fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            if (forced && forced[0])
                break;
            continue;
        }

        char name[128] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
            name[0] = '\0';

        char lowered[128];
        size_t j = 0;
        for (; name[j] && j < sizeof(lowered) - 1; j++)
            lowered[j] = (char)Btolower(name[j]);
        lowered[j] = '\0';

        bool const isTrackball = (forced && forced[0]) || (Bstrstr(lowered, "track") && Bstrstr(lowered, "ball"));

        if (isTrackball)
        {
            gsg_trackball_fd = fd;
            // Take exclusive ownership so SDL's evdev can't swallow the events.
            int const grabbed = ioctl(fd, EVIOCGRAB, 1);
            CONTROL_MousePresent = true;
            CONTROL_MouseEnabled = true;
            LOG_F(INFO, "GameStation: trackball on %s ('%s'), grab %s.", path, name,
                  grabbed == 0 ? "ok" : strerror(errno));
            return;
        }

        close(fd);
    }

    static bool notFoundLogged;
    if (!notFoundLogged)
    {
        notFoundLogged = true;
        LOG_F(INFO, "GameStation: no trackball device found (will keep trying).");
    }
}

void GSG_PrewarmTrackball(void)
{
    gsg_trackball_try_open();
}

// ---- Direct numeric keypad reader ------------------------------------------
// The Atari-style numeric keypad reports standard keycodes (numpad 0-9 plus
// KEY_9 for '*' and KEY_0 for '#') on an agpio-keys node that SDL's keyboard
// layer never delivers to the game on this device. Read agpio-keys nodes
// directly (passive; no grab) and inject the twelve keypad keys as keyboard
// 1-9, 0, Keypad *, Keypad / so they are assignable through the keyboard
// key-configuration screens (and drive weapon selection by default).
static int gsg_keypad_fds[8];
static int gsg_keypad_nodes[8]; // event node index per fd (rescan dedupe)
static int gsg_keypad_fd_count;
static uint32_t gsg_keypad_nextscan;
static bool gsg_keypad_is_zadc[8];
static bool gsg_keypad_is_pad[8];
static uint32_t gsg_lastVolumeKeyMs[2]; // KEY_VOLUMEDOWN / KEY_VOLUMEUP on zadc-keys
static bool gsg_volumeKeyHeld[2];

static void gsg_volume_note(int code, int value)
{
    int const idx = code == KEY_VOLUMEDOWN ? 0 : 1;
    gsg_lastVolumeKeyMs[idx] = SDL_GetTicks();
    gsg_volumeKeyHeld[idx] = value != 0;
}

static void gsg_zadc_drain(void)
{
    for (int i = 0; i < gsg_keypad_fd_count; i++)
    {
        if (!gsg_keypad_is_zadc[i])
            continue;

        struct input_event ev;
        while (read(gsg_keypad_fds[i], &ev, sizeof(ev)) == sizeof(ev))
            if (ev.type == EV_KEY && (ev.code == KEY_VOLUMEDOWN || ev.code == KEY_VOLUMEUP))
                gsg_volume_note(ev.code, ev.value);
    }
}

// In game mode the driver remaps the volume keys to game inputs (the remapped
// control differs per game and config) while still emitting the real key on
// zadc-keys. While a real volume key is held, and briefly after, new game
// presses are phantom. Only presses are dropped, so releases cannot stick.
static bool gsg_volume_block_active(void)
{
    gsg_zadc_drain();

    uint32_t const now = SDL_GetTicks();
    for (int i = 0; i < 2; i++)
    {
        uint32_t const last = gsg_lastVolumeKeyMs[i];
        if (last && (uint32_t)(now - last) < (gsg_volumeKeyHeld[i] ? 1000u : 250u))
            return true;
    }
    return false;
}

static void gsg_keypad_push(int sdlScancode, int value, uint32_t timestamp)
{
    if (value > 1)
        return; // skip autorepeat

    if (gsg_input_debug)
        LOG_F(INFO, "GSGDBG keypad push scancode=%d state=%d", sdlScancode, value);

    SDL_Event key = {};
    key.type = value ? SDL_KEYDOWN : SDL_KEYUP;
    key.key.type = key.type;
    key.key.timestamp = timestamp;
    key.key.state = value ? SDL_PRESSED : SDL_RELEASED;
    key.key.keysym.scancode = (SDL_Scancode)sdlScancode;
    key.key.keysym.sym = SDL_SCANCODE_TO_KEYCODE(sdlScancode);
    SDL_PushEvent(&key);
}

// SETTINGS and CREDIT have no controller mapping, so they are delivered as
// controller Guide / Paddle 1 button events.
static void gsg_keypad_push_button(int button, int value, uint32_t timestamp)
{
    if (value > 1)
        return; // skip autorepeat

    if (gsg_input_debug)
        LOG_F(INFO, "GSGDBG keypad push controller button=%d state=%d", button, value);

    SDL_Event ev = {};
    ev.type = value ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.type = ev.type;
    ev.cbutton.timestamp = timestamp;
    ev.cbutton.button = (Uint8)button;
    ev.cbutton.state = value ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&ev);
}

// Open every agpio-keys instance. The OS creates an additional instance (and
// the trackball node) when a game takes over, so rescan periodically for
// nodes that appear late; opened nodes are deduped by index. zadc-keys is
// watched only to timestamp the real volume keys (phantom-input filter).
static void gsg_keypad_try_open(void)
{
    uint32_t const now = SDL_GetTicks();
    if ((int32_t)(now - gsg_keypad_nextscan) < 0)
        return;
    gsg_keypad_nextscan = now + 2000;

    static gsg_devscan_t devscan;
    if (!gsg_input_devices_changed(&devscan))
        return;

    for (int i = 0; i < 32 && gsg_keypad_fd_count < (int)ARRAY_SIZE(gsg_keypad_fds); i++)
    {
        bool already = false;
        for (int j = 0; j < gsg_keypad_fd_count; j++)
            if (gsg_keypad_nodes[j] == i)
                { already = true; break; }
        if (already)
            continue;

        char path[40];
        Bsprintf(path, "/dev/input/event%d", i);

        int const fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        char name[128] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
            name[0] = '\0';

        bool isPad = false;
        {
            char lowered[128];
            size_t k = 0;
            for (; name[k] && k < sizeof(lowered) - 1; k++)
                lowered[k] = (char)Btolower(name[k]);
            lowered[k] = '\0';
            isPad = Bstrstr(lowered, "litestar") != nullptr;
        }

        if (isPad || Bstrcmp(name, "agpio-keys") == 0 || Bstrcmp(name, "zadc-keys") == 0)
        {
            gsg_keypad_is_zadc[gsg_keypad_fd_count] = name[0] == 'z';
            gsg_keypad_is_pad[gsg_keypad_fd_count] = isPad;
            gsg_keypad_nodes[gsg_keypad_fd_count] = i;
            gsg_keypad_fds[gsg_keypad_fd_count++] = fd;
            LOG_F(INFO, "GameStation: input watch on %s ('%s'), passive.", path, name);
            continue;
        }

        close(fd);
    }
}

static void gsg_keypad_poll(void)
{
    if (gsg_keypad_fd_count < (int)ARRAY_SIZE(gsg_keypad_fds))
        gsg_keypad_try_open();

    for (int i = 0; i < gsg_keypad_fd_count; i++)
    {
        struct input_event ev;
        ssize_t result;

        while ((result = read(gsg_keypad_fds[i], &ev, sizeof(ev))) == sizeof(ev))
        {
            if (gsg_input_debug && ev.type != EV_SYN)
                LOG_F(INFO, "GSGDBG keypad raw fd=%d type=%u code=%u value=%d", gsg_keypad_fds[i], ev.type, ev.code, ev.value);

            if (ev.type != EV_KEY)
                continue;

            if (gsg_keypad_is_zadc[i])
            {
                // Just note real volume-key activity; the phantom game inputs
                // the driver remaps them to are filtered on the event path.
                if (ev.code == KEY_VOLUMEDOWN || ev.code == KEY_VOLUMEUP)
                    gsg_volume_note(ev.code, ev.value);
                continue;
            }

            // Volume up/down (115/114) never become game input.
            if (ev.code == KEY_VOLUMEDOWN || ev.code == KEY_VOLUMEUP)
                continue;

            uint32_t const ms = (uint32_t)ev.time.tv_sec * 1000u + (uint32_t)ev.time.tv_usec / 1000u;

            // The wireless pad's nodes carry only its keypad here (the rest
            // of the pad is read through SDL); skip its other keys.
            if (gsg_keypad_is_pad[i] && ev.code != KEY_9 && ev.code != KEY_0 &&
                (ev.code < KEY_KP7 || ev.code > KEY_KP0))
                continue;

            switch (ev.code)
            {
            case KEY_KP1: gsg_keypad_push(SDL_SCANCODE_1, ev.value, ms); break;
            case KEY_KP2: gsg_keypad_push(SDL_SCANCODE_2, ev.value, ms); break;
            case KEY_KP3: gsg_keypad_push(SDL_SCANCODE_3, ev.value, ms); break;
            case KEY_KP4: gsg_keypad_push(SDL_SCANCODE_4, ev.value, ms); break;
            case KEY_KP5: gsg_keypad_push(SDL_SCANCODE_5, ev.value, ms); break;
            case KEY_KP6: gsg_keypad_push(SDL_SCANCODE_6, ev.value, ms); break;
            case KEY_KP7: gsg_keypad_push(SDL_SCANCODE_7, ev.value, ms); break;
            case KEY_KP8: gsg_keypad_push(SDL_SCANCODE_8, ev.value, ms); break;
            case KEY_KP9: gsg_keypad_push(SDL_SCANCODE_9, ev.value, ms); break;
            case KEY_KP0: gsg_keypad_push(SDL_SCANCODE_0, ev.value, ms); break;
            case KEY_9:   gsg_keypad_push_button(SDL_CONTROLLER_BUTTON_LEFTSTICK, ev.value, ms); break;  // '*'
            case KEY_0:   gsg_keypad_push_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, ev.value, ms); break; // '#'
            case KEY_Q:    gsg_keypad_push_button(SDL_CONTROLLER_BUTTON_GUIDE, ev.value, ms); break;   // SETTINGS
            case BTN_MODE: gsg_keypad_push_button(SDL_CONTROLLER_BUTTON_PADDLE1, ev.value, ms); break; // CREDIT
            }
        }

        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            close(gsg_keypad_fds[i]);
            gsg_keypad_fds[i] = gsg_keypad_fds[--gsg_keypad_fd_count];
            gsg_keypad_nodes[i] = gsg_keypad_nodes[gsg_keypad_fd_count];
            gsg_keypad_is_zadc[i] = gsg_keypad_is_zadc[gsg_keypad_fd_count];
            gsg_keypad_is_pad[i] = gsg_keypad_is_pad[gsg_keypad_fd_count];
            --i;
        }
    }
}

// The paired wireless gamepad also carries a trackball, reporting on the
// same vendor-quirk axes (REL_Z/REL_RX) via its "LiteStar PC-GamePad" evdev
// node. Read it passively (NO EVIOCGRAB -- SDL keeps the pad's buttons) and
// merge the deltas with the console trackball's.
static int gsg_padtrackball_fd = -1;
static uint32_t gsg_padtrackball_nextscan;

static void gsg_padtrackball_try_open(void)
{
    if (gsg_padtrackball_fd >= 0)
        return;

    uint32_t const now = SDL_GetTicks();
    if ((int32_t)(now - gsg_padtrackball_nextscan) < 0)
        return;
    gsg_padtrackball_nextscan = now + 2000;

    static gsg_devscan_t devscan;
    if (!gsg_input_devices_changed(&devscan))
        return;

    for (int i = 0; i < 32; i++)
    {
        char path[40];
        Bsprintf(path, "/dev/input/event%d", i);

        int const fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        char name[128] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
            name[0] = '\0';

        char lowered[128];
        size_t j = 0;
        for (; name[j] && j < sizeof(lowered) - 1; j++)
            lowered[j] = (char)Btolower(name[j]);
        lowered[j] = '\0';

        if (Bstrstr(lowered, "litestar"))
        {
            uint8_t relbits[(REL_MAX + 8) / 8] = {};
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) > 0 &&
                (relbits[REL_Z >> 3] & (1 << (REL_Z & 7))) &&
                (relbits[REL_RX >> 3] & (1 << (REL_RX & 7))))
            {
                gsg_padtrackball_fd = fd;
                LOG_F(INFO, "GameStation: gamepad trackball on %s ('%s'), passive.", path, name);
                return;
            }
        }

        close(fd);
    }
}

static void gsg_injectMenuKey(SDL_Scancode scancode)
{
    for (int state = 1; state >= 0; state--)
    {
        SDL_Event key = {};
        key.type = state ? SDL_KEYDOWN : SDL_KEYUP;
        key.key.type = key.type;
        key.key.timestamp = SDL_GetTicks();
        key.key.state = state ? SDL_PRESSED : SDL_RELEASED;
        key.key.keysym.scancode = scancode;
        key.key.keysym.sym = SDL_SCANCODE_TO_KEYCODE(scancode);
        SDL_PushEvent(&key);
    }
}

#define GSG_TRACKBALL_MENU_STEP 4 // evdev counts per menu step

static void gsg_trackball_poll(void)
{
    if (gsg_padtrackball_fd < 0)
        gsg_padtrackball_try_open();

    if (gsg_trackball_fd < 0)
        gsg_trackball_try_open();

    if (gsg_trackball_fd < 0 && gsg_padtrackball_fd < 0)
        return;

    int relX = 0, relY = 0;
    struct input_event ev;
    ssize_t result = 0;

    if (gsg_padtrackball_fd >= 0)
    {
        // Passive drain of the gamepad's trackball (vendor REL_Z/REL_RX
        // quirk, same as the console's). KEY events are left to SDL.
        while (read(gsg_padtrackball_fd, &ev, sizeof(ev)) == sizeof(ev))
        {
            if (ev.type == EV_REL)
            {
                if (ev.code == REL_Z)
                    relX += ev.value;
                else if (ev.code == REL_RX)
                    relY += ev.value;
            }
        }
    }

    if (gsg_trackball_fd < 0)
        goto feed;

    while ((result = read(gsg_trackball_fd, &ev, sizeof(ev))) == sizeof(ev))
    {
        if (gsg_input_debug && ev.code < 32)
        {
            static uint32_t seenRel, seenKey;
            uint32_t *const seen = ev.type == EV_REL ? &seenRel : ev.type == EV_KEY ? &seenKey : NULL;
            if (seen && !(*seen & (1u << ev.code)))
            {
                *seen |= 1u << ev.code;
                LOG_F(INFO, "GSGDBG trackball new %s code=%u value=%d",
                      ev.type == EV_REL ? "REL" : "KEY", ev.code, ev.value);
            }
        }

        if (ev.type == EV_REL)
        {
            // The "XC Track Ball" reports ball motion on REL_Z/REL_RX
            // (vendor quirk); REL_X/REL_Y are forwarded for completeness.
            if (ev.code == REL_X || ev.code == REL_Z)
                relX += ev.value;
            else if (ev.code == REL_Y || ev.code == REL_RX)
                relY += ev.value;
        }
        else if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) && ev.value <= 1)
        {
            if (g_gameStationMenuActive)
            {
                // Games without a menu cursor (VoidSW): buttons are
                // select/back, matching the GameStation launcher's own UI.
                if (ev.code != BTN_MIDDLE && ev.value == 1)
                    gsg_injectMenuKey(ev.code == BTN_LEFT ? SDL_SCANCODE_RETURN : SDL_SCANCODE_ESCAPE);
                continue;
            }

            SDL_Event button = {};
            button.type = ev.value ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            button.button.type = button.type;
            button.button.timestamp = SDL_GetTicks();
            button.button.state = ev.value ? SDL_PRESSED : SDL_RELEASED;
            button.button.button = ev.code == BTN_LEFT ? SDL_BUTTON_LEFT
                                 : ev.code == BTN_RIGHT ? SDL_BUTTON_RIGHT
                                                        : SDL_BUTTON_MIDDLE;
            SDL_PushEvent(&button);
        }
    }

    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        close(gsg_trackball_fd);
        gsg_trackball_fd = -1;
        return;
    }

feed:
    if (relX || relY)
    {
        if (g_gameStationMenuActive)
        {
            // Menu-mode: translate ball motion into arrow-key taps.
            static int menuAccumX, menuAccumY;
            menuAccumX += relX;
            menuAccumY += relY;

            while (menuAccumY <= -GSG_TRACKBALL_MENU_STEP)
            {
                gsg_injectMenuKey(SDL_SCANCODE_UP);
                menuAccumY += GSG_TRACKBALL_MENU_STEP;
            }
            while (menuAccumY >= GSG_TRACKBALL_MENU_STEP)
            {
                gsg_injectMenuKey(SDL_SCANCODE_DOWN);
                menuAccumY -= GSG_TRACKBALL_MENU_STEP;
            }
            while (menuAccumX <= -GSG_TRACKBALL_MENU_STEP)
            {
                gsg_injectMenuKey(SDL_SCANCODE_LEFT);
                menuAccumX += GSG_TRACKBALL_MENU_STEP;
            }
            while (menuAccumX >= GSG_TRACKBALL_MENU_STEP)
            {
                gsg_injectMenuKey(SDL_SCANCODE_RIGHT);
                menuAccumX -= GSG_TRACKBALL_MENU_STEP;
            }
            return;
        }

        if (gsg_trackball_x < 0)
        {
            gsg_trackball_x = xres >> 1;
            gsg_trackball_y = yres >> 1;
        }

        gsg_trackball_x = clamp(gsg_trackball_x + relX, 0, xres - 1);
        gsg_trackball_y = clamp(gsg_trackball_y + relY, 0, yres - 1);

        // Games that never grab the mouse (VoidSW) drop relative motion in
        // the SDL event handler; feed the engine's accumulator directly so
        // the trackball still aims. Grabbed games get it via the event below.
        if (!g_mouseGrabbed)
        {
            g_mousePos.x += relX;
            g_mousePos.y += relY;
        }

        SDL_Event motion = {};
        motion.type = SDL_MOUSEMOTION;
        motion.motion.type = SDL_MOUSEMOTION;
        motion.motion.timestamp = SDL_GetTicks();
        motion.motion.x = gsg_trackball_x;
        motion.motion.y = gsg_trackball_y;
        motion.motion.xrel = relX;
        motion.motion.yrel = relY;
        SDL_PushEvent(&motion);
    }
}

static void gsg_drm_overlay_teardown(void);

static void gsg_drm_wait_flip(void)
{
    if (!gsg_drm_flip_pending)
        return;

    struct pollfd pfd = { gsg_drm_fd, POLLIN, 0 };
    if (poll(&pfd, 1, 100) > 0)
    {
        char evbuf[256];
        ssize_t const n = read(gsg_drm_fd, evbuf, sizeof(evbuf));
        for (ssize_t off = 0; n > 0 && off + (ssize_t)sizeof(struct gsg_drm_event) <= n;)
        {
            auto const ev = (struct gsg_drm_event const *)(evbuf + off);
            if (ev->type == GSG_DRM_EVENT_FLIP_COMPLETE)
                gsg_drm_flip_pending = false;
            if (ev->length < sizeof(struct gsg_drm_event))
                break;
            off += ev->length;
        }
    }

    if (gsg_drm_flip_pending)
    {
        static bool warned;
        if (!warned)
        {
            warned = true;
            LOG_F(WARNING, "GameStation: DRM page flip event timed out; continuing unsynchronized.");
        }
        gsg_drm_flip_pending = false;
    }
}

static void gsg_drm_destroy(void)
{
    if (!gsg_drm_active)
        return;

    gsg_drm_wait_flip();
    gsg_drm_overlay_teardown();

    // Restore the CRTC as it was before we took it over.
    if (gsg_drm_saved_crtc.mode_valid)
    {
        struct gsg_drm_crtc restore = gsg_drm_saved_crtc;
        restore.set_connectors_ptr = (uintptr_t)&gsg_drm_connector_id;
        restore.count_connectors = 1;
        ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SETCRTC, &restore);
    }

    for (auto &buf : gsg_drm_bufs)
    {
        if (buf.fb_id)
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_RMFB, &buf.fb_id);
        if (buf.map)
            munmap(buf.map, buf.size);
        if (buf.handle)
        {
            struct gsg_drm_destroy_dumb destroy = { buf.handle };
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_DESTROY_DUMB, &destroy);
        }
        buf = {};
    }

    gsg_drm_active = false;
    gsg_drm_flip_pending = false;
    gsg_drm_fd = -1;
}

static void gsg_drm_init(int32_t x, int32_t y)
{
    gsg_drm_active = false;

    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    if (!SDL_GetWindowWMInfo(sdl_window, &wminfo) || wminfo.subsystem != SDL_SYSWM_KMSDRM)
    {
        LOG_F(WARNING, "GameStation: direct DRM unavailable (not a KMSDRM window).");
        gsg_drm_disabled = true;
        return;
    }

    gsg_drm_fd = wminfo.info.kmsdrm.drm_fd;

    struct gsg_drm_card_res res = {};
    if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETRESOURCES, &res) < 0 || !res.count_connectors || !res.count_crtcs)
    {
        LOG_F(WARNING, "GameStation: DRM GETRESOURCES failed: %s", strerror(errno));
        gsg_drm_disabled = true;
        return;
    }

    uint32_t * const crtcs = (uint32_t *)Xcalloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t * const connectors = (uint32_t *)Xcalloc(res.count_connectors, sizeof(uint32_t));
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.connector_id_ptr = (uintptr_t)connectors;
    // Only request the arrays we allocated; leftover counts with null
    // pointers (fbs, encoders) make the kernel write to address 0 (EFAULT).
    res.count_fbs = 0;
    res.count_encoders = 0;
    if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETRESOURCES, &res) < 0)
    {
        LOG_F(WARNING, "GameStation: DRM GETRESOURCES fetch failed: %s", strerror(errno));
        Xfree(crtcs);
        Xfree(connectors);
        gsg_drm_disabled = true;
        return;
    }

    uint32_t foundConnector = 0, foundCrtc = 0;
    struct gsg_drm_modeinfo foundMode = {};
    bool foundExternal = false;

    for (uint32_t i = 0; i < res.count_connectors; i++)
    {
        struct gsg_drm_connector conn = {};
        conn.connector_id = connectors[i];
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETCONNECTOR, &conn) < 0)
            continue;
        if (conn.connection != GSG_DRM_MODE_CONNECTED || conn.count_modes == 0)
            continue;

        auto const modes = (struct gsg_drm_modeinfo *)Xcalloc(conn.count_modes, sizeof(struct gsg_drm_modeinfo));
        auto const encoders = (uint32_t *)Xcalloc(max(1u, conn.count_encoders), sizeof(uint32_t));
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)encoders;
        // Don't request properties: props_ptr stays null.
        conn.count_props = 0;
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETCONNECTOR, &conn) < 0)
        {
            LOG_F(WARNING, "GameStation: DRM GETCONNECTOR %u failed: %s", connectors[i], strerror(errno));
            Xfree(modes);
            Xfree(encoders);
            continue;
        }

        // Mode: the preferred one if present, else the first.
        struct gsg_drm_modeinfo connMode = modes[0];
        for (uint32_t m = 0; m < conn.count_modes; m++)
        {
            if (modes[m].type & GSG_DRM_MODE_TYPE_PREFERRED)
            {
                connMode = modes[m];
                break;
            }
        }

        // Encoder/CRTC: the currently bound one, else the first possible.
        struct gsg_drm_encoder enc = {};
        uint32_t encoderIds[8];
        uint32_t numEncoders = 0;
        if (conn.encoder_id)
            encoderIds[numEncoders++] = conn.encoder_id;
        for (uint32_t e = 0; e < conn.count_encoders && numEncoders < ARRAY_SIZE(encoderIds); e++)
            encoderIds[numEncoders++] = encoders[e];

        uint32_t connCrtc = 0;
        for (uint32_t e = 0; e < numEncoders && !connCrtc; e++)
        {
            Bmemset(&enc, 0, sizeof(enc));
            enc.encoder_id = encoderIds[e];
            if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETENCODER, &enc) < 0)
                continue;

            if (enc.crtc_id)
                connCrtc = enc.crtc_id;
            else
            {
                for (uint32_t c = 0; c < res.count_crtcs; c++)
                {
                    if (enc.possible_crtcs & (1u << c))
                    {
                        connCrtc = crtcs[c];
                        break;
                    }
                }
            }
        }

        Xfree(modes);
        Xfree(encoders);

        if (!connCrtc)
            continue;

        // Priority: exact mode match, then an external (dock) connector, then
        // the first connected one. The requested size no longer has to match
        // the mode: the present blit scales the engine buffer to the
        // connector's native mode.
        bool const exactMatch = connMode.hdisplay == (uint16_t)x && connMode.vdisplay == (uint16_t)y;
        bool const external = conn.connector_type == 10 || conn.connector_type == 11; // DP / HDMI-A
        if (!foundConnector || exactMatch || (external && !foundExternal))
        {
            foundConnector = conn.connector_id;
            foundCrtc = connCrtc;
            foundMode = connMode;
            foundExternal = external;
            if (exactMatch)
                break;
        }
    }

    Xfree(crtcs);
    Xfree(connectors);

    if (!foundConnector)
    {
        // Not permanent: a later mode change retries via gsg_presenter_ensure.
        LOG_F(INFO, "GameStation: direct DRM found no connected connector yet; using SDL presenter for now.");
        gsg_drm_failed_x = x;
        gsg_drm_failed_y = y;
        return;
    }

    if (foundMode.hdisplay != (uint16_t)x || foundMode.vdisplay != (uint16_t)y)
        LOG_F(INFO, "GameStation: direct DRM using connector's native %ux%u for requested %dx%d (scaled on present).",
              foundMode.hdisplay, foundMode.vdisplay, x, y);

    gsg_drm_crtc_id = foundCrtc;
    gsg_drm_crtc_index = 0;
    {
        struct gsg_drm_card_res res2 = {};
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETRESOURCES, &res2) == 0 && res2.count_crtcs)
        {
            auto const ids = (uint32_t *)Xcalloc(res2.count_crtcs, sizeof(uint32_t));
            res2.crtc_id_ptr = (uintptr_t)ids;
            res2.count_fbs = res2.count_connectors = res2.count_encoders = 0;
            if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETRESOURCES, &res2) == 0)
            {
                for (uint32_t c = 0; c < res2.count_crtcs; c++)
                {
                    if (ids[c] == foundCrtc)
                    {
                        gsg_drm_crtc_index = c;
                        break;
                    }
                }
            }
            Xfree(ids);
        }
    }
    gsg_drm_connector_id = foundConnector;
    gsg_drm_mode = foundMode;

    struct gsg_drm_crtc saved = {};
    saved.crtc_id = gsg_drm_crtc_id;
    if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETCRTC, &saved) == 0)
        gsg_drm_saved_crtc = saved;

    for (auto &buf : gsg_drm_bufs)
    {
        // RGB565: the engine's source is an 8-bit palette, and halving the
        // framebuffer halves our write-combined-memory traffic per frame.
        // Buffers cover the connector's native mode; the present blit scales
        // the (possibly smaller) engine frame into them.
        struct gsg_drm_create_dumb create = { gsg_drm_mode.vdisplay, gsg_drm_mode.hdisplay, 16, 0, 0, 0, 0 };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_CREATE_DUMB, &create) < 0)
            goto fail;

        buf.handle = create.handle;
        buf.pitch = create.pitch;
        buf.size = create.size;

        if (buf.pitch != (uint32_t)gsg_drm_mode.hdisplay * sizeof(uint16_t))
        {
            LOG_F(WARNING, "GameStation: dumb buffer pitch %u != expected %u.", buf.pitch, gsg_drm_mode.hdisplay * 2);
            goto fail;
        }

        struct gsg_drm_fb_cmd fb = { 0, gsg_drm_mode.hdisplay, gsg_drm_mode.vdisplay, buf.pitch, 16, 16, buf.handle };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_ADDFB, &fb) < 0)
            goto fail;
        buf.fb_id = fb.fb_id;

        struct gsg_drm_map_dumb map = { buf.handle, 0, 0 };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_MAP_DUMB, &map) < 0)
            goto fail;

        buf.map = mmap(NULL, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, gsg_drm_fd, (off_t)map.offset);
        if (buf.map == MAP_FAILED)
        {
            buf.map = NULL;
            goto fail;
        }
    }

    {
        struct gsg_drm_crtc set = {};
        set.crtc_id = gsg_drm_crtc_id;
        set.fb_id = gsg_drm_bufs[0].fb_id;
        set.set_connectors_ptr = (uintptr_t)&gsg_drm_connector_id;
        set.count_connectors = 1;
        set.mode_valid = 1;
        set.mode = gsg_drm_mode;

        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SETCRTC, &set) < 0)
        {
            LOG_F(WARNING, "GameStation: DRM SETCRTC failed: %s", strerror(errno));
            goto fail;
        }
    }

    gsg_drm_back = 1;
    gsg_drm_flip_pending = false;
    gsg_drm_active = true;
    LOG_F(INFO, "GameStation: direct DRM presenter on connector %u, crtc %u, %ux%u@%u.",
          gsg_drm_connector_id, gsg_drm_crtc_id, gsg_drm_mode.hdisplay, gsg_drm_mode.vdisplay,
          gsg_drm_mode.vrefresh);
    return;

fail:
    LOG_F(WARNING, "GameStation: direct DRM setup failed: %s", strerror(errno));
    for (auto &buf : gsg_drm_bufs)
    {
        if (buf.fb_id)
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_RMFB, &buf.fb_id);
        if (buf.map)
            munmap(buf.map, buf.size);
        if (buf.handle)
        {
            struct gsg_drm_destroy_dumb destroy = { buf.handle };
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_DESTROY_DUMB, &destroy);
        }
        buf = {};
    }
    gsg_drm_fd = -1;
    gsg_drm_disabled = true;
}

static void gsg_drm_overlay_teardown(void)
{
    if (gsg_drm_overlay_plane)
    {
        struct gsg_drm_set_plane set = {};
        set.plane_id = gsg_drm_overlay_plane;
        set.crtc_id = gsg_drm_crtc_id;
        ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SETPLANE, &set); // fb_id 0 disables it
    }

    for (auto &buf : gsg_drm_overlay_bufs)
    {
        if (buf.fb_id)
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_RMFB, &buf.fb_id);
        if (buf.map)
            munmap(buf.map, buf.size);
        if (buf.handle)
        {
            struct gsg_drm_destroy_dumb destroy = { buf.handle };
            ioctl(gsg_drm_fd, GSG_DRM_IOCTL_DESTROY_DUMB, &destroy);
        }
        buf = {};
    }

    gsg_drm_overlay_plane = 0;
    gsg_drm_overlay_active = false;
    gsg_drm_overlay_res = {};
}

// Try to move presentation to a hardware-scaled overlay plane at the engine's
// internal resolution, eliminating both the CPU upscale and 3/4 of the
// write-combined framebuffer traffic. Called lazily on the first frame, when
// softsurface is fully initialized. Falls back permanently on any failure.
static void gsg_injectEscape(bool pressed, uint32_t timestamp)
{
    // Latch the press; M_DisplayMenus opens the quit prompt if (and only if)
    // the main menu is on screen. No key injection, no gameplay risk.
    if (!pressed)
        return;

    if (gsg_input_debug)
        LOG_F(INFO, "GSGDBG Home latched");

    g_gameStationHomePressed = 1;
    UNREFERENCED_PARAMETER(timestamp);
}

static void gsg_drm_overlay_try(void)
{
    if (gsg_drm_overlay_failed)
        return;

    vec2_t const src = softsurface_getBufferResolution();
    if (src.x <= 0 || src.y <= 0)
        return; // softsurface not initialized yet; retry next frame

    // Scale to the connector's native mode, which is not necessarily the
    // engine's requested (xres, yres) when a dock display took over.
    int32_t const dispW = gsg_drm_mode.hdisplay;
    int32_t const dispH = gsg_drm_mode.vdisplay;
    if (src.x == dispW && src.y == dispH)
    {
        gsg_drm_overlay_failed = true; // no upscaling in use, nothing to gain
        return;
    }

    gsg_drm_overlay_failed = true;

    struct gsg_drm_client_cap cap = { GSG_DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
    ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SET_CLIENT_CAP, &cap); // best effort

    struct gsg_drm_plane_res pres = {};
    if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETPLANERES, &pres) < 0 || !pres.count_planes)
        return;

    auto const planeIds = (uint32_t *)Xcalloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uintptr_t)planeIds;
    if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETPLANERES, &pres) < 0)
    {
        Xfree(planeIds);
        return;
    }

    uint32_t overlayPlane = 0;
    for (uint32_t i = 0; i < pres.count_planes && !overlayPlane; i++)
    {
        struct gsg_drm_get_plane plane = {};
        plane.plane_id = planeIds[i];
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETPLANE, &plane) < 0)
            continue;
        if (!(plane.possible_crtcs & (1u << gsg_drm_crtc_index)) || plane.crtc_id)
            continue; // can't drive our crtc, or already in use (e.g. primary)

        auto const formats = (uint32_t *)Xcalloc(max(1u, plane.count_format_types), sizeof(uint32_t));
        plane.format_type_ptr = (uintptr_t)formats;
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_GETPLANE, &plane) == 0)
        {
            for (uint32_t f = 0; f < plane.count_format_types; f++)
            {
                if (formats[f] == GSG_DRM_FORMAT_RGB565)
                {
                    overlayPlane = plane.plane_id;
                    break;
                }
            }
        }
        Xfree(formats);
    }
    Xfree(planeIds);

    if (!overlayPlane)
        return;

    for (auto &buf : gsg_drm_overlay_bufs)
    {
        struct gsg_drm_create_dumb create = { (uint32_t)src.y, (uint32_t)src.x, 16, 0, 0, 0, 0 };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_CREATE_DUMB, &create) < 0)
            goto fail;

        buf.handle = create.handle;
        buf.pitch = create.pitch;
        buf.size = create.size;

        if (buf.pitch != (uint32_t)src.x * sizeof(uint16_t))
            goto fail;

        struct gsg_drm_fb_cmd fb = { 0, (uint32_t)src.x, (uint32_t)src.y, buf.pitch, 16, 16, buf.handle };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_ADDFB, &fb) < 0)
            goto fail;
        buf.fb_id = fb.fb_id;

        struct gsg_drm_map_dumb map = { buf.handle, 0, 0 };
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_MAP_DUMB, &map) < 0)
            goto fail;

        buf.map = mmap(NULL, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, gsg_drm_fd, (off_t)map.offset);
        if (buf.map == MAP_FAILED)
        {
            buf.map = NULL;
            goto fail;
        }
    }

    {
        struct gsg_drm_set_plane set = {};
        set.plane_id = overlayPlane;
        set.crtc_id = gsg_drm_crtc_id;
        set.fb_id = gsg_drm_overlay_bufs[0].fb_id;
        set.crtc_w = dispW;
        set.crtc_h = dispH;
        set.src_w = (uint32_t)src.x << 16;
        set.src_h = (uint32_t)src.y << 16;
        if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SETPLANE, &set) < 0)
            goto fail;
    }

    gsg_drm_overlay_plane = overlayPlane;
    gsg_drm_overlay_res = src;
    gsg_drm_overlay_back = 1;
    gsg_drm_overlay_active = true;

    // Re-point the software converter to 1:1: palette conversion only, no CPU
    // upscale. The VOP stretches the frame to the display mode.
    softsurface_initialize(src, src);

    LOG_F(INFO, "GameStation: hardware-scaled overlay plane %u at %dx%d -> %dx%d.",
          overlayPlane, src.x, src.y, dispW, dispH);
    return;

fail:
    LOG_F(WARNING, "GameStation: overlay plane setup failed (%s); keeping full-resolution flip.",
          strerror(errno));
    gsg_drm_overlay_teardown();
}

static void gsg_presenter_ensure(int32_t x, int32_t y)
{
    if (!sdl_window)
        return;

    if (gsg_drm_active && (x != (int32_t)gsg_drm_mode.hdisplay || y != (int32_t)gsg_drm_mode.vdisplay))
    {
        gsg_drm_destroy();
        if (gsg_presenter_surface)
        {
            if (sdl_surface == gsg_presenter_surface)
                sdl_surface = NULL;
            SDL_FreeSurface(gsg_presenter_surface);
            gsg_presenter_surface = NULL;
        }
    }

    if (gsg_drm_active)
    {
        if (!gsg_presenter_surface)
        {
            gsg_presenter_surface = SDL_CreateRGBSurfaceWithFormat(0, x, y, 32, SDL_PIXELFORMAT_ARGB8888);
            sdl_surface = gsg_presenter_surface;
        }
        return;
    }

    if (gsg_presenter_active && (gsg_presenter_w != x || gsg_presenter_h != y))
        gsg_presenter_destroy();

    if (gsg_presenter_active || gsg_presenter_disabled)
        return;

    if (!gsg_drm_disabled && (gsg_drm_failed_x != x || gsg_drm_failed_y != y))
    {
        gsg_drm_init(x, y);
        if (gsg_drm_active)
        {
            gsg_drm_failed_x = gsg_drm_failed_y = 0;

            // Reference surface matching the dumb buffers (RGB565), sized to
            // the connector's native mode (which the present blit scales to).
            gsg_presenter_surface = SDL_CreateRGBSurfaceWithFormat(0, gsg_drm_mode.hdisplay, gsg_drm_mode.vdisplay,
                                                                   16, SDL_PIXELFORMAT_RGB565);
            sdl_surface = gsg_presenter_surface;
            return;
        }
    }

    gsg_presenter_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (gsg_presenter_renderer)
        gsg_presenter_texture = SDL_CreateTexture(gsg_presenter_renderer, SDL_PIXELFORMAT_ARGB8888,
                                                  SDL_TEXTUREACCESS_STREAMING, x, y);
    if (gsg_presenter_texture)
        gsg_presenter_surface = SDL_CreateRGBSurfaceWithFormat(0, x, y, 32, SDL_PIXELFORMAT_ARGB8888);

    if (gsg_presenter_surface)
    {
        sdl_surface = gsg_presenter_surface;
        gsg_presenter_w = x;
        gsg_presenter_h = y;
        gsg_presenter_active = true;
        LOG_F(INFO, "GameStation: presenting through software renderer (KMSDRM window-surface workaround).");
        return;
    }

    LOG_F(WARNING, "GameStation: software renderer unavailable (%s); falling back to window surface.", SDL_GetError());
    gsg_presenter_destroy();
    if (!sdl_surface)
        sdl_surface = SDL_GetWindowSurface(sdl_window);
}
#endif
static SDL_GLContext sdl_context;
static int vsync_unsupported;
#endif

static int32_t vsync_renderlayer;
int32_t maxrefreshfreq=0;
int32_t xres=-1, yres=-1, bpp=0, fullscreen=0, bytesperline;
double refreshfreq = 59.0;
intptr_t frameplace=0;
int32_t lockcount=0;
char modechange=1;
char offscreenrendering=0;
char videomodereset = 0;
int32_t nofog=0;
#ifndef EDUKE32_GLES
static uint16_t sysgamma[3][256];
#endif
#ifdef USE_OPENGL
// OpenGL stuff
char nogl=0;
#endif
// last gamma, contrast
static float lastvidgcb[2];

//#define KEY_PRINT_DEBUG

#include "sdlkeytrans.cpp"

static SDL_Surface *appicon = NULL;
#if !defined __APPLE__ && !defined EDUKE32_TOUCH_DEVICES
static SDL_Surface *loadappicon(void);
#endif

static mutex_t m_initprintf;
#if SDL_MAJOR_VERSION >= 2
static ImGuiIO *g_ImGui_IO;
bool g_ImGuiCaptureInput = true;
#endif
uint8_t g_ImGuiCapturedDevices;
#ifdef _WIN32
# if SDL_MAJOR_VERSION >= 2
//
// win_gethwnd() -- gets the window handle
//
HWND win_gethwnd(void)
{
    struct SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);

    if (SDL_GetWindowWMInfo(sdl_window, &wmInfo) != SDL_TRUE)
        return 0;

    if (wmInfo.subsystem == SDL_SYSWM_WINDOWS)
        return wmInfo.info.win.window;

    LOG_F(ERROR, "Unknown WM subsystem?!");

    return 0;
}
# endif
//
// win_gethinstance() -- gets the application instance
//
HINSTANCE win_gethinstance(void)
{
    return (HINSTANCE)GetModuleHandle(NULL);
}
#endif


int32_t wm_msgbox(const char *name, const char *fmt, ...)
{
    char *buf = (char *)Balloca(MSGBOX_PRINTF_MAX);
    va_list va;

    UNREFERENCED_PARAMETER(name);

    va_start(va,fmt);
    Bvsnprintf(buf,MSGBOX_PRINTF_MAX,fmt,va);
    va_end(va);
    buf[MSGBOX_PRINTF_MAX-1] = 0;
#if defined EDUKE32_OSX
    auto result = osx_msgbox(name, buf);
    return result;
#elif defined _WIN32
    MessageBox(win_gethwnd(),buf,name,MB_OK|MB_TASKMODAL);
    return 0;
#elif defined EDUKE32_TOUCH_DEVICES
    LOG_F(INFO, "wm_msgbox called. Message: %s: %s",name,buf);
    return 0;
#elif defined GEKKO
    puts(buf);
    return 0;
#else
# if defined HAVE_GTK2
    if (gtkbuild_msgbox(name, buf) >= 0)
        return 0;
# endif
# if SDL_MAJOR_VERSION >= 2
#  if !defined _WIN32
    // Replace all tab chars with spaces because the hand-rolled SDL message
    // box diplays the former as N/L instead of whitespace.
    for (size_t i=0; i<MSGBOX_PRINTF_MAX; i++)
        if (buf[i] == '\t')
            buf[i] = ' ';
#  endif
    auto result = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, name, buf, NULL);
    return result;
# else
    puts(buf);
    puts("   (press Return or Enter to continue)");
    getchar();
    return 0;
# endif
#endif
}

int32_t wm_ynbox(const char *name, const char *fmt, ...)
{
    char *buf = (char *)Balloca(MSGBOX_PRINTF_MAX);
    va_list va;

    UNREFERENCED_PARAMETER(name);

    va_start(va,fmt);
    Bvsnprintf(buf,MSGBOX_PRINTF_MAX,fmt,va);
    va_end(va);
    buf[MSGBOX_PRINTF_MAX-1] = 0;
#if defined EDUKE32_OSX
    auto result = osx_ynbox(name, buf);
    return result;
#elif defined _WIN32
    auto result = MessageBox(win_gethwnd(),buf,name,MB_YESNO|MB_ICONQUESTION|MB_TASKMODAL);
    return result == IDYES;
#elif defined EDUKE32_TOUCH_DEVICES
    LOG_F(WARNING, "wm_ynbox called, this is bad! Message: %s: %s",name,buf);
    LOG_F(INFO, "Returning false..");
    return 0;
#elif defined GEKKO
    puts(buf);
    puts("Assuming yes...");
    return 1;
#else
# if defined HAVE_GTK2
    int ret = gtkbuild_ynbox(name, buf);
    if (ret >= 0)
        return ret;
# endif
# if SDL_MAJOR_VERSION >= 2
    int r = -1;

    const SDL_MessageBoxButtonData buttons[] = {
        {
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            0,
            "No"
        },{
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
            1,
            "Yes"
        },
    };

    SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_INFORMATION,
        NULL, /* no parent window */
        name,
        buf,
        2,
        buttons,
        NULL /* Default color scheme */
    };

    SDL_ShowMessageBox(&data, &r);
    return r;
# else
    char c;

    puts(buf);
    puts("   (type 'Y' or 'N', and press Return or Enter to continue)");
    do c = getchar(); while (c != 'Y' && c != 'y' && c != 'N' && c != 'n');
    return c == 'Y' || c == 'y';
# endif
#endif
}

void wm_setapptitle(const char *name)
{
#ifndef EDUKE32_TOUCH_DEVICES
    if (name != apptitle)
        Bstrncpyz(apptitle, name, sizeof(apptitle));

#if !defined(__APPLE__)
    if (!appicon)
        appicon = loadappicon();
#endif

#if SDL_MAJOR_VERSION >= 2
    if (sdl_window)
    {
        SDL_SetWindowTitle(sdl_window, apptitle);

        if (appicon)
        {
#if defined _WIN32
        if (!EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 5))
#endif
            SDL_SetWindowIcon(sdl_window, appicon);
        }
    }
#else
    SDL_WM_SetCaption(apptitle, NULL);

    if (appicon && sdl_surface)
        SDL_WM_SetIcon(appicon, 0);
#endif

    startwin_settitle(apptitle);
#else
    UNREFERENCED_PARAMETER(name);
#endif
}

//
//
// ---------------------------------------
//
// System
//
// ---------------------------------------
//
//

/* XXX: libexecinfo could be used on systems without gnu libc. */
#if !defined _WIN32 && defined __GNUC__ && !defined __OpenBSD__ && !(defined __APPLE__ && defined __BIG_ENDIAN__) && !defined GEKKO && !defined EDUKE32_TOUCH_DEVICES && !defined __OPENDINGUX__
# define PRINTSTACKONSEGV 1
# include <execinfo.h>
#endif

static inline char grabmouse_low(char a);

#ifndef __ANDROID__
static void attach_debugger_here(void)
{
#ifdef DEBUGGINGAIDS
    debug_break();
#endif
}

static void sighandler(int signum)
{
    UNREFERENCED_PARAMETER(signum);
    //    if (signum==SIGSEGV)
    {
        grabmouse_low(0);

#if PRINTSTACKONSEGV
        {
            void *addr[32];
            int32_t errfd = fileno(stderr);
            int32_t n=backtrace(addr, ARRAY_SIZE(addr));
            backtrace_symbols_fd(addr, n, errfd);
        }
        // This is useful for attaching the debugger post-mortem. For those pesky
        // cases where the program runs through happily when inspected from the start.
        //        usleep(15000000);
#endif
        attach_debugger_here();
        app_crashhandler();
        Bexit(EXIT_FAILURE);
    }
}
#endif

#ifdef __ANDROID__
int mobile_halted = 0;
#ifdef __cplusplus
extern "C"
{
#endif
void G_Shutdown(void);
#ifdef __cplusplus
}
#endif

int sdlayer_mobilefilter(void *userdata, SDL_Event *event)
{
    switch (event->type)
    {
        case SDL_APP_TERMINATING:
            // yes, this calls into the game, ugh
            if (mobile_halted == 1)
                G_Shutdown();

            mobile_halted = 1;
            return 0;
        case SDL_APP_LOWMEMORY:
            gltexinvalidatetype(INVALIDATE_ALL);
            return 0;
        case SDL_APP_WILLENTERBACKGROUND:
            mobile_halted = 1;
            return 0;
        case SDL_APP_DIDENTERBACKGROUND:
            gltexinvalidatetype(INVALIDATE_ALL);
            // tear down video?
            return 0;
        case SDL_APP_WILLENTERFOREGROUND:
            // restore video?
            return 0;
        case SDL_APP_DIDENTERFOREGROUND:
            mobile_halted = 0;
            return 0;
        default:
            return 1;//!halt;
    }

    UNREFERENCED_PARAMETER(userdata);
}

# include <setjmp.h>
static jmp_buf eduke32_exit_jmp_buf;
static int eduke32_return_value;

void eduke32_exit_return(int retval)
{
    eduke32_return_value = retval;
    longjmp(eduke32_exit_jmp_buf, 1);
    EDUKE32_UNREACHABLE_SECTION(return);
}
#endif

void sdlayer_sethints()
{
#if defined _WIN32 && !defined _MSC_VER
    // Thread naming interferes with debugging using MinGW-w64's GDB.
#if defined SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "1");
#endif
#if defined SDL_HINT_XINPUT_ENABLED
    if (Bgetenv("EDUKE32_NO_XINPUT"))
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
#endif
#endif

#if defined SDL_HINT_NO_SIGNAL_HANDLERS
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#endif
#if defined SDL_HINT_VIDEO_HIGHDPI_DISABLED
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
#endif
#if defined SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "0");
#endif
#if defined SDL_HINT_AUDIO_RESAMPLING_MODE
    SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "3");
#endif
#if defined SDL_HINT_MOUSE_RELATIVE_SCALING
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SCALING, "0");
#endif
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow)
#elif defined __ANDROID__
# ifdef __cplusplus
extern "C" int eduke32_android_main(int argc, char const *argv[]);
# endif
int eduke32_android_main(int argc, char const *argv[])
#elif defined GEKKO
int SDL_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
#ifdef _WIN32
    char * argvbuf;
    int    argc = windowsGetCommandLine(&argvbuf);
    char * wp   = argvbuf;
    char **argv = (char **)Bmalloc(sizeof(char *)*(argc+1));

    for (int i = 0; i < argc; i++, wp++)
    {
        argv[i] = wp;
        while (*wp) wp++;
    }
    argv[argc] = NULL;
#endif

    engineSetupAllocator();

#ifndef __ANDROID__
    signal(SIGSEGV, sighandler);
    signal(SIGILL, sighandler);  /* clang -fcatch-undefined-behavior uses an ill. insn */
    signal(SIGABRT, sighandler);
    signal(SIGFPE, sighandler);
#endif

    engineSetupLogging(argc, argv);

#if SDL_VERSION_ATLEAST(2, 0, 8)
    if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 8))
        SDL_SetMemoryFunctions(_xmalloc, _xcalloc, _xrealloc, _xfree);
#endif

    MicroProfileOnThreadCreate("Main");
    MicroProfileSetForceEnable(true);
    MicroProfileSetEnableAllGroups(true);
    MicroProfileSetForceMetaCounters(true);

#ifdef __ANDROID__
    if (setjmp(eduke32_exit_jmp_buf))
    {
        return eduke32_return_value;
    }
#endif

    sdlayer_sethints();

#ifdef USE_OPENGL
    char *argp;

    if ((argp = Bgetenv("EDUKE32_NO_OPENGL_FOG")) != NULL)
        nofog = Batol(argp);

#if defined __linux__ || defined EDUKE32_BSD
    if (!Bgetenv("EDUKE32_NO_GL_THREADED_OPTIMIZATIONS"))
    {
        if (!Bgetenv("EDUKE32_NO_NVIDIA_THREADED_OPTIMIZATIONS"))
            setenv("__GL_THREADED_OPTIMIZATIONS", "1", 0);

        if (!Bgetenv("EDUKE32_NO_MESA_THREADED_OPTIMIZATIONS"))
            setenv("mesa_glthread", "true", 0);
    }
#endif
#endif

    buildkeytranslationtable();

#ifdef __ANDROID__
    SDL_SetEventFilter(sdlayer_mobilefilter, NULL);
#endif

#ifdef _WIN32
    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (windowsPreInit())
        return -1;

#elif defined(GEKKO)
    wii_open();
#elif defined(HAVE_GTK2)
    // Pre-initialize SDL video system in order to make sure XInitThreads() is called
    // before GTK starts talking to X11.
    if (SDL_WasInit(SDL_INIT_VIDEO) != SDL_INIT_VIDEO)
        SDL_InitSubSystem(SDL_INIT_VIDEO);

    gtkbuild_init(&argc, &argv);
#endif

#ifdef EDUKE32_OSX
    osx_preopen();
#endif
    startwin_open();
#ifdef EDUKE32_OSX
    osx_postopen();
#endif
    maybe_redirect_outputs();

#ifdef USE_PHYSFS
    int pfsi = PHYSFS_init(argv[0]);
    Bassert(pfsi != 0);
    PHYSFS_setWriteDir(PHYSFS_getUserDir());
#endif
    int const r = app_main(argc, argv);

    startwin_close();

#if defined(HAVE_GTK2)
    gtkbuild_exit(r);
#endif

    return r;
}


#if SDL_MAJOR_VERSION >= 2
#ifdef USE_OPENGL
static int sdlayer_getswapinterval(int const syncMode)
{
    static int intervals[] = { -1, 0, 1, 0};
    Bassert((unsigned)(syncMode + 1) < ARRAY_SIZE(intervals));
    return intervals[syncMode + 1];
}

static int sdlayer_checkvsync(int checkSync)
{
    int const actualSync = SDL_GL_GetSwapInterval();
    if (actualSync != sdlayer_getswapinterval(checkSync))
    {
        LOG_F(WARNING, "Video driver enforcing SwapInterval %d, unable to configure VSync!", actualSync);
        checkSync = actualSync;
        vsync_unsupported = true;
    }
    return checkSync;
}
#endif

int32_t videoSetVsync(int32_t newSync)
{
    if (newSync != 0 && vsync_unsupported)
    {
        LOG_F(WARNING, "VSync configuration locked by video driver.");
        return vsync_renderlayer;
    }

    if (vsync_renderlayer == newSync)
        return newSync;

#ifdef USE_OPENGL
    if (sdl_context)
    {
        int result = SDL_GL_SetSwapInterval(sdlayer_getswapinterval(newSync));

        if (result == -1)
        {
            if (newSync == -1)
            {
                LOG_F(WARNING, "Video driver rejected SwapInterval %d, unable to configure adaptive VSync!", sdlayer_getswapinterval(newSync));
                newSync = 1;
                result  = SDL_GL_SetSwapInterval(sdlayer_getswapinterval(newSync));
            }

            if (result == -1)
            {
                LOG_F(WARNING, "Video driver rejected SwapInterval %d, unable to configure VSync!", sdlayer_getswapinterval(newSync));
                newSync = 0;
            }
        }

        vsync_renderlayer = sdlayer_checkvsync(newSync);
    }
    else
#endif
    {
        vsync_renderlayer = newSync;

        videoResetMode();

        if (videoSetGameMode(fullscreen, xres, yres, bpp, upscalefactor))
            LOG_F(ERROR, "Failed to set video mode!");
    }

    return newSync;
}
#endif

int32_t sdlayer_checkversion(void);
#if SDL_MAJOR_VERSION >= 2
int32_t sdlayer_checkversion(void)
{
    SDL_version compiled;
    auto str = (char*)Balloca(256);
    str[0] = 0;

    SDL_GetVersion(&linked);

    // string is in the format "https://github.com/libsdl-org/SDL.git@bfd2f8993f173535efe436f8e60827cc44351bea"
    char const *rev;

    if ((rev = SDL_GetRevision()) && Bstrlen(rev) > 0)
    {
        Bsnprintf(str, 256, "Initializing SDL %d.%d.%d (%s)", linked.major, linked.minor, linked.patch, rev);
    }
    else
    {
        SDL_VERSION(&compiled);

        if (!Bmemcmp(&compiled, &linked, sizeof(SDL_version)))
            Bsnprintf(str, 256, "Initializing SDL %d.%d.%d", compiled.major, compiled.minor, compiled.patch);
        else
            Bsnprintf(str, 256, "Initializing SDL %d.%d.%d (built against version %d.%d.%d)",
                linked.major, linked.minor, linked.patch, compiled.major, compiled.minor, compiled.patch);
    }

    LOG_F(INFO, "%s", str);

    if (SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) < SDL2_REQUIREDVERSION)
    {
        /*reject running under SDL versions older than what is stated in sdl_inc.h */
        LOG_F(ERROR, "You need SDL %d.%d.%d or newer to run %s.",SDL2_MIN_X,SDL2_MIN_Y,SDL2_MIN_Z,apptitle);
        return -1;
    }

    return 0;
}

//
// initsystem() -- init SDL systems
//
int32_t initsystem(void)
{
    mutex_init(&m_initprintf);

#ifdef _WIN32
    windowsPlatformInit();
#endif
    sysReadCPUID();

    if (sdlayer_checkversion())
        return -1;

    int32_t err = 0;

    if (SDL_WasInit(SDL_INIT_VIDEO) != SDL_INIT_VIDEO)
        err = SDL_InitSubSystem(SDL_INIT_VIDEO);

    if (err)
    {
        LOG_F(ERROR, "SDL initialization failed: %s.", SDL_GetError());
        LOG_F(WARNING, "Non-interactive mode enabled; this is probably not what you want.");
        VLOG_F(LOG_GFX, "Video output disabled.");

        novideo = 1;
#ifdef USE_OPENGL
        nogl = 1;
#endif
    }

#if SDL_MAJOR_VERSION >= 2
    SDL_StopTextInput();
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    timerInit(CLOCKTICKSPERSECOND);

#ifdef EDUKE32_GAMESTATION_GO
    {
        auto const fpsLog = getenv("EDUKE32_GSG_FPS_LOG");
        gsg_fps_log = fpsLog && fpsLog[0] && Bstrcmp(fpsLog, "0");
        auto const noRenderer = getenv("EDUKE32_GSG_WINDOW_SURFACE");
        gsg_presenter_disabled = noRenderer && noRenderer[0] && Bstrcmp(noRenderer, "0");
        if (gsg_presenter_disabled)
            LOG_F(INFO, "GameStation: renderer presenter disabled, using window surface.");
        auto const noDrm = getenv("EDUKE32_GSG_NO_DIRECT_DRM");
        gsg_drm_disabled = noDrm && noDrm[0] && Bstrcmp(noDrm, "0");
        if (gsg_drm_disabled)
            LOG_F(INFO, "GameStation: direct DRM presenter disabled.");
        auto const inputDebug = getenv("EDUKE32_GSG_INPUT_DEBUG");
        gsg_input_debug = inputDebug && inputDebug[0] && Bstrcmp(inputDebug, "0");
    }
#endif

    frameplace = 0;
    lockcount = 0;

    if (!novideo)
    {
#ifdef USE_OPENGL
        if (SDL_GL_LoadLibrary(0))
        {
            LOG_F(ERROR, "Failed loading OpenGL driver: %s; all OpenGL modes are unavailable.", SDL_GetError());
            nogl = 1;
        }
#endif

#ifndef _WIN32
        const char *drvname = SDL_GetCurrentVideoDriver();

        if (drvname)
            LOG_F(INFO, "Using '%s' video driver.", drvname);
#endif
        wm_setapptitle(apptitle);
        g_numdisplays = SDL_GetNumVideoDisplays();
    }

    return 0;
}
#endif


//
// uninitsystem() -- uninit SDL systems
//
void uninitsystem(void)
{
    uninitinput();
    timerUninit();

#ifdef _WIN32
    windowsPlatformCleanup();
#endif

    if (appicon)
    {
        SDL_FreeSurface(appicon);
        appicon = NULL;
    }

#ifdef USE_OPENGL
# if SDL_MAJOR_VERSION >= 2
    SDL_GL_UnloadLibrary();
# endif
#endif
    SDL_Quit();
}


//
// system_getcvars() -- propagate any cvars that are read post-initialization
//
void system_getcvars(void)
{
# ifdef _WIN32
    windowsDwmSetupComposition(false);
# endif

    vsync = videoSetVsync(vsync);
}

//
// debugprintf() -- prints a formatted debug string to stderr
//
int debugprintf(const char *f, ...)
{
#if defined DEBUGGINGAIDS && !(defined __APPLE__ && defined __BIG_ENDIAN__)
    va_list va;

    va_start(va,f);
    int len = Bvfprintf(stderr, f, va);
    va_end(va);
    return len;
#else
    UNREFERENCED_PARAMETER(f);
    return 0;
#endif
}


//
//
// ---------------------------------------
//
// All things Input
//
// ---------------------------------------
//
//

// static int32_t joyblast=0;
static SDL_Joystick *joydev = NULL;
#if SDL_MAJOR_VERSION >= 2
static SDL_GameController *controller = NULL;
// GameStation Go: every other connected controller (e.g. the paired wireless
// gamepad alongside the built-in keypad) is opened too and its SDL controller
// events merge into the same joystick state -- the event handlers never
// discriminated by source anyway.
static SDL_GameController *gsg_extra_controllers[3];
static int gsg_num_extra_controllers;
static bool gameControllerDBLoaded;

static void LoadSDLControllerDB()
{
    if (gameControllerDBLoaded)
        return;

    buildvfs_kfd fh = kopen4load("gamecontrollerdb.txt", 0);
    if (fh == buildvfs_kfd_invalid)
        return;

    int const flen = kfilelength(fh);
    if (flen <= 0)
    {
        kclose(fh);
        return;
    }

    char * dbuf = (char *)Xaligned_alloc(16, flen + 1);
    if (!dbuf)
    {
        kclose(fh);
        return;
    }

    if (kread_and_test(fh, dbuf, flen))
    {
        Xaligned_free(dbuf);
        kclose(fh);
        return;
    }

    dbuf[flen] = '\0';
    kclose(fh);

    auto rwops = SDL_RWFromConstMem(dbuf, flen);
    if (!rwops)
    {
error:
        LOG_F(ERROR, "Failed loading game controller database: %s.", SDL_GetError());
        Xaligned_free(dbuf);
        return;
    }

    int i = SDL_GameControllerAddMappingsFromRW(rwops, 1);

    if (i == -1)
        goto error;
    else
        VLOG_F(LOG_INPUT, "Loaded game controller database.");

    Xaligned_free(dbuf);

    gameControllerDBLoaded = true;
}
#endif

static int numjoysticks;

void joyScanDevices()
{
    inputdevices &= ~DEV_JOYSTICK;

#if SDL_MAJOR_VERSION >= 2
    if (controller)
    {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }
    for (int i = 0; i < gsg_num_extra_controllers; i++)
        SDL_GameControllerClose(gsg_extra_controllers[i]);
    gsg_num_extra_controllers = 0;
#endif
    if (joydev)
    {
        SDL_JoystickClose(joydev);
        joydev = nullptr;
    }

    numjoysticks = SDL_NumJoysticks();

    if (numjoysticks < 1)
        VLOG_F(LOG_INPUT, "No game controllers found.");
    else
    {
        VLOG_F(LOG_INPUT, "Game controllers:");

        char name[128];

        for (int i = 0; i < numjoysticks; i++)
        {
#if SDL_MAJOR_VERSION >= 2
            if (SDL_IsGameController(i))
                Bstrncpyz(name, SDL_GameControllerNameForIndex(i), sizeof(name));
            else
#endif
                Bstrncpyz(name, SDL_JoystickNameForIndex(i), sizeof(name));

            VLOG_F(LOG_INPUT, "  %d. %s", i + 1, name);
        }
#if SDL_MAJOR_VERSION >= 2
        for (int i = 0; i < numjoysticks; i++)
        {
            SDL_GameController *newpad;
            if ((newpad = SDL_GameControllerOpen(i)))
            {
                if (controller != nullptr)
                {
                    // Merge additional controllers into the shared state.
                    if (gsg_num_extra_controllers < (int)ARRAY_SIZE(gsg_extra_controllers))
                    {
                        gsg_extra_controllers[gsg_num_extra_controllers++] = newpad;
                        VLOG_F(LOG_INPUT, "Merging additional controller: %s.", SDL_GameControllerName(newpad));
#if SDL_VERSION_ATLEAST(2, 0, 14)
                        if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 14))
                        {
                            for (int j = 0; j < SDL_CONTROLLER_AXIS_MAX; ++j)
                                if (SDL_GameControllerHasAxis(newpad, (SDL_GameControllerAxis)j))
                                    joystick.numAxes = max(joystick.numAxes, j + 1);

                            for (int j = 0; j < SDL_CONTROLLER_BUTTON_MAX; ++j)
                                if (SDL_GameControllerHasButton(newpad, (SDL_GameControllerButton)j))
                                {
                                    joystick.numButtons = max(joystick.numButtons, j + 1);
                                    joystick.validButtons |= (1 << j);
                                }
                        }
#endif
                    }
                    else
                        SDL_GameControllerClose(newpad);
                    continue;
                }

                controller = newpad;
#if SDL_VERSION_ATLEAST(2, 0, 14)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 14))
                    Bsnprintf(name, sizeof(name), "%s [%s]", SDL_GameControllerName(controller), SDL_GameControllerGetSerial(controller));
                else
#endif
                    Bsnprintf(name, sizeof(name), "%s", SDL_GameControllerName(controller));

                VLOG_F(LOG_INPUT, "Using controller: %s.", name);

                joystick.flags      = 0;
                joystick.numBalls   = 0;
                joystick.numHats    = 0;
                joystick.numAxes    = SDL_CONTROLLER_AXIS_MAX;
                joystick.numButtons = SDL_CONTROLLER_BUTTON_MAX;

                joystick.validButtons = UINT32_MAX;
#if SDL_VERSION_ATLEAST(2, 0, 14)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 14))
                {
                    joystick.numAxes = 0;
                    for (int j = 0; j < SDL_CONTROLLER_AXIS_MAX; ++j)
                        if (SDL_GameControllerHasAxis(controller, (SDL_GameControllerAxis)j))
                            joystick.numAxes = j + 1;

                    joystick.validButtons = 0;
                    joystick.numButtons = 0;
                    for (int j = 0; j < SDL_CONTROLLER_BUTTON_MAX; ++j)
                        if (SDL_GameControllerHasButton(controller, (SDL_GameControllerButton)j))
                        {
                            joystick.numButtons = j + 1;
                            joystick.validButtons |= (1 << j);
                        }
                }
#endif
#ifdef EDUKE32_GAMESTATION_GO
                // The keypad's * and # synthesize the stick clicks, so keep them assignable.
                joystick.validButtons |= (1 << SDL_CONTROLLER_BUTTON_LEFTSTICK) | (1 << SDL_CONTROLLER_BUTTON_RIGHTSTICK);
                joystick.numButtons = max<int>(joystick.numButtons, SDL_CONTROLLER_BUTTON_RIGHTSTICK + 1);

                // SETTINGS and CREDIT are injected as Guide and Paddle 1 (see gsg_keypad_poll).
                joystick.validButtons |= (1 << SDL_CONTROLLER_BUTTON_GUIDE) | (1 << SDL_CONTROLLER_BUTTON_PADDLE1);
                joystick.numButtons = max<int>(joystick.numButtons, SDL_CONTROLLER_BUTTON_PADDLE1 + 1);

                // The triggers are axes; expose them as digital buttons so they get
                // single and double assignments like every other button.
                joystick.validButtons |= (1 << GSG_JOYBUTTON_LTRIGGER) | (1 << GSG_JOYBUTTON_RTRIGGER);
                joystick.numButtons = max<int>(joystick.numButtons, GSG_JOYBUTTON_RTRIGGER + 1);
#endif
                joystick.isGameController = 1;

                if (gameControllerDBLoaded == false)
                    LoadSDLControllerDB();

                Xfree(joystick.pAxis);
                joystick.pAxis = (int32_t *)Xcalloc(joystick.numAxes, sizeof(int32_t));
                DO_FREE_AND_NULL(joystick.pHat);

                inputdevices |= DEV_JOYSTICK;

#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 18))
                {
                    if (SDL_GameControllerHasRumble(controller))
                        joystick.hasRumble = 1;
                    else DVLOG_F(LOG_INPUT, "Couldn't init controller rumble: %s.", SDL_GetError());
                }
                else
#endif
#if SDL_VERSION_ATLEAST(2, 0, 9)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 9))
                {
                    if (!SDL_GameControllerRumble(controller, 1, 1, 1))
                        joystick.hasRumble = 1;
                    else DVLOG_F(LOG_INPUT, "Couldn't init controller rumble: %s.", SDL_GetError());
                }
#endif
                // keep scanning: open (and merge) any remaining controllers
            }
        }
        if (controller)
            return;
#endif

        for (int i = 0; i < numjoysticks; i++)
        {
            if ((joydev = SDL_JoystickOpen(i)))
            {
                VLOG_F(LOG_INPUT, "Using joystick: %s", SDL_JoystickNameForIndex(i));

                // KEEPINSYNC duke3d/src/gamedefs.h, mact/include/_control.h
                joystick.flags      = 0;
                joystick.numAxes    = min(9, SDL_JoystickNumAxes(joydev));
                joystick.numBalls   = SDL_JoystickNumBalls(joydev);
                joystick.numButtons = min(32, SDL_JoystickNumButtons(joydev));
                joystick.numHats    = min((36 - joystick.numButtons) / 4, SDL_JoystickNumHats(joydev));

                joystick.validButtons = UINT32_MAX;
                joystick.isGameController = 0;

                VLOG_F(LOG_INPUT, "Joystick %d has %d axes, %d buttons, %d hats, and %d balls.", i+1, joystick.numAxes, joystick.numButtons, joystick.numHats, joystick.numBalls);

                Xfree(joystick.pAxis);
                joystick.pAxis = (int32_t *)Xcalloc(joystick.numAxes, sizeof(int32_t));

                Xfree(joystick.pHat);
                if (joystick.numHats)
                    joystick.pHat = (int32_t *)Xcalloc(joystick.numHats, sizeof(int32_t));
                else
                    joystick.pHat = nullptr;

                for (int j = 0; j < joystick.numHats; j++)
                    joystick.pHat[j] = -1; // center

                SDL_JoystickEventState(SDL_ENABLE);
                inputdevices |= DEV_JOYSTICK;

#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 18))
                {
                    if (SDL_JoystickHasRumble(joydev))
                        joystick.hasRumble = 1;
                    else DVLOG_F(LOG_INPUT, "Couldn't init joystick rumble: %s.", SDL_GetError());
                }
                else
#endif
#if SDL_VERSION_ATLEAST(2, 0, 9)
                if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 9))
                {
                    if (!SDL_JoystickRumble(joydev, 1, 1, 1))
                        joystick.hasRumble = 1;
                    else DVLOG_F(LOG_INPUT, "Couldn't init joystick rumble: %s.", SDL_GetError());
                }
#endif
                return;
            }
        }

        VLOG_F(LOG_INPUT, "No controllers are usable.");
    }
}

//
// initinput() -- init input system
//
int32_t initinput(void(*hotplugCallback)(void) /*= nullptr*/)
{
    int32_t i;

#if SDL_MAJOR_VERSION >= 2
    if (hotplugCallback)
        g_controllerHotplugCallback = hotplugCallback;
#else
    UNREFERENCED_PARAMETER(hotplugCallback);
#endif

#if defined EDUKE32_OSX
    // force OS X to operate in >1 button mouse mode so that LMB isn't adulterated
    if (!getenv("SDL_HAS3BUTTONMOUSE"))
    {
        static char sdl_has3buttonmouse[] = "SDL_HAS3BUTTONMOUSE=1";
        putenv(sdl_has3buttonmouse);
    }
#endif

    inputdevices = DEV_KEYBOARD | DEV_MOUSE;
    g_mouseGrabbed = false;

    memset(g_keyNameTable, 0, sizeof(g_keyNameTable));

#if SDL_MAJOR_VERSION < 2
#define SDL_SCANCODE_TO_KEYCODE(x) (SDLKey)(x)
#define SDL_NUM_SCANCODES SDLK_LAST
    if (SDL_EnableKeyRepeat(250, 30))
        LOG_F(ERROR, "Unable to configure keyboard repeat.");
    SDL_EnableUNICODE(1);  // let's hope this doesn't hit us too hard
#endif

    for (i = SDL_NUM_SCANCODES - 1; i >= 0; i--)
    {
        if (!keytranslation[i])
            continue;

        Bstrncpyz(g_keyNameTable[keytranslation[i]], SDL_GetKeyName(SDL_SCANCODE_TO_KEYCODE(i)), sizeof(g_keyNameTable[0]));
    }

    if ((g_controllerSupportFlags & CONTROLLER_DISABLED) == 0)
    {
#if SDL_MAJOR_VERSION >= 2
# if defined SDL_HINT_DIRECTINPUT_ENABLED
        if (g_controllerSupportFlags & CONTROLLER_NO_DINPUT)
            SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
# endif
# if defined SDL_HINT_XINPUT_ENABLED
        if (g_controllerSupportFlags & CONTROLLER_NO_XINPUT)
            SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
# endif
        if (!SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER))
#else
        if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK))
#endif
            joyScanDevices();

#if SDL_VERSION_ATLEAST(2, 0, 9)
        if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 9))
        {
            if (joystick.flags & JOY_RUMBLE)
            {
                switch (joystick.flags & JOY_RUMBLE)
                {
                case JOY_RUMBLE:
                    VLOG_F(LOG_INPUT, "Controller supports rumble.");
                    break;
                }
            }
        }
#endif
    }

    return 0;
}

//
// uninitinput() -- uninit input system
//
void uninitinput(void)
{
#ifdef EDUKE32_GAMESTATION_GO
    if (gsg_trackball_fd >= 0)
    {
        ioctl(gsg_trackball_fd, EVIOCGRAB, 0);
        close(gsg_trackball_fd);
        gsg_trackball_fd = -1;
    }
    if (gsg_padtrackball_fd >= 0)
    {
        close(gsg_padtrackball_fd);
        gsg_padtrackball_fd = -1;
    }
    for (int i = 0; i < gsg_keypad_fd_count; ++i)
        close(gsg_keypad_fds[i]);
    gsg_keypad_fd_count = 0;
    gsg_keypad_nextscan = 0;
#endif

    mouseUninit();

#if SDL_MAJOR_VERSION >= 2
    if (controller)
    {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }
    for (int i = 0; i < gsg_num_extra_controllers; i++)
        SDL_GameControllerClose(gsg_extra_controllers[i]);
    gsg_num_extra_controllers = 0;
#endif

    if (joydev)
    {
        SDL_JoystickClose(joydev);
        joydev = nullptr;
    }
}

#ifndef GEKKO
const char *joyGetName(int32_t what, int32_t num)
{
    static char tmp[64];

    switch (what)
    {
        case 0:  // axis
            if ((unsigned)num > (unsigned)joystick.numAxes)
                return NULL;

#if SDL_MAJOR_VERSION >= 2
            if (controller)
            {
                static char const * axisStrings[] =
                {
                    "Left Stick X-Axis",
                    "Left Stick Y-Axis",
                    "Right Stick X-Axis",
                    "Right Stick Y-Axis",
                    "Left Trigger",
                    "Right Trigger",
                    NULL
                };

                return num < ARRAY_SSIZE(axisStrings) - 1 ? axisStrings[num] : SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)num);
            }
#endif

            Bsprintf(tmp, "Axis %d", num);
            return (char *)tmp;

        case 1:  // button
            if ((unsigned)num > (unsigned)joystick.numButtons)
                return NULL;

#if SDL_MAJOR_VERSION >= 2
            if (controller)
            {
                static char const * buttonStrings[] =
                {
                    "A",
                    "B",
                    "X",
                    "Y",
#ifdef EDUKE32_GAMESTATION_GO
                    "Select",
                    "Settings",
#else
                    "Back",
                    "Guide",
#endif
                    "Start",
                    "Left Stick",
                    "Right Stick",
                    "Left Shoulder",
                    "Right Shoulder",
                    "D-Pad Up",
                    "D-Pad Down",
                    "D-Pad Left",
                    "D-Pad Right",
#ifdef EDUKE32_GAMESTATION_GO
                    "Home",
                    "Credit",
#else
                    "Misc",
                    "Paddle 1",
#endif
                    "Paddle 2",
                    "Paddle 3",
                    "Paddle 4",
                    "Touchpad",
#ifdef EDUKE32_GAMESTATION_GO
                    "Left Trigger",
                    "Right Trigger",
#endif
                    NULL
                };

                return num < ARRAY_SSIZE(buttonStrings) - 1 ? buttonStrings[num] : SDL_GameControllerGetStringForButton((SDL_GameControllerButton)num);
            }
#endif

            Bsprintf(tmp, "Button %d", num);
            return (char *)tmp;

        case 2:  // hat
            if ((unsigned)num > (unsigned)joystick.numHats)
                return NULL;
            Bsprintf(tmp, "Hat %d", num);
            return (char *)tmp;

        default: return NULL;
    }
}
#endif


//
// initmouse() -- init mouse input
//
void mouseInit(void)
{
    mouseGrabInput(g_mouseEnabled = g_mouseLockedToWindow);  // FIXME - SA
}

//
// uninitmouse() -- uninit mouse input
//
void mouseUninit(void)
{
    mouseGrabInput(0);
    g_mouseEnabled = 0;
}


#if SDL_MAJOR_VERSION >= 2
//
// grabmouse_low() -- show/hide mouse cursor, lower level (doesn't check state).
//                    furthermore return 0 if successful.
//

#if defined _WIN32 && !SDL_VERSION_ATLEAST(2, 0, 13)
// bypass SDL_SetWindowGrab--see https://github.com/libsdl-org/SDL/issues/3353
static void SetWindowGrab(SDL_Window *pWindow, int const clipToWindow)
{
    UNREFERENCED_PARAMETER(pWindow);
    RECT rect { g_windowPos.x, g_windowPos.y, g_windowPos.x + xdim, g_windowPos.y + ydim };
    ClipCursor(clipToWindow ? &rect : nullptr);
}
#define SDL_SetWindowGrab SetWindowGrab
#endif
static inline char grabmouse_low(char a)
{
#if !defined EDUKE32_TOUCH_DEVICES
    /* FIXME: Maybe it's better to make sure that grabmouse_low
       is called only when a window is ready?                */
    if (sdl_window)
        SDL_SetWindowGrab(sdl_window, a ? SDL_TRUE : SDL_FALSE);
    return SDL_SetRelativeMouseMode(a ? SDL_TRUE : SDL_FALSE);
#else
    UNREFERENCED_PARAMETER(a);
    return 0;
#endif
}
#undef SDL_SetWindowGrab
#endif

//
// grabmouse() -- show/hide mouse cursor
//
void mouseGrabInput(bool grab)
{
    if (grab != g_mouseGrabbed)
        g_mousePos.x = g_mousePos.y = 0;

    if (appactive && g_mouseEnabled)
    {
#if !defined EDUKE32_TOUCH_DEVICES
        if ((grab != g_mouseGrabbed) && !grabmouse_low(grab))
#endif
            g_mouseGrabbed = grab;
    }
    else
        g_mouseGrabbed = grab;

}

void mouseLockToWindow(char a)
{
#if SDL_MAJOR_VERSION >= 2
    if (!g_ImGui_IO || !g_ImGui_IO->WantCaptureMouse)
#endif
    if (!(a & 2))
    {
        mouseGrabInput(a);
        g_mouseLockedToWindow = g_mouseGrabbed;
    }

    int newstate = (osd && ((osd->flags & OSD_CAPTURE) || (g_ImGuiCapturedDevices & DEV_MOUSE))) ? SDL_ENABLE : SDL_DISABLE;

    SDL_ShowCursor(newstate);

#if SDL_MAJOR_VERSION >= 2
    if (g_ImGui_IO)
    {
        if (newstate)
            g_ImGui_IO->ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        else
            g_ImGui_IO->ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
#endif
}



//
//
// ---------------------------------------
//
// All things Video
//
// ---------------------------------------
//
//


//
// getvalidmodes() -- figure out what video modes are available
//
static int sortmodes(const void *a_, const void *b_)
{
    auto a = (const struct validmode_t *)b_;
    auto b = (const struct validmode_t *)a_;

    int x;

    if ((x = a->fs   - b->fs)   != 0) return x;
    if ((x = a->bpp  - b->bpp)  != 0) return x;
    if ((x = a->xdim - b->xdim) != 0) return x;
    if ((x = a->ydim - b->ydim) != 0) return x;

    return 0;
}

static char modeschecked=0;

#if SDL_MAJOR_VERSION >= 2
void videoGetModes(int display)
{
    char *dummy = NULL;
    int32_t i, maxx = 0, maxy = 0;
    SDL_DisplayMode dispmode;

    if (display < 0 || display >= g_numdisplays)
        display = r_displayindex < SDL_GetNumVideoDisplays() ? r_displayindex : 0;

    if (modeschecked || novideo)
        return;
    else
    {
        auto name = Xstrdup(videoGetDisplayName(display)), shortened = Bstrtoken(name, "(", &dummy, 1);
        if (!shortened) shortened = name;
        VLOG_F(LOG_GFX, "Detecting video modes for display %d (%s)...", display, shortened);
        DO_FREE_AND_NULL(name);
    }

    validmodecnt = 0;

    // do fullscreen modes first
    for (i = 0; i < SDL_GetNumDisplayModes(display); i++)
    {
        SDL_GetDisplayMode(display, i, &dispmode);

        if (!SDL_CHECKMODE(dispmode.w, dispmode.h) ||
            (maxrefreshfreq && (dispmode.refresh_rate > maxrefreshfreq)))
            continue;

        // HACK: 8-bit == Software, 32-bit == OpenGL
        SDL_ADDMODE(dispmode.w, dispmode.h, 8, 1);
#ifdef USE_OPENGL
        if (!nogl)
            SDL_ADDMODE(dispmode.w, dispmode.h, 32, 1);
#endif
        if ((dispmode.w > maxx) || (dispmode.h > maxy))
        {
            maxx = dispmode.w;
            maxy = dispmode.h;
        }
    }

    SDL_CHECKFSMODES(maxx, maxy);

    // add windowed modes next
    // SDL sorts display modes largest to smallest, so we can just compare with mode 0
    // to make sure we aren't adding modes that are larger than the actual screen res
    SDL_GetDisplayMode(display, 0, &dispmode);

    for (i = 0; g_defaultVideoModes[i].x; i++)
    {
        auto const &mode = g_defaultVideoModes[i];

        if (mode.x > dispmode.w || mode.y > dispmode.h || !SDL_CHECKMODE(mode.x, mode.y))
            continue;

        // 8-bit == Software, 32-bit == OpenGL
        SDL_ADDMODE(mode.x, mode.y, 8, 0);

#ifdef USE_OPENGL
        if (nogl)
            continue;

        SDL_ADDMODE(mode.x, mode.y, 32, 0);
#endif
    }

    qsort((void *)validmode, validmodecnt, sizeof(struct validmode_t), &sortmodes);

    modeschecked = 1;
}
#endif

//
// checkvideomode() -- makes sure the video mode passed is legal
//
int32_t videoCheckMode(int32_t *x, int32_t *y, int32_t c, int32_t fs, int32_t forced)
{
    int32_t i, nearest=-1, dx, dy, odx=9999, ody=9999;

    videoGetModes();

    if (c>8
#ifdef USE_OPENGL
            && nogl
#endif
       ) return -1;

    // fix up the passed resolution values to be multiples of 8
    // and at least 320x200 or at most MAXXDIMxMAXYDIM
    *x = clamp(*x & ~1, 320, MAXXDIM);
    *y = clamp(*y & ~1, 200, MAXYDIM);

    for (i = 0; i < validmodecnt; i++)
    {
        if (validmode[i].bpp != c || validmode[i].fs != fs)
            continue;

        dx = klabs(validmode[i].xdim - *x);
        dy = klabs(validmode[i].ydim - *y);

        if (!(dx | dy))
        {
            // perfect match
            nearest = i;
            break;
        }

        if ((dx <= odx) && (dy <= ody))
        {
            nearest = i;
            odx = dx;
            ody = dy;
        }
    }

#ifdef ANY_WINDOWED_SIZE
    if (!forced && (fs&1) == 0 && (nearest < 0 || (validmode[nearest].xdim!=*x || validmode[nearest].ydim!=*y)))
        return 0x7fffffffl;
#endif

    if (nearest < 0)
        return -1;

    *x = validmode[nearest].xdim;
    *y = validmode[nearest].ydim;

    return nearest;
}

char const *videoGetDisplayName(int display)
{
#if SDL_MAJOR_VERSION >= 2
    return SDL_GetDisplayName(display);
#else
    UNREFERENCED_PARAMETER(display);
    return "Primary display";
#endif
}

static void destroy_window_resources()
{
/* We should NOT destroy the window surface. This is done automatically
   when SDL_DestroyWindow or SDL_SetVideoMode is called.             */

#if MICROPROFILE_ENABLED != 0
    MicroProfileGpuShutdown();
#endif

#if SDL_MAJOR_VERSION >= 2
    if (g_ImGui_IO)
    {
#ifdef USE_OPENGL
        ImGui_ImplOpenGL3_Shutdown();
#endif
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        g_ImGui_IO = NULL;
    }

    if (sdl_context)
        SDL_GL_DeleteContext(sdl_context);
    sdl_context = NULL;
#ifdef EDUKE32_GAMESTATION_GO
    gsg_drm_destroy();
    gsg_presenter_destroy();
#endif
    if (sdl_window)
        SDL_DestroyWindow(sdl_window);
    sdl_window = NULL;
#endif
}

bool g_ImGuiFrameActive;

void engineBeginImGuiFrame(void)
{
    Bassert(g_ImGuiFrameActive == false);
#if SDL_MAJOR_VERSION >= 2
#ifdef USE_OPENGL
    ImGui_ImplOpenGL3_NewFrame();
#endif
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    g_ImGuiFrameActive = true;
#endif
}

void engineEndImGuiInput(void)
{
    keyFlushChars();
    keyFlushScans();
#if SDL_MAJOR_VERSION >= 2
    ImGui::GetIO().ClearInputKeys();
//    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    SDL_StopTextInput();
#endif
}

void engineBeginImGuiInput(void)
{
    keyFlushChars();
    keyFlushScans();
#if SDL_MAJOR_VERSION >= 2
    SDL_StartTextInput();
//    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
#endif
}

void engineSetupImGui(void)
{
#if SDL_MAJOR_VERSION >= 2
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_ImGui_IO = &ImGui::GetIO();
    g_ImGui_IO->IniFilename = nullptr;
    g_ImGui_IO->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    g_ImGui_IO->ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

#ifdef USE_OPENGL
    ImGui_ImplSDL2_InitForOpenGL(sdl_window, sdl_context);
    ImGui_ImplOpenGL3_Init();
#endif

    g_ImGui_IO->Fonts->AddFontDefault();
    //ImFont* font = g_ImGui_IO->Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\consola.ttf", 12.0f);
    //IM_ASSERT(font != NULL);
#endif
}

#ifdef USE_OPENGL
void sdlayer_setvideomode_opengl(void)
{
    glsurface_destroy();

    glShadeModel(GL_SMOOTH);  // GL_FLAT
    glClearColor(0, 0, 0, 1.0);  // Black Background
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);  // Use FASTEST for ortho!
//    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glViewport(0,0,xres,yres);

#ifndef EDUKE32_GLES
    glDisable(GL_DITHER);
#endif

    fill_glinfo();
}
#endif  // defined USE_OPENGL

//
// setvideomode() -- set SDL video mode
//

int32_t setvideomode_sdlcommon(int32_t *x, int32_t *y, int32_t c, int32_t fs, int32_t *regrab)
{
    if ((r_displayindex == g_displayindex) && (fs == fullscreen) && (*x == xres) && (*y == yres) && (c == bpp) && !videomodereset)
        return 0;

    if (videoCheckMode(x, y, c, fs, 0) < 0)
        return -1;

#ifdef GEKKO
    if (!sdl_surface) // only run this the first time we set a video mode
        wii_initgamevideo();
#endif

    startwin_close();

    if (g_mouseGrabbed)
    {
        *regrab = 1;
        mouseGrabInput(0);
    }

    while (lockcount) videoEndDrawing();

#ifdef USE_OPENGL
    if (sdl_surface)
    {
        if (bpp > 8)
            polymost_glreset();
    }
    if (!nogl)
    {
        if (bpp == 8)
            glsurface_destroy();
        if ((r_displayindex == g_displayindex) && (fs == fullscreen) && (*x == xres) && (*y == yres) && (bpp != 0) && !videomodereset)
            return 0;
    }
    else
#endif
    {
       softsurface_destroy();
    }

    // clear last gamma/contrast/brightness so that it will be set anew
    lastvidgcb[0] = lastvidgcb[1] = 0.0f;

    return 1;
}

int setvideomode_sdlcommonpost(int32_t x, int32_t y, int32_t c, int32_t fs, int32_t regrab)
{
    wm_setapptitle(apptitle);

    xres = x;
    yres = y;
    bpp = c;
    fullscreen = fs;
    // bytesperline = sdl_surface->pitch;
    numpages = c > 8 ? 2 : 1;
    frameplace = 0;
    lockcount = 0;
    modechange = 1;
    videomodereset = 0;

#ifdef USE_OPENGL
    if (!nogl)
        sdlayer_setvideomode_opengl();
#endif

#ifdef EDUKE32_GAMESTATION_GO
    gsg_presenter_ensure(x, y);
    if (!sdl_surface)
        LOG_F(ERROR, "GameStation: unable to create any presentation surface: %s", SDL_GetError());
#endif

    // save the current system gamma to determine if gamma is available
#ifndef EDUKE32_GLES
    if (!gammabrightness)
    {
        if (nogl)
        {
#if SDL_MAJOR_VERSION >= 2
            if (SDL_GetWindowGammaRamp(sdl_window, sysgamma[0], sysgamma[1], sysgamma[2]) == 0)
#else
            if (SDL_GetGammaRamp(sysgamma[0], sysgamma[1], sysgamma[2]) >= 0)
#endif
                gammabrightness = 1;

            // see if gamma really is working by trying to set the brightness
            if (gammabrightness && videoSetGamma() < 0)
                gammabrightness = 0;  // nope
        }
        else gammabrightness = 1;
    }
#endif

#if SDL_MAJOR_VERSION >= 2
    int const displayindex = SDL_GetWindowDisplayIndex(sdl_window);
    int const newdisplayindex = r_displayindex < SDL_GetNumVideoDisplays() ? r_displayindex : displayindex;

    SDL_DisplayMode desktopmode;
    if (SDL_GetDesktopDisplayMode(newdisplayindex, &desktopmode))
    {
        LOG_F(ERROR, "Unable to query desktop display mode: %s.", SDL_GetError());
        return -1;
    }

    if (displayindex != newdisplayindex || g_windowPos.x + x < 0 || g_windowPos.y + y < 0 || g_windowPos.x > desktopmode.w || g_windowPos.y > desktopmode.h)
        g_windowPosValid = false;

#ifdef _WIN32
    if (timingInfo.rateRefresh.uiNumerator && timingInfo.rateRefresh.uiDenominator)
        refreshfreq = (double)timingInfo.rateRefresh.uiNumerator / timingInfo.rateRefresh.uiDenominator;
    else
#endif
        refreshfreq = desktopmode.refresh_rate;

    int const matchedResolution = (desktopmode.w == x && desktopmode.h == y);
    int const borderless = (r_borderless == 1 || (r_borderless == 2 && matchedResolution)) ? SDL_WINDOW_BORDERLESS : 0;

    if (fs)
    {
        SDL_DisplayMode dispmode = { 0, x, y, maxrefreshfreq, nullptr }, newmode;

        if (SDL_GetClosestDisplayMode(newdisplayindex, &dispmode, &newmode) == nullptr)
        {
            LOG_F(ERROR, "Unable to find a fullscreen video mode suitable for or similar to %dx%d at %dHz: %s.", x, y, maxrefreshfreq, SDL_GetError());
            return -1;
        }
        else
        {
            if (SDL_SetWindowDisplayMode(sdl_window, &newmode))
            {
                LOG_F(ERROR, "Unable to set video mode: %s.", SDL_GetError());
                return -1;
            }
            else
                refreshfreq = newmode.refresh_rate;
        }
    }

    static double lastrefreshfreq;

    if (refreshfreq != lastrefreshfreq)
    {
        VLOG_F(LOG_GFX, "Refresh rate: %.2fHz.", refreshfreq);
        lastrefreshfreq = refreshfreq;
    }

    SDL_SetWindowSize(sdl_window, x, y);

    if (fs)
    {
        if (SDL_SetWindowFullscreen(sdl_window, SDL_WINDOW_FULLSCREEN))
        {
            LOG_F(ERROR, "Unable to set window fullscreen: %s.", SDL_GetError());
            return -1;
        }
        else SDL_SetWindowPosition(sdl_window, (int)SDL_WINDOWPOS_UNDEFINED_DISPLAY(newdisplayindex), (int)SDL_WINDOWPOS_UNDEFINED_DISPLAY(newdisplayindex));
    }
    else
    {
        if (SDL_SetWindowFullscreen(sdl_window, 0))
        {
            LOG_F(ERROR, "Unable to set windowed mode: %s.", SDL_GetError());
            return -1;
        }
        else
        {
            SDL_SetWindowBordered(sdl_window, borderless ? SDL_FALSE : SDL_TRUE);
            SDL_SetWindowPosition(sdl_window, g_windowPosValid ? g_windowPos.x : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(newdisplayindex),
                                              g_windowPosValid ? g_windowPos.y : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(newdisplayindex));
        }
    }

    SDL_FlushEvent(SDL_WINDOWEVENT);
#endif

    videoFadePalette(palfadergb.r, palfadergb.g, palfadergb.b, palfadedelta);

    if (regrab)
        mouseGrabInput(g_mouseLockedToWindow);

#if SDL_MAJOR_VERSION >= 2
    g_displayindex = newdisplayindex;
#endif

    return 0;
}

#if SDL_MAJOR_VERSION >= 2
int32_t videoSetMode(int32_t x, int32_t y, int32_t c, int32_t fs)
{
    int32_t regrab = 0, ret;

    ret = setvideomode_sdlcommon(&x, &y, c, fs, &regrab);

    if (ret != 1)
    {
        if (ret == 0)
        {
            if (setvideomode_sdlcommonpost(x, y, c, fs, regrab))
                return -1;
        }
        return ret;
    }

    VLOG_F(LOG_GFX, "Setting video mode %dx%d (%d-bpp %s).", x, y, c, ((fs & 1) ? "fullscreen" : "windowed"));

    if (sdl_window)
        return setvideomode_sdlcommonpost(x, y, c, fs, regrab);

    int const display = r_displayindex < SDL_GetNumVideoDisplays() ? r_displayindex : 0;

    SDL_DisplayMode desktopmode;
    SDL_GetDesktopDisplayMode(display, &desktopmode);

    int const matchedResolution = (desktopmode.w == x && desktopmode.h == y);
    int const borderless = (r_borderless == 1 || (r_borderless == 2 && matchedResolution)) ? SDL_WINDOW_BORDERLESS : 0;

#ifdef USE_OPENGL
    if (c > 8 || !nogl)
    {
        int32_t i;

        if (nogl)
            return -1;

        struct glattribs
        {
            SDL_GLattr attr;
            int32_t value;
        } sdlayer_gl_attributes[] =
        {
              { SDL_GL_CONTEXT_FLAGS,
#ifndef NDEBUG
              SDL_GL_CONTEXT_DEBUG_FLAG |
#endif
              SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG },
              { SDL_GL_CONTEXT_RESET_NOTIFICATION, SDL_GL_CONTEXT_RESET_LOSE_CONTEXT },
              { SDL_GL_DOUBLEBUFFER, 1 },

              { SDL_GL_STENCIL_SIZE, 1 },
              { SDL_GL_ACCELERATED_VISUAL, 1 },
              { SDL_GL_DEPTH_SIZE, 24 },
          };

        SDL_GL_ATTRIBUTES(i, sdlayer_gl_attributes);

        /* HACK: changing SDL GL attribs only works before surface creation,
            so we have to create a new surface in a different format first
            to force the surface we WANT to be recreated instead of reused. */

        sdl_window = SDL_CreateWindow("", g_windowPosValid ? g_windowPos.x : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                          g_windowPosValid ? g_windowPos.y : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(display), x, y,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | borderless);

        if (sdl_window)
            sdl_context = SDL_GL_CreateContext(sdl_window);

        if (!sdl_window || !sdl_context)
        {
            LOG_F(ERROR, "Unable to set video mode: %s failed: %s.", sdl_window ? "SDL_GL_CreateContext" : "SDL_GL_CreateWindow",  SDL_GetError());
            nogl = 1;
        }

#ifndef EDUKE32_GLES
        gladLoadGLLoader(SDL_GL_GetProcAddress);
#else
        gladLoadGLES2Loader(SDL_GL_GetProcAddress);
#endif
        if (GLVersion.major < 2)
        {
            LOG_F(ERROR, "Video driver does not support OpenGL version 2 or greater; all OpenGL modes are unavailable.");
            nogl = 1;
        }

        if (nogl)
        {
            destroy_window_resources();
            // If c == 8, retry without hardware accelaration
            return videoSetMode(x, y, c, fs);
        }

        SDL_GL_SetSwapInterval(sdlayer_getswapinterval(vsync_renderlayer));
        vsync_renderlayer = sdlayer_checkvsync(vsync_renderlayer);

        engineSetupImGui();
    }
    else
#endif  // defined USE_OPENGL
    {
        // init
#ifdef EDUKE32_GAMESTATION_GO
        // A saved window position from the built-in panel would pin the window
        // to display 0 under KMSDRM; always create on the selected display.
        bool const useWindowPos = false;
#else
        bool const useWindowPos = g_windowPosValid;
#endif
        sdl_window = SDL_CreateWindow("", useWindowPos ? g_windowPos.x : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                          useWindowPos ? g_windowPos.y : (int)SDL_WINDOWPOS_CENTERED_DISPLAY(display), x, y,
                                      SDL_WINDOW_RESIZABLE | borderless);
        if (!sdl_window)
            SDL2_VIDEO_ERR("SDL_CreateWindow");

#ifndef EDUKE32_GAMESTATION_GO
        sdl_surface = SDL_GetWindowSurface(sdl_window);
        if (!sdl_surface)
            SDL2_VIDEO_ERR("SDL_GetWindowSurface");
#endif
    }

    setvideomode_sdlcommonpost(x, y, c, fs, regrab);

#if MICROPROFILE_ENABLED != 0
    if (!nogl)
        MicroProfileGpuInitGL();
#endif

#ifdef _WIN32
    windowsHandleFocusChange(1);
#endif

    return 0;
}
#endif

//
// resetvideomode() -- resets the video system
//
void videoResetMode(void)
{
    videomodereset = 1;
    modeschecked = 0;
}

//
// begindrawing() -- locks the framebuffer for drawing
//

#ifdef DEBUG_FRAME_LOCKING
uint32_t begindrawing_line[BEGINDRAWING_SIZE];
const char *begindrawing_file[BEGINDRAWING_SIZE];
void begindrawing_real(void)
#else
void videoBeginDrawing(void)
#endif
{
    if (bpp > 8)
    {
        if (offscreenrendering) return;
        frameplace = 0;
        bytesperline = 0;
        modechange = 0;
        return;
    }

    // lock the frame
    if (lockcount++ > 0)
        return;

    static intptr_t backupFrameplace = 0;

    if (inpreparemirror)
    {
        //POGO: if we are offscreenrendering and we need to render a mirror
        //      or we are rendering a mirror and we start offscreenrendering,
        //      backup our offscreen target so we can restore it later
        //      (but only allow one level deep,
        //       i.e. no viewscreen showing a camera showing a mirror that reflects the same viewscreen and recursing)
        if (offscreenrendering)
        {
            if (!backupFrameplace)
                backupFrameplace = frameplace;
            else if (frameplace != (intptr_t)mirrorBuffer &&
                     frameplace != backupFrameplace)
                return;
        }

        frameplace = (intptr_t)mirrorBuffer;

        if (offscreenrendering)
            return;
    }
    else if (offscreenrendering)
    {
        if (backupFrameplace)
        {
            frameplace = backupFrameplace;
            backupFrameplace = 0;
        }
        return;
    }
    else
#ifdef USE_OPENGL
    if (!nogl)
    {
        frameplace = (intptr_t)glsurface_getBuffer();
    }
    else
#endif
    {
        frameplace = (intptr_t)softsurface_getBuffer();
    }

    if (modechange)
    {
        bytesperline = xdim;
        calc_ylookup(bytesperline, ydim);
        modechange=0;
    }
}


//
// enddrawing() -- unlocks the framebuffer
//
void videoEndDrawing(void)
{
    if (bpp > 8)
    {
        if (!offscreenrendering) frameplace = 0;
        return;
    }

    if (!frameplace) return;
    if (lockcount > 1) { lockcount--; return; }
    if (!offscreenrendering) frameplace = 0;
    if (lockcount == 0) return;
    lockcount = 0;
}

//
// showframe() -- update the display
//
#ifdef __ANDROID__
extern "C" void AndroidDrawControls();
#endif

void videoShowFrame(int32_t w)
{
    UNREFERENCED_PARAMETER(w);

    // if we're minimized or about to resize, just throw away the frame
    if (sdl_resize.x || sdl_minimized)
        return;

#ifdef __ANDROID__
    if (mobile_halted) return;
#endif

#ifdef USE_OPENGL
    if (!nogl)
    {
        if (bpp > 8)
        {
            if (palfadedelta)
                fullscreen_tint_gl(palfadergb.r, palfadergb.g, palfadergb.b, palfadedelta);

#ifdef __ANDROID__
            AndroidDrawControls();
#endif
        }
        else
        {
            glsurface_blitBuffer();
        }
#if SDL_MAJOR_VERSION >= 2
        if (g_ImGuiFrameActive)
        {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            g_ImGuiFrameActive = false;
        }
#endif
        if ((r_glfinish == 1 && r_finishbeforeswap == 1) || vsync_renderlayer == 2)
        {
            MICROPROFILE_SCOPEI("Engine", "glFinish", MP_GREEN);
            glFinish();
        }

#ifdef _WIN32
        if (vsync_renderlayer == 2)
        {
            static uint64_t nextSwapTime = timerGetPerformanceCounter();
            uint64_t const  swapInterval = (timerGetPerformanceFrequency() / refreshfreq);
            uint64_t const  swapTime     = timerGetPerformanceCounter();

            // TODO: use timing information to determine swap time and just busy loop ourselves for more timing control
            if (swapTime < nextSwapTime)
            {
                MICROPROFILE_SCOPEI("Engine", "waitForVBlank", MP_GREEN2);
                windowsWaitForVBlank();
            }
            else if (swapTime - nextSwapTime >= swapInterval)
                nextSwapTime += swapInterval;

            nextSwapTime += swapInterval;
        }
#endif

        {
            MICROPROFILE_SCOPEI("Engine", "SDL_GL_SwapWindow", MP_GREEN3);
#if SDL_MAJOR_VERSION >= 2
            SDL_GL_SwapWindow(sdl_window);
#else
            SDL_GL_SwapBuffers();
#endif
        }

        if (r_glfinish == 1 && r_finishbeforeswap == 0 && vsync_renderlayer != 2)
        {
            MICROPROFILE_SCOPEI("Engine", "glFinish2", MP_GREEN4);
            glFinish();
        }

        MicroProfileFlip();

#if 0
        if (glinfo.reset_notification)
        {
            static const auto glGetGraphicsReset = glGetGraphicsResetStatus ? glGetGraphicsResetStatus : glGetGraphicsResetStatusKHR;
            auto status = glGetGraphicsReset();
            if (status != GL_NO_ERROR)
            {
                do
                {
                    switch (status)
                    {
                    case GL_GUILTY_CONTEXT_RESET:
                        LOG_F(ERROR, "OPENGL CONTEXT LOST: GUILTY!");
                        break;
                    case GL_INNOCENT_CONTEXT_RESET:
                        LOG_F(ERROR, "OPENGL CONTEXT LOST: INNOCENT!");
                        break;
                    case GL_UNKNOWN_CONTEXT_RESET:
                        LOG_F(ERROR, "OPENGL CONTEXT LOST!");
                        break;
                    }
                } while ((status = glGetGraphicsReset()) != GL_NO_ERROR);

                videoResetMode();

                if (videoSetGameMode(fullscreen, xres, yres, bpp, upscalefactor))
                {
                    LOG_F(ERROR, "Failed to reset video mode after lost OpenGL context; terminating.");
                    Bexit(EXIT_FAILURE);
                }
            }
        }
#endif
        // attached overlays and streaming hooks tend to change the GL state without setting it back

        if (w != -1)
        {
            if (bpp > 8)
                polymost_resetState();
            else
                glsurface_refresh();
        }

        return;
    }
#endif

    if (offscreenrendering) return;

    if (lockcount)
    {
        LOG_F(WARNING, "Frame was still locked at a depth of %d when videoShowFrame() was called!", lockcount);
        while (lockcount) videoEndDrawing();
    }

#ifdef EDUKE32_GAMESTATION_GO
    static uint32_t gsgBlitMs, gsgPresentMs;
    uint32_t const gsgT0 = gsg_fps_log ? SDL_GetTicks() : 0;
#endif

#if SDL_MAJOR_VERSION >= 2 && defined EDUKE32_GAMESTATION_GO
    if (gsg_drm_active)
    {
        if (!gsg_drm_overlay_active && !gsg_drm_overlay_failed)
            gsg_drm_overlay_try();

        if (gsg_drm_overlay_active)
        {
            vec2_t const src = softsurface_getBufferResolution();
            if (src.x != gsg_drm_overlay_res.x || src.y != gsg_drm_overlay_res.y)
            {
                // Engine resolution changed at runtime; drop the overlay and
                // let the retry logic rebuild it on a later frame.
                gsg_drm_overlay_teardown();
                gsg_drm_overlay_failed = false;
            }
            else
            {
                // Pace to vblank so the back buffer is never on screen
                // mid-write, then point the overlay at it.
                struct gsg_drm_wait_vblank vbl = { (int32_t)(gsg_drm_crtc_index << 1), 1, 0, 0 };
                uint32_t const gsgT1 = gsg_fps_log ? SDL_GetTicks() : 0;

                ioctl(gsg_drm_fd, GSG_DRM_IOCTL_WAIT_VBLANK, &vbl);
                softsurface_blitBuffer((uint32_t *)gsg_drm_overlay_bufs[gsg_drm_overlay_back].map,
                                       sdl_surface->format->BitsPerPixel);

                struct gsg_drm_set_plane set = {};
                set.plane_id = gsg_drm_overlay_plane;
                set.crtc_id = gsg_drm_crtc_id;
                set.fb_id = gsg_drm_overlay_bufs[gsg_drm_overlay_back].fb_id;
                set.crtc_w = gsg_drm_mode.hdisplay;
                set.crtc_h = gsg_drm_mode.vdisplay;
                set.src_w = (uint32_t)gsg_drm_overlay_res.x << 16;
                set.src_h = (uint32_t)gsg_drm_overlay_res.y << 16;
                if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_SETPLANE, &set) == 0)
                    gsg_drm_overlay_back ^= 1;

                if (gsg_fps_log)
                {
                    gsgPresentMs += gsgT1 - gsgT0;
                    gsgBlitMs += SDL_GetTicks() - gsgT1;
                }
            }
        }

        if (!gsg_drm_overlay_active)
        {
            // Wait for the previous flip (vsync), render straight into the back
            // dumb buffer (cacheable GEM memory), and page-flip it. No Mali copy.
            gsg_drm_wait_flip();
            uint32_t const gsgT1 = gsg_fps_log ? SDL_GetTicks() : 0;

            // The dumb buffers are sized to the connector's native mode; when
            // the engine's destination resolution doesn't match (dock display
            // took over a smaller requested mode), point the converter at the
            // native mode so the blit scales instead of wrapping on pitch.
            {
                vec2_t const src = softsurface_getBufferResolution();
                vec2_t const dst = softsurface_getDestinationBufferResolution();
                if (src.x > 0 && src.y > 0 &&
                    (dst.x != (int32_t)gsg_drm_mode.hdisplay || dst.y != (int32_t)gsg_drm_mode.vdisplay))
                {
                    softsurface_initialize(src, { (int32_t)gsg_drm_mode.hdisplay, (int32_t)gsg_drm_mode.vdisplay });
                    frameplace = (intptr_t)softsurface_getBuffer();
                }
            }

            softsurface_blitBuffer((uint32_t *)gsg_drm_bufs[gsg_drm_back].map,
                                   sdl_surface->format->BitsPerPixel);

            struct gsg_drm_page_flip flip = { gsg_drm_crtc_id, gsg_drm_bufs[gsg_drm_back].fb_id,
                                              GSG_DRM_PAGE_FLIP_EVENT, 0, 0 };
            if (ioctl(gsg_drm_fd, GSG_DRM_IOCTL_PAGE_FLIP, &flip) == 0)
            {
                gsg_drm_flip_pending = true;
                gsg_drm_back ^= 1;
            }
            else
            {
                static bool flipWarned;
                if (!flipWarned)
                {
                    flipWarned = true;
                    LOG_F(WARNING, "GameStation: DRM page flip failed: %s", strerror(errno));
                }
            }

            if (gsg_fps_log)
            {
                gsgPresentMs += gsgT1 - gsgT0;
                gsgBlitMs += SDL_GetTicks() - gsgT1;
            }
        }
    }
    else if (gsg_presenter_active)
    {
        // Blit straight into the locked streaming texture when its layout
        // allows it, avoiding one full-frame copy per frame.
        void *texturePixels = NULL;
        int texturePitch = 0;
        if (SDL_LockTexture(gsg_presenter_texture, NULL, &texturePixels, &texturePitch) == 0
            && texturePitch == xres * (int)sizeof(uint32_t))
        {
            softsurface_blitBuffer((uint32_t *)texturePixels, 32);
            SDL_UnlockTexture(gsg_presenter_texture);
        }
        else
        {
            if (texturePixels)
                SDL_UnlockTexture(gsg_presenter_texture);
            softsurface_blitBuffer((uint32_t *)sdl_surface->pixels, sdl_surface->format->BitsPerPixel);
            SDL_UpdateTexture(gsg_presenter_texture, NULL, sdl_surface->pixels, sdl_surface->pitch);
        }

        uint32_t const gsgT1 = gsg_fps_log ? SDL_GetTicks() : 0;
        SDL_RenderClear(gsg_presenter_renderer);
        SDL_RenderCopy(gsg_presenter_renderer, gsg_presenter_texture, NULL, NULL);
        SDL_RenderPresent(gsg_presenter_renderer);

        if (gsg_fps_log)
        {
            gsgBlitMs += gsgT1 - gsgT0;
            gsgPresentMs += SDL_GetTicks() - gsgT1;
        }
    }
    else
#endif
    {
        if (SDL_MUSTLOCK(sdl_surface)) SDL_LockSurface(sdl_surface);
        softsurface_blitBuffer((uint32_t*) sdl_surface->pixels, sdl_surface->format->BitsPerPixel);
        if (SDL_MUSTLOCK(sdl_surface)) SDL_UnlockSurface(sdl_surface);
#if SDL_MAJOR_VERSION >= 2
# ifdef EDUKE32_GAMESTATION_GO
        uint32_t const gsgT1 = gsg_fps_log ? SDL_GetTicks() : 0;
# endif
        if (SDL_UpdateWindowSurface(sdl_window))
        {
            // If a fullscreen X11 window is minimized then this may be required.
            // FIXME: What to do if this fails...
            sdl_surface = SDL_GetWindowSurface(sdl_window);
            SDL_UpdateWindowSurface(sdl_window);
        }
# ifdef EDUKE32_GAMESTATION_GO
        if (gsg_fps_log)
        {
            gsgBlitMs += gsgT1 - gsgT0;
            gsgPresentMs += SDL_GetTicks() - gsgT1;
        }
# endif
#else
        SDL_Flip(sdl_surface);
#endif
    }
    MicroProfileFlip();

#ifdef EDUKE32_GAMESTATION_GO
    if (gsg_fps_log)
    {
        static uint32_t fpsWindowStart;
        static int32_t fpsFrames;
        uint32_t const now = SDL_GetTicks();

        if (fpsWindowStart == 0)
            fpsWindowStart = now;

        ++fpsFrames;
        uint32_t const elapsed = now - fpsWindowStart;
        if (elapsed >= 3000)
        {
            LOG_F(INFO, "GameStation: %.1f fps (%dx%d, upscale %d, blit %.1fms, present %.1fms, %s).",
                  fpsFrames * 1000.0 / elapsed, xres, yres, (int)upscalefactor,
                  (double)gsgBlitMs / fpsFrames, (double)gsgPresentMs / fpsFrames,
                  gsg_drm_active ? "direct-drm" : gsg_presenter_active ? "renderer" : "window-surface");
            fpsWindowStart = now;
            fpsFrames = 0;
            gsgBlitMs = gsgPresentMs = 0;
        }
    }
#endif
}

//
// setpalette() -- set palette values
//
int32_t videoUpdatePalette(int32_t start, int32_t num)
{
    UNREFERENCED_PARAMETER(start);
    UNREFERENCED_PARAMETER(num);

    if (bpp > 8)
        return 0;  // no palette in opengl

#ifdef USE_OPENGL
    if (!nogl)
        glsurface_setPalette(curpalettefaded);
    else
#endif
    {
        if (sdl_surface)
            softsurface_setPalette(curpalettefaded,
                                   sdl_surface->format->Rmask,
                                   sdl_surface->format->Gmask,
                                   sdl_surface->format->Bmask);
    }

    return 0;
}

//
// setgamma
//
int32_t videoSetGamma(void)
{
    if (novideo)
        return 0;

#ifdef USE_OPENGL
    if (!nogl)
    {
        g_glColorCorrection = { g_videoGamma, g_videoContrast, g_videoSaturation, 0.f };

        if (videoGetRenderMode() == REND_POLYMOST)
            polymost_setColorCorrection(g_glColorCorrection);
        return 0;
    }
#endif

    int32_t i;
    uint16_t gammaTable[768];
    float gamma = max(MIN_GAMMA, min(MAX_GAMMA, g_videoGamma));
    float contrast = max(MIN_CONTRAST, min(MAX_CONTRAST, g_videoContrast));

    float invgamma = 1.f / gamma;
    float norm = powf(255.f, invgamma - 1.f);

    if (lastvidgcb[0] == gamma && lastvidgcb[1] == contrast)
        return 0;

    // This formula is taken from Doomsday

    for (i = 0; i < 256; i++)
    {
        float val = max(0.f, i * contrast - (contrast - 1.f) * 127.f);
        if (gamma != 1.f)
            val = powf(val, invgamma) / norm;

        gammaTable[i] = gammaTable[i + 256] = gammaTable[i + 512] = (uint16_t)max(0.f, min(65535.f, val * 256.f));
    }

#if SDL_MAJOR_VERSION >= 2
    i = INT32_MIN;

    if (sdl_window)
        i = SDL_SetWindowGammaRamp(sdl_window, &gammaTable[0], &gammaTable[256], &gammaTable[512]);
    if (i < 0)
    {
#else
    i = SDL_SetGammaRamp(&gammaTable[0], &gammaTable[256], &gammaTable[512]);
    if (/*(i != -1) && */(i < 0))
    {
#endif
        if (i != INT32_MIN)
            DLOG_F(ERROR, "Failed setting window gamma ramp: %s.", SDL_GetError());

        gammabrightness = 0;
    }
    else
    {
        lastvidgcb[0] = gamma;
        lastvidgcb[1] = contrast;

        gammabrightness = 1;
    }

    return i;
}

#if !defined __APPLE__ && !defined EDUKE32_TOUCH_DEVICES
extern "C" struct sdlappicon sdlappicon;
static inline SDL_Surface *loadappicon(void)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom((void *)sdlappicon.pixels, sdlappicon.width, sdlappicon.height, 32,
                                                 sdlappicon.width * 4, 0xffl, 0xff00l, 0xff0000l, 0xff000000l);
    return surf;
}
#endif

//
//
// ---------------------------------------
//
// Miscellany
//
// ---------------------------------------
//
//

int32_t handleevents_peekkeys(void)
{
    SDL_PumpEvents();

#if SDL_MAJOR_VERSION >= 2
    return SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
#else
    return SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_KEYDOWN));
#endif
}

void handleevents_updatemousestate(uint8_t state)
{
    g_mouseClickState = state == SDL_RELEASED ? MOUSE_RELEASED : MOUSE_PRESSED;
}


//
// handleevents() -- process the SDL message queue
//   returns !0 if there was an important event worth checking (like quitting)
//

int32_t handleevents_sdlcommon(SDL_Event *ev)
{
    switch (ev->type)
    {
#if !defined EDUKE32_IOS
        case SDL_MOUSEMOTION:
#ifndef GEKKO
            g_mouseAbs.x = ev->motion.x;
            g_mouseAbs.y = ev->motion.y;
            fallthrough__;
#endif
        case SDL_JOYBALLMOTION:
            // SDL <VER> doesn't handle relative mouse movement correctly yet as the cursor still clips to the
            // screen edges
            // so, we call SDL_WarpMouse() to center the cursor and ignore the resulting motion event that occurs
            //  <VER> is 1.3 for PK, 1.2 for tueidj
            if (appactive && g_mouseGrabbed)
            {
# if SDL_MAJOR_VERSION < 2
                if (ev->motion.x != xdim >> 1 || ev->motion.y != ydim >> 1)
# endif
                {
                    g_mousePos.x += ev->motion.xrel;
                    g_mousePos.y += ev->motion.yrel;
# if SDL_MAJOR_VERSION < 2
                    SDL_WarpMouse(xdim>>1, ydim>>1);
# endif
                }
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            int32_t j;

            // some of these get reordered to match winlayer
            switch (ev->button.button)
            {
                default: j = -1; break;
                case SDL_BUTTON_LEFT: j = 0; handleevents_updatemousestate(ev->button.state); break;
                case SDL_BUTTON_RIGHT: j = 1; break;
                case SDL_BUTTON_MIDDLE: j = 2; break;

#if SDL_MAJOR_VERSION < 2
                case SDL_BUTTON_WHEELUP:    // 4
                case SDL_BUTTON_WHEELDOWN:  // 5
                    j = ev->button.button;
                    break;
#endif
                /* Thumb buttons. */
#if SDL_MAJOR_VERSION < 2
                // NOTE: SDL1 does have SDL_BUTTON_X1, but that's not what is
                // generated. (Only tested on Linux and Windows.)
                case 8: j = 3; break;
                case 9: j = 6; break;
#else
                // On SDL2/Windows and SDL >= 2.0.?/Linux, everything is as it should be.
                // If anyone cares about old versions of SDL2 on Linux, patches welcome.
                case SDL_BUTTON_X1: j = 3; break;
                case SDL_BUTTON_X2: j = 6; break;
#endif
            }

            if (j < 0)
                break;

            if (ev->button.state == SDL_PRESSED)
                g_mouseBits |= (1 << j);
            else
#if SDL_MAJOR_VERSION < 2
                if (j != SDL_BUTTON_WHEELUP && j != SDL_BUTTON_WHEELDOWN)
#endif
                g_mouseBits &= ~(1 << j);

            if (g_mouseCallback)
                g_mouseCallback(j+1, ev->button.state == SDL_PRESSED);
            break;
        }
#else
# if SDL_MAJOR_VERSION >= 2
        case SDL_FINGERUP:
            g_mouseClickState = MOUSE_RELEASED;
            break;
        case SDL_FINGERDOWN:
            g_mouseClickState = MOUSE_PRESSED;
        case SDL_FINGERMOTION:
            g_mouseAbs.x = Blrintf(ev->tfinger.x * xdim);
            g_mouseAbs.y = Blrintf(ev->tfinger.y * ydim);
            break;
# endif
#endif
#if SDL_MAJOR_VERSION >= 2
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_controllerHotplugCallback && SDL_NumJoysticks() != numjoysticks)
                g_controllerHotplugCallback();
            break;
#endif
        case SDL_JOYAXISMOTION:
#if SDL_MAJOR_VERSION >= 2
            if (joystick.isGameController)
                break;
            fallthrough__;
        case SDL_CONTROLLERAXISMOTION:
#endif
#ifdef EDUKE32_GAMESTATION_GO
            // A phantom axis push from the volume remap is ignored; returns to
            // centre still pass so nothing sticks.
            if (ev->jaxis.value > 8000 || ev->jaxis.value < -8000)
            {
                if (gsg_volume_block_active())
                {
                    if (gsg_input_debug)
                        LOG_F(INFO, "GSGDBG dropped phantom volume-remap axis=%u value=%d", ev->jaxis.axis, ev->jaxis.value);
                    break;
                }
            }

            if (appactive && joystick.isGameController && ev->type == SDL_CONTROLLERAXISMOTION &&
                (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
            {
                int const bit = ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? GSG_JOYBUTTON_LTRIGGER : GSG_JOYBUTTON_RTRIGGER;
                if (ev->caxis.value > 16000)
                    joystick.bits |= 1 << bit;
                else if (ev->caxis.value < 8000)
                    joystick.bits &= ~(1 << bit);
            }
#endif
            if (appactive && ev->jaxis.axis < joystick.numAxes)
            {
                joystick.pAxis[ev->jaxis.axis] = ev->jaxis.value;
            }
            break;

        case SDL_JOYHATMOTION:
        {
            int32_t hatvals[16] = {
                -1,     // centre
                0,      // up 1
                9000,   // right 2
                4500,   // up+right 3
                18000,  // down 4
                -1,     // down+up!! 5
                13500,  // down+right 6
                -1,     // down+right+up!! 7
                27000,  // left 8
                27500,  // left+up 9
                -1,     // left+right!! 10
                -1,     // left+right+up!! 11
                22500,  // left+down 12
                -1,     // left+down+up!! 13
                -1,     // left+down+right!! 14
                -1,     // left+down+right+up!! 15
            };
            if (appactive && ev->jhat.hat < joystick.numHats)
                joystick.pHat[ev->jhat.hat] = hatvals[ev->jhat.value & 15];
            break;
        }

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
#if SDL_MAJOR_VERSION >= 2
# ifdef EDUKE32_GAMESTATION_GO
            // Physical Home also arrives as raw button 25 on agpio-keys
            // (raw 28 is D-pad right).
            if (ev->jbutton.button == 25)
            {
                if (gsg_input_debug)
                    LOG_F(INFO, "GSGDBG Home raw button=%u state=%u isGC=%d",
                          ev->jbutton.button, ev->jbutton.state,
                          (int)joystick.isGameController);
                // The latch is idempotent; raw and controller paths may both
                // fire for one press without harm, and some games (VoidSW)
                // never open the device as a mapped controller at all.
                gsg_injectEscape(ev->type == SDL_JOYBUTTONDOWN, ev->jbutton.timestamp);
                break;
            }
# endif
            if (joystick.isGameController)
                break;
            fallthrough__;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
# ifdef EDUKE32_GAMESTATION_GO
            if (gsg_input_debug)
                LOG_F(INFO, "GSGDBG controller button=%u state=%u",
                      ev->cbutton.button, ev->cbutton.state);

            // gamecontrollerdb.txt maps the raw Home button to misc1. It
            // latches the main-menu exit and still falls through as an
            // assignable button (Home) for gameplay.
            if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_MISC1)
            {
                gsg_injectEscape(ev->type == SDL_CONTROLLERBUTTONDOWN, ev->cbutton.timestamp);
                if (g_gameStationMenuActive == 2) // main menu: exit only, no game action
                    break;
            }

            // In game mode the driver remaps the volume keys to game inputs;
            // drop any press that arrives while a real volume key is down on
            // zadc-keys (and briefly after). Home (misc1) is never phantom.
            if (!(ev->type == SDL_CONTROLLERBUTTONDOWN && ev->cbutton.button == SDL_CONTROLLER_BUTTON_MISC1) &&
                (ev->type == SDL_CONTROLLERBUTTONDOWN || ev->type == SDL_JOYBUTTONDOWN) &&
                gsg_volume_block_active())
            {
                if (gsg_input_debug)
                    LOG_F(INFO, "GSGDBG dropped phantom volume-remap button=%u", ev->cbutton.button);
                break;
            }

            // Start opens the menu from gameplay regardless of button
            // bindings; MNU_CheckForMenus consumes the latch like Escape.
            if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_START && !g_gameStationMenuActive &&
                ev->type == SDL_CONTROLLERBUTTONDOWN)
            {
                g_gameStationShowMenu = 1;
                break;
            }

            // VoidSW's menus read keyboard only: translate the controller
            // D-pad and face buttons into menu keys while a menu is up.
            if (g_gameStationMenuActive && ev->type == SDL_CONTROLLERBUTTONDOWN)
            {
                switch (ev->cbutton.button)
                {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    gsg_injectMenuKey(SDL_SCANCODE_UP); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  gsg_injectMenuKey(SDL_SCANCODE_DOWN); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  gsg_injectMenuKey(SDL_SCANCODE_LEFT); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: gsg_injectMenuKey(SDL_SCANCODE_RIGHT); break;
                    case SDL_CONTROLLER_BUTTON_A:          gsg_injectMenuKey(SDL_SCANCODE_RETURN); break;
                    case SDL_CONTROLLER_BUTTON_B:          gsg_injectMenuKey(SDL_SCANCODE_ESCAPE); break;
                    default: break;
                }
            }
# endif
#endif
            if (appactive && ev->jbutton.button < joystick.numButtons)
            {
                if (ev->jbutton.state == SDL_PRESSED)
                    joystick.bits |= 1 << ev->jbutton.button;
                else
                    joystick.bits &= ~(1 << ev->jbutton.button);

#ifdef GEKKO
                if (ev->jbutton.button == 0) // WII_A
                    handleevents_updatemousestate(ev->jbutton.state);
#endif
            }
            break;

        case SDL_QUIT:
            quitevent = 1;
            return -1;
    }

    return 0;
}

int32_t handleevents_pollsdl(void);
#if SDL_MAJOR_VERSION >= 2

// SDL 2.0 specific event handling
int32_t handleevents_pollsdl(void)
{
    int32_t code, rv=0, j;
    SDL_Event ev;

#ifdef EDUKE32_GAMESTATION_GO
    gsg_trackball_poll();
    gsg_keypad_poll();
#endif

    g_ImGuiCapturedDevices = 0;

    if (g_ImGui_IO)
    {
        if (g_ImGuiCaptureInput && g_ImGui_IO->WantCaptureKeyboard)
            g_ImGuiCapturedDevices = DEV_KEYBOARD;

        if (g_ImGui_IO->WantCaptureMouse)
            g_ImGuiCapturedDevices |= DEV_MOUSE;
    }

    while (SDL_PollEvent(&ev))
    {
        if (g_ImGui_IO)
        {
            if (g_ImGui_IO->WantCaptureKeyboard && (ev.type == SDL_TEXTINPUT || ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP))
                if (ImGui_ImplSDL2_ProcessEvent(&ev) && ev.type == SDL_TEXTINPUT)
                    continue;

            if (g_ImGui_IO->WantCaptureMouse && (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL))
                if (ImGui_ImplSDL2_ProcessEvent(&ev))
                    continue;
        }

        switch (ev.type)
        {
            case SDL_DROPFILE:
                if (g_fileDropCallback && ev.drop.type == SDL_DROPFILE)
                    g_fileDropCallback(ev.drop.file);
                SDL_free(ev.drop.file);
                break;
            case SDL_TEXTINPUT:
                j = 0;
                do
                {
                    code = ev.text.text[j];

                    if (code != g_keyAsciiTable[OSD_OSDKey()] && !keyBufferFull())
                    {
                        if (OSD_HandleChar(code))
                            keyBufferInsert(code);
                    }
                } while (j < SDL_TEXTINPUTEVENT_TEXT_SIZE-1 && ev.text.text[++j]);
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                auto const &sc = ev.key.keysym.scancode;
                code = keytranslation[sc];

#ifdef EDUKE32_GAMESTATION_GO
                // Volume up/down (SDL view of evdev 115/114) never reach the game.
                if (sc == SDL_SCANCODE_VOLUMEUP || sc == SDL_SCANCODE_VOLUMEDOWN ||
                    ev.key.keysym.sym == SDLK_VOLUMEUP || ev.key.keysym.sym == SDLK_VOLUMEDOWN)
                    break;

                // The driver remaps volume keys to game inputs in game mode;
                // drop key presses while a real volume key is down on zadc-keys.
                // Number keys (keypad digits, which the driver never remaps)
                // are exempt.
                if (ev.key.type == SDL_KEYDOWN &&
                    !((sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) ||
                      (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0)) &&
                    gsg_volume_block_active())
                {
                    if (gsg_input_debug)
                        LOG_F(INFO, "GSGDBG dropped phantom volume-remap key scancode=%d", sc);
                    break;
                }

                if (gsg_input_debug
                    && (sc == SDL_SCANCODE_HOME || sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN
                        || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT))
                    LOG_F(INFO, "GSGDBG key scancode=%d code=0x%x state=%u",
                          sc, code, ev.key.state);
#endif

                // Modifiers that have to be held down to be effective
                // (excludes KMOD_NUM, for example).
                static const int MODIFIERS =
                    KMOD_LSHIFT|KMOD_RSHIFT|KMOD_LCTRL|KMOD_RCTRL|
                    KMOD_LALT|KMOD_RALT|KMOD_LGUI|KMOD_RGUI;

                // XXX: see osd.c, OSD_HandleChar(), there are more...
                if (ev.key.type == SDL_KEYDOWN && !keyBufferFull() &&
                    (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER ||
                     sc == SDL_SCANCODE_ESCAPE ||
                     sc == SDL_SCANCODE_BACKSPACE ||
                     sc == SDL_SCANCODE_TAB ||
                     (((ev.key.keysym.mod) & MODIFIERS) == KMOD_LCTRL &&
                      (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z))))
                {
                    char keyvalue;
                    switch (sc)
                    {
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: keyvalue = '\r'; break;
                        case SDL_SCANCODE_ESCAPE: keyvalue = 27; break;
                        case SDL_SCANCODE_BACKSPACE: keyvalue = '\b'; break;
                        case SDL_SCANCODE_TAB: keyvalue = '\t'; break;
                        default: keyvalue = sc - SDL_SCANCODE_A + 1; break;  // Ctrl+A --> 1, etc.
                    }
                    if (OSD_HandleChar(keyvalue))
                        keyBufferInsert(keyvalue);
                }
                else if (ev.key.type == SDL_KEYDOWN &&
                         ev.key.keysym.sym != g_keyAsciiTable[OSD_OSDKey()] && !keyBufferFull() &&
                         !SDL_IsTextInputActive())
                {
                    /*
                    Necessary for Duke 3D's method of entering cheats to work without showing IMEs.
                    SDL_TEXTINPUT is preferable overall, but with bitmap fonts it has no advantage.
                    */
                    SDL_Keycode keyvalue = ev.key.keysym.sym;

                    if ('a' <= keyvalue && keyvalue <= 'z')
                    {
                        if (!!(ev.key.keysym.mod & KMOD_SHIFT) ^ !!(ev.key.keysym.mod & KMOD_CAPS))
                            keyvalue -= 'a'-'A';
                    }
                    else if (ev.key.keysym.mod & KMOD_SHIFT)
                    {
                        switch (keyvalue)
                        {
                            case '\'': keyvalue = '"'; break;

                            case ',': keyvalue = '<'; break;
                            case '-': keyvalue = '_'; break;
                            case '.': keyvalue = '>'; break;
                            case '/': keyvalue = '?'; break;
                            case '0': keyvalue = ')'; break;
                            case '1': keyvalue = '!'; break;
                            case '2': keyvalue = '@'; break;
                            case '3': keyvalue = '#'; break;
                            case '4': keyvalue = '$'; break;
                            case '5': keyvalue = '%'; break;
                            case '6': keyvalue = '^'; break;
                            case '7': keyvalue = '&'; break;
                            case '8': keyvalue = '*'; break;
                            case '9': keyvalue = '('; break;

                            case ';': keyvalue = ':'; break;

                            case '=': keyvalue = '+'; break;

                            case '[': keyvalue = '{'; break;
                            case '\\': keyvalue = '|'; break;
                            case ']': keyvalue = '}'; break;

                            case '`': keyvalue = '~'; break;
                        }
                    }
                    else if (ev.key.keysym.mod & KMOD_NUM) // && !(ev.key.keysym.mod & KMOD_SHIFT)
                    {
                        switch (keyvalue)
                        {
                            case SDLK_KP_1: keyvalue = '1'; break;
                            case SDLK_KP_2: keyvalue = '2'; break;
                            case SDLK_KP_3: keyvalue = '3'; break;
                            case SDLK_KP_4: keyvalue = '4'; break;
                            case SDLK_KP_5: keyvalue = '5'; break;
                            case SDLK_KP_6: keyvalue = '6'; break;
                            case SDLK_KP_7: keyvalue = '7'; break;
                            case SDLK_KP_8: keyvalue = '8'; break;
                            case SDLK_KP_9: keyvalue = '9'; break;
                            case SDLK_KP_0: keyvalue = '0'; break;
                            case SDLK_KP_PERIOD: keyvalue = '.'; break;
                            case SDLK_KP_COMMA: keyvalue = ','; break;
                        }
                    }

                    switch (keyvalue)
                    {
                        case SDLK_KP_DIVIDE: keyvalue = '/'; break;
                        case SDLK_KP_MULTIPLY: keyvalue = '*'; break;
                        case SDLK_KP_MINUS: keyvalue = '-'; break;
                        case SDLK_KP_PLUS: keyvalue = '+'; break;
                    }

                    if ((unsigned)keyvalue <= 0x7Fu)
                    {
                        if (OSD_HandleChar(keyvalue))
                            keyBufferInsert(keyvalue);
                    }
                }

                // initprintf("SDL2: got key %d, %d, %u\n", ev.key.keysym.scancode, code, ev.key.type);

                // hook in the osd
                if ((j = OSD_HandleScanCode(code, (ev.key.type == SDL_KEYDOWN))) <= 0)
                {
                    if (j == -1)  // osdkey
                    {
                        for (j = 0; j < NUMKEYS; ++j)
                        {
                            if (keyGetState(j))
                            {
                                if (keypresscallback)
                                    keypresscallback(j, 0);
                            }
                            keySetState(j, 0);
                        }
                    }
                    break;
                }

                if (ev.key.type == SDL_KEYDOWN)
                {
                    if (!keyGetState(code))
                    {
                        if (keypresscallback)
                            keypresscallback(code, 1);
                    }
                    keySetState(code, 1);
                }
                else
                {
# if 1
                    // The pause key generates a release event right after
                    // the pressing one. As a result, it gets unseen
                    // by the game most of the time.
                    if (code == 0x59)  // pause
                        break;
# endif
                    keySetState(code, 0);
                    if (keypresscallback)
                        keypresscallback(code, 0);
                }
                break;
            }

            case SDL_MOUSEWHEEL:
                // initprintf("wheel y %d\n",ev.wheel.y);
                if (ev.wheel.y > 0)
                {
                    g_mouseBits |= 16;
                    if (g_mouseCallback)
                        g_mouseCallback(5, 1);
                }
                if (ev.wheel.y < 0)
                {
                    g_mouseBits |= 32;
                    if (g_mouseCallback)
                        g_mouseCallback(6, 1);
                }
                break;

            case SDL_WINDOWEVENT:
                switch (ev.window.event)
                {
                    case SDL_WINDOWEVENT_MINIMIZED:
                        sdl_minimized = true;
                        break;
                    case SDL_WINDOWEVENT_RESTORED:
                    case SDL_WINDOWEVENT_MAXIMIZED:
                        sdl_minimized = false;
                        break;
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        appactive = (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
                        if (g_mouseGrabbed && g_mouseEnabled)
                            grabmouse_low(appactive);
#ifdef _WIN32
                        windowsHandleFocusChange(appactive);
#endif
                        break;

                    case SDL_WINDOWEVENT_MOVED:
                    {
                        if (fullscreen) break;
                        g_windowPos = { ev.window.data1, ev.window.data2 };
                        g_windowPosValid = true;

                        int displayindex = SDL_GetWindowDisplayIndex(sdl_window);

                        if (r_displayindex != displayindex || !modeschecked)
                        {
                            r_displayindex = displayindex;
                            modeschecked = 0;
                            videoGetModes();
                        }
                        break;
                    }
                    case SDL_WINDOWEVENT_RESIZED:
                        if (fullscreen) break;
                        sdl_resize = { ev.window.data1 & ~1, ev.window.data2 & ~1 };
                        break;
                    case SDL_WINDOWEVENT_ENTER:
                    case SDL_WINDOWEVENT_LEAVE:
                        g_mouseInsideWindow = (ev.window.event == SDL_WINDOWEVENT_ENTER);
                        break;
                }

                break;

            default:
                rv = handleevents_sdlcommon(&ev);
                break;
        }
    }

    return rv;
}
#endif

int32_t handleevents(void)
{
#ifdef __ANDROID__
    if (mobile_halted) return 0;
#endif

    int32_t rv;

#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (g_ImGuiFrameActive)
    {
        ImGui::EndFrame();
        g_ImGuiFrameActive = false;
    }

    if (EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 9))
    {
        if (joystick.hasRumble)
        {
            auto dorumble = [](uint16_t const low, uint16_t const high, uint32_t const time)
            {
                if (joystick.isGameController)
                    SDL_GameControllerRumble(controller, low, high, time);
                else
                    SDL_JoystickRumble(joydev, low, high, time);
            };

            static uint32_t rumbleZeroTime;

            if (joystick.rumbleLow || joystick.rumbleHigh)
            {
                rumbleZeroTime = timerGetTicks() + joystick.rumbleTime;

                dorumble(joystick.rumbleLow, joystick.rumbleHigh, joystick.rumbleTime);
                joystick.rumbleTime = joystick.rumbleLow = joystick.rumbleHigh = 0;
            }
            else if (rumbleZeroTime && timerGetTicks() >= rumbleZeroTime)
            {
                rumbleZeroTime = 0;
                dorumble(0, 0, 0);
            }
        }
    }
#endif

#if SDL_MAJOR_VERSION >= 2
    if (g_mouseBits & 2 && osd->flags & OSD_CAPTURE && SDL_HasClipboardText())
    {
        auto text = SDL_GetClipboardText();
        OSD_HandleClipboard(text);
        SDL_free(text);
    }
#endif

    if (inputchecked && g_mouseEnabled)
    {
        if (g_mouseCallback)
        {
            if (g_mouseBits & 16)
                g_mouseCallback(5, 0);
            if (g_mouseBits & 32)
                g_mouseCallback(6, 0);
        }

        OSD_HandleWheel();
        g_mouseBits &= ~(16 | 32);
    }

    rv = handleevents_pollsdl();

    inputchecked = 0;
    timerUpdateClock();

    communityapiRunCallbacks();

    if (!frameplace && sdl_resize.x)
    {
        if (in3dmode())
            videoSetGameMode(fullscreen, sdl_resize.x, sdl_resize.y, bpp, upscalefactor);
        else
            videoSet2dMode(fullscreen, sdl_resize.x, sdl_resize.y, upscalefactor);

        sdl_resize = {};
    }

#ifndef _WIN32
    startwin_idle(NULL);
#endif

    return rv;
}

#if SDL_MAJOR_VERSION < 2
# include "sdlayer12.cpp"
#endif
