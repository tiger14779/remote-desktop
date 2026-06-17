package protocol

import "encoding/json"

// Handshake message payloads. All are JSON to keep the relay debuggable; the
// data-plane frames (VIDEO_FRAME, MOUSE_*, KEY_EVENT) stay binary.

type RegisterReq struct {
	DeviceID    string `json:"device_id"`
	DeviceToken string `json:"device_token"`
}

type AuthReq struct {
	Account      string `json:"account"`
	SessionToken string `json:"session_token"`
}

type ConnectReq struct {
	DeviceID string `json:"device_id"`
}

type Response struct {
	OK      bool   `json:"ok"`
	Message string `json:"message,omitempty"`
}

func EncodeJSON(v any) ([]byte, error)        { return json.Marshal(v) }
func DecodeJSON(data []byte, v any) error     { return json.Unmarshal(data, v) }
