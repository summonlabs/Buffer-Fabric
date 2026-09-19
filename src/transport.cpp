#include "buffer_fabric/transport.hpp"

#include <cstring>
#include <mutex>

#include "buffer_fabric/version.hpp"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_handle = SOCKET;
inline constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_handle = int;
inline constexpr socket_handle kInvalidSocket = -1;
#endif

namespace buffer_fabric {
namespace {

std::once_flag g_socket_once;
Status g_socket_status = Status::success();

void initialise_sockets_once() {
#if defined(_WIN32)
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        g_socket_status = Status(ErrorCode::TransportError, "WSAStartup failed");
    }
#endif
}

[[nodiscard]] Status socket_error(const char* what) {
#if defined(_WIN32)
    const int code = ::WSAGetLastError();
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed with socket error %d", what, code);
    return Status(ErrorCode::TransportError, buffer);
#else
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed: %s", what, std::strerror(errno));
    return Status(ErrorCode::TransportError, buffer);
#endif
}

[[nodiscard]] bool is_interrupted() noexcept {
#if defined(_WIN32)
    const int code = ::WSAGetLastError();
    return code == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

void close_handle(socket_handle handle) noexcept {
    if (handle == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

[[nodiscard]] Status set_nodelay(socket_handle handle) {
    int one = 1;
#if defined(_WIN32)
    if (::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one)) != 0) {
        return socket_error("setsockopt(TCP_NODELAY)");
    }
#else
    if (::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        return socket_error("setsockopt(TCP_NODELAY)");
    }
#endif
    return Status::success();
}

}  // namespace

std::string_view to_string(MessageOp op) noexcept {
    switch (op) {
        case MessageOp::Hello: return "hello";
        case MessageOp::HelloAck: return "hello_ack";
        case MessageOp::Ping: return "ping";
        case MessageOp::Pong: return "pong";
        case MessageOp::Allocate: return "allocate";
        case MessageOp::Release: return "release";
        case MessageOp::Evaluate: return "evaluate";
        case MessageOp::ObservePressure: return "observe_pressure";
        case MessageOp::Explain: return "explain";
        case MessageOp::Summary: return "summary";
        case MessageOp::Ledger: return "ledger";
        case MessageOp::Metrics: return "metrics";
        case MessageOp::Shutdown: return "shutdown";
    }
    return "unknown";
}

Status ensure_sockets_initialised() {
    std::call_once(g_socket_once, initialise_sockets_once);
    return g_socket_status;
}

std::vector<u8> encode_message(const Message& message) {
    ByteWriter body(32 + message.body.size());
    body.put_u8(static_cast<u8>(message.op));
    body.put_u64(message.request.raw());
    body.put_u64(message.epoch.raw());
    body.put_u32(static_cast<u32>(message.body.size()));
    body.put_bytes(message.body.data(), message.body.size());
    return encode_frame(body.data().data(), body.size(), kTransportMagic,
                        BUFFER_FABRIC_WIRE_FORMAT_VERSION);
}

namespace {

Result<Message> decode_message_body(const u8* data, usize size, usize max_body_bytes) {
    ByteReader reader(data, size);
    u8 raw_op = 0;
    if (!reader.get_u8(&raw_op)) {
        return Status(ErrorCode::ProtocolViolation, "message has no opcode");
    }
    Message message;
    message.op = static_cast<MessageOp>(raw_op);
    u64 request = 0;
    u64 epoch = 0;
    u32 body_bytes = 0;
    if (!reader.get_u64(&request) || !reader.get_u64(&epoch) || !reader.get_u32(&body_bytes)) {
        return Status(ErrorCode::ProtocolViolation, "message header is not decodable");
    }
    if (static_cast<usize>(body_bytes) > max_body_bytes) {
        return Status(ErrorCode::OversizedMessage, "message body exceeds the bound");
    }
    const u8* body = nullptr;
    if (!reader.get_bytes(&body, static_cast<usize>(body_bytes))) {
        return Status(ErrorCode::TruncatedMessage, "message body is truncated");
    }
    message.request = RequestId::from_raw(request);
    message.epoch = EpochId::from_raw(epoch);
    message.body.assign(body, body + static_cast<usize>(body_bytes));
    if (!reader.empty()) {
        return Status(ErrorCode::ProtocolViolation, "message has trailing bytes");
    }
    return message;
}

}  // namespace

Result<Message> decode_message_payload(const u8* data, usize size, usize max_body_bytes) {
    return decode_message_body(data, size, max_body_bytes);
}

Result<Message> decode_message(const u8* data, usize size, usize max_body_bytes, usize* consumed) {
    Frame frame;
    usize used = 0;
    Status status = decode_frame(data, size, kTransportMagic, BUFFER_FABRIC_WIRE_FORMAT_VERSION,
                                 max_body_bytes + 32, &frame, &used);
    if (!status.ok()) return status;
    auto message = decode_message_body(frame.payload.data(), frame.payload.size(), max_body_bytes);
    if (!message.ok()) return message.status();
    if (consumed != nullptr) *consumed = used;
    return message;
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
    }
    return *this;
}

