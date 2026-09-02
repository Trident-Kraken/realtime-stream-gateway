# RUDP-RT 实时流传输网关 — 设计大纲 v0.2（审计修订版）

**状态**：设计冻结候选（待最终评审）
**本文档 = 原始设计 v0.1 + 六项 P0 修复（P0-1..P0-6）+ 审计采纳的 P1/P2 决策**
**用途**：编码指导基线 + 后续 AI/人工交叉审计基线
**与 v0.1 的关系**：v0.1 的战略层（定位、分层、范围裁剪、测试哲学）全部保留；本版冻结了 v0.1 缺失的协议对账语言（ACK 语义、ABANDON、帧描述符、RTT 采样、会话生命周期），并修正了会导致死锁/歧义的条款。原始背景、根本要求与约束以原貌保留于 §1，审计修订不回溯改写该节。

---

## 0. 阅读指南

- 想了解项目从哪来、为什么长成这个形态：先看 §1（原始事实，v0.1 原貌保留）。
- 想知道"为什么这样设计"：看 §9 决策日志（每条含取舍与理由）。
- 想直接写代码：按 §11 构建顺序，对照 §3 的规格与 §7 的测试条目。
- 想做交叉审计：先读 §1 建立原始动机基线，再按 §12 检查单逐项验证，重点攻击 §3.14 的不变量。

---

## 1. 项目背景、根本要求与约束（原始事实）

> 本节是原始设计文档（v0.1）中事实性内容的完整保留与编辑性整理，代表"最初的事实"。
> 后续审计修订只作用于协议与架构条款（§3 起），不回溯修改本节——审计者应能对照本节的原始动机，检查各项修订是否偏离初衷。

### 1.1 项目来源

这是在第一个"工业物联网通信网关"项目完成之后设计的第二个项目。第一个项目已经实现 TCP/UDP + 消息队列 + 网关转发一类的架构；第二个项目如果只是换一个协议名称继续做类似的东西，两个项目之间的技术跨度不够。因此第二个项目必须体现以下能力：

> Linux 网络编程、UDP、RTP、实时媒体传输、自定义协议设计、可靠传输、Sliding Window、ACK/SACK/NACK、Retransmission、RTT/RTO、Deadline、Reorder、Jitter Buffer、epoll、timerfd、多客户端 Gateway、故障注入、性能测试。

项目形态据此最终确定为：**基于 UDP + RTP 的实时流传输网关，并在 RTP/UDP 之上设计一个面向实时媒体的自定义可靠传输协议 RUDP-RT。**

### 1.2 目标读者与用途要求

- **定位**：校招项目。目标岗位：嵌入式软件工程师 / 协议栈 / 网络通信 / 视频物联网方向。目标企业方向：海康威视等视频监控、视频物联、智能物联网相关企业。这决定了选题向实时视频/媒体流收敛，使项目天然处于 Camera、RTP、UDP、实时性、丢包、延迟、抖动、多客户端、Gateway、Edge/IoT 的业务语境中。
- **用途要求**：项目不是"实现一个能跑的 UDP 程序"，它必须能回答以下问题——为什么这样设计？为什么不用 TCP？为什么不用 QUIC？为什么要自定义可靠层？ACK/SACK/NACK 各自解决什么问题？为什么实时数据不能无脑重传？RTO 怎么计算？如何处理乱序？如何处理 deadline？如何验证协议？在什么网络条件下它优于普通 UDP/TCP？
- 由此得出一条与功能同等重要的要求：**设计可解释性与实验可验证性**。

### 1.3 硬约束

| 约束 | 内容 | 对设计的直接后果 |
|---|---|---|
| 时间 | 开发周期约 3 周 | 不能设计成需要数月完成的工业级完整视频平台；范围裁剪是设计的一部分（D-13） |
| 硬件 | 拥有一台海康威视 USB 摄像头，可连接电脑使用；但摄像头不能成为协议开发的前置依赖 | 采用双轨：Synthetic Frame 用于协议开发与自动化测试，Camera 只用于最终 Demo/Integration（D-02）。协议与媒体传输系统必须脱离真实摄像头独立运行 |
| 环境 | Linux/Ubuntu、C++20、Linux Socket API、epoll、timerfd、CMake | 不依赖实际嵌入式硬件；不引入 Boost.Asio 等网络框架（D-14） |
| 用途 | 见 1.2 | 每个机制必须回答"解决什么问题"；每个性能结论必须有实验支撑（Hypothesis → Experiment → Metrics → Conclusion） |

### 1.4 设计演进的四个关键转折（原始动机）

