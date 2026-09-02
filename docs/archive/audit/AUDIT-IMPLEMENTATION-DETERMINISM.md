# RUDP-RT Implementation Determinism Audit

## 审计问题

假设三名经验相当的 C/C++/Embedded 工程师只能阅读 `rudp-rt-design-v0.2.md`，不能向设计者提问，也不能参考其他实现。本审计不评价某个实现是否“可能写出来”，而评价三个人是否会自然得到基本一致的模块、接口、状态、生命周期和故障行为。

本轮只记录实现者必须自行决定、但文档没有冻结的内容。它与普通 bug review 的区别是：即使某个选择本身合理，只要文档没有唯一化，仍然是 `Missing Decision`。

## 总体判断

**三名工程师不能独立实现出行为基本一致的系统。**

他们大概率会共享以下高层轮廓：一个 Gateway Reactor、一个 Capture Worker、一个 Metrics Worker；RTP 作为媒体层；RUDP-RT 作为可靠层；DATA/ACK/NACK/ABANDON；虚拟 Clock 和虚拟网络先行。但从高层轮廓到可编译 API 之间仍有大量未冻结选择，尤其是：

- 客户端和媒体输入是否属于交付物；
- session 建立、重启、保活和初始序列如何区分；
- frame、packet、payload、timer、queue 的所有权；
- partial send、EAGAIN、ACK/NACK/ABANDON 交错时的线性状态转换；
- timeout/retry 的最终失败和 shutdown 的完成条件；
- FDR 和测试 oracle 的统计主键。

因此不同工程师会产生不同的 module boundaries、API contracts、wire behavior 和 failure behavior；这些差异会影响 correctness、interoperability、performance 和 testability，而不仅是代码风格。

## Determinism Matrix

| 领域 | 当前能否独立得到基本一致实现 | 主要原因 |
|---|---:|---|
| Module boundaries | 否 | 只给出逻辑方框，没有所有权、调用方向和进程边界 |
| Thread model | 部分可以 | 三线程和 Reactor 单写者明确，但唤醒、阻塞、失败和关闭屏障未定义 |
| State machine | 否 | Sender/receiver/session 有片段状态，缺少完整状态和事件优先级 |
| Ownership model | 否 | payload、RTP buffer、FrameState、timer、snapshot 的所有权未冻结 |
| Lifecycle | 否 | startup、reconnect、timeout、shutdown、crash recovery 没有闭合 |
| Timeout behavior | 否 | 公式存在，但起点、调度、优先级、取消和最终动作不唯一 |
| Retry behavior | 否 | DATA/ABANDON 有局部规则，控制错误和最终失败没有规则 |
| Error semantics | 否 | 错误名称存在，触发后的状态转换和返回 API 不存在 |
| Shutdown semantics | 否 | 有顺序，但没有 barrier、drain deadline、远端语义和最终分类 |
| Queue semantics | 否 | 只有部分队列有容量或丢弃策略 |
| Buffer semantics | 否 | reorder 有局部上限，Jitter、临时 buffer、SACK 保留未定义 |
| Protocol behavior | 否 | 初始序列、wire layout、控制报文合法性和 RTP profile 不完整 |
| API contracts | 否 | 没有模块级函数、对象、线程和错误契约 |

## Missing Decisions

### MD-01：交付物边界

- **缺失的决策是什么**：是否必须实现客户端、接收端 Jitter Buffer、播放/消费线程、Camera 适配器，还是只交付 Gateway 和协议库。
- **为什么实现时必须决定**：FDR 分子在接收端测量，协议测试又要求 receiver 行为；没有客户端边界就无法定义端到端完成条件。
- **不同工程师可能产生什么不同实现**：一人实现独立 client 进程，一人实现进程内 loopback receiver，另一人只实现协议库和测试桩。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-02：进程与部署拓扑

- **缺失的决策是什么**：Gateway、client、虚拟网络、fault injector、metrics 是否在同一进程、不同线程或不同进程；真实 socket 集成测试的进程边界是什么。
- **为什么实现时必须决定**：这决定 API 是函数调用、线程消息还是 UDP wire，也决定故障注入能否覆盖真实边界。
- **不同工程师可能产生什么不同实现**：一人把虚拟网络直接接在 sender/receiver 对象之间，一人用 localhost UDP，另一人拆成多个可执行文件。
- **差异影响**：correctness 中；interoperability 高；performance 高；testability 高。

### MD-03：Stream 配置和生命周期

- **缺失的决策是什么**：stream_id 从哪里配置、何时生效、是否允许运行时增删、一个 stream 是否只有一个源帧序列，以及未配置 stream 的配置存储格式。
- **为什么实现时必须决定**：Join、REJECT_UNKNOWN_STREAM、Packetizer 和多客户端 fan-out 都依赖 stream 配置。
- **不同工程师可能产生什么不同实现**：静态 CLI 配置、配置文件热加载、代码内固定 stream，或把每个 camera 自动视为一个 stream。
- **差异影响**：correctness 中；interoperability 高；performance 中；testability 高。

### MD-04：媒体帧 API 契约

