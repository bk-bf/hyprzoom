#include "Overview.hpp"
#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
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
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private
#undef protected

using namespace Render;

constexpr double DRAG_THRESHOLD = 8.0;
constexpr double AXIS_STEP      = 30.0;
constexpr double SWIPE_LOCK     = 12.0;

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

static void addRect(const CBox& px, const CHyprColor& color, int round = 0, bool blur = false, float blurA = 1.F) {
    if ((color.a <= 0.F && !blur) || px.w <= 0 || px.h <= 0)
        return;

    CRectPassElement::SRectData data;
    data.box   = px;
    data.color = color;
    data.round = round;
    data.blur  = blur;
    data.blurA = blurA;
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
    select(selectedRow());

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
    m_swipeBegin  = Event::bus()->m_events.gesture.swipe.begin.listen([this](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) { onSwipeBegin(e, info); });
    m_swipeUpdate = Event::bus()->m_events.gesture.swipe.update.listen([this](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) { onSwipeUpdate(e, info); });
    m_swipeEnd    = Event::bus()->m_events.gesture.swipe.end.listen([this](IPointer::SSwipeEndEvent, Event::SCallbackInfo& info) { onSwipeEnd(info); });

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

    row = std::clamp(row, 0, (int)m_rows.size() - 1);
    if (m_rows[row] != m_selected || !m_focus || m_focus->m_workspace != m_rows[row]) {
        const auto WS = m_rows[row].lock();
        m_focus       = WS->getLastFocusedWindow();
        if (!m_focus || m_focus->m_workspace != WS)
            m_focus.reset();
    }

    m_selected = m_rows[row];
    *m_camera  = (float)row;
    keepFocusVisible();
}

double COverview::pan(size_t row) const {
    const auto IT = m_pan.find(m_rows[row]->m_id);
    return IT == m_pan.end() ? 0.0 : IT->second->value();
}

PHLANIMVAR<float>& COverview::panVar(WORKSPACEID id) {
    auto& var = m_pan[id];
    if (!var) {
        Animation::mgr()->createAnimation(0.F, var, Config::animationTree()->getAnimationPropertyConfig("hyprzoom"), AVARDAMAGE_NONE);
        var->setUpdateCallback(damageOverview);
    }
    return var;
}

void COverview::panBy(size_t row, double delta) {
    const auto MONITOR = m_monitor.lock();
    const auto WS      = m_rows[row].lock();
    const auto W       = MONITOR->m_size.x;
    double     right   = W;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (w->m_isMapped && !w->isHidden() && w->m_workspace == WS)
            right = std::max(right, w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).x + w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL).x - MONITOR->m_position.x);
    }

    auto& var = panVar(WS->m_id);
    var->setValueAndWarp(std::clamp(var->goal() + delta, -W / 2.0, std::max(0.0, right - W / 2.0)));
}

void COverview::focusColumn(int dir) {
    const auto WS      = m_selected.lock();
    const auto MONITOR = m_monitor.lock();
    if (!WS)
        return;

    std::vector<PHLWINDOW> tiled;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w->m_isMapped && !w->isHidden() && w->m_workspace == WS && !w->m_isFloating)
            tiled.emplace_back(w);
    }

    if (tiled.empty())
        return;

    std::vector<double> columns;
    for (const auto& w : tiled) {
        columns.emplace_back(std::round(w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).x));
    }
    std::ranges::sort(columns);
    const auto [first, last] = std::ranges::unique(columns);
    columns.erase(first, last);

    int current = -1;
    if (m_focus && m_focus->m_workspace == WS && !m_focus->m_isFloating) {
        const auto X = std::round(m_focus->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).x);
        current      = (int)(std::ranges::find(columns, X) - columns.begin());
    }

    const int TARGET = current < 0 ? 0 : std::clamp(current + dir, 0, (int)columns.size() - 1);
    PHLWINDOW best;
    for (const auto& w : tiled) {
        const auto POS = w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        if (std::round(POS.x) == columns[TARGET] && (!best || POS.y < best->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).y))
            best = w;
    }

    m_focus = best;
    keepFocusVisible();
}

