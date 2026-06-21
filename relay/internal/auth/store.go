// Package auth is the MVP credential & permission store. It reads a JSON
// config file containing device tokens and per-account allowed device lists.
//
// For >dozen devices this is fine; replace with a database when the operator
// surface (admin UI, audit trail, key rotation) actually requires it.
package auth

import (
	"encoding/json"
	"errors"
	"os"
	"sync"
)

type Config struct {
	// When true, any (device_id, token) pair is accepted on REGISTER even if
	// the device isn't pre-configured below. The pair is cached in memory
	// for the lifetime of the process, and CONNECT to that device id is
	// allowed for any authenticated user.
	//
	// This is the RustDesk-style model: every client generates its own
	// 9-digit id locally, registers with the relay, and the operator never
	// has to touch the config file when a new machine joins. Security comes
	// from the ids being long enough not to guess plus the verifyCode the
	// client UI displays (TODO: actually enforce verifyCode at CONNECT).
	OpenRegistration bool              `json:"open_registration"`
	Devices          map[string]string `json:"devices"`
	Users            map[string]User   `json:"users"`
}

type User struct {
	SessionToken   string   `json:"session_token"`
	AllowedDevices []string `json:"allowed_devices"`
}

type Store struct {
	mu             sync.RWMutex
	cfg            Config
	dynamicDevices map[string]string // device_id -> token, populated by VerifyDevice in open mode
}

var (
	ErrUnknownDevice   = errors.New("unknown device id")
	ErrBadDeviceToken  = errors.New("invalid device token")
	ErrUnknownAccount  = errors.New("unknown account")
	ErrBadSessionToken = errors.New("invalid session token")
	ErrForbidden       = errors.New("account is not allowed to access this device")
)

func LoadFromFile(path string) (*Store, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, err
	}
	if cfg.Devices == nil {
		cfg.Devices = map[string]string{}
	}
	if cfg.Users == nil {
		cfg.Users = map[string]User{}
	}
	return &Store{
		cfg:            cfg,
		dynamicDevices: map[string]string{},
	}, nil
}

func (s *Store) VerifyDevice(id, token string) error {
	// Write lock — we may insert into dynamicDevices below.
	s.mu.Lock()
	defer s.mu.Unlock()

	if id == "" {
		return ErrUnknownDevice
	}
	if expected, ok := s.cfg.Devices[id]; ok {
		if expected != token {
			return ErrBadDeviceToken
		}
		return nil
	}
	if s.cfg.OpenRegistration {
		// Cache so the matching CheckPermission knows this id is "alive".
		// Token isn't verified against anything — the security model in open
		// mode is "ids are long enough not to guess + viewer must know the
		// verifyCode shown in the owner's UI".
		s.dynamicDevices[id] = token
		return nil
	}
	return ErrUnknownDevice
}

func (s *Store) VerifyUser(account, sessionToken string) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	u, ok := s.cfg.Users[account]
	if !ok {
		return ErrUnknownAccount
	}
	if u.SessionToken != sessionToken {
		return ErrBadSessionToken
	}
	return nil
}

func (s *Store) CheckPermission(account, deviceID string) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	u, ok := s.cfg.Users[account]
	if !ok {
		return ErrUnknownAccount
	}
	for _, allowed := range u.AllowedDevices {
		if allowed == deviceID {
			return nil
		}
	}
	if s.cfg.OpenRegistration {
		if _, ok := s.dynamicDevices[deviceID]; ok {
			return nil
		}
	}
	return ErrForbidden
}
