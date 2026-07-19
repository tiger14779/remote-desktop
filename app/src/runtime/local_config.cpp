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
    // 8-digit decimal: 10000000..99999999 (90M IDs, ~26-bit entropy). Chosen
    // over the earlier 5-digit format because operators kept hitting
    // duplicates once the pool grew past a few hundred machines.
    auto* gen = QRandomGenerator::global();
    const quint64 raw = (gen->generate64() % 90000000ULL) + 10000000ULL;
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
    // KMBox settings — missing keys are fine on upgrade from an older
    // config file; they default to "not configured" via LocalConfig's
    // in-struct initialisers, and we simply don't overwrite them here.
    if (obj.contains("kmbox_ip"))
        out->kmboxIp = obj.value("kmbox_ip").toString();
    if (obj.contains("kmbox_port"))
        out->kmboxPort = static_cast<quint16>(obj.value("kmbox_port").toInt());
    if (obj.contains("kmbox_uuid"))
        out->kmboxUuid = obj.value("kmbox_uuid").toString();
    if (obj.contains("kmbox_enabled"))
        out->kmboxEnabled = obj.value("kmbox_enabled").toBool();
    if (obj.contains("force_tcp_video"))
        out->forceTcpVideo = obj.value("force_tcp_video").toBool();

    // Reject pre-existing IDs that aren't the current 8-digit format so the
    // upgrade silently re-rolls. Old 5-digit and 9-digit IDs both get
    // reissued; there's no history to preserve — connecting old viewers to
    // new agents still works as long as both run the upgraded build.
    if (out->deviceId.length() != 8) return false;
    return !out->deviceId.isEmpty();
}

} // namespace

void SaveLocalConfig(const LocalConfig& cfg) {
    QJsonObject obj;
    obj["device_id"] = cfg.deviceId;
    QJsonArray arr;
    for (const auto& s : cfg.recentRemoteIds) arr.append(s);
    obj["recent_remote_ids"] = arr;
    obj["kmbox_ip"]      = cfg.kmboxIp;
    obj["kmbox_port"]    = int(cfg.kmboxPort);
    obj["kmbox_uuid"]    = cfg.kmboxUuid;
    obj["kmbox_enabled"] = cfg.kmboxEnabled;
    obj["force_tcp_video"] = cfg.forceTcpVideo;
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