void COverview::keepFocusVisible() {
    const auto MONITOR = m_monitor.lock();
    const auto W       = m_focus.lock();
    if (!MONITOR || !W || !W->m_workspace)
        return;

    const auto LEFT  = W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).x - MONITOR->m_position.x;
    const auto WIDTH = W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL).x;
    auto&      var   = panVar(W->m_workspace->m_id);
    const auto P     = var->goal();

    if (LEFT < P)
        *var = LEFT;
    else if (LEFT + WIDTH > P + MONITOR->m_size.x)
        *var = LEFT + WIDTH - MONITOR->m_size.x;
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

CBox COverview::frameBox(const SView& v, size_t row) const {
    const auto& R = m_monitor->m_reservedArea;
    return {rowOrigin(v, row) + Vector2D{R.left(), R.top()} * v.scale, (m_monitor->m_size - Vector2D{R.left() + R.right(), R.top() + R.bottom()}) * v.scale};
}

Vector2D COverview::rowOrigin(const SView& v, size_t row) const {
    return v.origin + Vector2D{0.0, row * v.pitch * v.scale};
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

void COverview::renderWindowAt(PHLWINDOW window, const Vector2D& origin, float scale, const Time::steady_tp& now) {
    const auto MONITOR = m_monitor.lock();
    const auto LOCAL   = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
    const auto SHIFT   = origin + LOCAL * scale - LOCAL;
    const auto SAVED   = window->m_floatingOffset;

    pushModif(scale, (origin - SHIFT * scale) * MONITOR->m_scale);
    window->m_floatingOffset = SAVED + SHIFT;
    g_pHyprRenderer->renderWindow(window, MONITOR, now, true, RENDER_PASS_ALL);
    window->m_floatingOffset = SAVED;
}

void COverview::renderRow(const SView& v, size_t row, const Time::steady_tp& now) {
    const auto MONITOR = m_monitor.lock();
    const auto WS      = m_rows[row].lock();
    const auto ORIGIN  = rowOrigin(v, row);
    const auto STRIP   = ORIGIN - Vector2D{pan(row) * v.scale, 0.0};
    const CBox FRAME   = frameBox(v, row);
    const auto P       = m_progress->value();
    const auto MS      = MONITOR->m_scale;

    if (FRAME.y > MONITOR->m_size.y || FRAME.y + FRAME.h < 0)
        return;

    addRect(CBox{0.0, FRAME.y * MS, MONITOR->m_pixelSize.x, FRAME.h * MS}.round(), withAlpha(g_config.lane->value(), P));

    for (const bool FLOATING : {false, true}) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || w->m_workspace != WS || w->m_isFloating != FLOATING)
                continue;

            if (m_drag.moving && m_drag.window == w)
                continue;

            renderWindowAt(w, STRIP, v.scale, now);

            const auto POS  = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
            const auto SIZE = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            m_hitboxes.emplace_back(SHitbox{w, CBox{STRIP + POS * v.scale, SIZE * v.scale}});
        }
    }

    popModif();

    renderOutline(FRAME, withAlpha(g_config.frame->value(), P), 1.5);
    renderLabel(FRAME, WS->m_name, (int)row == selectedRow());

    if (WS != MONITOR->m_activeWorkspace)
        g_pHyprRenderer->sendFrameEventsToWorkspace(MONITOR, WS, now);
}

SP<Render::ITexture> COverview::text(const std::string& str, int64_t color, int pt) {
    const auto KEY = std::format("{}|{}|{}", str, color, pt);
    auto&      tex = m_text[KEY];
    if (!tex)
        tex = g_pHyprRenderer->renderText(str, CHyprColor{(uint64_t)color}, pt, false, "", 0, 600);
    return tex;
}

