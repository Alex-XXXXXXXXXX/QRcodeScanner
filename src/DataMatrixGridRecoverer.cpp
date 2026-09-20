#include "DataMatrixGridRecoverer.h"

#include "ImagePreprocessor.h"

#include <QElapsedTimer>

#include <ZXingCpp.h>
#include <BitMatrix.h>
#include <DecoderResult.h>
#include <datamatrix/DMDecoder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

#ifdef QRSCANNER_WITH_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace {

struct DimensionSpec
{
    int rows;
    int columns;
    int dataRegionRows;
    int dataRegionColumns;
};

constexpr std::array<DimensionSpec, 24> squareDimensions{{
    {10, 10, 8, 8}, {12, 12, 10, 10}, {14, 14, 12, 12},
    {16, 16, 14, 14}, {18, 18, 16, 16}, {20, 20, 18, 18},
    {22, 22, 20, 20}, {24, 24, 22, 22}, {26, 26, 24, 24},
    {32, 32, 14, 14}, {36, 36, 16, 16}, {40, 40, 18, 18},
    {44, 44, 20, 20}, {48, 48, 22, 22}, {52, 52, 24, 24},
    {64, 64, 14, 14}, {72, 72, 16, 16}, {80, 80, 18, 18},
    {88, 88, 20, 20}, {96, 96, 22, 22}, {104, 104, 24, 24},
    {120, 120, 18, 18}, {132, 132, 20, 20}, {144, 144, 22, 22}
}};

constexpr std::array<DimensionSpec, 6> rectangularDimensions{{
    {8, 18, 6, 16}, {8, 32, 6, 14}, {12, 26, 10, 24},
    {12, 36, 10, 16}, {16, 36, 14, 16}, {16, 48, 14, 22}
}};

bool expectedFinderValue(int row, int column, int rows, int columns,
                         int dataRegionRows, int dataRegionColumns,
                         bool& expectedBlack)
{
    const int regionSpanRows = dataRegionRows + 2;
    const int regionSpanColumns = dataRegionColumns + 2;
    if (rows % regionSpanRows != 0 || columns % regionSpanColumns != 0)
        return false;
    const int localRow = row % regionSpanRows;
    const int localColumn = column % regionSpanColumns;
    if (localColumn == 0 || localRow == regionSpanRows - 1) {
        expectedBlack = true;
        return true;
    }
    if (localRow == 0) {
        expectedBlack = (localColumn % 2) == 0;
        return true;
    }
    if (localColumn == regionSpanColumns - 1) {
        expectedBlack = (localRow % 2) == 1;
        return true;
    }
    return false;
}

#ifdef QRSCANNER_WITH_OPENCV
struct SampledGrid
{
    cv::Mat values;
    cv::Mat modules;
    int rows = 0;
    int columns = 0;
    int dataRegionRows = 0;
    int dataRegionColumns = 0;
    double finderMismatch = 1.0;
    double confidence = 0.0;
};

