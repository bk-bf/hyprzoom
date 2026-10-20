#define WLR_USE_UNSTABLE

#include "globals.hpp"

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprzoom] built against different Hyprland headers than the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprzoom] version mismatch");
    }

    return {"hyprzoom", "Zoomed-out workspace overview", "bk-bf", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