void COverview::renderLabel(const CBox& frame, const std::string& str, bool selected) {
    const auto MS  = m_monitor->m_scale;
    const auto P   = m_progress->value();
    const auto TEX = text(str, selected ? g_config.label->value() : g_config.labelText->value(), std::round(13 * MS));
    if (!TEX || P <= 0.F)
        return;

    const Vector2D PAD  = Vector2D{9, 5} * MS;
    const Vector2D SIZE = TEX->m_size + PAD * 2;
    const CBox     PILL = CBox{frame.x * MS - 14 * MS - SIZE.x, frame.y * MS, SIZE.x, SIZE.y}.round();

    addRect(PILL, withAlpha(selected ? g_config.accent->value() : g_config.label->value(), P), std::round(7 * MS));

    CTexPassElement::SRenderData data;
    data.tex = TEX;
    data.box = CBox{PILL.pos() + PAD, TEX->m_size}.round();
    data.a   = P;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
}

void COverview::renderNewSlot(const SView& v) {
    const auto MS    = m_monitor->m_scale;
    const auto P     = m_progress->value();
    const CBox FRAME = frameBox(v, m_rows.size());
    const bool DROP  = m_drag.moving && rowAt(m_cursor) >= (int)m_rows.size();
    const auto COLOR = DROP ? g_config.accent->value() : g_config.labelText->value();

    renderDashed(FRAME, withAlpha(COLOR, P * (DROP ? 1.F : 0.4F)), 2.0, 12.0, 8.0);

    const auto ZOOM = std::clamp((float)g_config.zoom->value(), 0.05F, 1.F);
    const auto TEX  = text("+", COLOR, std::round(160 * ZOOM * MS));
    if (!TEX)
        return;

    const auto                   SIZE = TEX->m_size * (v.scale / ZOOM);
    CTexPassElement::SRenderData data;
    data.tex = TEX;
    data.box = CBox{FRAME.middle() * MS - SIZE / 2.0, SIZE}.round();
    data.a   = P * (DROP ? 1.F : 0.4F);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
}

void COverview::renderDashed(const CBox& logical, const CHyprColor& color, double width, double dash, double space) {
    const auto S = m_monitor->m_scale;
    const CBox B = logical.copy().scale(S).round();
    const auto T = std::max(1.0, std::round(width * S));
    const auto D = dash * S, STEP = (dash + space) * S;

    for (double x = 0; x < B.w; x += STEP) {
        addRect(CBox{B.x + x, B.y, std::min(D, B.w - x), T}.round(), color);
        addRect(CBox{B.x + x, B.y + B.h - T, std::min(D, B.w - x), T}.round(), color);
    }
    for (double y = 0; y < B.h; y += STEP) {
        addRect(CBox{B.x, B.y + y, T, std::min(D, B.h - y)}.round(), color);
        addRect(CBox{B.x + B.w - T, B.y + y, T, std::min(D, B.h - y)}.round(), color);
    }
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
    addRect({{0, 0}, MONITOR->m_pixelSize}, withAlpha(g_config.backdrop->value(), P), 0, g_config.backdropBlur->value() && P > 0.F, P);

    m_hitboxes.clear();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i])
            renderRow(V, i, now);
    }

    if (!m_closing)
        renderNewSlot(V);

    if (m_drag.moving && m_drag.window && m_drag.row >= 0) {
        renderWindowAt(m_drag.window.lock(), rowOrigin(V, m_drag.row) - Vector2D{pan(m_drag.row) * V.scale, 0.0} + m_drag.delta, V.scale, now);
        popModif();
        const auto DROP  = dropTarget();
        const auto MS    = MONITOR->m_scale;
        const auto WIDTH = std::round(4 * MS);
        const auto COLOR = withAlpha(g_config.accent->value(), P);
        if (DROP.onto) {
            auto half = DROP.box;
            switch (DROP.side) {
                case SDropTarget::SIDE_TOP: half.h /= 2.0; break;
                case SDropTarget::SIDE_BOTTOM:
                    half.h /= 2.0;
                    half.y += half.h;
                    break;
                case SDropTarget::SIDE_LEFT: half.w /= 2.0; break;
                case SDropTarget::SIDE_RIGHT:
                    half.w /= 2.0;
                    half.x += half.w;
                    break;
            }
            addRect(half.copy().scale(MS).round(), withAlpha(g_config.accent->value(), P * 0.35F), std::round(6 * MS));
            renderRing(half);
        } else if (DROP.row >= 0 && DROP.row < (int)m_rows.size()) {
            const auto STRIP = rowOrigin(V, DROP.row) - Vector2D{pan(DROP.row) * V.scale, 0.0};
            const auto COLS  = columnsOf(m_rows[DROP.row].lock(), m_drag.window.lock());
            const auto GAP   = COLS.empty() ? 0.0 : DROP.column == 0 ? COLS.front().first : COLS[DROP.column - 1].second;
            const auto FRAME = frameBox(V, DROP.row);
            addRect(CBox{(STRIP.x + GAP * V.scale) * MS - WIDTH / 2.0, FRAME.y * MS, WIDTH, FRAME.h * MS}.round(), COLOR, WIDTH / 2);
        }
    }

    const auto RING = m_hovered ? m_hovered : m_focus;
    for (const auto& hb : m_hitboxes) {
        if (RING && hb.window == RING && !m_drag.moving)
            renderRing(hb.box);
    }

    renderLayers(MONITOR, {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}, now);
    renderLayers(MONITOR, {0, 1, 2, 3}, now, true);
}

