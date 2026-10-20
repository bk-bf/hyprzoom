#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/event/EventBus.hpp>

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

    struct SDrag {
        PHLWINDOWREF window;
        Vector2D     start;
        Vector2D     delta;
        int          row     = -1;
        bool         pressed = false;
        bool         moving  = false;
    };

    SView                        view() const;
    Vector2D                     rowOrigin(const SView& v, size_t row) const;
    int                          rowAt(const Vector2D& local) const;
    void                         refreshRows();
    void                         showAllRows();
    void                         select(int row);
    int                          selectedRow() const;
    void                         renderRow(const SView& v, size_t row, const Time::steady_tp& now);
    void                         renderOutline(const CBox& logical, const CHyprColor& color, double width);
    void                         drop();

    void                         onMouseMove(Event::SCallbackInfo& info);
    void                         onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void                         onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void                         onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);

    std::vector<PHLWORKSPACEREF> m_rows;
    std::vector<SHitbox>         m_hitboxes;
    PHLWORKSPACEREF              m_startedOn;
    PHLWORKSPACEREF              m_selected;
    PHLWORKSPACEREF              m_lastActive;
    PHLWINDOWREF                 m_hovered;
    SDrag                        m_drag;

    PHLANIMVAR<float>            m_progress;
    PHLANIMVAR<float>            m_camera;

    Vector2D                     m_cursor;
    double                       m_axisAccum     = 0.0;
    float                        m_gestureValue  = 0.F;
    bool                         m_gesturing     = false;
    bool                         m_closing       = false;

    CHyprSignalListener          m_mouseMove;
    CHyprSignalListener          m_mouseButton;
    CHyprSignalListener          m_mouseAxis;
    CHyprSignalListener          m_key;
};

inline UP<COverview> g_pOverview;
