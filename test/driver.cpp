#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <lua.hpp>
#include <vector>

inline HANDLE  PHANDLE = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

class CDriverKeyboard : public IKeyboard {
  public:
    static SP<CDriverKeyboard> create() {
        auto kb          = SP<CDriverKeyboard>(new CDriverKeyboard());
        kb->m_self       = kb;
        kb->m_hlName     = "hyprzoom-driver-keyboard";
        kb->m_deviceName = "hyprzoom-driver-keyboard";
        return kb;
    }

    bool isVirtual() override {
        return false;
    }

    SP<Aquamarine::IKeyboard> aq() override {
        return nullptr;
    }

    void send(uint32_t key, bool pressed) {
        IKeyboard::SKeyEvent e{
            .timeMs  = (uint32_t)Time::millis(Time::steadyNow()),
            .keycode = key,
            .state   = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
        };
        updatePressed(e.keycode, pressed);
        m_keyboardEvents.key.emit(e);
    }

    void destroy() {
        m_events.destroy.emit();
    }
};

class CDriverMouse : public IPointer {
  public:
    static SP<CDriverMouse> create() {
        auto m          = SP<CDriverMouse>(new CDriverMouse());
        m->m_self       = m;
        m->m_hlName     = "hyprzoom-driver-mouse";
        m->m_deviceName = "hyprzoom-driver-mouse";
        return m;
    }

    bool isVirtual() override {
        return false;
    }

    SP<Aquamarine::IPointer> aq() override {
        return nullptr;
    }

    void destroy() {
        m_events.destroy.emit();
    }
};

static SP<CDriverKeyboard>                   g_keyboard;
static SP<CDriverMouse>                      g_mouse;
static std::vector<double>                   g_frames;
static std::chrono::steady_clock::time_point g_lastFrame;
static bool                                  g_recording = false;

static uint32_t                              now() {
    return (uint32_t)Time::millis(Time::steadyNow());
}

static int luaKey(lua_State* L) {
    const auto KEY = (uint32_t)luaL_checkinteger(L, 1);
    if (lua_gettop(L) > 1) {
        g_keyboard->send(KEY, lua_toboolean(L, 2));
        return 0;
    }
    g_keyboard->send(KEY, true);
    g_keyboard->send(KEY, false);
    return 0;
}

static int luaMove(lua_State* L) {
    const Vector2D TARGET = {luaL_checknumber(L, 1), luaL_checknumber(L, 2)};
    const auto     DELTA  = TARGET - g_pInputManager->getMouseCoordsInternal();
    g_mouse->m_pointerEvents.motion.emit(IPointer::SMotionEvent{.timeMs = now(), .delta = DELTA, .unaccel = DELTA, .mouse = true, .device = g_mouse});
    g_mouse->m_pointerEvents.frame.emit();
    return 0;
}

static int luaButton(lua_State* L) {
    const auto BUTTON  = (uint32_t)luaL_checkinteger(L, 1);
    const bool PRESSED = lua_toboolean(L, 2);
    g_mouse->m_pointerEvents.button.emit(IPointer::SButtonEvent{
        .timeMs = now(),
        .button = BUTTON,
        .state  = PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
        .mouse  = true,
    });
    g_mouse->m_pointerEvents.frame.emit();
    return 0;
}

static int luaScroll(lua_State* L) {
    const bool   HORIZONTAL = lua_toboolean(L, 1);
    const double DELTA      = luaL_checknumber(L, 2);
    g_mouse->m_pointerEvents.axis.emit(IPointer::SAxisEvent{
        .timeMs = now(),
        .source = WL_POINTER_AXIS_SOURCE_FINGER,
        .axis   = HORIZONTAL ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
        .delta  = DELTA,
        .mouse  = true,
    });
    g_mouse->m_pointerEvents.frame.emit();
    return 0;
}

static int luaSwipe(lua_State* L) {
    const auto FINGERS = (uint32_t)luaL_checkinteger(L, 1);
    const auto DX      = luaL_checknumber(L, 2);
    const auto DY      = luaL_checknumber(L, 3);
    const int  STEPS   = 10;
    g_mouse->m_pointerEvents.swipeBegin.emit(IPointer::SSwipeBeginEvent{.timeMs = now(), .fingers = FINGERS});
    for (int i = 0; i < STEPS; ++i) {
        g_mouse->m_pointerEvents.swipeUpdate.emit(IPointer::SSwipeUpdateEvent{.timeMs = now(), .fingers = FINGERS, .delta = Vector2D{DX, DY} / STEPS});
    }
    g_mouse->m_pointerEvents.swipeEnd.emit(IPointer::SSwipeEndEvent{.timeMs = now()});
    return 0;
}

static int luaRecord(lua_State* L) {
    g_frames.clear();
    g_recording = true;
    g_lastFrame = std::chrono::steady_clock::now();
    return 0;
}

static int luaFrames(lua_State* L) {
    g_recording = false;
    std::ofstream out(luaL_checkstring(L, 1));
    for (const auto f : g_frames) {
        out << f << "\n";
    }
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    g_keyboard = CDriverKeyboard::create();
    g_pInputManager->newKeyboard(g_keyboard);
    g_mouse = CDriverMouse::create();
    g_pInputManager->newMouse(g_mouse);

    static auto PRERENDER = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) {
        if (!g_recording)
            return;
        const auto NOW = std::chrono::steady_clock::now();
        g_frames.emplace_back(std::chrono::duration<double, std::milli>(NOW - g_lastFrame).count());
        g_lastFrame = NOW;
    });

    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "key", ::luaKey);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "move", ::luaMove);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "button", ::luaButton);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "scroll", ::luaScroll);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "swipe", ::luaSwipe);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "record", ::luaRecord);
    HyprlandAPI::addLuaFunction(PHANDLE, "driver", "frames", ::luaFrames);

    return {"hyprzoom-driver", "Input and frame timing for hyprzoom's tests", "bk-bf", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_keyboard->destroy();
    g_keyboard.reset();
    g_mouse->destroy();
    g_mouse.reset();
}
