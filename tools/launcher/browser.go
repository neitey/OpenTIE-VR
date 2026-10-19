package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// openURL opens a web link in the user's normal browser.
func openURL(u string) error { return openPath(u) }

// openPath opens a folder, file or link with the system's default program.
func openPath(p string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		if isDir(p) {
			cmd = exec.Command("explorer.exe", p)
		} else {
			cmd = exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", p)
		}
	case "darwin":
		cmd = exec.Command("open", p)
	default:
		cmd = exec.Command("xdg-open", p)
	}
	noConsoleWindow(cmd)
	if err := cmd.Start(); err != nil {
		return err
	}
	go cmd.Wait() // explorer.exe exits with 1 even on success; ignore
	return nil
}

// folderPickerAvailable: Windows has a native picker. OPENTIE_FAKE_PICKER
// (tests only) makes the picker return that path on any system.
func folderPickerAvailable() bool {
	return os.Getenv("OPENTIE_FAKE_PICKER") != "" || nativePickerAvailable()
}

// pickFolder shows a folder picker; ok is false when the user cancelled.
func pickFolder(initial string) (string, bool, error) {
	if p := os.Getenv("OPENTIE_FAKE_PICKER"); p != "" {
		return p, true, nil
	}
	return pickFolderNative(initial)
}

// browserProfileDir is a separate profile for the app window, so it does not
// mix with the user's own browser session.
func browserProfileDir(exeDir string) string {
	if d := os.Getenv("LOCALAPPDATA"); d != "" && runtime.GOOS == "windows" {
		return filepath.Join(d, "OpenTIE-VR-Launcher", "edge")
	}
	if d, err := os.UserCacheDir(); err == nil {
		return filepath.Join(d, "OpenTIE-VR-Launcher", "browser")
	}
	return filepath.Join(exeDir, "OpenTIE-VR-Launcher", "browser")
}

// appWindowArgs are the arguments for Edge/Chrome in app mode. Background
// throttling is turned off so a minimized window keeps talking to us.
func appWindowArgs(url, profile string) []string {
	return []string{
		"--app=" + url,
		"--window-size=760,600",
		"--user-data-dir=" + profile,
		"--no-first-run",
		"--no-default-browser-check",
		"--disable-background-timer-throttling",
		"--disable-renderer-backgrounding",
		"--disable-backgrounding-occluded-windows",
	}
}

// openAppWindow opens url as an app window: on Windows Edge or Chrome in app
// mode, otherwise (or if neither exists) the default browser.
func openAppWindow(url, exeDir string) error {
	profile := browserProfileDir(exeDir)
	for _, b := range appBrowsers() {
		if !isFile(b) {
			continue
		}
		_ = os.MkdirAll(profile, 0o755)
		cmd := exec.Command(b, appWindowArgs(url, profile)...)
		if err := cmd.Start(); err != nil {
			logf("could not start %s: %v", b, err)
			continue
		}
		logf("opened the launcher window with %s (profile %s)", b, profile)
		go cmd.Wait()
		return nil
	}
	logf("no Edge/Chrome found for an app window; using the default browser")
	return openURL(url)
}
