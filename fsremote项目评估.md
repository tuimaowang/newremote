# FSRemote 源码评估与新版参考方案

评估日期：2026-09-21  
源码目录：`C:\Users\test\Documents\Fsremote`  
报告目录：`C:\Users\test\Documents\ChatGPT\newfsremote`  
最终检查时 HEAD：`ada925a91ed1959989008444f6a787e5730f84ca`

## 1. 结论先行

**FSRemote 最值得继承的是 Windows 媒体处理和多人控制的实现经验；不建议直接继承它的网络控制面、身份管理组织方式和 SSH 文件传输链路。**

针对你已经确定的方向，建议继续采用：

- 原生 `libwebrtc` 作为实时音视频与数据通道基础，优先满足功能和性能。
- Qt 作为 Windows 界面层，将设备管理、会话控制和媒体核心从界面业务中分离。
- 每个安装实例同时具备发起控制和接受控制的能力，角色属于具体会话，不属于安装包。
- 中央服务器负责账户、设备登记、在线状态、授权、信令和必要中继，不默认解码桌面视频。
- 独立应用身份系统负责设备密钥、用户身份和权限；**OpenSSH 仅用于远程命令、脚本和 Shell**。
- 文件传输改为独立的应用文件协议，不使用 SCP/SFTP，也不通过 OpenSSH 完成应用身份认证。

这不是“从零重写所有细节”：可以迁移经过验证的媒体组件、状态机和测试，但应重新定义其上层协议与安全边界。

### 1.1 本报告的证据边界

本次依据实际工作目录、生产构建入口和调用链做静态检查。目录中存在未跟踪的文档、实验和渲染重构文件；不能把“文件存在”或“任务标记完成”视为已经接入产品。

检查期间 HEAD 从 `b20d87191086e6563dee723d3f9c112ce32e4fea` 前进至上述提交，差异为两个安装器文件，不涉及本文重点分析的媒体及控制链路。本报告不是某个提交的完整审计，也未全面评估安装器。

**没有执行构建、测试、程序启动、远程连接或 GPU 性能测试；所有性能收益均为设计分析，不是实测结论。安全问题也未进行攻击验证。** 旧目录中的评估文档仅作为索引，本文关键结论以源码为依据。本文只新增报告，不修改旧项目或原架构总结。

## 2. 旧项目实际是什么架构

### 2.1 同一个程序已经兼具两种角色

主程序启动 Host、状态服务、命令服务和便携 OpenSSH，然后创建 Qt 主窗口；用户可以再从界面发起 Viewer 会话。因此它并不是“控制端安装包”和“被控端安装包”完全分离的结构。

```text
一台 Windows 电脑上的 FSRemote.exe
|
+-- Qt 设备列表、远程窗口、任务和设置
+-- Host：接受别人的观看或控制
+-- Viewer：观看或控制其他设备，可开多个窗口
+-- StreamRuntime / C ABI / WebRTC 媒体模块
+-- 状态、命令、剪贴板与文件业务
+-- PortableOpenSshManager
    +-- SSH 终端
    +-- SCP/SFTP 文件传输
    +-- 借助 ssh-keygen 进行应用层签名
```

源码依据：[main.cpp 启动入口](C:/Users/test/Documents/Fsremote/src/main.cpp:328)、[Qt 主程序构建](C:/Users/test/Documents/Fsremote/CMakeLists.txt:586)。

**Host/Viewer 是会话角色，Agent/Core 是本机执行组件，这两组概念不是同一维度。** 新版可统一称“设备节点”，节点内部按会话承担 Host 或 Viewer，避免让人误以为 Agent 只能被控。

### 2.2 前后端有接口边界，但没有完成进程分离

旧项目已经有 `FsRemoteStreamApi.h` 的 C ABI、配置结构和回调，以及 `StreamRuntime` 包装层。这是可借鉴的解耦基础；但它主要是同一应用进程中的 DLL/API 边界，不等于独立后台服务加 IPC。

源码依据：[媒体 C ABI](C:/Users/test/Documents/Fsremote/include/FsRemoteStreamApi.h:27)、[StreamRuntime](C:/Users/test/Documents/Fsremote/src/stream/StreamRuntime.h:1)。