bool Socket::valid() const noexcept {
    return handle_ != static_cast<std::uintptr_t>(kInvalidSocket);
}

void Socket::close() noexcept {
    if (!valid()) return;
    close_handle(static_cast<socket_handle>(handle_));
    handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
}

void Socket::shutdown() noexcept {
    if (!valid()) return;
#if defined(_WIN32)
    ::shutdown(static_cast<socket_handle>(handle_), SD_BOTH);
#else
    ::shutdown(static_cast<socket_handle>(handle_), SHUT_RDWR);
#endif
}

Result<Socket> Socket::listen(const std::string& address, u16 port, u16* bound_port) {
    BF_TRY(ensure_sockets_initialised());
    if (address.empty()) {
        return Status(ErrorCode::InvalidArgument, "listen address is empty");
    }
    socket_handle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) return socket_error("socket");
    Socket socket(static_cast<std::uintptr_t>(handle));

    int one = 1;
#if defined(_WIN32)
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        return Status(ErrorCode::InvalidArgument, "listen address must be a dotted-quad IPv4 address");
    }
    if (::bind(handle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        return socket_error("bind");
    }
    if (::listen(handle, 64) != 0) {
        return socket_error("listen");
    }
    if (bound_port != nullptr) {
        sockaddr_in actual{};
#if defined(_WIN32)
        int length = sizeof(actual);
#else
        socklen_t length = sizeof(actual);
#endif
        if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &length) != 0) {
            return socket_error("getsockname");
        }
        *bound_port = ntohs(actual.sin_port);
    }
    return socket;
}

Result<Socket> Socket::connect_to(const std::string& address, u16 port) {
    BF_TRY(ensure_sockets_initialised());
    if (address.empty()) {
        return Status(ErrorCode::InvalidArgument, "connect address is empty");
    }
    if (port == 0) {
        return Status(ErrorCode::InvalidArgument, "connect port must be non-zero");
    }
    socket_handle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) return socket_error("socket");
    Socket socket(static_cast<std::uintptr_t>(handle));
    BF_TRY(set_nodelay(handle));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        return Status(ErrorCode::InvalidArgument, "connect address must be a dotted-quad IPv4 address");
    }
    if (::connect(handle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        const Status failure = socket_error("connect");
        return failure;
    }
    return socket;
}

Result<Socket> Socket::accept_peer() const {
    if (!valid()) {
        return Status(ErrorCode::LifecycleViolation, "socket is not open");
    }
    socket_handle peer = ::accept(static_cast<socket_handle>(handle_), nullptr, nullptr);
    if (peer == kInvalidSocket) {
        return socket_error("accept");
    }
    Socket socket(static_cast<std::uintptr_t>(peer));
    const Status status = set_nodelay(peer);
    if (!status.ok()) return status;
    return socket;
}

