# Viewer — 管理端(C++ / Qt6 / Windows)

完成度:接收 VIDEO_FRAME → libavcodec H.264 解码 → Qt 窗口渲染,
本地鼠标/键盘事件 → 归一化 → 推 MOUSE/KEY 帧。覆盖文档 §7 步骤 3 和步骤 5。

## 目录

```
viewer/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── decode/      # H264Decoder:libavcodec + swscale(YUV→BGRA)
│   ├── net/         # 与 agent/ 同形,加 AUTH/CONNECT 编码
│   └── ui/
│       ├── video_widget.{h,cpp}    # 画 QImage 的 QWidget
│       ├── viewer_session.{h,cpp}  # 后台 QObject:网络 + 解码
│       └── main_window.{h,cpp}     # 主窗口 + 状态栏
└── cmd/
    ├── decode_test/  # 离线:.h264 文件喂解码器,打印帧数
    └── viewer/       # Qt 主程序
```

## 线程模型

```
GUI 线程 (QApplication)                  Worker 线程 (QThread)
─────────────────────────                ─────────────────────
MainWindow                               ViewerSession
  ├── VideoWidget          frameReady       ├── ViewerRelayClient (socket)
  │   onFrame(QImage)  ←─Qt::Queued──────  └── H264Decoder
  ├── 状态栏 QLabel                                ↓ DecodedFrame
  └── close → requestStop                          ↓ QImage::Format_RGB32
                                                   ↓ shallow.copy()
                                                   → emit frameReady
```

- 网络读 + 解码都在 worker 线程,不阻塞 GUI。
- `frameReady(QImage)` 通过 `Qt::QueuedConnection` 跨线程,每帧做了一次
  `QImage::copy()` 把数据脱离 DecodedFrame 的生命周期 —— GUI 想拿多久就拿多久。
- 关窗口时 `MainWindow::closeEvent` 调 `session->requestStop()`,后者 close 套接字,
  worker 线程 `Run()` 立刻返回,`QThread::quit()` + `wait()` 一气呵成。

## 解码格式约定

- libavcodec 输出 YUV420P,通过 `sws_scale` 转 BGRA8 紧凑布局(`width*4` 步距)。
- 这个字节序正好等于 `QImage::Format_RGB32` 在小端机上的实际内存布局,
  `QImage(buf, w, h, stride, Format_RGB32)` 零拷贝包住,再 `.copy()` 一次得到独立副本。
- Agent 那边保证一个 VIDEO_FRAME 等于一个完整 AU,**不需要 av_parser**;`decode_test`
  例外,因为它从一个连接的 .h264 文件里读,需要切 AU,这才用了 parser。

## 构建

```powershell
$env:VCPKG_ROOT = "<your-vcpkg-root>"   # 与 agent/ 同
$env:QT_DIR     = "E:\Development_environment\qt\6.7.3\msvc2022_64"

cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

构建结束后:
- FFmpeg 运行时 DLL 已经被拷到 `build/Release/`
- `windeployqt` 自动跑了一次,Qt 自己的运行时也已经备齐

直接 `build\Release\viewer.exe` 双击就能跑。

## 跑

```powershell
# 1. 起 relay 和 agent(见各自 README)
..\relay\relay.exe -listen :7100 -config ..\relay\config.json
..\agent\build\Release\agent.exe -relay 127.0.0.1:7100 `
    -device-id device-001 -device-token agent-token-001 -fps 30 -bitrate 8000

# 2. 起 viewer
.\build\Release\viewer.exe -relay 127.0.0.1:7100 -account alice `
    -session-token viewer-token-alice -device-id device-001
```

窗口标题里会显示 `account@host:port → device_id`,状态栏第一行 "authenticating..." →
"connected, waiting for frames"。收到帧后中央 widget 就开始放被控端桌面。

## decode_test(离线)

```powershell
# 拿 agent/encode_test 生成的 .h264 验证解码独立工作
..\agent\build\Release\encode_test.exe sample.h264 5
.\build\Release\decode_test.exe sample.h264
# 期望输出: decoded ~150 frame(s), last size <WxH>
# 差 1~2 帧是 libavcodec 流水线延迟,正常
```

## 输入帧字节布局(与 Agent 同步)

```
MOUSE_MOVE    (4 字节)  [u16 BE x][u16 BE y]    0..65535 = video widget 显示区
MOUSE_BUTTON  (2 字节)  [button][down]          button: 0=L,1=R,2=M,3=X1,4=X2
KEY_EVENT     (4 字节)  [u16 BE vk][down][rsv]  vk = Qt::nativeVirtualKey() (Windows 上等于 VK_*)
```

归一化坐标是相对 video widget **去掉黑边后**的内容区:

```
VideoWidget 整块
┌─────────────────────┐
│ ░░░░░░░░░░░░░░░░░░░ │  ← 黑边,鼠标在这里时事件被丢
│ ░ ┌───────────────┐ ░ │
│ ░ │  video target │ ░ │  ← 落在这个矩形里,
│ ░ │  (保持宽高比)  │ ░ │     映射到 (0,0)..(65535,65535)
│ ░ └───────────────┘ ░ │
│ ░░░░░░░░░░░░░░░░░░░ │
└─────────────────────┘
```

`mouseReleaseEvent` 例外:即使鼠标已经飘到黑边外也会发送 `MouseButton(down=0)`,
否则 agent 那边 down/up 不配对会留个永远按下的按键。

## ViewerSession 的线程边界

`sendMouseMove / sendMouseButton / sendKey` 不是 Qt slot,**在 GUI 线程上直接调用**。
理由:worker 线程一直阻塞在 `Run()` 里读 socket,根本不会跑事件循环,
所以靠 `Qt::QueuedConnection` 把输入帧排到 worker 上发出去会被堵死。
TcpClient 内部有写互斥,GUI 线程直接调 write 是安全的。

## TODO

- [ ] FPS / 比特率统计显示在状态栏。
- [ ] 高 DPI 缩放(目前 paintEvent 用 `KeepAspectRatio`,够用,但没做 dpr 处理)。
- [ ] QOpenGLWidget / 纹理上传,4K @ 60fps 时 paintEvent 路径可能跟不上。
- [ ] 重连 + 友好的连接错误提示对话框。
- [ ] 鼠标滚轮(`wheelEvent` + 新增 `MOUSE_WHEEL` 协议消息类型)。
- [ ] 失焦时主动放掉所有按下的键,免得切窗口后某个 Ctrl/Shift 永远按住。

## 与 agent/ 的代码重复

`src/net/tcp_client.cpp` 是从 `agent/src/net/` 完全照搬过来的,`frame.h` 也大量重复。
当前两个项目独立维护,等 step 5 把输入帧接进来、协议字节布局完全确定之后,
打算把 `src/net/` 整个抽成 `../shared/cpp_protocol/` 或者 vcpkg 内部 port。
现在还不动 —— 抽得太早会因为细节没磨完而反复改接口。
