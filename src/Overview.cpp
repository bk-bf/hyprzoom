#include "Overview.hpp"
#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#undef private
#undef protected

using namespace Render;

constexpr double DRAG_THRESHOLD = 8.0;
constexpr double AXIS_STEP      = 30.0;

static void      damageOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable>) {
    if (g_pOverview && g_pOverview->m_monitor)
        g_pHyprRenderer->damageMonitor(g_pOverview->m_monitor.lock());
}

class CNoSimplifyPassElement : public IPassElement {
  public:
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    const char* passName() override {
        return "CNoSimplifyPassElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }
    bool disableSimplification() override {
        return true;
    }
};

static void pushModif(float scale, const Vector2D& translatePx) {
    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, scale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, translatePx);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{modif}));
}

static void popModif() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{SRenderModifData{}}));
}

static void addRect(const CBox& px, const CHyprColor& color) {
    if (color.a <= 0.F || px.w <= 0 || px.h <= 0)
        return;

    CRectPassElement::SRectData data;
    data.box   = px;
    data.color = color;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

static CHyprColor withAlpha(Config::INTEGER argb, float alpha) {
    CHyprColor c{(uint64_t)argb};
    c.a *= alpha;
    return c;
}

static void renderLayers(PHLMONITOR monitor, std::initializer_list<int> layers, const Time::steady_tp& now, bool popups = false) {
    for (const auto layer : layers) {
        for (const auto& ls : monitor->m_layerSurfaceLayers[layer]) {
            if (ls)
                g_pHyprRenderer->renderLayer(ls.lock(), monitor, now, popups);
        }
    }
}

COverview::COverview(PHLMONITOR monitor, bool fromGesture) : m_monitor(monitor) {
    m_startedOn  = monitor->m_activeWorkspace;
    m_selected   = monitor->m_activeWorkspace;
    m_lastActive = monitor->m_activeWorkspace;

    refreshRows();
    showAllRows();

    const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig("hyprzoom");
    Animation::mgr()->createAnimation(0.F, m_progress, CONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation((float)std::max(0, selectedRow()), m_camera, CONFIG, AVARDAMAGE_NONE);
    m_progress->setUpdateCallback(damageOverview);
    m_camera->setUpdateCallback(damageOverview);

    if (fromGesture)
        gestureBegin();
    else
        *m_progress = 1.F;

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    m_cursor = g_pInputManager->getMouseCoordsInternal() - monitor->m_position;

    m_mouseMove   = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo& info) { onMouseMove(info); });
    m_mouseButton = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    m_mouseAxis   = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });
    m_key         = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { onKey(e, info); });

    Log::logger->log(Log::DEBUG, "[hyprzoom] open on {} at workspace {}, {} rows, gesture {}", monitor->m_name, monitor->activeWorkspaceID(), m_rows.size(), fromGesture);

    g_pHyprRenderer->damageMonitor(monitor);
}

COverview::~COverview() {
    Log::logger->log(Log::DEBUG, "[hyprzoom] destroyed");

    Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);

    const auto MONITOR = m_monitor.lock();
    if (!MONITOR)
        return;

    for (const auto& ws : m_rows) {
        if (!ws || ws == MONITOR->m_activeWorkspace)
            continue;
        Animation::Workspace::startAnimation(ws.lock(), Animation::Workspace::ANIMATION_TYPE_OUT, true, true);
    }

    if (MONITOR->m_activeWorkspace)
        Animation::Workspace::startAnimation(MONITOR->m_activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);

    g_pHyprRenderer->damageMonitor(MONITOR);
}

void COverview::refreshRows() {
    const auto MONITOR = m_monitor.lock();
    if (!MONITOR)
        return;

    const int OLDSELECTED = selectedRow();

    m_rows.clear();
    for (const auto& ws : State::workspaceState()->workspaces()) {
        if (ws->m_isSpecialWorkspace || ws->m_monitor != MONITOR)
            continue;
        m_rows.emplace_back(ws);
    }

    std::ranges::sort(m_rows, [](const auto& a, const auto& b) { return a->m_id < b->m_id; });

    if (!m_selected)
        m_selected = MONITOR->m_activeWorkspace;

    const int NEWSELECTED = selectedRow();
    if (m_camera && OLDSELECTED >= 0 && NEWSELECTED >= 0 && OLDSELECTED != NEWSELECTED) {
        m_camera->setValueAndWarp(m_camera->value() + (NEWSELECTED - OLDSELECTED));
        *m_camera = (float)NEWSELECTED;
    }
}

