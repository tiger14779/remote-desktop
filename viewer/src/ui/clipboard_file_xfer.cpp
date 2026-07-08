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
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;

// Emit progress log lines no more often than this on the send side. Any
// finer and a 50 MB folder floods the log panel with hundreds of nearly-
// identical lines.
constexpr int kProgressLogIntervalMs = 500;

// Delete xfer-* temp dirs older than this on startup. Long enough that
// dragging into Explorer minutes later still works, short enough that
// the temp base doesn't turn into a de facto multi-GB backup of every
// remote file the user has ever pasted.
constexpr qint64 kTempMaxAgeSeconds = 24 * 60 * 60;  // 24h

struct FileEntry {
    QString path;     // relative, posix slashes
    qint64  size = 0;
    bool    isDir = false;
    QString absSrc;   // absolute source path — resolved once on the GUI
                      // thread so the worker never has to interpret URLs
};

// Walks a list of URLs (top-level entries the user copied) and yields a flat
// file list. Directories are recursed; subdirectories appear as isDir=true
// entries so the receiver can mkpath them even if they're empty. Also
// captures each entry's absolute source path so the send worker can open
// files without re-walking the URL list.
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
            {
                FileEntry top;
                top.path = topName;
                top.isDir = true;
                top.absSrc = root;
                out.push_back(top);
            }

            QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QFileInfo fi = it.fileInfo();
                const QString rel = topName + QLatin1Char('/') +
                                    QDir(root).relativeFilePath(fi.absoluteFilePath());
                FileEntry e;
                e.path   = rel;
                e.isDir  = fi.isDir();
                e.size   = fi.isDir() ? 0 : fi.size();
                e.absSrc = fi.absoluteFilePath();
                out.push_back(e);
            }
        } else {
            FileEntry e;
            e.path   = info.fileName();
            e.size   = info.size();
            e.absSrc = info.absoluteFilePath();
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

QString tempBase() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
           QStringLiteral("/remote-desktop");
}

