$version: "2.0"

namespace example.chat

use alloy#simpleRestJson

/// Full-duplex chat fixture (ADR-0016; PLAN §Phase 8's exit criterion): the
/// generated client and generated server talk event streams over real
/// WebSockets in CI. One service carries a bidirectional operation, a
/// server-push one, and a unary neighbor, proving unary routing and streaming
/// upgrades coexist on one generated service (and one port).
@simpleRestJson
@title("Chat Service")
service Chat {
    version: "2026-07-19"
    operations: [ListRooms, Converse, Watch]
}

/// Unary neighbor: an ordinary request/response on the same service, served
/// by the same transport that upgrades the streaming operations.
@readonly
@http(method: "GET", uri: "/rooms")
operation ListRooms {
    output := {
        @required
        rooms: RoomSummaries
    }
}

list RoomSummaries {
    member: RoomSummary
}

structure RoomSummary {
    @required
    name: String

    @required
    members: Integer
}

/// Bidirectional: the client streams ChatEvents up and receives RoomEvents
/// down over one WebSocket session. Initial-request members ride the upgrade
/// GET (the room label and the nickname header). Kicked is the mid-stream
/// modeled error: a handler returning it ends the stream with a typed
/// exception message before the close (exceptions travel via the operation's
/// errors list, not as event union members — ADR-0016's wire binding).
@http(method: "POST", uri: "/rooms/{room}/converse")
operation Converse {
    input := {
        @required
        @httpLabel
        room: String

        @httpHeader("x-chat-nickname")
        nickname: String

        @httpPayload
        events: ChatEvents
    }
    output := {
        @httpPayload
        events: RoomEvents
    }
    errors: [Kicked]
}

/// Server-push: no input stream, so the client's transmit direction is the
/// runtime's NoEvents — the client only listens to the room.
@readonly
@http(method: "GET", uri: "/rooms/{room}/watch")
operation Watch {
    input := {
        @required
        @httpLabel
        room: String
    }
    output := {
        @httpPayload
        events: RoomEvents
    }
}

/// What a client sends: messages, and a goodbye that asks the server to end
/// the session from its side.
@streaming
union ChatEvents {
    message: ChatMessage
    leave: LeaveNotice
}

/// What the room sends back: broadcast messages and membership changes.
@streaming
union RoomEvents {
    message: ChatMessage
    joined: MemberJoined
    left: MemberLeft
}

structure ChatMessage {
    /// Bounded, so a client's oversized message is refused as one event
    /// (ADR-0025) instead of reaching the handler.
    @required
    @length(max: 280)
    text: String

    sender: String
}

structure LeaveNotice {
    reason: String
}

structure MemberJoined {
    @required
    member: String
}

structure MemberLeft {
    @required
    member: String
}

/// The moderator ended the session mid-stream. Surfaces on the client as the
/// unary error shape: kind kModeled, code "Kicked", typed detail attached.
@error("client")
@httpError(403)
structure Kicked {
    message: String

    @required
    by: String
}
