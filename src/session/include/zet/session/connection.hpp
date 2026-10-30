#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/channel.hpp"
#include "zet/session/channel_error.hpp"
#include "zet/session/client_session.hpp"
#include "zet/wire/frame_stream.hpp"
#include "zet/wire/handshake.hpp"
#include "zet/wire/limits.hpp"

/// One TCP connection of a session, from the first byte to the last, on either
/// end, per docs/design.md §5 and §9.
///
///     agent:  Hello ▶ route by sid ▶ Challenge … AuthOk ▶ Resume opens ▶
///     Channel client: Hello ……………………………………………▶ Auth ▶ AuthOk ▶ Resume ▶
///     Channel
///
/// Sans-I/O like everything under it. Bytes come in through OnBytes, frames go
/// out through Pump into a buffer the caller provides, and what happened in
/// between comes back as an event. Time and nonces are handed in: the clock
/// and the generator are the two things §16 lets a test replace.
///
/// Neither class does the budget of §5 beyond its own deadline — failed Auth
/// counts per sid, caps per IP — for the reason the handshake does not: those
/// span connections, and live with whoever holds the table of them.
namespace zet::session {

/// Room a connection needs to reassemble the largest frame there is.
inline constexpr std::size_t CONNECTION_FRAME_STORAGE =
    wire::HEADER_SIZE + wire::MAX_FRAME;

/// Room Pump needs in `out` to write the largest frame there is.
inline constexpr std::size_t CONNECTION_OUT_SIZE =
    wire::HEADER_SIZE + wire::MAX_FRAME;

/// Room Pump needs in `scratch`: the plaintext of the largest frame, which is
/// also more than the widest Data a Channel builds.
inline constexpr std::size_t CONNECTION_SCRATCH_SIZE = wire::MAX_FRAME;

static_assert(CONNECTION_SCRATCH_SIZE >= Channel::POLL_SCRATCH_SIZE);

/// The agent read a Hello and needs the session it names. The caller looks
/// the sid up and answers with Route or RouteUnknown before pumping again.
struct NeedsSession {
    wire::SessionId Id{};
};

/// `Written` bytes at the front of `out` are a frame for the socket.
struct SendFrame {
    std::size_t Written{0};
};

/// Stream bytes for the local side: the pty on the agent, the terminal on the
/// client. They point into `scratch` and are good until the next Pump.
struct Deliver {
    ByteSpan Bytes;
};

/// The client's terminal changed size. Agent only.
struct Resized {
    wire::ResizeMessage Size;
};

/// The agent's shell has ended, and the client holds every byte it wrote.
/// Client only; the session has recorded it too (ClientSession::EndedWith),
/// so the caller restores the terminal, closes the connection and exits
/// rather than reconnecting.
struct Ended {
    wire::SessionEndMessage End;
};

/// Nothing to do until more bytes arrive or NextDeadline passes.
struct Idle {};

using ConnectionEvent =
    std::variant<NeedsSession, SendFrame, Deliver, Resized, Ended, Idle>;

using ConnectionResult = ChannelResult<ConnectionEvent>;

/// What both ends do alike: reassemble frames, keep a deadline, remember the
/// first error. Not used on its own.
class ConnectionBase {
public:
    ConnectionBase(const ConnectionBase&) = delete;
    ConnectionBase& operator=(const ConnectionBase&) = delete;
    ConnectionBase(ConnectionBase&&) = delete;
    ConnectionBase& operator=(ConnectionBase&&) = delete;

    /// Takes bytes off the socket. A refusal here is the frame layer's, and
    /// ends the connection like any other.
    [[nodiscard]] ChannelResult<void> OnBytes(ByteSpan chunk) noexcept;

    /// Whether Resume has gone through and the session's streams flow.
    [[nodiscard]] bool IsLive() const noexcept { return Channel_.has_value(); }

    /// What the next OnBytes will still take. Reading at most this much off
    /// the socket keeps a caller out of the error path; zero means Pump has
    /// whole frames to work through first.
    [[nodiscard]] std::size_t ReadRoom() const noexcept {
        return Frames_.Room();
    }

    /// The most OnBytes ever holds: one frame of the current stage. Whatever
    /// Pump delivers out of one read is never more than this.
    [[nodiscard]] std::size_t ReadCapacity() const noexcept {
        return Frames_.Capacity();
    }

    /// When Pump next has something to do on its own: the handshake deadline
    /// until the connection is live, the channel's afterwards.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

protected:
    ConnectionBase(MutableByteSpan frameStorage, Timestamp now) noexcept;
    ~ConnectionBase() = default;

    /// Remembers the first error and hands it back; every later Pump returns
    /// it again. A connection that failed stays failed.
    [[nodiscard]] SessionError Fail(SessionError error) noexcept;

    /// The handshake deadline, checked at the top of every Pump.
    [[nodiscard]] bool HandshakeExpired(Timestamp now) const noexcept;

    /// Refuses buffers smaller than Pump's contract. A defect in the caller,
    /// not the peer, so it is reported without failing the connection.
    [[nodiscard]] static ChannelResult<void> CheckBuffers(
        MutableByteSpan out, MutableByteSpan scratch) noexcept;

    /// Writes a cleartext frame header in front of `bodyLength` bytes that
    /// the handshake already wrote at out[HEADER_SIZE..], and returns the
    /// length of the whole frame.
    [[nodiscard]] static std::size_t FrameCleartext(
        MutableByteSpan out, std::size_t bodyLength) noexcept;

