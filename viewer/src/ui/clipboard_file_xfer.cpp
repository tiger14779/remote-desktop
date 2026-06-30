#include "ui/clipboard_file_xfer.h"

#include "net/frame.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cstring>

namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;

struct FileEntry {
    QString path;     // relative, posix slashes
    qint64  size = 0;
    bool    isDir = false;
};

// Walks a list of URLs (top-level entries the user copied) and yields a flat
// file list. Directories are recursed; subdirectories appear as isDir=true
// entries so the receiver can mkpath them even if they're empty.
std::vector<FileEntry> enumerate(const QList<QUrl>& urls) {
    std::vector<FileEntry> out;
    for (const QUrl& u : urls) {
        if (!u.isLocalFile()) continue;
        const QString abs = u.toLocalFile();
        QFileInfo info(abs);
        if (!info.exists()) continue;

        if (info.isDir()) {
            const QString root = info.absoluteFilePath();
            const QString topName = info.fileName();
            FileEntry top;
            top.path = topName;
            top.isDir = true;
            out.push_back(top);

            QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QFileInfo fi = it.fileInfo();
                const QString rel = topName + QLatin1Char('/') +
                                    QDir(root).relativeFilePath(fi.absoluteFilePath());
                FileEntry e;
                e.path = rel;
                e.isDir = fi.isDir();
                e.size  = fi.isDir() ? 0 : fi.size();
                out.push_back(e);
            }
        } else {
            FileEntry e;
            e.path = info.fileName();
            e.size = info.size();
            out.push_back(e);
        }
    }
    return out;
}

