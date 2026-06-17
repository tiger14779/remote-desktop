// Package server is the TCP relay. It accepts both Agent and Viewer
// connections on the same port and demultiplexes by inspecting the first frame
// (REGISTER → Agent path, AUTH → Viewer path).
//
// Forwarding model
//
// Each Agent has one dedicated reader goroutine. While the device is idle, the
// reader handles heartbeats and drops everything else. When a Viewer attaches,
// the same reader forwards every Agent frame to the bound Viewer.
//
// Each Viewer has its own reader goroutine that forwards Viewer frames to the
// Agent. The two directions are independent — neither holds the connection
// mutex of the side it writes to for longer than one frame (safeconn
// serialises writes).
package server

import (
	"errors"
	"io"
	"log"
	"net"
	"time"

	"remote_desktop/relay/internal/auth"
	"remote_desktop/relay/internal/hub"
	"remote_desktop/relay/internal/protocol"
	"remote_desktop/relay/internal/safeconn"
)

const handshakeTimeout = 10 * time.Second

type Server struct {
	addr  string
	hub   *hub.Hub
	store *auth.Store
}

func New(addr string, h *hub.Hub, a *auth.Store) *Server {
	return &Server{addr: addr, hub: h, store: a}
}

func (s *Server) ListenAndServe() error {
	ln, err := net.Listen("tcp", s.addr)
	if err != nil {
		return err
	}
	log.Printf("relay listening on %s", s.addr)
	for {
		nc, err := ln.Accept()
		if err != nil {
			return err
		}
		go s.handleConn(nc)
	}
}

func (s *Server) handleConn(nc net.Conn) {
	conn := safeconn.New(nc)
	addr := conn.RemoteAddr()
	log.Printf("[%s] connection opened", addr)
	defer log.Printf("[%s] connection closed", addr)
	defer conn.Close()

	if err := nc.SetReadDeadline(time.Now().Add(handshakeTimeout)); err != nil {
		return
	}
	first, err := conn.ReadFrame()
	if err != nil {
		log.Printf("[%s] read first frame: %v", addr, err)
		return
	}
	_ = nc.SetReadDeadline(time.Time{})

	switch first.Type {
	case protocol.MsgRegister:
		s.handleAgent(conn, first)
	case protocol.MsgAuth:
		s.handleViewer(conn, first)
	default:
		log.Printf("[%s] unexpected first frame %s", addr, first.Type)
		_ = conn.WriteJSON(protocol.MsgError, protocol.Response{Message: "first frame must be REGISTER or AUTH"})
	}
}

func (s *Server) handleAgent(conn *safeconn.Conn, first *protocol.Frame) {
	addr := conn.RemoteAddr()

	var req protocol.RegisterReq
	if err := protocol.DecodeJSON(first.Payload, &req); err != nil {
		_ = conn.WriteJSON(protocol.MsgRegisterResp, protocol.Response{Message: "bad REGISTER payload"})
		return
	}
	if err := s.store.VerifyDevice(req.DeviceID, req.DeviceToken); err != nil {
		log.Printf("[%s] register fail %q: %v", addr, req.DeviceID, err)
		_ = conn.WriteJSON(protocol.MsgRegisterResp, protocol.Response{Message: "authentication failed"})
		return
	}

	dev := hub.NewDevice(req.DeviceID, conn)
	s.hub.Register(dev)
	defer s.hub.Unregister(dev.ID, conn)

	if err := conn.WriteJSON(protocol.MsgRegisterResp, protocol.Response{OK: true}); err != nil {
		log.Printf("[%s] send REGISTER_RESP: %v", addr, err)
		return
	}
	log.Printf("[%s] agent registered: %s", addr, dev.ID)

	s.agentReadLoop(dev)
}

