# RUDP-RT — 确定性、截止时间感知的低时延可靠流媒体传输网关

[English](README.md) | [简体中文](README_zh.md)

[![CI](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/workflows/ci.yml)
![Tests](https://img.shields.io/badge/tests-74%2F74%20passed-brightgreen.svg)
![Sanitizers](https://img.shields.io/badge/sanitizers-ASan%20%7C%20UBSan%20%7C%20TSan-success.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

**RUDP-RT** 是一套专为超低时延实时音视频流媒体研发的高性能 Linux 传输协议栈与网关系统。与传统 TCP“必须完整送达每个字节”的设计哲学不同，本协议针对流媒体的“有效时效性”，**主动用非关键数据的放弃换取关键帧的高概率按时交付（Useful, On-time Delivery）**。

系统基于现代 C++20 与 Linux 异步 I/O 架构构建，集成了 ACK/SACK/NACK 混合重传、动态 RTT/RTO 估计、滑动窗口流控、帧截止时间（Deadline）感知、**显式 ABANDON 窗口解卡握手**、**GOP 级联放弃与破损帧快速剪枝（Cascade Abandon）**，并通过双账本对账机制保证了端到端流媒体生命周期的**度量守恒律**。

```
┌────────────────────────────────────────────────────────────────────────┐
│                   媒体采集与编码层 (Synthetic / V4L2)                   │
│         - GOP 组织 (KeyFrame / DeltaFrame)  - 关键帧请求 (PLI)          │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │ SPSC 无锁环形队列 (丢最新过载保护)
┌──────────────────────────────────▼─────────────────────────────────────┐
│                 Linux 高性能运行时 (Reactor / EventLoop)                │
│    - 单写者模型 (零锁网络状态机)     - epoll + timerfd + signalfd 统一驱动│
│    - 非阻塞 UDP + EAGAIN 优雅暂存   - TokenBucket 令牌桶平滑限流         │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                   传输层核心 (RUDP-RT 协议状态机)                       │
│ - 28 字节定长二进制协议头          - 序列号安全回绕 (RFC 1982)           │
│ - ACK / SACK (Bitmap) / NACK 快速 - RFC 6298 RTO (单包采样防膨胀)       │
│ - 帧剩余时效生命周期管理          - 显式 ABANDON 报文彻底消除队头阻塞   │
│ - 级联放弃 (Cascade Abandon)      - 破损 GOP 入队快速拦截剪枝           │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                 接收端管线与播放时间线 (PlayoutPipeline)               │
│ - 严格防重/分类/过滤流水线       - 帧分片重组器 (FrameAssembler)       │
│ - RTP 绝对时间线 JitterBuffer     - 双账本对账守恒保证 (Conservation)   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 核心设计与技术亮点

### 1. 单写者并发模型（Single-Writer Principle）
核心网络协议状态机运行在专用的 Reactor 线程中，状态跃迁、定时器调度和报文收发完全无需细粒度互斥锁（Mutex），彻底消除了锁竞争与多线程死锁风险。媒体输入与网络线程间采用有界的单生产者单消费者（SPSC）环形队列解耦，在突发过载时采取确定的丢弃最新帧（Drop-newest）保护策略（[ADR-0001](docs/adr/0001-capture-queue-drop-newest.md)）。

### 2. 截止时间感知与显式 ABANDON 协议握手
传统基于重传的可靠协议（如 TCP / 传统 ARQ）在弱网丢包时会无限重传历史数据，引发严重的队头阻塞，导致后续可用的新视频帧全部超时卡死（Wedge）。  
本系统为每个媒体帧赋予截止时间（Deadline）。当帧超时判定为放弃时，发送端释放其媒体数据负载，向接收端发出携带连续序号区间的 `ABANDON` 报文，协同推进接收端的滑动窗口与重组队列，**在保障传输层状态机严谨闭环的同时，彻底打破队头阻塞**。

### 3. GOP 帧依赖感知与级联丢弃（Cascade Abandon, ADR-0019）
针对流媒体特定的编解码依赖关系（I 帧独立解码，P 帧强依赖 I 帧）：
- **级联放弃**：当关键帧因丢包超时被 Abandon 时，发送端立即识别其后续未完成的所有差分参考帧，一并执行级联放弃并合并序列号段，杜绝向网络注入“注定无法被解码”的无效废弃数据；
- **破损 GOP 快速剪枝**：新到达的差分帧若发现其参考帧已被放弃，在入队准入时直接拦截丢弃（`PRE_ADMISSION_DROPPED`），完全不占用发送滑动窗口；
- **反向关键帧重置（PLI）**：级联放弃时向上层发送关键帧请求信号，通知编码端立即生成全新 I 帧，加速解码画面恢复。

### 4. RFC 6298 RTT 采样防膨胀机制（ADR-0018）
在丢包恢复阶段，累积确认跨越了多次重传周期。若在此时采样 RTT，算出的时延将包含重传等待时间，导致平滑往返时延（SRTT）和 RTO 虚高膨胀（从 10ms 虚增到 150ms），进而拉长 NACK 抑制窗口并引发雪崩式超时。  
本系统严格限定**仅在单包前进（未受空洞阻塞）时采样真实链路 RTT**，使传输层时延估算始终贴合真实物理网络。

### 5. 双账本度量守恒律（Conservation Law）
全系统严格区分“报文级物理计数器”与“帧级生命周期状态”，在运行时建立严密的守恒等式：
$$\text{Offered} = \text{Ontime} + \text{Late} + \text{Abandoned} + \text{Expired} + \text{Pre-admission} + \text{In-flight}$$
任何一帧从进入网关到销毁，必须且只能流向一个确定的终态。内置运行时看门狗（Watchdog）对该等式进行连续审计，出现账本不平立即熔断。

### 6. 100% 确定性仿真与比特级重放（Deterministic Simulation）
将时钟与底层网络抽象为 `IClock` 与 `INetwork` 接口。基于同一随机种子，无论在何种机器架构上运行何种丢包（IID、Gilbert-Elliott 突发）、抖动、乱序组合，均可产生**比特级完全相同（Bit-identical）的测试结果与度量 JSON**，使弱网极端边界 Bug 具备 100% 可复现性。

---

## 系统组件

| 可执行目标 | 模块定位 | 核心职责 |
|---|---|---|
| `rudp_gateway` | 媒体传输网关 | 非阻塞 UDP、Linux `epoll` 多路复用、单一 `timerfd` 最小堆、SPSC 采集队列、单写者协议驱动 |
| `rudp_client` | 参考接收端 | ACK/SACK/NACK 生成器、帧分片重组器、静态抖动缓冲区（JitterBuffer）、播放分类与度量上报 |
| `rudp_sim` | 确定性仿真器 | 进程内虚拟时钟与虚拟网络测试平台，供集成测试与弱网故障矩阵快速驱动 |
| `rudp_bench` | 基准测试套件 | 提供 UDP、TCP-model、RUDP-RT 的弱网对抗多维对比与时延/FDR 压测 |

---

## 快速上手

### 1. 编译构建

```bash
# Release 模式编译（包含调试符号）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8

# 运行完整自动化测试套件（74 项测试：单元测试、模糊属性测试、集成测试、故障矩阵、系统测试）
ctest --test-dir build -j1
```

### 2. 内存与并发安全审计（Sanitizers）

```bash
# AddressSanitizer & UndefinedBehaviorSanitizer 内存安全检查
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=address -DRUDP_WERROR=OFF
cmake --build build-asan -j8 && ctest --test-dir build-asan -j1 -E "system\."

# ThreadSanitizer 并发数据竞争审计
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=thread -DRUDP_WERROR=OFF
cmake --build build-tsan -j8 && ctest --test-dir build-tsan -j1 -R "unit|integration|fault"
```

### 3. 运行回环演示程序

打开终端 1（启动网关）：
```bash
./build/rudp_gateway --port 50000 --duration 10000 --metrics-out /tmp/gw.jsonl
```

打开终端 2（启动客户端接入）：
```bash
./build/rudp_client --port 50000 --duration 8000 --metrics-out /tmp/cl.jsonl
```

运行结束后，两端均会输出不可变的最终对账 JSON 日志，记录所有帧的终态流向、FDR（按时送达率）、时延分位数（p50/p95/p99）及看门狗违例数。

---

## 实验对比数据（基于种子 0x5EED，固定 10,000 帧压测）

测试负载：30 fps，12,000 字节/帧（10 包/帧），10ms RTT ±2ms 抖动，100ms 发送端 Deadline，60ms 接收端 JitterBuffer 播放延迟。

| 测试场景 | 原生 UDP | TCP 传输模型 | RUDP-RT（时效感知版） | RUDP-RT（无时效纯重传版） |
|---|---|---|---|---|
| **5% 随机独立丢包 — 按时交付率（FDR）** | 60.0% | 99.0%* | **99.0%** | 99.0%（但伴随时延尾部拖长） |
| **20% 突发丢包（Burst-4）— 按时交付率** | 12.0% | 45.0%（发生队头阻塞卡死） | **≥ 90.0%** | 58.0%（窗口被积压死锁） |

> **关键实验结论**：在遭遇连续突发丢包时，传统 TCP 以及无限重传机制会因等待早已过期的历史数据而将整个发送窗口锁死，导致后续全新的视频帧被全部阻塞；而 RUDP-RT 凭借 `ABANDON` 与破损 GOP 级联剪枝，能以最快速度清空无效重传窗口，保证后续可用关键帧的准时交付率稳定在 **90% 以上**。

---

## 文档索引导航

- [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) — 规范级架构设计与工程落地规范书
- [`docs/protocol.md`](docs/protocol.md) — 线协议 v1 格式、二进制字段定义与状态机跃迁图
- [`docs/adr/`](docs/adr/) — 19 篇系统级架构决策记录（ADR-0001 ~ ADR-0019）
- [`docs/DEV-LOG.md`](docs/DEV-LOG.md) — 真实开发里程碑、重大 Bug 攻关复盘（Postmortems）与排错记录
- [`docs/experiments/`](docs/experiments/) — 故障注入矩阵、弱网基准测试原始数据与重放脚本
- [`CONTEXT.md`](CONTEXT.md) — 领域核心术语表与统一命名规范

---

## 开源协议

本项目采用 [MIT License](LICENSE) 开源许可。
