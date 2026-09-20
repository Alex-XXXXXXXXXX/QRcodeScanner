#include "DataMatrixGridRecoverer.h"
#include "DecodeEngine.h"
#include "FrameDecodeController.h"
#include "FrameSource.h"
#include "RoiDetector.h"
#include "RoiTracker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTransform>

#include <ZXingCpp.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

QImage createSymbol(const QString& text, ZXing::BarcodeFormat format, int scale)
{
    const auto barcode = ZXing::CreateBarcodeFromText(text.toStdString(), format);
    const auto image = ZXing::WriteBarcodeToImage(
        barcode, ZXing::WriterOptions().scale(scale).addQuietZones(true));
    return QImage(image.data(), image.width(), image.height(), image.width(),
                  QImage::Format_Grayscale8).copy();
}

QImage createRectifiedDataMatrix(const QString& text, bool forceSquare,
                                 int& rows, int& columns,
                                 QImage& originalModules)
{
    const auto barcode = ZXing::CreateBarcodeFromText(
        text.toStdString(),
        ZXing::CreatorOptions(ZXing::BarcodeFormat::DataMatrix,
                              forceSquare ? "forceSquare" : ""));
    const auto generated = ZXing::WriteBarcodeToImage(
        barcode, ZXing::WriterOptions().scale(1).addQuietZones(false));
    rows = generated.height();
    columns = generated.width();
    originalModules = QImage(columns, rows, QImage::Format_Grayscale8);
    originalModules.fill(255);
    QImage rectified(832, 832, QImage::Format_Grayscale8);
    rectified.fill(255);

    // A scale-1 symbol may have a scanline width that is not 32-bit aligned.
    // Copy it row-by-row into Qt-owned storage, then rasterize each module with
    // exact integer boundaries. This keeps the fixture independent of QImage's
    // external-buffer alignment and scaling rules.
    for (int row = 0; row < generated.height(); ++row) {
        const uchar* source = generated.data() + row * generated.rowStride();
        uchar* moduleRow = originalModules.scanLine(row);
        std::copy_n(source, columns, moduleRow);
        const int y0 = 32 + row * 768 / generated.height();
        const int y1 = 32 + (row + 1) * 768 / generated.height();
        for (int column = 0; column < columns; ++column) {
            const int x0 = 32 + column * 768 / columns;
            const int x1 = 32 + (column + 1) * 768 / columns;
            for (int y = y0; y < y1; ++y)
                std::fill(rectified.scanLine(y) + x0,
                          rectified.scanLine(y) + x1, source[column]);
        }
    }
    return rectified;
}

QImage addRegionBoundaryWarp(const QImage& source, double splitX, double splitY)
{
    QImage output(source.size(), QImage::Format_Grayscale8);
    output.fill(255);
    constexpr int start = 32;
    constexpr int extent = 768;
    for (int y = start; y < start + extent; ++y) {
        const double destinationV = (y - start + 0.5) / extent;
        const double sourceV = destinationV < splitY
            ? destinationV * 0.5 / splitY
            : 0.5 + (destinationV - splitY) * 0.5 / (1.0 - splitY);
        const int sourceY = std::clamp(start + static_cast<int>(sourceV * extent),
                                       start, start + extent - 1);
        uchar* destination = output.scanLine(y);
        const uchar* sourceRow = source.constScanLine(sourceY);
        for (int x = start; x < start + extent; ++x) {
            const double destinationU = (x - start + 0.5) / extent;
            const double sourceU = destinationU < splitX
                ? destinationU * 0.5 / splitX
                : 0.5 + (destinationU - splitX) * 0.5 / (1.0 - splitX);
            const int sourceX = std::clamp(start + static_cast<int>(sourceU * extent),
                                           start, start + extent - 1);
            destination[x] = sourceRow[sourceX];
        }
    }
    return output;
}