1. **从"可靠 UDP"转向"Deadline-aware 可靠传输"**。如果方案只是 UDP + ACK + 重传，很容易退化成"自己写了一个简化 TCP"，技术含量和项目故事都不足。整个项目最重要的设计转折是：可靠性不再是绝对目标，而是**受时间价值约束的可靠性**——丢包后数据仍有时效价值则重传，已过期则主动丢弃。
2. **摄像头有价值，但不是协议设计的核心依赖**。若从 Camera/V4L2/视频格式/编码起步，任何驱动、格式、编码、V4L2 问题都会阻塞协议开发。因此协议栈必须先以 Synthetic Frame 独立完成，Camera 最后接入——Camera 是真实 Demo 的加分项，而不是协议工程的单点故障。
3. **RTP 与 RUDP-RT 分层**。RTP 已提供 Sequence Number、Timestamp、Marker、Payload Type、SSRC，但它本身不是完整可靠传输协议；把 ACK/SACK/RTO/重传/流控全部塞进 RTP 会让媒体协议与传输协议耦合。最终分层为"RTP = 媒体语义，RUDP-RT = 传输语义"，使未来替换 H.264/H.265/JPEG 无需修改 RUDP 核心。
4. **Deadline 采用 Relative Deadline（剩余寿命）表达**。发送端与接收端的单调时钟没有天然的绝对同步关系，因此不传 sender 的绝对 monotonic 时间，改传剩余生命周期；数据经过的时间从寿命中扣除，重传时只能沿用剩余量、不能重置（两个序列号与 Frame ID 的引入动机见决策日志 D-04 与 §3.11）。

### 1.5 五条设计原则

1. **Real-time First**：目标不是"每一个 packet 必须到达"，而是"有价值的数据必须尽可能及时到达"。
2. **Deadline-aware Reliability**：可靠性不是绝对目标，受 deadline 约束，最终衡量标准是有用交付（Useful Delivery）。
3. **Layer Separation**：Application → RTP → RUDP-RT → UDP，媒体语义与传输语义解耦。
4. **Measurement Driven**：所有性能结论必须经过 Hypothesis → Experiment → Metrics → Conclusion，不允许"理论上应该更快"式的断言。
5. **Controlled Complexity**：任何机制都必须回答"它解决什么问题"，回答不了就不实现。主动放弃清单：FEC、复杂握手、拥塞控制、TLS、NAT 穿越、自有编解码、DPDK、GPU。

### 1.6 核心价值主张与技术主线

这个项目最终呈现的不是"我写了一个 UDP 视频传输程序"，而是：

> **针对实时媒体传输中"可靠性与时效性冲突"的问题，在 UDP/RTP 之上设计并实现了一个 Deadline-aware Reliable Transport，并通过 ACK/SACK/NACK、Sliding Window、RTO、Reorder Buffer、Jitter Buffer 和故障注入实验验证其行为。**

技术主线：UDP → Reliable Transport → Real-time Constraint → Deadline → Media Semantics → Gateway → Fault Injection → Measurement。这条链是本项目在简历和面试中呈现的核心价值。

---

## 2. 总体架构

```text
USB Camera / Synthetic
        │
  Capture Worker（线程 2）
        │  SPSC Ring Buffer（满时丢最旧，计入指标）
        ▼
   Gateway（线程 1 = Reactor，单写者）
   ├── epoll(LT) + non-blocking UDP + 单 timerfd（下次到期时间堆）
   ├── Packetizer → RTP → RUDP-RT → UDP
   ├── Session Manager（per-session 全部协议状态）
   └── Watchdog / Metrics 快照队列 → Metrics Worker（线程 3，只读快照）
        │            │            │
     Client A      Client B     Client C
```

- 单写者原则（D-15）：所有协议状态仅 Reactor 线程修改；Metrics 通过原子快照或快照队列读取；运行时控制命令（故障注入调参等）经 eventfd 命令队列进入 Reactor，不直接写。
- 关停顺序（D-16）：停止采集 → 排空 SPSC → 发送侧停止新帧 → 输出最终 metrics → 关 socket → join 线程。信号经 signalfd/self-pipe 进入 Reactor。

---

## 3. 协议规格（冻结范围）

### 3.1 术语与约定

- 所有整数**网络字节序（大端）**。
- 序列/帧号比较一律使用**串行算术**（RFC 1982 风格）：`lt(a,b) := (int32)(a-b) < 0`；窗口归属与 SACK 锚点同样用模运算，禁止裸减法。
- 会话 = 一对 (client_addr, stream_id) 上的双向控制、单向媒体流（gateway→client 数据，client→gateway 控制报文）。

### 3.2 报文头部位段表（28 字节定长）

