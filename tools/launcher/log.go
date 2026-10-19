package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const logFileName = "opentie-vr-launcher.log"

var logState struct {
	mu   sync.Mutex
	f    *os.File
	path string
}

// openLog opens opentie-vr-launcher.log next to the exe (or in the temp folder
// if that is not writable) for appending. A log over 5 MB is moved to
// opentie-vr-launcher.old.log first. Returns the log path ("" if none).
func openLog(dir string) string {
	for _, d := range []string{dir, os.TempDir()} {
		p := filepath.Join(d, logFileName)
		if st, err := os.Stat(p); err == nil && st.Size() > 5<<20 {
			_ = os.Rename(p, filepath.Join(d, "opentie-vr-launcher.old.log"))
		}
		f, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
		if err != nil {
			continue
		}
		logState.mu.Lock()
		logState.f = f
		logState.path = p
		logState.mu.Unlock()
		fmt.Fprintf(f, "\n==================== %s ====================\n", time.Now().Format(time.RFC3339))
		return p
	}
	return ""
}

func logPath() string {
	logState.mu.Lock()
	defer logState.mu.Unlock()
	return logState.path
}

func closeLog() {
	logState.mu.Lock()
	defer logState.mu.Unlock()
	if logState.f != nil {
		logState.f.Close()
		logState.f = nil
	}
}

// logf writes one timestamped entry; continuation lines are indented.
func logf(format string, args ...any) {
	logState.mu.Lock()
	defer logState.mu.Unlock()
	if logState.f == nil {
		return
	}
	msg := fmt.Sprintf(format, args...)
	msg = strings.TrimRight(msg, "\n")
	msg = strings.ReplaceAll(msg, "\n", "\n    ")
	fmt.Fprintf(logState.f, "%s %s\n", time.Now().Format("2006-01-02 15:04:05.000"), msg)
}
