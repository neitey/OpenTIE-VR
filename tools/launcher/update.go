package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const (
	defaultUpdateAPI = "https://api.github.com"
	releasesPath     = "/repos/neitey/OpenTIE-VR/releases"
	launcherExeName  = "OpenTIE-VR-Launcher.exe"
)

var errNoRelease = errors.New("no release found")

type ghAsset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
	Size int64  `json:"size"`
}

type ghRelease struct {
	TagName     string    `json:"tag_name"`
	Name        string    `json:"name"`
	Body        string    `json:"body"`
	Draft       bool      `json:"draft"`
	Prerelease  bool      `json:"prerelease"`
	PublishedAt string    `json:"published_at"`
	Assets      []ghAsset `json:"assets"`
}

func (r *ghRelease) version() string {
	return strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(r.TagName), "v"), "V")
}

// releaseDate turns "2026-10-18T12:00:00Z" into "18 Oct 2026".
func (r *ghRelease) releaseDate() string {
	t, err := time.Parse(time.RFC3339, r.PublishedAt)
	if err != nil {
		return r.PublishedAt
	}
	return t.Format("2 Jan 2006")
}

func updateAPIBase() string {
	if b := os.Getenv("OPENTIE_UPDATE_API"); b != "" {
		return strings.TrimRight(b, "/")
	}
	return defaultUpdateAPI
}

// getJSON fetches a GitHub API URL into v (8 s limit, proxy from environment).
func getJSON(url string, v any) error {
	client := &http.Client{Timeout: 8 * time.Second}
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", userAgent())
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return errNoRelease
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("GitHub answered %s", resp.Status)
	}
	return json.NewDecoder(io.LimitReader(resp.Body, 4<<20)).Decode(v)
}

// fetchNewestRelease returns the newest stable release, or with includeTest
// the highest version among the last 10 non-draft releases.
func fetchNewestRelease(base string, includeTest bool) (*ghRelease, error) {
	if !includeTest {
		var r ghRelease
		if err := getJSON(base+releasesPath+"/latest", &r); err != nil {
			return nil, err
		}
		if r.Draft || r.Prerelease || parseVersion(r.version()).pre {
			return nil, errNoRelease
		}
		return &r, nil
	}
	var list []ghRelease
	if err := getJSON(base+releasesPath+"?per_page=10", &list); err != nil {
		return nil, err
	}
	var best *ghRelease
	for i := range list {
		r := &list[i]
		if r.Draft {
			continue
		}
		if best == nil || compareVersions(r.version(), best.version()) > 0 {
			best = r
		}
	}
	if best == nil {
		return nil, errNoRelease
	}
	return best, nil
}

// checkForUpdates is the update check at start. It returns true when a new
// launcher was opened and this one should exit.
func (a *app) checkForUpdates() bool {
	s := loadSettings(filepath.Join(a.exeDir, settingsFileName))
	if !s.getBool("check_updates", true) {
		logf("update check disabled in %s", settingsFileName)
		return false
	}
	say("Checking for a new launcher version...")
	rel, err := fetchNewestRelease(updateAPIBase(), s.getBool("include_test_versions", false))
	if errors.Is(err, errNoRelease) {
		say("The launcher is up to date.")
		return false
	}
	if err != nil {
		logf("update check failed: %v", err)
		say("Could not check for updates (offline?)")
		return false
	}
	newVer := rel.version()
	logf("newest release %q, this launcher %q", newVer, version)
	if compareVersions(newVer, version) <= 0 {
		say("The launcher is up to date.")
		return false
	}
	if skip := s.get("skip_version", ""); skip != "" && strings.EqualFold(strings.TrimPrefix(skip, "v"), newVer) {
		say("Version %s is available, but you chose to skip it.", newVer)
		return false
	}

	notes := cleanNotes(rel.Body)
	text := fmt.Sprintf("Released %s. What's new:", rel.releaseDate())
	if notes == "" {
		text = fmt.Sprintf("Released %s.", rel.releaseDate())
	}
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: fmt.Sprintf("New version %s available", newVer),
		Text:  text,
		Notes: notes,
		Options: []Option{
			{ID: "download", Label: "Download now", Key: "d", Style: "primary"},
			{ID: "later", Label: "Later", Key: "l"},
			{ID: "skip", Label: "Skip this version", Key: "s"},
		},
		Default: "later",
	})
	switch ans.ID {
	case "download":
		return a.downloadUpdate(rel)
	case "skip":
		if err := s.set("skip_version", newVer); err != nil {
			say("Could not save the setting: %v", err)
		} else {
			say("Version %s will not be offered again.", newVer)
		}
	default:
		say("OK, maybe later.")
	}
	return false
}

// cleanNotes makes Markdown release notes readable as plain text: heading
// marks and bold/code marks are removed.
func cleanNotes(body string) string {
	var out []string
	for _, l := range strings.Split(strings.ReplaceAll(body, "\r\n", "\n"), "\n") {
		t := strings.TrimLeft(l, "#")
		if t != l {
			l = strings.TrimSpace(t)
		}
		l = strings.NewReplacer("**", "", "`", "").Replace(l)
		out = append(out, l)
	}
	return strings.TrimSpace(strings.Join(out, "\n"))
}

