#pragma once

// Reviewed fixed decision-tree thresholds. The offline trainer exports the
// candidate tree as JSON; only validated values are copied here, so deployment
// adds no model loader or runtime dependency.
namespace RouteModelParameters {

inline constexpr double LargeImagePixels = 2000000.0;
inline constexpr double SmallRoiFraction = 0.35;
inline constexpr double LowContrast = 175.0;
inline constexpr double ThresholdContrast = 135.0;
inline constexpr double HighSaturationRatio = 0.08;
inline constexpr double DarkMeanLuminance = 75.0;
inline constexpr double BrightMeanLuminance = 205.0;
inline constexpr double LowSharpness = 38.0;
inline constexpr double LowEdgeConsistency = 0.62;

} // namespace RouteModelParameters
