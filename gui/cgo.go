package main

// cgo.go owns the global cgo directives (CFLAGS / LDFLAGS) for the package
// and includes the shared wrapper header. Other app_*.go files that need
// to call C.wrap_* must each have their own preamble `#include
// "cgo_wrappers.h"` — cgo parses files independently and a symbol
// declared only here is NOT visible to them.

/*
#cgo CFLAGS: -I${SRCDIR}/../driver
#cgo LDFLAGS: ${SRCDIR}/../driver/libpicoscope2204a.a -lusb-1.0 -lpthread -lm
#include "cgo_wrappers.h"
*/
import "C"
