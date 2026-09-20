#include "ImageQualityAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>

QString ImageQuality::summary() const
{
    return QStringLiteral("%1 × %2 | 平均亮度 %3 | 对比度 %4 | 饱和 %5% | 清晰度 %6 | 边缘一致性 %7")
        .arg(width)
        .arg(height)
        .arg(meanLuminance, 0, 'f', 1)
        .arg(contrast, 0, 'f', 1)
        .arg(saturatedRatio * 100.0, 0, 'f', 1)
        .arg(sharpness, 0, 'f', 1)
        .arg(edgeConsistency, 0, 'f', 3);
}

ImageQuality ImageQualityAnalyzer::analyze(const QImage& input)
{
    const QImage gray = input.format() == QImage::Format_Grayscale8
        ? input
        : input.convertToFormat(QImage::Format_Grayscale8);

    ImageQuality result;
    result.width = gray.width();
    result.height = gray.height();
    if (gray.isNull() || gray.width() < 3 || gray.height() < 3)
        return result;

    std::array<quint64, 256> histogram{};
    quint64 sampleCount = 0;
    double luminanceSum = 0.0;

    constexpr double targetSamples = 30000.0;
    const int sampleStep = std::max(1, static_cast<int>(std::ceil(
        std::sqrt(static_cast<double>(gray.width()) * gray.height() / targetSamples))));
    for (int y = 0; y < gray.height(); y += sampleStep) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); x += sampleStep) {
            const int value = row[x];
            ++histogram[static_cast<size_t>(value)];
            luminanceSum += value;
            ++sampleCount;
        }
    }

    result.meanLuminance = sampleCount > 0
        ? static_cast<double>(luminanceSum / sampleCount)
        : 0.0;

    const auto percentile = [&](double fraction) {
        const quint64 target = static_cast<quint64>(fraction * sampleCount);
        quint64 accumulated = 0;
        for (int i = 0; i < 256; ++i) {
            accumulated += histogram[static_cast<size_t>(i)];
            if (accumulated >= target)
                return i;
        }
        return 255;
    };

    const int p05 = percentile(0.05);
    const int p95 = percentile(0.95);
    result.contrast = static_cast<double>(p95 - p05);

    const quint64 saturated = histogram[0] + histogram[1] + histogram[2]
        + histogram[253] + histogram[254] + histogram[255];
    result.saturatedRatio = sampleCount > 0
        ? static_cast<double>(saturated) / static_cast<double>(sampleCount)
        : 0.0;

    double laplacianSum = 0.0;
    double laplacianSquaredSum = 0.0;
    double axisAlignmentSum = 0.0;
    quint64 strongEdgeCount = 0;
    quint64 laplacianCount = 0;
    for (int y = 1; y < gray.height() - 1; y += sampleStep) {
        const uchar* previous = gray.constScanLine(y - 1);
        const uchar* current = gray.constScanLine(y);
        const uchar* next = gray.constScanLine(y + 1);
        for (int x = 1; x < gray.width() - 1; x += sampleStep) {
            const int laplacian = previous[x] + next[x] + current[x - 1]
                + current[x + 1] - 4 * current[x];
            const int gradientX = static_cast<int>(current[x + 1]) - current[x - 1];
            const int gradientY = static_cast<int>(next[x]) - previous[x];
            const int absoluteX = std::abs(gradientX);
            const int absoluteY = std::abs(gradientY);
            const int gradientMagnitude = absoluteX + absoluteY;
            if (gradientMagnitude >= 24) {
                axisAlignmentSum += std::max(absoluteX, absoluteY)
                    / static_cast<double>(gradientMagnitude);
                ++strongEdgeCount;
            }
            laplacianSum += laplacian;
            laplacianSquaredSum += static_cast<double>(laplacian) * laplacian;
            ++laplacianCount;
        }
    }

    if (laplacianCount > 0) {
        const double mean = laplacianSum / laplacianCount;
        const double variance = std::max(
            0.0, laplacianSquaredSum / laplacianCount - mean * mean);
        result.sharpness = std::sqrt(variance);
    }
    result.edgeConsistency = strongEdgeCount > 0
        ? axisAlignmentSum / strongEdgeCount : 0.0;

    return result;
}
