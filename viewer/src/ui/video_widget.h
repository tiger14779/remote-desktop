#pragma once

#include <QImage>
#include <QOpenGLWidget>

#include <cstdint>

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
    void inputKey(std::uint16_t vkCode, bool down);

    // Emitted at the end of paintEvent. ViewerSession listens to this so it
    // knows GUI has caught up and another decoded frame is allowed through.
    void framePainted();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    QRect videoTarget() const;
    bool mapToNormalized(QPoint widgetPos, std::uint16_t* nx, std::uint16_t* ny) const;
    static std::uint8_t qtButtonToId(int qtButton);

    QImage current_;
};
