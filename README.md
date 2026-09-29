# FSRemote Messages

最小原生 Windows 消息原型。界面参考旧 FSRemote 的浅色布局、240px 设备栏和蓝色选中态；图标取自本机旧项目，未修改旧项目源码。

## 独立公网连通性 Demo

先验证路由器映射与双向收发时，使用独立的 `FSRemoteNetworkDemo`，入口在
`dist/NetworkDemo`，操作说明见 `demo/README.txt`。它只交换固定探针消息，不读取原型的设备令牌或配置。
服务器运行 `Start-Server.cmd`；依次使用 `Test-Local.cmd`、`Test-LAN.cmd` 和真正外网电脑上的
`Test-Public.cmd` 定位问题。当前地址和端口由 `demo/endpoint.json` 配置。
这是临时明文连通测试，不代表最终产品的认证、加密或消息功能已经完成。

重新打包：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-network-demo.ps1`。
本机验证：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-network-demo.ps1`。

## 现在能做什么

- 启动独立消息服务端和多个 Qt 客户端。
- 使用每设备独立的访问令牌认证，查看配置组内的设备在线状态。
- 向选中设备发送纯文字，区分“等待服务器”“服务器已受理”“目标已收到”和“结果未知”。
- 断线自动重连，重新认证并刷新设备列表；不自动重发旧消息。

本轮刻意不做远控、SSH、文件、离线消息、账号管理或自动更新。此前要求的更新能力仍属于后续初版计划，不在这个通信原型中实现。

## 直接运行

已构建时，在项目根目录双击 `Start-Demo.cmd`。

也可以直接双击 `dist\FSRemoteMessageServer.exe`。不带 `--config` 时，服务器会在系统应用数据目录自动创建随机令牌和随机空闲端口，并生成 `client-device-a.json`、`client-device-b.json`。随后双击 `dist\FSRemoteMessages.exe`，客户端会自动读取 `device-a` 配置并连接，不需要手写 JSON。自动配置目录通常是 `%APPDATA%\FSRemote\FSRemoteMessages`。

脚本会选择一个空闲本机端口，生成随机的独立设备令牌，启动一个隐藏的服务器和两个可交互的客户端。选中对方，输入文字后点击“发送”。关闭客户端窗口退出客户端；脚本输出的 `stop-demo.ps1 -Session ...` 可停止整组演示进程。

也可以在 PowerShell 执行：

```powershell
.\scripts\start-demo.ps1
```

### 局域网自动配置测试

不想手写服务器地址、端口或令牌时，在作为服务端的电脑执行：

```powershell
.\scripts\start-lan-demo.ps1
```

服务端会自动选择本机局域网 IPv4、随机空闲端口，并生成两个独立令牌。脚本会把配置导出到 `lan-demo-data\<session>`；将其中的 `client-device-a.json` 和 `client-device-b.json` 分别复制到各测试电脑的 `FSRemoteMessages.exe` 同目录，直接双击客户端即可连接。首次运行 Windows 防火墙可能需要允许程序访问“专用网络”。

这是为了局域网联调提供的显式测试模式，使用未加密的 `ws://`，只应在可信的私有网络中使用；默认启动仍只监听本机回环地址。正式跨设备部署应改用 WSS 证书。

单独打开 `dist\FSRemoteMessages.exe` 时，通过右上角设置填写服务器地址、设备 ID 和已有访问令牌。这不是注册入口，设备必须预先配置在服务器端。

## 编译与验证

### Qt Creator

本机 Qt Creator 配置使用 Qt 6.11.1 MSVC 2022 x64，包含 Debug / Release 和 `FSRemoteMessages`（客户端）、`FSRemoteMessageServer`（服务端）、`message_tests` 三个运行目标。打开根目录 `CMakeLists.txt` 即可使用，运行参数保持为空；本机自动配置测试时先启动服务端，再启动客户端。

项目的 `CMakePresets.json` 固定当前机器的 Qt 路径，构建输出在 `build/qtcreator-msvc`。需要重新生成本机 IDE 设置时，关闭 Qt Creator 后执行 `scripts/configure-qtcreator.ps1`；脚本会备份并更新项目配置、注册本项目专用 Kit，保留其他 Kit。`.qtcreator` 中的个人设置及备份不纳入 Git。

同一构建配置也可在命令行验证：

```powershell
cmake --preset windows-msvc
cmake --build --preset debug
ctest --preset debug
```

依赖：Windows、CMake 3.24+、Visual Studio 2022 C++ 工具链、Qt 6.5+ 的 Widgets / Network / WebSockets / Test 模块。

```powershell
.\scripts\build.ps1 -QtRoot C:\Qt\6.11.1\msvc2022_64
```

脚本构建 Release、运行 Qt Test/CTest，并使用 `windeployqt` 将程序及 Qt 运行库部署到 `dist`。构建目录中的 `test-results.txt` 是测试明细，`screenshots` 保存实际 Qt 窗口的正常和紧凑尺寸截图。

## 程序职责

```text
FSRemoteMessages.exe A               FSRemoteMessages.exe B
Qt 界面 + MessageClient              Qt 界面 + MessageClient
           \                           /
            WebSocket 消息连接
                      |
           FSRemoteMessageServer.exe
           认证 / 设备状态 / 转发 / 回执
```

