// Command fakeviewer is a stand-in for the real Viewer. It authenticates,
// connects to a target device, prints incoming VIDEO_FRAME metadata, and (if
// asked) sends a couple of synthetic MOUSE_MOVE frames so the relay's reverse
// direction is exercised too.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"time"

	"remote_desktop/relay/internal/protocol"
	"remote_desktop/relay/internal/safeconn"
)

func main() {
	addr := flag.String("relay", "127.0.0.1:7100", "relay address")
	account := flag.String("account", "alice", "account name")
	token := flag.String("session-token", "viewer-token-alice", "session token")
	deviceID := flag.String("device-id", "device-001", "target device id")
	duration := flag.Duration("run", 5*time.Second, "how long to stay connected")
	inputBurst := flag.Bool("input-burst", false,
		"on connect, send one MOUSE_MOVE and KEY_EVENT to exercise the agent's "+
			"input-injection path (will visibly move the cursor on the agent)")
	flag.Parse()

	nc, err := net.Dial("tcp", *addr)
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	conn := safeconn.New(nc)
	defer conn.Close()

	// AUTH
	if err := conn.WriteJSON(protocol.MsgAuth, protocol.AuthReq{
		Account: *account, SessionToken: *token,
	}); err != nil {
		log.Fatalf("send AUTH: %v", err)
	}
	if err := expectOK(conn, protocol.MsgAuthResp); err != nil {
		log.Fatalf("AUTH: %v", err)
	}
	log.Printf("authenticated as %q", *account)

	// CONNECT
	if err := conn.WriteJSON(protocol.MsgConnect, protocol.ConnectReq{DeviceID: *deviceID}); err != nil {
		log.Fatalf("send CONNECT: %v", err)
	}
	if err := expectOK(conn, protocol.MsgConnectResp); err != nil {
		log.Fatalf("CONNECT: %v", err)
	}
	log.Printf("session up with %q", *deviceID)

	// One-shot input burst: tests that agent's input-injection path validates
	// and dispatches a real-format MOUSE_MOVE + KEY_EVENT pair. Off by default
	// because it visibly moves the agent's mouse cursor.
	if *inputBurst {
		mouseMove := []byte{0x7F, 0xFF, 0x7F, 0xFF} // x=32767, y=32767 (screen centre)
		if err := conn.WriteFrame(&protocol.Frame{Type: protocol.MsgMouseMove, Payload: mouseMove}); err != nil {
			log.Fatalf("send MOUSE_MOVE: %v", err)
		}
		// VK_F13 (0x7C) — a key that almost no app maps, so smoke tests don't
		// trigger unintended actions on the agent machine.
		keyDown := []byte{0x00, 0x7C, 0x01, 0x00}
		keyUp := []byte{0x00, 0x7C, 0x00, 0x00}
		if err := conn.WriteFrame(&protocol.Frame{Type: protocol.MsgKeyEvent, Payload: keyDown}); err != nil {
			log.Fatalf("send KEY_EVENT down: %v", err)
		}
		if err := conn.WriteFrame(&protocol.Frame{Type: protocol.MsgKeyEvent, Payload: keyUp}); err != nil {
			log.Fatalf("send KEY_EVENT up: %v", err)
		}
		log.Print("input burst sent (MOUSE_MOVE centre + F13 down/up)")
	}

	// Foreground: read frames until duration expires.
	done := time.After(*duration)
	frameC := make(chan *protocol.Frame, 32)
	errC := make(chan error, 1)
	go func() {
		for {
			f, err := conn.ReadFrame()
			if err != nil {
				errC <- err
				return
			}
			frameC <- f
		}
	}()

	var videoCount int
	for {
		select {
		case <-done:
			log.Printf("done — received %d VIDEO_FRAME(s)", videoCount)
			return
		case err := <-errC:
			log.Fatalf("read: %v", err)
		case f := <-frameC:
			switch f.Type {
			case protocol.MsgVideoFrame:
				videoCount++
				if videoCount <= 3 || videoCount%30 == 0 {
					log.Printf("VIDEO_FRAME #%d (%d bytes): %s", videoCount, len(f.Payload), trim(f.Payload))
				}
			default:
				log.Printf("frame %s (%d bytes)", f.Type, len(f.Payload))
			}
		}
	}
}

func expectOK(c *safeconn.Conn, want protocol.MessageType) error {
	f, err := c.ReadFrame()
	if err != nil {
		return err
	}
	if f.Type != want {
		return fmt.Errorf("got %s, want %s", f.Type, want)
	}
	var r protocol.Response
	if err := protocol.DecodeJSON(f.Payload, &r); err != nil {
		return err
	}
	if !r.OK {
		return fmt.Errorf("rejected: %s", r.Message)
	}
	return nil
}

func trim(b []byte) string {
	if len(b) > 80 {
		return string(b[:80]) + "…"
	}
	return string(b)
}