void COverview::showAllRows() {
    for (const auto& ws : m_rows) {
        if (ws)
            Animation::Workspace::startAnimation(ws.lock(), Animation::Workspace::ANIMATION_TYPE_IN, true, true);
    }
}

int COverview::selectedRow() const {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i] == m_selected)
            return (int)i;
    }
    return -1;
}

void COverview::select(int row) {
    if (m_rows.empty())
        return;

    row        = std::clamp(row, 0, (int)m_rows.size() - 1);
    m_selected = m_rows[row];
    *m_camera  = (float)row;
}

bool COverview::closing() const {
    return m_closing;
}

COverview::SView COverview::view() const {
    const auto MONITOR = m_monitor.lock();
    const auto P       = m_progress->value();
    const auto W       = MONITOR->m_size.x;
    const auto H       = MONITOR->m_size.y;

    SView      v;
    v.scale  = 1.F + (std::clamp((float)g_config.zoom->value(), 0.05F, 1.F) - 1.F) * P;
    v.pitch  = H + P * g_config.gap->value() / v.scale;
    v.origin = {W / 2.0 * (1.0 - v.scale), H / 2.0 - (m_camera->value() * v.pitch + H / 2.0) * v.scale};
    return v;
}

double COverview::stripCenter(size_t row) const {
    const auto MONITOR = m_monitor.lock();
    const auto WS      = row < m_rows.size() ? m_rows[row].lock() : nullptr;
    double     left = 0.0, right = MONITOR->m_size.x;

    if (WS) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || w->m_workspace != WS)
                continue;

            const auto BOX = w->getFullWindowBoundingBox();
            left           = std::min(left, BOX.x - MONITOR->m_position.x);
            right          = std::max(right, BOX.x + BOX.w - MONITOR->m_position.x);
        }
    }

    return (left + right) / 2.0;
}

Vector2D COverview::rowOrigin(const SView& v, size_t row) const {
    const auto MONITOR = m_monitor.lock();
    const auto SHIFT   = (MONITOR->m_size.x / 2.0 - stripCenter(row)) * v.scale * m_progress->value();
    return v.origin + Vector2D{SHIFT, row * v.pitch * v.scale};
}

int COverview::rowAt(const Vector2D& local) const {
    const auto V      = view();
    const auto STRIDE = V.pitch * V.scale;
    const auto GAP    = STRIDE - m_monitor->m_size.y * V.scale;
    const auto ROW    = (int)std::floor((local.y - V.origin.y + GAP / 2.0) / STRIDE);
    return std::clamp(ROW, 0, (int)m_rows.size());
}

void COverview::renderOutline(const CBox& logical, const CHyprColor& color, double width) {
    const auto S = m_monitor->m_scale;
    const CBox B = logical.copy().scale(S).round();
    const auto T = std::max(1.0, std::round(width * S));

    addRect({B.x, B.y, B.w, T}, color);
    addRect({B.x, B.y + B.h - T, B.w, T}, color);
    addRect({B.x, B.y + T, T, B.h - 2 * T}, color);
    addRect({B.x + B.w - T, B.y + T, T, B.h - 2 * T}, color);
}

void COverview::renderRow(const SView& v, size_t row, const Time::steady_tp& now) {
    const auto MONITOR = m_monitor.lock();
    const auto WS      = m_rows[row].lock();
    const auto ORIGIN  = rowOrigin(v, row);
    const CBox FRAME   = {ORIGIN, MONITOR->m_size * v.scale};

    if (FRAME.y > MONITOR->m_size.y || FRAME.y + FRAME.h < 0)
        return;

    pushModif(v.scale, ORIGIN * MONITOR->m_scale);

    renderLayers(MONITOR, {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND}, now);

    for (const bool FLOATING : {false, true}) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || w->m_workspace != WS || w->m_isFloating != FLOATING)
                continue;

            if (m_drag.moving && m_drag.window == w)
                continue;

            g_pHyprRenderer->renderWindow(w, MONITOR, now, true, RENDER_PASS_ALL);

            const auto BOX = w->getFullWindowBoundingBox();
            m_hitboxes.emplace_back(SHitbox{w, CBox{ORIGIN + (BOX.pos() - MONITOR->m_position) * v.scale, BOX.size() * v.scale}});
        }
    }

    popModif();

    const auto P     = m_progress->value();
    const auto COLOR = (int)row == selectedRow() ? g_config.selected->value() : g_config.frame->value();
    renderOutline(FRAME, withAlpha(COLOR, P), 2.0);

    if (WS != MONITOR->m_activeWorkspace)
        g_pHyprRenderer->sendFrameEventsToWorkspace(MONITOR, WS, now);
}

