# RUDP-RT 第二轮场景攻击审计

## 审计方式

本轮不重新做第一轮的静态问题清单，而是把设计当作一个将被故障、负载和并发事件持续攻击的系统，逐场景推演从输入到最终状态。审计对象仍然只有 `rudp-rt-design-v0.2.md`；没有实现代码，因此“未明确时的系统行为”是根据文档最接近的实现推断，不是对某个实现的事实断言。

每个场景固定回答十项：处理方式、规定程度、未规定时的推断行为、状态一致性、资源泄漏、数据丢失、死锁/livelock/race、无限等待、无界内存，以及需要增加的约束。

严重程度只表示这个场景对“能否得到唯一、可验证实现”的风险：

- **CRITICAL**：核心状态可能不收敛，或所有实现无法得到一致行为。
- **HIGH**：常见负载/故障会损害活性、指标或资源安全。
- **MEDIUM**：最小 Demo 可能继续运行，但结果或边界行为不可信。
- **LOW**：主要是实验边界、运维和后续演进风险。

## 场景推演

### R2-01：正常流量（MEDIUM）

1. **当前设计如何处理？** Capture Worker 把 Synthetic/Camera 帧放入 SPSC，Reactor 取帧、组 RTP/RUDP-RT、在窗口内发送 DATA；客户端回 ACK，receiver 组帧后进入 Jitter Buffer 并按 playout_time 交付。
2. **是否有明确规定？** 主路径在 §2、§3.5、§3.10、§3.11 有描述，但客户端消费 API、首个 ACK/ISN、Reactor 被新帧唤醒和 RTP profile 没有完整规定。
3. **如果没有明确规定，系统行为是什么？** 实现者会自行选择初始 `recv_next`、ring 消费时机、帧交付接口和 RTP 字段；不同实现可能互不兼容。
4. **是否可能出现状态不一致？** 可能。首个 SYNC_START、frame_id 顺序和 ACK 锚点没有唯一初始化规则。
5. **是否可能出现资源泄漏？** 有可能。Outstanding、Jitter Buffer 和 timer heap 的所有权/销毁关系没有闭合；正常流量下通常表现为有界滞留而非必然泄漏。
6. **是否可能出现数据丢失？** 可能。SPSC 到 Reactor 没有规定唤醒事件，帧可能延迟到 deadline；窗口或发送预算也可能丢新帧。
7. **是否可能出现死锁 / livelock / race？** SPSC 满时生产者丢最旧与消费者读同项存在 race；首个控制报文缺失时可能出现协议初始化停滞。
8. **是否可能出现无限等待？** 可能。后帧按 frame_id 交付时，前一帧缺口如果没有可见的 ABANDON，receiver 没有合法跳过依据。
9. **是否可能出现 unbounded memory growth？** Jitter Buffer、控制队列和 FrameState 没有容量定义，因此端到端正常流量也没有完整的有界证明。
10. **当前设计是否需要增加约束？** 需要：冻结最小端到端契约、初始化握手、帧消费边界、唤醒机制和所有运行队列容量。

### R2-02：突发流量（HIGH）

1. **当前设计如何处理？** SPSC 深度为 256 帧，满时丢最旧；窗口为 64 包，per-session 发送预算用于限制突发；reorder 满时丢新包。
2. **是否有明确规定？** ring 和 reorder 的单点策略有规定，但突发时 frame_id 缺口、多个 session 的公平调度、控制报文队列和 Jitter Buffer 没有规定。
3. **如果没有明确规定，系统行为是什么？** 最可能是 capture 帧被覆盖、未入窗帧不分配 seq、发送队列继续堆积，接收端看到 frame_id 间断后自行等待或自行跳过。
4. **是否可能出现状态不一致？** 是。源帧被丢弃但没有 ABANDON/Skip 记录，sender、receiver 和 FDR 对“这帧是否存在”会有不同看法。
5. **是否可能出现资源泄漏？** ring 本身有界；未定义的 outgoing/control/Jitter 队列可能无限增长，或在丢弃时忘记释放 payload。
6. **是否可能出现数据丢失？** 会，丢最旧和丢新均是设计允许行为；但丢失类别和后续帧能否交付没有闭合定义。
7. **是否可能出现死锁 / livelock / race？** 高概率出现 ring 覆盖 race；单 Reactor 若持续处理一个 session 的重传，其他 session 可能饿死，形成调度 livelock。
8. **是否可能出现无限等待？** 窗口满后“帧在 SPSC 前等待”没有解除条件；若 SPSC 同时满且没有唤醒/淘汰规则，生产者或 Reactor 可能长期等。
9. **是否可能出现 unbounded memory growth？** 只要发送队列、Jitter Buffer 或 metrics snapshot queue 没有上限，就可能；文档只给了部分结构的上限。
10. **当前设计是否需要增加约束？** 需要：定义每个队列的高水位/低水位、突发预算、跨 session 调度和每种丢弃的唯一记账规则。

### R2-03：持续高负载（CRITICAL）

1. **当前设计如何处理？** 理论上依靠 `W=64`、SPSC=256、reorder=256、SO buffer=4MB 和 per-session budget 形成上限；窗口只随 ACK 前进。
2. **是否有明确规定？** 只有这些局部常量，没有端到端服务率、CPU 饱和后的处理预算、控制面优先级或持续过载的 admission policy。
3. **如果没有明确规定，系统行为是什么？** Reactor 会在 UDP drain、重传、ACK/NACK、抓帧和快照之间竞争 CPU；未规定的队列将增长，已规定的队列开始丢弃。
4. **是否可能出现状态不一致？** 是。每个阶段可能独立丢弃同一源帧的不同副本，FDR 既没有唯一主键也没有明确终态优先级。
5. **是否可能出现资源泄漏？** 有可能。持续触发 RESOURCE_EXHAUSTED 时，部分创建的 FrameState、timer entry、snapshot 和 payload 的回滚没有定义。
6. **是否可能出现数据丢失？** 必然会，且可能先丢旧帧再丢新帧；“有意丢弃”和“来不及处理而静默丢弃”无法区分。
7. **是否可能出现死锁 / livelock / race？** 单线程 Reactor 不会因协议锁死，但可能在异常/重传控制上 livelock；SPSC 和异步 metrics 仍有 race 风险。
8. **是否可能出现无限等待？** ACK 停止时 send window 不释放；若 ABANDON 发送路径也被资源或 socket 压制，窗口可永久停在前端。
9. **是否可能出现 unbounded memory growth？** 会，除非实现自行给所有未列出的队列加上限；文档不能证明高负载下总内存有界。
10. **当前设计是否需要增加约束？** 必须增加：定义系统容量模型、过载状态、优先级、限时处理预算、全队列上限和明确的“丢弃即终止”状态转换。

