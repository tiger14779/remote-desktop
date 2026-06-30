#include "runtime/local_config.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
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
    // 5-digit decimal: 10000..99999 (90000 IDs, ~17-bit entropy — fine for
    // a ten-people internal deployment where collisions are resolved by
    // the operator hitting "new id" in practice).
    auto* gen = QRandomGenerator::global();
    const quint64 raw = (gen->generate64() % 90000ULL) + 10000ULL;
    return QString::number(raw);
}

bool tryRead(const QString& path, LocalConfig* out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto bytes = f.readAll();
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const auto obj = doc.object();
    out->deviceId = obj.value("device_id").toString();
    out->recentRemoteIds.clear();
    for (const auto& v : obj.value("recent_remote_ids").toArray()) {
        const QString s = v.toString();
        if (!s.isEmpty()) out->recentRemoteIds.append(s);
    }
    // Reject pre-existing 9-digit IDs from the previous schema so the
    // upgrade silently re-rolls to the new 5-digit format. There's no
    // history to preserve — connecting old viewers to new agents still
    // works as long as both run the upgraded build.
    if (out->deviceId.length() != 5) return false;
    return !out->deviceId.isEmpty();
}

} // namespace

void SaveLocalConfig(const LocalConfig& cfg) {
    QJsonObject obj;
    obj["device_id"] = cfg.deviceId;
    QJsonArray arr;
    for (const auto& s : cfg.recentRemoteIds) arr.append(s);
    obj["recent_remote_ids"] = arr;
    QFile f(configFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    }
}

LocalConfig LoadOrCreateLocalConfig() {
    LocalConfig cfg;
    const QString path = configFilePath();
    if (tryRead(path, &cfg)) return cfg;

    cfg.deviceId = randomDeviceId();
    SaveLocalConfig(cfg);
    return cfg;
}