```text
偏移 长度 字段            说明
0    1    version         固定 1；不符 → INVALID_VERSION 丢弃
1    1    type            DATA/ACK/NACK/PING/PONG/ABANDON
2    2    flags           bit0 RETRANSMIT（重传副本，用于统计与去重）
                          bit1 FRAME_END（帧尾包，与 frame_index 交叉校验）
                          bit2 SYNC_START（会话首个数据包标记，见 3.13）
                          其余必须为 0
4    1    header_len      固定 7（28>>2）
5    1    reserved        0
6    2    stream_id
8    4    seq             RUDP 序列，会话内单调，随机初始（ISN，见 D-19）
12   4    frame_id        帧号，会话内单调，32 位，串行算术
16   4    rtp_timestamp   媒体层权威副本（RTP 头内同名字段必须相等，不等 → PROTOCOL_VIOLATION）
20   2    deadline_ms     剩余生命周期（毫秒），随重传递减
22   2    frame_index     本包在帧内序号（0 基），仅 DATA 有效
24   2    frame_count     帧总包数，仅 DATA 有效
26   2    payload_len
28   ..   payload
```

开销核算：IP 20 + UDP 8 + RUDP 28 + RTP 12 = 68 字节；payload 1200 → 全包 1268 < 1500，无 IP 分片（D-06）。

### 3.3 报文类型与载荷

| 类型 | 方向 | 载荷 |
|---|---|---|
| DATA | G→C | RTP 头(12B) + 媒体分片 |
| ACK | C→G | `u32 ack_number` + `u64 sack_bitmap`（12B） |
| NACK | C→G | `u16 count` + `count × u32 seq`（单包 ≤ 64 个） |
| ABANDON | G→C | `u16 range_count` + `range_count × {u32 start_seq, u32 end_seq}`（闭区间，单包 ≤ 64 个区间） |
| PING | 双向 | `u32 nonce` |
| PONG | 双向 | `u32 nonce` + `u8 status`（0=OK, 1=REJECT_MAX_SESSIONS, 2=REJECT_UNKNOWN_STREAM） |

控制报文不占用序列空间，不受滑动窗口约束；ABANDON 重发受 RTO 节奏限制（防风暴）。

### 3.4 序列空间与算术规则

- RUDP seq 与 RTP seq 相互独立（D-08）。seq 在**进入滑动窗口（发送）时**才分配，不在打包时分配；帧级入队是原子的（一帧的全部包同时获得连续 seq、同时入窗），因此 `frame_count ≤ W`（D-18）。
- 序列一旦分配即必须了结于两种终态之一：**已消耗（收到）** 或 **已放弃→已消耗**。不存在第三态。

### 3.5 ACK/SACK 语义（冻结，修正 v0.1 §13/§22 的歧义）

- **ACK = 累积消耗位置**：最大序列号 S，使所有 ≤ S 的序列均已被接收或已被 ABANDON 覆盖。约定为"最后已消耗"（recv_next = 100 时 ACK = 99）。
- **SACK**：bit i（0..63）= 序列 ACK+1+i 已收到。bitmap 只报告**当前仍持有**的包。
- **no-reneging 不变量（P0-2 修复）**：接收端一旦报告（ACK/SACK）即永不反悔。执行手段是准入控制——reorder buffer 满时丢弃**新到的**未报告包（它从未被报告，不构成反悔），绝不淘汰已报告的包。bitmap 因此永远真实。
- 接收端对每个 DATA 回 ACK（不做 delayed ACK，D-22）。ACK 丢失无害（RTO 兜底）。
- v0.1 §22 示例勘误：收到 102 时置位的是 bit2（100=bit0, 101=bit1, 102=bit2），原文写 bit1。

### 3.6 NACK 语义

- 触发：收到乱序包产生空洞时，对该空洞发 NACK；**同一空洞的 NACK 间隔 ≥ SRTT**（每洞节流，状态 ≤ W 项）。
- **首个 NACK 即触发 sender 快速重传**（无 TCP 式三次重复阈值——deadline 预算太薄，风暴防护由两侧的抑制间隔承担，D-21）。
- Sender 收到 NACK(x)：若 x 已放弃 → 立即重发 ABANDON（快路径，兼作 ABANDON 丢失的快速修复）；若 x 距上次发送 < SRTT → 抑制；否则重传。

### 3.7 ABANDON 语义（P0-1/P0-2 修复，本协议的核心新机制）

**动机**：deadline 到期丢弃若不进入双方对账语言，会在累积 ACK 上制造永久空洞 → 窗口死锁 → 整流停摆。

