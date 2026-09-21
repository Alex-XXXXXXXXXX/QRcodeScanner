#pragma once

#include <QImage>
#include <QMetaType>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QVector>

enum class DecodeStatus { Success, NoRead, Timeout, Conflict, InvalidInput };
enum class FailureStage { None, Input, Locate, Sample, Correct, Validate, Deadline };

struct DecodeRecipe
{
    QString id = QStringLiteral("default");
    QRect fixedRoi;
    QPolygonF fixedQuadrilateral;
    bool allowQrCode = true;
    bool allowDataMatrix = true;
    bool allowInverted = true;
    bool allowMultipleSymbols = false;
    int maximumSymbols = 16;
    int expectedDataMatrixRows = 0;
    int expectedDataMatrixColumns = 0;
    int maximumRoiCandidates = 3;
    int timeBudgetMs = 150;
    int l0BudgetMs = 20;
    int roiBudgetMs = 25;
    int l1L2BudgetMs = 65;
    int l3BudgetMs = 40;
    QString payloadRegularExpression;
    QString previousSuccessfulRoute;
};

struct DecodeRequest
{
    QImage image;
    DecodeRecipe recipe;
    quint64 sequence = 0;
    bool collectDebugArtifacts = false;
};

struct ImageQuality
{
    int width = 0;
    int height = 0;
    double meanLuminance = 0.0;
    double contrast = 0.0;
    double saturatedRatio = 0.0;
    double sharpness = 0.0;
    double edgeConsistency = 0.0;
    QString summary() const;
};

struct DecodeAttempt
{
    QString name;
    qint64 elapsedMicroseconds = 0;
    bool success = false;
    QString diagnostic;
};

struct StageTiming
{
    QString stage;
    qint64 elapsedMicroseconds = 0;
};

struct DecodedSymbol
{
    QString text;
    QString format;
    QVector<QPointF> corners;
};

struct DecodeReport
{
    bool success = false;
    DecodeStatus status = DecodeStatus::NoRead;
    FailureStage failureStage = FailureStage::None;
    QString text;
    QString format;
    QString route;
    QString failureReason;
    qint64 totalMicroseconds = 0;
    ImageQuality quality;
    QVector<DecodeAttempt> attempts;
    QVector<StageTiming> stageTimings;
    QVector<DecodedSymbol> symbols;
    QVector<QPointF> corners;
    QVector<QPolygonF> candidateCorners;
    double confidence = 0.0;
    int estimatedRows = 0;
    int estimatedColumns = 0;
    bool deadlineExceeded = false;
};

QString toString(DecodeStatus status);
QString toString(FailureStage stage);

Q_DECLARE_METATYPE(DecodeReport)
