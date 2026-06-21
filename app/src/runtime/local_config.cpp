#include "runtime/local_config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {

QString configFilePath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    return base + "/local.json";
}

QString randomDeviceId() {
    // 9-digit decimal — matches the RustDesk feel and is short enough to
    // type / dictate over the phone.
    auto* gen = QRandomGenerator::global();
    quint64 raw = gen->generate64() % 1000000000ULL;
    if (raw < 100000000ULL) raw += 100000000ULL;
    return QString::number(raw);
}

QString randomVerifyCode() {
    static const char kAlphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // skip O/0, I/1 for legibility
    constexpr int kLen = 8;
    constexpr int kAlphaLen = sizeof(kAlphabet) - 1;
    QString out;
    out.reserve(kLen);
    auto* gen = QRandomGenerator::global();
    for (int i = 0; i < kLen; ++i) {
        out.append(QLatin1Char(kAlphabet[gen->bounded(kAlphaLen)]));
    }
    return out;
}

bool tryRead(const QString& path, LocalConfig* out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto bytes = f.readAll();
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const auto obj = doc.object();
    out->deviceId   = obj.value("device_id").toString();
    out->verifyCode = obj.value("verify_code").toString();
    return !out->deviceId.isEmpty() && !out->verifyCode.isEmpty();
}

void write(const QString& path, const LocalConfig& cfg) {
    QJsonObject obj;
    obj["device_id"]   = cfg.deviceId;
    obj["verify_code"] = cfg.verifyCode;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    }
}

} // namespace

LocalConfig LoadOrCreateLocalConfig() {
    LocalConfig cfg;
    const QString path = configFilePath();
    if (tryRead(path, &cfg)) return cfg;

    cfg.deviceId   = randomDeviceId();
    cfg.verifyCode = randomVerifyCode();
    write(path, cfg);
    return cfg;
}
