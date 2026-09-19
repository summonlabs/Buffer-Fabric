#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/codec.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Wire protocol version. A peer that presents a different version is refused
/// before any state is touched.
inline constexpr u32 kTransportMagic = 0x42465431u;  // "BFT1"

enum class MessageOp : u8 {
    Hello = 1,
    HelloAck = 2,
    Ping = 3,
    Pong = 4,
    Allocate = 5,
    Release = 6,
    Evaluate = 7,
    ObservePressure = 8,
    Explain = 9,
    Summary = 10,
    Ledger = 11,
    Metrics = 12,
    Shutdown = 13,
};

[[nodiscard]] BF_API std::string_view to_string(MessageOp op) noexcept;

/// One framed protocol message.
struct BF_API Message {
    MessageOp op{MessageOp::Ping};
    RequestId request{};
    /// The epoch the sender believes is current. Zero means "not asserted",
    /// which is only accepted for Hello and Ping.
    EpochId epoch{};
    std::vector<u8> body{};
};

/// Encode a message as one frame: u32 magic | u32 version | u32 payload_size |
/// u32 crc32c(payload) | payload, where the payload is
/// u8 op | u64 request | u64 epoch | u32 body_size | body.
[[nodiscard]] BF_API std::vector<u8> encode_message(const Message& message);

/// Decode one complete frame (header included) into a message. \p max_body_bytes
/// bounds the body. Use this when reading a raw byte stream.
[[nodiscard]] BF_API Result<Message> decode_message(const u8* data, usize size, usize max_body_bytes,
                                                    usize* consumed);

/// Decode a message from the payload already extracted by
/// Socket::receive_frame. The payload is the message body only; the transport
/// frame has been consumed and verified by the socket layer.
[[nodiscard]] BF_API Result<Message> decode_message_payload(const u8* data, usize size,
                                                            usize max_body_bytes);

// ---------------------------------------------------------------------------
// Socket
//
// A thin, bounded TCP wrapper. The runtime uses it only for loopback
// coordinator/worker traffic; nothing here reaches a non-loopback address
// unless the embedder asks for one explicitly.
// ---------------------------------------------------------------------------
class BF_API Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] static Result<Socket> listen(const std::string& address, u16 port,
                                               u16* bound_port);
    [[nodiscard]] static Result<Socket> connect_to(const std::string& address, u16 port);

    [[nodiscard]] Result<Socket> accept_peer() const;
    [[nodiscard]] Status send_all(const void* data, usize size);
    /// Returns the number of bytes read; zero means the peer closed cleanly.
    [[nodiscard]] Result<usize> receive_some(void* buffer, usize capacity);
    /// Read exactly \p size bytes. PeerClosed when the peer closed early.
    [[nodiscard]] VoidResult receive_exact(void* buffer, usize size);
    [[nodiscard]] Status send_frame(const void* payload, usize size);
    /// Read one frame with a bounded payload.
    [[nodiscard]] Result<std::vector<u8>> receive_frame(usize max_payload);

    /// Unblock any thread currently blocked in a read or accept on this socket.
    void shutdown() noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] u16 local_port() const noexcept;

private:
    explicit Socket(std::uintptr_t handle) noexcept : handle_(handle) {}
    std::uintptr_t handle_{~static_cast<std::uintptr_t>(0)};
};

/// One-time process-wide socket subsystem initialisation. Safe to call
/// concurrently; the initialisation happens exactly once.
[[nodiscard]] BF_API Status ensure_sockets_initialised();

}  // namespace buffer_fabric
