#pragma once

#include <QObject>
#include <QString>

// Captures every line written to std::cerr (and std::cout) and re-emits it
// as a Qt signal so the GUI's log panel can subscribe. Install once at app
// startup BEFORE any agent/encoder/etc code runs, so the [stat], [encoder],
// [agent] diagnostic lines that StreamPump and H264Encoder write end up in
// the panel instead of disappearing into the void (Win32 GUI exes have no
// console attached).

class LogSink : public QObject {
    Q_OBJECT
public:
    static LogSink* instance();

    // Replace std::cerr.rdbuf() (and std::cout.rdbuf()) with our buffer.
    // Idempotent — calling twice does nothing.
    void install();

public slots:
    // Internal: streambuf calls this via QMetaObject::invokeMethod from
    // whichever thread cerr was written on, marshalled onto the GUI thread.
    void emitLine(const QString& s);

signals:
    void line(const QString& s);

private:
    LogSink();
    ~LogSink() override;

    class Buf;
    Buf* buf_ = nullptr;
};