void COverview::onPreRender(PHLMONITOR monitor) {
    if (monitor != m_monitor)
        return;

    g_pEventLoopManager->doLater([ref = PHLMONITORREF{monitor}] {
        if (g_pOverview && ref)
            g_pHyprRenderer->damageMonitor(ref.lock());
    });
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

    if (!target)
        return;

    const auto DROP = dropTarget();

    if (target != W->m_workspace) {
        Log::logger->log(Log::DEBUG, "[hyprzoom] move {} to workspace {}", W->m_title, target->m_id);

        const auto RESULT = Config::Actions::moveToWorkspace(target, true, W);
        if (!RESULT)
            Log::logger->log(Log::ERR, "[hyprzoom] moving {} failed", W->m_title);
    }

    if (W->m_workspace == target) {
        if (const auto ONTO = DROP.onto.lock(); ONTO && ONTO->m_workspace == target)
            dropOnto(W, ONTO, DROP.side);
        else if (DROP.column >= 0)
            placeColumn(W, DROP.column);
    }

    refreshRows();
    showAllRows();
}

std::vector<std::pair<double, double>> COverview::columnsOf(PHLWORKSPACE ws, PHLWINDOW skip) const {
    const auto                             MONITOR = m_monitor.lock();
    std::vector<std::pair<double, double>> columns;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w->m_isMapped || w->isHidden() || w->m_workspace != ws || w->m_isFloating || w == skip)
            continue;

        const auto LEFT  = std::round(w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).x - MONITOR->m_position.x);
        const auto RIGHT = LEFT + w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL).x;
        if (std::ranges::none_of(columns, [LEFT](const auto& c) { return c.first == LEFT; }))
            columns.emplace_back(LEFT, RIGHT);
    }

    std::ranges::sort(columns);
    return columns;
}

COverview::SDropTarget COverview::dropTarget() const {
    SDropTarget target{.row = rowAt(m_cursor)};
    if (target.row >= (int)m_rows.size())
        return target;

    const auto WS      = m_rows[target.row].lock();
    const auto DRAGGED = m_drag.window.lock();

    for (const auto& hb : m_hitboxes) {
        const auto W = hb.window.lock();
        if (!W || W == DRAGGED || W->m_workspace != WS || W->m_isFloating || !hb.box.containsPoint(m_cursor))
            continue;

        const auto D = (m_cursor - hb.box.middle()) / (hb.box.size() / 2.0);
        target.onto  = W;
        target.box   = hb.box;
        target.side  = std::abs(D.x) > std::abs(D.y) ? (D.x < 0 ? SDropTarget::SIDE_LEFT : SDropTarget::SIDE_RIGHT) : (D.y < 0 ? SDropTarget::SIDE_TOP : SDropTarget::SIDE_BOTTOM);
        return target;
    }

    const auto V     = view();
    const auto STRIP = rowOrigin(V, target.row) - Vector2D{pan(target.row) * V.scale, 0.0};
    target.column    = insertIndex(columnsOf(WS, DRAGGED), (m_cursor.x - STRIP.x) / V.scale);
    return target;
}

