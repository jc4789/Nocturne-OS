//go:build !nocturne

package main

import (
	"os"
	"syscall"
)

// stopSignals end a compilation or server early.
var stopSignals = []os.Signal{os.Interrupt, syscall.SIGTERM}