新版应保留明确的会话接口和异步事件模型，但需要另行设计 IPC、客户端断开处理、服务重启恢复和本地调用方鉴权。D3D11 指针不能直接作为跨进程参数；共享纹理需要明确句柄权限、打开方式、同步协议及所有权。

### 2.3 旧版通信通道

| 默认端口/通道 | 实际用途 | 新版处理意见 |
| --- | --- | --- |
| TCP 49100 | 桌面连接握手与 SDP/ICE 信令 | 改为经过认证的信令系统 |
| TCP 49101 | 设备状态服务 | 汇入设备状态模型与中央在线服务 |
| TCP 49102 | 电源、SSH 公钥登记、文件准备/提交等命令 | 不照搬裸命令监听 |
| TCP 49103 | OpenSSH，旧版同时服务终端和文件传输 | 新版仅保留远程命令能力 |
| UDP 49104 | 局域网实时设备状态 | 可保留受限发现能力，不作为身份权威 |
| TCP 49105 | 独立系统音频流 | 改为 WebRTC 音轨 |
| WebRTC 媒体与 DataChannel | 视频、输入、部分剪贴板等 | 继续使用，补齐协议和公网能力 |

端口是当前默认实现，不是建议对公网开放的清单。WebRTC 媒体端口还由 ICE/传输配置决定，不能把 TCP 49100 当成全部媒体流量。

源码依据：[服务启动](C:/Users/test/Documents/Fsremote/src/main.cpp:380)、[SSH 端口](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:31)、[实时状态服务](C:/Users/test/Documents/Fsremote/src/system/DeviceRealtimeStateService.cpp:1)、[音频端口](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/system_audio_stream.cpp:44)。

## 3. 功能参考总表

| 功能 | 旧版实现原理 | 参考价值 | 新版建议 |
| --- | --- | --- | --- |
| 双向控制 | 同实例启动 Host，并按需创建 Viewer | 高 | 继承会话角色模型 |
| 桌面采集 | DXGI/D3D11，纹理租约与有限缓冲 | 很高 | 作为媒体模块原型 |
| 硬件编解码 | 实际主链路为 H.265 NVENC + D3D11VA | 高，但有硬件约束 | 抽象能力探测和多厂商后端 |
| 桌面呈现 | D3D11 共享纹理、DirectComposition | 很高 | 保留 GPU 驻留思路，重新确认线程边界 |
| 多人观看 | Host 共享采集源，多会话传输 | 高 | 共享采集，分别做会话授权和质量控制 |
| 多人输入 | 按会话维护按键持有关系 | 很高 | 直接参考状态机，适配你的驱动 |
| 一控多群控 | 主窗口、代际和按键释放屏障 | 高 | 保留防重复、防残留按键语义 |
| 画质调节 | 依据窗口状态调整帧率和画质 | 中高 | 加入网络、解码和 GPU 的真实反馈 |
| 文件粘贴 | 准备、传输、提交，目标侧 IFileOperation | 很高 | 保留事务语义，替换 SSH 传输 |
| 设备身份迁移 | 稳定指纹、签名证明、端点代际 | 高 | 迁入独立身份服务 |
| SSH 终端 | 调用 ssh.exe，管理子进程生命周期 | 中高 | 独立成远程命令模块 |
| 自动更新 | 等待退出、备份替换、失败回滚 | 中 | 补签名验证后迁移 |
| 公网穿透 | 当前 PeerConnection 未配置 STUN/TURN | 不完整 | 新增中央信令和中继体系 |
| 锁屏/UAC | 有隔离 PoC，不能视为主程序已支持 | 待验证 | 单独设计服务及桌面会话模型 |

以下章节给出各项的具体依据和限制。

## 4. 最值得参考的媒体链路

### 4.1 当前主链路是 H.265，而不是文件名暗示的 H.264

```text
被控设备
DXGI 桌面采集 -> D3D11 纹理 -> GPU 处理/编码输入转换
-> H.265 NVENC 自定义编码器 -> WebRTC 视频传输

控制设备
WebRTC 视频接收 -> FFmpeg HEVC/D3D11VA 解码
-> 共享 BGRA 纹理 -> D3D11 / DirectComposition -> Qt 窗口
```