### R2-04：下游处理速度下降（HIGH）

1. **当前设计如何处理？** receiver 对每个 DATA 立即 ACK；完整帧进入 Jitter Buffer，按静态 playout_delay 交付；没有描述应用消费速度反馈。
2. **是否有明确规定？** 传输 ACK 行为明确，但 Jitter Buffer 容量、慢消费者策略和 ACK 是否代表“应用已消费”没有规定。
3. **如果没有明确规定，系统行为是什么？** 最可能是客户端继续 ACK 并接收，完整帧在未定义的 Jitter Buffer 中累积；sender 认为数据已消耗而释放重传 payload。
4. **是否可能出现状态不一致？** 是。传输层认为已消耗，播放层却尚未消费；FDR 按 playout_time 统计时可能把仍在队列中的帧算 late/expired。
5. **是否可能出现资源泄漏？** 若 Jitter Buffer 没有限制，会无限增长；若强行丢弃，所有权和指标归属未定义。
6. **是否可能出现数据丢失？** 会。慢消费者最终必须丢帧或进程耗尽内存，但文档未指定丢弃优先级。
7. **是否可能出现死锁 / livelock / race？** 客户端应用若与 receiver 分线程，FrameState/Jitter Buffer 的所有权没有同步规则，存在 race；持续慢消费可造成无效接收/丢弃 livelock。
8. **是否可能出现无限等待？** 若实现选择不丢帧，交付队列可能等待应用无限久；若按 frame_id 等待某个缺帧，也会阻塞后帧。
9. **是否可能出现 unbounded memory growth？** 明确可能，Jitter Buffer 是最直接的未定义增长点。
10. **当前设计是否需要增加约束？** 必须增加：Jitter Buffer 上限、慢消费者策略、应用消费接口和“ACK/接收/交付”三者的语义边界。

### R2-05：上游突然断开（HIGH）

1. **当前设计如何处理？** 若“上游”指 Camera/Capture Worker，文档只规定停止采集和 SPSC 排空的关停顺序；没有规定 V4L2 断开、Synthetic producer 退出或输入错误的运行时处理。
2. **是否有明确规定？** 没有。上游断开的含义本身也未定义，是设备断开、采集线程崩溃还是输入流结束。
3. **如果没有明确规定，系统行为是什么？** Reactor 可能继续保持 session、发送 PING/处理 ACK，但不再产生 DATA；或把输入停止误当作全局 shutdown，取决于实现者。
4. **是否可能出现状态不一致？** 是。Capture、Gateway、client 之间没有“源结束/暂时无帧/输入故障”的协议状态；客户端可能一直认为流仍然存在。
5. **是否可能出现资源泄漏？** session、timer 和空闲队列可能保持到 keepalive timeout；如果 worker 消失且没有通知，资源可能永不按预期释放。
6. **是否可能出现数据丢失？** 断开时在 ring、Outstanding 和 Jitter Buffer 中的数据会丢失；其归类为 capture_dropped、abandoned 还是 in_flight 未定义。
7. **是否可能出现死锁 / livelock / race？** Capture Worker 退出与 Reactor 读取 SPSC 可能 race；Reactor 继续给空输入做 PING 可形成无意义工作循环。
8. **是否可能出现无限等待？** 没有源结束事件时，session 可能等到超时；若 keepalive 仍被认为是“活跃”，可能长期不结束。
9. **是否可能出现 unbounded memory growth？** 输入停止本身不会增长 ring，但未释放的 session/Jitter/metrics 状态可能累积，尤其是反复断开但不拆会话时。
10. **当前设计是否需要增加约束？** 必须明确输入 source lifecycle、断开/EOF/error 事件、是否广播流结束、残留帧终态和 worker 到 Reactor 的故障通知。

### R2-06：下游突然断开（HIGH）

1. **当前设计如何处理？** UDP 没有连接断开通知；网关依靠 PONG 丢失或连续 `3×RTO_MAX` 无有效 ACK 触发 SESSION_TIMEOUT，然后释放 session。
2. **是否有明确规定？** 检测条件有规定，但 PING nonce 匹配、单向可达、超时期间的新帧处理、重连和旧报文过滤没有规定。
3. **如果没有明确规定，系统行为是什么？** Gateway 会继续占用窗口和重传内存，直到某个 timeout；实现者可能在 timeout 前继续发送所有新帧，或提前停止但没有协议依据。
4. **是否可能出现状态不一致？** 是。客户端已释放而 gateway 仍持有 session；客户端重连时旧 timer/ACK/DATA 可能与新 ISN 交错。
5. **是否可能出现资源泄漏？** 有限期资源滞留几乎必然；若 timeout 事件失效或重连覆盖旧 session 未清理 timer，可能永久泄漏。
6. **是否可能出现数据丢失？** 会。断开期间的数据和重传预算会丢失，且最终计入 abandoned/expired/in_flight 的规则不明。
7. **是否可能出现死锁 / livelock / race？** timeout、迟到 ACK 和 Join PING 的排序可能造成 teardown/recreate race；重传直到超时可能形成无效 livelock。
8. **是否可能出现无限等待？** 如果 PING/PONG 计数被错误地重置，或 ACK timeout 未定义从哪个事件起算，session 可能不结束。
9. **是否可能出现 unbounded memory growth？** 单 session 受 W 限制，但反复使用不同 endpoint/stream 建立未清理 session 时总量可能超过 `max_sessions` 语义范围。
10. **当前设计是否需要增加约束？** 必须定义 UDP 端点失联状态机、超时后禁止发送的时刻、重连 token/epoch 和 teardown 的原子性。

### R2-07：网络抖动（HIGH）

