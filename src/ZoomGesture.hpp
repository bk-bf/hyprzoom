#pragma once

#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>

class CZoomGesture : public ITrackpadGesture {
  public:
    CZoomGesture()          = default;
    virtual ~CZoomGesture() = default;

    virtual void begin(const ITrackpadGesture::STrackpadGestureBegin& e);
    virtual void update(const ITrackpadGesture::STrackpadGestureUpdate& e);
    virtual void end(const ITrackpadGesture::STrackpadGestureEnd& e);

  private:
    bool m_firstUpdate = false;
};