SampledGrid sampleGrid(const cv::Mat& gray, const cv::Rect2d& bounds,
                       const DimensionSpec& spec, double phaseX, double phaseY,
                       double splitShiftX = 0.0, double splitShiftY = 0.0,
                       double bowX = 0.0, double bowY = 0.0)
{
    SampledGrid result;
    result.rows = spec.rows;
    result.columns = spec.columns;
    result.dataRegionRows = spec.dataRegionRows;
    result.dataRegionColumns = spec.dataRegionColumns;
    cv::Mat values(spec.rows, spec.columns, CV_8UC1);
    const double pitchX = bounds.width / spec.columns;
    const double pitchY = bounds.height / spec.rows;
    const int radius = std::max(1, static_cast<int>(std::floor(std::min(pitchX, pitchY) * 0.16)));
    const int regionSpanRows = spec.dataRegionRows + 2;
    const int regionSpanColumns = spec.dataRegionColumns + 2;
    const int regionCountY = spec.rows / regionSpanRows;
    const int regionCountX = spec.columns / regionSpanColumns;
    for (int row = 0; row < spec.rows; ++row) {
        for (int column = 0; column < spec.columns; ++column) {
            double u = (column + 0.5 + phaseX) / spec.columns;
            double v = (row + 0.5 + phaseY) / spec.rows;
            if (regionCountX == 2) {
                const double splitX = 0.5 + splitShiftX;
                u = u < 0.5 ? u * splitX / 0.5
                            : splitX + (u - 0.5) * (1.0 - splitX) / 0.5;
            }
            if (regionCountY == 2) {
                const double splitY = 0.5 + splitShiftY;
                v = v < 0.5 ? v * splitY / 0.5
                            : splitY + (v - 0.5) * (1.0 - splitY) / 0.5;
            }
            const double centerX = bounds.x + u * bounds.width
                + bowX * pitchX * (4.0 * (v - 0.5) * (v - 0.5) - 1.0);
            const double centerY = bounds.y + v * bounds.height
                + bowY * pitchY * (4.0 * (u - 0.5) * (u - 0.5) - 1.0);
            const int x = std::clamp(static_cast<int>(std::lround(centerX)), radius, gray.cols - radius - 1);
            const int y = std::clamp(static_cast<int>(std::lround(centerY)), radius, gray.rows - radius - 1);
            std::array<uchar, 5> samples{{
                gray.at<uchar>(y, x), gray.at<uchar>(y - radius, x),
                gray.at<uchar>(y + radius, x), gray.at<uchar>(y, x - radius),
                gray.at<uchar>(y, x + radius)}};
            std::nth_element(samples.begin(), samples.begin() + 2, samples.end());
            values.at<uchar>(row, column) = samples[2];
        }
    }
    double threshold = cv::threshold(values, result.modules, 0, 255,
                                     cv::THRESH_BINARY | cv::THRESH_OTSU);
    result.values = values;
    int finderCount = 0;
    int finderErrors = 0;
    double confidenceSum = 0.0;
    for (int row = 0; row < spec.rows; ++row) {
        for (int column = 0; column < spec.columns; ++column) {
            const bool black = result.modules.at<uchar>(row, column) == 0;
            confidenceSum += std::min(1.0, std::abs(values.at<uchar>(row, column) - threshold) / 96.0);
            bool expectedBlack = false;
            if (expectedFinderValue(row, column, spec.rows, spec.columns,
                                    spec.dataRegionRows, spec.dataRegionColumns,
                                    expectedBlack)) {
                ++finderCount;
                finderErrors += black != expectedBlack;
            }
        }
    }
    result.finderMismatch = finderCount > 0
        ? static_cast<double>(finderErrors) / finderCount : 1.0;
    result.confidence = confidenceSum / (spec.rows * spec.columns);
    return result;
}

QImage renderGrid(const cv::Mat& modules)
{
    constexpr int quietModules = 4;
    const int scale = std::max(4, 640 / (modules.cols + quietModules * 2));
    cv::Mat clean((modules.rows + quietModules * 2) * scale,
                  (modules.cols + quietModules * 2) * scale, CV_8UC1, cv::Scalar(255));
    for (int row = 0; row < modules.rows; ++row) {
        for (int column = 0; column < modules.cols; ++column) {
            if (modules.at<uchar>(row, column) == 0) {
                cv::rectangle(clean,
                    cv::Rect((column + quietModules) * scale, (row + quietModules) * scale,
                             scale, scale), cv::Scalar(0), cv::FILLED);
            }
        }
    }
    return QImage(clean.data, clean.cols, clean.rows, static_cast<int>(clean.step),
                  QImage::Format_Grayscale8).copy();
}

