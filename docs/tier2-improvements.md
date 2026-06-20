# 第二档优化项 —— 待办与设计取向

这份文档记的是 Tier 1 之后准备做、但需要更大改动的优化项。Tier 1
(画质 + 静止跳帧 + intra-refresh)已经合到 main 分支了,Tier 2 在那个
基础上继续推。

**明确不做的事**:**自适应码率(ABR)**。用户场景是远程改代码/调试,
30–60 fps 中等码率就够,带宽不是瓶颈,不需要按 RTT/丢包率动态调码率
的复杂度。如果以后用例变成跑视频/动画,再回来加。

按 ROI 排序,越靠前越值得先做。

---

## T2-1. 鼠标光标层独立(Cursor as overlay)

**状况**:目前 DXGI Desktop Duplication 默认是**不**把光标烤进帧的
(光标信息在 `DXGI_OUTDUPL_FRAME_INFO::PointerPosition` / `PointerShape`
里另发)。Viewer 当前看到的"光标"其实是 Qt widget 本地光标,所以
位置反应已经是零延迟。

**剩下要做的**:同步光标**形状**。目前不管远程在文本框里(应该是 I-beam)
还是在拖东西(应该是 SizeAll),viewer 永远显示默认箭头。

**改动**:
- 协议:新 `CURSOR_SHAPE = 0x40`,payload = `[u32 BE width][u32 BE height]
  [u32 BE hotX][u32 BE hotY][u8 colorType][pixels...]`,只在形状变化时发
- 协议:新 `CURSOR_POS = 0x41`(可选),payload `[u16 BE x][u16 BE y][u8 visible]`
  —— 双重保险,万一 SendInput 回路丢帧
- Agent:DxgiCapturer 暴露 `OnPointerShape(...)` 回调
- Viewer:VideoWidget 监听形状变化,调 `setCursor(QCursor(QPixmap(...), hotX, hotY))`

**收益**:UX 提升一档。无延迟收益(因为本地光标本来就实时)。

**估计工作量**:1–2 天。

---

## T2-2. 脏区域感知编码(Dirty rect aware encoding)

**状况**:每帧不管多大都全屏编码。代码编辑场景下,大部分时间只有
**一个字符变化** —— 输入个字符,只有那 10×20 像素变了,却被编进
一整张 4K 帧里。

**思路**:
1. DXGI 已经给出 `DirtyRects[]` 和 `MoveRects[]` 数组,精确告知哪些
   矩形像素变了
2. 取所有 dirty rect 的最小外接矩形 BBox,只编 BBox 这块
3. 在协议里 VIDEO_FRAME 加几个字段标注"这帧只覆盖屏幕的 (x, y, w, h) 区域"
4. Viewer 收到后做局部 blit,而不是替换整帧
5. MoveRect 单独走 `SCREEN_SCROLL` 消息,viewer 端本地 blit 复制,根本
   不用编码

**复杂的地方**:
- 编码器要支持变分辨率编码 —— 用 NVENC 的 multi-stream 或者分块编码
  方案;libx264 一旦 init 就固定分辨率,得每次 dirty bbox 变了就重 init
- 简化方案:固定输出分辨率,但把 dirty 区域以外的像素**复用上一帧**
  喂给编码器,让 H.264 的"skip macroblock"机制自然把这部分变成 ~0 字节
  —— 编码器已经这么干了,但显式提示能让它更激进
- 更激进:把屏幕切成固定 N×N 个 tile,每个 tile 一个独立 H.264 stream,
  只编 dirty 那几个 tile —— RDP RemoteFX 的做法

**收益**:代码编辑场景带宽降 90%+。延迟微降(更小的帧解码更快)。

**估计工作量**:2 周(细节多)。先做"简化方案",看效果再决定要不要上 tile。

---

## T2-3. UDP / QUIC 传输(消除 TCP 队头阻塞)

**状况**:当前所有流量走 Relay 的单条 TCP。一个包丢了 → TCP 重传 →
后面所有视频包都得等 → 延迟尖峰。

**思路**:把 Agent ↔ Relay 和 Viewer ↔ Relay 改成 QUIC。视频帧走
"unreliable datagram"(QUIC 的 DATAGRAM 扩展),控制帧走可靠 stream。

**为什么选 QUIC 不选裸 UDP**:
- 自带 TLS,token 不裸跑
- 自带连接迁移 / NAT 重绑
- Go 有成熟的 `quic-go`,集成 Relay 几百行就能搞定
- C++ 端有 `msquic`(微软,Windows 原生支持)、`lsquic`(LiteSpeed)