QByteArray encodeBegin(const std::vector<FileEntry>& files) {
    QJsonArray arr;
    for (const auto& f : files) {
        QJsonObject obj;
        obj["path"] = f.path;
        obj["size"] = static_cast<double>(f.size);
        obj["isDir"] = f.isDir;
        arr.append(obj);
    }
    QJsonObject root;
    root["files"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool decodeBegin(const std::uint8_t* data, std::size_t size,
                 std::vector<FileEntry>* out) {
    auto doc = QJsonDocument::fromJson(QByteArray::fromRawData(
        reinterpret_cast<const char*>(data), static_cast<int>(size)));
    if (!doc.isObject()) return false;
    const auto arr = doc.object().value("files").toArray();
    out->clear();
    for (const auto& v : arr) {
        const auto obj = v.toObject();
        FileEntry e;
        e.path  = obj.value("path").toString();
        e.size  = static_cast<qint64>(obj.value("size").toDouble());
        e.isDir = obj.value("isDir").toBool();
        if (e.path.isEmpty() || e.path.contains("..")) return false;  // path-traversal guard
        out->push_back(e);
    }
    return true;
}

} // namespace

struct ClipboardFileXfer::Impl {
    SendFn send;

    // Send state — used while a transfer is being pushed out. We keep file
    // handles, source paths, and the running index so chunking can pace
    // itself across many onPeerFrame invocations if we ever go async (today
    // we drain a transfer synchronously inside onLocalClipboardChanged).

    // Receive state.
    bool         rxActive = false;
    QString      rxRoot;             // absolute path of this xfer's temp dir
    std::vector<FileEntry> rxFiles;  // parsed from Begin
    std::size_t  rxFileIdx = 0;      // currently-receiving file index
    qint64       rxFileBytesRem = 0; // bytes still expected for rxFiles[rxFileIdx]
    std::unique_ptr<QFile> rxFile;   // open handle for rxFiles[rxFileIdx]

    // Set of temp roots we've created. The QClipboard handler skips
    // outgoing transfers whose URLs all live under one of these — that's
    // how we break the echo loop when we just set the clipboard ourselves
    // after a successful inbound transfer.
    std::set<QString> ourTempRoots;
};

ClipboardFileXfer::ClipboardFileXfer(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

ClipboardFileXfer::~ClipboardFileXfer() = default;

void ClipboardFileXfer::setSendFn(SendFn fn) { impl_->send = std::move(fn); }

void ClipboardFileXfer::onLocalClipboardChanged() {
    auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) return;
    if (!impl_->send) return;

    const auto urls = mime->urls();
    if (urls.isEmpty()) return;

    // Loop-break: if all URLs live under a temp root we created on this
    // process, the clipboard change came from our own setMimeData and we
    // must not echo back.
    bool allOurs = true;
    for (const auto& u : urls) {
        if (!u.isLocalFile()) { allOurs = false; break; }
        const QString abs = QFileInfo(u.toLocalFile()).absoluteFilePath();
        bool matched = false;
        for (const QString& root : impl_->ourTempRoots) {
            if (abs.startsWith(root + QLatin1Char('/'))) { matched = true; break; }
        }
        if (!matched) { allOurs = false; break; }
    }
    if (allOurs) return;

    const auto entries = enumerate(urls);
    if (entries.empty()) return;

    qint64 totalBytes = 0;
    int fileCount = 0;
    for (const auto& e : entries) {
        if (!e.isDir) { totalBytes += e.size; ++fileCount; }
    }
    emit log(QStringLiteral("[xfer] sending %1 file(s), %2 KB total")
                 .arg(fileCount).arg(totalBytes / 1024));

    // BEGIN
    const QByteArray beginJson = encodeBegin(entries);
    impl_->send(proto::MsgFileXferBegin,
                reinterpret_cast<const std::uint8_t*>(beginJson.constData()),
                static_cast<std::size_t>(beginJson.size()));

    // CHUNKS — open the on-disk file for each non-directory entry and
    // pump in 64KB slices. Done synchronously on the GUI thread; for the
    // multi-GB case we'd want a worker thread, but copy/paste workflows
    // are typically small enough to ride the GUI thread for now.
    QByteArray buf;
    buf.resize(static_cast<int>(kChunkBytes));
    for (const auto& e : entries) {
        if (e.isDir) continue;
        // Resolve real source path: we need to map relative `e.path` back
        // to an absolute path on disk. Top-level entries match a URL; for
        // nested files we walk from the URL of their containing root.
        QString absSrc;
        for (const QUrl& u : urls) {
            if (!u.isLocalFile()) continue;
            QFileInfo top(u.toLocalFile());
            if (top.isDir()) {
                if (e.path.startsWith(top.fileName() + QLatin1Char('/'))) {
                    const QString rest = e.path.mid(top.fileName().size() + 1);
                    absSrc = QDir(top.absoluteFilePath()).filePath(rest);
                    break;
                }
            } else if (top.fileName() == e.path) {
                absSrc = top.absoluteFilePath();
                break;
            }
        }
        if (absSrc.isEmpty()) continue;

        QFile f(absSrc);
        if (!f.open(QIODevice::ReadOnly)) {
            emit log(QStringLiteral("[xfer] open failed: %1").arg(absSrc));
            continue;
        }
        while (!f.atEnd()) {
            const qint64 n = f.read(buf.data(), kChunkBytes);
            if (n <= 0) break;
            impl_->send(proto::MsgFileXferChunk,
                        reinterpret_cast<const std::uint8_t*>(buf.constData()),
                        static_cast<std::size_t>(n));
        }
    }

    // END
    const QByteArray endJson = QByteArrayLiteral("{\"ok\":true}");
    impl_->send(proto::MsgFileXferEnd,
                reinterpret_cast<const std::uint8_t*>(endJson.constData()),
                static_cast<std::size_t>(endJson.size()));
    emit log(QStringLiteral("[xfer] send complete"));
}

void ClipboardFileXfer::onPeerFrame(std::uint8_t type,
                                     const std::uint8_t* data,
                                     std::size_t size) {
    if (type == proto::MsgFileXferBegin) {
        // Start fresh: drop any in-flight rx state, allocate a new temp dir.
        impl_->rxFile.reset();
        impl_->rxActive = false;
        if (!decodeBegin(data, size, &impl_->rxFiles)) {
            emit log(QStringLiteral("[xfer] bad BEGIN payload, ignoring"));
            return;
        }
        const QString tempBase = QStandardPaths::writableLocation(
            QStandardPaths::TempLocation) + QStringLiteral("/remote-desktop");
        QDir().mkpath(tempBase);
        const QString stamp = QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        impl_->rxRoot = QDir(tempBase).filePath(QStringLiteral("xfer-") + stamp);
        QDir().mkpath(impl_->rxRoot);
        impl_->ourTempRoots.insert(impl_->rxRoot);

        // Pre-create directory entries so we can skip them when chunking.
        for (const auto& e : impl_->rxFiles) {
            if (e.isDir) {
                QDir().mkpath(impl_->rxRoot + QLatin1Char('/') + e.path);
            }
        }

        impl_->rxFileIdx = 0;
        impl_->rxFileBytesRem = 0;
        impl_->rxActive = true;
        emit log(QStringLiteral("[xfer] receiving %1 entries into %2")
                     .arg(impl_->rxFiles.size()).arg(impl_->rxRoot));
        return;
    }

    if (type == proto::MsgFileXferChunk) {
        if (!impl_->rxActive) return;
        // Advance through any zero-byte / directory entries before writing.
        auto openNext = [this]() {
            while (impl_->rxFileIdx < impl_->rxFiles.size()) {
                const auto& e = impl_->rxFiles[impl_->rxFileIdx];
                if (e.isDir) {
                    ++impl_->rxFileIdx;
                    continue;
                }
                // open file
                const QString abs = impl_->rxRoot + QLatin1Char('/') + e.path;
                QDir().mkpath(QFileInfo(abs).absolutePath());
                impl_->rxFile = std::make_unique<QFile>(abs);
                if (!impl_->rxFile->open(QIODevice::WriteOnly)) {
                    emit log(QStringLiteral("[xfer] create failed: %1").arg(abs));
                    impl_->rxFile.reset();
                    ++impl_->rxFileIdx;
                    continue;
                }
                impl_->rxFileBytesRem = e.size;
                if (impl_->rxFileBytesRem == 0) {
                    impl_->rxFile.reset();
                    ++impl_->rxFileIdx;
                    continue;
                }
                return true;
            }
            return false;
        };

        std::size_t remaining = size;
        const std::uint8_t* p = data;
        while (remaining > 0) {
            if (!impl_->rxFile || impl_->rxFileBytesRem == 0) {
                if (impl_->rxFile) {
                    impl_->rxFile->close();
                    impl_->rxFile.reset();
                    ++impl_->rxFileIdx;
                }
                if (!openNext()) {
                    emit log(QStringLiteral("[xfer] extra chunk bytes, dropping %1")
                                 .arg(remaining));
                    return;
                }
            }
            const std::size_t take = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(impl_->rxFileBytesRem));
            const qint64 written = impl_->rxFile->write(
                reinterpret_cast<const char*>(p), static_cast<qint64>(take));
            if (written <= 0) {
                emit log(QStringLiteral("[xfer] write failed"));
                return;
            }
            p += written;
            remaining -= static_cast<std::size_t>(written);
            impl_->rxFileBytesRem -= written;
        }
        return;
    }

    if (type == proto::MsgFileXferEnd) {
        if (!impl_->rxActive) return;
        // Close any in-progress file.
        if (impl_->rxFile) {
            impl_->rxFile->close();
            impl_->rxFile.reset();
        }
        impl_->rxActive = false;

        // Build the absolute path list for top-level entries (matches what
        // a real Explorer copy would put on the clipboard).
        QStringList topPaths;
        for (const auto& e : impl_->rxFiles) {
            if (e.path.contains(QLatin1Char('/'))) continue;
            topPaths.append(impl_->rxRoot + QLatin1Char('/') + e.path);
        }
        emit log(QStringLiteral("[xfer] recv done, %1 top-level entries")
                     .arg(topPaths.size()));

        // Hop to the GUI thread for QClipboard interaction (Qt enforces
        // single-thread access to the clipboard).
        QMetaObject::invokeMethod(
            this, "applyReceivedFiles", Qt::QueuedConnection,
            Q_ARG(QString, impl_->rxRoot),
            Q_ARG(QStringList, topPaths));
        return;
    }
}

void ClipboardFileXfer::applyReceivedFiles(const QString& tempRoot,
                                            const QStringList& absPaths) {
    Q_UNUSED(tempRoot);
    if (absPaths.isEmpty()) return;
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    urls.reserve(absPaths.size());
    for (const QString& p : absPaths) urls.append(QUrl::fromLocalFile(p));
    mime->setUrls(urls);
    QApplication::clipboard()->setMimeData(mime);
    emit log(QStringLiteral("[xfer] clipboard set, ready to paste"));
}
