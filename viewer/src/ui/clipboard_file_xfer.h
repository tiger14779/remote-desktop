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
    // current mime data; if it contains file URLs (and they aren't from a
    // transfer we just received), spins up an outbound transfer.
    void onLocalClipboardChanged();

signals:
    // Diagnostic logs piped to the app's log panel.
    void log(const QString& s);

private slots:
    void applyReceivedFiles(const QString& tempRoot,
                            const QStringList& absPaths);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
