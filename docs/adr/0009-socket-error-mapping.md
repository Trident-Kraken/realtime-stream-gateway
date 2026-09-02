# ADR-0009: Socket send/receive error mapping and pending-datagram semantics

- **Status:** Accepted (Phase 0)
- **Context:** Spec §4.6 and §5 fix how errno-classified failures map to protocol transitions. This ADR records the exact mapping used by `socket_network.cpp` and the protocol cores.
- **Decision:**
  - `INetwork::send()` returns `SENT`, `WOULD_BLOCK`, `INTERRUPTED`, `MESSAGE_TOO_LARGE`, `UNREACHABLE`, `REFUSED`, `FATAL` — mapping from `EAGAIN/EWOULDBLOCK`, `EINTR`, `EMSGSIZE`, `ENETUNREACH/EHOSTUNREACH/ENETDOWN`, `ECONNREFUSED`, everything else respectively. `errno` never escapes the adapter.
  - `WOULD_BLOCK`: the datagram is retained as the session's single pending datagram and EPOLLOUT is armed; at most one datagram is pending because the sender stops emitting for that session until the pending slot drains. Pending DATA past the frame deadline triggers normal abandonment (INV-16: no orphan).
  - `INTERRUPTED`: retried immediately by the adapter, transparent to the protocol.
  - `MESSAGE_TOO_LARGE` (`EMSGSIZE`): configuration error — startup validation must prevent it; at runtime it is a `FATAL`-class event for the session: bounded teardown with cause `SOCKET_ERROR` and a process-level error counter.
  - `UNREACHABLE` / `REFUSED` / persistent `EPOLLERR`: bounded session teardown (cause `SOCKET_ERROR`); admitted frames finalized `ABANDONED(SOCKET_ERROR)`; the process continues serving other sessions.
  - `receive()` returns `RECEIVED` (with peer + size), `WOULD_BLOCK`, `INTERRUPTED` (transparently retried), `TOO_LARGE` (datagram exceeded the 1240-byte limit or `MSG_TRUNC` observed; the datagram is discarded by the caller and counted malformed), `FATAL`.
- **Consequences:** no protocol component sees errno; virtual-network and socket adapters are substitutable (identical `SendResult`/`RecvResult` vocabulary).
- **Test anchors:** unit error-mapping cases with a scripted fault adapter; system socket-error case (client `connect()` + unreachable gateway port).