为了保持最小，客户端通信模块与 UI 逻辑分为独立代码模块，暂时在同一进程中运行，没有另建 Core 进程、Windows 服务或本地 IPC。

## 服务器配置

自动配置默认只绑定 `127.0.0.1`，适合本机测试。使用 `--lan` 或 `scripts/start-lan-demo.ps1` 时，软件会自动绑定局域网并生成客户端配置；该模式明确启用明文 WS，仅适合可信私有网络。正式跨设备部署应使用 WSS：服务器配置需要提供证书和私钥，客户端必须信任证书且主机名匹配。

启动命令：

```powershell
.\dist\FSRemoteMessageServer.exe --config C:\path\server.json
```

配置结构如下，实际令牌必须是独立随机值，不要使用示例占位符：

```json
{
  "listen": "127.0.0.1",
  "port": 17890,
  "devices": [
    { "id": "device-a", "name": "Desktop A", "token": "<unique-random-token-at-least-32-characters>" },
    { "id": "device-b", "name": "Desktop B", "token": "<another-independent-random-token>" }
  ]
}
```

客户端配置由 `FSRemoteMessages.exe --config C:\path\client.json` 加载并自动连接：

```json
{
  "server": "ws://127.0.0.1:17890",
  "device_id": "device-a",
  "token": "<the-token-provisioned-for-device-a>"
}
```

服务端绑定设备 ID、名称和令牌；客户端不能自己声明任意发送者身份。配置内设备被视为同一个可互发消息的测试组，目前没有分组 ACL 或用户角色。每个设备只允许一个在线连接，重复登录会被拒绝，不踢掉已有连接。

## 消息协议 v1

```json
{
  "version": 1,
  "id": "unique-request-id",
  "type": "message.send",
  "to": "device-b",
  "payload": { "text": "hello" }
}
```

响应通过 `reply_to` 关联请求。每条响应和事件都有自己的 `id`。服务端为转发事件生成新的投递 ID，并从认证连接补充 `from`；目标端以投递 ID 回执，服务器确认回执来自实际目标连接后，再通知发送方。

| 类型 | 方向 | 作用 |
| --- | --- | --- |
| `auth.login` / `auth.result` | 客户端请求 / 服务器响应 | 令牌认证 |
| `device.list` | 客户端可请求，服务器响应或主动推送 | 完整设备状态快照 |
| `message.send` | 客户端 -> 服务器 | 请求转发文字 |
| `message.accepted` | 服务器 -> 发起端 | 已受理并向目标连接排队发送 |
| `message.deliver` | 服务器 -> 目标端 | 携带可信来源的文字消息 |
| `message.received` | 目标 -> 服务器 -> 发起端 | 目标客户端已接收，不表示人已阅读 |
| `error` | 服务器 -> 客户端 | `payload.code` 为稳定错误码 |

上下线使用完整 `device.list` 快照，不再同时引入增量 online/offline 事件，以减少状态合并逻辑。心跳使用 WebSocket Ping/Pong，不占用业务消息类型。

## 限制和安全边界

- 明文 WS 只允许回环地址；服务器拒绝以明文绑定公网或局域网地址，客户端也拒绝非本机 WS URL。
- 服务端预留 WSS 配置：同时提供 `certificate` 和 `private_key` PEM 文件路径，路径相对于配置目录。客户端必须信任证书且主机名匹配，不忽略 TLS 错误。需要支持服务端 TLS 的 Qt 后端；跨机器 WSS 部署尚未做实际网络验收。
- 演示令牌以明文保存在被 Git 忽略的 `demo-data` 配置中，只适合本机实验。不要把这些配置上传或用于真实部署；正式设备密钥、凭证存储和用户授权尚未实现。
- 没有执行任何远程命令，也没有把消息当作代码执行。消息显示前做 HTML 转义。
- 最多 32 个配置设备、32 个活动连接，单消息 16 KiB，文字最长 4000 个 UTF-16 单元。每连接 10 秒最多 120 个请求，待发送队列有界，每发起端最多 16 个待回执任务。
- 认证超时 5 秒；投递回执超时约 5 秒。超时或断线可能发生在目标已经收到但回执丢失之后，因此显示“结果未知”，不承诺恰好一次，也不盲目重发。
- 请求 ID 在当前连接内去重；累计 8192 个请求后要求重新连接以限制内存，不提供跨重连持久去重。
- 聊天记录仅在内存中，每设备保留最近 200 条，关闭程序后清空。没有消息数据库或离线队列。
- 不修改防火墙、系统证书存储、系统服务或旧 FSRemote 配置。

## 源码索引

| 文件 | 职责 |
| --- | --- |
| `src/protocol.h` | 公共信封、编码、解析与边界 |
| `src/message_server.*` | 认证、在线连接、转发和回执 |
| `src/message_client.*` | 连接状态、重连、心跳和消息事件 |
| `src/main_window.*` | 最小 Qt 界面 |
| `src/server_main.cpp` / `src/client_main.cpp` | 配置与进程入口 |
| `tests/message_tests.cpp` | 协议、安全边界、网络和 UI 集成测试 |

本机验证覆盖协议校验、错误令牌、重复身份、伪造来源和回执、重复请求、目标离线、超长消息、无回执超时、断连、重连与真实界面双向发送。没有将本机回环测试视为公网、高并发或长期稳定性验收。
