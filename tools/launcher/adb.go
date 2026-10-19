package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

// ADB runs Google's adb tool. Once a headset is chosen, Serial is passed as
// "-s <serial>" to every command.
type ADB struct {
	Path   string
	Serial string
	ctx    context.Context // nil: appCtx (stopped when the launcher stops)
}

// run executes adb with the given arguments and returns stdout+stderr
// (with \r\n turned into \n). Every call is logged with its output.
// timeout 0 means no limit (used for long pushes and installs). The command
// is stopped when the launcher stops (window closed).
func (a *ADB) run(timeout time.Duration, args ...string) (string, error) {
	return a.exec(timeout, true, args...)
}

// runQuiet is run without logging a successful call (for polling).
func (a *ADB) runQuiet(timeout time.Duration, args ...string) (string, error) {
	return a.exec(timeout, false, args...)
}

// stream is run with stdin feeding adb's standard input (for exec-in).
func (a *ADB) stream(timeout time.Duration, stdin io.Reader, args ...string) (string, error) {
	return a.execIn(timeout, true, stdin, args...)
}

func (a *ADB) exec(timeout time.Duration, logOK bool, args ...string) (string, error) {
	return a.execIn(timeout, logOK, nil, args...)
}

func (a *ADB) execIn(timeout time.Duration, logOK bool, stdin io.Reader, args ...string) (string, error) {
	full := args
	if a.Serial != "" {
		full = append([]string{"-s", a.Serial}, args...)
	}
	base := appCtx
	if a.ctx != nil {
		base = a.ctx
	}
	ctx := base
	if timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, timeout)
		defer cancel()
	}
	cmd := exec.CommandContext(ctx, a.Path, full...)
	noConsoleWindow(cmd)
	var buf bytes.Buffer
	cmd.Stdin = stdin
	cmd.Stdout = &buf
	cmd.Stderr = &buf
	// adb may start its background server, which can inherit our output
	// pipes and keep them open forever. WaitDelay stops waiting for the
	// pipes shortly after the adb client itself has exited.
	cmd.WaitDelay = 3 * time.Second
	start := time.Now()
	err := cmd.Run()
	if errors.Is(err, exec.ErrWaitDelay) {
		err = nil
	}
	switch {
	case base.Err() != nil:
		err = errClosed
	case ctx.Err() == context.DeadlineExceeded:
		err = fmt.Errorf("adb did not answer within %s", timeout)
	}
	out := strings.ReplaceAll(buf.String(), "\r\n", "\n")
	status := "ok"
	if err != nil {
		status = err.Error()
	}
	if logOK || err != nil {
		logf("$ adb %s  [%s, %.2fs]\n%s", strings.Join(quoteForLog(full), " "), status, time.Since(start).Seconds(), out)
	}
	return out, err
}

// shell runs one command line in the headset's shell. The caller must quote
// paths with shQuote.
func (a *ADB) shell(timeout time.Duration, command string) (string, error) {
	return a.run(timeout, "shell", command)
}

func quoteForLog(args []string) []string {
	out := make([]string, len(args))
	for i, s := range args {
		if s == "" || strings.ContainsAny(s, " \t'\"") {
			out[i] = fmt.Sprintf("%q", s)
		} else {
			out[i] = s
		}
	}
	return out
}

// shQuote quotes s for the headset's sh: 'it'\”s'.
func shQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

func adbExeName() string {
	if runtime.GOOS == "windows" {
		return "adb.exe"
	}
	return "adb"
}

// ownToolsDirs are the places where the launcher itself puts a downloaded
// platform-tools folder: next to the exe, or (if that is read-only) in the
// user's cache folder.
func ownToolsDirs(exeDir string) []string {
	dirs := []string{exeDir}
	if c, err := os.UserCacheDir(); err == nil {
		dirs = append(dirs, filepath.Join(c, "OpenTIE-VR"))
	}
	return dirs
}