- **缺失的决策是什么**：输入帧对象包含哪些字段（bytes、length、frame_id、RTP timestamp、capture time、format、deadline），字段由谁生成，payload 是否可变。
- **为什么实现时必须决定**：Packetizer、deadline、RTP timestamp、FDR 和 payload 生命周期都需要同一个明确的输入对象。
- **不同工程师可能产生什么不同实现**：一人用 owning `vector<uint8_t>`，一人用借用 span，另一人让 Packetizer 自己生成 frame_id/timestamp。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-05：媒体格式与 Camera 边界

- **缺失的决策是什么**：Synthetic Frame 和 Camera 输出的编码、像素格式、尺寸、帧率、timestamp 来源和格式变化行为。
- **为什么实现时必须决定**：RTP payloadization、frame_count、MTU 计算和最终 demo 都不能只依赖“媒体分片”这个抽象词。
- **不同工程师可能产生什么不同实现**：固定任意字节流、H.264 Annex-B、JPEG 或 raw bytes；Camera 可能被直接复制或先编码。
- **差异影响**：correctness 高；interoperability 高；performance 高；testability 高。

### MD-06：模块所有权边界

- **缺失的决策是什么**：Capture、Packetizer、RTP、RUDP sender、Socket adapter、Session Manager、Metrics 各自拥有还是借用哪些对象。
- **为什么实现时必须决定**：没有 ownership graph 就无法决定何时 move/copy/free，也不能安全执行 fan-out。
- **不同工程师可能产生什么不同实现**：跨模块共享 `shared_ptr`、每 session 深拷贝、单一 owner 传递，或使用裸指针和隐含生命周期。
- **差异影响**：correctness 高；interoperability 低；performance 高；testability 高。

### MD-07：模块接口形态

- **缺失的决策是什么**：模块之间使用同步函数、回调、事件、消息队列还是返回值；调用是否只发生在 Reactor 线程。
- **为什么实现时必须决定**：同一个“Packetizer → RUDP → UDP”图无法推导 C++ 类的线程安全和重入契约。
- **不同工程师可能产生什么不同实现**：一人使用纯状态机 `on_event`，一人使用对象回调，一人允许 sender 直接调用 socket。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-08：Clock API 契约

- **缺失的决策是什么**：`IClock::now()` 的类型、单位、精度、是否允许时间倒退、虚拟时间如何推进，以及一个 session 是否可使用多个 clock。
- **为什么实现时必须决定**：deadline、RTO、timer heap、playout 和测试所有依赖时间；仅指定 `now()` 不足以形成 ABI/API。
- **不同工程师可能产生什么不同实现**：纳秒 `int64`、毫秒 `uint64`、`time_point`；有人在组件内取整，有人在全局取整。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-09：网络抽象 API

- **缺失的决策是什么**：虚拟网络和真实 UDP 是否共享同一接口；send/receive 返回值、地址类型、时间戳、截断标志、EAGAIN 表示和 ownership 如何表达。
- **为什么实现时必须决定**：协议逻辑必须知道 datagram 是否完整发送、是否需要重试以及错误属于哪个对象。
- **不同工程师可能产生什么不同实现**：返回 `bool`、errno、结构化 `SendResult`，或将 socket 错误直接转换为协议事件。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-10：配置校验矩阵

- **缺失的决策是什么**：W、`max_frame_packets`、payload、D、playout_delay、RTO、buffer、max_sessions、预算等的合法范围和交叉约束。
- **为什么实现时必须决定**：文档给默认值但没有说明非法组合是否拒绝、修正还是运行时退化。
- **不同工程师可能产生什么不同实现**：有人启动失败，有人 clamp，有人让协议进入不可收敛状态。
- **差异影响**：correctness 高；interoperability 高；performance 高；testability 高。

### MD-11：Join/Restart/Heartbeat 的状态与消息区分

- **缺失的决策是什么**：PING 的角色、重复 PING 的副作用、PONG 的请求关联、会话建立时机和客户端重启识别方式。
- **为什么实现时必须决定**：§3.13 同时把已有 session 的 PING 视为重启、又把 PING 用作 keepalive，无法由实现者唯一推导。
- **不同工程师可能产生什么不同实现**：一人新增本地状态字段，一人增加消息类型，一人按来源/nonce 猜测，一人每次 PING 都重建。
- **差异影响**：correctness 高；interoperability 高；performance 高；testability 高。

### MD-12：Session identity 与 endpoint 变化

- **缺失的决策是什么**：session key 是否包含源端口、地址族、本地 stream、epoch/token；客户端源端口或 IP 变化时是迁移、重连还是新 session。
- **为什么实现时必须决定**：文档只写 `(client_addr, stream_id)`，但 UDP endpoint 的实际身份还包括端口和地址族。
- **不同工程师可能产生什么不同实现**：按 IP 合并、按 IP:port 新建、按 token 迁移，或拒绝地址变化。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-13：初始 ISN、recv_next 和 ACK 无值状态

