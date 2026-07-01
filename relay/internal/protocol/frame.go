// Package protocol defines the wire format used between Agent, Relay and Viewer.
//
// Frame layout:
//
//	[1 byte type] [4 byte big-endian payload length] [payload bytes]
//
// The relay never interprets payload bytes for data-plane frames (VIDEO_FRAME,
// MOUSE_*, KEY_EVENT, HEARTBEAT) — it only inspects type during the handshake
// (REGISTER, AUTH, CONNECT). Adding new frame types therefore requires no relay
// changes.
package protocol

import (
	"encoding/binary"
	"errors"
	"io"
)

type MessageType byte

const (
	// Handshake / control (text-ish, JSON payloads).
	MsgRegister     MessageType = 0x01 // Agent  -> Relay
	MsgAuth         MessageType = 0x02 // Viewer -> Relay
	MsgConnect      MessageType = 0x03 // Viewer -> Relay
	MsgSessionStart MessageType = 0x04 // Relay  -> Agent (a Viewer is now attached)
	MsgSessionEnd   MessageType = 0x05 // Relay  -> Agent (Viewer detached)

	// Data plane (binary payloads).
	MsgVideoFrame  MessageType = 0x10 // Agent  -> Viewer
	MsgMouseMove   MessageType = 0x20 // Viewer -> Agent
	MsgMouseButton MessageType = 0x21 // Viewer -> Agent
	MsgKeyEvent    MessageType = 0x22 // Viewer -> Agent
	MsgHeartbeat   MessageType = 0x30 // bidirectional, empty payload

	MsgClipboardText MessageType = 0x40 // bidirectional, payload: UTF-8 bytes
	MsgCursorShape   MessageType = 0x50 // Agent -> Viewer, payload: cursor BGRA + metadata

	MsgFileXferBegin MessageType = 0x41 // bidirectional, JSON file list
	MsgFileXferChunk MessageType = 0x42 // bidirectional, raw bytes
	MsgFileXferEnd   MessageType = 0x43 // bidirectional, JSON status

	// UDP data plane handshake. Sent from relay to both peers right after a
	// session is matched. Carries the relay-assigned session id + the
	// public UDP endpoint they should target. Clients then start sending
	// VIDEO_FRAG packets there; control-plane frames stay on TCP.
	MsgUdpInfo MessageType = 0x60 // relay -> peer, JSON {session_id, host, port}

	// Responses.
	MsgRegisterResp MessageType = 0x81 // Relay -> Agent
	MsgAuthResp     MessageType = 0x82 // Relay -> Viewer
	MsgConnectResp  MessageType = 0x83 // Relay -> Viewer
	MsgError        MessageType = 0xFE
)

// HeaderSize is the size of the fixed-length frame header in bytes.
const HeaderSize = 5

// MaxPayloadSize caps a single frame's payload. 16 MiB comfortably fits a
// keyframe at 4K, and rejects garbage that would otherwise let a peer allocate
// arbitrary memory.
const MaxPayloadSize = 16 * 1024 * 1024

var (
	ErrPayloadTooLarge = errors.New("frame payload exceeds maximum size")
)

type Frame struct {
	Type    MessageType
	Payload []byte
}

func ReadFrame(r io.Reader) (*Frame, error) {
	var header [HeaderSize]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header[1:5])
	if length > MaxPayloadSize {
		return nil, ErrPayloadTooLarge
	}
	payload := make([]byte, length)
	if length > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return nil, err
		}
	}
	return &Frame{Type: MessageType(header[0]), Payload: payload}, nil
}

func WriteFrame(w io.Writer, f *Frame) error {
	if len(f.Payload) > MaxPayloadSize {
		return ErrPayloadTooLarge
	}
	var header [HeaderSize]byte
	header[0] = byte(f.Type)
	binary.BigEndian.PutUint32(header[1:5], uint32(len(f.Payload)))
	if _, err := w.Write(header[:]); err != nil {
		return err
	}
	if len(f.Payload) > 0 {
		if _, err := w.Write(f.Payload); err != nil {
			return err
		}
	}
	return nil
}

func (t MessageType) String() string {
	switch t {
	case MsgRegister:
		return "REGISTER"
	case MsgAuth:
		return "AUTH"
	case MsgConnect:
		return "CONNECT"
	case MsgSessionStart:
		return "SESSION_START"
	case MsgSessionEnd:
		return "SESSION_END"
	case MsgVideoFrame:
		return "VIDEO_FRAME"
	case MsgMouseMove:
		return "MOUSE_MOVE"
	case MsgMouseButton:
		return "MOUSE_BUTTON"
	case MsgKeyEvent:
		return "KEY_EVENT"
	case MsgHeartbeat:
		return "HEARTBEAT"
	case MsgClipboardText:
		return "CLIPBOARD_TEXT"
	case MsgCursorShape:
		return "CURSOR_SHAPE"
	case MsgFileXferBegin:
		return "FILE_XFER_BEGIN"
	case MsgFileXferChunk:
		return "FILE_XFER_CHUNK"
	case MsgFileXferEnd:
		return "FILE_XFER_END"
	case MsgRegisterResp:
		return "REGISTER_RESP"
	case MsgAuthResp:
		return "AUTH_RESP"
	case MsgConnectResp:
		return "CONNECT_RESP"
	case MsgError:
		return "ERROR"
	default:
		return "UNKNOWN"
	}
}
