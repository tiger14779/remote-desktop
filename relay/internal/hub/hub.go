// Package hub tracks registered Agent devices and brokers Viewer sessions to
// them. For the MVP each device hosts at most one Viewer session at a time.
package hub

import (
	"errors"
	"sync"

	"remote_desktop/relay/internal/safeconn"
)

// Device represents an online Agent. It owns the underlying connection and the
// session-binding to a Viewer (if any).
type Device struct {
	ID   string
	Conn *safeconn.Conn

	mu     sync.Mutex
	viewer *safeconn.Conn // active viewer connection, nil if idle
}

func NewDevice(id string, conn *safeconn.Conn) *Device {
	return &Device{ID: id, Conn: conn}
}

// AttachViewer binds a viewer to this device. Returns false if a session is
// already in progress; the caller should reject the new viewer in that case.
func (d *Device) AttachViewer(v *safeconn.Conn) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.viewer != nil {
		return false
	}
	d.viewer = v
	return true
}

// DetachViewer clears the viewer binding. Idempotent.
func (d *Device) DetachViewer(v *safeconn.Conn) {
	d.mu.Lock()
	defer d.mu.Unlock()
	// Only clear if it matches — guards against a stale defer racing a new session.
	if d.viewer == v {
		d.viewer = nil
	}
}

// Viewer returns the currently bound viewer, or nil if idle.
func (d *Device) Viewer() *safeconn.Conn {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.viewer
}

// Hub indexes online devices by ID.
type Hub struct {
	mu      sync.RWMutex
	devices map[string]*Device
}

func New() *Hub { return &Hub{devices: map[string]*Device{}} }

var ErrDuplicateRegister = errors.New("device id already registered")

// Register adds the device. If another connection has already claimed this ID,
// the old one is evicted (its underlying conn is closed) — this matches user
// expectations after a network blip where the old TCP socket may still be
// half-open on the server side.
func (h *Hub) Register(d *Device) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if old, ok := h.devices[d.ID]; ok {
		_ = old.Conn.Close()
	}
	h.devices[d.ID] = d
}

// Unregister removes the device only if the recorded entry still points at the
// given connection — prevents a delayed Unregister from a dead session
// clobbering a freshly-reconnected device.
func (h *Hub) Unregister(id string, conn *safeconn.Conn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if d, ok := h.devices[id]; ok && d.Conn == conn {
		delete(h.devices, id)
	}
}

func (h *Hub) Lookup(id string) (*Device, bool) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	d, ok := h.devices[id]
	return d, ok
}

func (h *Hub) ListIDs() []string {
	h.mu.RLock()
	defer h.mu.RUnlock()
	ids := make([]string, 0, len(h.devices))
	for id := range h.devices {
		ids = append(ids, id)
	}
	return ids
}