void COverview::dropOnto(PHLWINDOW window, PHLWINDOW onto, SDropTarget::eSide side) {
    if (!window->m_workspace || !window->m_workspace->m_space || !window->m_workspace->m_space->algorithm())
        return;

    const auto ALGO = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(window->m_workspace->m_space->algorithm()->tiledAlgo().get());
    if (!ALGO)
        return;

    const auto& DATA  = ALGO->m_scrollingData;
    const auto  TDATA = ALGO->dataFor(window->layoutTarget());
    const auto  ODATA = ALGO->dataFor(onto->layoutTarget());
    const auto  SRC   = TDATA ? TDATA->column.lock() : nullptr;
    const auto  DST   = ODATA ? ODATA->column.lock() : nullptr;
    if (!SRC || !DST)
        return;

    if (side == SDropTarget::SIDE_TOP || side == SDropTarget::SIDE_BOTTOM) {
        SRC->remove(window->layoutTarget());
        const int AT = (int)DST->idx(onto->layoutTarget());
        DST->add(TDATA, side == SDropTarget::SIDE_BOTTOM ? AT : AT - 1);
        DATA->recalculate();
        return;
    }

    int at = (int)DATA->idx(DST);
    if (SRC->targetDatas.size() == 1 && SRC != DST && DATA->idx(SRC) < at)
        --at;
    placeColumn(window, side == SDropTarget::SIDE_RIGHT ? at + 1 : at);

    const auto NEWCOL = TDATA->column.lock();
    if (!NEWCOL || NEWCOL == DST)
        return;

    auto&      dst                                     = DATA->controller->getStrip(DATA->idx(DST));
    const auto WIDTH                                   = dst.size / 2.F;
    dst.size                                           = WIDTH;
    DATA->controller->getStrip(DATA->idx(NEWCOL)).size = WIDTH;

    const auto LEFT  = side == SDropTarget::SIDE_RIGHT ? DST : NEWCOL;
    const auto RIGHT = side == SDropTarget::SIDE_RIGHT ? NEWCOL : DST;
    DATA->recalculate();
    DATA->fitCol(RIGHT);
    DATA->fitCol(LEFT);
    DATA->recalculate();
}

int COverview::insertIndex(const std::vector<std::pair<double, double>>& columns, double localX) {
    return (int)std::ranges::count_if(columns, [localX](const auto& c) { return (c.first + c.second) / 2.0 < localX; });
}

void COverview::placeColumn(PHLWINDOW window, int index) {
    if (!window->m_workspace || !window->m_workspace->m_space || !window->m_workspace->m_space->algorithm())
        return;

    const auto ALGO = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(window->m_workspace->m_space->algorithm()->tiledAlgo().get());
    if (!ALGO)
        return;

    const auto& DATA  = ALGO->m_scrollingData;
    const auto  TDATA = ALGO->dataFor(window->layoutTarget());
    const auto  SRC   = TDATA ? TDATA->column.lock() : nullptr;
    if (!SRC)
        return;

    if (SRC->targetDatas.size() > 1) {
        index          = std::clamp(index, 0, (int)DATA->columns.size());
        const auto COL = DATA->add(index - 1);
        SRC->remove(window->layoutTarget());
        COL->add(TDATA);
    } else {
        int current = (int)DATA->idx(SRC);
        index       = std::clamp(index, 0, (int)DATA->columns.size() - 1);
        while (current != index) {
            const int NEXT = current + (index > current ? 1 : -1);
            std::swap(DATA->columns.at(current), DATA->columns.at(NEXT));
            DATA->controller->swapStrips(current, NEXT);
            current = NEXT;
        }
    }

    DATA->recalculate();
    Log::logger->log(Log::DEBUG, "[hyprzoom] placed {} at column {}", window->m_title, index);
}