void COverview::render(const Time::steady_tp& now) {
    const auto MONITOR = m_monitor.lock();
    if (!MONITOR)
        return;

    refreshRows();

    if (MONITOR->m_activeWorkspace != m_lastActive) {
        m_lastActive = MONITOR->m_activeWorkspace;
        showAllRows();
        if (!m_closing) {
            m_selected = MONITOR->m_activeWorkspace;
            select(selectedRow());
        }
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CNoSimplifyPassElement>());

    const auto V = view();
    const auto P = m_progress->value();

    g_pHyprRenderer->renderBackground(MONITOR);
    renderLayers(MONITOR, {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}, now);
    addRect({{0, 0}, MONITOR->m_pixelSize}, withAlpha(g_config.backdrop->value(), P));

    m_hitboxes.clear();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i])
            renderRow(V, i, now);
    }

    if (!m_closing)
        renderOutline({rowOrigin(V, m_rows.size()), MONITOR->m_size * V.scale}, withAlpha(g_config.frame->value(), P * 0.5F), 2.0);

    if (m_drag.moving && m_drag.window && m_drag.row >= 0) {
        const auto W      = m_drag.window.lock();
        const auto ORIGIN = rowOrigin(V, m_drag.row) + m_drag.delta;
        pushModif(V.scale, ORIGIN * MONITOR->m_scale);
        g_pHyprRenderer->renderWindow(W, MONITOR, now, true, RENDER_PASS_ALL);
        popModif();
    }

    if (m_hovered && P > 0.5F && !m_drag.moving) {
        for (const auto& hb : m_hitboxes) {
            if (hb.window == m_hovered)
                renderOutline(hb.box, withAlpha(g_config.hover->value(), P), 3.0);
        }
    }

    renderLayers(MONITOR, {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}, now);
    renderLayers(MONITOR, {0, 1, 2, 3}, now, true);
}

void COverview::onPreRender(PHLMONITOR monitor) {
    if (monitor != m_monitor)
        return;

    g_pHyprRenderer->damageMonitor(monitor);
}

void COverview::onMouseMove(Event::SCallbackInfo& info) {
    if (m_closing)
        return;

    info.cancelled = true;
    m_cursor       = g_pInputManager->getMouseCoordsInternal() - m_monitor->m_position;

    if (m_drag.pressed) {
        m_drag.delta = m_cursor - m_drag.start;
        if (!m_drag.moving && m_drag.window && m_drag.delta.size() > DRAG_THRESHOLD)
            m_drag.moving = true;
    }

    m_hovered.reset();
    for (auto it = m_hitboxes.rbegin(); it != m_hitboxes.rend(); ++it) {
        if (it->box.containsPoint(m_cursor)) {
            m_hovered = it->window;
            break;
        }
    }
}

void COverview::onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    if (m_closing)
        return;

    info.cancelled = true;

    if (e.button != BTN_LEFT)
        return;

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int row = -1;
        if (m_hovered) {
            for (size_t i = 0; i < m_rows.size(); ++i) {
                if (m_rows[i] == m_hovered->m_workspace)
                    row = (int)i;
            }
        }
        m_drag = SDrag{.window = m_hovered, .start = m_cursor, .row = row, .pressed = true};
        return;
    }

    if (!m_drag.pressed)
        return;

    const auto DRAG = m_drag;
    m_drag          = SDrag{};

    if (DRAG.moving) {
        m_drag = DRAG;
        drop();
        m_drag = SDrag{};
        return;
    }

    if (DRAG.window) {
        const auto W = DRAG.window.lock();
        close(W->m_workspace, W);
        return;
    }

    const auto ROW = rowAt(m_cursor);
    if (ROW < (int)m_rows.size())
        close(m_rows[ROW].lock());
}

