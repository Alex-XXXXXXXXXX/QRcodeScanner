#include "DecodeEngine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTextStream>

#include <ZXingCpp.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace {

double percentile(QVector<qint64> values, double fraction)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const int index = std::clamp(static_cast<int>(std::ceil(fraction * values.size())) - 1,
                                 0, values.size() - 1);
    return values[index] / 1000.0;
}

double percentileDouble(QVector<double> values, double fraction)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const int index = std::clamp(static_cast<int>(std::ceil(fraction * values.size())) - 1,
                                 0, values.size() - 1);
    return values[index];
}

QPolygonF polygonFromJson(const QJsonValue& value)
{
    QPolygonF polygon;
    const QJsonArray array = value.toArray();
    for (const QJsonValue& pointValue : array) {
        const QJsonArray point = pointValue.toArray();
        if (point.size() == 2)
            polygon << QPointF(point[0].toDouble(), point[1].toDouble());
    }
    return polygon.size() == 4 ? polygon : QPolygonF{};
}

double boundingBoxIoU(const QPolygonF& left, const QPolygonF& right)
{
    if (left.size() != 4 || right.size() != 4)
        return 0.0;
    const QRectF leftBox = left.boundingRect();
    const QRectF rightBox = right.boundingRect();
    const QRectF intersection = leftBox.intersected(rightBox);
    const double unionArea = leftBox.width() * leftBox.height()
        + rightBox.width() * rightBox.height()
        - intersection.width() * intersection.height();
    return unionArea > 0.0
        ? intersection.width() * intersection.height() / unionArea : 0.0;
}

quint64 differenceHash(const QImage& image)
{
    const QImage tiny = image.convertToFormat(QImage::Format_Grayscale8)
        .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    quint64 hash = 0;
    int bitIndex = 0;
    for (int y = 0; y < 8; ++y) {
        const uchar* row = tiny.constScanLine(y);
        for (int x = 0; x < 8; ++x, ++bitIndex)
            hash |= static_cast<quint64>(row[x] > row[x + 1]) << bitIndex;
    }
    return hash;
}

struct Fingerprint
{
    quint64 hash = 0;
    QString split;
    QString image;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QRCodeScannerBenchmark"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({{"m", "manifest"}, "JSONL dataset manifest", "path"});
    parser.addOption({{"o", "output"}, "Write JSON summary", "path"});
    parser.addOption({{"n", "iterations"}, "Decode iterations per image", "count", "1"});
    parser.addOption({{"b", "budget-ms"}, "Per-image deadline", "milliseconds", "150"});
    parser.process(application);
    if (!parser.isSet("manifest"))
        parser.showHelp(2);