- **放弃粒度 = 帧**（D-17）：一帧缺任何包即无价值（V0.1 无部分交付），且帧内放弃会与帧内存活包的 SACK 冲突。因此 sender 的 deadline 判定命中时，放弃**整个帧的序列区间** [first_seq, first_seq+frame_count-1]。
- Sender 侧：命中放弃 → 释放该帧全部 payload（内存即时回收），保留簿记 stub，区间进入 `pending_abandon`；发出 ABANDON；此后对 NACK 只回 ABANDON，**永不复活**。
- ABANDON 的可靠性完全复用 DATA 机制：RTO 到期重发、NACK 快路径重发、累积 ACK 越过即销账。不新增定时器类型、不新增 ACK 类型。
- Receiver 侧：应用区间（幂等）→ 将被覆盖的帧判死（ABANDONED，优先于本地 EXPIRED，不等计时器）→ flush 对应 reorder buffer 条目 → `advance_recv_next()`（越过已收或已放弃）→ 发新 ACK。**先于 recv_next 到达的 ABANDON 区间先记录、后生效。**
- 边界规则：
  - 放弃区间与已收到包重叠：状态层不撤销"已收到"（no-reneging 不受影响），交付层按帧判死丢弃。
  - ABANDON 丢失：最坏 RTO 后由重发路径恢复，liveness 保持。
  - 迟到重传副本落在 recv_next 之下：按 TOO_OLD 丢弃，不特殊处理。
  - 越界 ABANDON（超出 [recv_next-W, recv_next+W]）：按 PROTOCOL_VIOLATION 丢弃并计数。
  - 回绕：区间为模 2^32 闭区间，串行算术判定归属。
- **窗口语义保持单指针**：`send_base` 只随 ACK 前进，放弃本身不释放窗口槽位，代价是每批次放弃约 1 RTT 窗口滞留（V0.1 接受，双指针优化记为 future work）。

### 3.8 Deadline 语义（传输层与播放层分离，修正 v0.1 §10/§24/§62 的混用）

- **传输层 deadline（sender 时钟，唯一权威）**：帧创建时设定总预算 D（默认 100ms）；任一副本发送时头部携带 `deadline_ms = D - (now - frame.first_send_time)`；重传副本递减、不重置。
- **播放层时效（receiver 时钟，仅用于测量与交付）**：`playout_time(f) = local_anchor + (f.rtp_ts - rtp_anchor)/90000 + playout_delay`，锚点取会话同步后首帧首包的到达时刻与 RTP 时间戳。playout_delay 默认 60ms，V0.1 固定不自适应（D-24）。
- receiver 不用传输层 deadline 做"是否过期"的协议判定（跨机时钟语义不成立，D-23）；它只出现在 sender 的放弃决策里。同机实验会掩盖该区别，模拟器必须支持非对称时延（D-26）。

### 3.9 Sender 状态机

```text
OutstandingPacket（帧级簿记，帧 = 连续 seq 区间）：
  frame_id, seq_range, payload[], first_send_time, last_send_time,
  retransmission_count, sacked_bits, state ∈ {IN_FLIGHT, ABANDONED}

统一决策入口（NACK / RTO / 定时扫描共用）：
  if state == ABANDONED:
      if now - last_abandon_send >= RTO(entry): 重发 ABANDON（继承退避节奏）
      return                                    # 不变量：放弃后永不重传数据
  if remaining(D) <= recovery_estimate(k):       # k = retransmission_count
      state = ABANDONED; free(payload); pending_abandon += 帧区间
      emit ABANDON(合并后的 pending_abandon)
      return
  if seq 已被 SACK 覆盖: return                  # 不重传接收端声称持有的包
  if now - last_send_time < SRTT: return         # NACK 抑制窗口
  retransmit(payload, 标记 RETRANSMIT)

入队前早退：帧在打包/入队时 remaining <= SRTT + 4×RTTVAR → 直接丢弃（不分配序列号）
入队约束：next_seq - send_base < W（模运算判定）；窗口满 → 帧在 SPSC 前等待
```

### 3.10 Receiver 状态机

```text
分类（对每个 DATA）：
  DUPLICATE   seq 已在 reorder buffer / 已消耗于本窗口内 → 丢弃计数
  TOO_OLD     seq < recv_next（模运算）→ 丢弃计数
  OUT_OF_WINDOW  seq ≥ recv_next + W_re → 丢弃（依赖两端 W 配置一致，D-20）
  VALID       → 准入：buffer 有空位则收下并报告（SACK）；满则丢新包（未报告，不违 no-reneging）
advance_recv_next():
  while recv_next 已收到 或 落在 abandoned_intervals: 交付/跳过; recv_next++
  发送 ACK(recv_next-1) + SACK
帧级过滤：frame 所在帧已 ABANDONED/EXPIRED/SKIPPED → 到达即丢
```

### 3.11 Frame Assembly 与 Jitter Buffer