QImage addHorizontalBow(const QImage& source, double modulePitch)
{
    QImage output(source.size(), QImage::Format_Grayscale8);
    output.fill(255);
    for (int y = 0; y < source.height(); ++y) {
        const double normalized = y / static_cast<double>(source.height() - 1) - 0.5;
        const int shift = qRound(modulePitch * 0.5
            * std::max(0.0, 1.0 - 4.0 * normalized * normalized));
        const uchar* input = source.constScanLine(y);
        uchar* destination = output.scanLine(y);
        if (shift < source.width())
            std::copy_n(input, source.width() - shift, destination + shift);
    }
    return output;
}

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

class MockCameraBackend final : public IIndustrialCameraBackend
{
public:
    bool open(QString*) override
    {
        opened = true;
        return true;
    }

    void close() override { opened = false; }

    bool configure(const CameraAcquisitionRecipe& recipe, QString*) override
    {
        configured = true;
        configuredRecipe = recipe;
        return opened;
    }

    bool waitForTrigger(quint64& triggerId, int, QString*) override
    {
        if (!opened)
            return false;
        triggerId = 42;
        return true;
    }

    bool capture(double exposureMicroseconds, const QString& lightingRecipeId,
                 FramePacket& packet, QString*) override
    {
        ++captureCount;
        packet.image = captureCount == 1 ? saturated : detailed;
        packet.exposureMicroseconds = exposureMicroseconds;
        packet.sourceId = lightingRecipeId;
        return true;
    }

    QImage saturated;
    QImage detailed;
    int captureCount = 0;
    bool opened = false;
    bool configured = false;
    CameraAcquisitionRecipe configuredRecipe;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (!require(DataMatrixGridRecoverer::isLegalEcc200Dimension(48, 48),
                 "48x48 must be a legal ECC200 dimension")
        || !require(DataMatrixGridRecoverer::isLegalEcc200Dimension(16, 48),
                    "16x48 must be a legal rectangular ECC200 dimension")
        || !require(!DataMatrixGridRecoverer::isLegalEcc200Dimension(49, 49),
                    "49x49 must be rejected")) {
        return 1;
    }
    bool expectedBlack = false;
    if (!require(DataMatrixGridRecoverer::expectedFinderModule(
                     48, 48, 0, 0, expectedBlack) && expectedBlack,
                 "ECC200 top-left finder module must be black")
        || !require(DataMatrixGridRecoverer::expectedFinderModule(
                        48, 48, 0, 1, expectedBlack) && !expectedBlack,
                    "ECC200 top clock edge parity is invalid")
        || !require(DataMatrixGridRecoverer::expectedFinderModule(
                        48, 48, 1, 23, expectedBlack) && expectedBlack,
                    "ECC200 right clock edge parity is invalid")
        || !require(DataMatrixGridRecoverer::expectedFinderModule(
                        48, 48, 24, 1, expectedBlack) && !expectedBlack,
                    "ECC200 internal region clock edge is invalid")
        || !require(!DataMatrixGridRecoverer::expectedFinderModule(
                        48, 48, 1, 1, expectedBlack),
                    "ECC200 data module must not be reported as a finder module")) {
        return 17;
    }

