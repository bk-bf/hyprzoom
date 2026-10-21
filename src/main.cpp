#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include "Config.hpp"
#include "Overview.hpp"
#include "ZoomGesture.hpp"

#include <lua.hpp>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#undef private
#undef protected

typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);

inline CFunctionHook* g_pRenderWorkspaceHook = nullptr;

struct SRegisteredGesture {
    size_t                    fingers;
    eTrackpadGestureDirection direction;
    float                     scale;
};

static std::vector<SRegisteredGesture> g_gestures;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void hkRenderWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const CBox& geometry) {
    if (g_pOverview && g_pOverview->m_monitor == monitor) {
        g_pOverview->render(now);
        return;
    }

    ((origRenderWorkspace)g_pRenderWorkspaceHook->m_original)(thisptr, monitor, workspace, now, geometry);
}

static void openOverview() {
    if (g_pOverview)
        return;

    const auto MONITOR = Desktop::focusState()->monitor();
    if (MONITOR && MONITOR->m_activeWorkspace)
        g_pOverview = makeUnique<COverview>(MONITOR, false);
}

static void closeOverview() {
    if (g_pOverview && !g_pOverview->closing())
        g_pOverview->closeToSelection();
}

static void toggleOverview() {
    if (g_pOverview)
        closeOverview();
    else
        openOverview();
}

static SDispatchResult dispatch(std::string arg) {
    if (arg == "open")
        openOverview();
    else if (arg == "close")
        closeOverview();
    else
        toggleOverview();
    return {};
}

static int luaToggle(lua_State* L) {
    toggleOverview();
    return 0;
}

static int luaOpen(lua_State* L) {
    openOverview();
    return 0;
}

static int luaClose(lua_State* L) {
    closeOverview();
    return 0;
}

static int luaGesture(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "fingers");
    const auto FINGERS = (size_t)luaL_checkinteger(L, -1);
    lua_getfield(L, 1, "direction");
    const auto DIRECTION = g_pTrackpadGestures->dirForString(luaL_checkstring(L, -1));
    lua_getfield(L, 1, "scale");
    const auto SCALE = (float)luaL_optnumber(L, -1, 1.0);
    lua_pop(L, 3);

    if (DIRECTION == TRACKPAD_GESTURE_DIR_NONE)
        return luaL_error(L, "hyprzoom.gesture: invalid direction");

    const auto RESULT = g_pTrackpadGestures->addGesture(makeUnique<CZoomGesture>(), FINGERS, DIRECTION, 0, SCALE, false);
    if (!RESULT) {
        static std::string error;
        error = "hyprzoom.gesture: " + RESULT.error();
        return luaL_error(L, "%s", error.c_str());
    }

    g_gestures.emplace_back(SRegisteredGesture{FINGERS, DIRECTION, SCALE});
    return 0;
}

static void fail(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE, "[hyprzoom] " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error("[hyprzoom] " + reason);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        fail("built against different Hyprland headers than the running Hyprland");

    for (const auto& fn : HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace")) {
        if (fn.demangled.contains("IHyprRenderer::renderWorkspace("))
            g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, (void*)hkRenderWorkspace);
    }

    if (!g_pRenderWorkspaceHook || !g_pRenderWorkspaceHook->hook())
        fail("could not hook IHyprRenderer::renderWorkspace");

    static auto PRERENDER = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (g_pOverview)
            g_pOverview->onPreRender(monitor);
    });

    static auto PRERELOAD = Event::bus()->m_events.config.preReload.listen([] { g_gestures.clear(); });

    g_config.zoom =
        makeShared<Config::Values::CFloatValue>("plugin:hyprzoom:zoom", "scale of a workspace when zoomed out", 0.4F, Config::Values::SFloatValueOptions{.min = 0.05F, .max = 1.F});
    g_config.gap             = makeShared<Config::Values::CIntValue>("plugin:hyprzoom:gap", "gap between workspaces when zoomed out, in pixels", 40);
    g_config.backdrop        = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:backdrop_color", "colour laid over the wallpaper behind the workspaces", 0x80000000);
    g_config.backdropBlur    = makeShared<Config::Values::CIntValue>("plugin:hyprzoom:backdrop_blur", "blur the wallpaper behind the workspaces", 1);
    g_config.lane            = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:lane_color", "band behind each workspace's row", 0x99282136);
    g_config.frame           = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:frame_color", "outline of the part of each workspace the monitor shows", 0x2EFFFFFF);
    g_config.accent          = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:accent_color", "selected workspace's label and the drop target", 0xFFCB6FA1);
    g_config.label           = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:label_color", "workspace label background", 0xFF1A1623);
    g_config.labelText       = makeShared<Config::Values::CColorValue>("plugin:hyprzoom:label_text_color", "workspace label text", 0xFFABB2BF);
    g_config.gestureDistance = makeShared<Config::Values::CIntValue>("plugin:hyprzoom:gesture_distance", "gesture travel that zooms all the way out", 300);

    for (const auto& value : std::initializer_list<SP<Config::Values::IValue>>{g_config.zoom, g_config.gap, g_config.backdrop, g_config.backdropBlur, g_config.lane, g_config.frame,
                                                                               g_config.accent, g_config.label, g_config.labelText, g_config.gestureDistance}) {
        HyprlandAPI::addConfigValueV2(PHANDLE, value);
    }

    Config::animationTree()->m_animationTree.createNode("hyprzoom", "workspaces");

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprzoom:overview", ::dispatch);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprzoom", "toggle", ::luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprzoom", "open", ::luaOpen);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprzoom", "close", ::luaClose);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprzoom", "gesture", ::luaGesture);

    return {"hyprzoom", "Zoomed-out workspace overview", "bk-bf", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pOverview.reset();

    for (const auto& g : g_gestures) {
        g_pTrackpadGestures->removeGesture(g.fingers, g.direction, 0, g.scale, false);
    }
    g_gestures.clear();
}