    QFile manifest(parser.value("manifest"));
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical("Could not open manifest");
        return 2;
    }
    const QDir baseDirectory = QFileInfo(manifest).absoluteDir();
    const int iterations = std::max(1, parser.value("iterations").toInt());
    const int budgetMs = std::clamp(parser.value("budget-ms").toInt(), 1, 5000);
    int labelled = 0, correct = 0, wrong = 0, noRead = 0, detectOnlyPassed = 0;
    int negativeAttempts = 0, negativeFalsePositives = 0;
    int roiAnnotated = 0, roiLocatedAt50 = 0, roiLocatedAt70 = 0;
    QVector<qint64> latencies;
    QVector<double> bestCandidateIous;
    QMap<QString, int> routes;
    QMap<QString, QVector<qint64>> stageLatencies;
    QVector<Fingerprint> fingerprints;
    QJsonArray records;
    int lineNumber = 0;

    while (!manifest.atEnd()) {
        ++lineNumber;
        const QByteArray line = manifest.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (!document.isObject()) {
            qWarning("Skipping invalid JSON at line %d: %s", lineNumber,
                     qPrintable(parseError.errorString()));
            continue;
        }
        const QJsonObject item = document.object();
        const QString relativePath = item.value(QStringLiteral("image")).toString();
        QImage image;
        if (relativePath == QStringLiteral("generated:datamatrix")
            || relativePath == QStringLiteral("generated:qrcode")) {
            const QString fixtureText = item.value(QStringLiteral("expectedText")).toString();
            const ZXing::BarcodeFormat fixtureFormat = relativePath.endsWith(QStringLiteral("qrcode"))
                ? ZXing::BarcodeFormat::QRCode : ZXing::BarcodeFormat::DataMatrix;
            const auto barcode = ZXing::CreateBarcodeFromText(fixtureText.toStdString(), fixtureFormat);
            const auto generated = ZXing::WriteBarcodeToImage(
                barcode, ZXing::WriterOptions().scale(8).addQuietZones(true));
            image = QImage(generated.data(), generated.width(), generated.height(),
                           generated.width(), QImage::Format_Grayscale8).copy();
        } else {
            const QString imagePath = baseDirectory.absoluteFilePath(relativePath);
            QImageReader reader(imagePath);
            reader.setAutoTransform(true);
            image = reader.read();
        }
        if (image.isNull()) {
            qWarning("Skipping unreadable image: %s", qPrintable(relativePath));
            continue;
        }
        const QString expected = item.value(QStringLiteral("expectedText")).toString();
        const QString expectedOutcome = item.value(QStringLiteral("expectedOutcome")).toString("success");
        const QString split = item.value(QStringLiteral("split")).toString();
        fingerprints.push_back({differenceHash(image), split, relativePath});
        const QPolygonF groundTruthCorners = polygonFromJson(
            item.value(QStringLiteral("groundTruthCorners")));
        if (!expected.isEmpty())
            ++labelled;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            DecodeRequest request;
            request.image = image;
            request.recipe.timeBudgetMs = budgetMs;
            request.recipe.expectedDataMatrixRows = item.value(QStringLiteral("rows")).toInt();
            request.recipe.expectedDataMatrixColumns = item.value(QStringLiteral("columns")).toInt();
            request.recipe.payloadRegularExpression = item.value(QStringLiteral("payloadRegex")).toString();
            request.recipe.previousSuccessfulRoute = item.value(
                QStringLiteral("previousSuccessfulRoute")).toString();
            const QJsonArray roi = item.value(QStringLiteral("roi")).toArray();
            if (roi.size() == 4) {
                request.recipe.fixedRoi = QRect(roi[0].toInt(), roi[1].toInt(),
                                                roi[2].toInt(), roi[3].toInt());
            }
            request.recipe.fixedQuadrilateral = polygonFromJson(
                item.value(QStringLiteral("corners")));
            const DecodeReport report = DecodeEngine().decode(request);
            latencies.push_back(report.totalMicroseconds);
            routes[report.route.isEmpty() ? toString(report.status) : report.route]++;
            for (const StageTiming& timing : report.stageTimings)
                stageLatencies[timing.stage].push_back(timing.elapsedMicroseconds);
            if (!expected.isEmpty()) {
                if (report.success && report.text == expected)
                    ++correct;
                else if (report.success)
                    ++wrong;
                else
                    ++noRead;
            } else if (expectedOutcome == QStringLiteral("detect_only")) {
                detectOnlyPassed += report.success || report.failureStage != FailureStage::Locate;
            } else if (expectedOutcome == QStringLiteral("no_read")) {
                ++negativeAttempts;
                if (report.success) {
                    ++negativeFalsePositives;
                    ++wrong;
                }
            }
            double bestCandidateIoU = 0.0;
            if (!groundTruthCorners.isEmpty()) {
                ++roiAnnotated;
                for (const QPolygonF& candidate : report.candidateCorners)
                    bestCandidateIoU = std::max(bestCandidateIoU,
                        boundingBoxIoU(candidate, groundTruthCorners));
                bestCandidateIous.push_back(bestCandidateIoU);
                roiLocatedAt50 += bestCandidateIoU >= 0.50;
                roiLocatedAt70 += bestCandidateIoU >= 0.70;
            }
            QJsonObject record;
            record.insert(QStringLiteral("image"), relativePath);
            record.insert(QStringLiteral("iteration"), iteration);
            record.insert(QStringLiteral("split"), split);
            record.insert(QStringLiteral("batch"), item.value(QStringLiteral("batch")));
            record.insert(QStringLiteral("expectedText"), expected);
            record.insert(QStringLiteral("expectedOutcome"), expectedOutcome);
            record.insert(QStringLiteral("tags"), item.value(QStringLiteral("tags")));
            record.insert(QStringLiteral("status"), toString(report.status));
            record.insert(QStringLiteral("failureStage"), toString(report.failureStage));
            record.insert(QStringLiteral("route"), report.route);
            record.insert(QStringLiteral("latencyUs"), static_cast<double>(report.totalMicroseconds));
            record.insert(QStringLiteral("text"), report.text);
            record.insert(QStringLiteral("failureReason"), report.failureReason);
            record.insert(QStringLiteral("confidence"), report.confidence);
            record.insert(QStringLiteral("estimatedRows"), report.estimatedRows);
            record.insert(QStringLiteral("estimatedColumns"), report.estimatedColumns);
            QJsonObject qualityObject;
            qualityObject.insert(QStringLiteral("meanLuminance"), report.quality.meanLuminance);
            qualityObject.insert(QStringLiteral("contrast"), report.quality.contrast);
            qualityObject.insert(QStringLiteral("saturatedRatio"), report.quality.saturatedRatio);
            qualityObject.insert(QStringLiteral("sharpness"), report.quality.sharpness);
            qualityObject.insert(QStringLiteral("edgeConsistency"), report.quality.edgeConsistency);
            qualityObject.insert(QStringLiteral("pixels"),
                static_cast<double>(report.quality.width) * report.quality.height);
            const QRectF routeRoi = !request.recipe.fixedQuadrilateral.isEmpty()
                ? request.recipe.fixedQuadrilateral.boundingRect()
                : QRectF(request.recipe.fixedRoi);
            const double imageArea = static_cast<double>(report.quality.width)
                * report.quality.height;
            qualityObject.insert(QStringLiteral("roiFraction"),
                routeRoi.isValid() && imageArea > 0.0
                    ? routeRoi.width() * routeRoi.height() / imageArea : 1.0);
            qualityObject.insert(QStringLiteral("previousSuccessfulRoute"),
                                 request.recipe.previousSuccessfulRoute);
            record.insert(QStringLiteral("quality"), qualityObject);
            if (!groundTruthCorners.isEmpty())
                record.insert(QStringLiteral("bestCandidateIoU"), bestCandidateIoU);
            QJsonArray candidateCornerArray;
            for (const QPolygonF& polygon : report.candidateCorners) {
                QJsonArray polygonArray;
                for (const QPointF& point : polygon) {
                    QJsonArray pointArray;
                    pointArray.append(point.x());
                    pointArray.append(point.y());
                    polygonArray.append(pointArray);
                }
                candidateCornerArray.append(polygonArray);
            }
            record.insert(QStringLiteral("candidateCorners"), candidateCornerArray);
            QJsonObject timingObject;
            for (const StageTiming& timing : report.stageTimings)
                timingObject.insert(timing.stage, static_cast<double>(timing.elapsedMicroseconds));
            record.insert(QStringLiteral("stageTimingsUs"), timingObject);
            QJsonArray attemptArray;
            for (const DecodeAttempt& attempt : report.attempts) {
                QJsonObject attemptObject;
                attemptObject.insert(QStringLiteral("name"), attempt.name);
                attemptObject.insert(QStringLiteral("success"), attempt.success);
                attemptObject.insert(QStringLiteral("latencyUs"),
                                     static_cast<double>(attempt.elapsedMicroseconds));
                attemptObject.insert(QStringLiteral("diagnostic"), attempt.diagnostic);
                attemptArray.append(attemptObject);
            }
            record.insert(QStringLiteral("attempts"), attemptArray);
            records.append(record);
        }
    }

    QJsonObject summary;
    summary.insert(QStringLiteral("labelledImages"), labelled);
    summary.insert(QStringLiteral("iterations"), iterations);
    summary.insert(QStringLiteral("correct"), correct);
    summary.insert(QStringLiteral("wrong"), wrong);
    summary.insert(QStringLiteral("noRead"), noRead);
    summary.insert(QStringLiteral("detectOnlyPassed"), detectOnlyPassed);
    summary.insert(QStringLiteral("negativeAttempts"), negativeAttempts);
    summary.insert(QStringLiteral("negativeFalsePositives"), negativeFalsePositives);
    summary.insert(QStringLiteral("negativeFalsePositiveRate"), negativeAttempts > 0
        ? static_cast<double>(negativeFalsePositives) / negativeAttempts : 0.0);
    summary.insert(QStringLiteral("firstReadRate"), labelled > 0
        ? static_cast<double>(correct) / (labelled * iterations) : 0.0);
    summary.insert(QStringLiteral("misreadRate"), labelled > 0
        ? static_cast<double>(wrong) / (labelled * iterations) : 0.0);
    summary.insert(QStringLiteral("noReadRate"), labelled > 0
        ? static_cast<double>(noRead) / (labelled * iterations) : 0.0);
    summary.insert(QStringLiteral("p50Ms"), percentile(latencies, 0.50));
    summary.insert(QStringLiteral("p95Ms"), percentile(latencies, 0.95));
    summary.insert(QStringLiteral("roiAnnotatedAttempts"), roiAnnotated);
    summary.insert(QStringLiteral("roiRecallAt50"), roiAnnotated > 0
        ? static_cast<double>(roiLocatedAt50) / roiAnnotated : 0.0);
    summary.insert(QStringLiteral("roiRecallAt70"), roiAnnotated > 0
        ? static_cast<double>(roiLocatedAt70) / roiAnnotated : 0.0);
    summary.insert(QStringLiteral("roiBestIoUP50"), percentileDouble(bestCandidateIous, 0.50));
    QJsonObject stageP95Object;
    for (auto it = stageLatencies.cbegin(); it != stageLatencies.cend(); ++it)
        stageP95Object.insert(it.key(), percentile(it.value(), 0.95));
    summary.insert(QStringLiteral("stageP95Ms"), stageP95Object);
    QJsonArray nearDuplicateLeakage;
    for (int left = 0; left < fingerprints.size(); ++left) {
        for (int right = left + 1; right < fingerprints.size(); ++right) {
            if (fingerprints[left].split.isEmpty()
                || fingerprints[right].split.isEmpty()
                || fingerprints[left].split == fingerprints[right].split) {
                continue;
            }
            const int distance = std::popcount(
                fingerprints[left].hash ^ fingerprints[right].hash);
            if (distance <= 5) {
                QJsonObject duplicate;
                duplicate.insert(QStringLiteral("left"), fingerprints[left].image);
                duplicate.insert(QStringLiteral("leftSplit"), fingerprints[left].split);
                duplicate.insert(QStringLiteral("right"), fingerprints[right].image);
                duplicate.insert(QStringLiteral("rightSplit"), fingerprints[right].split);
                duplicate.insert(QStringLiteral("hammingDistance"), distance);
                nearDuplicateLeakage.append(duplicate);
            }
        }
    }
    summary.insert(QStringLiteral("nearDuplicateLeakage"), nearDuplicateLeakage);
    summary.insert(QStringLiteral("nearDuplicateLeakageCount"), nearDuplicateLeakage.size());
    QJsonObject routeObject;
    for (auto it = routes.cbegin(); it != routes.cend(); ++it)
        routeObject.insert(it.key(), it.value());
    summary.insert(QStringLiteral("routes"), routeObject);
    summary.insert(QStringLiteral("records"), records);

    QTextStream(stdout) << QJsonDocument(summary).toJson(QJsonDocument::Indented);
    if (parser.isSet("output")) {
        QFile output(parser.value("output"));
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return 3;
        output.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    }
    if (wrong != 0)
        return 4;
    return nearDuplicateLeakage.isEmpty() ? 0 : 5;
}
