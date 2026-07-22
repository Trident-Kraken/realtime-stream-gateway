#pragma once
// rudp_rt/types.hpp — value types, enums, and serial arithmetic.
//
// Everything in this header is a plain value type usable from any thread.
// Protocol state machines (Sender/Receiver/sessions) are single-writer and
// confined to the owning runtime thread; see docs/adr/0016-ownership-table.md.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rudp_rt {

using MonoNs = std::int64_t;

constexpr MonoNs kNsPerUs = 1'000;
constexpr MonoNs kNsPerMs = 1'000'000;
constexpr MonoNs kNsPerSec = 1'000'000'000;

constexpr MonoNs ns_from_ms(MonoNs ms) { return ms * kNsPerMs; }
constexpr MonoNs ns_from_us(MonoNs us) { return us * kNsPerUs; }

// ---------------------------------------------------------------------------
// Endpoints and datagrams
// ---------------------------------------------------------------------------

struct Endpoint {
  std::uint32_t ipv4_be = 0;  // network byte order IPv4
  std::uint16_t port_be = 0;  // network byte order port

  friend bool operator==(const Endpoint&, const Endpoint&) = default;
  friend auto operator<=>(const Endpoint&, const Endpoint&) = default;
};

struct Datagram {
  Endpoint peer;
  std::vector<std::byte> bytes;
};

// ---------------------------------------------------------------------------
// Network results (closed vocabulary; no errno leaks — ADR-0009)
// ---------------------------------------------------------------------------

enum class SendStatus : std::uint8_t {
  SENT,
  WOULD_BLOCK,
  INTERRUPTED,  // adapter retries internally; reported only if it must defer
  MESSAGE_TOO_LARGE,
  UNREACHABLE,
  REFUSED,
  FATAL,
};

struct SendResult {
  SendStatus status = SendStatus::FATAL;
};

enum class RecvStatus : std::uint8_t {
  RECEIVED,
  WOULD_BLOCK,
  INTERRUPTED,
  TOO_LARGE,  // datagram exceeded the 1240-byte limit / MSG_TRUNC
  FATAL,
};

struct RecvResult {
  RecvStatus status = RecvStatus::FATAL;
  Endpoint peer;
  std::uint32_t size = 0;  // valid when status == RECEIVED
};

// ---------------------------------------------------------------------------
// Wire vocabulary (types.hpp side; codecs live in wire.hpp)
// ---------------------------------------------------------------------------

enum class PacketType : std::uint8_t {
  DATA = 1,
  ACK = 2,
  NACK = 3,
  PING = 4,
  PONG = 5,
  ABANDON = 6,
};

constexpr std::uint16_t kFlagRetransmit = 0x0001;
constexpr std::uint16_t kFlagFrameEnd = 0x0002;
constexpr std::uint16_t kFlagSyncStart = 0x0004;
constexpr std::uint16_t kFlagKnownMask = 0x0007;

enum class PingPurpose : std::uint8_t {
  JOIN = 1,
  KEEPALIVE = 2,
  RESTART = 3,
};

enum class PongStatus : std::uint8_t {
  OK = 0,
  REJECT_MAX_SESSIONS = 1,
  REJECT_UNKNOWN_STREAM = 2,
  REJECT_STALE = 3,
};

// ---------------------------------------------------------------------------
// Session/ledger vocabulary
// ---------------------------------------------------------------------------

// Terminal outcome of one session-frame (spec §6; ADR-0005).
enum class SessionFrameOutcome : std::uint8_t {
  IN_FLIGHT = 0,
  ONTIME = 1,
  LATE = 2,
  ABANDONED = 3,
  EXPIRED = 4,
  PRE_ADMISSION_DROPPED = 5,
};

inline const char* to_string(SessionFrameOutcome o) {
  switch (o) {
    case SessionFrameOutcome::IN_FLIGHT: return "in_flight";
    case SessionFrameOutcome::ONTIME: return "ontime";
    case SessionFrameOutcome::LATE: return "late";
    case SessionFrameOutcome::ABANDONED: return "abandoned";
    case SessionFrameOutcome::EXPIRED: return "expired";
    case SessionFrameOutcome::PRE_ADMISSION_DROPPED: return "pre_admission_dropped";
  }
  return "unknown";
}

// Terminal outcome of one source frame in the source ledger.
enum class SourceFrameOutcome : std::uint8_t {
  IN_FLIGHT = 0,
  CAPTURE_DROPPED = 1,
  ADMITTED_TO_FANOUT = 2,
};

inline const char* to_string(SourceFrameOutcome o) {
  switch (o) {
    case SourceFrameOutcome::IN_FLIGHT: return "in_flight";
    case SourceFrameOutcome::CAPTURE_DROPPED: return "capture_dropped";
    case SourceFrameOutcome::ADMITTED_TO_FANOUT: return "admitted_to_fanout";
  }
  return "unknown";
}

enum class TeardownCause : std::uint8_t {
  NONE = 0,
  SHUTDOWN = 1,
  SESSION_TIMEOUT = 2,
  SOCKET_ERROR = 3,
  RESOURCE_EXHAUSTED = 4,
  PROTOCOL_VIOLATION = 5,
  REPLACED = 6,  // replaced by a fresh Join with a different client epoch
};