// Best-effort cleanup of aged transfer dirs. Runs once per process on the
// first Impl construction — a cheap way to keep the temp base bounded
// without adding a background sweeper.
void pruneOldTempDirs() {
    QDir base(tempBase());
    if (!base.exists()) return;
    const auto entries = base.entryInfoList(
        QStringList{QStringLiteral("xfer-*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    const QDateTime now = QDateTime::currentDateTime();
    for (const QFileInfo& fi : entries) {
        // Use last-modified rather than birth time: birthTime is unreliable
        // on some Windows volumes (NTFS returns garbage for a copied dir).
        // lastModified moves forward as we write into it, so a still-active
        // transfer will never look aged.
        const qint64 ageSec = fi.lastModified().secsTo(now);
        if (ageSec > kTempMaxAgeSeconds) {
            QDir(fi.absoluteFilePath()).removeRecursively();
        }
    }
}

} // namespace

struct ClipboardFileXfer::Impl {
    SendFn send;

    // Outbound worker. Lives for the object's lifetime; sits parked on
    // sendCv_ waiting for a signal from onLocalClipboardChanged.
    std::thread              sendThread;
    std::mutex               sendMu;
    std::condition_variable  sendCv;
    // Pending outbound job. When set, worker wakes up, sends, clears.
    // We reject a new job while sendActive_ so a repeated Ctrl+C on the
    // sender doesn't fire two overlapping transfers over the same stream.
    std::vector<FileEntry>   pendingSend;
    std::atomic<bool>        sendActive{false};
    std::atomic<bool>        sendCancel{false};
    std::atomic<bool>        exitWorker{false};

    // Receive state.
    bool         rxActive = false;
    QString      rxRoot;             // absolute path of this xfer's temp dir
    std::vector<FileEntry> rxFiles;  // parsed from Begin
    std::size_t  rxFileIdx = 0;      // currently-receiving file index
    qint64       rxFileBytesRem = 0; // bytes still expected for rxFiles[rxFileIdx]
    std::unique_ptr<QFile> rxFile;   // open handle for rxFiles[rxFileIdx]
    qint64       rxTotalBytes = 0;   // sum of non-dir sizes at BEGIN — for percentage
    qint64       rxBytesSoFar = 0;   // running total across files
    qint64       rxLastLogAtMs = 0;  // throttled progress log
    QDateTime    rxStartedAt;

    // Set of temp roots we've created. The QClipboard handler skips
    // outgoing transfers whose URLs all live under one of these — that's
    // how we break the echo loop when we just set the clipboard ourselves
    // after a successful inbound transfer.
    std::set<QString> ourTempRoots;
};

ClipboardFileXfer::ClipboardFileXfer(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    // Housekeeping on construction — cheap, one-shot.
    QDir().mkpath(tempBase());
    pruneOldTempDirs();

    // Spawn the send worker. Uses a QueuedConnection-style hop back to the
    // GUI thread via emit log() so all progress lines end up in the log
    // panel from the right thread.
    impl_->sendThread = std::thread([this]() {
        std::vector<FileEntry> job;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(impl_->sendMu);
                impl_->sendCv.wait(lk, [this]() {
                    return impl_->exitWorker.load() ||
                           !impl_->pendingSend.empty();
                });
                if (impl_->exitWorker.load()) return;
                job = std::move(impl_->pendingSend);
                impl_->pendingSend.clear();
            }
            if (job.empty()) continue;

            impl_->sendActive.store(true);
            impl_->sendCancel.store(false);

            qint64 totalBytes = 0;
            int fileCount = 0;
            for (const auto& e : job) {
                if (!e.isDir) { totalBytes += e.size; ++fileCount; }
            }
            const int dirCount = static_cast<int>(job.size()) - fileCount;
            emit log(QStringLiteral("[xfer] 开始发送 %1 个文件, %2 个目录, 共 %3 KB")
                         .arg(fileCount).arg(dirCount).arg(totalBytes / 1024));
            emit xferStarted(/*sending=*/true, totalBytes, fileCount);

            // BEGIN
            const QByteArray beginJson = encodeBegin(job);
            if (impl_->send) {
                impl_->send(proto::MsgFileXferBegin,
                            reinterpret_cast<const std::uint8_t*>(beginJson.constData()),
                            static_cast<std::size_t>(beginJson.size()));
            }

            // CHUNKS
            QByteArray buf;
            buf.resize(static_cast<int>(kChunkBytes));
            qint64 sentBytes = 0;
            qint64 lastLogMs = QDateTime::currentMSecsSinceEpoch();
            const QDateTime startAt = QDateTime::currentDateTime();

            for (const auto& e : job) {
                if (impl_->sendCancel.load()) break;
                if (e.isDir) continue;

                QFile f(e.absSrc);
                if (!f.open(QIODevice::ReadOnly)) {
                    emit log(QStringLiteral("[xfer] 读取失败: %1").arg(e.absSrc));
                    continue;
                }
                while (!f.atEnd()) {
                    if (impl_->sendCancel.load()) break;
                    const qint64 n = f.read(buf.data(), kChunkBytes);
                    if (n <= 0) break;
                    if (impl_->send) {
                        impl_->send(proto::MsgFileXferChunk,
                                    reinterpret_cast<const std::uint8_t*>(buf.constData()),
                                    static_cast<std::size_t>(n));
                    }
                    sentBytes += n;
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (nowMs - lastLogMs >= kProgressLogIntervalMs) {
                        const int pct = totalBytes > 0
                            ? static_cast<int>((sentBytes * 100) / totalBytes)
                            : 100;
                        emit log(QStringLiteral("[xfer] 发送进度 %1% (%2 / %3 KB)")
                                     .arg(pct)
                                     .arg(sentBytes / 1024)
                                     .arg(totalBytes / 1024));
                        emit xferProgress(/*sending=*/true, sentBytes,
                                          totalBytes, pct);
                        lastLogMs = nowMs;
                    }
                }
            }

            // END
            const QByteArray endJson = impl_->sendCancel.load()
                ? QByteArrayLiteral("{\"ok\":false,\"cancelled\":true}")
                : QByteArrayLiteral("{\"ok\":true}");
            if (impl_->send) {
                impl_->send(proto::MsgFileXferEnd,
                            reinterpret_cast<const std::uint8_t*>(endJson.constData()),
                            static_cast<std::size_t>(endJson.size()));
            }

            const int elapsedSec =
                static_cast<int>(startAt.secsTo(QDateTime::currentDateTime()));
            const bool cancelled = impl_->sendCancel.load();
            if (cancelled) {
                emit log(QStringLiteral("[xfer] 发送已取消 (共 %1 KB 已发出)")
                             .arg(sentBytes / 1024));
            } else {
                emit log(QStringLiteral("[xfer] 发送完成, 共 %1 KB, 用时 %2 秒")
                             .arg(sentBytes / 1024)
                             .arg(elapsedSec));
            }
            emit xferFinished(/*sending=*/true, cancelled, sentBytes);
            impl_->sendActive.store(false);
        }
    });
}