- **缺失的决策是什么**：Gateway 如何把随机 ISN 告知 client，receiver 在 SYNC_START 前的 `recv_next` 和 ACK 如何表示，ACK 是否允许“尚未建立序列域”。
- **为什么实现时必须决定**：没有初始基线，首个 DATA 丢失、ACK/NACK 生成和窗口分类都无法唯一实现。
- **不同工程师可能产生什么不同实现**：PONG 携带 ISN、SYNC_START 隐式建立、使用特殊 ACK 值、直接接受第一个 seq，或缓存直到超时。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-14：Epoch/session token 与旧报文隔离

- **缺失的决策是什么**：是否需要 epoch/token、放在哪个报文、如何验证、随机 ISN 是否足够以及旧 ACK/NACK/DATA 的最终处理。
- **为什么实现时必须决定**：随机 ISN 只给概率性污染说明，不给控制面确定隔离语义。
- **不同工程师可能产生什么不同实现**：不加字段、复用 nonce、增加 token、仅依赖地址，导致 wire 不兼容。
- **差异影响**：correctness 高；interoperability 高；performance 低；testability 高。

### MD-15：完整 Session 状态机

- **缺失的决策是什么**：`NEW/NEGOTIATING/ESTABLISHED/QUIET/DEAD/CLOSING` 等状态是否存在、各事件的合法性、状态进入/退出副作用。
- **为什么实现时必须决定**：文档描述 Join、保持、超时和拆净，但没有状态节点和事件表。
- **不同工程师可能产生什么不同实现**：有人用布尔 `joined/timed_out`，有人实现多状态；同一报文在不同状态可能被接受或丢弃。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-16：Sender packet/frame 状态粒度

- **缺失的决策是什么**：Outstanding 是帧级还是同时包含逐包状态；部分已发送、SACK、逐包重传、逐包 RTO 如何表达。
- **为什么实现时必须决定**：文档给帧级结构，却给出逐 seq 的 NACK/SACK/RTO 行为。
- **不同工程师可能产生什么不同实现**：一人整个帧重传，一人逐包重传，一人添加隐含 packet map。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-17：Receiver sync-to-normal 状态切换

- **缺失的决策是什么**：SYNC_START 到达、丢失、重复、后续包反推和预同步缓存何时原子切换为正常窗口。
- **为什么实现时必须决定**：预同步包必须既不误 ACK，又不能因缓存升级丢掉合法包。
- **不同工程师可能产生什么不同实现**：按第一个 DATA 锚定、按 SYNC_START 锚定、按最小 seq 锚定，或等待整帧。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-18：Frame descriptor 定型与冲突

- **缺失的决策是什么**：首次 frame_id 包是否固定 `frame_count/rtp_timestamp/seq range`，冲突包丢弃单包、整帧还是关闭 session。
- **为什么实现时必须决定**：单包反推帧区间不等于规定跨包 descriptor 的一致性提交。
- **不同工程师可能产生什么不同实现**：后到 descriptor 覆盖前值、首包 wins、最大值 wins 或将帧标为协议错误。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-19：未入窗 frame_id 缺口

- **缺失的决策是什么**：capture drop、pre-admission drop、窗口等待超时和 join 边界造成的 frame_id 缺口如何对 receiver 可见、何时跳过。
- **为什么实现时必须决定**：D-18 让未入窗帧没有 seq/ABANDON，但 §3.11 又要求按 frame_id 顺序且允许跳过死帧。
- **不同工程师可能产生什么不同实现**：接收端等待、按 playout 自主跳洞、sender 发送 frame-level skip/ABANDON，或从 frame_id gap 推断丢失。
- **差异影响**：correctness 高；interoperability 高；performance 高；testability 高。

### MD-20：帧级原子入窗的失败回滚

- **缺失的决策是什么**：一帧“同时获得连续 seq、同时入窗”后，分配失败、部分 send、预算耗尽、EAGAIN 时如何回滚或了结所有 seq。
- **为什么实现时必须决定**：原子入窗并不等于原子写 socket；没有回滚规则会产生永久洞。
- **不同工程师可能产生什么不同实现**：分配前预留、允许部分发送、失败后 ABANDON、回滚 seq，或关闭 session。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-21：ACK 合法性与单调性

- **缺失的决策是什么**：stale ACK、future ACK、跨未分配 seq 的 ACK、重复 ACK、ACK 跳过 ABANDON 前后的处理和错误计数。
- **为什么实现时必须决定**：ACK 是唯一推进 send_base、释放资源和产生 RTT 样本的输入。
- **不同工程师可能产生什么不同实现**：接受任意 serial-forward ACK、只接受窗口内、忽略 future、关闭 session 或回发错误。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-22：SACK 生命周期与 ABANDON 交互

- **缺失的决策是什么**：SACKed payload 是保留字节还是只保留序列标记；ABANDON 覆盖已 SACK 包时 ACK/SACK 如何生成、何时释放。
- **为什么实现时必须决定**：D-12b 的 no-reneging 和 §3.7 的 flush 对同一存储对象给出不同压力。
- **不同工程师可能产生什么不同实现**：保留完整 payload、立即释放只留 bitset、ABANDON 后清 SACK、保留 SACK 到累计 ACK。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-23：NACK 输入处理