1. **当前设计如何处理？** 乱序 DATA 进入 reorder buffer，产生 NACK；RTO 根据 SRTT/RTTVAR 计算，重传受 SRTT 间隔抑制；deadline 到期则整帧 ABANDON。
2. **是否有明确规定？** 机制名称和部分公式有规定，但重排窗口、NACK 洞生命周期、虚假快速重传以及 ACK/控制报文的抖动处理没有完整规定。
3. **如果没有明确规定，系统行为是什么？** 延迟包可能先触发 NACK 和重传，随后原包作为 duplicate；RTO 可能被未重传样本或批量 ACK 的时间偏差影响。
4. **是否可能出现状态不一致？** 会。receiver 把包报告为 SACK 后，sender 只知道“当前持有”，但 ABANDON/延迟副本的交互没有唯一释放顺序。
5. **是否可能出现资源泄漏？** reorder 有 256 包上限，但洞表、FrameState、控制队列和 Jitter Buffer 没有同等完整的上限。
6. **是否可能出现数据丢失？** 会。抖动超过窗口或 playout_delay 时，按设计可丢弃；但是否 late、expired 或 abandoned 未明确。
7. **是否可能出现死锁 / livelock / race？** 高频乱序可造成 NACK/重传振荡，属于 livelock 风险；真实 socket 收发与 timerfd 事件顺序也会影响结果。
8. **是否可能出现无限等待？** 缺失包没有后续包触发 NACK，或 NACK/ABANDON 被持续延迟时，receiver 无法自行决定跳洞，可能等待至 session timeout。
9. **是否可能出现 unbounded memory growth？** 高抖动下未定义的 FrameState/Jitter/控制队列可能增长；已列出的 reorder 受限。
10. **当前设计是否需要增加约束？** 需要：定义最大可容忍重排时延、洞过期、NACK 上限、控制面预算和抖动下的统计分类。

### R2-08：半连接 / 单向可达（HIGH）

1. **当前设计如何处理？** 通过双向 PING/PONG、ACK 和 timeout 观察对端；没有 TCP 式半连接状态，协议假定双方可互发控制报文。
2. **是否有明确规定？** 没有“半连接”或单向路径的状态定义；“任一方出向静默”的 PING 条件不足以说明哪条方向失效。
3. **如果没有明确规定，系统行为是什么？** 例如 DATA/ACK 可达但 PING/PONG 不可达时，双方会按不同计数器重传或拆除；实现者会自行决定以 ACK 还是 PONG 为准。
4. **是否可能出现状态不一致？** 是。Gateway 可能认为 session 活跃，receiver 已停止；或 receiver 继续 ACK 旧数据而 gateway 已超时销毁。
5. **是否可能出现资源泄漏？** 直到两端各自 timeout 前资源会滞留；如果一侧永远有单向控制流，另一侧可能永不释放。
6. **是否可能出现数据丢失？** 会，单向可达时控制/媒体必有一方向丢失；数据归类未定义。
7. **是否可能出现死锁 / livelock / race？** 两侧可能持续 PING、NACK、重传而互不收敛，形成控制 livelock；重连时还会和旧 timer 竞争。
8. **是否可能出现无限等待？** 是。仅依赖“有效 ACK”或仅依赖 PONG 的实现都可能在另一类报文仍可达时无限保持。
9. **是否可能出现 unbounded memory growth？** 每 session 的 DATA 受窗口限制，但控制重试/诊断队列若无上限可能增长。
10. **当前设计是否需要增加约束？** 必须定义方向性健康状态、哪些报文证明会话活性、单向故障的拆除上限和重连行为。

### R2-09：timeout（CRITICAL）

1. **当前设计如何处理？** 文档同时有 frame deadline、RTO、ABANDON retry、PING/PONG 三次失败、`3×RTO_MAX` ACK 超时、playout EXPIRED 和 send_base watchdog。
2. **是否有明确规定？** 每个局部条件有文字或公式，但没有统一的优先级、时钟来源、状态转换顺序和相互覆盖关系。
3. **如果没有明确规定，系统行为是什么？** 同一帧可能先被 receiver 标记 EXPIRED，再被 sender ABANDON；同一 session 可能同时满足 PONG timeout 和 ACK timeout，触发不同清理路径。
4. **是否可能出现状态不一致？** 明确可能。FrameState、Outstanding state、abandoned_intervals 和 FDR 终态可能被多个 timeout 重复结算。
5. **是否可能出现资源泄漏？** timer heap 中已失效事件、RTO/ABANDON 条目和 session teardown 的取消方式未定义，存在悬挂条目。
6. **是否可能出现数据丢失？** timeout 本来允许丢帧，但超时阈值不一致会导致仍有播放价值的数据过早 ABANDON，或过期数据占住窗口。
7. **是否可能出现死锁 / livelock / race？** 多个到期事件对同一对象进行重传、放弃、拆除，若没有 generation/幂等状态转换会出现 race 或重复动作。
8. **是否可能出现无限等待？** frame timeout 不会自动推进 ACK；ABANDON timeout 若控制报文不可发也可能无限等待；watchdog 目前只是告警而非恢复动作。
9. **是否可能出现 unbounded memory growth？** 到期事件和已销毁对象若不从堆移除，会造成堆与诊断记录增长；每项上限未完整给出。
10. **当前设计是否需要增加约束？** 必须建立统一 timeout 状态机，明确单调时间轴、优先级、幂等性、事件取消和 watchdog 触发后的具体恢复动作。

### R2-10：retry（HIGH）

1. **当前设计如何处理？** DATA 可由 NACK 或 RTO 重传；ABANDON 用 RTO 和 NACK 快路径重发；重传次数参与 recovery_estimate，条目 RTO 指数退避。
2. **是否有明确规定？** DATA/ABANDON 的主要路径明确；PING/PONG、发送 errno、达到 RTO_MAX 后是否继续、全帧与逐包重试边界没有明确规定。
3. **如果没有明确规定，系统行为是什么？** 实现者可能无限重试 ABANDON、在 EAGAIN 时直接丢弃控制报文，或把一次帧重传视为一次/多次计数；结果会不同。
4. **是否可能出现状态不一致？** 是。sender 可能已将 seq 标成 ABANDONED，但 receiver 未收到；或者 ACK 已越过而延迟 NACK 又触发 DATA 重传。
5. **是否可能出现资源泄漏？** ABANDON stub、重试 timer 和控制队列若没有最大数量/清理条件，会长期保留。
6. **是否可能出现数据丢失？** deadline-aware 设计允许丢失；重试策略不明确时会额外丢失本可恢复的帧，或错误重传已不可用帧。
7. **是否可能出现死锁 / livelock / race？** NACK 与 RTO 同时触发可能重复安排重传；没有统一决策入口的原子实现会产生 retry storm/livelock。
8. **是否可能出现无限等待？** 设计没有明确总 retry budget 或最终失败状态；控制报文永久丢失时 ABANDON 可能一直在等待 ACK。
9. **是否可能出现 unbounded memory growth？** 单窗口 payload 有界，但 pending retry/control entries 的总量没有独立上限。
10. **当前设计是否需要增加约束？** 需要：定义各消息的最大重试次数/最终动作、重试合并、errno 处理和所有 retry entry 的上限。

### R2-11：duplicate message（MEDIUM）