生产编码和解码工厂目前只公布 `H265`。虽然存在 `nvenc_h264_encoder.cpp` 这样的命名，内部使用的是 `NV_ENC_CODEC_HEVC_GUID`。不能依据文件名或其他候选 codec 列表，推断当前主链路已经具有 H.264 自动回退。

源码依据：[编码工厂](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:1191)、[解码工厂](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:1229)、[NVENC 实际配置](C:/Users/test/Documents/Fsremote/third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:89)、[D3D11VA 解码](C:/Users/test/Documents/Fsremote/third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:141)。

建议：以这条链路作为 NVIDIA 环境的首个性能样本，但将 H.264 兼容路径、Intel/AMD 编码后端、软件回退和可选 HEVC/AV1 分别列为待实现项。最终选择应由双方能力协商决定，不能把“使用 libwebrtc”理解为所有硬件编码器已经就绪。

### 4.2 应继承 GPU 驻留，不应笼统宣称全链路零拷贝

采集端使用有限纹理槽和租约，避免正在被消费的纹理被覆盖；槽全部被占用时丢帧。呈现端通过共享纹理和 GPU 路径显示，不要求每一帧都回读 CPU 再交给 Qt 图片控件。

代码仍然存在 `CopyResource` 等 GPU 内部拷贝。因此准确目标应是“尽量不做 CPU 回读与上传，减少不必要拷贝”，不是没有任何复制。

源码依据：[采集纹理槽与租约](C:/Users/test/Documents/Fsremote/third_party/lan_stream_probe/src/dxgi_capture.cpp:328)、[共享纹理打开](C:/Users/test/Documents/Fsremote/src/ui/D3D11FramePresenter.cpp:1052)、[DirectComposition](C:/Users/test/Documents/Fsremote/src/ui/D3D11FramePresenter.cpp:401)。

### 4.3 有界队列值得保留，但两种队列的语义不同

| 队列 | 当前满载行为 | 注意事项 |
| --- | --- | --- |
| 编码输入 `LatestEncodeFrameSlot` | 新帧替换未处理的旧帧，并继承关键帧请求 | 适合减少待编码帧龄 |
| UI 纹理 `LatestTextureFrameSlot` | 保留待消费旧帧，拒绝新帧 | 为保持 keyed mutex 交接，不能直接覆盖句柄 |

两个队列都限制积压，但不能都描述为“最新帧覆盖旧帧”。UI 类的 `replacedFrameCount()` 名称也容易误导，实际统计的是新纹理被拒绝的次数。

源码依据：[编码单槽](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/latest_encode_frame_slot.h:19)、[纹理单槽](C:/Users/test/Documents/Fsremote/src/ui/LatestTextureFrameSlot.h:43)、[纹理槽测试](C:/Users/test/Documents/Fsremote/tests/latest_texture_frame_slot_tests.cpp:1)。

新版若改成呈现端“最新帧优先”，必须同时设计旧纹理归还、取消、设备丢失和窗口关闭语义；不能只修改容器覆盖策略。建议记录采集时间、解码时间、呈现时间、丢帧原因和队列年龄。

### 4.4 共享采集不等于共享编码

`HostMediaPipeline` 通过订阅管理共享采集源，各会话复用源；WebRTC 网络、worker、signaling 线程也存在进程级共享机制。这些都值得参考。

但当前 Host 启动后持有长期 `startup_media_subscription_`，因此不能说“最后一个远程用户断开就自动停止采集”。另一方面，共享采集源也不代表只编码一次后向所有人发送：当前仍有各 PeerConnection 的编码路径与成本。

源码依据：[采集订阅接口](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/host_media_pipeline.h:1)、[长期启动订阅](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2933)、[共享 WebRTC 线程](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:23)。

建议第一阶段维持共享采集、各会话独立码率控制。若以后引入共享编码或服务器分发，应单独验证不同接收方带宽、分辨率、丢包和关键帧需求，不能为了共享而破坏拥塞控制。

### 4.5 呈现线程重构尚不能算已完成

当前生产构建接入的是 `D3D11FramePresenter`，远程窗口仍通过 Qt drain 路径取帧和调用呈现。工作目录里的 `RemoteVideoRenderWorker`、`RemoteVideoRenderService` 等新文件尚未出现在所检查的生产 CMake 源列表及主调用链中。

