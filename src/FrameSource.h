#pragma once

#include <QImage>
#include <QString>
#include <QVector>

struct FramePacket
{
    QImage image;
    quint64 sequence = 0;
    qint64 timestampMicroseconds = 0;
    double exposureMicroseconds = 0.0;
    quint64 triggerId = 0;
    QString sourceId;
};

class IFrameSource
{
public:
    virtual ~IFrameSource() = default;
    virtual bool open(QString* errorMessage = nullptr) = 0;
    virtual void close() = 0;
    virtual bool nextFrame(FramePacket& packet, int timeoutMs,
                           QString* errorMessage = nullptr) = 0;
};

class LocalFileFrameSource final : public IFrameSource
{
public:
    explicit LocalFileFrameSource(QString filePath);
    bool open(QString* errorMessage = nullptr) override;
    void close() override;
    bool nextFrame(FramePacket& packet, int timeoutMs,
                   QString* errorMessage = nullptr) override;

private:
    QString filePath_;
    QImage image_;
    bool delivered_ = false;
};

struct CameraAcquisitionRecipe
{
    QVector<double> exposureMicroseconds{500.0};
    int maximumFramesPerTrigger = 2;
    QString lightingRecipeId;
    QString triggerSource = QStringLiteral("PLC");
    bool requireGlobalShutter = true;
    double strobeDelayMicroseconds = 0.0;
    double strobeDurationMicroseconds = 50.0;
};

class IIndustrialCameraBackend
{
public:
    virtual ~IIndustrialCameraBackend() = default;
    virtual bool open(QString* errorMessage) = 0;
    virtual bool configure(const CameraAcquisitionRecipe& recipe,
                           QString* errorMessage) = 0;
    virtual void close() = 0;
    virtual bool waitForTrigger(quint64& triggerId, int timeoutMs,
                                QString* errorMessage) = 0;
    virtual bool capture(double exposureMicroseconds, const QString& lightingRecipeId,
                         FramePacket& packet, QString* errorMessage) = 0;
};

class IndustrialCameraFrameSource final : public IFrameSource
{
public:
    IndustrialCameraFrameSource(IIndustrialCameraBackend& backend,
                                CameraAcquisitionRecipe recipe = {});
    bool open(QString* errorMessage = nullptr) override;
    void close() override;
    bool nextFrame(FramePacket& packet, int timeoutMs,
                   QString* errorMessage = nullptr) override;

private:
    IIndustrialCameraBackend& backend_;
    CameraAcquisitionRecipe recipe_;
    quint64 sequence_ = 0;
};