1. **当前设计如何处理？** DATA 按 DUPLICATE/TOO_OLD 分类；ABANDON 明确幂等；ACK 丢失被认为无害；其他控制报文重复处理未规定。
2. **是否有明确规定？** DATA duplicate 和 ABANDON 幂等有规定，但 ACK、NACK、PING、PONG、SYNC_START 重复及其计数/副作用没有完整规定。
3. **如果没有明确规定，系统行为是什么？** 重复 ACK 可能被忽略或重复采样；重复 PING 可能按 Join 逻辑重建；重复 SYNC_START 可能重新锚定；实现选择会改变会话。
4. **是否可能出现状态不一致？** 是，尤其是重复 PING/同步包；重复 ACK 的非单调处理也可能错误推进 sender。
5. **是否可能出现资源泄漏？** 重复 DATA 受窗口限制，通常不增长；重复控制若每次创建 retry/timer 状态，可能泄漏或膨胀。
6. **是否可能出现数据丢失？** 重复本身不应丢新数据，但错误把重复 SYNC_START 当重启可清空在途帧。
7. **是否可能出现死锁 / livelock / race？** 重复 NACK/PING 可制造重传或重建 livelock；单 Reactor 串行化事件不能替代幂等语义。
8. **是否可能出现无限等待？** 重复 ACK 若被拒绝且真实 ACK 丢失，sender 可能保持窗口；重复 PING 反复重建则无法稳定到达同步态。
9. **是否可能出现 unbounded memory growth？** 控制报文副本的 pending 状态和 metrics 记录未定义上限，存在增长风险。
10. **当前设计是否需要增加约束？** 需要一份逐消息幂等/去重表，定义 duplicate 的状态副作用、计数和可接受窗口。

### R2-12：out-of-order message（HIGH）

1. **当前设计如何处理？** DATA 乱序进入 reorder buffer，产生空洞 NACK；ABANDON 可先记录后生效；`advance_recv_next` 按连续收到/放弃推进。
2. **是否有明确规定？** DATA 的主要路径较明确；控制报文乱序、frame_id 与 seq 乱序交叉、ACK 先于 SYNC_START 的行为不明确。
3. **如果没有明确规定，系统行为是什么？** 实现者可能缓存或丢弃早到控制报文；ACK 可能被直接应用，导致 sender 状态相对于新 session 错位。
4. **是否可能出现状态不一致？** 是。ABANDON 与 DATA、ACK 与 NACK、Join 与旧数据的不同到达顺序没有完整偏序规则。
5. **是否可能出现资源泄漏？** reorder 有界；预同步缓存、abandon intervals、future FrameState 的容量和淘汰没有全部给出。
6. **是否可能出现数据丢失？** 丢包和窗口外包可按设计丢弃，但控制乱序可能使本可恢复的包变成永久洞。
7. **是否可能出现死锁 / livelock / race？** 缺包且对应 ABANDON 先被丢弃时可能停窗；NACK/重传互相触发可 livelock。
8. **是否可能出现无限等待？** 是，尤其是 SYNC_START 丢失且后续描述不足以建立唯一基线时。
9. **是否可能出现 unbounded memory growth？** 预同步缓存明确 ≤W，但 FrameState/Jitter/control queue 未完整有界。
10. **当前设计是否需要增加约束？** 必须定义所有消息类型的乱序偏序、预同步缓存升级为正常窗口的原子切换，以及每种 future state 的上限。

### R2-13：malformed input（HIGH）

1. **当前设计如何处理？** 列出了 INVALID_VERSION、HEADER、LENGTH、TYPE、SEQUENCE、WINDOW 和 PROTOCOL_VIOLATION 等错误类型，并要求畸形头拒绝。
2. **是否有明确规定？** 错误名称明确，但解析顺序、错误响应、计数粒度、是否创建 session、`MSG_TRUNC`/短 datagram 和未知字段处理没有规定。
3. **如果没有明确规定，系统行为是什么？** 实现者可能在长度校验前读取字段，或对坏包回错误/静默丢弃；不同实现的安全性和 CPU 成本不同。
4. **是否可能出现状态不一致？** 可能。半解析包若已更新 session/FrameState 后才发现 RTP 或 frame descriptor 错误，会留下部分状态。
5. **是否可能出现资源泄漏？** 恶意或持续坏包可能创建临时对象、metrics 记录和错误响应队列；上限与速率限制未定义。
6. **是否可能出现数据丢失？** 坏包应丢弃，但若错误分类误伤同序列的合法重传，可能丢失恢复机会。
7. **是否可能出现死锁 / livelock / race？** 大量坏包可让 Reactor 一直排空 UDP 而延迟 timer/发送，形成 CPU livelock；并发结构仍有既有 race。
8. **是否可能出现无限等待？** 坏包制造的序列洞如果被错误接受部分字段，receiver 可能等待永远不会到达的合法包。
9. **是否可能出现 unbounded memory growth？** 若每个坏包都被保存为诊断事件或触发 NACK，可能无界增长；文档没有错误日志/计数上限。
10. **当前设计是否需要增加约束？** 需要：先验长度/类型，再做无副作用校验，最后一次性提交状态；定义坏包限速、计数上限和是否关闭 session。

### R2-14：oversized message（HIGH）

1. **当前设计如何处理？** 设计目标是 payload 1200B、全包 1268B，避免 IPv4 分片；错误表列出 EMSGSIZE 和 INVALID_LENGTH。
2. **是否有明确规定？** 没有精确规定 DATA 最大 payload、RTP+媒体在 `payload_len` 中的含义、接收端 `MSG_TRUNC` 和 ABANDON/NACK 的最大总包长。
3. **如果没有明确规定，系统行为是什么？** 发送端可能让 kernel 返回 EMSGSIZE、产生 IP 分片，或在 Packetizer 处截断；接收端可能按 header 长度读取不存在的字节或静默丢弃。
4. **是否可能出现状态不一致？** 若 seq 已分配后发现 oversized，sender 可能留下洞；若只截断媒体，frame_count/index 与实际 payload 不再匹配。
5. **是否可能出现资源泄漏？** 大包解析若按 `payload_len` 分配而不受硬上限约束，会瞬间耗尽内存；失败回滚没有规定。
6. **是否可能出现数据丢失？** 会，且可能是整帧丢失；是否计入 pre_admission_dropped、protocol_dropped 或 malformed 未定义。
7. **是否可能出现死锁 / livelock / race？** 连续 oversized 输入可让 Reactor 反复分配/拒绝，造成 CPU livelock；发送失败后的 seq 洞会停窗。
8. **是否可能出现无限等待？** 如果 oversized 帧已入窗但无法发送且没有 ABANDON 路径，窗口可能无限等待。
9. **是否可能出现 unbounded memory growth？** 在未定义接收上限或 Packetizer 临时 buffer 上限时，可能无界/瞬时爆发式增长。
10. **当前设计是否需要增加约束？** 必须冻结 wire-level max datagram、各字段长度公式、拒绝时机、seq 回滚/ABANDON 策略和 `MSG_TRUNC` 处理。