- **缺失的决策是什么**：NACK 中重复 seq、已消耗 seq、SACKed seq、ABANDONED seq、窗口外 seq 的逐项裁决、排序、合并和速率限制。
- **为什么实现时必须决定**：NACK 是外部可控的快路径，必须知道它是否改变 retry/timer 状态。
- **不同工程师可能产生什么不同实现**：整包拒绝、逐项过滤、重复项各触发一次、只处理第一个或关闭 session。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-24：ABANDON 区间规范化

- **缺失的决策是什么**：区间排序、相邻/重叠合并、跨回绕表示、单包超 64 区间时分片、重复区间、旧区间和未来区间的处理。
- **为什么实现时必须决定**：区间是填补 recv_next 洞和释放 sender 窗口的核心协议对象。
- **不同工程师可能产生什么不同实现**：先合并后发送、按输入顺序发送、超过上限丢弃、拆成多个控制包或关闭 session。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-25：Deadline 起点、量化和零值语义

- **缺失的决策是什么**：D 从 capture、frame creation、入队、首次发送还是入窗开始；毫秒取整方向；remaining=0 是否可发送；超过 D 如何编码。
- **为什么实现时必须决定**：§3.8 的 frame creation 与 §3.9 的 `first_send_time` 不足以覆盖排队和未发送对象。
- **不同工程师可能产生什么不同实现**：按创建时刻扣除、按首次发送重置、向上/向下取整、0 视为已过期或仍可发送。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-26：Playback timeline 与 RTP clock

- **缺失的决策是什么**：RTP clock rate、PT、SSRC、timestamp wrap、帧率变化、anchor 重建、暂停/恢复和负 timestamp delta 的行为。
- **为什么实现时必须决定**：固定 `/90000` 只是一个公式，不是完整的 RTP 时间契约。
- **不同工程师可能产生什么不同实现**：只支持固定 90kHz、从配置读 clock rate、遇到 wrap 重锚或按 serial arithmetic 继续。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-27：Receiver EXPIRED 的协议效果

- **缺失的决策是什么**：本地 playout 过期是否能推进 `recv_next`、是否要生成任何上报、与后到 DATA/ABANDON 的优先级是什么。
- **为什么实现时必须决定**：FrameState 有 EXPIRED，但 `advance_recv_next` 只认识收到和 abandoned intervals。
- **不同工程师可能产生什么不同实现**：EXPIRED 只影响播放、自动跳洞、等待 sender ABANDON 或关闭 frame/session。
- **差异影响**：correctness 高；interoperability 高；performance 高；testability 高。

### MD-28：Timeout 类型之间的优先级

- **缺失的决策是什么**：frame deadline、RTO、NACK 抑制、ABANDON retry、PING、ACK timeout、session timeout、watchdog 同时到期时的顺序和互斥关系。
- **为什么实现时必须决定**：单 timerfd 到期堆会把多个事件送到同一 Reactor，但文档没有事件排序。
- **不同工程师可能产生什么不同实现**：先重传后放弃、先放弃后处理 ACK、先 teardown 再发 PING，导致不同终态。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-29：Timer entry 的身份、取消和重算

- **缺失的决策是什么**：timer heap 存对象指针、句柄还是 `(session_id,generation)`；修改 RTO、删除 session、关停后旧 entry 如何失效。
- **为什么实现时必须决定**：teardown 和退避会频繁取消/重排事件，裸指针会引入 UAF，保留旧事件会重复执行。
- **不同工程师可能产生什么不同实现**：惰性删除加 generation、显式 heap erase、每次重建所有 timer 或忽略 stale event。
- **差异影响**：correctness 高；interoperability 低；performance 中；testability 高。

### MD-30：Retry 的最终失败

- **缺失的决策是什么**：DATA、ABANDON、PING/PONG、控制发送各自的最大重试次数、达到 RTO_MAX 后的动作、是否拆 session、metrics 终态。
- **为什么实现时必须决定**：当前只规定退避和部分重发，没有“最终失败”状态；没有最终状态就无法证明等待有界。
- **不同工程师可能产生什么不同实现**：无限 retry、按 deadline 停止、按次数拆 session、只停止数据但保持控制面。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-31：Socket 发送错误语义

- **缺失的决策是什么**：EAGAIN、EINTR、EMSGSIZE、ENETUNREACH、ECONNREFUSED、EPOLLERR 分别作用于 datagram、frame、session 还是 process；是否重试和如何计数。
- **为什么实现时必须决定**：§6 只列错误名，未定义每个错误后的序列终态和控制路径。
- **不同工程师可能产生什么不同实现**：重试 send queue、丢新帧、ABANDON、拆 session、退出进程或静默忽略。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-32：Malformed input 的解析提交边界

- **缺失的决策是什么**：长度、版本、类型、flags、RTP、descriptor、窗口校验的顺序；校验失败前是否允许更新 ACK/FrameState/metrics。
- **为什么实现时必须决定**：部分解析后更新状态会使坏包留下半提交对象。
- **不同工程师可能产生什么不同实现**：先解析头再更新、边读边更新、先分配 payload 后校验、坏包触发错误响应或静默丢弃。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-33：Oversized datagram 与 `payload_len`

