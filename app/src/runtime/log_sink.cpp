#include "runtime/log_sink.h"

#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>

class LogSink::Buf : public std::streambuf {
public:
    explicit Buf(LogSink* sink) : sink_(sink) {}

protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return c;
        std::lock_guard<std::mutex> g(mu_);
        absorb(static_cast<char>(c));
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> g(mu_);
        for (std::streamsize i = 0; i < n; ++i) absorb(s[i]);
        return n;
    }

private:
    // Caller holds mu_.
    void absorb(char ch) {
        if (ch == '\n') {
            QString line = QString::fromStdString(buf_);
            if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
            buf_.clear();
            // Re-marshal to the GUI thread via the sink's slot. This is
            // safe to call while holding mu_ — invokeMethod just queues an
            // event and returns immediately.
            QMetaObject::invokeMethod(sink_, "emitLine",
                                       Qt::QueuedConnection,
                                       Q_ARG(QString, line));
        } else {
            buf_.push_back(ch);
        }
    }

    LogSink* sink_;
    std::string buf_;
    std::mutex mu_;
};

LogSink* LogSink::instance() {
    static LogSink s;
    return &s;
}

LogSink::LogSink() = default;
LogSink::~LogSink() = default;

void LogSink::install() {
    if (buf_) return;
    buf_ = new Buf(this);
    // Keep the original buffers around — we leak our Buf on purpose so it
    // outlives any cerr/cout writes during static destruction.
    std::cerr.rdbuf(buf_);
    std::cout.rdbuf(buf_);
}

void LogSink::emitLine(const QString& s) { emit line(s); }
