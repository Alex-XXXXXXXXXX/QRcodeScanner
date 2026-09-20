#pragma once

#include "DecodeEngine.h"
#include "FrameSource.h"
#include "RoiTracker.h"

class FrameDecodeController
{
public:
    explicit FrameDecodeController(DecodeRecipe recipe = {});

    DecodeReport decode(const FramePacket& frame);
    void setRecipe(const DecodeRecipe& recipe);
    void resetTracking();
    QRect lastPredictedRoi() const;

private:
    DecodeEngine engine_;
    DecodeRecipe recipe_;
    RoiTracker tracker_;
    QString previousSuccessfulRoute_;
    QRect lastPredictedRoi_;
};
