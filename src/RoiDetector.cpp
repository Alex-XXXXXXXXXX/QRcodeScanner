#include "RoiDetector.h"

#include "ImagePreprocessor.h"

#include <algorithm>
#include <array>

#ifdef QRSCANNER_WITH_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace {

#ifdef QRSCANNER_WITH_OPENCV
std::array<cv::Point2f, 4> orderedQuad(const cv::RotatedRect& rectangle)
{
    cv::Point2f points[4];
    rectangle.points(points);
    std::array<cv::Point2f, 4> ordered{};
    auto sum = [](const cv::Point2f& p) { return p.x + p.y; };
    auto difference = [](const cv::Point2f& p) { return p.x - p.y; };
    ordered[0] = *std::min_element(points, points + 4,
        [&](const auto& a, const auto& b) { return sum(a) < sum(b); });
    ordered[2] = *std::max_element(points, points + 4,
        [&](const auto& a, const auto& b) { return sum(a) < sum(b); });
    ordered[1] = *std::max_element(points, points + 4,
        [&](const auto& a, const auto& b) { return difference(a) < difference(b); });
    ordered[3] = *std::min_element(points, points + 4,
        [&](const auto& a, const auto& b) { return difference(a) < difference(b); });
    return ordered;
}

double edgeSequenceScore(const cv::Mat& binary, bool horizontal,
                         int bandStart, int bandEnd)
{
    std::vector<uchar> sequence;
    const int length = horizontal ? binary.cols : binary.rows;
    sequence.reserve(static_cast<size_t>(length));
    for (int position = 0; position < length; ++position) {
        double sum = 0.0;
        for (int band = bandStart; band <= bandEnd; ++band)
            sum += horizontal ? binary.at<uchar>(band, position)
                              : binary.at<uchar>(position, band);
        sequence.push_back(sum / (bandEnd - bandStart + 1) >= 128.0);
    }
    int transitions = 0;
    int black = 0;
    for (size_t i = 0; i < sequence.size(); ++i) {
        black += sequence[i] != 0;
        transitions += i > 0 && sequence[i] != sequence[i - 1];
    }
    const double blackRatio = black / static_cast<double>(std::max<size_t>(1, sequence.size()));
    const double balance = std::max(0.0, 1.0 - 2.0 * std::abs(blackRatio - 0.5));
    const double transitionScore = std::min(1.0, transitions / 12.0);
    return 0.55 * balance + 0.45 * transitionScore;
}

double dataMatrixStructureScore(const cv::Mat& source,
                                const cv::RotatedRect& rectangle)
{
    constexpr int sampleSize = 64;
    const auto input = orderedQuad(rectangle);
    const std::array<cv::Point2f, 4> output{{
        {0, 0}, {sampleSize - 1.0f, 0},
        {sampleSize - 1.0f, sampleSize - 1.0f}, {0, sampleSize - 1.0f}}};
    cv::Mat normalized;
    cv::warpPerspective(source, normalized,
        cv::getPerspectiveTransform(input.data(), output.data()),
        cv::Size(sampleSize, sampleSize), cv::INTER_AREA,
        cv::BORDER_CONSTANT, cv::Scalar(255));
    cv::Mat binary;
    cv::threshold(normalized, binary, 0, 255,
                  cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

    double best = 0.0;
    for (int rotation = 0; rotation < 4; ++rotation) {
        const cv::Rect leftBand(1, 1, 4, sampleSize - 2);
        const cv::Rect bottomBand(1, sampleSize - 5, sampleSize - 2, 4);
        const double leftSolid = cv::mean(binary(leftBand))[0] / 255.0;
        const double bottomSolid = cv::mean(binary(bottomBand))[0] / 255.0;
        const double topClock = edgeSequenceScore(binary, true, 1, 4);
        const double rightClock = edgeSequenceScore(binary, false,
                                                     sampleSize - 5, sampleSize - 2);
        const double score = 0.55 * (leftSolid + bottomSolid) * 0.5
            + 0.45 * (topClock + rightClock) * 0.5;
        best = std::max(best, score);
        cv::rotate(binary, binary, cv::ROTATE_90_CLOCKWISE);
    }
    return best;
}

QImage matToOwnedQImage(const cv::Mat& input)
{
    return QImage(input.data, input.cols, input.rows,
                  static_cast<int>(input.step), QImage::Format_Grayscale8).copy();
}
#endif

} // namespace