- 帧完备性可从**任意单个包**判定：帧区间 = [seq - frame_index, seq - frame_index + frame_count - 1]（P0-3 修复，v0.1 依赖 Marker 的方案废弃）。`FRAME_END` 标志与 `frame_index == frame_count-1` 交叉校验，不符 → PROTOCOL_VIOLATION。
- FrameState：RECEIVING → COMPLETE（集齐）| ABANDONED（命中放弃区间）| EXPIRED（本地 playout 已过仍未齐）。前两者立即结算，不等计时器。
- 已判定死帧的后续到达包直接丢弃；交付按 frame_id 序（死帧跳过，不阻塞后帧——这是对 TCP 队头阻塞的真正解除点）。
- Jitter Buffer V0.1 = playout_time 驱动的静态延迟交付，无自适应。

### 3.12 RTT/RTO 策略栈（P0-5 修复）

- **采样规则（冻结）**：仅当累积 ACK 推进覆盖了至少一个"从未重传"的条目时采样；sample = now - 该批新覆盖条目中**最老者**的 first_send_time；每个 ACK 至多 1 个样本。重传过的条目永不采样（Karn）；SACK 不产生样本（无法区分到达的是哪个副本）。首样本按 RFC 6298：SRTT=R, RTTVAR=R/2。
- **更新**：`RTTVAR = 0.75×RTTVAR + 0.25×|SRTT-R|`；`SRTT = 0.875×SRTT + 0.125×R`；`RTO = SRTT + max(G, 4×RTTVAR)`，G = 1ms。
- **退避**：同一条目连续超时则该条目有效 RTO 翻倍，上限 RTO_MAX；条目销账后回落。退避期间不产生新样本（Karn 推论）。
- **恢复估计（替代 v0.1 的 `=RTO`，修正②的数学错误）**：`recovery_estimate(k) = k×SRTT + 4×RTTVAR`。RTO 的职责是丢包判定与状态推进（"兜 liveness"），不承担交付率（deadline 通常 < RTO_MIN，RTO 路径本就来不及交付）。
- **参数默认**（全部为实验可调常量，见 §7）：initial SRTT=25ms, RTTVAR=12.5ms, RTO_MIN=50ms, RTO_MAX=1000ms, W=64, D=100ms, playout_delay=60ms。

### 3.13 会话生命周期（P0-6 修复，复用既有机制）

- **Join**：客户端向 (gateway, stream_id S) 发 PING。Gateway：S 未配置 → PONG(REJECT_UNKNOWN_STREAM)；会话表满 → PONG(REJECT_MAX_SESSIONS)；已有同名会话 → **判定为客户端重启**，销毁旧会话、以新随机 ISN 重建（PONG OK）。
- **起始对齐**：Gateway 只从**下一帧边界**开始向新会话发送；该会话第一个 DATA 携带 `SYNC_START`（在其未被 ACK 前，重传副本保留该标志）。客户端以 SYNC_START 包锚定 recv_next；锚定前到达的包按绝对序列暂存（≤ W，溢出丢新包）。若 SYNC_START 包丢失：客户端从后续包的 index/count 反推帧区间并 NACK 缺失者 → 重传路径补齐 → 锚定完成。**join 不引入任何接收端自主跳洞**。
- **保持**：任一方出向静默 > 200ms 即每 500ms 发 PING；连续 3 次无 PONG，或连续 3×RTO_MAX 无有效 ACK → SESSION_TIMEOUT，双向各自拆除并释放状态。
- **重启污染防护**：客户端重启后旧 epoch 在途包落入新窗口的概率 ≈ W/2^32（ISN 随机化，D-19），V0.1 接受；出现即按 DUPLICATE/越界规则消化。
- **拆净**：teardown 释放 reorder/abandon/簿记/定时器，输出会话级 metrics。

### 3.14 不变量与看门狗

1. **序列终态唯一且不可逆**：已分配序列 ∈ {已消耗, 已放弃→已消耗}；放弃不可撤销。
2. **no-reneging**：接收端从不撤销已报告状态；放弃只填洞。
3. **窗口有界**：未消耗序列（含 ABANDONED 簿记）< W；窗口只随 ACK 滑动。
4. **内存有界**：放弃即释放 payload；pending_abandon / abandoned_intervals / reorder buffer 均有上限（§5）。
5. **Liveness**：窗口前端任一位置在"重传预算耗尽 + 一次 RTO 的 ABANDON 通告"内必推进。落地为看门狗断言：**send_base 超过 K×RTO 未推进即告警**（原 P0-1 死锁场景在此断言下必暴露）。
6. **FDR 守恒方程**：offered_frames = ontime + late + abandoned + expired + capture_dropped + pre_admission_dropped + in_flight。任何 metrics 输出必须满足该守恒，作为集成测试断言。