// adbCandidates lists the places where adb is commonly installed, in order
// of preference.
func adbCandidates(exeDir string) []string {
	exe := adbExeName()
	var c []string
	for _, d := range ownToolsDirs(exeDir) {
		c = append(c, filepath.Join(d, "platform-tools", exe))
	}
	if p, err := exec.LookPath("adb"); err == nil {
		c = append(c, p)
	}
	home, _ := os.UserHomeDir()
	switch runtime.GOOS {
	case "windows":
		local := os.Getenv("LOCALAPPDATA")
		roaming := os.Getenv("APPDATA")
		pf := os.Getenv("ProgramFiles")
		pf86 := os.Getenv("ProgramFiles(x86)")
		if local != "" {
			c = append(c,
				filepath.Join(local, "Android", "Sdk", "platform-tools", exe),
				filepath.Join(local, "Programs", "Meta Quest Developer Hub", "resources", "bin", exe),
				filepath.Join(local, "Programs", "Oculus Developer Hub", "resources", "bin", exe),
			)
		}
		if roaming != "" {
			c = append(c, filepath.Join(roaming, "SideQuest", "platform-tools", exe))
		}
		for _, p := range []string{pf, pf86} {
			if p == "" {
				continue
			}
			c = append(c,
				filepath.Join(p, "Meta Quest Developer Hub", "resources", "bin", exe),
				filepath.Join(p, "Oculus Developer Hub", "resources", "bin", exe),
				filepath.Join(p, "Android", "android-sdk", "platform-tools", exe),
			)
		}
		if home != "" {
			c = append(c, filepath.Join(home, "AppData", "Local", "Android", "Sdk", "platform-tools", exe))
		}
	case "darwin":
		if home != "" {
			c = append(c,
				filepath.Join(home, "Library", "Android", "sdk", "platform-tools", exe),
				filepath.Join(home, "Library", "Application Support", "SideQuest", "platform-tools", exe),
			)
		}
		c = append(c,
			"/Applications/Meta Quest Developer Hub.app/Contents/Resources/bin/adb",
			"/opt/homebrew/bin/adb",
			"/usr/local/bin/adb",
		)
	default:
		if home != "" {
			c = append(c,
				filepath.Join(home, "Android", "Sdk", "platform-tools", exe),
				filepath.Join(home, ".config", "SideQuest", "platform-tools", exe),
			)
		}
		c = append(c, "/usr/bin/adb")
	}
	return c
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.Mode().IsRegular()
}

func isDir(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

// setupADB finds adb (or downloads it after the user accepts Google's licence).
func (a *app) setupADB() error {
	path := ""
	if env := os.Getenv("OPENTIE_ADB"); env != "" {
		if !isFile(env) {
			return fail("OPENTIE_ADB is set to %q, but that file does not exist.", env)
		}
		path = env
	} else {
		for _, c := range adbCandidates(a.exeDir) {
			if isFile(c) {
				path = c
				break
			}
		}
	}
	if path == "" {
		logf("adb not found in: %s", strings.Join(adbCandidates(a.exeDir), "; "))
		p, err := a.downloadPlatformTools()
		if err != nil {
			return err
		}
		path = p
	}
	a.adb = &ADB{Path: path}
	out, err := a.adb.run(30*time.Second, "version")
	if err != nil {
		return fail("adb was found at %s but does not work:\n%s", path, strings.TrimSpace(out))
	}
	say("Using adb: %s", path)
	return nil
}

func platformToolsURL() string {
	if u := os.Getenv("OPENTIE_PLATFORM_TOOLS_URL"); u != "" { // for tests
		return u
	}
	osName := runtime.GOOS
	if osName != "windows" && osName != "darwin" {
		osName = "linux"
	}
	return "https://dl.google.com/android/repository/platform-tools-latest-" + osName + ".zip"
}

// downloadPlatformTools asks for the licence, downloads Google's
// platform-tools zip and unpacks it into <exe dir>/platform-tools.
func (a *app) downloadPlatformTools() (string, error) {
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "Google's adb tool is needed",
		Text: "OpenTIE-VR needs Google's free \"adb\" tool (Android Platform Tools) to talk\n" +
			"to the headset. It is not on this computer yet, so it can be downloaded\n" +
			"now from Google (a few MB).\n\n" +
			"Downloading it means you accept the Android SDK License Agreement:",
		Link: "https://developer.android.com/studio/terms",
		Options: []Option{
			{ID: "yes", Label: "I accept, download adb", Key: "yes", Style: "primary"},
			{ID: "no", Label: "Cancel", Key: "no"},
		},
		Default:       "no",
		ConsolePrompt: "Type yes to accept and download:",
	})
	if ans.Closed && appCtx.Err() != nil {
		return "", errClosed
	}
	if ans.ID != "yes" {
		return "", fail("adb was not downloaded, so the launcher cannot continue.\n" +
			"You can also install SideQuest or Meta Quest Developer Hub (they include adb)\n" +
			"and then try again.")
	}

	var lastErr error
	for _, dir := range ownToolsDirs(a.exeDir) {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			lastErr = err
			continue
		}
		zipPath := filepath.Join(dir, "platform-tools-download.zip")
		say("Downloading adb...")
		if err := downloadFile(platformToolsURL(), zipPath, "Downloading adb"); err != nil {
			if errors.Is(err, errClosed) {
				return "", err
			}
			os.Remove(zipPath)
			lastErr = err
			if errors.Is(err, errNotWritable) {
				continue
			}
			break
		}
		_, err := extractZip(zipPath, dir)
		os.Remove(zipPath)
		if err != nil {
			lastErr = err
			continue
		}
		adb := filepath.Join(dir, "platform-tools", adbExeName())
		if !isFile(adb) {
			lastErr = fmt.Errorf("the download did not contain %s", adbExeName())
			break
		}
		say("adb is ready.")
		return adb, nil
	}
	logf("platform-tools download failed: %v", lastErr)
	return "", fail("Could not download adb: %v\nCheck your internet connection and try again.", lastErr)
}