源码依据：[生产 Presenter 构建项](C:/Users/test/Documents/Fsremote/CMakeLists.txt:622)、[窗口取帧与呈现入口](C:/Users/test/Documents/Fsremote/src/ui/RemoteDesktopWindow.cpp:6363)。

建议把“专用渲染线程、与设备列表/UI 重绘隔离”当成新版待验证任务，而不是旧版已经交付的能力。当前 UI 线程是否构成瓶颈，需要实际采样确认。

## 5. 多人控制和群控可以重点复用

### 5.1 多人同时操作一台设备

旧版不是简单地把所有人的 keydown/keyup 原样注入，而是维护“会话持有哪些键”以及“每个键被多少会话持有”。首次持有触发按下，最后一个持有者释放时才触发抬起，断线时释放该会话的状态。

这解决了 A 按住 Ctrl 时 B 的释放操作不应把 A 的 Ctrl 提前放开的问题，也能减少断线后的粘键。鼠标按钮也有对应管理；鼠标指针位置的竞争则仍需产品策略。

源码依据：[共享输入状态](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/shared_input_state.h:28)、[会话准入策略](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/control_admission_policy.h:1)、[断线释放](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3495)。

你已经有驱动，仍然需要这层状态管理。建议接口顺序是：权限检查 -> 会话输入状态机 -> 驱动适配器。驱动注入成功不等于调用者有控制权限。

### 5.2 一个人同时控制多台设备

`RemoteInputBroadcastCoordinator` 管理群控主窗口、端点和代际，在切换主窗口或移除端点时释放输入，并拒绝旧代际事件；输入来源标记用于减少自注入事件再次广播形成循环。

源码依据：[主窗口切换屏障](C:/Users/test/Documents/Fsremote/src/ui/RemoteInputBroadcastCoordinator.cpp:160)、[代际检查](C:/Users/test/Documents/Fsremote/src/ui/RemoteInputBroadcastCoordinator.cpp:213)、[输入来源标记](C:/Users/test/Documents/Fsremote/include/fsremote_input_origin.h:1)。

新版还应规定不同 DPI/分辨率时采用绝对坐标映射还是相对移动，以及每个目标独立授权、部分失败反馈和断连后的全量状态收敛。

### 5.3 质量策略需要与真实反馈结合

旧版会根据可见、最小化、遮挡等状态调整质量，值得用于多窗口资源分配。但 `aggregateReceiveBudgetMbps` 在所检查的质量策略中只是保留字段，不能因此宣称已实现总带宽动态预算。

源码依据：[窗口质量协调](C:/Users/test/Documents/Fsremote/src/ui/RemoteQualityCoordinator.cpp:110)、[质量策略字段](C:/Users/test/Documents/Fsremote/src/stream/RemoteQualityPolicy.h:29)。

新版建议同时观察 WebRTC 发送反馈、解码耗时、GPU 占用和窗口重要性。先定义观测指标，再增加全局资源分配策略。

## 6. 剪贴板与文件传输：保留事务，替换通道

### 6.1 旧版值得保留的不是 SCP，而是文件落地流程

当前流程大致是：目标端准备粘贴位置 -> 返回缓存/任务信息 -> SSH/SCP/SFTP 传输实际文件 -> 提交 -> 目标侧通过 `IFileOperation` 完成粘贴和冲突交互。

代码包含前台目标冻结、随机 token、租约、缓存清理，以及等待真正操作结果后再报告成功。这比“发完文件就认为粘贴完成”更完整。剪贴板上的文件清单消息只描述文件，并不等于承载了文件内容。

源码依据：[文件清单编码](C:/Users/test/Documents/Fsremote/src/ui/RemoteClipboardCodec.cpp:1)、[实际调用 SSH 复制](C:/Users/test/Documents/Fsremote/src/ui/RemoteDesktopWindow.cpp:4608)、[目标侧文件操作](C:/Users/test/Documents/Fsremote/src/system/RemoteFilePasteService.cpp:96)、[任务租约](C:/Users/test/Documents/Fsremote/src/system/RemoteFilePasteService.cpp:165)。

### 6.2 新版明确不通过 SSH 传文件

建议第一版使用专用、可靠的 WebRTC 文件 DataChannel，定义应用协议：