bool decodeCleanGrid(const QImage& image, QString& text, QString& diagnostic)
{
    const ZXing::ImageView view(image.constBits(), image.width(), image.height(),
                                ZXing::ImageFormat::Lum, image.bytesPerLine());
    const auto barcodes = ZXing::ReadBarcodes(view,
        ZXing::ReaderOptions().formats(ZXing::BarcodeFormat::DataMatrix)
            .tryHarder(true).returnErrors(true).maxNumberOfSymbols(1));
    for (const auto& barcode : barcodes) {
        if (barcode.isValid()) {
            text = QString::fromUtf8(barcode.text().c_str());
            return true;
        }
        if (barcode.format() == ZXing::BarcodeFormat::DataMatrix)
            diagnostic = QString::fromStdString(ZXing::ToString(barcode.error()));
    }
    return false;
}

bool decodeModuleMatrix(const cv::Mat& modules, QString& text, QString& diagnostic)
{
    ZXing::BitMatrix bits(modules.cols, modules.rows);
    for (int row = 0; row < modules.rows; ++row) {
        for (int column = 0; column < modules.cols; ++column)
            bits.set(column, row, modules.at<uchar>(row, column) == 0);
    }
    auto decoded = ZXing::DataMatrix::Decode(bits);
    if (decoded.isValid()) {
        text = QString::fromUtf8(decoded.content().utf8().c_str());
        diagnostic.clear();
        return true;
    }
    diagnostic = QString::fromStdString(ZXing::ToString(decoded.error()));
    return false;
}

cv::Mat thresholdModules(const SampledGrid& hypothesis, int offset, bool perRegion)
{
    cv::Mat modules(hypothesis.values.size(), CV_8UC1);
    const int spanRows = hypothesis.dataRegionRows + 2;
    const int spanColumns = hypothesis.dataRegionColumns + 2;
    if (!perRegion || hypothesis.rows % spanRows != 0
        || hypothesis.columns % spanColumns != 0) {
        cv::Mat ignored;
        const double threshold = cv::threshold(hypothesis.values, ignored, 0, 255,
                                               cv::THRESH_BINARY | cv::THRESH_OTSU);
        cv::threshold(hypothesis.values, modules, threshold + offset, 255,
                      cv::THRESH_BINARY);
        return modules;
    }

    const int regionsY = hypothesis.rows / spanRows;
    const int regionsX = hypothesis.columns / spanColumns;
    for (int regionY = 0; regionY < regionsY; ++regionY) {
        for (int regionX = 0; regionX < regionsX; ++regionX) {
            const cv::Rect rectangle(regionX * spanColumns, regionY * spanRows,
                                     spanColumns, spanRows);
            const cv::Mat values = hypothesis.values(rectangle);
            cv::Mat ignored;
            const double threshold = cv::threshold(values, ignored, 0, 255,
                                                   cv::THRESH_BINARY | cv::THRESH_OTSU);
            cv::threshold(values, modules(rectangle), threshold + offset, 255,
                          cv::THRESH_BINARY);
        }
    }
    return modules;
}

struct CellConfidence
{
    double distance = 0.0;
    int row = 0;
    int column = 0;
};

std::vector<CellConfidence> uncertainDataCells(const SampledGrid& hypothesis,
                                               int offset, bool perRegion)
{
    const int spanRows = hypothesis.dataRegionRows + 2;
    const int spanColumns = hypothesis.dataRegionColumns + 2;
    const int regionsY = hypothesis.rows % spanRows == 0
        ? hypothesis.rows / spanRows : 1;
    const int regionsX = hypothesis.columns % spanColumns == 0
        ? hypothesis.columns / spanColumns : 1;
    std::vector<double> thresholds(static_cast<size_t>(regionsY * regionsX), 0.0);
    for (int regionY = 0; regionY < regionsY; ++regionY) {
        for (int regionX = 0; regionX < regionsX; ++regionX) {
            cv::Mat ignored;
            const cv::Mat values = perRegion
                ? hypothesis.values(cv::Rect(regionX * spanColumns,
                                             regionY * spanRows,
                                             spanColumns, spanRows))
                : hypothesis.values;
            const double threshold = cv::threshold(values, ignored, 0, 255,
                                                    cv::THRESH_BINARY | cv::THRESH_OTSU);
            thresholds[static_cast<size_t>(regionY * regionsX + regionX)] = threshold + offset;
            if (!perRegion)
                break;
        }
        if (!perRegion)
            break;
    }
    if (!perRegion)
        std::fill(thresholds.begin(), thresholds.end(), thresholds.front());

    std::vector<CellConfidence> cells;
    for (int row = 0; row < hypothesis.rows; ++row) {
        for (int column = 0; column < hypothesis.columns; ++column) {
            bool expectedBlack = false;
            if (expectedFinderValue(row, column, hypothesis.rows,
                                    hypothesis.columns, hypothesis.dataRegionRows,
                                    hypothesis.dataRegionColumns, expectedBlack)) {
                continue;
            }
            const int regionX = std::min(regionsX - 1, column / spanColumns);
            const int regionY = std::min(regionsY - 1, row / spanRows);
            const double threshold = thresholds[static_cast<size_t>(
                regionY * regionsX + regionX)];
            cells.push_back({std::abs(hypothesis.values.at<uchar>(row, column)
                                      - threshold), row, column});
        }
    }
    std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
        return a.distance < b.distance;
    });
    return cells;
}
#endif

} // namespace

