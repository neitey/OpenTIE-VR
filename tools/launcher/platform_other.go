//go:build !windows

package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os/exec"
)

func isHiddenSystemFile(fs.FileInfo) bool { return false }

func windowsSteamRoots() []string               { return nil }
func windowsRegistryGameDirs([]string) []string { return nil }

func noConsoleWindow(*exec.Cmd) {}
func setupConsole()             {}

// startLauncher is only supported on Windows (the release contains a Windows exe).
func startLauncher(string) error { return errors.New("not supported on this system") }

func showOpenProblem(url string) {
	fmt.Println("Open this address in your web browser: " + url)
}

// appBrowsers: on macOS and Linux the default browser is used.
func appBrowsers() []string { return nil }

func nativePickerAvailable() bool { return false }

func pickFolderNative(string) (string, bool, error) {
	return "", false, errors.New("no folder picker on this system")
}
