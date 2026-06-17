# Agent — 被控端(C++ / Windows)

完成度:DXGI 抓屏 → FFmpeg 编码 → 推到 Relay → 接收输入帧 → SendInput 注入。
覆盖文档 §7 的步骤 2 和步骤 4。

## 目录

```
agent/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── capture/    # DxgiCapturer:D3D11 + IDXGIOutputDuplication
│   ├── encode/     # H264Encoder:libavcodec,运行时选 H.264 编码器
│   ├── input/      # input_injector:Viewer 输入帧 → SendInput
│   ├── net/        # TcpClient(winsock)+ frame 协议 + RelayClient
│   └── util/       # bmp_writer(2a 验证用)
└── cmd/
    ├── capture_test/  # 2a:抓一帧写 capture.bmp
    ├── encode_test/   # 2b:抓 N 秒、写裸 Annex-B .h264
    └── agent/         # 2c:正式 agent,连 Relay 推 VIDEO_FRAME
```

## 编码器优先级

`H264Encoder::Init` 按下列顺序找,选第一个能开起来的:

```
h264_nvenc  →  h264_amf  →  h264_qsv  →  h264_mf  →  libx264
```

`h264_mf` 是 Windows Media Foundation 内置的 H.264 编码器,所有 Win10+ 机器都有,
所以即便 vcpkg 装的 ffmpeg **不带 x264 特性**,本机也能跑得起来 —— 当前开发机就走的这条路。

要切到 libx264 软编做对比,把 vcpkg.json 改成 `"ffmpeg[x264]"` 重装即可
(初次编译需要几分钟)。

## 构建

```powershell
# 一次性:让 CMake 找到 ffmpeg
$env:VCPKG_ROOT = "<your-vcpkg-root>"   # e.g. E:\Development_environment\vcpkg

cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

CMake 会自动从 `$VCPKG_ROOT/installed/x64-windows` 取头文件和 .lib。
也可以显式传 `-DFFMPEG_ROOT=<path>` 覆盖。

构建结束后,FFmpeg 的运行时 DLL 已经被 `add_custom_command` 拷到
`build/Release/` 旁边,可执行文件能直接跑,不用改 PATH。

## 三个测试可执行

### 2a · capture_test —— 抓一帧落 BMP

```powershell
.\build\Release\capture_test.exe capture.bmp
```

控制台会打印实际抓到的分辨率;BMP 用任何看图工具都能开。
4K 屏一帧约 33 MB(全屏 BGRA 没压缩),正常。

### 2b · encode_test —— 抓 N 秒 → 裸 H.264 文件

```powershell
.\build\Release\encode_test.exe out.h264 5
# 验证:
ffplay -f h264 out.h264
```

文件是 Annex-B 裸流(AUD → SPS → PPS → IDR → ...),ffplay/VLC 都能直接解。

### 2c · agent —— 接 Relay 推流

```powershell
# 先把 relay 跑起来(见 ../relay/README.md)
..\relay\relay.exe -listen :7100 -config ..\relay\config.json

# 再起 agent
.\build\Release\agent.exe `
    -relay 127.0.0.1:7100 `
    -device-id device-001 `
    -device-token agent-token-001 `
    -fps 30 -bitrate 8000

# 用 fakeviewer 模拟管理端拉流
..\relay\fakeviewer.exe -relay 127.0.0.1:7100 -account alice `
    -session-token viewer-token-alice -device-id device-001 -run 5s
```

`fakeviewer` 应该打印 `VIDEO_FRAME #N (...)` 一直到时间到。

## Agent 状态机

```
                ┌─────────────┐
        启动 ─→ │  REGISTER   │
                └──────┬──────┘
                       │ REGISTER_RESP{ok}
                ┌──────▼──────┐  读 Relay 帧
       ┌───────▶│    idle     │──┐
       │        └──────┬──────┘  │ HEARTBEAT / 其它  → 丢
       │               │ SESSION_START
       │        ┌──────▼──────────────┐
       │        │ 抓屏 + 编码 + 推帧    │ ←─ MOUSE/KEY 帧 → input_injector
       │        │ (子线程,sleep_until) │      → SendInput
       │        └──────┬──────────────┘
       │               │ SESSION_END
       └───────────────┘
```

线程模型:
- 主线程 — 读 Relay 帧,分发 SESSION_START/END/input
- StreamPump 子线程 — `condition_variable` 上等 session,**会话期间**才抓+编+发
- TcpClient 内有写锁,主线程的握手回包 / pump 的 VIDEO_FRAME 并发安全

会话切换时编码器**整个销毁重建**,目的是让新 Viewer 接进来就拿到一份完整的
SPS/PPS/IDR。`H264Encoder` 在 4K 下重建 < 100ms,够廉价。

## 输入帧字节布局(与 Viewer 同步)

```
MOUSE_MOVE    (4 字节)  [u16 BE x][u16 BE y]    0..65535 over agent 主屏
MOUSE_BUTTON  (2 字节)  [button][down]          button: 0=L,1=R,2=M,3=X1,4=X2
KEY_EVENT     (4 字节)  [u16 BE vk][down][rsv]  vk = Windows VK_*
```

- `MOUSE_MOVE` 用 `MOUSEEVENTF_ABSOLUTE`(不带 `VIRTUALDESK`),所以 0..65535
  映射到**主屏**,多显示器要扩展时再加 `VIRTUALDESK`。
- `KEY_EVENT` 的 EXTENDEDKEY 标志由 agent 内部 VK 表决定(右 Ctrl / 右 Alt /
  方向键 / 小键盘 `/` / Insert/Delete/Home/End/PgUp/PgDn / Win 键 / Apps),
  Viewer 不用关心。
- scancode 由 agent 用 `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)` 反查,所以
  Viewer 也不用传。

## 验证 input 路径

`fakeviewer -input-burst` 会发一个 MOUSE_MOVE 到 (32767, 32767) 和一对
F13 KEY_EVENT 按下/抬起:

```powershell
..\relay\fakeviewer.exe -relay 127.0.0.1:7100 -account alice `
    -session-token viewer-token-alice -device-id device-001 `
    -run 1500ms -input-burst
```

可见效果:agent 主机的鼠标光标跳到主屏正中。F13 选的是几乎没有 app
绑定的键,做冒烟时不会触发非预期动作。

## TODO

- [ ] 心跳超时(目前 agent 完全不发 HEARTBEAT,纯靠 TCP keepalive 兜底)。
- [ ] 断线重连(Run() 返回后 main 直接退出,生产环境需要带退避的重连循环)。
- [ ] 自适应码率 / 分辨率。
- [ ] NVENC / QSV / AMF 实测验证(本机暂时只走到 h264_mf)。
- [ ] 多显示器:鼠标加 `MOUSEEVENTF_VIRTUALDESK`,坐标从虚拟桌面起算。
- [ ] 鼠标滚轮 / 水平滚轮(需要新增 `MOUSE_WHEEL` 消息类型)。