- **缺失的决策是什么**：`payload_len` 是否包括 RTP 头，DATA/控制的最大 datagram，接收 `MSG_TRUNC`、发送超过 MTU、临时 buffer 分配失败时的动作。
- **为什么实现时必须决定**：当前 1200B/1268B 是核算目标，不是完整 wire-level max-length contract。
- **不同工程师可能产生什么不同实现**：拒绝、截断、分片、EMSGSIZE 后回滚 seq 或把整帧标为 dropped。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-34：RTP header 的完整取值

- **缺失的决策是什么**：RTP version、P/X/CC、marker、PT、SSRC、RTP sequence、padding、extension、网络字节序和校验失败动作。
- **为什么实现时必须决定**：文档只说 RTP 头为 12B 和 timestamp 等值校验，不能生成唯一可互操作的 RTP。
- **不同工程师可能产生什么不同实现**：固定字段为零、随机 SSRC、每 session 一个 SSRC、每帧/每包不同 PT 或保留扩展位。
- **差异影响**：correctness 中；interoperability 高；performance 低；testability 高。

### MD-35：版本和扩展兼容性

- **缺失的决策是什么**：未知 flags/type/header_len/version 的 forward/backward compatibility、错误响应是否允许扩展字段。
- **为什么实现时必须决定**：当前要求保留位非零即拒绝，但没有定义新版本或扩展客户端如何共存。
- **不同工程师可能产生什么不同实现**：严格丢弃、忽略未知位、按 header_len 跳过扩展、会话级拒绝。
- **差异影响**：correctness 中；interoperability 高；performance 低；testability 中。

### MD-36：Payload 所有权与 fan-out

- **缺失的决策是什么**：一帧发给多个 session 时是复制、引用计数、共享 immutable buffer 还是每 session 独立 packet；何时允许释放源帧。
- **为什么实现时必须决定**：D-31 说不共享 payload，但没有规定 Capture ring、Packetizer 临时 buffer 和 session payload 的转移时刻。
- **不同工程师可能产生什么不同实现**：每客户端复制、一次编码多次引用、sender 持有到 ACK 或到 ABANDON，内存峰值不同。
- **差异影响**：correctness 高；interoperability 低；performance 高；testability 中。

### MD-37：FrameState 与 payload 的析构责任

- **缺失的决策是什么**：FrameState 是否拥有所有 packet bytes；ABANDON/EXPIRED/COMPLETE/teardown 时由哪一层释放，是否允许零拷贝交付。
- **为什么实现时必须决定**：sender 释放 payload、receiver flush、Jitter 交付和 Metrics 统计会同时引用同一帧。
- **不同工程师可能产生什么不同实现**：状态对象拥有全部数据、packet 独立拥有、交付后异步释放、统一 arena 回收。
- **差异影响**：correctness 高；interoperability 低；performance 高；testability 高。

### MD-38：Queue 的完整集合与容量

- **缺失的决策是什么**：除了 SPSC，还需要哪些 outgoing、control、NACK、ABANDON、snapshot、command、Jitter、error/log queue；每个容量是多少。
- **为什么实现时必须决定**：设计只列了部分结构，无法证明“所有上限触发的丢弃”覆盖了所有内存增长点。
- **不同工程师可能产生什么不同实现**：用 `std::queue` 无界、固定数组、共享总预算或直接同步调用。
- **差异影响**：correctness 中；interoperability 低；performance 高；testability 高。

### MD-39：Queue 满、空和覆盖语义

- **缺失的决策是什么**：每类队列满时丢旧/丢新/阻塞/拒绝/覆盖，空时是否轮询；队列项已计入哪一类 metrics，取消时如何释放。
- **为什么实现时必须决定**：只有 SPSC 和 reorder 有局部策略，其他队列没有；窗口“在 SPSC 前等待”也没有超时。
- **不同工程师可能产生什么不同实现**：Reactor 被阻塞、丢新帧、丢重传、扩容或返回 RESOURCE_EXHAUSTED。
- **差异影响**：correctness 高；interoperability 低；performance 高；testability 高。

### MD-40：SPSC 覆盖操作的同步契约

- **缺失的决策是什么**：生产者丢最旧是否能修改 consumer head；使用何种原子变量、内存序、slot generation 和对象析构顺序。
- **为什么实现时必须决定**：传统 SPSC 由生产者/消费者分别拥有一个指针，满时丢最旧需要跨越这一所有权边界。
- **不同工程师可能产生什么不同实现**：用 mutex、双 CAS、生产者覆盖、消费者协作丢弃或改为 MPSC/MPMC。
- **差异影响**：correctness 高；interoperability 低；performance 高；testability 高。

### MD-41：Reactor 唤醒与公平调度

- **缺失的决策是什么**：Capture 新帧如何唤醒 Reactor，eventfd 是否合并事件，UDP drain、timer、send budget、metrics 和 command 的每轮处理上限与顺序。
- **为什么实现时必须决定**：epoll 不会因为普通 SPSC 写入自动唤醒；无调度契约会让某 session 饿死其他 session。
- **不同工程师可能产生什么不同实现**：timerfd 周期轮询、eventfd、短超时 epoll；先 drain UDP、先发重传或轮询 session。
- **差异影响**：correctness 中；interoperability 低；performance 高；testability 高。

