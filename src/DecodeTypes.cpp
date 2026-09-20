#include "DecodeTypes.h"

QString toString(DecodeStatus status)
{
    switch (status) {
    case DecodeStatus::Success: return QStringLiteral("success");
    case DecodeStatus::NoRead: return QStringLiteral("no_read");
    case DecodeStatus::Timeout: return QStringLiteral("timeout");
    case DecodeStatus::Conflict: return QStringLiteral("conflict");
    case DecodeStatus::InvalidInput: return QStringLiteral("invalid_input");
    }
    return QStringLiteral("unknown");
}

QString toString(FailureStage stage)
{
    switch (stage) {
    case FailureStage::None: return QStringLiteral("none");
    case FailureStage::Input: return QStringLiteral("input");
    case FailureStage::Locate: return QStringLiteral("locate");
    case FailureStage::Sample: return QStringLiteral("sample");
    case FailureStage::Correct: return QStringLiteral("correct");
    case FailureStage::Validate: return QStringLiteral("validate");
    case FailureStage::Deadline: return QStringLiteral("deadline");
    }
    return QStringLiteral("unknown");
}