    const QString gridPayload = QStringLiteral("L3-GRID-RECOVERY-2026");
    int gridRows = 0;
    int gridColumns = 0;
    QImage originalModules;
    const QImage rectified = createRectifiedDataMatrix(
        gridPayload, true, gridRows, gridColumns, originalModules);
    const int gridDimension = gridColumns;
    if (!require(gridRows == gridColumns, "forced-square L3 fixture is not square"))
        return 18;
    int sampledDifferences = 0;
    for (int row = 0; row < gridDimension; ++row) {
        for (int column = 0; column < gridDimension; ++column) {
            const int x = qRound(32.0 + (column + 0.5) * 768.0 / gridDimension);
            const int y = qRound(32.0 + (row + 0.5) * 768.0 / gridDimension);
            const uchar sampledValue = rectified.constScanLine(y)[x];
            const uchar originalValue = originalModules.constScanLine(row)[column];
            const bool sampledBlack = sampledValue < 128;
            const bool originalBlack = originalValue < 128;
            sampledDifferences += sampledBlack != originalBlack;
        }
    }
    if (!require(sampledDifferences == 0,
                 "normalized L3 fixture changed module values")) {
        return 10;
    }
    DecodeRecipe gridRecipe;
    gridRecipe.allowQrCode = false;
    gridRecipe.expectedDataMatrixRows = gridDimension;
    gridRecipe.expectedDataMatrixColumns = gridDimension;
    gridRecipe.fixedQuadrilateral << QPointF(0, 0) << QPointF(1, 0)
                                  << QPointF(1, 1) << QPointF(0, 1);
    DecodeRequest normalizedRequest;
    normalizedRequest.image = rectified;
    normalizedRequest.recipe.allowQrCode = false;
    const DecodeReport normalizedReport = DecodeEngine().decode(normalizedRequest);
    if (!require(normalizedReport.success && normalizedReport.text == gridPayload,
                 "ZXing baseline could not decode the normalized L3 fixture")) {
        return 9;
    }
    const GridRecoveryResult cleanGrid = DataMatrixGridRecoverer().recover(
        rectified, gridRecipe, 500000);
    if (!cleanGrid.success) {
        std::cerr << "L3 clean diagnostic: dimension=" << gridDimension
                  << " estimated=" << cleanGrid.rows << 'x' << cleanGrid.columns
                  << " confidence=" << cleanGrid.confidence
                  << " diagnostic=" << cleanGrid.diagnostic.toStdString() << '\n';
    }
    if (!require(cleanGrid.success && cleanGrid.text == gridPayload,
                 "clean normalized L3 grid recovery failed")) {
        return 7;
    }
    if (!require(cleanGrid.confidence > 0.20,
                 "clean L3 module confidence is unexpectedly low")) {
        return 19;
    }
    const GridRecoveryResult curvedGrid = DataMatrixGridRecoverer().recover(
        addHorizontalBow(rectified, 768.0 / gridDimension), gridRecipe, 500000);
    if (!require(curvedGrid.success && curvedGrid.text == gridPayload,
                 "curved normalized L3 grid recovery failed")) {
        return 8;
    }
    if (!require(curvedGrid.confidence > 0.10,
                 "curved L3 module confidence is unexpectedly low")) {
        return 20;
    }

    int rectangularRows = 0;
    int rectangularColumns = 0;
    QImage rectangularModules;
    const QImage rectangular = createRectifiedDataMatrix(
        gridPayload, false, rectangularRows, rectangularColumns,
        rectangularModules);
    if (!require(rectangularRows != rectangularColumns
                 && DataMatrixGridRecoverer::isLegalEcc200Dimension(
                     rectangularRows, rectangularColumns),
                 "writer did not produce a legal rectangular ECC200 fixture")) {
        return 21;
    }
    DecodeRecipe rectangularRecipe = gridRecipe;
    rectangularRecipe.expectedDataMatrixRows = rectangularRows;
    rectangularRecipe.expectedDataMatrixColumns = rectangularColumns;
    const GridRecoveryResult rectangularGrid = DataMatrixGridRecoverer().recover(
        rectangular, rectangularRecipe, 500000);
    if (!require(rectangularGrid.success && rectangularGrid.text == gridPayload
                 && rectangularGrid.rows == rectangularRows
                 && rectangularGrid.columns == rectangularColumns,
                 "rectangular ECC200 L3 recovery failed")) {
        std::cerr << "rectangular=" << rectangularRows << 'x' << rectangularColumns
                  << " diagnostic=" << rectangularGrid.diagnostic.toStdString() << '\n';
        return 22;
    }

