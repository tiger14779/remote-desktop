// Package safeconn wraps net.Conn with a write mutex so multiple goroutines
// can write whole frames without interleaving. Reads are expected to come from
// a single goroutine, so no read mutex is provided.
package safeconn

import (
	"net"
	"sync"

	"remote_desktop/relay/internal/protocol"
)

type Conn struct {
	nc      net.Conn
	writeMu sync.Mutex
}

func New(nc net.Conn) *Conn { return &Conn{nc: nc} }

func (c *Conn) Net() net.Conn { return c.nc }

func (c *Conn) Close() error { return c.nc.Close() }

func (c *Conn) RemoteAddr() net.Addr { return c.nc.RemoteAddr() }

// WriteFrame serialises a frame atomically. Safe for concurrent callers.
func (c *Conn) WriteFrame(f *protocol.Frame) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return protocol.WriteFrame(c.nc, f)
}

// ReadFrame reads a frame. NOT safe for concurrent callers — designate one
// goroutine per connection as the reader.
func (c *Conn) ReadFrame() (*protocol.Frame, error) {
	return protocol.ReadFrame(c.nc)
}

// WriteJSON is a convenience for control responses.
func (c *Conn) WriteJSON(t protocol.MessageType, v any) error {
	payload, err := protocol.EncodeJSON(v)
	if err != nil {
		return err
	}
	return c.WriteFrame(&protocol.Frame{Type: t, Payload: payload})
}