void COverview::drop() {
    const auto W       = m_drag.window.lock();
    const auto MONITOR = m_monitor.lock();
    if (!W || !MONITOR)
        return;

    const auto   ROW = rowAt(m_cursor);
    PHLWORKSPACE target;

    if (ROW >= (int)m_rows.size()) {
        WORKSPACEID id = 0;
        for (const auto& ws : State::workspaceState()->workspaces()) {
            if (!ws->m_isSpecialWorkspace)
                id = std::max(id, ws->m_id);
        }
        target = State::workspaceState()->create(id + 1, MONITOR->m_id);
    } else
        target = m_rows[ROW].lock();

    if (!target || target == W->m_workspace)
        return;

    Log::logger->log(Log::DEBUG, "[hyprzoom] move {} to workspace {}", W->m_title, target->m_id);

    const auto RESULT = Config::Actions::moveToWorkspace(target, true, W);
    if (!RESULT)
        Log::logger->log(Log::ERR, "[hyprzoom] moving {} failed", W->m_title);

    refreshRows();
    showAllRows();
}

void COverview::onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    if (m_closing)
        return;

    info.cancelled = true;

    if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
        select(selectedRow() + (e.delta > 0 ? 1 : -1));
        return;
    }

    m_axisAccum += e.delta;
    while (std::abs(m_axisAccum) >= AXIS_STEP) {
        const int DIR = m_axisAccum > 0 ? 1 : -1;
        select(selectedRow() + DIR);
        m_axisAccum -= DIR * AXIS_STEP;
    }
}

void COverview::onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
    if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (m_heldKeys.erase(e.keycode))
            info.cancelled = true;
        return;
    }

    if (m_closing)
        return;

    switch (e.keycode) {
        case KEY_ESC: abort(); break;
        case KEY_ENTER:
        case KEY_KPENTER:
        case KEY_SPACE: closeToSelection(); break;
        case KEY_UP: select(selectedRow() - 1); break;
        case KEY_DOWN: select(selectedRow() + 1); break;
        default: return;
    }

    m_heldKeys.insert(e.keycode);
    info.cancelled = true;
}

void COverview::close(PHLWORKSPACE target, PHLWINDOW focus) {
    if (m_closing)
        return;

    Log::logger->log(Log::DEBUG, "[hyprzoom] close to workspace {}, focus {}", target ? target->m_id : WORKSPACE_INVALID, focus ? focus->m_title : "none");

    m_closing   = true;
    m_gesturing = false;
    m_hovered.reset();

    const auto MONITOR = m_monitor.lock();

    if (target && target != MONITOR->m_activeWorkspace)
        (void)Config::Actions::changeWorkspace(target);

    m_lastActive = MONITOR->m_activeWorkspace;
    m_selected   = MONITOR->m_activeWorkspace;
    refreshRows();
    showAllRows();
    *m_camera = (float)std::max(0, selectedRow());

    if (focus)
        Desktop::focusState()->fullWindowFocus(focus, Desktop::FOCUS_REASON_CLICK);

    if (m_progress->value() <= 0.001F) {
        g_pEventLoopManager->doLater([] { g_pOverview.reset(); });
        return;
    }

    *m_progress = 0.F;
    m_progress->setCallbackOnEnd([](auto) { g_pEventLoopManager->doLater([] { g_pOverview.reset(); }); });
}

void COverview::closeToSelection() {
    close(m_selected.lock());
}

void COverview::abort() {
    close(m_startedOn.lock());
}

void COverview::gestureBegin() {
    m_gesturing    = true;
    m_closing      = false;
    m_gestureValue = m_progress->value();
    m_progress->setCallbackOnEnd(nullptr);
}

void COverview::gestureUpdate(float delta) {
    if (!m_gesturing)
        return;

    m_gestureValue = std::clamp(m_gestureValue + delta, 0.F, 1.F);
    m_progress->setValueAndWarp(m_gestureValue);
}

void COverview::gestureEnd() {
    if (!m_gesturing)
        return;

    m_gesturing = false;

    if (m_gestureValue > 0.5F)
        *m_progress = 1.F;
    else
        closeToSelection();
}
