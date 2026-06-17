// Command fakeagent is a stand-in for the real Agent that exercises the relay
// without requiring DXGI capture or FFmpeg. It registers as a device, then
// when a session starts streams synthetic "VIDEO_FRAME" payloads at a fixed
// rate and prints any input frames it receives.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"sync/atomic"
	"time"

	"remote_desktop/relay/internal/protocol"
	"remote_desktop/relay/internal/safeconn"
)

func main() {
	addr := flag.String("relay", "127.0.0.1:7100", "relay address")
	deviceID := flag.String("device-id", "device-001", "device id")
	token := flag.String("device-token", "agent-token-001", "device token")
	fps := flag.Int("fps", 30, "fake video framerate while a session is active")
	flag.Parse()

	nc, err := net.Dial("tcp", *addr)
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	conn := safeconn.New(nc)
	defer conn.Close()

	if err := conn.WriteJSON(protocol.MsgRegister, protocol.RegisterReq{
		DeviceID:    *deviceID,
		DeviceToken: *token,
	}); err != nil {
		log.Fatalf("send REGISTER: %v", err)
	}
	resp, err := conn.ReadFrame()
	if err != nil {
		log.Fatalf("read REGISTER_RESP: %v", err)
	}
	if resp.Type != protocol.MsgRegisterResp {
		log.Fatalf("expected REGISTER_RESP, got %s", resp.Type)
	}
	var r protocol.Response
	_ = protocol.DecodeJSON(resp.Payload, &r)
	if !r.OK {
		log.Fatalf("register rejected: %s", r.Message)
	}
	log.Printf("registered as %q, waiting for session", *deviceID)

	var sessionActive atomic.Bool
	stopStream := make(chan struct{})

	// Streamer goroutine. Runs only while a session is active.
	go func() {
		ticker := time.NewTicker(time.Second / time.Duration(*fps))
		defer ticker.Stop()
		var seq uint32
		for {
			select {
			case <-ticker.C:
				if !sessionActive.Load() {
					continue
				}
				seq++
				payload := []byte(fmt.Sprintf("fake-video-frame seq=%d ts=%d", seq, time.Now().UnixMilli()))
				if err := conn.WriteFrame(&protocol.Frame{Type: protocol.MsgVideoFrame, Payload: payload}); err != nil {
					log.Printf("send video: %v", err)
					return
				}
			case <-stopStream:
				return
			}
		}
	}()

	for {
		f, err := conn.ReadFrame()
		if err != nil {
			log.Printf("read: %v", err)
			close(stopStream)
			return
		}
		switch f.Type {
		case protocol.MsgSessionStart:
			log.Print("SESSION_START received — streaming fake video")
			sessionActive.Store(true)
		case protocol.MsgSessionEnd:
			log.Print("SESSION_END received — pausing")
			sessionActive.Store(false)
		case protocol.MsgMouseMove, protocol.MsgMouseButton, protocol.MsgKeyEvent:
			log.Printf("input %s: %d bytes", f.Type, len(f.Payload))
		case protocol.MsgHeartbeat:
			// Relay reflects our heartbeats; nothing to do.
		default:
			log.Printf("unexpected frame %s (%d bytes)", f.Type, len(f.Payload))
		}
	}
}
