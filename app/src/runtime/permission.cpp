#include "runtime/permission.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace permission {
namespace {

// The super-password. Hard-coded on purpose — this isn't a strong secret
// (anyone who dumps strings from the exe will find it), it's a gate to
// tell "official" installs from casual ones without an admin UI.
constexpr const char* kSuperPassword = "70786967856fdgsdfsdf";

// File name looked up in the exe's directory. Named plainly ("config") so
// operators grepping for the mechanism see "oh, some config file" rather
// than "aha, this file is the key" — matches user's UI hiding intent.
constexpr const char* kConfigFileName = "config";

} // namespace

bool HasControlPermission() {
    // No cache — Connect clicks are infrequent, reading a ~20 byte file
    // costs microseconds, and "I added config but still no perm" without
    // restarting is a bad UX trap. Read fresh every call.
    const QString exePath = QCoreApplication::applicationFilePath();
    const QString dir = QFileInfo(exePath).absolutePath();
    const QString cfgPath = QDir(dir).filePath(
        QString::fromLatin1(kConfigFileName));
    QFile f(cfgPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    // Read up to 1 MB — the password is 20 bytes; anything bigger than
    // a few hundred bytes is already suspicious.
    const QByteArray content = f.read(1024 * 1024);
    return content.contains(QByteArray::fromRawData(
        kSuperPassword,
        static_cast<int>(std::char_traits<char>::length(kSuperPassword))));
}

} // namespace permission
