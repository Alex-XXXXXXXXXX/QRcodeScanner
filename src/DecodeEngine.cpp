#include "DecodeEngine.h"

#include "DataMatrixGridRecoverer.h"
#include "ImagePreprocessor.h"
#include "ImageQualityAnalyzer.h"
#include "RoiDetector.h"
#include "RoutePredictor.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QThreadPool>
#include <QWaitCondition>

#include <ZXingCpp.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace {

enum class CandidateTransform {
    None,
    Contrast,
    Unsharp,
    ContrastUnsharp,
    Threshold,
    ContrastThreshold
};

struct Candidate
{
    QString name;
    QString route;
    QImage image;
    CandidateTransform transform = CandidateTransform::None;
    bool hardMode = false;
    QPolygonF sourceCorners;
    double resultScaleX = 1.0;
    double resultScaleY = 1.0;
};

struct CandidateResult
{
    DecodeAttempt attempt;
    QString route;
    QString text;
    QString format;
    QVector<QPointF> corners;
    QVector<QPointF> detectedCorners;
    QImage sourceImage;
};

struct AsyncState
{
    QMutex mutex;
    QWaitCondition ready;
    QVector<CandidateResult> results;
    std::atomic_bool cancelled{false};
    int remaining = 0;
};

struct GridAsyncState
{
    QMutex mutex;
    QWaitCondition ready;
    GridRecoveryResult result;
    bool done = false;
};

class FunctionRunnable final : public QRunnable
{
public:
    explicit FunctionRunnable(std::function<void()> function)
        : function_(std::move(function)) {}
    void run() override { function_(); }
private:
    std::function<void()> function_;
};

QThreadPool& decodePool()
{
    static QThreadPool* pool = [] {
        auto* instance = new QThreadPool;
        instance->setMaxThreadCount(2);
        instance->setExpiryTimeout(1000);
        return instance;
    }();
    return *pool;
}

bool payloadIsAllowed(const QString& text, const DecodeRecipe& recipe)
{
    if (recipe.payloadRegularExpression.isEmpty())
        return true;
    const QRegularExpression expression(recipe.payloadRegularExpression);
    return expression.isValid() && expression.match(text).hasMatch();
}

qint64 remainingMicroseconds(const QElapsedTimer& timer, int budgetMs)
{
    return std::max<qint64>(0, static_cast<qint64>(budgetMs) * 1000
        - timer.nsecsElapsed() / 1000);
}

QVector<QPointF> scaledPoints(const QVector<QPointF>& points,
                              double scaleX, double scaleY)
{
    QVector<QPointF> result;
    result.reserve(points.size());
    for (const QPointF& point : points)
        result.push_back(QPointF(point.x() * scaleX, point.y() * scaleY));
    return result;
}

QImage transformedCandidate(const QImage& image, CandidateTransform transform)
{
    switch (transform) {
    case CandidateTransform::Contrast:
        return ImagePreprocessor::normalizeContrast(image);
    case CandidateTransform::Unsharp:
        return ImagePreprocessor::unsharpMask(image);
    case CandidateTransform::ContrastUnsharp:
        return ImagePreprocessor::unsharpMask(
            ImagePreprocessor::normalizeContrast(image));
    case CandidateTransform::Threshold:
        return ImagePreprocessor::localThreshold(image);
    case CandidateTransform::ContrastThreshold:
        return ImagePreprocessor::localThreshold(
            ImagePreprocessor::normalizeContrast(image));
    case CandidateTransform::None:
        return image;
    }
    return image;
}

} // namespace