// agentReadLoop is the only reader on the Agent connection. When idle it
// handles heartbeats; when a viewer is attached it forwards every frame.
func (s *Server) agentReadLoop(dev *hub.Device) {
	addr := dev.Conn.RemoteAddr()
	for {
		f, err := dev.Conn.ReadFrame()
		if err != nil {
			if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				log.Printf("[%s] agent read: %v", addr, err)
			}
			// Drag the viewer down with us so its loop unblocks.
			if v := dev.Viewer(); v != nil {
				_ = v.Close()
			}
			return
		}

		if v := dev.Viewer(); v != nil {
			if err := v.WriteFrame(f); err != nil {
				log.Printf("[%s] forward to viewer: %v", addr, err)
				_ = v.Close()
				// Don't DetachViewer here — the viewer's own goroutine owns that on exit.
			}
			continue
		}

		// Idle. Reflect heartbeats so the agent can confirm liveness.
		if f.Type == protocol.MsgHeartbeat {
			_ = dev.Conn.WriteFrame(&protocol.Frame{Type: protocol.MsgHeartbeat})
		}
	}
}

func (s *Server) handleViewer(conn *safeconn.Conn, first *protocol.Frame) {
	addr := conn.RemoteAddr()

	var authReq protocol.AuthReq
	if err := protocol.DecodeJSON(first.Payload, &authReq); err != nil {
		_ = conn.WriteJSON(protocol.MsgAuthResp, protocol.Response{Message: "bad AUTH payload"})
		return
	}
	if err := s.store.VerifyUser(authReq.Account, authReq.SessionToken); err != nil {
		log.Printf("[%s] auth fail %q: %v", addr, authReq.Account, err)
		_ = conn.WriteJSON(protocol.MsgAuthResp, protocol.Response{Message: "authentication failed"})
		return
	}
	if err := conn.WriteJSON(protocol.MsgAuthResp, protocol.Response{OK: true}); err != nil {
		return
	}

	_ = conn.Net().SetReadDeadline(time.Now().Add(handshakeTimeout))
	connFrame, err := conn.ReadFrame()
	if err != nil {
		log.Printf("[%s] read CONNECT: %v", addr, err)
		return
	}
	_ = conn.Net().SetReadDeadline(time.Time{})

	if connFrame.Type != protocol.MsgConnect {
		_ = conn.WriteJSON(protocol.MsgError, protocol.Response{Message: "expected CONNECT after AUTH"})
		return
	}
	var connReq protocol.ConnectReq
	if err := protocol.DecodeJSON(connFrame.Payload, &connReq); err != nil {
		_ = conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{Message: "bad CONNECT payload"})
		return
	}

	if err := s.store.CheckPermission(authReq.Account, connReq.DeviceID); err != nil {
		log.Printf("[%s] permission denied %q -> %q: %v", addr, authReq.Account, connReq.DeviceID, err)
		_ = conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{Message: "permission denied"})
		return
	}

	dev, ok := s.hub.Lookup(connReq.DeviceID)
	if !ok {
		_ = conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{Message: "device offline"})
		return
	}
	if !dev.AttachViewer(conn) {
		_ = conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{Message: "device busy"})
		return
	}
	defer func() {
		dev.DetachViewer(conn)
		// Tell the agent the session ended so it can stop capturing/encoding.
		// Best-effort — if the agent is already gone, the write fails silently.
		_ = dev.Conn.WriteFrame(&protocol.Frame{Type: protocol.MsgSessionEnd})
	}()

	if err := dev.Conn.WriteFrame(&protocol.Frame{Type: protocol.MsgSessionStart}); err != nil {
		log.Printf("[%s] send SESSION_START to agent: %v", addr, err)
		_ = conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{Message: "agent unreachable"})
		return
	}
	if err := conn.WriteJSON(protocol.MsgConnectResp, protocol.Response{OK: true}); err != nil {
		return
	}
	log.Printf("[%s] session start: %s <-> %s", addr, authReq.Account, dev.ID)
	defer log.Printf("[%s] session end:   %s <-> %s", addr, authReq.Account, dev.ID)

	for {
		f, err := conn.ReadFrame()
		if err != nil {
			if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				log.Printf("[%s] viewer read: %v", addr, err)
			}
			return
		}
		if err := dev.Conn.WriteFrame(f); err != nil {
			log.Printf("[%s] forward to agent: %v", addr, err)
			return
		}
	}
}
