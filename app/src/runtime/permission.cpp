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
    // Cache: -1 = unknown, 0 = no, 1 = yes. Single-threaded init OK
    // because the first caller is AppMainWindow::onConnectClicked on the
    // GUI thread and later callers hit the same thread.
    static int cached = -1;
    if (cached != -1) return cached == 1;

    const QString exePath = QCoreApplication::applicationFilePath();
    const QString dir = QFileInfo(exePath).absolutePath();
    const QString cfgPath = QDir(dir).filePath(
        QString::fromLatin1(kConfigFileName));
    QFile f(cfgPath);
    if (!f.open(QIODevice::ReadOnly)) {
        cached = 0;
        return false;
    }
    // Read up to 1 MB just so a giant file dropped in by mistake doesn't
    // eat memory. The password is 20 bytes; anything bigger than a few
    // hundred bytes is already suspicious.
    const QByteArray content = f.read(1024 * 1024);
    cached = content.contains(QByteArray::fromRawData(
        kSuperPassword,
        static_cast<int>(std::char_traits<char>::length(kSuperPassword))))
        ? 1 : 0;
    return cached == 1;
}

} // namespace permission