### 3.15 参数默认值表

| 参数 | 默认 | 说明 |
|---|---|---|
| W（窗口） | 64 包 | ≈ 75KB 在途 |
| max_frame_packets | 64 | = W（帧级原子入队要求） |
| payload | 1200 B | 防 IP 分片 |
| D（deadline） | 100 ms | 帧级，sender 时钟 |
| playout_delay | 60 ms | 固定 |
| RTO_MIN / MAX | 50 / 1000 ms | |
| initial SRTT | 25 ms | 首样本前 |
| NACK / ABANDON 抑制 | SRTT / RTO | 每洞 / 每条目 |
| max_sessions | 16 | |
| SPSC 深度 | 256 帧 | 满时丢最旧 |
| reorder buffer | 256 包 | 丢新不逐旧 |
| SO_RCVBUF/SO_SNDBUF | 4 MB | 显式设置，防内核丢包污染实验（D-25） |

---

## 4. 运行时架构要点

- **epoll LT + 非阻塞 + 排空到 EAGAIN**（D-27）；单 UDP socket，会话按 (addr, stream_id) 分派。
- **单 timerfd**：维护下次到期堆（RTO 扫描 / ABANDON 重发 / PING / 会话超时共用），epoll 超时取堆顶；禁止每包每会话一个 timerfd（D-28）。
- **批量收发**：V0.1 用 recvmsg 一次一包；`recvmmsg/sendmmsg` 记为优化项。
- **发送调度**：per-session 发送预算（令牌/字节），防止单会话重传风暴挤占 Reactor 与 socket 缓冲——这是对"客户端间隔离"主张的唯一机制支撑，无此机制该主张只是软隔离（D-29）。
- **Clock 抽象**：`IClock::now()` 注入所有协议组件；生产用 CLOCK_MONOTONIC，测试/模拟器用虚拟时钟。没有它，§7 的 RTO/deadline 测试与确定性实验都不成立（D-30）。

---

## 5. 资源上限表

见 §3.15。补充核算口径：

- 重传内存 per session ≤ W × 1200B ≈ 75KB + 簿记；多会话各自独立（fan-out 复制是已知成本，V0.1 不做跨会话 payload 共享，D-31）。
- 帧缓冲、reorder、abandon 区间、会话表全部有上限并纳入 metrics（queue_depth / memory 口径）。
- 所有上限触发的丢弃都必须计入 FDR 守恒方程的对应项，不允许静默丢弃。

---

## 6. 错误处理

- Protocol：INVALID_VERSION / INVALID_HEADER / INVALID_LENGTH / INVALID_TYPE / INVALID_SEQUENCE / INVALID_WINDOW / PROTOCOL_VIOLATION（含 rtp_ts 不一致、FRAME_END 与 index/count 矛盾、ABANDON 越界、flags 保留位非 0）。
- Session：SESSION_TIMEOUT / RESOURCE_EXHAUSTED。
- Linux：EAGAIN（正常排空信号）/ EMSGSIZE / ENETUNREACH / ECONNREFUSED。发送 EAGAIN 时的丢弃对象与计数策略：丢新帧（保护重传路径），计入 pre_admission_dropped。

---

## 7. 测试体系与验收目标

四层：Unit / Integration / Fault Injection / Benchmark。全部跑在注入的 Clock 上。

- **Unit**：codec 往返与畸形头拒绝；串行算术与回绕区间；ACK/SACK 编解码与锚点；ABANDON 区间合并、幂等应用、received-wins、越界拒绝、覆盖销账；RTT 采样（含 Karn 排除与 SACK 不采样）、退避、recovery_estimate；deadline 入队早退；帧完备性（任意单包反推区间）。
- **Integration（进程内虚拟网络先行，后接 socket）**：按序/乱序/重复/丢包→NACK/RTO 恢复/放弃→不卡窗/ABANDON 丢失恢复/NACK 先于 ABANDON/迟到副本 TOO_OLD/SYNC_START 丢失锚定/重启换 ISN。
- **Fault Injection**：种子化 loss{iid, burst}×{0,1,5,10,20}%、delay、jitter、reorder、duplicate、非对称时延；每个场景断言 FDR 守恒与看门狗零违例。
- **Benchmark 实验**：
  - ① UDP vs TCP vs RUDP-RT（TCP 臂经同一丢包代理，应用层等长帧流，同 deadline 判定）；
  - ② Always-Retransmit vs Deadline-aware+ABANDON，**阈值参数化**（{RTO, SRTT 基}），否则结论是阈值选择的产物；
  - ③（stretch）SACK/NACK 消融。
