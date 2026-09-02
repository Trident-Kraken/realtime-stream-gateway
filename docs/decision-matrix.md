# RUDP-RT Decision Matrix

This matrix maps unresolved decisions to the parts of the system they can change. It is a review aid, not a substitute for the decision records in [`decisions.md`](decisions.md).

| Decision area | Requirements affected | Architecture/API affected | Protocol/state affected | Resource/test impact | Current status |
|---|---|---|---|---|---|
| Delivered processes and client boundary | FR-02, FR-05, observability | Executables, public modules, IPC/wire boundary | Session ownership and acceptance | End-to-end test topology | Open |
| Media frame contract and RTP profile | FR-01, FR-02, FR-04 | Source/packetizer/RTP APIs | Descriptor and playout state | Frame-size model, interoperability vectors | Open |
| Join/restart/heartbeat distinction | FR-05, liveness | Session manager and control API | Session states, PING/PONG semantics | Reconnect and timeout tests | Blocking |
| Epoch/token and endpoint identity | FR-05, safety | Session lookup and wire fields | Old packet/control filtering | Restart and spoof/flood tests | Blocking |
| Initial ISN/ACK/SYNC_START | FR-03, interoperability | Sender/receiver initialization APIs | Initial sequence state | Loss-before-sync tests | Blocking |
| Frame-id gap visibility | FR-04, FR-07 | Capture/metrics/control interfaces | Receiver skip and delivery order | Drop accounting and liveness tests | Blocking |
| Deadline origin and rounding | FR-04, acceptance targets | Frame metadata and clock APIs | Sender expiry/ABANDON states | Deterministic timing tests | Blocking |
| Packet versus frame bookkeeping | FR-03, memory bounds | Sender object model | RTO/SACK/NACK transitions | Retransmission and ownership tests | Blocking |
| EXPIRED versus ABANDON | FR-03, FR-04 | Receiver/playout interface | `recv_next` progression | Late-frame and no-stall tests | Blocking |
| Queue inventory and full policy | FR-06, FR-07 | Queue APIs and worker boundaries | Admission/drop state | Backpressure and exhaustion tests | Blocking |
| SPSC overwrite synchronization | FR-06, concurrency | Capture/Reactor handoff | Frame visibility | Race/sanitizer tests | Blocking |
| Send budget and fairness | FR-03, multi-client goal | Scheduler API | Retry/data priority | Burst, starvation, throughput tests | Open |
| Timer abstraction and cancellation | FR-03, liveness | Reactor/timer adapter | Timeout event ordering | Virtual/real clock equivalence tests | Blocking |
| Socket errno and malformed input | Safety, FR-02 | Socket adapter/error API | Sequence terminal action | Fuzz, truncation, errno tests | Blocking |
| Startup and partial initialization | Operational quality | Runtime lifecycle API | Session visibility/ready state | Rollback and failure-injection tests | Blocking |
| Worker failure propagation | Operational quality | Supervisor and health API | Global/session failure states | Crash, join, restart tests | Blocking |
| Graceful/abnormal shutdown | Lifecycle, observability | Stop API and barriers | Closing/final states | Bounded drain and final snapshot tests | Blocking |
| FDR unit and terminal outcome | FR-07, acceptance targets | Metrics API | Frame outcome state | Conservation and fan-out tests | Blocking |
| Synthetic benchmark model | Acceptance targets | Test harness API | No direct protocol state | Reproducibility and confidence intervals | Open |
| Threat model and control rate limits | Scope and deployability | Admission/filtering boundary | Unauthorized state updates | Flood and resource-exhaustion tests | Open |

## Review Gate

Rows marked **Blocking** must be resolved before implementation can claim conformance. Rows marked **Open** may be staged only if the implementation explicitly declares the resulting behavior as provisional and excludes it from frozen interoperability claims.

