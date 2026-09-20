#include "RoutePredictor.h"
#include "RouteModelParameters.h"

RoutePlan RoutePredictor::predict(const ImageQuality& quality,
                                  const DecodeRecipe& recipe) const
{
    RoutePlan plan;
    const double imageArea = static_cast<double>(quality.width) * quality.height;
    double roiFraction = 1.0;
    if (recipe.fixedRoi.isValid() && imageArea > 0.0)
        roiFraction = recipe.fixedRoi.width() * recipe.fixedRoi.height() / imageArea;
    else if (recipe.fixedQuadrilateral.size() == 4 && imageArea > 0.0) {
        const QRectF bounds = recipe.fixedQuadrilateral.boundingRect();
        roiFraction = bounds.width() * bounds.height() / imageArea;
    }
    const bool previousHardPath = recipe.previousSuccessfulRoute.contains(
        QStringLiteral("L2"), Qt::CaseInsensitive)
        || recipe.previousSuccessfulRoute.contains(
            QStringLiteral("L3"), Qt::CaseInsensitive);
    plan.preferRoi = recipe.fixedRoi.isValid()
        || !recipe.fixedQuadrilateral.isEmpty()
        || imageArea > RouteModelParameters::LargeImagePixels
        || roiFraction < RouteModelParameters::SmallRoiFraction || previousHardPath;
    plan.enableContrast = quality.contrast < RouteModelParameters::LowContrast
        || quality.saturatedRatio > RouteModelParameters::HighSaturationRatio;
    plan.enableThreshold = quality.contrast < RouteModelParameters::ThresholdContrast
        || quality.meanLuminance < RouteModelParameters::DarkMeanLuminance
        || quality.meanLuminance > RouteModelParameters::BrightMeanLuminance;
    plan.enableGridRecovery = recipe.allowDataMatrix
        && (quality.sharpness < RouteModelParameters::LowSharpness
            || quality.edgeConsistency < RouteModelParameters::LowEdgeConsistency
            || recipe.previousSuccessfulRoute.contains(
                QStringLiteral("L3"), Qt::CaseInsensitive));
    return plan;
}