inline const char* to_string(TeardownCause c) {
  switch (c) {
    case TeardownCause::NONE: return "none";
    case TeardownCause::SHUTDOWN: return "shutdown";
    case TeardownCause::SESSION_TIMEOUT: return "session_timeout";
    case TeardownCause::SOCKET_ERROR: return "socket_error";
    case TeardownCause::RESOURCE_EXHAUSTED: return "resource_exhausted";
    case TeardownCause::PROTOCOL_VIOLATION: return "protocol_violation";
    case TeardownCause::REPLACED: return "replaced";
  }
  return "unknown";
}

// How a DATA packet was classified by the receiver (spec §4.7 order).
enum class DataClassification : std::uint8_t {
  VALID = 0,
  MALFORMED = 1,
  SESSION_MISMATCH = 2,
  TOO_OLD = 3,
  OUT_OF_WINDOW = 4,
  DUPLICATE = 5,
  DESCRIPTOR_CONFLICT = 6,
  PROTOCOL_VIOLATION = 7,
  STORAGE_FULL = 8,
};

// ---------------------------------------------------------------------------
// Serial (RFC 1982) arithmetic for uint32 sequence/frame space.
// Every comparison below assumes both operands are within 2^31 of each other,
// which the configured windows guarantee (spec §4.5).
// ---------------------------------------------------------------------------

constexpr bool seq_lt(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) < 0;
}
constexpr bool seq_le(std::uint32_t a, std::uint32_t b) {
  return a == b || seq_lt(a, b);
}
constexpr bool seq_gt(std::uint32_t a, std::uint32_t b) { return seq_lt(b, a); }
constexpr bool seq_ge(std::uint32_t a, std::uint32_t b) {
  return a == b || seq_gt(a, b);
}

// Forward distance from a to b (valid when a is before b within 2^31).
constexpr std::uint64_t seq_forward(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::uint64_t>(b - a);
}

constexpr std::uint32_t seq_advance(std::uint32_t s, std::uint32_t n) {
  return s + n;
}
constexpr std::uint32_t seq_next(std::uint32_t s) { return s + 1; }
constexpr std::uint32_t seq_prev(std::uint32_t s) { return s - 1; }

// Frame ids use the same serial arithmetic (spec §4.1/§4.7).
constexpr bool frame_lt(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) < 0;
}

// ---------------------------------------------------------------------------
// Header value type (codec in wire.hpp)
// ---------------------------------------------------------------------------

struct Header {
  std::uint8_t version = 0;
  std::uint8_t type = 0;
  std::uint16_t flags = 0;
  std::uint8_t header_len = 0;
  std::uint8_t reserved = 0;
  std::uint16_t stream_id = 0;
  std::uint32_t seq = 0;
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint16_t deadline_ms = 0;
  std::uint16_t frame_index = 0;
  std::uint16_t frame_count = 0;
  std::uint16_t payload_len = 0;
};

struct Ack {
  std::uint32_t ack_number = 0;
  std::uint64_t sack_bitmap = 0;
  std::uint32_t session_epoch = 0;
};

struct AbandonRange {
  std::uint32_t start = 0;  // inclusive
  std::uint32_t end = 0;    // inclusive

  friend bool operator==(const AbandonRange&, const AbandonRange&) = default;
};

struct Nack {
  std::uint32_t session_epoch = 0;
  std::vector<std::uint32_t> seqs;  // count <= 64
};

struct Ping {
  std::uint32_t nonce = 0;
  PingPurpose purpose = PingPurpose::JOIN;
  std::uint32_t session_epoch = 0;  // KEEPALIVE/RESTART only
  std::uint32_t client_epoch = 0;
};

struct Pong {
  std::uint32_t nonce = 0;
  PongStatus status = PongStatus::OK;
  std::uint32_t session_epoch = 0;
  std::uint32_t server_isn = 0;
};

// ---------------------------------------------------------------------------
// Malformed-input reasons (packet counters, INV-21)
// ---------------------------------------------------------------------------

enum class RejectReason : std::uint8_t {
  NONE = 0,
  TOO_LARGE = 1,
  BAD_VERSION = 2,
  BAD_TYPE = 3,
  BAD_FLAGS = 4,
  BAD_HEADER_LEN = 5,
  BAD_RESERVED = 6,
  BAD_STREAM_ID = 7,
  LENGTH_MISMATCH = 8,
  BAD_CONTROL_FORMAT = 9,
  BAD_COUNT = 10,
  TRUNCATED = 11,
};

inline const char* to_string(RejectReason r) {
  switch (r) {
    case RejectReason::NONE: return "none";
    case RejectReason::TOO_LARGE: return "too_large";
    case RejectReason::BAD_VERSION: return "bad_version";
    case RejectReason::BAD_TYPE: return "bad_type";
    case RejectReason::BAD_FLAGS: return "bad_flags";
    case RejectReason::BAD_HEADER_LEN: return "bad_header_len";
    case RejectReason::BAD_RESERVED: return "bad_reserved";
    case RejectReason::BAD_STREAM_ID: return "bad_stream_id";
    case RejectReason::LENGTH_MISMATCH: return "length_mismatch";
    case RejectReason::BAD_CONTROL_FORMAT: return "bad_control_format";
    case RejectReason::BAD_COUNT: return "bad_count";
    case RejectReason::TRUNCATED: return "truncated";
  }
  return "unknown";
}

}  // namespace rudp_rt