1. `Prepare`：目标端检查文件权限、目标目录和配额，创建绑定会话的任务。
2. `Manifest`：发送版本化文件清单和长度，校验路径，不信任对端提供的绝对路径。
3. `Chunk`：有界分块传输，支持进度、取消、超时及背压。
4. `Verify`：检查实际长度和内容摘要，处理不完整任务；摘要本身不是发送者身份验证。
5. `Commit`：目标端完成实际落地/粘贴，返回最终结果。
6. `Cleanup`：清理取消、失败和过期任务，禁止跨会话重用 token。

文件通道与输入通道分开管理队列和预算；即使是两个 DataChannel，也仍可能竞争同一链路资源，必须限制文件流量。断点续传、目录树和大文件支持均需另行实现与测试，不能自动归功于 WebRTC。

## 7. 身份系统：参考证明机制，独立于 OpenSSH

### 7.1 旧版已有可参考的身份设计

旧版有设备独立密钥、指纹、随机挑战、签名证明、端点代际，并根据验证结果处理 IP 变化。设备身份不应等于 IP、电脑名或 MAC，这一点值得继承。

但是签名和验签通过 `PortableOpenSshManager` 调用 `ssh-keygen -Y sign/verify` 实现，与 SSH 终端、文件和授权密钥管理集中在同一模块。

源码依据：[会话签名](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:1264)、[设备证明签名](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:1378)、[独立设备私钥路径](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:2260)、[身份迁移协调](C:/Users/test/Documents/Fsremote/src/system/DeviceEndpointMigrationCoordinator.cpp:164)。

**这里的 ssh-keygen 通用签名不等于建立 SSH 网络登录会话。** 问题在于模块与凭证职责耦合，不能笼统说旧版所有身份验证都经过 SSH 网络协议。

### 7.2 新版模块建议

| 模块 | 职责 | 不应承担 |
| --- | --- | --- |
| `DeviceIdentityService` | CNG 生成和使用设备密钥，注册、挑战签名、轮换和吊销 | SSH 登录、文件复制 |
| `UserAuthService` | 用户登录、组织归属和权限 | 以设备在线状态替代用户授权 |
| `SessionAuthorization` | 短期授权票据、目标、发起方、角色和能力约束 | 单凭 IP 放行 |
| `SshCommandService` | SSH 主机验证、命令登录凭证、进程及会话管理 | 主应用设备身份、桌面、剪贴板和文件传输 |

设备密钥继续沿用架构文档中的 Windows CNG / ECDSA P-256 方向，安装或首次启动时生成，服务器只登记公钥与归属。是否必须硬件保护需要单独定义，不应把“使用 CNG”自动等同于“私钥必定在 TPM”。

会话签名内容应版本化、无歧义编码、用途隔离，并绑定发起方、目标、双方随机数、会话标识、票据及允许能力。首次绑定、重新安装、密钥轮换和设备转移需要明确的管理流程。

### 7.3 必须把应用身份与实际媒体连接绑定

旧版 `build_challenge_context` 绑定双方 ID、nonce、角色、版本和主机身份指纹，但没有绑定 SDP 或 WebRTC DTLS 证书指纹；后续信令通过普通 TCP 发送。在检查到的这条路径中，不能仅凭挑战验签就推导“后续 WebRTC 对端一定是已验证设备”。

源码依据：[挑战上下文](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:367)、[信令帧封装](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/signaling.cpp:44)、[后续消息处理](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3479)。

新版首先要声明信任模型：信任中央服务器时，使用经过认证的 WSS 信令、会话路由授权和票据校验。如果还要求中央服务器不能替换媒体对端，则另外签名绑定双方 DTLS 指纹及协商 transcript，并确认实际连接对应已验证的协商结果；这还需要可信的设备公钥绑定。仅加 WSS 不能解决恶意受信服务器问题。

## 8. OpenSSH 仅用于远程命令

### 8.1 可以参考的部分

旧版封装 `ssh.exe` 进程启动，并通过 Job 等机制管理进程树生命周期；这些适合独立命令模块。旧版默认启动外部终端窗口，不代表已经实现 Qt 内嵌终端。

源码依据：[终端启动](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:425)、[子进程创建](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:480)。

### 8.2 新版必须与旧版不同的地方