    QString multiRegionPayload;
    const QString alphabet = QStringLiteral("aB3-dE6_fG9.hJ2:kL5/mN8+pQ1=rS4?tU7!vW0");
    for (int index = 0; index < 120; ++index)
        multiRegionPayload += alphabet[index % alphabet.size()];
    int multiRows = 0;
    int multiColumns = 0;
    QImage multiModules;
    const QImage multiRegion = createRectifiedDataMatrix(
        multiRegionPayload, true, multiRows, multiColumns, multiModules);
    if (!require(multiRows == multiColumns && multiRows >= 32 && multiRows <= 52,
                 "multi-region ECC200 fixture is outside the two-region range")) {
        std::cerr << "multi-region fixture=" << multiRows << 'x' << multiColumns << '\n';
        return 23;
    }
    DecodeRecipe multiRecipe = gridRecipe;
    multiRecipe.expectedDataMatrixRows = multiRows;
    multiRecipe.expectedDataMatrixColumns = multiColumns;
    const GridRecoveryResult multiGrid = DataMatrixGridRecoverer().recover(
        addRegionBoundaryWarp(multiRegion, 0.518, 0.482), multiRecipe, 700000);
    if (!require(multiGrid.success && multiGrid.text == multiRegionPayload,
                 "multi-region nonlinear grid recovery failed")) {
        std::cerr << "multi-region=" << multiRows << 'x' << multiColumns
                  << " diagnostic=" << multiGrid.diagnostic.toStdString() << '\n';
        return 24;
    }

