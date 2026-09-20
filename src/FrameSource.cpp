#include "FrameSource.h"

#include "ImageQualityAnalyzer.h"

#include <QDateTime>
#include <QImageReader>

#include <algorithm>
#include <utility>

LocalFileFrameSource::LocalFileFrameSource(QString filePath)
    : filePath_(std::move(filePath))
{
}

bool LocalFileFrameSource::open(QString* errorMessage)
{
    QImageReader reader(filePath_);
    reader.setAutoTransform(true);
    image_ = reader.read();
    delivered_ = false;
    if (!image_.isNull())
        return true;
    if (errorMessage)
        *errorMessage = reader.errorString();
    return false;
}

void LocalFileFrameSource::close()
{
    image_ = {};
    delivered_ = false;
}

bool LocalFileFrameSource::nextFrame(FramePacket& packet, int, QString* errorMessage)
{
    if (image_.isNull() || delivered_) {
        if (errorMessage)
            *errorMessage = image_.isNull() ? QStringLiteral("frame source is not open")
                                            : QStringLiteral("end of local source");
        return false;
    }
    packet.image = image_;
    packet.sequence = 1;
    packet.timestampMicroseconds = QDateTime::currentMSecsSinceEpoch() * 1000;
    packet.sourceId = filePath_;
    delivered_ = true;
    return true;
}

IndustrialCameraFrameSource::IndustrialCameraFrameSource(
    IIndustrialCameraBackend& backend, CameraAcquisitionRecipe recipe)
    : backend_(backend), recipe_(std::move(recipe))
{
    recipe_.maximumFramesPerTrigger = std::clamp(recipe_.maximumFramesPerTrigger, 1, 2);
    if (recipe_.exposureMicroseconds.isEmpty())
        recipe_.exposureMicroseconds.push_back(500.0);
}

bool IndustrialCameraFrameSource::open(QString* errorMessage)
{
    sequence_ = 0;
    if (!backend_.open(errorMessage))
        return false;
    if (backend_.configure(recipe_, errorMessage))
        return true;
    backend_.close();
    return false;
}

void IndustrialCameraFrameSource::close()
{
    backend_.close();
}

bool IndustrialCameraFrameSource::nextFrame(FramePacket& packet, int timeoutMs,
                                             QString* errorMessage)
{
    quint64 triggerId = 0;
    if (!backend_.waitForTrigger(triggerId, timeoutMs, errorMessage))
        return false;

    FramePacket best;
    double bestScore = -1.0;
    const int frameCount = std::min(recipe_.maximumFramesPerTrigger,
                                    recipe_.exposureMicroseconds.size());
    for (int i = 0; i < frameCount; ++i) {
        FramePacket candidate;
        const double exposure = recipe_.exposureMicroseconds[i];
        if (!backend_.capture(exposure, recipe_.lightingRecipeId, candidate, errorMessage))
            continue;
        const ImageQuality quality = ImageQualityAnalyzer::analyze(candidate.image);
        const double exposurePenalty = std::min(0.8, quality.saturatedRatio * 3.0);
        const double score = quality.sharpness * (1.0 - exposurePenalty)
            + quality.contrast * 0.15;
        if (score > bestScore) {
            bestScore = score;
            best = std::move(candidate);
        }
    }
    if (best.image.isNull()) {
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("camera returned no valid exposure frame");
        return false;
    }
    best.sequence = ++sequence_;
    best.triggerId = triggerId;
    if (best.timestampMicroseconds <= 0)
        best.timestampMicroseconds = QDateTime::currentMSecsSinceEpoch() * 1000;
    packet = std::move(best);
    return true;
}