### MD-42：Per-session 发送预算

- **缺失的决策是什么**：令牌生成速率、突发上限、DATA/重传/ABANDON/PING 优先级、预算耗尽后的排队和跨 session 调度。
- **为什么实现时必须决定**：D-29 把预算视为隔离的唯一机制，但没有可执行参数。
- **不同工程师可能产生什么不同实现**：字节令牌、包令牌、严格轮询、加权轮询或完全不限制重传。
- **差异影响**：correctness 中；interoperability 低；performance 高；testability 高。

### MD-43：timerfd 与虚拟 Clock 的分层

- **缺失的决策是什么**：虚拟测试是否绕过 timerfd、真实 timerfd 是否只作 adapter、时间跳跃如何驱动 heap，以及 timerfd 读数与 `IClock::now()` 不一致时谁权威。
- **为什么实现时必须决定**：内核 timerfd 不能直接由进程内虚拟时间推进；同一实现若没有边界会出现测试绿、生产不触发。
- **不同工程师可能产生什么不同实现**：测试直接调用 `on_time`、伪造 timerfd、使用真实睡眠或把 timerfd 事件仅作唤醒。
- **差异影响**：correctness 高；interoperability 低；performance 中；testability 高。

### MD-44：Runtime command queue

- **缺失的决策是什么**：故障注入/调参命令的格式、生产者线程、容量、命令应用时刻、无效命令、并发顺序和是否可取消。
- **为什么实现时必须决定**：文档要求经 eventfd 命令队列进入 Reactor，却没有 queue/API 契约。
- **不同工程师可能产生什么不同实现**：命令直接写共享配置、复制到无界队列、只保留最后一个调参或按到达顺序全部执行。
- **差异影响**：correctness 中；interoperability 低；performance 中；testability 高。

### MD-45：Worker 失败传播

- **缺失的决策是什么**：Capture、Metrics 或 Reactor 线程异常/退出/阻塞时，谁检测、如何通知、是否重启、是否全局 shutdown、线程如何 join。
- **为什么实现时必须决定**：三线程拓扑没有监督者或健康状态契约；单写者原则不能解释 worker 失败。
- **不同工程师可能产生什么不同实现**：捕获异常后全局退出、静默停止该 worker、主线程 watchdog 重启或永久等待 join。
- **差异影响**：correctness 高；interoperability 低；performance 中；testability 高。

### MD-46：Metrics snapshot API 与反压

- **缺失的决策是什么**：snapshot 是 immutable copy、原子结构还是队列项；生成频率、容量、丢弃策略、读取顺序和 final snapshot 确认方式。
- **为什么实现时必须决定**：Metrics Worker 只读快照，但没有定义快照是否拥有底层字符串/容器，也没有规定慢消费者。
- **不同工程师可能产生什么不同实现**：每次深拷贝、环形覆盖、阻塞 Reactor 或只保留最新快照。
- **差异影响**：correctness 中；interoperability 低；performance 高；testability 高。

### MD-47：Startup transaction

- **缺失的决策是什么**：socket、epoll、timerfd、eventfd、signalfd、queues、camera、threads 的初始化顺序、ready barrier、失败回滚和降级模式。
- **为什么实现时必须决定**：部分初始化时 session map、timer heap、线程和 fd 可能只完成一部分。
- **不同工程师可能产生什么不同实现**：先启动线程后建 fd、先建全部资源再 ready、Camera 失败降级 Synthetic 或整个进程失败。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-48：Graceful shutdown 状态机

- **缺失的决策是什么**：收到 stop 后是否停止接收新控制报文、是否停止新帧、是否继续重传/ABANDON、SPSC drain deadline、client 如何获知结束。
- **为什么实现时必须决定**：D-16 只有顺序，没有事件门禁和完成条件；“排空”可能无限等待。
- **不同工程师可能产生什么不同实现**：立即关闭 socket、继续 drain 到超时、发送 FIN/ABANDON、只靠 client timeout。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-49：Cancellation 与 teardown 的线性化点

- **缺失的决策是什么**：ACK、NACK、RTO、deadline、ABANDON、session timeout、shutdown 同时作用于对象时，哪一个事件先提交，哪些后续事件必须被忽略。
- **为什么实现时必须决定**：统一决策入口没有规定调用顺序，释放 payload 后迟到 timer/ACK 仍可能引用对象。
- **不同工程师可能产生什么不同实现**：事件按 epoll 顺序、按 timer 优先、按 deadline 优先，或每个对象加 generation。
- **差异影响**：correctness 高；interoperability 中；performance 中；testability 高。

### MD-50：异常终止与重新同步

- **缺失的决策是什么**：进程 crash、SIGKILL、断电、worker abort 后 client 如何发现、多久重 Join、旧控制/数据如何处理、metrics 如何归类。
- **为什么实现时必须决定**：随机 ISN 不能替代重启后的协议状态恢复和确定性旧报文隔离。
- **不同工程师可能产生什么不同实现**：client 无限重试旧 session、主动重新 Join、等待 PING timeout、网关接受旧 ACK 或全部丢弃。
- **差异影响**：correctness 高；interoperability 高；performance 中；testability 高。