### R2-15：queue full（CRITICAL）

1. **当前设计如何处理？** SPSC 满时丢最旧；窗口满时帧“在 SPSC 前等待”；发送 EAGAIN 时丢新帧并记为 pre_admission_dropped；其他 queue 的行为未写。
2. **是否有明确规定？** 只有 SPSC 的一种策略明确，metrics snapshot、命令、发送、控制、Jitter 和 pending_abandon 队列没有统一策略。
3. **如果没有明确规定，系统行为是什么？** 实现者会选择阻塞、丢旧、丢新、覆盖或扩容；若选择阻塞，Reactor 可能被某个队列卡住；若选择扩容，内存不再有界。
4. **是否可能出现状态不一致？** 是。队列中的帧可能已分配 seq、已计入 offered、但在出队前被丢弃而没有 ABANDON 或终态记账。
5. **是否可能出现资源泄漏？** 队列覆盖/取消时 payload、FrameState 和 timer entry 的释放没有规定，存在泄漏或 double free 风险。
6. **是否可能出现数据丢失？** 设计明确允许部分丢弃，但丢弃对象、顺序和 FDR 分类不完整。
7. **是否可能出现死锁 / livelock / race？** 阻塞式等待会死锁 Reactor；无锁覆盖的 SPSC 会 race；不停重试满队列会 livelock。
8. **是否可能出现无限等待？** “在 SPSC 前等待”没有超时/取消；发送队列满又没有 EPOLLOUT 或预算唤醒时可能无限等。
9. **是否可能出现 unbounded memory growth？** 若任何未定义队列采用无界增长，是；文档没有总队列预算。
10. **当前设计是否需要增加约束？** 必须为每个队列定义固定容量、生产/消费所有权、满队列动作、超时/取消和指标映射。

### R2-16：buffer exhaustion（CRITICAL）

1. **当前设计如何处理？** reorder buffer 满时丢新包；sender 放弃帧时释放 payload；SO_RCVBUF/SO_SNDBUF 目标为 4MB；发送 EAGAIN 时丢新帧。
2. **是否有明确规定？** reorder 和 sender payload 有局部规定，但 Jitter、socket 实际容量、临时 codec buffer、控制 buffer 和 metrics queue 没有完整规定。
3. **如果没有明确规定，系统行为是什么？** 实现者可能丢包、阻塞、扩容或关闭 session；不同选择会改变 ACK/SACK 和 deadline 结果。
4. **是否可能出现状态不一致？** 是。reorder 丢新包无需 SACK，但如果对应 FrameState 已被标记“有包”或 descriptor 已提交，状态仍可能不一致。
5. **是否可能出现资源泄漏？** 内存分配失败和 buffer 覆盖的回滚/析构未定义；SACKed payload 需保留到累计 ACK，也增加释放时序复杂度。
6. **是否可能出现数据丢失？** 会。buffer 满时的丢弃是设计路径，但必须保证未报告包不会被 sender 永久跳过。
7. **是否可能出现死锁 / livelock / race？** buffer 满后 sender 等 ACK、receiver 等缺包、控制报文又进不去时可能停窗；跨线程 buffer 所有权有 race。
8. **是否可能出现无限等待？** 是，尤其是已丢新包成为洞而 sender 未触发 ABANDON/重传时。
9. **是否可能出现 unbounded memory growth？** 未定义 buffer 可能；显式固定 buffer 本身有界，但“总内存”没有核算临时对象。
10. **当前设计是否需要增加约束？** 必须建立全局内存预算和每类 buffer 的硬上限、拒绝/释放顺序、状态回滚及 metrics。

### R2-17：resource exhaustion（CRITICAL）

1. **当前设计如何处理？** `max_sessions=16`，会话表满返回 REJECT_MAX_SESSIONS；协议错误和 RESOURCE_EXHAUSTED 被列为错误类别。
2. **是否有明确规定？** 只对 session slot 有明确响应；FD、线程、timer heap、payload、FrameState、control queue、metrics 和临时 parse buffer 的耗尽动作没有规定。
3. **如果没有明确规定，系统行为是什么？** 可能拒绝新 session、丢当前帧、关闭 socket、进程退出或继续分配直到 OOM，完全取决于实现者。
4. **是否可能出现状态不一致？** 是。旧 session 是否先销毁、PONG 是否已发出、session map 是否已插入，都没有事务边界。
5. **是否可能出现资源泄漏？** 明确有风险，特别是“销毁旧 session、重建新 ISN”遇到中途分配失败时。
6. **是否可能出现数据丢失？** 会，资源保护必然需要拒绝/丢弃；但缺少唯一终态和 FDR 归类。
7. **是否可能出现死锁 / livelock / race？** 反复分配失败/重试 Join 或控制报文可 livelock；timer teardown 和 session map 更新可能 race。
8. **是否可能出现无限等待？** 若资源释放依赖 session timeout，而 timeout 事件本身无法入 heap/执行，新的 Join 可能无限等待。
9. **是否可能出现 unbounded memory growth？** 资源耗尽前的未定义队列会先增长；错误日志/metrics 也没有上限。
10. **当前设计是否需要增加约束？** 必须定义资源分级、预留控制面资源、分配失败事务回滚、拒绝码和 OOM 后的进程级策略。

### R2-18：concurrent shutdown（CRITICAL）

1. **当前设计如何处理？** 信号经 signalfd/self-pipe 进入 Reactor；顺序是停止采集、排空 SPSC、停止新帧、输出 final metrics、关 socket、join 线程。
2. **是否有明确规定？** 有名义顺序，但没有规定 shutdown 与 UDP、timerfd、eventfd、Capture enqueue、Metrics snapshot 同时发生时的事件优先级和屏障。
3. **如果没有明确规定，系统行为是什么？** 实现者会在 event loop 中按到达顺序处理；shutdown 期间可能仍接受 ACK/NACK、创建 timer 或消费新帧。
4. **是否可能出现状态不一致？** 明确可能。final metrics 可能早于最后一次状态提交，session map 可能在 timer/packet 仍引用时释放。
5. **是否可能出现资源泄漏？** pending socket sends、SPSC payload、Jitter、timer heap 和 metrics queue 的 drain/取消不完整会泄漏或被粗暴遗弃。
6. **是否可能出现数据丢失？** graceful stop 也会丢未完成/未交付帧；文档没有最终 ABANDON/FIN，因此客户端无法区分正常结束和故障。
7. **是否可能出现死锁 / livelock / race？** join 线程若等待 Capture Worker，而 Capture Worker 等 ring 空间/事件，会死锁；Metrics Worker 可能等待永远不会来的 final snapshot。
8. **是否可能出现无限等待？** “排空 SPSC”没有 deadline；网络发送 drain 也没有上限；线程故障时 join 可能永久等待。
9. **是否可能出现 unbounded memory growth？** shutdown 本身不应增长，但如果停止过程中继续接收/排队且没有状态门禁，队列可能继续增长。
10. **当前设计是否需要增加约束？** 必须定义 shutdown state machine、停止接收/发送的原子门、drain deadline、线程完成确认、timer invalidation 和最终快照 barrier。

