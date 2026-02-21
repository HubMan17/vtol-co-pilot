#pragma once

#include <QString>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace vtol::theme {

// ── Backgrounds ──
inline constexpr auto BG_APP       = "#0B0F1A";
inline constexpr auto BG_CARD      = "#111827";
inline constexpr auto BG_SIDEBAR   = "#0F1320";
inline constexpr auto BG_INPUT     = "#1A2035";
inline constexpr auto BG_HOVER     = "#1E293B";
inline constexpr auto BG_TOOLTIP   = "#1E293B";
inline constexpr auto BG_ELEVATED  = "#162031";

// ── Accents ──
inline constexpr auto PRIMARY       = "#2563EB";
inline constexpr auto PRIMARY_HOVER = "#3B82F6";
inline constexpr auto PRIMARY_LIGHT = "#3B82F6";
inline constexpr auto PRIMARY_DIM   = "rgba(37, 99, 235, 0.15)";
inline constexpr auto SUCCESS       = "#22C55E";
inline constexpr auto SUCCESS_BG    = "rgba(34, 197, 94, 0.12)";
inline constexpr auto ERROR_CLR     = "#EF4444";
inline constexpr auto ERROR_BG      = "rgba(239, 68, 68, 0.12)";
inline constexpr auto WARNING       = "#F59E0B";
inline constexpr auto WARNING_BG    = "rgba(245, 158, 11, 0.12)";

// ── Text ──
inline constexpr auto TEXT_PRIMARY   = "#FFFFFF";
inline constexpr auto TEXT_SECONDARY = "#E2E8F0";
inline constexpr auto TEXT_TERTIARY  = "#CBD5E1";
inline constexpr auto TEXT_DIM       = "#94A3B8";

// ── Metric colors (Mission Planner style) ──
inline constexpr auto METRIC_SPEED = "#FF9F43";
inline constexpr auto METRIC_GS    = "#2ED573";
inline constexpr auto METRIC_HDG   = "#FFFFFF";
inline constexpr auto METRIC_ALT   = "#FECA57";
inline constexpr auto METRIC_VS    = "#DCDDE1";
inline constexpr auto METRIC_WIND  = "#48DBFB";
inline constexpr auto METRIC_ATT   = "#C8A2FF";
inline constexpr auto METRIC_BAT   = "#FF6B6B";
inline constexpr auto METRIC_GPS   = "#2ED573";

// ── Borders ──
inline constexpr auto BORDER        = "#1E293B";
inline constexpr auto BORDER_LIGHT  = "#334155";
inline constexpr auto BORDER_SUBTLE = "#1A2332";

// ── Fonts ──
inline constexpr auto FONT_FAMILY = "Segoe UI";
inline constexpr auto FONT_MONO   = "Consolas";

/// Apply dark title bar on Windows 10/11 via DWM
inline void applyDarkTitlebar([[maybe_unused]] quintptr hwnd)
{
#ifdef _WIN32
    DWORD value = 1;
    DwmSetWindowAttribute(
        reinterpret_cast<HWND>(hwnd),
        20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
        &value, sizeof(value));
#endif
}

/// Full dark QSS stylesheet — load via QApplication::setStyleSheet()
QString stylesheet();

} // namespace vtol::theme
