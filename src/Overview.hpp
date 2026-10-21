#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class COverview {
  public:
    COverview(PHLMONITOR monitor, bool fromGesture);
    ~COverview();

    void          render(const Time::steady_tp& now);
    void          onPreRender(PHLMONITOR monitor);

    void          close(PHLWORKSPACE target, PHLWINDOW focus = nullptr);
    void          closeToSelection();
    void          abort();
    bool          closing() const;

    void          gestureBegin();
    void          gestureUpdate(float delta);
    void          gestureEnd();

    PHLMONITORREF m_monitor;

  private:
    struct SView {
        float    scale = 1.F;
        double   pitch = 0.0;
        Vector2D origin;
    };

    struct SHitbox {
        PHLWINDOWREF window;
        CBox         box;
    };
    struct SSwipe {
        enum eAxis : uint8_t {
            AXIS_NONE,
            AXIS_X,
            AXIS_Y,
        };
        Vector2D travel;
        eAxis    axis = AXIS_NONE;
        int      row  = -1;
    };

    struct SDrag {
        PHLWINDOWREF window;
        Vector2D     start;
        Vector2D     delta;
        int          row     = -1;
        bool         pressed = false;
        bool         moving  = false;
    };

    SView                                                 view() const;
    CBox                                                  frameBox(const SView& v, size_t row) const;
    Vector2D                                              rowOrigin(const SView& v, size_t row) const;
    int                                                   rowAt(const Vector2D& local) const;
    void                                                  refreshRows();
    void                                                  showAllRows();
    void                                                  select(int row);
    int                                                   selectedRow() const;
    double                                                pan(size_t row) const;
    PHLANIMVAR<float>&                                    panVar(WORKSPACEID id);
    void                                                  panBy(size_t row, double delta);
    void                                                  focusColumn(int dir);
    void                                                  keepFocusVisible();
    void                                                  renderRow(const SView& v, size_t row, const Time::steady_tp& now);
    void                                                  renderWindowAt(PHLWINDOW window, const Vector2D& origin, float scale, const Time::steady_tp& now);
    void                                                  renderLabel(const CBox& frame, const std::string& text, bool selected);
    void                                                  renderNewSlot(const SView& v);
    void                                                  renderOutline(const CBox& logical, const CHyprColor& color, double width);
    void                                                  renderDashed(const CBox& logical, const CHyprColor& color, double width, double dash, double space);
    SP<Render::ITexture>                                  text(const std::string& str, int64_t color, int pt);
    void                                                  drop();
    std::vector<std::pair<double, double>>                columnsOf(PHLWORKSPACE ws, PHLWINDOW skip) const;
    static int                                            insertIndex(const std::vector<std::pair<double, double>>& columns, double localX);
    void                                                  placeColumn(PHLWINDOW window, int index);
    void                                                  renderRing(const CBox& logical);
    void                                                  onSwipeBegin(Event::SCallbackInfo& info);
    void                                                  onSwipeUpdate(const IPointer::SSwipeUpdateEvent& e, Event::SCallbackInfo& info);
    void                                                  onSwipeEnd(Event::SCallbackInfo& info);

    void                                                  onMouseMove(Event::SCallbackInfo& info);
    void                                                  onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void                                                  onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void                                                  onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);

    std::vector<PHLWORKSPACEREF>                          m_rows;
    std::vector<SHitbox>                                  m_hitboxes;
    PHLWORKSPACEREF                                       m_startedOn;
    PHLWORKSPACEREF                                       m_selected;
    PHLWORKSPACEREF                                       m_lastActive;
    PHLWINDOWREF                                          m_hovered;
    PHLWINDOWREF                                          m_focus;
    SDrag                                                 m_drag;
    SSwipe                                                m_swipe;
    std::unordered_set<uint32_t>                          m_heldKeys;
    std::unordered_map<WORKSPACEID, PHLANIMVAR<float>>    m_pan;
    std::unordered_map<std::string, SP<Render::ITexture>> m_text;

    PHLANIMVAR<float>                                     m_progress;
    PHLANIMVAR<float>                                     m_camera;

    Vector2D                                              m_cursor;
    double                                                m_axisAccum    = 0.0;
    float                                                 m_gestureValue = 0.F;
    bool                                                  m_gesturing    = false;
    bool                                                  m_closing      = false;

    CHyprSignalListener                                   m_mouseMove;
    CHyprSignalListener                                   m_mouseButton;
    CHyprSignalListener                                   m_mouseAxis;
    CHyprSignalListener                                   m_key;
    CHyprSignalListener                                   m_swipeBegin;
    CHyprSignalListener                                   m_swipeUpdate;
    CHyprSignalListener                                   m_swipeEnd;
};

inline UP<COverview> g_pOverview;