ClipboardFileXfer::~ClipboardFileXfer() {
    if (impl_) {
        {
            std::lock_guard<std::mutex> lk(impl_->sendMu);
            impl_->exitWorker.store(true);
            impl_->sendCancel.store(true);
        }
        impl_->sendCv.notify_all();
        if (impl_->sendThread.joinable()) impl_->sendThread.join();
    }
}

void ClipboardFileXfer::setSendFn(SendFn fn) { impl_->send = std::move(fn); }

void ClipboardFileXfer::onLocalClipboardChanged() {
    // Detection only — never starts a transfer. The old behaviour
    // (auto-send on Ctrl+C) burned WAN bandwidth every time the operator
    // copied a file just to paste it locally, and there was no cancel
    // path. Now the UI shows a "发送到对方" button on the banner and the
    // operator explicitly opts in via sendClipboardNow().
    auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        emit localFilesAvailable(false, 0);
        return;
    }
    const auto urls = mime->urls();
    if (urls.isEmpty()) {
        emit localFilesAvailable(false, 0);
        return;
    }
    // Loop-break: if all URLs live under a temp root we created on this
    // process, the clipboard change came from our own setMimeData
    // (i.e. we just received a transfer and set the clipboard). Don't
    // offer to bounce them back to sender.
    bool allOurs = true;
    for (const auto& u : urls) {
        if (!u.isLocalFile()) { allOurs = false; break; }
        const QString abs = QFileInfo(u.toLocalFile()).absoluteFilePath();
        bool matched = false;
        for (const QString& root : impl_->ourTempRoots) {
            if (abs.startsWith(root + QLatin1Char('/')) || abs == root) {
                matched = true;
                break;
            }
        }
        if (!matched) { allOurs = false; break; }
    }
    if (allOurs) {
        emit localFilesAvailable(false, 0);
        return;
    }
    emit localFilesAvailable(true, urls.size());
}

void ClipboardFileXfer::sendClipboardNow() {
    auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        emit log(QStringLiteral("[xfer] 剪切板里没有文件, 无法发送"));
        return;
    }
    sendFilesNow(mime->urls());
}

void ClipboardFileXfer::sendFilesNow(const QList<QUrl>& urls) {
    if (!impl_->send) return;
    // Concurrency guard: two overlapping BEGIN/CHUNK/END triples on the
    // same stream interleave at the receiver and corrupt both files. The
    // UI hides the send button while sendActive, so this path is mostly
    // defensive against drag-drop firing during an in-flight transfer.
    if (impl_->sendActive.load()) {
        emit log(QStringLiteral("[xfer] 上一个发送任务尚未结束, 已忽略"));
        return;
    }
    if (urls.isEmpty()) return;

    // Loop-break: refuse to bounce files that came from our own receive
    // path (dropping a file we just received back into the video pane).
    bool allOurs = true;
    for (const auto& u : urls) {
        if (!u.isLocalFile()) { allOurs = false; break; }
        const QString abs = QFileInfo(u.toLocalFile()).absoluteFilePath();
        bool matched = false;
        for (const QString& root : impl_->ourTempRoots) {
            if (abs.startsWith(root + QLatin1Char('/')) || abs == root) {
                matched = true;
                break;
            }
        }
        if (!matched) { allOurs = false; break; }
    }
    if (allOurs) {
        emit log(QStringLiteral("[xfer] 这些文件是刚从对方接收的, 忽略回传"));
        return;
    }

    auto entries = enumerate(urls);
    if (entries.empty()) return;

    {
        std::lock_guard<std::mutex> lk(impl_->sendMu);
        impl_->pendingSend = std::move(entries);
    }
    impl_->sendCv.notify_one();
}

void ClipboardFileXfer::cancelActiveTransfer() {
    // Just flip the flag; the worker checks it between files and between
    // 64KB chunks. Not called under sendMu — sendCancel is atomic and
    // race-free with the worker's reads.
    if (impl_->sendActive.load()) {
        impl_->sendCancel.store(true);
        emit log(QStringLiteral("[xfer] 已请求取消当前传输"));
    }
}