    QImage perspectiveCanvas(1200, 1000, QImage::Format_Grayscale8);
    perspectiveCanvas.fill(240);
    const QPolygonF sourceSymbol({QPointF(32, 32), QPointF(800, 32),
                                  QPointF(800, 800), QPointF(32, 800)});
    const QPolygonF targetSymbol({QPointF(170, 120), QPointF(1010, 185),
                                  QPointF(925, 900), QPointF(115, 815)});
    QTransform perspectiveTransform;
    if (!require(QTransform::quadToQuad(sourceSymbol, targetSymbol,
                                        perspectiveTransform),
                 "could not construct perspective fixture transform")) {
        return 25;
    }
    {
        QPainter painter(&perspectiveCanvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setWorldTransform(perspectiveTransform);
        painter.drawImage(QPointF(0, 0), rectified);
    }
    const RoiCandidate perspectiveRoi = RoiDetector().rectify(
        perspectiveCanvas, targetSymbol, 1.0);
    const GridRecoveryResult perspectiveGrid = DataMatrixGridRecoverer().recover(
        perspectiveRoi.rectifiedImage, gridRecipe, 500000);
    if (!require(perspectiveGrid.success && perspectiveGrid.text == gridPayload,
                 "perspective mapping plus L3 recovery failed")) {
        std::cerr << "perspective diagnostic="
                  << perspectiveGrid.diagnostic.toStdString() << '\n';
        return 26;
    }

    QImage detectorCanvas(1600, 1200, QImage::Format_Grayscale8);
    detectorCanvas.fill(230);
    const QPoint detectorOrigin(380, 180);
    {
        QPainter painter(&detectorCanvas);
        painter.drawImage(detectorOrigin, rectified);
    }
    DecodeRecipe detectorRecipe;
    detectorRecipe.allowQrCode = false;
    detectorRecipe.maximumRoiCandidates = 3;
    QElapsedTimer roiTimer;
    roiTimer.start();
    const QVector<RoiCandidate> detectedRois = RoiDetector().detect(
        detectorCanvas, detectorRecipe);
    const qint64 roiElapsedUs = roiTimer.nsecsElapsed() / 1000;
    const QRectF expectedBox(detectorOrigin + QPoint(32, 32), QSize(768, 768));
    double bestRoiIoU = 0.0;
    for (const RoiCandidate& candidate : detectedRois) {
        const QRectF candidateBox = candidate.corners.boundingRect();
        const QRectF intersection = candidateBox.intersected(expectedBox);
        const double unionArea = candidateBox.width() * candidateBox.height()
            + expectedBox.width() * expectedBox.height()
            - intersection.width() * intersection.height();
        if (unionArea > 0.0)
            bestRoiIoU = std::max(bestRoiIoU,
                intersection.width() * intersection.height() / unionArea);
    }
    if (!require(bestRoiIoU >= 0.5,
                 "OpenCV ROI pyramid did not recall the normalized Data Matrix")) {
        std::cerr << "ROI candidates=" << detectedRois.size()
                  << " bestIoU=" << bestRoiIoU
                  << " elapsedUs=" << roiElapsedUs << '\n';
        return 14;
    }
    const QString payload = QStringLiteral("FIXED-ROI-TEST-2026");
    const QImage symbol = createSymbol(payload, ZXing::BarcodeFormat::QRCode, 6);
    QImage canvas(1200, 900, QImage::Format_Grayscale8);
    canvas.fill(232);
    const QPoint origin(640, 330);
    {
        QPainter painter(&canvas);
        painter.drawImage(origin, symbol);
    }
    DecodeRequest roiRequest;
    roiRequest.image = canvas;
    roiRequest.recipe.allowDataMatrix = false;
    roiRequest.recipe.fixedRoi = QRect(origin, symbol.size());
    const DecodeReport roiReport = DecodeEngine().decode(roiRequest);
    if (!require(roiReport.success && roiReport.text == payload,
                 "fixed ROI decode failed")) {
        return 2;
    }

    DecodeRecipe trackingRecipe;
    trackingRecipe.allowDataMatrix = false;
    FrameDecodeController frameController(trackingRecipe);
    FramePacket trackedFrame;
    trackedFrame.image = canvas;
    trackedFrame.sequence = 1;
    const DecodeReport firstTracked = frameController.decode(trackedFrame);
    trackedFrame.sequence = 2;
    const DecodeReport secondTracked = frameController.decode(trackedFrame);
    if (!require(firstTracked.success && secondTracked.success
                 && frameController.lastPredictedRoi().isValid()
                 && frameController.lastPredictedRoi().intersects(
                     QRect(origin, symbol.size())),
                 "frame decode controller did not reuse the previous decoded ROI")) {
        return 28;
    }

    DecodeRequest rejectedRequest = roiRequest;
    rejectedRequest.recipe.payloadRegularExpression = QStringLiteral("^NEVER-ACCEPT$");
    const DecodeReport rejectedReport = DecodeEngine().decode(rejectedRequest);
    if (!require(!rejectedReport.success,
                 "payload validation must reject a checksum-valid but invalid business payload")) {
        return 3;
    }

    const QImage conflictA = createSymbol(QStringLiteral("CONFLICT-A"),
                                          ZXing::BarcodeFormat::QRCode, 7);
    const QImage conflictB = createSymbol(QStringLiteral("CONFLICT-B"),
                                          ZXing::BarcodeFormat::QRCode, 7);
    QImage conflictCanvas(conflictA.width() + conflictB.width() + 160,
                          std::max(conflictA.height(), conflictB.height()) + 80,
                          QImage::Format_Grayscale8);
    conflictCanvas.fill(255);
    {
        QPainter painter(&conflictCanvas);
        painter.drawImage(QPoint(30, 40), conflictA);
        painter.drawImage(QPoint(conflictA.width() + 130, 40), conflictB);
    }
    DecodeRequest conflictRequest;
    conflictRequest.image = conflictCanvas;
    conflictRequest.recipe.allowDataMatrix = false;
    const DecodeReport conflictReport = DecodeEngine().decode(conflictRequest);
    if (!require(!conflictReport.success
                 && conflictReport.status == DecodeStatus::Conflict,
                 "two checksum-valid conflicting payloads must return Conflict")) {
        return 11;
    }

    QTemporaryDir temporaryDirectory;
    const QString filePath = temporaryDirectory.filePath(QStringLiteral("frame.png"));
    if (!require(temporaryDirectory.isValid() && canvas.save(filePath, "PNG"),
                 "could not create local frame fixture")) {
        return 4;
    }
    LocalFileFrameSource source(filePath);
    QString error;
    FramePacket packet;
    if (!require(source.open(&error) && source.nextFrame(packet, 50, &error)
                 && !packet.image.isNull() && packet.sequence == 1,
                 "local frame source failed")) {
        return 5;
    }

    MockCameraBackend cameraBackend;
    cameraBackend.saturated = QImage(320, 240, QImage::Format_Grayscale8);
    cameraBackend.saturated.fill(255);
    cameraBackend.detailed = QImage(320, 240, QImage::Format_Grayscale8);
    for (int y = 0; y < cameraBackend.detailed.height(); ++y) {
        uchar* row = cameraBackend.detailed.scanLine(y);
        for (int x = 0; x < cameraBackend.detailed.width(); ++x)
            row[x] = ((x / 8 + y / 8) & 1) ? 245 : 10;
    }
    CameraAcquisitionRecipe cameraRecipe;
    cameraRecipe.exposureMicroseconds = {400.0, 900.0, 1600.0};
    cameraRecipe.maximumFramesPerTrigger = 2;
    cameraRecipe.lightingRecipeId = QStringLiteral("strobe-A");
    cameraRecipe.triggerSource = QStringLiteral("Line1");
    cameraRecipe.requireGlobalShutter = true;
    cameraRecipe.strobeDelayMicroseconds = 12.0;
    cameraRecipe.strobeDurationMicroseconds = 80.0;
    IndustrialCameraFrameSource cameraSource(cameraBackend, cameraRecipe);
    FramePacket cameraPacket;
    error.clear();
    if (!require(cameraSource.open(&error)
                 && cameraSource.nextFrame(cameraPacket, 50, &error)
                 && cameraBackend.captureCount == 2
                 && cameraBackend.configured
                 && cameraBackend.configuredRecipe.triggerSource == QStringLiteral("Line1")
                 && cameraBackend.configuredRecipe.requireGlobalShutter
                 && cameraPacket.triggerId == 42
                 && cameraPacket.sequence == 1
                 && cameraPacket.exposureMicroseconds == 900.0
                 && cameraPacket.timestampMicroseconds > 0
                 && cameraPacket.sourceId == QStringLiteral("strobe-A"),
                 "industrial camera source did not select the best of two exposures")) {
        return 12;
    }
    cameraSource.close();

    RoiTracker tracker;
    tracker.update(QPolygonF({QPointF(100, 100), QPointF(300, 100),
                              QPointF(300, 300), QPointF(100, 300)}));
    const QRect predicted = tracker.predict(QSize(640, 480), 0.2);
    if (!require(predicted.contains(QRect(100, 100, 200, 200))
                 && predicted.left() >= 0 && predicted.top() >= 0
                 && predicted.right() < 640 && predicted.bottom() < 480,
                 "ROI tracker prediction is invalid or outside the frame")) {
        return 13;
    }

    DecodeRequest deadlineRequest;
    deadlineRequest.image = QImage(2400, 1800, QImage::Format_Grayscale8);
    deadlineRequest.image.fill(127);
    deadlineRequest.recipe.timeBudgetMs = 30;
    const DecodeReport deadlineReport = DecodeEngine().decode(deadlineRequest);
#ifdef NDEBUG
    constexpr qint64 deadlineSafetyEnvelopeUs = 150000;
#else
    // Debug builds deliberately disable optimization and are not the performance
    // acceptance configuration; retain a generous bound to catch hangs.
    constexpr qint64 deadlineSafetyEnvelopeUs = 750000;
#endif
    if (!require(!deadlineReport.success
                 && deadlineReport.totalMicroseconds < deadlineSafetyEnvelopeUs,
                 "deadline cancellation exceeded its safety envelope")) {
        std::cerr << "deadline totalUs=" << deadlineReport.totalMicroseconds
                  << " status=" << toString(deadlineReport.status).toStdString() << '\n';
        return 6;
    }
#ifdef NDEBUG
    deadlineRequest.recipe.timeBudgetMs = 150;
    for (int iteration = 0; iteration < 12; ++iteration) {
        const DecodeReport continuous = DecodeEngine().decode(deadlineRequest);
        if (!require(!continuous.success && continuous.totalMicroseconds < 150000,
                     "continuous requests violated the 150 ms hard deadline")) {
            std::cerr << "iteration=" << iteration
                      << " totalUs=" << continuous.totalMicroseconds << '\n';
            return 27;
        }
    }
#endif

    std::cout << "Pipeline tests passed\n";
    return 0;
}