- **验收目标（提案，W1 校准）**：RTT 10ms / jitter 2ms / deadline 100ms / 30fps 合成流：loss 5% → FDR ≥ 95%，P99 帧时延 ≤ 150ms；loss 20% burst → FDR ≥ 85% 且实验②中 deadline 臂的按时帧率严格高于 always 臂；看门狗全程零违例；守恒方程全程成立。

---

## 8. 指标定义

- **FDR（核心）**：分母 = offered frames（采集后进入网关的帧，含各阶段丢弃），分子 = 在 playout_time 前完整交付的帧。测量在接收端、用接收端锚定时钟（D-23）。分子分母必须满足 §3.14-6 守恒。
- 其余：retransmission ratio、recovery rate、abandon/expired/dropped 分解、P50/P95/P99 帧时延、window stall 次数、CPU/内存/队列深度。

---

## 9. 决策日志（策略 · 取舍 · 原因）

| # | 决策 | 备选 | 原因 |
|---|---|---|---|
| D-01 | 在 UDP 上自研可靠层而非用 TCP | TCP | TCP 可靠有序字节流 ⇒ 队头阻塞；实时媒体要"有用数据按时到" |
| D-02 | Synthetic 先行，Camera 末端接入 | 围绕相机开发 | 驱动/格式/编码问题不得阻塞协议；相机是加分项不是单点故障 |
| D-03 | RTP 管媒体语义 / RUDP 管传输语义 | 合并进 RTP | 解耦后可换编解码；可靠机制塞进 RTP 会让媒体协议与传输耦合 |
| D-04 | 双序列（RTP seq / RUDP seq）独立 | 复用一个 | 服务对象与生命周期不同；先例：QUIC packet number vs stream offset |
| D-05 | Relative Deadline（剩余寿命） | 绝对时间戳 | 两端单调钟无同步关系；相对量自洽且随重传递减 |
| D-06 | payload 1200B | 接近 64KB | 防 IP 分片放大丢包；68B 头部开销 ~5% 可接受 |
| D-07 | 64-bit SACK bitmap | 大 bitmap/编码 | 覆盖 W=64 恰好一个窗口；实现与测试简单；已知边界（ACK 滞后时远端包不可选认）由 NACK 点名缓解 |
| D-08 | 双序列空间独立编号 | 单序列 | 见 D-04 |
| D-09 | ACK=累积消耗位置（含放弃） | ACK=最后收到 | 放弃必须能被 ACK 越过，否则窗口死锁（P0-1） |
| D-10 | ABANDON 显式控制报文 | 接收端推断跳洞 | sender 确知放弃集合；不依赖接收端时钟；覆盖"帧从未到达"场景；推断错误=静默永久丢包 |
| D-11 | 放弃粒度=帧 | 按包放弃 | 帧缺一包即无价值；按包放弃会与帧内存活包的 SACK 冲突（flush 掉 sender 仍持有的包=reneging） |
| D-12 | no-reneging 不变量 + 丢新不逐旧 | 允许反悔 | SACK 后释放接收端缓冲会让 sender 永久丢失；QUIC 同款裁决 |
| D-12b | SACKed 包保留至累积 ACK | 收到 SACK 即释放 | 保留成本 ≤ 75KB/会话；把内存安全从"相信对端不变量"收回到本地；重传仲裁排除 SACKed 项 |
| D-13 | 范围裁剪：无拥塞控制/FEC/加密/NAT/自有编解码 | 全做 | 3 周 × 单人；每项都会吞掉协议验证主线；已知代价：W×MTU/RTT 吞吐天花板与无 pacing 突发 |
| D-14 | 裸 epoll 而非 Asio | Boost.Asio | 项目价值在展示 I/O 底层理解；框架会隐藏细节 |
| D-15 | Reactor 单写者 | 线程每连接 | 减少锁与数据竞争；线程模型固定为 3 线程 |
| D-16 | 显式关停顺序 | 不管 | 面试必问；资源生命周期完整性 |
| D-17 | 帧级原子入队 | 按包入窗 | 使放弃区间天然对齐帧边界；要求 frame_count ≤ W |
| D-18 | seq 在入窗时分配 | 打包时分配 | 未发送帧不产生协议痕迹；ABANDON 永不作用于未分配空间 |
| D-19 | ISN 随机化 + 无 epoch 字段 | 显式 epoch | 重启污染概率 ≈ W/2^32，可接受；省 4 字节 |
| D-20 | 窗口/缓冲参数两端静态一致，无协商 | 握手协商 | V0.1 无握手；不匹配的 OUT_OF_WINDOW 丢弃行为已定义且可测 |
| D-21 | 首个 NACK 即快重传 + 双侧抑制间隔 | 三次重复阈值 | deadline 预算薄；风暴由 SRTT 间隔抑制 |
| D-22 | 每包 ACK，无 delayed ACK | delayed | 实时协议 ACK 停留无收益；简化状态机 |
| D-23 | 传输层 deadline 仅 sender 执行；接收端时效用 playout 模型 | 接收端判过期 | 相对 deadline 跨机不可验证；同机实验会掩盖（模拟器须支持非对称时延） |
| D-24 | Jitter buffer 固定 playout delay | 自适应 | 3 周内可测可控；自适应是 V0.2 课题 |
| D-25 | 显式 SO_RCVBUF/SO_SNDBUF | 系统默认 | 内核丢包会与注入丢包混杂，破坏实验有效性 |
| D-26 | 模拟器支持非对称时延 | 仅对称 | 暴露时钟语义类缺陷（同机共享单调钟会假绿） |
| D-27 | epoll LT | ET | LT+排空最不易漏事件；ET 收益在批量场景，非本项目重点 |
| D-28 | 单 timerfd + 到期堆 | 每会话/每包 timerfd | fd 数量与扫描成本可控 |
| D-29 | per-session 发送预算 | 无 | 无机制支撑的隔离主张是过度承诺；这是隔离的唯一真实来源 |
| D-30 | IClock 注入 | 直接 chrono::now | 无虚拟时钟则 RTO/deadline 测试 flaky、"确定性实验"不成立 |
| D-31 | 会话间不共享 payload | 不可变共享 | move 所有权模型简单；N=16 时复制成本可计量、可接受 |
| D-32 | RTP 头完整保留 + RUDP 头存 rtp_ts 副本（等值校验） | 二选一 | 12B 换取标准可解码性与廉价预过滤；重复是**受检的**、有理由的 |
| D-33 | 加入从下一帧边界开始 + SYNC_START 锚定 | 客户端自主对齐 | 复用 ABANDON/重传机制；杜绝接收端自主跳洞 |
| D-34 | PING 兼作 join/restart 探测 | 新增 HELLO 类型 | 不加类型；重启即销毁重建，语义清晰 |

