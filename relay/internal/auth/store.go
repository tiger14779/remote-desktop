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
	Devices map[string]string `json:"devices"`
	Users   map[string]User   `json:"users"`
}

type User struct {
	SessionToken   string   `json:"session_token"`
	AllowedDevices []string `json:"allowed_devices"`
}

type Store struct {
	mu  sync.RWMutex
	cfg Config
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
	return &Store{cfg: cfg}, nil
}

func (s *Store) VerifyDevice(id, token string) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	expected, ok := s.cfg.Devices[id]
	if !ok {
		return ErrUnknownDevice
	}
	if expected != token {
		return ErrBadDeviceToken
	}
	return nil
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
	return ErrForbidden
}