- 不调用 SCP/SFTP，不把 OpenSSH 管理器用于设备签名和桌面授权。
- 固定验证目标 SSH 主机公钥或受信主机证书，禁止为了免提示而忽略主机身份。
- 将应用用户与明确的目标 Windows 登录账户、权限等级关联，不能默认全部以管理员身份运行。
- 根据产品策略关闭 SFTP subsystem 及不需要的端口转发等能力；具体 Windows OpenSSH 版本支持情况须在实现时验证。
- 无法直连时设计独立受控的 SSH 字节中继/隧道，服务器可只转发加密 SSH 流；不能假设配置 TURN 后标准 ssh.exe 就会自动使用它。

源码依据：旧版配置中有 [SFTP subsystem 与 StrictModes](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:1745)，以及 [忽略主机密钥检查](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:110)。这些不能原样迁移。

### 8.3 对原架构文档第 19.2 节的补充澄清

“应用授权后才能建立 SSH 命令会话”的职责划分没有问题，但要补上真实执行机制：

1. OpenSSH 不会自动理解应用的 `command_ticket`。需要命令服务校验票据后，通过受控凭证签发、SSH 服务端配置或可执行的认证集成，让服务端实际拒绝未授权登录；不能只在 Qt 按钮处检查一次。
2. 临时 SSH 公钥必须绑定实际证明持有该私钥的会话发起方，短期证书或授权项还要约束目标账户、用途和有效期。
3. 删除 `authorized_keys` 项或凭证过期不能作为已建立 Shell 自动断开的保证。撤销、超时、用户退出时，需要显式追踪并终止已授权的 SSH 会话及相关进程。
4. 应用设备密钥、SSH 登录密钥和 SSH 主机密钥是三种职责，不复用为一个万能密钥。
5. 允许任意 Shell 时，用户本身可能通过命令读取、输出或上传下载文件。这里的“SSH 仅用于命令”是产品传输职责划分，不是阻止任意数据传输的沙箱保证；严格限制必须进一步约束命令与系统权限。

原架构文档第 19.3 节中“兼容 scp”的措辞也应在后续修订时删除或注明不对产品开放，以免与已经确定的 SSH-only-command 要求矛盾。本次报告没有修改原文。

## 9. 不能直接照搬的安全与公网问题

### 9.1 优先级最高：命令监听没有统一应用鉴权

`DeviceCommandServer` 在 `AnyIPv4` 监听并创建命令连接。检查到的命令分派直接处理 `shutdown`、`restart` 和 `authorize_ssh_key`，未经过统一的认证及权限校验；公钥登记随后写入授权文件。

源码依据：[监听与连接创建](C:/Users/test/Documents/Fsremote/src/system/DeviceCommandService.cpp:837)、[电源命令](C:/Users/test/Documents/Fsremote/src/system/DeviceCommandService.cpp:593)、[SSH 公钥登记命令](C:/Users/test/Documents/Fsremote/src/system/DeviceCommandService.cpp:793)、[写入授权入口](C:/Users/test/Documents/Fsremote/src/system/PortableOpenSshManager.cpp:1210)。

这是代码级高优先级风险：端口可达且功能启用时，不能依靠客户端 UI 或“只在局域网使用”保证安全。实际可达性仍取决于防火墙和部署环境，本次未测试。

新版应在目标 Core 强制鉴权，把命令、文件、桌面、输入分别授权；所有外部请求都要有长度、并发和超时限制，拒绝未绑定会话的公钥登记。

### 9.2 当前 WebRTC 还不是完整公网方案

创建 PeerConnection 时清空 ICE servers，所检查的主链路没有 STUN/TURN 配置，信令是普通 TCP 长度帧。这不代表 WebRTC 库没有 ICE 能力，而是产品尚未配置完整的穿透与中继基础设施。

源码依据：[RTCConfiguration](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/webrtc_session.cpp:804)、[普通 TCP 信令](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/signaling.cpp:20)。

新版需要经认证的信令、STUN/TURN、限时中继凭证、ICE restart、断线重连及 relay-only 测试。你的服务器可以继续以 Windows 为宿主；TURN 的实现及运行方式需要单独验证，必要时评估隔离的 Linux VM，不能未经验证承诺所有组件都已有 Windows 原生部署方案。

### 9.3 独立音频通道未继承 WebRTC 的安全边界

