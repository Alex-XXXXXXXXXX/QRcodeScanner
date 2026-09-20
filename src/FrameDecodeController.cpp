#include "FrameDecodeController.h"

#include <utility>

FrameDecodeController::FrameDecodeController(DecodeRecipe recipe)
    : recipe_(std::move(recipe))
{
}

DecodeReport FrameDecodeController::decode(const FramePacket& frame)
{
    DecodeRequest request;
    request.image = frame.image;
    request.sequence = frame.sequence;
    request.recipe = recipe_;
    request.recipe.previousSuccessfulRoute = previousSuccessfulRoute_;
    lastPredictedRoi_ = {};
    if (!request.recipe.fixedRoi.isValid()
        && request.recipe.fixedQuadrilateral.size() != 4) {
        lastPredictedRoi_ = tracker_.predict(frame.image.size());
        if (lastPredictedRoi_.isValid())
            request.recipe.fixedRoi = lastPredictedRoi_;
    }

    DecodeReport report = engine_.decode(request);
    if (report.success) {
        QPolygonF corners;
        for (const QPointF& point : report.corners)
            corners << point;
        tracker_.update(corners);
        previousSuccessfulRoute_ = report.route;
    }
    return report;
}

void FrameDecodeController::setRecipe(const DecodeRecipe& recipe)
{
    recipe_ = recipe;
}

void FrameDecodeController::resetTracking()
{
    tracker_.reset();
    previousSuccessfulRoute_.clear();
    lastPredictedRoi_ = {};
}

QRect FrameDecodeController::lastPredictedRoi() const
{
    return lastPredictedRoi_;
}