### MD-51：错误 API 与错误归属

- **缺失的决策是什么**：协议错误、资源错误、系统 errno 是返回值、异常、事件、metrics 还是 session teardown；错误是否带 seq/frame/session context。
- **为什么实现时必须决定**：错误名称列表不能告诉上层调用者是否继续、重试、丢帧或关闭会话。
- **不同工程师可能产生什么不同实现**：`enum` 返回、C++ exception、日志后忽略、转换为 ABANDON 或转换为 process fatal。
- **差异影响**：correctness 中；interoperability 中；performance 低；testability 高。

### MD-52：资源耗尽分级与预留

- **缺失的决策是什么**：session、payload、FrameState、reorder、control、timer、metrics、fd、线程资源分别耗尽时的优先级、保留量和拒绝动作。
- **为什么实现时必须决定**：只有 `max_sessions` 有明确 reject，其他 RESOURCE_EXHAUSTED 情况没有保护控制面的最低资源。
- **不同工程师可能产生什么不同实现**：先拒绝新 session、丢媒体保留 ABANDON、关闭最老 session、直接退出或继续分配到 OOM。
- **差异影响**：correctness 高；interoperability 中；performance 高；testability 高。

### MD-53：Socket buffer 与地址族契约

- **缺失的决策是什么**：仅支持 IPv4 还是 IPv6、绑定地址/端口、源端口变化、实际 SO buffer 校验失败时的动作，以及 kernel drop 如何记录。
- **为什么实现时必须决定**：设计按 IPv4 MTU 核算，并将 buffer 设置当作实验前提，但没有启动校验和 endpoint 契约。
- **不同工程师可能产生什么不同实现**：只 bind IPv4、dual-stack、按配置绑定；buffer 失败时继续、降级或退出。
- **差异影响**：correctness 中；interoperability 高；performance 高；testability 中。

### MD-54：控制面可信边界与限速

- **缺失的决策是什么**：是否假定封闭可信网络；未认证 PING/NACK/ABANDON 的来源验证、速率限制、session admission 和日志限额。
- **为什么实现时必须决定**：不做 TLS 不等于不需要定义哪些外部报文有权改变 session 状态。
- **不同工程师可能产生什么不同实现**：完全开放、按 endpoint 白名单、按 token 验证、只限制 NACK 或限制所有控制报文。
- **差异影响**：correctness 中；interoperability 中；performance 高；testability 高。

### MD-55：FDR 统计主键与 fan-out 口径

- **缺失的决策是什么**：FDR 按源 frame、按 session-frame 副本还是按交付事件统计；`offered_frames` 在 fan-out、重连、丢弃和异常终止下如何计数。
- **为什么实现时必须决定**：同一源帧可能发给多个 client，当前方程不能从文字唯一推导分子/分母。
- **不同工程师可能产生什么不同实现**：全局源帧计一次、每 session 计一次、只统计成功建立 session，得到不同于 100% 的结果。
- **差异影响**：correctness 中；interoperability 低；performance 低；testability 高。

### MD-56：Frame 终态与指标优先级

- **缺失的决策是什么**：ontime、late、abandoned、expired、capture_dropped、pre_admission_dropped、in_flight、protocol_dropped 之间是否互斥，多个事件命中时谁胜出。
- **为什么实现时必须决定**：sender ABANDON、receiver EXPIRED、shutdown 和 timeout 可能对同一 frame 产生多个标签。
- **不同工程师可能产生什么不同实现**：首次事件 wins、最终事件 wins、sender/receiver 各记一份或允许多标签。
- **差异影响**：correctness 中；interoperability 低；performance 低；testability 高。

### MD-57：测试调度和随机确定性

- **缺失的决策是什么**：虚拟网络事件顺序、同一时间到期事件的 tie-break、loss/delay/reorder/duplicate 的随机分布、seed 记录和重放格式。
- **为什么实现时必须决定**：注入 Clock 不足以保证同一 timestamp 的 packet、timer 和 command 顺序一致。
- **不同工程师可能产生什么不同实现**：按插入顺序、按 seq、按 session、按随机 tie-break；同一 seed 也可能产生不同事件序列。
- **差异影响**：correctness 中；interoperability 低；performance 中；testability 高。

### MD-58：测试 oracle 的观察点

- **缺失的决策是什么**：ACK 发送算接收成功还是只有应用交付算成功；何时采样 FDR/P99；丢弃、duplicate、late 和 in_flight 的观测来自哪一层。
- **为什么实现时必须决定**：协议状态、播放状态和指标状态不是同一个时间点。
- **不同工程师可能产生什么不同实现**：sender 日志作为真相、receiver ACK 作为真相、Jitter callback 作为真相，测试会得到不同结果。
- **差异影响**：correctness 中；interoperability 低；performance 高；testability 高。

### MD-59：Benchmark 输入和比较协议

