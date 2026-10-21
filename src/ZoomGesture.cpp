#include "ZoomGesture.hpp"
#include "Overview.hpp"
#include "Config.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

void CZoomGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_firstUpdate = true;

    if (g_pOverview) {
        g_pOverview->gestureBegin();
        return;
    }

    const auto MONITOR = Desktop::focusState()->monitor();
    if (MONITOR && MONITOR->m_activeWorkspace)
        g_pOverview = makeUnique<COverview>(MONITOR, true);
}

void CZoomGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_firstUpdate) {
        m_firstUpdate = false;
        return;
    }

    if (g_pOverview)
        g_pOverview->gestureUpdate(distance(e) / std::max<float>(1.F, g_config.gestureDistance->value()));
}

void CZoomGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (g_pOverview)
        g_pOverview->gestureEnd();
}