bool DecodeEngine::tryDecode(const QImage& input, bool hardMode,
                             QString& decodedText, QString& decodedFormat,
                             QString& detectionDiagnostic,
                             QVector<QPointF>& corners,
                             const DecodeRecipe& recipe)
{
    decodedText.clear();
    decodedFormat.clear();
    detectionDiagnostic.clear();
    corners.clear();
    const QImage gray = input.format() == QImage::Format_Grayscale8
        ? input : input.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return false;

    std::vector<ZXing::BarcodeFormat> enabledFormats;
    if (recipe.allowQrCode)
        enabledFormats.push_back(ZXing::BarcodeFormat::QRCode);
    if (recipe.allowDataMatrix)
        enabledFormats.push_back(ZXing::BarcodeFormat::DataMatrix);
    if (enabledFormats.empty()) {
        detectionDiagnostic = QStringLiteral("no barcode format enabled by recipe");
        return false;
    }
    ZXing::BarcodeFormats formats(std::move(enabledFormats));
    const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(),
                                ZXing::ImageFormat::Lum, gray.bytesPerLine());
    auto options = ZXing::ReaderOptions().formats(formats)
        .tryHarder(hardMode).tryRotate(hardMode)
        .tryInvert(hardMode && recipe.allowInverted).tryDownscale(hardMode)
        .returnErrors(hardMode).maxNumberOfSymbols(2);

    bool acceptedSymbol = false;
    const auto barcodes = ZXing::ReadBarcodes(view, options);
    for (const auto& barcode : barcodes) {
        QVector<QPointF> detectedCorners;
        for (const auto& point : barcode.position())
            detectedCorners.push_back(QPointF(point.x, point.y));
        if (!barcode.isValid()) {
            if (detectionDiagnostic.isEmpty()
                && barcode.format() != ZXing::BarcodeFormat::None) {
                detectionDiagnostic = QStringLiteral("%1：%2")
                    .arg(QString::fromStdString(ZXing::ToString(barcode.format())),
                         QString::fromStdString(ZXing::ToString(barcode.error())));
            }
            if (corners.isEmpty() && detectedCorners.size() == 4)
                corners = detectedCorners;
            continue;
        }
        const QString text = QString::fromUtf8(barcode.text().c_str());
        if (!payloadIsAllowed(text, recipe)) {
            detectionDiagnostic = QStringLiteral("payload validation rejected a decoded symbol");
            continue;
        }
        const QString decodedBarcodeFormat =
            QString::fromStdString(ZXing::ToString(barcode.format()));
        if (acceptedSymbol && text != decodedText) {
            decodedText.clear();
            decodedFormat.clear();
            corners.clear();
            detectionDiagnostic = QStringLiteral(
                "conflicting checksum-valid payloads in one candidate");
            return false;
        }
        if (acceptedSymbol)
            continue;
        decodedText = text;
        decodedFormat = decodedBarcodeFormat;
        corners = detectedCorners;
        acceptedSymbol = true;
    }
    return acceptedSymbol;
}

DecodeReport DecodeEngine::decode(const QImage& image) const
{
    DecodeRequest request;
    request.image = image;
    return decode(request);
}