Status Socket::send_all(const void* data, usize size) {
    if (!valid()) return Status(ErrorCode::LifecycleViolation, "socket is not open");
    const auto* cursor = static_cast<const u8*>(data);
    usize remaining = size;
    while (remaining > 0) {
        const int chunk = static_cast<int>(remaining > 0x40000000ull ? 0x40000000ull : remaining);
#if defined(_WIN32)
        const int written = ::send(static_cast<socket_handle>(handle_),
                                   reinterpret_cast<const char*>(cursor), chunk, 0);
#else
        const ssize_t written = ::send(static_cast<socket_handle>(handle_), cursor,
                                       static_cast<usize>(chunk), 0);
#endif
        if (written <= 0) {
            if (is_interrupted()) continue;
            return socket_error("send");
        }
        cursor += written;
        remaining -= static_cast<usize>(written);
    }
    return Status::success();
}

Result<usize> Socket::receive_some(void* buffer, usize capacity) {
    if (!valid()) return Status(ErrorCode::LifecycleViolation, "socket is not open");
    if (capacity == 0) return static_cast<usize>(0);
    const int chunk = static_cast<int>(capacity > 0x40000000ull ? 0x40000000ull : capacity);
#if defined(_WIN32)
    const int got = ::recv(static_cast<socket_handle>(handle_), static_cast<char*>(buffer), chunk, 0);
#else
    const ssize_t got = ::recv(static_cast<socket_handle>(handle_), buffer,
                               static_cast<usize>(chunk), 0);
#endif
    if (got == 0) return static_cast<usize>(0);
    if (got < 0) {
        if (is_interrupted()) return static_cast<usize>(0);
        return socket_error("recv");
    }
    return static_cast<usize>(got);
}

VoidResult Socket::receive_exact(void* buffer, usize size) {
    auto* cursor = static_cast<u8*>(buffer);
    usize remaining = size;
    while (remaining > 0) {
        auto got = receive_some(cursor, remaining);
        if (!got.ok()) return got.status();
        if (got.value() == 0) {
            return Status(ErrorCode::PeerClosed, "peer closed the connection");
        }
        cursor += got.value();
        remaining -= got.value();
    }
    return VoidResult{};
}

Status Socket::send_frame(const void* payload, usize size) {
    if (size > kMaxFrameBytes) {
        return Status(ErrorCode::OversizedMessage, "frame exceeds the transport bound");
    }
    const std::vector<u8> bytes =
        encode_frame(payload, size, kTransportMagic, BUFFER_FABRIC_WIRE_FORMAT_VERSION);
    return send_all(bytes.data(), bytes.size());
}

Result<std::vector<u8>> Socket::receive_frame(usize max_payload) {
    u8 header[kFrameHeaderBytes];
    BF_TRY(receive_exact(header, kFrameHeaderBytes));
    ByteReader reader(header, kFrameHeaderBytes);
    u32 magic = 0;
    u32 version = 0;
    u32 payload_bytes = 0;
    u32 expected_crc = 0;
    if (!reader.get_u32(&magic) || !reader.get_u32(&version) || !reader.get_u32(&payload_bytes) ||
        !reader.get_u32(&expected_crc)) {
        return Status(ErrorCode::ProtocolViolation, "frame header is not decodable");
    }
    if (magic != kTransportMagic) {
        return Status(ErrorCode::ProtocolViolation, "frame magic mismatch");
    }
    if (version != BUFFER_FABRIC_WIRE_FORMAT_VERSION) {
        return Status(ErrorCode::ProtocolVersionMismatch, "wire format version mismatch");
    }
    if (static_cast<usize>(payload_bytes) > max_payload) {
        return Status(ErrorCode::OversizedMessage, "frame payload exceeds the transport bound");
    }
    std::vector<u8> payload(payload_bytes);
    if (payload_bytes != 0) {
        BF_TRY(receive_exact(payload.data(), payload.size()));
    }
    if (crc32c(payload.data(), payload.size()) != expected_crc) {
        return Status(ErrorCode::IntegrityFailure, "frame checksum mismatch");
    }
    return payload;
}

u16 Socket::local_port() const noexcept {
    if (!valid()) return 0;
    sockaddr_in actual{};
#if defined(_WIN32)
    int length = sizeof(actual);
#else
    socklen_t length = sizeof(actual);
#endif
    if (::getsockname(static_cast<socket_handle>(handle_), reinterpret_cast<sockaddr*>(&actual),
                      &length) != 0) {
        return 0;
    }
    return ntohs(actual.sin_port);
}

}  // namespace buffer_fabric
