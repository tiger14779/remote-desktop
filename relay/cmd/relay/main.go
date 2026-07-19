// Command relay is the entry point for the Relay server.
package main

import (
	"flag"
	"io"
	"log"
	"net"
	"strconv"

	"remote_desktop/relay/internal/auth"
	"remote_desktop/relay/internal/hub"
	"remote_desktop/relay/internal/server"
	"remote_desktop/relay/internal/udprelay"
)

func main() {
	addr := flag.String("listen", ":7100", "TCP listen address")
	udpAddr := flag.String("udp", ":7101", "UDP listen address (video data plane)")
	cfgPath := flag.String("config", "config.json", "path to auth config file")
	// Silence all metadata logging (connection open/close, session pairs,
	// register/auth events, etc). Only fatal errors reach stderr in this
	// mode. Rationale: operators who don't want session metadata
	// preserved anywhere (journald, log shipping, cloud provider log
	// ingestion) can flip this on. Startup + fatal errors still fire so
	// the systemd unit can still detect misconfiguration.
	quiet := flag.Bool("quiet", false, "suppress all session metadata logging")
	flag.Parse()

	// Load config BEFORE silencing — a bad config path should still fail
	// loud so systemd can mark the service failed with a visible message.
	store, err := auth.LoadFromFile(*cfgPath)
	if err != nil {
		log.Fatalf("load config %q: %v", *cfgPath, err)
	}

	if *quiet {
		// Silence per-session metadata (connection open/close, session
		// pairs, register/auth events). Runtime fatal errors also go
		// silent as a side-effect — that's an acceptable tradeoff for
		// operators who don't want any metadata trail; they can drop
		// -quiet to debug.
		log.SetOutput(io.Discard)
	}

	// Pull the bare port out of ":7101" so we can tell clients which UDP
	// port to dial via the MsgUdpInfo handshake.
	udpPort := 7101
	if _, p, err := net.SplitHostPort(*udpAddr); err == nil && p != "" {
		if n, err := strconv.Atoi(p); err == nil {
			udpPort = n
		}
	}

	// UDP forwarder runs in its own goroutine. TCP signaling layer reaches
	// into it to Allocate/Release session ids — both live in the same
	// process for that reason.
	udpFwd := udprelay.New()
	go func() {
		if err := udpFwd.ListenAndServe(*udpAddr); err != nil {
			log.Fatalf("udprelay: %v", err)
		}
	}()

	srv := server.New(*addr, hub.New(), store, udpFwd, udpPort)
	if err := srv.ListenAndServe(); err != nil {
		log.Fatalf("relay: %v", err)
	}
}