QVector<QSize> DataMatrixGridRecoverer::legalEcc200Dimensions()
{
    QVector<QSize> result;
    result.reserve(static_cast<int>(squareDimensions.size()
                                    + rectangularDimensions.size()));
    for (const DimensionSpec& dimension : squareDimensions)
        result.push_back(QSize(dimension.columns, dimension.rows));
    for (const DimensionSpec& dimension : rectangularDimensions)
        result.push_back(QSize(dimension.columns, dimension.rows));
    return result;
}

bool DataMatrixGridRecoverer::isLegalEcc200Dimension(int rows, int columns)
{
    const QVector<QSize> dimensions = legalEcc200Dimensions();
    return std::any_of(dimensions.cbegin(), dimensions.cend(),
        [&](const QSize& value) { return value.height() == rows && value.width() == columns; });
}

bool DataMatrixGridRecoverer::expectedFinderModule(int rows, int columns,
                                                    int row, int column,
                                                    bool& expectedBlack)
{
    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return false;
    const auto matches = [=](const DimensionSpec& value) {
        return value.rows == rows && value.columns == columns;
    };
    auto square = std::find_if(squareDimensions.begin(), squareDimensions.end(), matches);
    if (square != squareDimensions.end()) {
        return expectedFinderValue(row, column, rows, columns,
                                   square->dataRegionRows,
                                   square->dataRegionColumns, expectedBlack);
    }
    auto rectangle = std::find_if(rectangularDimensions.begin(),
                                  rectangularDimensions.end(), matches);
    if (rectangle != rectangularDimensions.end()) {
        return expectedFinderValue(row, column, rows, columns,
                                   rectangle->dataRegionRows,
                                   rectangle->dataRegionColumns, expectedBlack);
    }
    return false;
}