    /// The room a handshake message may be written into.
    [[nodiscard]] static MutableByteSpan HandshakeBody(
        MutableByteSpan out) noexcept;

    /// The live half, shared by both ends once Resume has gone through: one
    /// incoming frame, else one outgoing one. `onOpened` runs after a frame
    /// opened, which is where the client confirms the seed. A connection that
    /// hears nothing for the session's PeerSilenceTimeout fails here with
    /// PeerSilent (§9).
    template <typename TOnOpened>
    [[nodiscard]] ConnectionResult PumpLive(Timestamp now, MutableByteSpan out,
                                            MutableByteSpan scratch,
                                            TOnOpened&& onOpened) noexcept;

    wire::FrameStream Frames_;
    Timestamp Started_;
    std::optional<Channel> Channel_;
    std::optional<SessionError> Failed_;
    std::uint16_t NegotiatedVersion_{wire::PROTOCOL_VERSION_MIN};
};

/// The agent's end.
class AgentConnection : public ConnectionBase {
public:
    /// `frameStorage` must hold CONNECTION_FRAME_STORAGE bytes; until the peer
    /// authenticates only MAX_HANDSHAKE_FRAME of it is used.
    AgentConnection(MutableByteSpan frameStorage, Timestamp now) noexcept;

    /// Answers NeedsSession for a sid that was found.
    void Route(SessionRef session, wire::HandshakeNonce serverNonce) noexcept;

    /// Answers NeedsSession for a sid that was not. What happens next looks
    /// the same on the wire, by §5 rule two, and can never end in IsLive.
    void RouteUnknown(const crypto::Key& localSecret,
                      wire::HandshakeNonce serverNonce) noexcept;

    /// Does the next thing there is to do and says what it was. Call until it
    /// returns Idle; `out` needs CONNECTION_OUT_SIZE bytes and `scratch`
    /// CONNECTION_SCRATCH_SIZE.
    ///
    /// An error carries its disposition. KillSession has already been applied
    /// to the session by the time it is returned, and only an authenticated
    /// peer can cause one: everything before tag_c is capped at
    /// CloseConnection.
    [[nodiscard]] ConnectionResult Pump(Timestamp now, MutableByteSpan out,
                                        MutableByteSpan scratch) noexcept;

    /// The socket is gone. Detaches the session if this connection still
    /// carries it.
    void Close(Timestamp now) noexcept;

    /// Whether this connection carries the session — live and not superseded
    /// — and has sent it SessionEnd. A client closes its end once it has
    /// that, so a close seen when this is true is the client saying it holds
    /// the whole stream; a close of any other connection says nothing.
    [[nodiscard]] bool DeliveredEnd() const noexcept;

private:
    enum class EStage : std::uint8_t {
        AwaitHello,
        AwaitRoute,
        Handshake,
        AwaitResume,
        Live,
    };

    [[nodiscard]] ConnectionResult PumpHello() noexcept;
    [[nodiscard]] ConnectionResult PumpHandshake(MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult PumpResume(Timestamp now,
                                              MutableByteSpan out,
                                              MutableByteSpan scratch) noexcept;
    [[nodiscard]] ConnectionResult Refuse(SessionError error) noexcept;
    /// Hands the session's SessionEnd, if any, to the live channel.
    void QueueEnd() noexcept;

    EStage Stage_{EStage::AwaitHello};
    /// Hello is copied out of the frame buffer: the caller routes it between
    /// two Pumps, and the frame stream reclaims its bytes on the next call.
    std::array<std::byte, wire::MAX_HANDSHAKE_FRAME> Hello_{};
    std::size_t HelloSize_{0};
    wire::SessionId HelloId_{};
    bool HelloAnswered_{false};
    std::optional<ResumeAttempt> Attempt_;
    std::optional<SessionHandle> Handle_;
    std::optional<DirectionFrames> Current_;
    std::optional<DirectionFrames> Fallback_;
};

/// The client's end.
class ClientConnection : public ConnectionBase {
public:
    /// `frameStorage` must hold CONNECTION_FRAME_STORAGE bytes.
    ClientConnection(ClientSession& session, wire::HandshakeNonce clientNonce,
                     MutableByteSpan frameStorage, Timestamp now) noexcept;

    /// As AgentConnection::Pump. The first call writes Hello.
    [[nodiscard]] ConnectionResult Pump(Timestamp now, MutableByteSpan out,
                                        MutableByteSpan scratch) noexcept;

    /// The terminal's size, sent once live and again on every change. Kept
    /// until then: §7 has the client send it first after Resume.
    void QueueResize(wire::ResizeMessage size) noexcept;

    /// As AgentConnection::Close.
    void Close(Timestamp now) noexcept;

private:
    enum class EStage : std::uint8_t {
        SendHello,
        Handshake,
        Live,
    };

    [[nodiscard]] ConnectionResult PumpHandshake(Timestamp now,
                                                 MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult SendResume(Timestamp now,
                                              MutableByteSpan out) noexcept;
    [[nodiscard]] ConnectionResult Refuse(SessionError error) noexcept;

    ClientSession& Session_;
    EStage Stage_{EStage::SendHello};
    ClientResumeAttempt Attempt_;
    std::optional<ClientSessionHandle> Handle_;
    std::optional<wire::ResizeMessage> PendingResize_;
};

}  // namespace zet::session
