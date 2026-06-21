#include "ui/video_widget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 180);
    setMouseTracking(true);          // mouseMoveEvent without buttons
    setFocusPolicy(Qt::StrongFocus); // keyboard
}

void VideoWidget::onFrame(const QImage& img) {
    current_ = img;
    update();
}

QRect VideoWidget::videoTarget() const {
    if (current_.isNull()) return {};
    const QSize fitted = current_.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect((width() - fitted.width()) / 2,
                 (height() - fitted.height()) / 2,
                 fitted.width(), fitted.height());
}

void VideoWidget::paintEvent(QPaintEvent*) {
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!current_.isNull()) {
            // SmoothPixmapTransform on: bilinear downscale. Without it,
            // downscaling a 4K source to a 1280x720 window does
            // nearest-neighbour and text turns crunchy. With it, text edges
            // stay legible at the cost of some CPU.
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(videoTarget(), current_);
        }
    }
    // Outside the QPainter scope so the GPU/CPU work has actually finished
    // before we tell the upstream "I'm done, send me the next frame".
    emit framePainted();
}

bool VideoWidget::mapToNormalized(QPoint widgetPos,
                                  std::uint16_t* nx,
                                  std::uint16_t* ny) const {
    const QRect t = videoTarget();
    if (t.isEmpty()) return false;
    if (!t.contains(widgetPos)) return false;
    const double fx = double(widgetPos.x() - t.x()) / double(t.width());
    const double fy = double(widgetPos.y() - t.y()) / double(t.height());
    // Clamp belt-and-braces — t.contains() should have already guaranteed it.
    const double cx = std::clamp(fx, 0.0, 1.0);
    const double cy = std::clamp(fy, 0.0, 1.0);
    *nx = static_cast<std::uint16_t>(std::lround(cx * 65535.0));
    *ny = static_cast<std::uint16_t>(std::lround(cy * 65535.0));
    return true;
}

std::uint8_t VideoWidget::qtButtonToId(int qtButton) {
    switch (qtButton) {
        case Qt::LeftButton:    return 0;
        case Qt::RightButton:   return 1;
        case Qt::MiddleButton:  return 2;
        case Qt::XButton1:      return 3;
        case Qt::XButton2:      return 4;
        default:                return 0xFF;
    }
}

void VideoWidget::mouseMoveEvent(QMouseEvent* e) {
    std::uint16_t nx, ny;
    if (mapToNormalized(e->pos(), &nx, &ny)) {
        emit inputMouseMove(nx, ny);
    }
}

void VideoWidget::mousePressEvent(QMouseEvent* e) {
    setFocus(); // ensure we keep receiving keys
    std::uint16_t nx, ny;
    if (!mapToNormalized(e->pos(), &nx, &ny)) return;
    const std::uint8_t btn = qtButtonToId(e->button());
    if (btn == 0xFF) return;
    // Position-first so the click lands where the cursor visibly is, even if
    // the move event got coalesced.
    emit inputMouseMove(nx, ny);
    emit inputMouseButton(btn, true);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* e) {
    const std::uint8_t btn = qtButtonToId(e->button());
    if (btn == 0xFF) return;
    // Release fires even if the cursor has wandered into the letterbox; the
    // agent needs the up event to balance the down event regardless.
    emit inputMouseButton(btn, false);
}

void VideoWidget::keyPressEvent(QKeyEvent* e) {
    const auto vk = static_cast<std::uint16_t>(e->nativeVirtualKey());
    if (vk == 0) return;
    emit inputKey(vk, true);
}

void VideoWidget::keyReleaseEvent(QKeyEvent* e) {
    const auto vk = static_cast<std::uint16_t>(e->nativeVirtualKey());
    if (vk == 0) return;
    emit inputKey(vk, false);
}
