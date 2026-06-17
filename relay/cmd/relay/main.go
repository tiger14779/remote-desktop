// Command relay is the entry point for the Relay server.
package main

import (
	"flag"
	"log"

	"remote_desktop/relay/internal/auth"
	"remote_desktop/relay/internal/hub"
	"remote_desktop/relay/internal/server"
)

func main() {
	addr := flag.String("listen", ":7100", "TCP listen address")
	cfgPath := flag.String("config", "config.json", "path to auth config file")
	flag.Parse()

	store, err := auth.LoadFromFile(*cfgPath)
	if err != nil {
		log.Fatalf("load config %q: %v", *cfgPath, err)
	}

	srv := server.New(*addr, hub.New(), store)
	if err := srv.ListenAndServe(); err != nil {
		log.Fatalf("relay: %v", err)
	}
}