### R2-19：startup failure（HIGH）

1. **当前设计如何处理？** 设计列出 Linux socket、timerfd、eventfd、signalfd、SO buffer 和 Camera 等运行组件，但没有启动失败流程。
2. **是否有明确规定？** 没有。错误表只列运行时 errno，没有启动阶段的失败分类、日志、退出码或回滚顺序。
3. **如果没有明确规定，系统行为是什么？** 实现者可能直接退出、降级为 Synthetic、继续运行缺少 timer/socket 的半成品，或让线程启动后才发现依赖缺失。
4. **是否可能出现状态不一致？** 是。部分组件已注册 epoll/heap 或线程已运行，后续初始化失败时是否撤销未定义。
5. **是否可能出现资源泄漏？** 可能。已创建 fd、绑定 socket、分配 queue、启动线程和打开 camera 的反初始化顺序没有规定。
6. **是否可能出现数据丢失？** 启动尚未建立 session 时输入帧可能被采集并丢弃；是否计入 offered 尚未定义。
7. **是否可能出现死锁 / livelock / race？** 失败通知与线程启动竞态可能导致 join/退出 race；epoll 空 fd 循环也可能 livelock。
8. **是否可能出现无限等待？** 某个 worker 等待 Reactor ready，而 Reactor 因初始化失败未进入 loop 时可能无限等待。
9. **是否可能出现 unbounded memory growth？** 启动失败重试若没有退避和资源回收，重复创建 queue/session/日志可增长。
10. **当前设计是否需要增加约束？** 必须定义原子 startup transaction、依赖顺序、失败回滚、降级是否允许、ready barrier 和进程退出语义。

### R2-20：partial initialization（CRITICAL）

1. **当前设计如何处理？** Join PING 触发 session 创建；已有同名会话先销毁再以新随机 ISN 重建；资源表和协议状态在 session 内集中保存。
2. **是否有明确规定？** 没有定义 session 创建的事务阶段、PONG OK 发送时机、失败回滚和旧会话保留策略。
3. **如果没有明确规定，系统行为是什么？** 可能先插入 session map，再分配窗口/reorder/timer；中途失败会留下“可查到但不可用”的 session，或已经销毁旧会话却没有新会话。
4. **是否可能出现状态不一致？** 是，且直接影响 stream_id 路由、max_sessions、ACK/SYNC_START 和 PONG 状态。
5. **是否可能出现资源泄漏？** 是。任何中途分配失败都可能留下 fd、timer、payload、旧状态或 metrics stub。
6. **是否可能出现数据丢失？** 会。重启判定先销毁旧 session，会丢失旧会话仍可交付的数据；失败后的 Join 也可能吞掉重试。
7. **是否可能出现死锁 / livelock / race？** Join、timeout、旧 ACK 和 timer 同时命中时存在 cancellation/teardown race；反复失败 Join 可 livelock。
8. **是否可能出现无限等待？** PONG OK/REJECT 的发送失败和初始化 ready 条件没有上限，客户端可能一直重试或等待。
9. **是否可能出现 unbounded memory growth？** 半初始化 session 和重复 Join 若未被严格计数/回收，可耗尽 session 与辅助资源。
10. **当前设计是否需要增加约束？** 必须把 session 创建做成可回滚事务，规定 map 可见时刻、资源预留、PONG 时机、失败码和旧 session 的保留/替换原子性。

### R2-21：worker thread failure（CRITICAL）

1. **当前设计如何处理？** 设计只有 Capture Worker、Reactor 和 Metrics Worker 的职责与正常关停顺序，没有 worker heartbeat、异常捕获、监督或重启策略。
2. **是否有明确规定？** 没有。线程崩溃、异常退出、阻塞和被系统取消都没有运行时语义。
3. **如果没有明确规定，系统行为是什么？** Reactor 可能继续运行并读取已停止的 SPSC；Capture 失败会让流静默；Metrics 失败会让指标队列堆积或丢失；主线程可能仍认为服务健康。
4. **是否可能出现状态不一致？** 是。单写者原则只约束协议状态，不定义 worker 健康状态；Reactor 与 Metrics 对最终状态的认识可能分叉。
5. **是否可能出现资源泄漏？** 失败线程持有的 payload、camera fd、queue slot、snapshot 和待 join 资源可能不释放。
6. **是否可能出现数据丢失？** 会。Capture/metrics worker 失败会丢输入或指标；Reactor 失败则所有在途状态丢失。
7. **是否可能出现死锁 / livelock / race？** join 失败线程、ring 访问和 shutdown 通知存在 race；Reactor 继续空转可 livelock。
8. **是否可能出现无限等待？** 主线程 join 一个已失去通知的 worker，或 Reactor 等待永远不会产生的 ring/event，均可能无限等待。
9. **是否可能出现 unbounded memory growth？** Metrics Worker 失败时 snapshot queue 没有消费；错误监控/重启队列同样未定义上限。
10. **当前设计是否需要增加约束？** 必须定义线程监督、异常边界、健康状态、失败传播、可重启性和 join/cancel 的超时策略。

### R2-22：connection race（CRITICAL）