DecodeReport DecodeEngine::decode(const DecodeRequest& request) const
{
    DecodeReport report;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const int requestedBudgetMs = std::clamp(request.recipe.timeBudgetMs, 1, 5000);
    // ZXing's individual decode call is synchronous and cannot be interrupted in
    // the middle. Keep a measured safety reserve so the cooperative scheduler can
    // still return before the caller's hard deadline after its last in-flight call.
    const int safetyReserveMs = std::clamp(requestedBudgetMs / 2, 12, 70);
    const int budgetMs = std::max(1, requestedBudgetMs - safetyReserveMs);
    if (request.image.isNull()) {
        report.status = DecodeStatus::InvalidInput;
        report.failureStage = FailureStage::Input;
        report.failureReason = QStringLiteral("输入图片为空或格式不受支持");
        return report;
    }

    QElapsedTimer stageTimer;
    stageTimer.start();
    constexpr int analysisMaximumDimension = 896;
    const QImage boundedInput = request.image.width() <= analysisMaximumDimension
            && request.image.height() <= analysisMaximumDimension
        ? request.image
        : request.image.scaled(analysisMaximumDimension, analysisMaximumDimension,
                               Qt::KeepAspectRatio, Qt::FastTransformation);
    const QImage gray = ImagePreprocessor::toGrayscale(boundedInput);
    const QImage decodeBase = gray;
    report.quality = ImageQualityAnalyzer::analyze(decodeBase);
    report.quality.width = request.image.width();
    report.quality.height = request.image.height();
    const RoutePlan routePlan = RoutePredictor().predict(report.quality, request.recipe);
    report.stageTimings.push_back({QStringLiteral("quality"), stageTimer.nsecsElapsed() / 1000});

    QString text, format, diagnostic;
    QVector<QPointF> points;
    stageTimer.restart();
    const bool fastSuccess = tryDecode(decodeBase, false,
                                       text, format, diagnostic, points, request.recipe);
    points = scaledPoints(points,
        request.image.width() / static_cast<double>(decodeBase.width()),
        request.image.height() / static_cast<double>(decodeBase.height()));
    report.attempts.push_back({QStringLiteral("原始灰度-快速"),
                               stageTimer.nsecsElapsed() / 1000, fastSuccess, diagnostic});
    report.stageTimings.push_back({QStringLiteral("L0"), stageTimer.nsecsElapsed() / 1000});
    if (diagnostic.contains(QStringLiteral("conflicting checksum-valid"),
                            Qt::CaseInsensitive)) {
        report.status = DecodeStatus::Conflict;
        report.failureStage = FailureStage::Validate;
        report.failureReason = QStringLiteral("同一图像包含冲突的有效码，已按安全策略拒读");
        report.totalMicroseconds = totalTimer.nsecsElapsed() / 1000;
        return report;
    }
    if (fastSuccess) {
        report.success = true;
        report.status = DecodeStatus::Success;
        report.text = text;
        report.format = format;
        report.route = QStringLiteral("L0 快速路径 / 原始灰度");
        report.corners = points;
        report.confidence = 1.0;
        report.totalMicroseconds = totalTimer.nsecsElapsed() / 1000;
        return report;
    }

    stageTimer.restart();
    QVector<RoiCandidate> rois;
    if (request.recipe.fixedQuadrilateral.size() == 4) {
        QRect cropRect = request.recipe.fixedQuadrilateral.boundingRect().toAlignedRect()
            .adjusted(-24, -24, 24, 24).intersected(request.image.rect());
        if (cropRect.isValid()) {
            const QImage crop = ImagePreprocessor::toGrayscale(request.image.copy(cropRect));
            QPolygonF localCorners;
            for (const QPointF& point : request.recipe.fixedQuadrilateral)
                localCorners << point - cropRect.topLeft();
            RoiCandidate rectified = RoiDetector().rectify(crop, localCorners, 3.0);
            rectified.corners = request.recipe.fixedQuadrilateral;
            if (!rectified.rectifiedImage.isNull())
                rois.push_back(std::move(rectified));
        }
    } else if (request.recipe.fixedRoi.isValid()) {
        const QRect fixed = request.recipe.fixedRoi.intersected(request.image.rect());
        if (fixed.isValid()) {
            QPolygonF corners;
            corners << fixed.topLeft() << fixed.topRight()
                    << fixed.bottomRight() << fixed.bottomLeft();
            const QImage roiGray = ImagePreprocessor::toGrayscale(request.image.copy(fixed));
            rois.push_back({ImagePreprocessor::fitForDecode(roiGray, 1200), corners, 1.0});
        }
    } else {
        rois = RoiDetector().detect(gray, request.recipe);
        const double scaleX = request.image.width() / static_cast<double>(gray.width());
        const double scaleY = request.image.height() / static_cast<double>(gray.height());
        if (scaleX != 1.0 || scaleY != 1.0) {
            for (RoiCandidate& roi : rois) {
                for (QPointF& point : roi.corners) {
                    point.setX(point.x() * scaleX);
                    point.setY(point.y() * scaleY);
                }
            }
        }
    }
    for (const RoiCandidate& roi : rois)
        report.candidateCorners.push_back(roi.corners);
    report.stageTimings.push_back({QStringLiteral("ROI"), stageTimer.nsecsElapsed() / 1000});
    QVector<Candidate> candidates;
    for (int i = 0; i < rois.size(); ++i) {
        candidates.push_back({QStringLiteral("ROI-%1").arg(i + 1),
                              QStringLiteral("L1 ROI"), rois[i].rectifiedImage,
                              CandidateTransform::None, true, rois[i].corners});
        if (routePlan.enableContrast) {
            candidates.push_back({QStringLiteral("ROI-%1-对比度").arg(i + 1),
                                  QStringLiteral("L2 ROI 增强"), rois[i].rectifiedImage,
                                  CandidateTransform::Contrast, true, rois[i].corners});
        }
        candidates.push_back({QStringLiteral("ROI-%1-反锐化").arg(i + 1),
                              QStringLiteral("L2 ROI 增强"), rois[i].rectifiedImage,
                              routePlan.enableContrast
                                  ? CandidateTransform::ContrastUnsharp
                                  : CandidateTransform::Unsharp,
                              true, rois[i].corners});
        if (routePlan.enableThreshold) {
            candidates.push_back({QStringLiteral("ROI-%1-局部阈值").arg(i + 1),
                                  QStringLiteral("L2 ROI 二值化"), rois[i].rectifiedImage,
                                  routePlan.enableContrast
                                      ? CandidateTransform::ContrastThreshold
                                      : CandidateTransform::Threshold,
                                  true, rois[i].corners});
        }
    }
    candidates.push_back({QStringLiteral("原始灰度-困难"),
                          QStringLiteral("L1 标准回退"), decodeBase,
                          CandidateTransform::None, true, {},
                          request.image.width() / static_cast<double>(decodeBase.width()),
                          request.image.height() / static_cast<double>(decodeBase.height())});
    const bool hasLockedRoi = !rois.isEmpty()
        && (request.recipe.fixedRoi.isValid()
            || request.recipe.fixedQuadrilateral.size() == 4);
    if (!hasLockedRoi) {
        if (routePlan.enableContrast)
            candidates.push_back({QStringLiteral("对比度拉伸"), QStringLiteral("L2 增强"),
                                  decodeBase, CandidateTransform::Contrast, true, {},
                                  request.image.width() / static_cast<double>(decodeBase.width()),
                                  request.image.height() / static_cast<double>(decodeBase.height())});
        candidates.push_back({QStringLiteral("反锐化增强"), QStringLiteral("L2 增强"),
                              decodeBase, routePlan.enableContrast
                                  ? CandidateTransform::ContrastUnsharp
                                  : CandidateTransform::Unsharp,
                              true, {},
                              request.image.width() / static_cast<double>(decodeBase.width()),
                              request.image.height() / static_cast<double>(decodeBase.height())});
        if (routePlan.enableThreshold)
            candidates.push_back({QStringLiteral("局部自适应阈值"), QStringLiteral("L2 二值化"),
                                  decodeBase, routePlan.enableContrast
                                      ? CandidateTransform::ContrastThreshold
                                      : CandidateTransform::Threshold,
                                  true, {},
                                  request.image.width() / static_cast<double>(decodeBase.width()),
                                  request.image.height() / static_cast<double>(decodeBase.height())});
    }

    auto state = std::make_shared<AsyncState>();
    state->remaining = candidates.size();
    for (const Candidate& candidate : candidates) {
        auto task = [state, candidate, recipe = request.recipe] {
            CandidateResult result;
            result.attempt.name = candidate.name;
            result.route = candidate.route;
            if (!state->cancelled.load(std::memory_order_relaxed)) {
                QElapsedTimer timer;
                timer.start();
                const QImage workingImage = transformedCandidate(
                    candidate.image, candidate.transform);
                result.sourceImage = workingImage;
                QString diagnostic;
                QVector<QPointF> localCorners;
                if (!state->cancelled.load(std::memory_order_relaxed)) {
                    result.attempt.success = DecodeEngine::tryDecode(
                        workingImage, candidate.hardMode, result.text, result.format,
                        diagnostic, localCorners, recipe);
                }
                result.attempt.elapsedMicroseconds = timer.nsecsElapsed() / 1000;
                result.attempt.diagnostic = diagnostic;
                result.detectedCorners = localCorners;
                if (!candidate.sourceCorners.isEmpty()) {
                    for (const QPointF& p : candidate.sourceCorners)
                        result.corners.push_back(p);
                } else {
                    result.corners = scaledPoints(localCorners,
                        candidate.resultScaleX, candidate.resultScaleY);
                }
            }
            QMutexLocker locker(&state->mutex);
            state->results.push_back(result);
            --state->remaining;
            state->ready.wakeAll();
        };
        decodePool().start(new FunctionRunnable(std::move(task)));
    }

    QVector<CandidateResult> completed;
    bool receivedSuccess = false;
    qint64 successAtUs = 0;
    const qint64 reserveForL3Us = routePlan.enableGridRecovery && !rois.isEmpty()
        ? std::min<qint64>(static_cast<qint64>(request.recipe.l3BudgetMs) * 1000,
                           remainingMicroseconds(totalTimer, budgetMs) / 2)
        : 0;
    const qint64 candidatePhaseEndUs = static_cast<qint64>(budgetMs) * 1000 - reserveForL3Us;
    while (totalTimer.nsecsElapsed() / 1000 < candidatePhaseEndUs) {
        QMutexLocker locker(&state->mutex);
        if (state->results.isEmpty() && state->remaining > 0) {
            const qint64 waitUs = receivedSuccess
                ? std::min<qint64>(5000 - (totalTimer.nsecsElapsed() / 1000 - successAtUs),
                                   remainingMicroseconds(totalTimer, budgetMs))
                : std::max<qint64>(0, candidatePhaseEndUs - totalTimer.nsecsElapsed() / 1000);
            if (waitUs > 0)
                state->ready.wait(&state->mutex, static_cast<unsigned long>((waitUs + 999) / 1000));
        }
        completed += state->results;
        state->results.clear();
        for (const CandidateResult& value : completed) {
            if (value.attempt.success && !receivedSuccess) {
                receivedSuccess = true;
                successAtUs = totalTimer.nsecsElapsed() / 1000;
                state->cancelled.store(true, std::memory_order_relaxed);
            }
        }
        if (state->remaining == 0 || (receivedSuccess
            && totalTimer.nsecsElapsed() / 1000 - successAtUs >= 5000))
            break;
    }
    state->cancelled.store(true, std::memory_order_relaxed);

    QString strongestDiagnostic;
    QVector<CandidateResult> successful;
    bool candidateConflict = false;
    for (const CandidateResult& value : completed) {
        report.attempts.push_back(value.attempt);
        if (!value.attempt.diagnostic.isEmpty())
            strongestDiagnostic = value.attempt.diagnostic;
        candidateConflict = candidateConflict
            || value.attempt.diagnostic.contains(
                QStringLiteral("conflicting checksum-valid"), Qt::CaseInsensitive);
        if (value.attempt.success)
            successful.push_back(value);
    }
    if (strongestDiagnostic.isEmpty() && !rois.isEmpty())
        strongestDiagnostic = QStringLiteral(
            "located ROI; sampling did not produce a checksum-valid payload");

    QVector<RoiCandidate> l3Rois = rois;
    for (const CandidateResult& value : completed) {
        if (value.detectedCorners.size() == 4
            && value.attempt.diagnostic.contains(QStringLiteral("Data Matrix"),
                                                  Qt::CaseInsensitive)) {
            QPolygonF polygon;
            for (const QPointF& point : value.detectedCorners)
                polygon << point;
            RoiCandidate hinted = RoiDetector().rectify(value.sourceImage, polygon, 2.0);
            if (!hinted.rectifiedImage.isNull()) {
                // detectedCorners belongs to the normalized candidate image
                // (typically 320x320), while DecodeReport coordinates must
                // always be expressed in the original input-image space.
                // The enclosing candidate already carries that mapping.
                hinted.corners.clear();
                for (const QPointF& point : value.corners)
                    hinted.corners << point;
                l3Rois.prepend(std::move(hinted));
            }
        }
    }
    if (candidateConflict) {
        report.status = DecodeStatus::Conflict;
        report.failureStage = FailureStage::Validate;
        report.failureReason = QStringLiteral("同一候选包含冲突的有效码，已按安全策略拒读");
    } else if (!successful.isEmpty()) {
        const QString acceptedText = successful.first().text;
        const bool conflict = std::any_of(successful.begin(), successful.end(),
            [&](const auto& value) { return value.text != acceptedText; });
        if (conflict) {
            report.status = DecodeStatus::Conflict;
            report.failureStage = FailureStage::Validate;
            report.failureReason = QStringLiteral("候选路径输出冲突，已按安全策略拒读");
        } else {
            const CandidateResult& accepted = successful.first();
            report.success = true;
            report.status = DecodeStatus::Success;
            report.text = accepted.text;
            report.format = accepted.format;
            report.route = accepted.route + QStringLiteral(" / ") + accepted.attempt.name;
            report.corners = accepted.corners;
            report.confidence = 1.0;
        }
    }

    if (!report.success && report.status != DecodeStatus::Conflict
        && routePlan.enableGridRecovery && !l3Rois.isEmpty()
        && remainingMicroseconds(totalTimer, budgetMs) > 1000) {
        stageTimer.restart();
        for (int i = 0; i < l3Rois.size(); ++i) {
            const qint64 remainingUs = remainingMicroseconds(totalTimer, budgetMs);
            if (remainingUs <= 1000)
                break;
            auto gridState = std::make_shared<GridAsyncState>();
            const QImage gridImage = l3Rois[i].rectifiedImage;
            const DecodeRecipe gridRecipe = request.recipe;
            decodePool().start(new FunctionRunnable(
                [gridState, gridImage, gridRecipe, remainingUs] {
                    GridRecoveryResult recovered = DataMatrixGridRecoverer().recover(
                        gridImage, gridRecipe, remainingUs);
                    QMutexLocker locker(&gridState->mutex);
                    gridState->result = std::move(recovered);
                    gridState->done = true;
                    gridState->ready.wakeAll();
                }), 1);
            bool gridFinished = false;
            GridRecoveryResult recovered;
            {
                QMutexLocker locker(&gridState->mutex);
                while (!gridState->done) {
                    const qint64 waitUs = remainingMicroseconds(totalTimer, budgetMs);
                    if (waitUs <= 0)
                        break;
                    gridState->ready.wait(&gridState->mutex,
                        static_cast<unsigned long>((waitUs + 999) / 1000));
                }
                gridFinished = gridState->done;
                if (gridFinished)
                    recovered = gridState->result;
            }
            if (!gridFinished) {
                strongestDiagnostic = QStringLiteral("L3 recovery reached request deadline");
                break;
            }
            report.attempts.push_back({QStringLiteral("L3-网格恢复-%1").arg(i + 1),
                stageTimer.nsecsElapsed() / 1000, recovered.success, recovered.diagnostic});
            report.estimatedRows = recovered.rows;
            report.estimatedColumns = recovered.columns;
            report.confidence = recovered.confidence;
            if (recovered.success && payloadIsAllowed(recovered.text, request.recipe)) {
                report.success = true;
                report.status = DecodeStatus::Success;
                report.text = recovered.text;
                report.format = QStringLiteral("Data Matrix");
                report.route = QStringLiteral("L3 ECC200 网格恢复");
                for (const QPointF& p : l3Rois[i].corners)
                    report.corners.push_back(p);
                break;
            }
            if (!recovered.diagnostic.isEmpty())
                strongestDiagnostic = recovered.diagnostic;
            if (remainingMicroseconds(totalTimer, budgetMs) <= 0)
                break;
        }
        report.stageTimings.push_back({QStringLiteral("L3"), stageTimer.nsecsElapsed() / 1000});
    }

    report.totalMicroseconds = totalTimer.nsecsElapsed() / 1000;
    if (!report.success && report.status != DecodeStatus::Conflict) {
        report.deadlineExceeded = report.totalMicroseconds >= static_cast<qint64>(budgetMs) * 1000;
        report.status = report.deadlineExceeded ? DecodeStatus::Timeout : DecodeStatus::NoRead;
        report.failureStage = report.deadlineExceeded ? FailureStage::Deadline
            : strongestDiagnostic.contains(QStringLiteral("Checksum"), Qt::CaseInsensitive)
                ? FailureStage::Correct
                : strongestDiagnostic.isEmpty() ? FailureStage::Locate : FailureStage::Sample;
        report.failureReason = strongestDiagnostic.isEmpty()
            ? QStringLiteral("未定位到可信二维码候选")
            : QStringLiteral("已定位候选码，但未通过网格采样或纠错：%1").arg(strongestDiagnostic);
    }
    return report;
}
