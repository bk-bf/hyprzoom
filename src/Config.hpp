#pragma once

#include "globals.hpp"

#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

struct SHyprzoomConfig {
    SP<Config::Values::CFloatValue> zoom;
    SP<Config::Values::CIntValue>   gap;
    SP<Config::Values::CColorValue> backdrop;
    SP<Config::Values::CIntValue>   backdropBlur;
    SP<Config::Values::CColorValue> lane;
    SP<Config::Values::CColorValue> frame;
    SP<Config::Values::CColorValue> accent;
    SP<Config::Values::CColorValue> label;
    SP<Config::Values::CColorValue> labelText;
    SP<Config::Values::CIntValue>   gestureDistance;
};

inline SHyprzoomConfig g_config;