1. **当前设计如何处理？** session key 是 `(client_addr, stream_id)`；同名 PING 被解释为重启并销毁旧会话；单 Reactor 负责串行修改协议状态。
2. **是否有明确规定？** 单线程顺序明确，但同一 client 的并发 Join、旧 PING/ACK/DATA 迟到、不同源端口和超时交错没有裁决规则。
3. **如果没有明确规定，系统行为是什么？** 事件按 UDP 到达顺序进入同一 key；旧事件可能被应用到新 session，或第二个 Join 再次销毁刚创建的 session。
4. **是否可能出现状态不一致？** 是。不同 epoch 没有 token，seq 随机化只能降低误收概率，不能为控制报文提供确定隔离。
5. **是否可能出现资源泄漏？** 旧 session 销毁与新 timer/heap entry 的交叉可能留下悬挂事件和 payload stub。
6. **是否可能出现数据丢失？** 会，重建 session 的定义就是清除旧在途状态；并发 Join 会让新会话刚发出的数据也被清空。
7. **是否可能出现死锁 / livelock / race？** 单 Reactor 避免协议字段的数据竞争，但语义层面有重建 livelock；跨线程 shutdown/metrics 仍有 race。
8. **是否可能出现无限等待？** 会话不断被旧 Join 重建时，可能永远无法完成 SYNC_START/ACK；没有 epoch 过期时间界限。
9. **是否可能出现 unbounded memory growth？** 若每次重建都把旧 timer/metrics 延迟清理，总资源可能增长；session 数表面仍不超过 16。
10. **当前设计是否需要增加约束？** 必须定义连接/会话代际标识、并发 Join 的胜者、旧报文过滤、源端口变化和 teardown 与创建的原子交换。

### R2-23：cancellation race（CRITICAL）

1. **当前设计如何处理？** sender 统一决策入口同时处理 NACK、RTO 和定时扫描；deadline 命中把帧改为 ABANDON、释放 payload；ACK 可推进 send_base 并销账。
2. **是否有明确规定？** 同一 Reactor 内调用顺序没有规定；NACK/RTO/ACK/ABANDON/关停同时针对同一条目的取消与提交顺序也没有规定。
3. **如果没有明确规定，系统行为是什么？** 实现者可能先释放 payload 再处理迟到 ACK/NACK，也可能先重传再放弃；只要没有单一事务，就会出现不同结果。
4. **是否可能出现状态不一致？** 是。条目可能已经 ABANDONED 但仍被重传，或已销账后 timer 再次处理；FrameState 和 pending_abandon 可能重复更新。
5. **是否可能出现资源泄漏？** payload、timer entry、NACK 洞和 ABANDON stub 在取消时的幂等释放没有定义，存在 leak/double free/UAF 风险。
6. **是否可能出现数据丢失？** 会，deadline 取消和 ACK 到达的先后会决定数据是可交付还是被放弃；当前没有可审计的优先级。
7. **是否可能出现死锁 / livelock / race？** timerfd heap 与 session teardown 的取消 race 明确存在；反复取消/重排控制事件可 livelock。
8. **是否可能出现无限等待？** 被取消的重传若没有最终 ABANDON/销账事件，窗口仍可等待；取消信号本身也没有确认。
9. **是否可能出现 unbounded memory growth？** 取消失败导致 stale heap entries、pending controls 和 snapshots 累积时可能增长。
10. **当前设计是否需要增加约束？** 必须定义每个对象的线性状态转换、事件优先级、generation/取消令牌、幂等销毁和单一提交点。

### R2-24：configuration error（HIGH）

1. **当前设计如何处理？** 默认值给出 W、max_frame_packets、payload、D、playout、RTO、buffer 和 max_sessions；窗口静态一致但不协商。
2. **是否有明确规定？** 没有启动时配置校验矩阵，也没有规定 W、`max_frame_packets`、payload、RTO、D、playout、socket buffer 之间的合法关系和错误响应。
3. **如果没有明确规定，系统行为是什么？** 可能启动一个数学上不成立的协议：`max_frame_packets>W` 无法原子入窗，`D<RTO_MIN` 使所有帧早退，`RTO_MIN>RTO_MAX` 使退避失效。
4. **是否可能出现状态不一致？** 是。两端 W/reorder 不一致时 receiver 按 D-20 丢包，但 sender 不知道配置错误；不同 clock rate 会破坏 playout。
5. **是否可能出现资源泄漏？** 无效配置若在部分初始化后才被发现，会继承 startup/partial-init 的回滚风险。
6. **是否可能出现数据丢失？** 可能全部丢失，例如 payload 超过 MTU、D 小于最小恢复时间、stream 未配置或 packet count 超 W。
7. **是否可能出现死锁 / livelock / race？** W 与 buffer 配置不匹配可使洞永远无法填；重试配置无效可造成 timer livelock。
8. **是否可能出现无限等待？** `W=0`、无效 timeout、未唤醒的 timer 或不一致窗口均可能让 sender/receiver 永久等待。
9. **是否可能出现 unbounded memory growth？** 过大的未校验 queue/buffer 配置会直接取消设计中的内存上限；配置项没有全局预算。
10. **当前设计是否需要增加约束？** 必须在启动前验证所有配置、拒绝不相容组合、记录实际值，并规定两端不一致时的发现与退出行为。

### R2-25：graceful shutdown（HIGH）

1. **当前设计如何处理？** 按 D-16 停止采集、排空 SPSC、停止新帧、输出 final metrics、关闭 socket、join 线程。
2. **是否有明确规定？** 有顺序，但没有协议级 FIN/END/ABANDON、客户端结束通知、未完成帧 drain deadline 或 final snapshot 消费确认。
3. **如果没有明确规定，系统行为是什么？** 网关关闭 socket 后客户端只能通过 timeout 推断结束；未 ACK 的 DATA 可能保持为 in_flight 或被本地直接释放。
4. **是否可能出现状态不一致？** 是。sender 最终状态、receiver playout 结果和 metrics worker 看到的快照可能不同；FDR 方程的 in_flight 终态不唯一。
5. **是否可能出现资源泄漏？** 排空期间未定义的控制/定时器/队列若没有 barrier，可能泄漏或访问已释放对象。
6. **是否可能出现数据丢失？** 会，优雅关停仍可能丢 deadline 未到但尚未发送/交付的帧；是否算正常结束丢弃没有规定。
7. **是否可能出现死锁 / livelock / race？** 等待 SPSC 排空和网络发送 drain 可能阻塞；关停事件与 ACK/timer 处理存在 race。
8. **是否可能出现无限等待？** 如果以“排空”为完成条件而输入/网络持续产生事件，没有明确截止时间就可能无限等待。
9. **是否可能出现 unbounded memory growth？** shutdown 中若仍接收入队而停止消费，队列可能增长；没有 shutdown 门禁和硬上限说明。
10. **当前设计是否需要增加约束？** 强烈需要：定义有界 graceful drain、结束协议、未完成对象分类、最终快照 barrier 和超时后强制终止动作。

### R2-26：abnormal termination（CRITICAL）

