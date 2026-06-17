# Relay — 远程桌面中转服务器(Go)

第 1 步 MVP:在 Agent ↔ Viewer 之间做注册、鉴权、权限校验和帧转发,**不解析**数据帧 payload。

## 帧协议

```
[1 字节 type] [4 字节 BE length] [payload...]
```

| 类型              | 值     | 方向              | payload      |
|-------------------|--------|-------------------|--------------|
| REGISTER          | 0x01   | Agent → Relay     | JSON         |
| AUTH              | 0x02   | Viewer → Relay    | JSON         |
| CONNECT           | 0x03   | Viewer → Relay    | JSON         |
| SESSION_START     | 0x04   | Relay → Agent     | 空           |
| SESSION_END       | 0x05   | Relay → Agent     | 空           |
| VIDEO_FRAME       | 0x10   | Agent → Viewer    | 二进制       |
| MOUSE_MOVE        | 0x20   | Viewer → Agent    | 二进制       |
| MOUSE_BUTTON      | 0x21   | Viewer → Agent    | 二进制       |
| KEY_EVENT         | 0x22   | Viewer → Agent    | 二进制       |
| HEARTBEAT         | 0x30   | 双向              | 空           |
| REGISTER_RESP     | 0x81   | Relay → Agent     | JSON         |
| AUTH_RESP         | 0x82   | Relay → Viewer    | JSON         |
| CONNECT_RESP      | 0x83   | Relay → Viewer    | JSON         |
| ERROR             | 0xFE   | Relay → 任一方    | JSON         |

Relay 只感知前 8 类(REGISTER/AUTH/CONNECT/SESSION_*/响应),会话建立后其余帧
**按字节原样转发**。新增功能(如剪贴板、文件传输)只需要新增 type,Relay 不动。

## 会话生命周期

```
Agent             Relay               Viewer
  │  REGISTER →     │                    │
  │ ← REGISTER_RESP │                    │
  │     (idle, 只回 HEARTBEAT)           │
  │                 │  ← AUTH            │
  │                 │  AUTH_RESP →       │
  │                 │  ← CONNECT         │
  │ ← SESSION_START │                    │
  │                 │  CONNECT_RESP →    │
  │                                      │
  │ VIDEO_FRAME → Relay → VIDEO_FRAME → Viewer
  │ MOUSE_MOVE  ← Relay ← MOUSE_MOVE  ← Viewer
  │                                      │
  │                  (viewer 断开)
  │ ← SESSION_END   │                    │
  │     (回到 idle, 等下一次会话)
```

## 鉴权

`config.json` 一份 JSON 文件,启动时加载到内存。MVP 规模(~十几台)用配置文件足够,
改成数据库的时机是出现"设备/账号管理 UI"或"密钥轮换"这类需求。

```json
{
  "devices": {
    "device-001": "agent-token-001"
  },
  "users": {
    "alice": {
      "session_token": "viewer-token-alice",
      "allowed_devices": ["device-001"]
    }
  }
}
```

- Agent 用 `device_id + device_token` 注册。
- Viewer 用 `account + session_token` 登录,再带 `device_id` 请求会话。
- 权限表显式列出允许访问的 device_id;不在列表里直接拒。

后续接入真正的"账号密码登录拿 session_token"流程时,只要新增一个 HTTP 端点签发
token 写回这份内存表(或换成 DB)即可,帧协议层面不用动。

## 运行

```powershell
# 1. 准备配置
cp config.example.json config.json

# 2. 启动 relay
go run ./cmd/relay -listen :7100 -config config.json

# 3. 另开两个窗口,跑 mock 客户端做端到端验证
go run ./cmd/fakeagent  -relay 127.0.0.1:7100 -device-id device-001 -device-token agent-token-001
go run ./cmd/fakeviewer -relay 127.0.0.1:7100 -account alice -session-token viewer-token-alice -device-id device-001 -run 5s
```

`fakeviewer` 会打印收到的 VIDEO_FRAME 数量,`fakeagent` 会打印 MOUSE_MOVE,
两端日志应该看到对应的 SESSION_START / SESSION_END。

## 目录

```
relay/
├── cmd/
│   ├── relay/         # 服务端入口
│   ├── fakeagent/     # 模拟被控端(MVP 测试用)
│   └── fakeviewer/    # 模拟管理端(MVP 测试用)
└── internal/
    ├── protocol/      # 帧编解码 + 消息常量
    ├── safeconn/      # net.Conn + 写互斥锁
    ├── auth/          # 配置文件鉴权 / 权限表
    ├── hub/           # 设备注册表 + 会话绑定
    └── server/        # TCP 监听 + 协议握手 + 转发
```

## MVP 范围与下一步

已覆盖(对应文档 §7.1):
- [x] 注册/鉴权/权限校验
- [x] 单 Viewer ↔ 单 Agent 会话绑定
- [x] 数据帧双向透传
- [x] SESSION_START / SESSION_END 通知 Agent
- [x] 第二个 Viewer 抢同一台 Agent 直接拒(`device busy`)
- [x] 同一 device_id 重复 REGISTER 时,旧连接被踢下线(网络抖动后能自愈)

待做(对应文档 §7.2 及以后):
- [ ] C++/Qt Agent:DXGI 抓屏 → FFmpeg 编码 → 推 VIDEO_FRAME
- [ ] C++/Qt Viewer:接 VIDEO_FRAME → 解码渲染 → 捕获输入推 MOUSE/KEY
- [ ] 心跳超时(目前只反射,没有"长时间无心跳判死"逻辑)
- [ ] 会话审计、多 Viewer 并发观看、会话录像