GridRecoveryResult DataMatrixGridRecoverer::recover(const QImage& rectifiedImage,
                                                     const DecodeRecipe& recipe,
                                                     qint64 timeBudgetMicroseconds) const
{
    GridRecoveryResult result;
#ifndef QRSCANNER_WITH_OPENCV
    Q_UNUSED(rectifiedImage)
    Q_UNUSED(recipe)
    Q_UNUSED(timeBudgetMicroseconds)
    result.diagnostic = QStringLiteral("OpenCV L3 grid recovery is not available");
    return result;
#else
    if (rectifiedImage.isNull() || timeBudgetMicroseconds <= 0)
        return result;
    QElapsedTimer timer;
    timer.start();
    const QImage grayImage = ImagePreprocessor::toGrayscale(rectifiedImage);
    cv::Mat gray(grayImage.height(), grayImage.width(), CV_8UC1,
                 const_cast<uchar*>(grayImage.constBits()), grayImage.bytesPerLine());
    cv::Mat binary;
    cv::threshold(gray, binary, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    cv::Mat labels, statistics, centroids;
    const int componentCount = cv::connectedComponentsWithStats(
        binary, labels, statistics, centroids, 8, CV_32S);
    int structureComponent = -1;
    int structureArea = 0;
    for (int component = 1; component < componentCount; ++component) {
        const int width = statistics.at<int>(component, cv::CC_STAT_WIDTH);
        const int height = statistics.at<int>(component, cv::CC_STAT_HEIGHT);
        const int area = statistics.at<int>(component, cv::CC_STAT_AREA);
        const double aspect = static_cast<double>(std::min(width, height))
            / std::max(1, std::max(width, height));
        if (width >= 48 && height >= 48 && aspect >= 0.65 && area > structureArea) {
            structureComponent = component;
            structureArea = area;
        }
    }
    if (structureComponent < 0) {
        result.diagnostic = QStringLiteral("no dark modules in rectified ROI");
        return result;
    }
    const cv::Rect bounds(
        statistics.at<int>(structureComponent, cv::CC_STAT_LEFT),
        statistics.at<int>(structureComponent, cv::CC_STAT_TOP),
        statistics.at<int>(structureComponent, cv::CC_STAT_WIDTH),
        statistics.at<int>(structureComponent, cv::CC_STAT_HEIGHT));
    const cv::Rect2d darkSamplingBounds(bounds);

    std::vector<DimensionSpec> dimensions(squareDimensions.begin(), squareDimensions.end());
    dimensions.insert(dimensions.end(), rectangularDimensions.begin(),
                      rectangularDimensions.end());
    if (recipe.expectedDataMatrixRows > 0
        && recipe.expectedDataMatrixColumns > 0) {
        std::stable_sort(dimensions.begin(), dimensions.end(), [&](const auto& a, const auto& b) {
            return std::abs(a.rows - recipe.expectedDataMatrixRows)
                    + std::abs(a.columns - recipe.expectedDataMatrixColumns)
                < std::abs(b.rows - recipe.expectedDataMatrixRows)
                    + std::abs(b.columns - recipe.expectedDataMatrixColumns);
        });
        const auto exact = std::find_if(dimensions.begin(), dimensions.end(),
            [&](const auto& value) {
                return value.rows == recipe.expectedDataMatrixRows
                    && value.columns == recipe.expectedDataMatrixColumns;
            });
        if (exact != dimensions.end())
            dimensions = {*exact};
        else if (dimensions.size() > 3)
            dimensions.resize(3);
    } else {
        std::stable_sort(dimensions.begin(), dimensions.end(), [](const auto& a, const auto& b) {
            return std::abs(a.rows - 48) + std::abs(a.columns - 48)
                < std::abs(b.rows - 48) + std::abs(b.columns - 48);
        });
    }

    std::vector<SampledGrid> hypotheses;
    constexpr std::array<double, 5> phases{{-0.40, -0.20, 0.0, 0.20, 0.40}};
    constexpr std::array<double, 3> splits{{-0.018, 0.0, 0.018}};
    constexpr std::array<double, 5> bows{{-1.0, -0.5, 0.0, 0.5, 1.0}};
    const qint64 generationBudgetMicroseconds = std::min<qint64>(
        120000, std::max<qint64>(5000, timeBudgetMicroseconds * 55 / 100));
    const auto appendHypothesis = [&](const cv::Rect2d& samplingBounds,
                                      const DimensionSpec& dimension, double phaseX,
                                      double phaseY, double splitX = 0.0,
                                      double splitY = 0.0, double bowX = 0.0,
                                      double bowY = 0.0) {
        if (timer.nsecsElapsed() / 1000 >= generationBudgetMicroseconds)
            return false;
        hypotheses.push_back(sampleGrid(gray, samplingBounds, dimension, phaseX, phaseY,
                                        splitX, splitY, bowX, bowY));
        return true;
    };
    bool generationExpired = false;
    for (const DimensionSpec& dimension : dimensions) {
        if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds)
            break;
        std::vector<cv::Rect2d> boundsCandidates;
        if (gray.cols == gray.rows && gray.cols >= 256) {
            const double mappedMargin = std::round(gray.cols / 26.0);
            const double mappedFirstCenter = mappedMargin;
            const double mappedLastCenter = gray.cols - mappedMargin - 1.0;
            const double centerPitchX = (mappedLastCenter - mappedFirstCenter)
                / (dimension.columns - 1);
            const double centerPitchY = (mappedLastCenter - mappedFirstCenter)
                / (dimension.rows - 1);
            const cv::Rect2d centerModel(
                mappedFirstCenter - centerPitchX * 0.5,
                mappedFirstCenter - centerPitchY * 0.5,
                centerPitchX * dimension.columns, centerPitchY * dimension.rows);
            const cv::Rect2d edgeModel(mappedMargin, mappedMargin,
                                       gray.cols - 2.0 * mappedMargin,
                                       gray.rows - 2.0 * mappedMargin);
            if (recipe.fixedQuadrilateral.size() == 4) {
                boundsCandidates = {edgeModel, darkSamplingBounds, centerModel};
            } else {
                boundsCandidates = {darkSamplingBounds, edgeModel, centerModel};
            }
        } else {
            boundsCandidates = {darkSamplingBounds};
        }
        for (const cv::Rect2d& samplingBounds : boundsCandidates) {
            for (double phaseY : phases) {
                for (double phaseX : phases) {
                    if (!appendHypothesis(samplingBounds, dimension, phaseX, phaseY)) {
                        generationExpired = true;
                        break;
                    }
                }
                if (generationExpired)
                    break;
            }
            if (generationExpired)
                break;
            const int regionsX = dimension.columns / (dimension.dataRegionColumns + 2);
            const int regionsY = dimension.rows / (dimension.dataRegionRows + 2);
            if (regionsX == 2 || regionsY == 2) {
                for (double splitY : splits) {
                    for (double splitX : splits) {
                        const double applicableSplitX = regionsX == 2 ? splitX : 0.0;
                        const double applicableSplitY = regionsY == 2 ? splitY : 0.0;
                        if ((applicableSplitX != 0.0 || applicableSplitY != 0.0)
                            && !appendHypothesis(samplingBounds, dimension, 0.0, 0.0,
                                                 applicableSplitX, applicableSplitY)) {
                            generationExpired = true;
                            break;
                        }
                    }
                    if (generationExpired)
                        break;
                }
            }
            if (!generationExpired) {
                for (double bowY : bows) {
                    for (double bowX : bows) {
                        if ((bowX != 0.0 || bowY != 0.0)
                            && !appendHypothesis(samplingBounds, dimension, 0.0, 0.0,
                                                 0.0, 0.0, bowX, bowY)) {
                            generationExpired = true;
                            break;
                        }
                    }
                    if (generationExpired)
                        break;
                }
            }
            if (generationExpired)
                break;
        }
        if (generationExpired)
            break;
    }
    std::sort(hypotheses.begin(), hypotheses.end(), [](const auto& a, const auto& b) {
        if (a.finderMismatch != b.finderMismatch)
            return a.finderMismatch < b.finderMismatch;
        return a.confidence > b.confidence;
    });

    if (!hypotheses.empty()) {
        result.rows = hypotheses.front().rows;
        result.columns = hypotheses.front().columns;
        result.confidence = std::max(0.0,
            (1.0 - hypotheses.front().finderMismatch) * hypotheses.front().confidence);
    }
    const double bestFinderMismatch = hypotheses.empty()
        ? 1.0 : hypotheses.front().finderMismatch;

    const int attempts = std::min(32, static_cast<int>(hypotheses.size()));
    constexpr std::array<int, 7> thresholdOffsets{{0, -8, 8, -16, 16, -24, 24}};
    for (int i = 0; i < attempts; ++i) {
        if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds) {
            result.diagnostic = QStringLiteral("L3 deadline exceeded");
            break;
        }
        const SampledGrid& hypothesis = hypotheses[static_cast<size_t>(i)];
        const double hypothesisConfidence = std::max(
            0.0, (1.0 - hypothesis.finderMismatch) * hypothesis.confidence);
        QString diagnostic;
        if (hypothesis.finderMismatch <= 0.34) {
            for (bool perRegion : {false, true}) {
                for (int thresholdOffset : thresholdOffsets) {
                    if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds) {
                        result.diagnostic = QStringLiteral("L3 deadline exceeded");
                        return result;
                    }
                    const cv::Mat modules = thresholdModules(
                        hypothesis, thresholdOffset, perRegion);
                    if (decodeModuleMatrix(modules, result.text, diagnostic)) {
                        result.success = true;
                        result.rows = hypothesis.rows;
                        result.columns = hypothesis.columns;
                        result.confidence = hypothesisConfidence;
                        result.diagnostic.clear();
                        return result;
                    }
                    if (i == 0 && std::abs(thresholdOffset) <= 8
                        && diagnostic.contains(QStringLiteral("Checksum"),
                                               Qt::CaseInsensitive)) {
                        const std::vector<CellConfidence> uncertain = uncertainDataCells(
                            hypothesis, thresholdOffset, perRegion);
                        cv::Mat corrected = modules.clone();
                        const int singles = std::min(16, static_cast<int>(uncertain.size()));
                        const auto tryFlipSet = [&](std::initializer_list<int> indices) {
                            for (int index : indices) {
                                const auto& cell = uncertain[static_cast<size_t>(index)];
                                corrected.at<uchar>(cell.row, cell.column) ^= 255;
                            }
                            const bool decoded = decodeModuleMatrix(
                                corrected, result.text, diagnostic);
                            for (int index : indices) {
                                const auto& cell = uncertain[static_cast<size_t>(index)];
                                corrected.at<uchar>(cell.row, cell.column) ^= 255;
                            }
                            return decoded;
                        };
                        for (int first = 0; first < singles; ++first) {
                            if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds)
                                return result;
                            if (tryFlipSet({first})) {
                                result.success = true;
                                result.rows = hypothesis.rows;
                                result.columns = hypothesis.columns;
                                result.confidence = hypothesisConfidence;
                                result.diagnostic.clear();
                                return result;
                            }
                        }
                        const int pairs = std::min(10, singles);
                        for (int first = 0; first < pairs; ++first) {
                            for (int second = first + 1; second < pairs; ++second) {
                                if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds)
                                    return result;
                                if (tryFlipSet({first, second})) {
                                    result.success = true;
                                    result.rows = hypothesis.rows;
                                    result.columns = hypothesis.columns;
                                    result.confidence = hypothesisConfidence;
                                    result.diagnostic.clear();
                                    return result;
                                }
                            }
                        }
                        const int triples = std::min(6, pairs);
                        for (int first = 0; first < triples; ++first) {
                            for (int second = first + 1; second < triples; ++second) {
                                for (int third = second + 1; third < triples; ++third) {
                                    if (timer.nsecsElapsed() / 1000 >= timeBudgetMicroseconds)
                                        return result;
                                    if (tryFlipSet({first, second, third})) {
                                        result.success = true;
                                        result.rows = hypothesis.rows;
                                        result.columns = hypothesis.columns;
                                        result.confidence = hypothesisConfidence;
                                        result.diagnostic.clear();
                                        return result;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!diagnostic.isEmpty())
            result.diagnostic = diagnostic;
    }
    if (result.diagnostic.isEmpty()) {
        result.diagnostic = hypotheses.empty()
            ? QStringLiteral("L3 deadline exceeded before a grid hypothesis was sampled")
            : QStringLiteral("no valid ECC200 grid hypothesis (finder mismatch %1)")
                .arg(hypotheses.front().finderMismatch, 0, 'f', 3);
    } else if (!hypotheses.empty()) {
        result.diagnostic += QStringLiteral("; best finder mismatch %1")
            .arg(bestFinderMismatch, 0, 'f', 3);
    }
    return result;
#endif
}
