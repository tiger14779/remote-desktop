# Remote Desktop —— 自建远程桌面管理平台

为团队 / 客户家庭版 Windows 机搭一套**自有源码、可长期维护**的远控,
解决 RustDesk 在企业微信场景下被识别为虚拟显示器导致掉线的问题。

设计文档:[远程桌面管理平台-技术架构文档.md](远程桌面管理平台-技术架构文档.md)

## 架构

```
┌─────────────┐        ┌──────────────────┐        ┌─────────────┐
│  Agent       │  TCP   │   Relay Server     │  TCP   │  Viewer      │
│ (被控端,客户机) │ ─────▶ │ (Go, 公网服务器)    │ ◀───── │ (管理端,Qt)   │
│ C++ / Qt-free│ ◀───── │ 注册 + 鉴权 + 转发  │ ─────▶ │ C++ / Qt6    │
└─────────────┘        └──────────────────┘        └─────────────┘
```

- 三方都通过 TCP 单连接 + 自定义帧协议通信。
- Relay 不解析视频/输入 payload,只做注册、鉴权、单 Viewer ↔ 单 Agent 会话绑定 + 字节转发。
- Agent 用 DXGI Desktop Duplication 抓屏,FFmpeg 编 H.264 推流;
  反向接收 MOUSE/KEY 帧后调 `SendInput` 注入。
- Viewer 用 libavcodec 解 H.264,Qt6 Widgets 渲染;
  本地鼠标键盘事件归一化到 0..65535 后回推。
- **不**装虚拟显示驱动 / **不**装内核态输入驱动 —— 这是避开企业微信/游戏反作弊
  检测的关键设计(详见技术文档 §6)。

## 仓库结构

```
remote_desktop/
├── 远程桌面管理平台-技术架构文档.md   # 设计 + 决策
├── README.md                          # ← 你在这里
├── release.bat                        # Windows 一键打 agent/viewer release zip
│
├── relay/                             # Go 中转服务器
│   ├── cmd/{relay,fakeagent,fakeviewer}/
│   ├── internal/{protocol,safeconn,auth,hub,server}/
│   ├── config.example.json
│   └── deploy/
│       ├── deploy.sh                  # 一键 cross-compile + scp + systemd
│       └── remote_desktop_relay.service
│
├── agent/                             # 被控端 (C++ / Windows)
│   ├── CMakeLists.txt, vcpkg.json
│   ├── src/{capture,encode,input,net,util}/
│   └── cmd/{capture_test,encode_test,agent}/
│
└── viewer/                            # 管理端 (C++ / Qt6 / Windows)
    ├── CMakeLists.txt, vcpkg.json
    ├── src/{decode,net,ui}/
    └── cmd/{decode_test,viewer}/
```

每个子项目自己有 README,涵盖各自的构建、运行、设计要点。

## 当前进度(对应技术文档 §7)

- [x] **1.** Relay MVP — 注册/鉴权/权限/转发,fakeagent + fakeviewer 端到端跑通
- [x] **2.** Agent:DXGI 抓屏 → FFmpeg 编码 → 推 VIDEO_FRAME 到 Relay
- [x] **3.** Viewer:接 VIDEO_FRAME → libavcodec 解码 → Qt 渲染
- [x] **4.** Agent:收 MOUSE/KEY 帧 → SendInput 注入(光标移动实测验证)
- [x] **5.** Viewer:本地鼠标/键盘事件捕获 → 归一化 0..65535 → 回推
- [ ] **6.** 公网联调(局域网已通)
- [ ] **7.** 企业微信掉线问题实测验证(需要装着企业微信的机器)
- [ ] **8.** 设备列表 UI、断线重连、心跳超时、会话审计/录像

## 快速开始(开发机)

### 1. Relay(Go)

```powershell
cd relay
Copy-Item config.example.json config.json
go run ./cmd/relay -listen :7100 -config config.json
```

### 2. Agent(C++ / FFmpeg)

需要本机有 vcpkg 装好的 ffmpeg(`avcodec/swscale/avutil` 即可,
不必有 x264 —— 默认会用 Windows MediaFoundation 内置 H.264 编码器)。

```powershell
$env:VCPKG_ROOT = "<path-to-vcpkg>"
cd agent
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
.\build\Release\agent.exe -relay 127.0.0.1:7100 `
    -device-id device-001 -device-token agent-token-001
```

### 3. Viewer(C++ / Qt6 / FFmpeg)

需要 Qt6(msvc 构建)+ 上面那份 vcpkg ffmpeg。

```powershell
$env:VCPKG_ROOT = "<path-to-vcpkg>"
$env:QT_DIR     = "<path-to-qt>\msvc2022_64"
cd viewer
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
.\build\Release\viewer.exe -relay 127.0.0.1:7100 -account alice `
    -session-token viewer-token-alice -device-id device-001
```

详见各子项目 README。

## 发布 / 部署

| 目标 | 工具 |
|---|---|
| Windows 上打 agent / viewer 的 release zip | `release.bat [版本号]` |
| Linux 服务器上一键部署 relay | `relay/deploy/deploy.sh user@host` |



git clone https://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@github.com/tiger14779/remote-desktop.git /opt/remote-desktop-src && sudo bash /opt/remote-desktop-src/relay/deploy/install.sh



1.我认为 远程软件 无非就是 2步骤  1.抓屏   2.键鼠控制 , 那么他的检测也无非这两个部分 , 是否可以这样做呢  1.先屏蔽掉 键鼠操作 , 只用抓屏 做测试 ,如果仍然这样 , 是否 可以采用其他 抓图方案 , 比如 从显存中直接获取  , 或者 opencv图像抓取, 或者其他更好的办法wgc这类





```
交互:快捷键 (Ctrl+Alt+Del)、多显示器切换、全屏/窗口、录屏 
会话:自动重连、密码验证、连接白名单、会话时长 
安全:被控端允许弹窗、加密、审计日志、连接来源记录 
反检测:输入节流/抖动、被控端可视 banner 
生产力:文件夹传输进度、双向音频、被控端锁屏、观察者黑屏 
管理运维:管理面板(在线设备列表)、被控端自更新、Windows 服务化开机自启 
UI 打磨:系统托盘图标、通知、深色主题

1.增加Windows 服务化开机自启  主界面增加一个勾选框 勾选后注册开机自启 ,并且id延长到8位  怕重复
完成

2.完善 文件传输   剪切板拷贝功能			完成
3.增加互斥锁, 同时只能打开一个 , 再打开就打开之前的 主窗口显示出来  完成	
4.增加 被控 被远程的时候 提示用户 			完成
5.去掉debug调试部分


6.企业微信远程检测 实际是检测的虚拟键鼠输入 ,  我的解决办法是 使用kmbox net 盒子 , 来解决
E:\programme\SynologyDrive\work_project\newboos\qt_WxSight\src
这是我的企业微信自动化项目,里面已经有了 kmbox net盒子的 调用代码
,远程程序 采用 虚拟键鼠 + kmbox盒子键鼠 两部分 , 界面增加 ui  可以输入 kmbox盒子 地址  端口 和UUID
并且自动保存配置文件, 增加勾选框  kmbox硬件键鼠 , 勾选后 被控端 键鼠输入 使用硬件 , 防止企业微信掉线
```