// parseSHA256Sums reads "<hex>  <filename>" lines (sha256sum format, with an
// optional '*' before binary file names) into filename -> lower-case hex.
func parseSHA256Sums(text string) map[string]string {
	sums := map[string]string{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		i := strings.IndexAny(line, " \t")
		if i != 64 {
			continue
		}
		h := strings.ToLower(line[:i])
		if _, err := hex.DecodeString(h); err != nil {
			continue
		}
		name := strings.TrimPrefix(strings.TrimSpace(line[i:]), "*")
		if name != "" {
			sums[path.Base(strings.ReplaceAll(name, "\\", "/"))] = h
		}
	}
	return sums
}

func fileSHA256(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// downloadsDir is the user's Downloads folder, or the launcher's own folder.
func downloadsDir(fallback string) string {
	home := os.Getenv("USERPROFILE")
	if runtime.GOOS != "windows" || home == "" {
		home, _ = os.UserHomeDir()
	}
	if home != "" {
		d := filepath.Join(home, "Downloads")
		if isDir(d) {
			return d
		}
	}
	return fallback
}

func findReleaseAssets(rel *ghRelease) (zipAsset, sums *ghAsset) {
	for i := range rel.Assets {
		as := &rel.Assets[i]
		l := strings.ToLower(as.Name)
		if ok, _ := path.Match("opentie-vr-*.zip", l); ok && zipAsset == nil {
			zipAsset = as
		}
		if l == "sha256sums.txt" {
			sums = as
		}
	}
	return zipAsset, sums
}

// downloadUpdate downloads, checks and unpacks the release zip, then offers
// to open the new launcher. Returns true when this launcher should exit.
func (a *app) downloadUpdate(rel *ghRelease) bool {
	zipAsset, sumsAsset := findReleaseAssets(rel)
	if zipAsset == nil {
		say("This release has no download for the launcher yet. Please try again later.")
		return false
	}
	dlDir := downloadsDir(a.exeDir)
	zipPath := filepath.Join(dlDir, filepath.Base(zipAsset.Name))
	say("Downloading %s to %s", zipAsset.Name, dlDir)
	if err := downloadFile(zipAsset.URL, zipPath, "Downloading"); err != nil {
		if errors.Is(err, errClosed) {
			return false
		}
		logf("update download failed: %v", err)
		say("The download failed (%v). Continuing with this version.", err)
		return false
	}

	// Check the SHA-256 checksum from SHA256SUMS.txt of the same release.
	expected := ""
	if sumsAsset != nil {
		sumsPath := zipPath + ".sha256sums.tmp"
		if err := downloadFile(sumsAsset.URL, sumsPath, "Downloading checksums"); err == nil {
			data, _ := os.ReadFile(sumsPath)
			expected = parseSHA256Sums(string(data))[filepath.Base(zipAsset.Name)]
		} else {
			logf("SHA256SUMS download failed: %v", err)
		}
		os.Remove(sumsPath)
	}
	if expected == "" {
		say("Warning: the download could not be checked (no checksum found in the release).")
	} else {
		got, err := fileSHA256(zipPath)
		if err != nil || got != expected {
			logf("checksum mismatch: got %s, want %s (%v)", got, expected, err)
			os.Remove(zipPath)
			say("The download was damaged (its checksum did not match), so it was deleted.")
			say("Please try again later. Continuing with this version.")
			return false
		}
		say("Download checked (SHA-256 OK).")
	}

	base := strings.TrimSuffix(zipPath, filepath.Ext(zipPath))
	dest := base
	for i := 2; isDir(dest) || isFile(dest); i++ {
		dest = fmt.Sprintf("%s (%d)", base, i)
	}
	if _, err := extractZip(zipPath, dest); err != nil {
		logf("extract failed: %v", err)
		say("Could not unpack the download (%v). Continuing with this version.", err)
		return false
	}
	say("The new version is in: %s", dest)

	if !askYesNo("Download complete", "Open the new version now?", true) {
		say("OK. You can start it later from that folder. Continuing with this version.")
		return false
	}
	exe := findFileNamed(dest, launcherExeName, 2)
	if runtime.GOOS == "windows" && exe != "" {
		if err := startLauncher(exe); err == nil {
			logf("started %s", exe)
			a.skipPause = true
			return true
		} else {
			logf("could not start %s: %v", exe, err)
		}
	}
	if exe != "" {
		say("Start the new launcher from here: %s", exe)
	} else {
		say("Open this folder and start %s: %s", launcherExeName, dest)
	}
	return true
}

// findFileNamed looks for name (any case) in dir and up to depth levels below.
func findFileNamed(dir, name string, depth int) string {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	for _, e := range entries {
		if !e.IsDir() && strings.EqualFold(e.Name(), name) {
			return filepath.Join(dir, e.Name())
		}
	}
	if depth > 0 {
		for _, e := range entries {
			if e.IsDir() {
				if p := findFileNamed(filepath.Join(dir, e.Name()), name, depth-1); p != "" {
					return p
				}
			}
		}
	}
	return ""
}
