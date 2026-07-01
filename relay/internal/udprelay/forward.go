// Package udprelay is the UDP datagram forwarder that pairs with the TCP
// signaling server. One process, two listeners — TCP handles handshake +
// signaling + small messages; UDP handles the video stream.
//
// Wire format (every datagram going to or from the relay):
//
//	[4 bytes session_id, big-endian uint32]  ── opaque, assigned by the TCP
//	                                            signaling handshake
//	[1 byte type]                            ── 1 = VIDEO_FRAG, 2 = REGISTER
//	[ remaining payload ... ]
//
// REGISTER (type=2): the client tells the relay "I'm one half of session N,
// remember my UDP endpoint so the partner's traffic can reach me". Sent
// repeatedly (every ~5s) so NAT mappings don't expire.
//
// VIDEO_FRAG (type=1): forwarded blindly to the other half of the session.
// The relay never inspects the inner H.264 fragment header.
package udprelay

import (
	"encoding/binary"
	"log"
	"net"
	"sync"
	"time"
)

const (
	pktTypeVideoFrag byte = 0x01
	pktTypeRegister  byte = 0x02

	registerExpiry = 30 * time.Second
	headerSize     = 5 // 4-byte session id + 1-byte type
)

// Server runs the UDP datagram forwarder. One per process, listens on a single
// fixed port and keys all sessions by an opaque 32-bit id assigned by the TCP
// signaling layer.
type Server struct {
	conn *net.UDPConn

	// Per-session endpoint pair. Two peers register against the same id; the
	// forwarder swaps each packet to the partner endpoint of the sender.
	mu       sync.RWMutex
	sessions map[uint32]*pair
}

type pair struct {
	endpoints map[string]*net.UDPAddr // key = "ip:port" stringified
	lastSeen  map[string]time.Time
}

func New() *Server { return &Server{sessions: map[uint32]*pair{}} }

// Allocate reserves a session_id for an upcoming UDP channel. Called from the
// TCP signaling layer once it has matched a viewer to an agent.
func (s *Server) Allocate(id uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.sessions[id]; !exists {
		s.sessions[id] = &pair{
			endpoints: map[string]*net.UDPAddr{},
			lastSeen:  map[string]time.Time{},
		}
	}
}

// Release drops a session's state. Called from TCP signaling on session end.
func (s *Server) Release(id uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.sessions, id)
}

// ListenAndServe blocks reading datagrams on addr (e.g. ":7101"). Spawns a
// background janitor that times out endpoints that haven't re-registered in
// registerExpiry.
func (s *Server) ListenAndServe(addr string) error {
	laddr, err := net.ResolveUDPAddr("udp4", addr)
	if err != nil {
		return err
	}
	s.conn, err = net.ListenUDP("udp4", laddr)
	if err != nil {
		return err
	}
	log.Printf("udprelay listening on %s", addr)

	// Generous read buffer so a burst of video fragments while we're between
	// ReadFromUDP calls doesn't get dropped by the kernel.
	_ = s.conn.SetReadBuffer(8 * 1024 * 1024)
	_ = s.conn.SetWriteBuffer(8 * 1024 * 1024)

	go s.janitor()

	buf := make([]byte, 64*1024)
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			return err
		}
		if n < headerSize {
			continue
		}
		id := binary.BigEndian.Uint32(buf[0:4])
		typ := buf[4]
		// Copy the slice because we re-use buf next iteration and forward
		// may outlive this scope (the WriteToUDP below is synchronous, so
		// today it's actually fine — but cheap insurance for future).
		payload := append([]byte(nil), buf[:n]...)
		s.handle(id, typ, src, payload)
	}
}

func (s *Server) handle(id uint32, typ byte, src *net.UDPAddr, pkt []byte) {
	s.mu.RLock()
	p := s.sessions[id]
	s.mu.RUnlock()
	if p == nil {
		// Either the session was never allocated by TCP signaling, or the
		// signaling already tore it down. Drop silently — registering an
		// unknown session would let anyone with a guessable id punch holes
		// in our forwarder.
		return
	}

	key := src.String()

	switch typ {
	case pktTypeRegister:
		// Just refresh the endpoint mapping. No payload to forward.
		s.mu.Lock()
		p.endpoints[key] = src
		p.lastSeen[key] = time.Now()
		s.mu.Unlock()
		return

	case pktTypeVideoFrag:
		// Update lastSeen (the sender is alive) and forward to every OTHER
		// endpoint registered against this session. The peer-count is
		// typically 2 — one agent, one viewer — so "every other endpoint"
		// is at most one destination. Loops over a map of size <= 2.
		s.mu.Lock()
		p.endpoints[key] = src
		p.lastSeen[key] = time.Now()
		targets := make([]*net.UDPAddr, 0, len(p.endpoints))
		for k, ep := range p.endpoints {
			if k != key {
				targets = append(targets, ep)
			}
		}
		s.mu.Unlock()
		for _, t := range targets {
			_, _ = s.conn.WriteToUDP(pkt, t)
		}
		return

	default:
		// Unknown type — drop.
		return
	}
}

// janitor evicts endpoints that haven't sent a REGISTER or VIDEO_FRAG packet
// recently. Without this, a client whose NAT mapping changes (or whose laptop
// closed) would be remembered forever, and stray packets bound for the dead
// peer would keep getting forwarded into the void.
func (s *Server) janitor() {
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for range t.C {
		now := time.Now()
		s.mu.Lock()
		for _, p := range s.sessions {
			for k, last := range p.lastSeen {
				if now.Sub(last) > registerExpiry {
					delete(p.endpoints, k)
					delete(p.lastSeen, k)
				}
			}
		}
		s.mu.Unlock()
	}
}

// Close shuts down the listener. Returns the error from the underlying close.
func (s *Server) Close() error {
	if s.conn == nil {
		return nil
	}
	return s.conn.Close()
}