void COverview::renderRing(const CBox& logical) {
    const auto                      MS = m_monitor->m_scale;

    CBorderPassElement::SBorderData data;
    data.box        = logical.copy().scale(MS).round();
    data.grad1      = Config::CGradientValueData{withAlpha(g_config.accent->value(), m_progress->value())};
    data.borderSize = 3;
    data.round      = 0;
    data.outerRound = std::round(6 * MS);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
}

void COverview::onSwipeBegin(const IPointer::SSwipeBeginEvent& e, Event::SCallbackInfo& info) {
    if (m_closing)
        return;

    info.cancelled = true;
    m_swipe        = {.consuming = true, .zoom = isZoomSwipe(e.fingers)};
}

void COverview::onSwipeUpdate(const IPointer::SSwipeUpdateEvent& e, Event::SCallbackInfo& info) {
    if (!m_swipe.consuming)
        return;

    info.cancelled = true;
    m_swipe.travel = m_swipe.travel + e.delta;

    if (m_swipe.axis == SSwipe::AXIS_NONE) {
        if (m_swipe.travel.size() < SWIPE_LOCK)
            return;
        m_swipe.axis = std::abs(m_swipe.travel.x) > std::abs(m_swipe.travel.y) ? SSwipe::AXIS_X : SSwipe::AXIS_Y;
        m_swipe.row  = rowAt(m_cursor) < (int)m_rows.size() ? rowAt(m_cursor) : selectedRow();
        if (m_swipe.zoom && m_swipe.axis == SSwipe::AXIS_Y)
            gestureBegin();
    }

    const auto V = view();
    if (m_swipe.axis == SSwipe::AXIS_X) {
        if (m_swipe.row >= 0)
            panBy(m_swipe.row, -e.delta.x / V.scale);
        return;
    }

    if (m_swipe.zoom) {
        gestureUpdate(-e.delta.y / std::max<float>(1.F, g_config.gestureDistance->value()));
        return;
    }

    const auto LAST = std::max(0.0, (double)m_rows.size() - 1);
    m_camera->setValueAndWarp(std::clamp(m_camera->value() - e.delta.y / (V.pitch * V.scale), -0.5, LAST + 0.5));
}

void COverview::onSwipeEnd(Event::SCallbackInfo& info) {
    if (!m_swipe.consuming)
        return;

    info.cancelled   = true;
    const auto SWIPE = m_swipe;
    m_swipe          = {};

    if (SWIPE.axis != SSwipe::AXIS_Y)
        return;

    if (SWIPE.zoom)
        gestureEnd();
    else
        select((int)std::lround(m_camera->value()));
}

void COverview::onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    if (m_closing)
        return;

    info.cancelled = true;

    if (e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        const auto ROW = rowAt(m_cursor) < (int)m_rows.size() ? rowAt(m_cursor) : selectedRow();
        if (ROW >= 0)
            panBy(ROW, e.delta / view().scale);
        return;
    }

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
        case KEY_UP:
            m_hovered.reset();
            select(selectedRow() - 1);
            break;
        case KEY_DOWN:
            m_hovered.reset();
            select(selectedRow() + 1);
            break;
        case KEY_LEFT:
            m_hovered.reset();
            focusColumn(-1);
            break;
        case KEY_RIGHT:
            m_hovered.reset();
            focusColumn(1);
            break;
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
    const auto FOCUS = m_focus.lock();
    close(m_selected.lock(), FOCUS && FOCUS->m_workspace == m_selected ? FOCUS : nullptr);
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