旧版通过 WASAPI loopback 获取系统音频，然后以独立 TCP 发送音频数据。检查到的 accept 路径直接 `add_client`，Viewer 通过 IP 和 49105 启动，没有传入会话 token；虽然其他代码存在音频 token 字段，不能据此认为该 TCP 音频连接已完成校验。该路径未见 TLS 或 WebRTC 音轨加密。

源码依据：[音频发送](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/system_audio_stream.cpp:528)、[音频连接接入](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/system_audio_stream.cpp:777)、[Viewer 音频启动](C:/Users/test/Documents/Fsremote/third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3795)。

建议复用 WASAPI 采集经验，将系统音频接入 WebRTC audio track 和 Opus，统一会话授权、网络适配及音视频时间线；系统音频和麦克风仍应分别授权。

### 9.4 更新流程缺少可确认的发布者校验

旧 updater 有等待退出、备份、文件替换和回滚机制，但检查到的 `copyVerified` 主要验证文件存在和大小，相关更新路径未查到明确的发布者签名校验。

源码依据：[copyVerified](C:/Users/test/Documents/Fsremote/src/updater/main.cpp:354)、[备份与替换](C:/Users/test/Documents/Fsremote/src/updater/main.cpp:391)。

新版可以继承生命周期管理，执行前则需要验证受信发布者签名、清单及包摘要，并定义防降级策略。相同文件大小不证明内容正确，单独摘要也不证明发布者身份。此结论范围是所检查的应用更新路径，不是对整个供应链的完整审计。

### 9.5 状态广播不是身份认证

旧实时状态有 `bootId`、序列号和 TTL，适合解决重启、乱序和过期；它们本身不使 UDP 广播具备真实性。必须将“发现一台机器”与“验证这台机器身份”区分开。

源码依据：[实时状态构造](C:/Users/test/Documents/Fsremote/src/system/DeviceRealtimeStateService.cpp:448)、[序列与过期处理](C:/Users/test/Documents/Fsremote/src/system/DeviceRealtimeStateService.cpp:1062)。

新版以认证连接和服务器租约管理在线状态，区分在线、可建立媒体连接、忙碌和无权限。局域网发现只能提供候选端点，不应自动赋予设备信任。

## 10. Qt、Core 和 Windows 服务怎么拆

建议按权限与生命周期拆，而不是机械地把所有代码放进一个高权限后台进程：

```text
Qt UI（普通用户权限）
  设备列表、远程窗口、文件任务、命令界面
          |
          | 本地 IPC：权限校验、版本、请求 ID、事件订阅
          v
Core / Session Manager
  身份注册、信令、授权、会话生命周期、文件协议
          |
          +-- 用户会话 Media Worker
          |   采集、WebRTC、编解码、剪贴板、用户桌面交互
          |
          +-- Windows Service Broker
          |   后台在线、受控启动/停止 Worker、必要特权操作
          |
          +-- SshCommandService
              仅命令凭证、OpenSSH 和命令进程管理

Qt 视频区域与 Presenter 的最终进程归属：通过 GPU 共享和渲染实验决定
```

这是一种建议的职责划分，是否让 Core 独立于服务进程需要根据恢复和性能原型确定。Windows 服务、登录用户桌面、锁屏和安全桌面属于不同执行环境，不能假设把 UI 进程改成 SYSTEM 服务就自然获得全部桌面能力。

旧项目的安全桌面相关内容位于隔离 PoC；本次没有确认已接入主产品，也没有验证 UAC、锁屏、用户切换和会话注销行为。参见 [安全桌面 PoC 任务](C:/Users/test/Documents/Fsremote/openspec/changes/windows-secure-desktop-agent-poc/tasks.md:1)。

本地 IPC 建议使用带 ACL 的命名管道等机制，并对调用者身份、会话归属和具体能力做检查。跨进程视频不通过普通 IPC 每帧搬运完整像素；优先验证 D3D11 共享资源协议，同时准备设备丢失及跨 GPU 回退。

## 11. 中央 Windows 服务器的定位

