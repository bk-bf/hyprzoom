#pragma once

#include "globals.hpp"

#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

struct SHyprzoomConfig {
    SP<Config::Values::CFloatValue> zoom;
    SP<Config::Values::CIntValue>   gap;
    SP<Config::Values::CColorValue> backdrop;
    SP<Config::Values::CColorValue> frame;
    SP<Config::Values::CColorValue> selected;
    SP<Config::Values::CColorValue> hover;
    SP<Config::Values::CIntValue>   gestureDistance;
};

inline SHyprzoomConfig g_config;
