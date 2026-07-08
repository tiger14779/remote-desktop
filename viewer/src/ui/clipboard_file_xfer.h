#pragma once

// Clipboard-driven bidirectional file transfer.
//
// When the user Ctrl+C's a file (or folder) on one side, QClipboard hands us
// a QMimeData with URLs. ClipboardFileXfer enumerates the URLs (recursing
// into directories), pushes a FILE_XFER_BEGIN frame with the JSON file list,
// streams each file's bytes in 64KB chunks via FILE_XFER_CHUNK, and closes
// with FILE_XFER_END.
//
// On the receiving side, the same class reverses the flow: writes incoming
// bytes into a per-transfer temp directory, then sets the local clipboard
// to a QMimeData pointing at those temp files so a Ctrl+V in Explorer just
// works.
//
// Loop-break: every temp dir we create is remembered. When QClipboard
// dataChanged fires from our own setMimeData, we see the URLs point at one
// of our remembered temps and skip sending them back.

#include <QObject>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

class QFile;

class ClipboardFileXfer : public QObject {
    Q_OBJECT
public:
    // Send callback. The bridge invokes this for each outbound frame; the
    // caller decides which side's RelayClient to write to.
    using SendFn = std::function<void(std::uint8_t type,
                                       const std::uint8_t* data,
                                       std::size_t size)>;

    explicit ClipboardFileXfer(QObject* parent = nullptr);
    ~ClipboardFileXfer() override;

    // Installs the outbound channel. Pass nullptr to disable sending (e.g.,
    // when no session is active).
    void setSendFn(SendFn fn);

    // Wire incoming protocol frames from the relay read loop. These can be
    // called from any thread — internally we hop to the GUI thread for
    // anything that touches QClipboard.
    void onPeerFrame(std::uint8_t type,
                     const std::uint8_t* data, std::size_t size);

    // Hook from the GUI-thread QClipboard::dataChanged handler. Inspects the
    // current mime data — if it contains file URLs (and they aren't ours),
    // emits localFilesAvailable(true) so the UI can offer a "send to peer"
    // button. Does NOT auto-start a transfer. Reason: the user often
    // Ctrl+C's a file just to paste it locally on the same machine, and
    // eagerly streaming gigabytes across the WAN in that case wastes
    // bandwidth AND can't be un-done.
    void onLocalClipboardChanged();

    // Start pushing the current clipboard's files to the peer. Called from
    // AppMainWindow when the operator clicks the banner's "发送到对方"
    // button. Safe to call from the GUI thread.
    void sendClipboardNow();

    // Same push, but from an explicit URL list — used by drag-and-drop
    // (MainWindow::dropEvent) so we don't have to clobber the operator's
    // clipboard just to piggy-back on sendClipboardNow. Rejects the same
    // "these are our own temp files" and "already sending" cases.
    void sendFilesNow(const QList<QUrl>& urls);

    // Best-effort cancel of an in-flight outbound transfer. The worker
    // notices the flag at the next chunk boundary and stops after sending
    // FILE_XFER_END with cancelled=true. No-op if nothing is in flight.
    void cancelActiveTransfer();

signals:
    // Diagnostic logs piped to the app's log panel.
    void log(const QString& s);

    // Local clipboard now / no longer contains sendable file URLs. Fires
    // on every QClipboard::dataChanged transition. The UI uses this to
    // show/hide the "发送到对方" banner button. `topLevelCount` is the
    // number of items the user copied at top-level (may include folders);
    // meaningful only when available is true.
    void localFilesAvailable(bool available, int topLevelCount);

    // Coarse-grained transfer state — surfaced on the "正在被远程" banner so
    // the local user can see when their machine is being read from / written
    // to over the same session. `sending == true` means WE are pushing bytes
    // to the peer; false means we're receiving.
    void xferStarted(bool sending, qint64 totalBytes, int fileCount);
    void xferProgress(bool sending, qint64 doneBytes, qint64 totalBytes,
                      int percent);
    void xferFinished(bool sending, bool cancelled, qint64 totalBytes);

private slots:
    void applyReceivedFiles(const QString& tempRoot,
                            const QStringList& absPaths);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