**改动**:
- Relay 改两套监听:旧 TCP 保留兼容 + 新 QUIC 端口
- Agent/Viewer 端协议层抽象,先按 TCP 路径写完,再加 QUIC backend
- 帧协议本身不动 —— 还是 `[type][len][payload]`,只是承载方式变了
- 视频帧从 stream 转 datagram:**单帧不超过 ~1200 字节**就直接发,
  超了的话上层切片 + 自己加序号
- 帧丢失策略:丢 P 帧无所谓,丢 IDR / intra-refresh 关键 slice 时发
  "请重发"信令,Agent 收到立刻 force keyframe

**收益**:LAN 内延迟方差大幅降低;弱网下不再有 200+ ms 尖峰。

**估计工作量**:3–4 周(协议改造 + 两端 + Relay 都要动)。

---

## T2-4. Viewer 硬解 + GPU 渲染

**状况**:libavcodec 软解 4K H.264 在 30 fps 下能跑,60 fps 就吃力;
QImage 一帧 33 MB 拷贝 + QPainter CPU 光栅化是大头。

**思路**:
1. 解码用 D3D11VA hardware acceleration(libavcodec 自带,加 `-hwaccel d3d11va`)
   解出的 frame 就是个 D3D11 共享纹理,内存不下来,留在 GPU
2. 渲染层 从 QWidget + QPainter 换成 QQuickWindow + DirectX 互操作,或者
   QOpenGLWidget + WGL_NV_DX_interop 把 D3D11 纹理共享给 OpenGL
3. 跳过 QImage 复制和 emit/queued connection 的整段开销

**改动**:
- 新建 `D3D11Decoder` 类(并存于现 H264Decoder,通过 config 切换)
- VideoWidget 改为 QOpenGLWidget,paintGL 用纹理渲染
- 跨线程交付从 emit QImage 改成 emit "frame ready token",GPU 资源用引用计数

**收益**:CPU 占用降一半,4K @ 60 fps 平滑;端到端延迟 -20ms。

**估计工作量**:1–2 周。

---

## T2-5. YUV444 编码(文字锐度)

**状况**:libx264/h264_nvenc 当前用 YUV420P,色度采样比亮度低 75%,
彩色文字边缘必糊。

**思路**:把编码器 `pix_fmt` 改 `AV_PIX_FMT_YUV444P`,sws_scale 也对应
改。带宽约多 30%(可接受),文字锐度差距明显。

**前提**:libx264 编译时支持 444 profile(vcpkg 的 ffmpeg 默认带);
NVENC 支持 444 需要 Turing 架构以上;h264_mf 不支持 444 —— 但走到这里
应该已经把 h264_mf 抛弃了。

**收益**:文字主观锐度明显提升,跟原生显示几乎区分不出。

**估计工作量**:半天(改两个 enum + 测试)。

---

## T2-6. 心跳超时 + 断线重连

**状况**:目前 agent 完全不发 HEARTBEAT,纯靠 TCP keepalive(默认 2 小时)
检测断线。relay 一侧只 reflect 心跳,没有"N 秒无心跳就踢"的逻辑。
Agent `Run()` 返回后 main 直接退出,生产环境需要带退避的重连。

**改动**:
- Agent:每 5 秒主动发一次 HEARTBEAT
- Relay:每条连接独立 idle timer,30 秒无任何帧就 close
- Agent main:`Run()` 返回后进入指数退避重连循环(1s, 2s, 4s, ...,
  上限 30s)
- Viewer:同样的重连,UI 上状态栏显示 "reconnecting in 4s..."

**收益**:服务端重启/网络抖动 60 秒内自愈,不需要客户手动重启 agent。

**估计工作量**:2–3 天。

---

## T2-7. 设备列表 UI

**状况**:Viewer 现在 `-device-id` 是命令行参数,启动后就锁定一台。
切设备得关掉重开。

**改动**:
- Relay:新增 HTTP 端点 `GET /devices`,返回当前 account 可访问且在线的
  设备列表(JSON)
- Viewer:启动后先拉这份列表,显示成左侧 dock,双击进会话
- 协议层:支持在同一 TCP 连接里发多次 `CONNECT`(目前只能开始一次),
  或者每选一台设备开一条新连接

**估计工作量**:1 周。

---

## 暂不做(明确取舍)

- **自适应码率(ABR)**:用例是远程改代码/调试,稳定中等码率即可,
  不值得 GCC / RTT 反馈环的复杂度。
- **多 Viewer 并发观看同一设备**:`Hub.AttachViewer` 现在硬限制单连接,
  改成 broadcast 不难但 MVP 用不上。
- **会话录像 / 审计日志**:做了 Item 6(心跳)再考虑,跟客户合规需求挂钩。
- **P2P 打洞**:RustDesk 实测对对称型 NAT 无效,失败仍要中转兜底,实际
  收益小,不做。