- **缺失的决策是什么**：synthetic frame size/count 分布、TCP 应用 framing、代理位置、buffer 配置、CPU pinning、client consumption、warm-up、重复次数和置信区间。
- **为什么实现时必须决定**：UDP/TCP/RUDP-RT 的结论高度依赖 packet count、代理和消费模型；当前验收条件不足以复现。
- **不同工程师可能产生什么不同实现**：固定 1 包帧、固定 20 包帧、不同 TCP socket buffer 或不同 warm-up，导致 FDR/P99 不可比。
- **差异影响**：correctness 低；interoperability 低；performance 高；testability 高。

### MD-60：实现范围与完成标准

- **缺失的决策是什么**：三周里程碑中哪些是硬性交付、哪些可延期；camera、stretch benchmark、SACK/NACK 消融和真实 socket 集成的失败是否阻断发布。
- **为什么实现时必须决定**：当前文档同时要求协议闭环、实验、报告和 Camera demo，但没有定义缺一项时的发布判定。
- **不同工程师可能产生什么不同实现**：一人先完成协议库，一人优先 camera demo，另一人把未冻结机制用 stub 通过里程碑。
- **差异影响**：correctness 中；interoperability 低；performance 中；testability 高。

## 已明确且可保留的部分

以下内容虽未覆盖全部实现细节，但三名工程师通常能得到相近方向；它们不应被误报为本轮 Missing Decision：

1. 高层线程拓扑是 Capture Worker、Reactor、Metrics Worker，协议状态由 Reactor 单写者维护。
2. 运行时使用非阻塞 UDP、epoll LT、单 timerfd 和到期堆的总体结构。
3. RTP 与 RUDP-RT 分层；RUDP seq 与 RTP seq 分离。
4. 帧级原子入窗、帧级 ABANDON、no-reneging 和 reorder 满时丢新包的总体意图。
5. Synthetic Frame 先于 Camera、进程内虚拟网络先于真实 socket 的构建顺序。
6. 显式列出 FDR、watchdog、故障注入和不变量，说明设计希望通过可观测性质验证，而不是只做 happy-path demo。
7. 暂不加入 FEC、TLS、NAT、拥塞控制和自有编解码的范围裁剪方向；但其运行前提和限制仍需在接口/测试层声明。

## Design Completeness Score

评分范围为 0-10，表示“仅凭当前文档，独立工程师可以得到唯一或近似唯一实现”的程度，不表示设计思想的好坏。

| 维度 | 分数 | 判断依据 |
|---|---:|---|
| Requirements | **3/10** | 动机、目标岗位和部分验收目标明确，但交付边界、客户端、媒体输入、运行条件和失败后的产品行为未冻结 |
| Architecture | **5/10** | 高层三线程/单 Reactor/epoll 结构清晰，但模块责任、进程拓扑、调度和 source/client 边界不唯一 |
| Interfaces | **1/10** | 没有可直接实现的 C/C++ 类型、函数、回调、错误、网络和媒体 API 契约 |
| Concurrency | **3/10** | 单写者原则明确，但 SPSC 覆盖、唤醒、队列、worker failure、command 和 shutdown barrier 未定义 |
| State Machine | **3/10** | sender/receiver/frame/session 有伪代码和状态名，但初始化、冲突、超时、取消、终态优先级不完整 |
| Error Handling | **2/10** | 错误分类名称较全，触发后的动作、归属、返回语义和回滚不明确 |
| Resource Management | **3/10** | W、reorder、SPSC 和部分 payload 上限有数值，但总队列、Jitter、timer、snapshot、OOM 和析构不闭合 |
| Testing | **5/10** | 测试层次、注入 Clock、故障维度和部分不变量很有方向，但输入模型、调度重放、oracle 和 FDR 口径不唯一 |

**综合确定性判断：约 3/10。** 该分数不是将各项简单平均后的质量分，而是考虑到 Interfaces、State Machine、Error Handling 中存在低于可冻结门槛的维度；这些维度会让后续模块无法稳定对接。

## Design Freeze 判断

**当前未达到 Design Freeze 条件。**

Design Freeze 至少需要满足以下可观察条件：

1. 三名工程师可以从文档写出相同的模块清单、公开类型和调用/线程契约，不需要补写隐含 API。
2. 对每种输入事件，session、sender、receiver、frame、timer 和 shutdown 状态都有唯一的合法转换和优先级。
3. 每个 payload、queue item、timer entry、FrameState、snapshot 和 session 的 owner、转移点、释放点都可以画成唯一 ownership graph。
4. 所有 timeout/retry/error 事件都有有界的最终动作，且不会依赖未定义的“实现自行重试”行为。
5. wire format、初始化序列、控制报文合法性和 endpoint identity 足以让两个独立实现互通。
6. 所有队列和 buffer 都有容量、满/空策略、背压、取消、统计归属和内存预算。
7. FDR、frame 终态、P99 和 watchdog 的测试 oracle 在 fan-out、重连、shutdown、资源耗尽和异常终止下仍然唯一。
8. 启动失败、partial initialization、worker failure、graceful shutdown 和 abnormal termination 都有可验证的终态。

在这些条件满足之前，继续编码会把设计缺口固化为某一位工程师的隐含选择；之后三方实现之间的差异将无法仅凭文档判断谁正确。
