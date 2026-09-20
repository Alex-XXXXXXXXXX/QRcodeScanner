#include "DecodeEngine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTextStream>

#include <ZXingCpp.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

QImage ownedImage(const cv::Mat& image)
{
    return QImage(image.data, image.cols, image.rows, static_cast<int>(image.step),
                  QImage::Format_Grayscale8).copy();
}

cv::Mat baseDataMatrix(const QString& payload)
{
    const auto barcode = ZXing::CreateBarcodeFromText(
        payload.toStdString(), ZXing::BarcodeFormat::DataMatrix);
    const auto generated = ZXing::WriteBarcodeToImage(
        barcode, ZXing::WriterOptions().scale(10).addQuietZones(true));
    cv::Mat symbol(generated.height(), generated.width(), CV_8UC1,
                   const_cast<uint8_t*>(generated.data()), generated.width());
    cv::Mat canvas(900, 900, CV_8UC1, cv::Scalar(235));
    const int x = (canvas.cols - symbol.cols) / 2;
    const int y = (canvas.rows - symbol.rows) / 2;
    symbol.copyTo(canvas(cv::Rect(x, y, symbol.cols, symbol.rows)));
    return canvas;
}

cv::Mat distort(const cv::Mat& base, const QString& kind, int level)
{
    cv::Mat output = base.clone();
    if (kind == QStringLiteral("perspective")) {
        const float delta = static_cast<float>(level * 12);
        std::array<cv::Point2f, 4> source{{{0, 0}, {899, 0}, {899, 899}, {0, 899}}};
        std::array<cv::Point2f, 4> target{{{delta, 0}, {899 - delta * 0.3f, delta},
                                         {899, 899 - delta}, {0, 899}}};
        cv::warpPerspective(base, output,
            cv::getPerspectiveTransform(source.data(), target.data()), base.size(),
            cv::INTER_CUBIC, cv::BORDER_CONSTANT, cv::Scalar(235));
    } else if (kind == QStringLiteral("defocus")) {
        const int kernel = 1 + level * 2;
        if (kernel > 1)
            cv::GaussianBlur(base, output, cv::Size(kernel, kernel), level * 0.7);
    } else if (kind == QStringLiteral("motion")) {
        const int kernelSize = 1 + level * 4;
        cv::Mat kernel = cv::Mat::zeros(kernelSize, kernelSize, CV_32F);
        kernel.row(kernelSize / 2).setTo(1.0f / kernelSize);
        cv::filter2D(base, output, -1, kernel);
    } else if (kind == QStringLiteral("contrast")) {
        const double alpha = std::max(0.18, 1.0 - level * 0.13);
        base.convertTo(output, -1, alpha, 128.0 * (1.0 - alpha));
    } else if (kind == QStringLiteral("glare")) {
        cv::circle(output, cv::Point(520, 420), 10 + level * 15,
                   cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    } else if (kind == QStringLiteral("occlusion")) {
        const int size = level * 10;
        if (size > 0)
            cv::rectangle(output, cv::Rect(430, 430, size, size), cv::Scalar(235), cv::FILLED);
    }
    return output;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({{"o", "output-dir"}, "Optionally save generated samples", "path"});
    parser.process(application);

    const QString payload = QStringLiteral("SYNTHETIC-DM-REGRESSION-2026");
    const cv::Mat base = baseDataMatrix(payload);
    const QStringList kinds{QStringLiteral("perspective"), QStringLiteral("defocus"),
                            QStringLiteral("motion"), QStringLiteral("contrast"),
                            QStringLiteral("glare"), QStringLiteral("occlusion")};
    const QString outputPath = parser.value(QStringLiteral("output-dir"));
    QDir outputDirectory(outputPath);
    if (!outputPath.isEmpty())
        outputDirectory.mkpath(QStringLiteral("."));

    QJsonArray records;
    int wrong = 0;
    int correct = 0;
    int noRead = 0;
    QVector<qint64> latencies;
    QMap<QString, int> maximumSuccessfulLevel;
    for (const QString& kind : kinds) {
        for (int level = 0; level <= 6; ++level) {
            const QImage image = ownedImage(distort(base, kind, level));
            DecodeRequest request;
            request.image = image;
            request.recipe.allowQrCode = false;
            request.recipe.timeBudgetMs = 150;
            request.recipe.payloadRegularExpression = QStringLiteral("^SYNTHETIC-DM-");
            const DecodeReport report = DecodeEngine().decode(request);
            if (report.success && report.text != payload)
                ++wrong;
            const bool isCorrect = report.success && report.text == payload;
            correct += isCorrect;
            noRead += !report.success;
            latencies.push_back(report.totalMicroseconds);
            if (isCorrect)
                maximumSuccessfulLevel[kind] = level;
            QJsonObject record;
            record.insert(QStringLiteral("distortion"), kind);
            record.insert(QStringLiteral("level"), level);
            record.insert(QStringLiteral("status"), toString(report.status));
            record.insert(QStringLiteral("correct"), report.success && report.text == payload);
            record.insert(QStringLiteral("latencyUs"), static_cast<double>(report.totalMicroseconds));
            record.insert(QStringLiteral("route"), report.route);
            records.append(record);
            if (!outputPath.isEmpty())
                image.save(outputDirectory.filePath(
                    QStringLiteral("%1-%2.png").arg(kind).arg(level)), "PNG");
        }
    }
    QJsonObject summary;
    summary.insert(QStringLiteral("wrongOutputs"), wrong);
    summary.insert(QStringLiteral("correct"), correct);
    summary.insert(QStringLiteral("safeNoRead"), noRead);
    std::sort(latencies.begin(), latencies.end());
    const int p95Index = std::clamp(
        static_cast<int>(std::ceil(latencies.size() * 0.95)) - 1,
        0, std::max(0, latencies.size() - 1));
    summary.insert(QStringLiteral("p95Ms"), latencies.isEmpty()
        ? 0.0 : latencies[p95Index] / 1000.0);
    summary.insert(QStringLiteral("maximumMs"), latencies.isEmpty()
        ? 0.0 : latencies.back() / 1000.0);
    QJsonObject maximumLevelObject;
    for (auto it = maximumSuccessfulLevel.cbegin();
         it != maximumSuccessfulLevel.cend(); ++it)
        maximumLevelObject.insert(it.key(), it.value());
    summary.insert(QStringLiteral("maximumSuccessfulLevel"), maximumLevelObject);
    summary.insert(QStringLiteral("records"), records);
    QTextStream(stdout) << QJsonDocument(summary).toJson(QJsonDocument::Indented);
    return wrong == 0 ? 0 : 4;
}