1. **当前设计如何处理？** 未规定。进程崩溃、SIGKILL、断电或 kernel kill 后 UDP 状态消失；客户端只能依赖 PING/PONG 或 ACK timeout。
2. **是否有明确规定？** 只有“重启随机 ISN、旧包污染概率约 W/2^32”的弱假设，没有 crash recovery、持久化、RESET 或重新同步协议。
3. **如果没有明确规定，系统行为是什么？** 重启后的网关以新进程状态开始；旧客户端可能继续发 ACK/NACK，网关把它们当未知/新会话或静默丢弃，直到客户端重新 Join。
4. **是否可能出现状态不一致？** 是。客户端和网关对 seq、frame_id、RTP anchor、metrics epoch 不一致；随机 ISN 只降低误收，不提供确定性隔离。
5. **是否可能出现资源泄漏？** 操作系统会回收 fd/内存，但外部客户端、实验记录、共享临时文件和远端 session 状态会滞留；重启后的 stale timers 不在新进程中可见。
6. **是否可能出现数据丢失？** 必然会，所有非持久化在途 payload 丢失；FDR 是否把异常终止帧排除、计入 abandoned 或 in_flight 未定义。
7. **是否可能出现死锁 / livelock / race？** 恢复期间旧控制流和新 Join 可能反复碰撞；客户端若无限重试旧 seq 可能形成恢复 livelock。
8. **是否可能出现无限等待？** 没有明确 client restart timeout/rejoin policy 时，客户端可能一直等待旧会话恢复；网关也无法知道旧客户端是否仍存在。
9. **是否可能出现 unbounded memory growth？** 反复 crash/restart 的监控、metrics 和客户端重试队列若未限流可能增长；单次进程内资源由 OS 回收。
10. **当前设计是否需要增加约束？** 必须至少定义异常终止后的重连/重同步协议、epoch 或 session token、客户端重试上限，以及 metrics 的异常终止归类。

## 场景横向结论

### 必须修改

以下项目在进入实现前必须从“概念描述”变成唯一可执行的规范：

1. **控制面和会话代际**：区分 Join、Restart、Heartbeat、PONG，定义单向故障、并发 Join、迟到报文和异常重启的裁决。
2. **初始序列与所有控制报文的顺序规则**：冻结 ISN/epoch/session token、初始 `recv_next`、ACK 无值状态，以及 ACK/NACK/ABANDON 的 stale/future/duplicate 处理。
3. **队列、buffer 和 payload 所有权**：为 SPSC、发送、控制、Jitter、metrics、timer heap 和 FrameState 给出容量、内存序、满队列动作、取消和析构规则；“满时丢最旧”不能继续依赖一个未定义的 SPSC 覆盖实现。
4. **发送错误与 cancellation 状态机**：明确 seq 何时算已发送、EAGAIN/EMSGSIZE/网络错误如何终止帧、ABANDON 如何在控制面受阻时保证或放弃 liveness。
5. **deadline、EXPIRED、ABANDON 和 frame_id 缺口**：确定预算起点、receiver 过期是否能跳洞、未入窗/采集丢弃帧如何被后续帧观察，以及所有状态的唯一终态。
6. **timeout/retry/timer 生命周期**：统一 frame/RTO/keepalive/session/watchdog 的优先级和时钟源；定义 timer generation、取消、重试最终失败和 shutdown 后事件失效。
7. **启动、部分初始化、worker 失败和关停**：增加 startup transaction、失败回滚、线程监督、ready/stop barrier、有界 drain 和异常终止规则。
8. **wire specification 与配置验证**：冻结每种报文的长度、字段含义、RTP profile/clock rate、最大 datagram、异常输入路径，并在启动前拒绝不相容配置。
9. **FDR 和测试 oracle**：固定 fan-out 统计主键、唯一终态优先级、丢弃类别和 synthetic frame/packet 分布；否则“守恒”和验收阈值没有可操作含义。

### 强烈建议修改

这些项目不一定阻塞最小单客户端 Demo，但不补齐就无法把系统称为可运维、可重复的网关：

1. 为半连接/单向可达定义方向性健康指标和最大滞留时间。
2. 为 malformed/duplicate/control flood 增加速率限制、诊断计数上限和可信网络威胁模型。
3. 把实际 `SO_RCVBUF/SO_SNDBUF`、kernel drop、CPU 饱和、队列水位纳入实验前置和 metrics。
4. 明确 IPv4/IPv6、UDP endpoint identity、源端口变化和 stream 配置边界。
5. 增加 ThreadSanitizer、ASan/UBSan、分配失败、队列覆盖、timer teardown、worker failure 和 concurrent shutdown 测试。
6. 为 graceful shutdown 提供明确的结束信号或文档化“客户端只能通过 timeout 感知结束”的限制。
7. 为无 pacing/无拥塞控制的实验写出链路容量、session 数、发送预算和突发限制，避免把自发拥塞误当成注入 loss。
8. 明确 Camera/输入 source 的 EOF、格式变化、设备断开和恢复策略。

### 可以保持现状

这些决策在场景攻击后仍然自洽，可以保留，但必须建立在上面约束补齐之后：

1. Synthetic Frame 先行、Camera 仅作末端 Demo 依赖。
2. RTP 承担媒体语义、RUDP-RT 承担传输语义的分层。
3. 单 Reactor 作为协议状态唯一写者；只要跨线程通知、内存序和 shutdown barrier 被明确，三线程模型可以保持。
4. `seq` 在入窗时分配、帧级原子入窗和帧级 ABANDON；前提是部分发送/发送失败状态被补充为包级可执行簿记。
5. no-reneging、reorder 满时丢新包，以及显式 ABANDON 填洞的总体方向。
6. IClock 与进程内虚拟网络先于真实 socket 的测试顺序；但真实 timerfd 的适配边界必须单独验证。
7. 固定 playout delay、暂不做 FEC/TLS/NAT/拥塞控制等范围裁剪；应同时保留清晰的实验条件和非目标声明。

## 最终判断

不同经验丰富的工程师目前仍会对以下问题做出不同实现：何时一个 session 算建立/重启/失联，初始 ACK 如何表达，丢失但未入窗的帧如何跳过，发送 EAGAIN 后 seq 如何了结，慢消费者是否丢帧，timeout/ABANDON/EXPIRED 谁优先，最终 FDR 如何计数，以及 shutdown/worker failure 后哪些对象必须被回收。因此当前设计尚未达到“可以直接实现且不同实现行为基本一致”的程度。

**本轮结论：不能进入实现阶段。** 必须修改项完成并通过场景化状态测试后，才可以进入最小协议闭环；强烈建议项可以分阶段完成，但不应在未声明限制的情况下用于性能或可靠性结论。
