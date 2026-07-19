#pragma once

#include <QImage>
#include <QOpenGLWidget>

#include <cstdint>
#include <unordered_set>

// QOpenGLWidget that paints the latest frame via Qt's OpenGL paint engine
// (drawImage uploads the QImage to a GPU texture and uses bilinear sampling
// hardware-side instead of CPU raster). Also captures mouse + keyboard input
// over the video area (the letterbox bars are ignored) and emits normalised
// events.
//
// Normalisation: mouse coordinates are mapped to 0..65535 over the displayed
// video area. The agent's SendInput consumes these directly as
// MOUSEEVENTF_ABSOLUTE coordinates over its primary monitor — viewer never
// needs to know the agent's actual resolution.
//
// Used to be plain QWidget. CPU drawImage at 1440p was 10-20ms per paint on
// the test box and accumulated paint-backpressure drops during fast drags;
// QOpenGLWidget drops that to single-digit ms because the scale + composite
// run on the GPU.
class VideoWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(960, 540); }

public slots:
    void onFrame(const QImage& img);

signals:
    void inputMouseMove(std::uint16_t x, std::uint16_t y);
    void inputMouseButton(std::uint8_t button, bool down);
    void inputMouseWheel(std::int16_t deltaBE);
    void inputKey(std::uint16_t vkCode, bool down);

    // Emitted at the end of paintEvent. ViewerSession listens to this so it
    // knows GUI has caught up and another decoded frame is allowed through.
    void framePainted();
    // Timing side-channel: how long paintEvent took (ms). MainWindow wires
    // this into ViewerSession's stat accumulator so the per-2s log can
    // report avg/max paint cost.
    void paintTook(int ms);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    QRect videoTarget() const;
    bool mapToNormalized(QPoint widgetPos, std::uint16_t* nx, std::uint16_t* ny) const;
    static std::uint8_t qtButtonToId(int qtButton);
    // Release every VK we currently believe is held. Fires on focus loss so
    // Alt+Tab / clicking away can't leave the agent side stuck with a
    // modifier down (which manifested as Alt+DoubleClick = "Properties"
    // instead of "open folder" once Alt got lost).
    void releaseHeldKeys();

    QImage current_;
    // Every VK we've emitted a keyDown for and not yet balanced with keyUp.
    std::unordered_set<std::uint16_t> heldKeys_;
};
