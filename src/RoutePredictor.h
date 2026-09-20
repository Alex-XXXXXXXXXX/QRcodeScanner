#pragma once

#include "DecodeTypes.h"

struct RoutePlan
{
    bool preferRoi = false;
    bool enableContrast = true;
    bool enableThreshold = true;
    bool enableGridRecovery = true;
};

class RoutePredictor
{
public:
    RoutePlan predict(const ImageQuality& quality, const DecodeRecipe& recipe) const;
};