| 服务职责 | 建议 | 与旧版关系 |
| --- | --- | --- |
| 账户、设备目录 | 管理用户归属、设备公钥、权限与吊销 | 旧版目录模型可参考，中央体系需新增 |
| 在线与信令 | 双方主动连出，认证 WSS，交换会话与 ICE 信息 | 替代局域网直连信令入口 |
| 授权 | 签发绑定双方和能力的短期会话票据 | 目标节点仍需执行权限检查 |
| 媒体中继 | 需要时通过 TURN 转发，不默认解码 | 旧版缺少可直接继承的完整配置 |
| SSH 命令中继 | 独立受控字节通道，绑定命令会话 | 不等同于 TURN 自动转发 ssh.exe |
| 更新分发 | 签名包、版本清单、灰度和回滚策略 | 可参考旧 updater 的退出与替换流程 |
| 审计 | 记录谁在何时获得何种能力、会话及撤销结果 | 不能仅靠客户端日志 |

第一版不建议默认采用 SFU 或服务器解码转发所有桌面。多人同看一台设备、所有观看流都必须经服务器或大规模广播等需求明确后，再评估 SFU。TURN 与 SFU 是不同职责，服务器是否可信、能否看到媒体明文也需要分别定义。

## 12. 建议的实施与验证顺序

### 阶段 A：先做媒体基线

把现有 DXGI -> NVENC -> WebRTC -> D3D11VA -> Presenter 链路抽成可独立测试样本。记录机器型号、驱动、分辨率、帧率、码率及端到端延迟；保留 HEVC 样本，同时明确兼容性补齐工作。

### 阶段 B：建立身份和公网会话

实现独立设备密钥、用户授权、认证信令、DTLS 身份绑定策略和 TURN；先打通两台节点互控，再加入多会话。禁止沿用裸 `authorize_ssh_key` 等入口。

### 阶段 C：迁移输入和桌面体验

迁移共享输入状态机、群控代际管理、窗口质量策略、剪贴板和光标协议。先验证你的驱动适配接口，再进行多控制端测试。

### 阶段 D：迁移文件和命令

保留文件准备/提交与缓存清理语义，用新文件协议传输内容。SSH 单独实现命令凭证、目标主机验证、账户映射、吊销后的会话终止和网络中继。

### 阶段 E：完成 Windows 产品化

完成服务与用户进程生命周期、锁屏/UAC 原型、签名更新、设备恢复和长期稳定性测试。第三方代码、编解码器及驱动的许可证和可再分发条件单独核对，本报告不作合规结论。

### 验证清单

| 类别 | 至少验证的情况 |
| --- | --- |
| 网络 | 同网段、跨 NAT、UDP 受限、强制 TURN、网络切换、ICE restart |
| 媒体 | 不同厂商 GPU、编码能力缺失、D3D 设备丢失、分辨率/DPI/显示器热插拔 |
| 延迟 | 采集到编码、接收后解码、解码到呈现、输入到画面响应及 P95/P99 |
| 多人 | 多 Viewer、控制与旁观并存、断线释放、重连代际、群控目标部分失败 |
| 安全 | 未授权命令、公钥登记拒绝、票据重放、角色提升、SDP 身份绑定、吊销 |
| 文件 | 超大文件、取消、断点、路径越界、冲突、磁盘满、提交失败、缓存过期 |
| SSH | 主机密钥不匹配、凭证过期、已建会话撤销、账户权限、转发限制 |
| Windows | 注销、锁屏、UAC、快速用户切换、Core/Worker/UI 分别崩溃与重启 |
| 更新 | 包被篡改、清单错误、断电、替换失败、回滚和旧版降级限制 |

## 13. 最终取舍

**保留实现思想并优先迁移：** GPU 纹理管线、资源租约、有界队列、共享采集、多人按键状态、群控代际、文件粘贴事务、端点迁移及已有针对性单元测试。

**必须重新设计：** 公网信令与中继、应用身份模块、统一授权入口、SSH 凭证与主机校验、文件字节传输、音频安全通道、服务与用户桌面边界、更新发布者验证。

**暂不作为已经具备的能力：** 全厂商硬件编解码、自动 H.264 回退、完整公网穿透、生产级独立渲染线程、主程序锁屏/UAC 支持，以及任何未经测试的延迟或并发性能指标。

一句话：**以 FSRemote 的媒体和交互组件作为参考基础，以新的身份、权限和会话架构重新组织产品；Qt 负责界面，原生 libwebrtc 负责实时通信，OpenSSH 只负责远程命令。**