void ClipboardFileXfer::onPeerFrame(std::uint8_t type,
                                     const std::uint8_t* data,
                                     std::size_t size) {
    if (type == proto::MsgFileXferBegin) {
        // Start fresh: drop any in-flight rx state, allocate a new temp dir.
        impl_->rxFile.reset();
        impl_->rxActive = false;
        if (!decodeBegin(data, size, &impl_->rxFiles)) {
            emit log(QStringLiteral("[xfer] BEGIN 负载无效, 忽略"));
            return;
        }
        QDir().mkpath(tempBase());
        const QString stamp = QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        impl_->rxRoot = QDir(tempBase()).filePath(QStringLiteral("xfer-") + stamp);
        QDir().mkpath(impl_->rxRoot);
        impl_->ourTempRoots.insert(impl_->rxRoot);

        // Pre-create directory entries so we can skip them when chunking.
        int fileCount = 0;
        int dirCount = 0;
        impl_->rxTotalBytes = 0;
        for (const auto& e : impl_->rxFiles) {
            if (e.isDir) {
                QDir().mkpath(impl_->rxRoot + QLatin1Char('/') + e.path);
                ++dirCount;
            } else {
                impl_->rxTotalBytes += e.size;
                ++fileCount;
            }
        }

        impl_->rxFileIdx = 0;
        impl_->rxFileBytesRem = 0;
        impl_->rxBytesSoFar = 0;
        impl_->rxLastLogAtMs = QDateTime::currentMSecsSinceEpoch();
        impl_->rxStartedAt = QDateTime::currentDateTime();
        impl_->rxActive = true;
        emit log(QStringLiteral("[xfer] 开始接收 %1 个文件, %2 个目录, "
                                 "共 %3 KB → %4")
                     .arg(fileCount).arg(dirCount)
                     .arg(impl_->rxTotalBytes / 1024)
                     .arg(impl_->rxRoot));
        emit xferStarted(/*sending=*/false, impl_->rxTotalBytes, fileCount);
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
                const QString abs = impl_->rxRoot + QLatin1Char('/') + e.path;
                QDir().mkpath(QFileInfo(abs).absolutePath());
                impl_->rxFile = std::make_unique<QFile>(abs);
                if (!impl_->rxFile->open(QIODevice::WriteOnly)) {
                    emit log(QStringLiteral("[xfer] 创建文件失败: %1").arg(abs));
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
                    emit log(QStringLiteral("[xfer] 多余的 CHUNK 字节, 丢弃 %1")
                                 .arg(remaining));
                    return;
                }
            }
            const std::size_t take = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(impl_->rxFileBytesRem));
            const qint64 written = impl_->rxFile->write(
                reinterpret_cast<const char*>(p), static_cast<qint64>(take));
            if (written <= 0) {
                emit log(QStringLiteral("[xfer] 写入失败"));
                return;
            }
            p += written;
            remaining -= static_cast<std::size_t>(written);
            impl_->rxFileBytesRem -= written;
            impl_->rxBytesSoFar += written;
        }

        // Throttled progress. Same cadence as the sender side so the
        // operator sees a matching pair of lines and can tell "sender X
        // done, receiver Y done" is on track.
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - impl_->rxLastLogAtMs >= kProgressLogIntervalMs) {
            const int pct = impl_->rxTotalBytes > 0
                ? static_cast<int>((impl_->rxBytesSoFar * 100) /
                                   impl_->rxTotalBytes)
                : 100;
            emit log(QStringLiteral("[xfer] 接收进度 %1% (%2 / %3 KB)")
                         .arg(pct)
                         .arg(impl_->rxBytesSoFar / 1024)
                         .arg(impl_->rxTotalBytes / 1024));
            emit xferProgress(/*sending=*/false, impl_->rxBytesSoFar,
                              impl_->rxTotalBytes, pct);
            impl_->rxLastLogAtMs = nowMs;
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
        const int elapsedSec = static_cast<int>(
            impl_->rxStartedAt.secsTo(QDateTime::currentDateTime()));
        emit log(QStringLiteral("[xfer] 接收完成, 共 %1 KB, 用时 %2 秒 · "
                                 "剪切板已就绪, 在目标位置 Ctrl+V 即可粘贴")
                     .arg(impl_->rxBytesSoFar / 1024)
                     .arg(elapsedSec));
        emit xferFinished(/*sending=*/false, /*cancelled=*/false,
                          impl_->rxBytesSoFar);

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
    emit log(QStringLiteral("[xfer] 剪切板已设置 %1 项, 可以 Ctrl+V 粘贴")
                 .arg(absPaths.size()));
}