---

## 10. 已知限制与 Future Work

- 无拥塞控制：吞吐天花板 = W×payload/RTT（RTT 100ms 时 ≈ 6 Mbps/会话）；无 pacing，ACK 推进诱发 64 包突发——演示参数须选在天花板内。
- 客户端间软隔离之外的公平性、AIMD、pacing：预留了 send window / RTT / loss stats 接口。
- FEC、TLS、NAT 穿越、跨会话 payload 共享、双指针窗口、自适应 jitter buffer、接收端 FORCED_SKIP（带上报的强制跳洞）：明确列为 V0.2+。
- 部分帧交付（< 全帧也交付）：V0.1 明确不做，放弃粒度=帧依赖此决定。

---

## 11. 构建顺序与里程碑（3 周）

```text
W1 D1-2  规格冻结(本文档) → IClock + 类型 + 头部 codec + 串行算术（单测绿）
W1 D3-5  Sender/Receiver 核心进程内联调（虚拟网络：按序/乱序/重复/丢失/放弃不卡窗）
W2 D1-3  RTT/RTO/PING/会话生命周期 → epoll/timerfd 运行时 → Gateway 多会话
W2 D4-5  故障注入矩阵 + Metrics + FDR 守恒断言
W3 D1-3  实验①②③ + 参数校准 + 看门狗长跑
W3 D4-5  README/实验报告 + 摄像头 demo(stretch) + 冻结
```

每个里程碑的完成标准 = 对应测试全绿 + 不变量断言无违例。**禁止跳过进程内虚拟网络直接上 socket。**

---

## 12. 后续审计检查单（给下一个 AI）

1. 构造"丢 102 + 过期 + 窗口填满"剧本，验证 recv_next/ACK/send_base 全部推进且 FDR 守恒。
2. 攻击 no-reneging：构造 reorder buffer 打满的突发，验证无已报告包被撤销。
3. 攻击 ABANDON 可靠性：100% 丢 ABANDON，验证 RTO 重发路径收敛。
4. 攻击 join：SYNC_START 丢失、中途加入、重启换 ISN，验证锚定与旧包消化。
5. 验证 RTT 采样永不来自重传条目与 SACK；退避与销账回落正确。
6. 验证恢复估计含 k 因子；实验②的阈值确为参数而非常量。
7. 验证同机仿真无法掩盖时钟类缺陷（非对称时延下 sender/receiver 判定分离）。
8. 验证所有丢弃路径都出现在 FDR 守恒方程中（无静默丢弃）。
9. 验证看门狗在所有故障注入下零违例。
10. 复核全部常量（§3.15）确为注入参数而非编译期散落魔数。
