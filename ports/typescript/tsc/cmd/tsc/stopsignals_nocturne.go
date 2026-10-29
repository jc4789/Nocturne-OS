package main

import "os"

// stopSignals end a compilation or server early. Nocturne stops an app
// outright (AppStop) and delivers no termination signal.
var stopSignals = []os.Signal{os.Interrupt}