QVector<RoiCandidate> RoiDetector::detect(const QImage& grayImage,
                                          const DecodeRecipe& recipe) const
{
    QVector<RoiCandidate> result;
    if (grayImage.isNull())
        return result;

    if (recipe.fixedRoi.isValid()) {
        const QRect roi = recipe.fixedRoi.intersected(grayImage.rect());
        if (roi.isValid()) {
            QPolygonF corners;
            corners << roi.topLeft() << roi.topRight() << roi.bottomRight() << roi.bottomLeft();
            result.push_back({ImagePreprocessor::fitForDecode(grayImage.copy(roi), 1024),
                              corners, 1.0});
        }
        return result;
    }

#ifdef QRSCANNER_WITH_OPENCV
    const QImage gray = ImagePreprocessor::toGrayscale(grayImage);
    cv::Mat source(gray.height(), gray.width(), CV_8UC1,
                   const_cast<uchar*>(gray.constBits()), gray.bytesPerLine());
    struct ScoredRect { cv::RotatedRect rect; double score; };
    std::vector<ScoredRect> scored;
    // ROI localization only needs module-edge geometry. Keeping the largest
    // pyramid level at 480 px preserves roughly two pixels/module for the
    // 48x48 field target while meeting the i5-7260U 25 ms stage budget.
    const double baseScale = std::min(1.0, 480.0 / std::max(source.cols, source.rows));
    const std::array<double, 2> pyramidScales{{baseScale, baseScale * 0.62}};
    for (const double scale : pyramidScales) {
        if (std::min(source.cols, source.rows) * scale < 180.0)
            continue;
        cv::Mat working;
        cv::resize(source, working, {}, scale, scale, cv::INTER_AREA);
        cv::Mat blurred, binary;
        cv::GaussianBlur(working, blurred, cv::Size(5, 5), 0.0);
        cv::adaptiveThreshold(blurred, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY_INV, 21, 5);
        cv::morphologyEx(binary, binary, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(binary, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        const double imageArea = static_cast<double>(working.cols) * working.rows;
        for (const auto& contour : contours) {
            const double area = std::abs(cv::contourArea(contour));
            if (area < imageArea * 0.003 || area > imageArea * 0.75)
                continue;
            cv::RotatedRect rectangle = cv::minAreaRect(contour);
            const double shortSide = std::min(rectangle.size.width, rectangle.size.height);
            const double longSide = std::max(rectangle.size.width, rectangle.size.height);
            if (shortSide < 18.0 || shortSide / std::max(1.0, longSide) < 0.55)
                continue;
            const double fill = area / std::max(1.0, static_cast<double>(rectangle.size.area()));
            rectangle.center *= static_cast<float>(1.0 / scale);
            rectangle.size.width *= static_cast<float>(1.0 / scale);
            rectangle.size.height *= static_cast<float>(1.0 / scale);
            const double geometryScore = 0.58 * (shortSide / longSide)
                + 0.27 * std::min(1.0, fill)
                + 0.15 * std::min(1.0, area / (imageArea * 0.1));
            scored.push_back({rectangle, geometryScore});
        }
        // The lower-resolution level is a recall fallback, not unconditional
        // work. Once the primary level has enough geometrically plausible
        // candidates, skip it to preserve the ROI deadline.
        if (scored.size() >= static_cast<size_t>(
                std::max(6, recipe.maximumRoiCandidates * 2)))
            break;
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    if (scored.size() > 4)
        scored.resize(4);
    for (ScoredRect& item : scored) {
        const double structure = dataMatrixStructureScore(source, item.rect);
        item.score = 0.55 * item.score + 0.45 * structure;
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });

    // Auto-located candidates use a compact normalized image so all Top-3
    // homographies fit inside the 25 ms ROI budget. Fixed/high-confidence
    // quadrilaterals use the 512 px path in rectify().
    constexpr int outputSize = 320;
    constexpr float margin = 12.0f;
    const std::array<cv::Point2f, 4> destination{{
        {margin, margin}, {outputSize - margin - 1, margin},
        {outputSize - margin - 1, outputSize - margin - 1},
        {margin, outputSize - margin - 1}}};
    for (const ScoredRect& item : scored) {
        if (result.size() >= std::max(1, recipe.maximumRoiCandidates))
            break;
        auto points = orderedQuad(item.rect);
        QPolygonF polygon;
        std::array<cv::Point2f, 4> originalPoints{};
        for (int i = 0; i < 4; ++i) {
            originalPoints[static_cast<size_t>(i)] = points[static_cast<size_t>(i)];
            polygon << QPointF(originalPoints[static_cast<size_t>(i)].x,
                               originalPoints[static_cast<size_t>(i)].y);
        }
        bool duplicate = false;
        for (const RoiCandidate& existing : result) {
            const QRectF a = existing.corners.boundingRect();
            const QRectF b = polygon.boundingRect();
            const QRectF intersection = a.intersected(b);
            const double unionArea = a.width() * a.height() + b.width() * b.height()
                - intersection.width() * intersection.height();
            if (unionArea > 0.0 && intersection.width() * intersection.height() / unionArea > 0.65) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        cv::Mat transform = cv::getPerspectiveTransform(originalPoints.data(), destination.data());
        cv::Mat rectified;
        cv::warpPerspective(source, rectified, transform, cv::Size(outputSize, outputSize),
                            cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
        result.push_back({matToOwnedQImage(rectified), polygon, item.score});
    }
#endif
    return result;
}

RoiCandidate RoiDetector::rectify(const QImage& grayImage, const QPolygonF& corners,
                                  double score) const
{
    RoiCandidate result;
    if (grayImage.isNull() || corners.size() != 4)
        return result;
#ifdef QRSCANNER_WITH_OPENCV
    const QImage gray = ImagePreprocessor::toGrayscale(grayImage);
    cv::Mat source(gray.height(), gray.width(), CV_8UC1,
                   const_cast<uchar*>(gray.constBits()), gray.bytesPerLine());
    std::array<cv::Point2f, 4> input{};
    for (int i = 0; i < 4; ++i)
        input[static_cast<size_t>(i)] = cv::Point2f(
            static_cast<float>(corners[i].x()), static_cast<float>(corners[i].y()));
    constexpr int outputSize = 512;
    constexpr float margin = 20.0f;
    const std::array<cv::Point2f, 4> output{{
        {margin, margin}, {outputSize - margin - 1, margin},
        {outputSize - margin - 1, outputSize - margin - 1},
        {margin, outputSize - margin - 1}}};
    cv::Mat transform = cv::getPerspectiveTransform(input.data(), output.data());
    cv::Mat rectified;
    cv::warpPerspective(source, rectified, transform, cv::Size(outputSize, outputSize),
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
    result.rectifiedImage = matToOwnedQImage(rectified);
#else
    result.rectifiedImage = grayImage.copy(corners.boundingRect().toAlignedRect()
        .intersected(grayImage.rect()));
#endif
    result.corners = corners;
    result.score = score;
    return result;
}
