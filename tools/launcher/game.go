package main

import (
	"fmt"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
)

// findCaseInsensitive resolves a slash-separated relative path below dir,
// matching each part without regard to case. Returns "" if not found.
func findCaseInsensitive(dir, rel string) string {
	cur := dir
	for _, part := range strings.Split(rel, "/") {
		exact := filepath.Join(cur, part)
		if _, err := os.Lstat(exact); err == nil {
			cur = exact
			continue
		}
		entries, err := os.ReadDir(cur)
		if err != nil {
			return ""
		}
		found := ""
		for _, e := range entries {
			if strings.EqualFold(e.Name(), part) {
				found = filepath.Join(cur, e.Name())
				break
			}
		}
		if found == "" {
			return ""
		}
		cur = found
	}
	return cur
}

func hasCI(dir, rel string) bool { return findCaseInsensitive(dir, rel) != "" }

// TIE Fighter editions. OpenTIE accepts a folder (see
// src/tie_app/setup/installation.c) with STRINGS.DAT, RESOURCE/TITLE.LFD,
// RESOURCE/MAINMENU.LFD, MISSION, RES320 and CP320; it is the 1998 edition
// (TIE98) when RESOURCE/MM640.LFD and IVFILES are there too, else the
// Collector's CD (TIE95), which may keep part of its data in a CD image
// (game.ins / game.gog / game.fil).
//
// Steam ("STAR WARS Tie Fighter"): remastered = TIE98, collectors_cd =
// TIE95 (its DOSBOX folder is skipped), classic = the 1994 floppy version,
// which OpenTIE does not use. GOG and CD installs are recognised by their
// contents in the same way.
var tieCommonFiles = []string{"STRINGS.DAT", "RESOURCE/TITLE.LFD", "RESOURCE/MAINMENU.LFD", "MISSION", "RES320", "CP320"}

var tieDiscImages = []string{"game.ins", "game.gog", "game.fil"}

// skippedEditionDir: folders that are never a usable edition (the floppy
// version, DOSBox).
func skippedEditionDir(name string) bool {
	l := strings.ToLower(name)
	return l == "classic" || l == "dosbox" || strings.HasPrefix(l, ".")
}

// classifyEdition returns "TIE98", "TIE95" or "" for a folder.
func classifyEdition(dir string) string {
	if !isDir(dir) || skippedEditionDir(filepath.Base(dir)) {
		return ""
	}
	disc := false
	for _, f := range tieDiscImages {
		if hasCI(dir, f) {
			disc = true
		}
	}
	common := true
	for _, f := range tieCommonFiles {
		if !hasCI(dir, f) {
			common = false
			break
		}
	}
	if common && hasCI(dir, "IVFILES") && hasCI(dir, "RESOURCE/MM640.LFD") {
		return tie98Folder
	}
	if hasCI(dir, "IVFILES") {
		return "" // an incomplete 1998 edition
	}
	// The floppy version has no 640x480 art (RES640) and no CD image.
	if (common && hasCI(dir, "RES640")) || (disc && hasCI(dir, "RESOURCE")) {
		return tie95Folder
	}
	return ""
}

// tieInstall is one TIE Fighter install: root is the folder found or chosen,
// tie98 and tie95 the edition folders in it ("" when missing).
type tieInstall struct {
	root, tie98, tie95 string
}

func (t tieInstall) ok() bool { return t.tie98 != "" || t.tie95 != "" }

// describe is "TIE Fighter 1998 edition and Collector's CD" etc.
func (t tieInstall) describe() string {
	switch {
	case t.tie98 != "" && t.tie95 != "":
		return "the 1998 edition and the Collector's CD (1995)"
	case t.tie98 != "":
		return "the 1998 edition"
	case t.tie95 != "":
		return "the Collector's CD (1995)"
	}
	return "nothing usable"
}

// findTieInstall looks at dir and up to depth folder levels below it for the
// two editions. A folder that is an edition itself is used alone.
func findTieInstall(dir string, depth int) tieInstall {
	t := tieInstall{root: dir}
	switch classifyEdition(dir) {
	case tie98Folder:
		t.tie98 = dir
		return t
	case tie95Folder:
		t.tie95 = dir
		return t
	}
	var walk func(d string, depth int)
	walk = func(d string, depth int) {
		if depth <= 0 {
			return
		}
		entries, err := os.ReadDir(d)
		if err != nil {
			return
		}
		for _, e := range entries {
			p := filepath.Join(d, e.Name())
			if !isDir(p) || skippedEditionDir(e.Name()) {
				continue
			}
			switch classifyEdition(p) {
			case tie98Folder:
				if t.tie98 == "" {
					t.tie98 = p
				}
			case tie95Folder:
				if t.tie95 == "" {
					t.tie95 = p
				}
			default:
				walk(p, depth-1)
			}
		}
	}
	walk(dir, depth)
	return t
}

// X-Wing Alliance: only its three TIE cockpit models are used. They go to
// files/XWA/FlightModels under these names (the app looks them up there).
var xwaCockpitNames = []string{"TIECOCKPIT.OPT", "TIEBOMBERCOCKPIT.OPT", "COMBATSIMCOCKPIT.OPT"}

// xwaCockpitFiles lists the cockpit models in an X-Wing Alliance folder.
func xwaCockpitFiles(dir string) []gameFile {
	var out []gameFile
	for _, n := range xwaCockpitNames {
		p := findCaseInsensitive(dir, "FLIGHTMODELS/"+n)
		if p == "" {
			continue
		}
		info, err := os.Stat(p)
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		out = append(out, gameFile{Rel: cockpitsFolder + "/" + n, Abs: p, Size: info.Size()})
	}
	return out
}

func isXWADir(dir string) bool { return len(xwaCockpitFiles(dir)) > 0 }

// findXWARoot looks for an X-Wing Alliance folder with the cockpit models at
// dir or up to depth levels below it.
func findXWARoot(dir string, depth int) string {
	if isXWADir(dir) {
		return dir
	}
	if depth <= 0 {
		return ""
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	for _, e := range entries {
		p := filepath.Join(dir, e.Name())
		if !isDir(p) || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		if r := findXWARoot(p, depth-1); r != "" {
			return r
		}
	}
	return ""
}

// parseVDFPaths returns the library folder paths in Steam's
// libraryfolders.vdf. New files have "path" keys; old ones list the extra
// libraries as "1" "D:\\SteamLibrary".
func parseVDFPaths(data string) []string {
	toks := vdfTokens(data)
	var paths []string
	for i := 0; i+1 < len(toks); i++ {
		k, v := toks[i], toks[i+1]
		if !k.quoted || !v.quoted {
			continue
		}
		key := strings.ToLower(k.text)
		_, numErr := strconv.Atoi(key)
		if key == "path" || (numErr == nil && looksLikePath(v.text)) {
			paths = append(paths, v.text)
			i++
		}
	}
	return paths
}

func looksLikePath(s string) bool {
	return strings.Contains(s, "/") || strings.Contains(s, "\\") || (len(s) >= 2 && s[1] == ':')
}

type vdfToken struct {
	text   string
	quoted bool
}

// vdfTokens splits Valve's KeyValues text into quoted strings and braces,
// handling \\ and \" escapes and // comments.
func vdfTokens(s string) []vdfToken {
	var toks []vdfToken
	for i := 0; i < len(s); {
		c := s[i]
		switch {
		case c == '"':
			var b strings.Builder
			i++
			for i < len(s) && s[i] != '"' {
				if s[i] == '\\' && i+1 < len(s) {
					switch s[i+1] {
					case '\\', '"':
						b.WriteByte(s[i+1])
					case 'n':
						b.WriteByte('\n')
					case 't':
						b.WriteByte('\t')
					default:
						b.WriteByte('\\')
						b.WriteByte(s[i+1])
					}
					i += 2
					continue
				}
				b.WriteByte(s[i])
				i++
			}
			i++ // closing quote
			toks = append(toks, vdfToken{text: b.String(), quoted: true})
		case c == '{' || c == '}':
			toks = append(toks, vdfToken{text: string(c)})
			i++
		case c == '/' && i+1 < len(s) && s[i+1] == '/':
			for i < len(s) && s[i] != '\n' {
				i++
			}
		default:
			i++
		}
	}
	return toks
}

// steamLibraries returns the Steam library folders below a Steam install.
func steamLibraries(steamRoot string) []string {
	libs := []string{steamRoot}
	for _, rel := range []string{"steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"} {
		data, err := os.ReadFile(filepath.Join(steamRoot, filepath.FromSlash(rel)))
		if err != nil {
			continue
		}
		libs = append(libs, parseVDFPaths(string(data))...)
	}
	return libs
}

// Folder name keywords: TIE Fighter installs and X-Wing Alliance installs.
var (
	tieKeywords = []string{"tie fighter", "tiefighter", "tie_fighter", "tie-fighter"}
	xwaKeywords = []string{"alliance"}
)

// steamGameDirs returns steamapps/common folders whose name has a keyword.
func steamGameDirs(steamRoots []string, keywords []string) []string {
	var out []string
	for _, root := range steamRoots {
		for _, lib := range steamLibraries(root) {
			out = append(out, globNamed(filepath.Join(lib, "steamapps", "common"), keywords)...)
		}
	}
	return out
}

// hasKeyword reports whether name contains one of the (lower case) keywords.
func hasKeyword(name string, keywords []string) bool {
	l := strings.ToLower(name)
	for _, k := range keywords {
		if strings.Contains(l, k) {
			return true
		}
	}
	return false
}

// globNamed lists the sub-folders of dir whose name contains a keyword
// (lower case).
func globNamed(dir string, keywords []string) []string {
	var out []string
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	for _, e := range entries {
		if hasKeyword(e.Name(), keywords) && isDir(filepath.Join(dir, e.Name())) {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

// subDirs lists all sub-folders of dir.
func subDirs(dir string) []string {
	var out []string
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	for _, e := range entries {
		if isDir(filepath.Join(dir, e.Name())) {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

// gameSearchDirs collects folders that may contain a game whose folder name
// has one of the keywords; each one is checked afterwards.
func gameSearchDirs(keywords []string) []string {
	var dirs []string
	home, _ := os.UserHomeDir()
	var steamRoots []string
	if runtime.GOOS == "windows" {
		steamRoots = append(steamRoots, windowsSteamRoots()...)
		dirs = append(dirs, windowsRegistryGameDirs(keywords)...)
		pf86 := os.Getenv("ProgramFiles(x86)")
		pf := os.Getenv("ProgramFiles")
		for _, p := range []string{pf86, pf} {
			if p == "" {
				continue
			}
			steamRoots = append(steamRoots, filepath.Join(p, "Steam"))
			dirs = append(dirs, globNamed(filepath.Join(p, "GOG Galaxy", "Games"), keywords)...)
			dirs = append(dirs, globNamed(filepath.Join(p, "GOG.com"), keywords)...)
			dirs = append(dirs, globNamed(filepath.Join(p, "LucasArts"), keywords)...)
		}
		for _, drive := range "CDEFGH" {
			d := string(drive) + `:\`
			dirs = append(dirs, globNamed(filepath.Join(d, "GOG Games"), keywords)...)
			dirs = append(dirs, globNamed(filepath.Join(d, "Games"), keywords)...)
		}
	} else if home != "" {
		steamRoots = append(steamRoots,
			filepath.Join(home, ".steam", "steam"),
			filepath.Join(home, ".local", "share", "Steam"),
			filepath.Join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
			filepath.Join(home, "Library", "Application Support", "Steam"),
		)
		dirs = append(dirs, globNamed(filepath.Join(home, "GOG Games"), keywords)...)
		dirs = append(dirs, globNamed(filepath.Join(home, "Games"), keywords)...)
	}
	dirs = append(dirs, steamGameDirs(steamRoots, keywords)...)
	return dirs
}

// pathKey makes paths comparable for de-duplication.
func pathKey(p string) string {
	if r, err := filepath.EvalSymlinks(p); err == nil {
		p = r
	}
	if abs, err := filepath.Abs(p); err == nil {
		p = abs
	}
	p = filepath.Clean(p)
	if runtime.GOOS == "windows" || runtime.GOOS == "darwin" {
		p = strings.ToLower(p)
	}
	return p
}

// detectGames returns the TIE Fighter installs found on this computer.
func detectGames() []tieInstall {
	seen := map[string]bool{}
	var out []tieInstall
	for _, d := range gameSearchDirs(tieKeywords) {
		t := findTieInstall(d, 2)
		if !t.ok() || seen[pathKey(t.root)] {
			continue
		}
		seen[pathKey(t.root)] = true
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].root < out[j].root })
	return out
}

// detectXWA returns the X-Wing Alliance folders with the cockpit models;
// those with all three come first.
func detectXWA() []string {
	seen := map[string]bool{}
	var out []string
	for _, d := range gameSearchDirs(xwaKeywords) {
		r := findXWARoot(d, 2)
		if r == "" || seen[pathKey(r)] {
			continue
		}
		seen[pathKey(r)] = true
		out = append(out, r)
	}
	sort.SliceStable(out, func(i, j int) bool {
		ni, nj := len(xwaCockpitFiles(out[i])), len(xwaCockpitFiles(out[j]))
		if ni != nj {
			return ni > nj
		}
		return out[i] < out[j]
	})
	return out
}

// cleanUserPath removes quotes and spaces around a typed or pasted path.
func cleanUserPath(s string) string {
	s = strings.TrimSpace(s)
	for len(s) >= 2 && ((s[0] == '"' && s[len(s)-1] == '"') || (s[0] == '\'' && s[len(s)-1] == '\'')) {
		s = strings.TrimSpace(s[1 : len(s)-1])
	}
	s = strings.Trim(s, `"`)
	if strings.HasPrefix(s, "~/") || s == "~" {
		if home, err := os.UserHomeDir(); err == nil {
			s = filepath.Join(home, s[1:])
		}
	}
	if s != "" {
		s = filepath.Clean(s)
	}
	return s
}

// tryUserFolder checks a folder named by the user. It returns the install,
// or a problem text saying why the folder is not usable.
func tryUserFolder(p string) (t tieInstall, problem string) {
	if p == "" {
		return t, "Please choose a folder."
	}
	if !isDir(p) {
		return t, "That folder does not exist: " + p
	}
	t = findTieInstall(p, 2)
	if t.ok() {
		return t, ""
	}
	return t, "No TIE Fighter game files were found in: " + p + "\n" +
		"Choose the TIE Fighter folder (on Steam \"STAR WARS Tie Fighter\", which has\n" +
		"the remastered and collectors_cd folders), or a folder with STRINGS.DAT and\n" +
		"the RESOURCE, MISSION, RES320 and CP320 folders."
}

func folderQuestion(text, value, problem string) Question {
	prompt := "Folder:"
	if value != "" {
		prompt = "Press Enter to use it, or type another folder path:"
	}
	return Question{
		Kind:          "text",
		Title:         "Where is TIE Fighter?",
		Text:          text,
		Value:         value,
		Problem:       problem,
		Placeholder:   `for example C:\Program Files (x86)\Steam\steamapps\common\STAR WARS Tie Fighter`,
		Browse:        true,
		Options:       []Option{{ID: "ok", Label: "Use this folder", Style: "primary"}},
		ConsolePrompt: prompt,
	}
}

func (a *app) setGame(t tieInstall) error {
	a.gameDir = t.root
	a.game = t
	say("TIE Fighter folder: %s", t.root)
	if t.tie98 != "" {
		say("  1998 edition: %s", t.tie98)
	}
	if t.tie95 != "" {
		say("  Collector's CD (1995): %s", t.tie95)
	}
	logf("TIE install: %+v", t)
	return nil
}

func noFolderChosen() error {
	if appCtx.Err() != nil {
		return errClosed
	}
	return fail("No game folder was chosen.")
}

// chooseGame is step 2: use the folder from the command line, or find the
// game automatically and let the user confirm or choose.
func (a *app) chooseGame(arg string) error {
	problem := ""
	if arg != "" {
		t, prob := tryUserFolder(cleanUserPath(arg))
		if t.ok() {
			return a.setGame(t)
		}
		problem = prob
	}

	text := "Choose your TIE Fighter folder (TIE Fighter Special Edition from Steam or GOG)."
	value := ""
	if arg == "" {
		say("Looking for TIE Fighter on this computer...")
		found := detectGames()
		logf("detected TIE Fighter installs: %+v", found)
		switch {
		case len(found) == 1:
			text, value = "Found TIE Fighter ("+found[0].describe()+") here:", found[0].root
		case len(found) > 1:
			var opts []Option
			for i, v := range found {
				opts = append(opts, Option{ID: fmt.Sprint(i), Label: v.root, Key: fmt.Sprint(i + 1)})
			}
			opts = append(opts, Option{ID: "other", Label: "Another folder...", Key: "o"})
			for {
				ans := ui.Ask(Question{
					Kind:      "buttons",
					Title:     "Which TIE Fighter folder?",
					Text:      "Found TIE Fighter in several places. Choose the one to use:",
					Problem:   problem,
					Options:   opts,
					AllowText: true,
				})
				if ans.Closed {
					return noFolderChosen()
				}
				if ans.ID == "other" {
					break
				}
				if ans.ID == "text" {
					t, prob := tryUserFolder(cleanUserPath(ans.Text))
					if t.ok() {
						return a.setGame(t)
					}
					problem = prob
					continue
				}
				var n int
				fmt.Sscan(ans.ID, &n)
				if n >= 0 && n < len(found) {
					return a.setGame(found[n])
				}
			}
		default:
			text = "TIE Fighter was not found automatically.\n" + text
		}
	}
	if runtime.GOOS == "windows" {
		if _, console := ui.(*consoleUI); console {
			text += "\nTip: in Explorer, hold Shift, right-click the folder and choose \"Copy as path\"."
		}
	}

	for {
		ans := ui.Ask(folderQuestion(text, value, problem))
		if ans.Closed {
			return noFolderChosen()
		}
		t, prob := tryUserFolder(cleanUserPath(ans.Text))
		if t.ok() {
			return a.setGame(t)
		}
		say("%s", prob)
		problem = prob
		value = ""
		if _, console := ui.(*consoleUI); console {
			problem = "" // already printed
		}
	}
}

const noXWAText = "Without X-Wing Alliance OpenTIE-VR still has 3D cockpits for every TIE craft:\n" +
	"the X-Wing Revival cockpits that come with the app."

func (a *app) setXWA(dir string) error {
	a.xwaDir = dir
	a.cockpits = xwaCockpitFiles(dir)
	var names []string
	for _, f := range a.cockpits {
		names = append(names, path.Base(f.Rel))
	}
	say("X-Wing Alliance folder: %s", dir)
	say("Cockpit models to copy: %s", strings.Join(names, ", "))
	if len(a.cockpits) < len(xwaCockpitNames) {
		say("Some cockpit models are missing there; those crafts use the included X-Wing Revival cockpits.")
	}
	return nil
}

func (a *app) noXWA() error {
	a.xwaDir, a.cockpits = "", nil
	say("No X-Wing Alliance cockpits. %s", noXWAText)
	return nil
}

// chooseXWA is step 3: X-Wing Alliance is optional. Only its three TIE
// cockpit models (FlightModels/TIECOCKPIT.OPT, TIEBOMBERCOCKPIT.OPT,
// COMBATSIMCOCKPIT.OPT) are copied; the app then uses them instead of the
// X-Wing Revival cockpits it brings along.
func (a *app) chooseXWA() error {
	if env := os.Getenv("OPENTIE_XWA"); env != "" { // tests and power users
		if env == "none" {
			return a.noXWA()
		}
		if r := findXWARoot(cleanUserPath(env), 2); r != "" {
			return a.setXWA(r)
		}
	}
	say("Looking for X-Wing Alliance (optional, for its own 3D cockpits)...")
	found := detectXWA()
	logf("detected X-Wing Alliance folders: %q", found)
	if len(found) > 0 {
		return a.setXWA(found[0])
	}
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "X-Wing Alliance was not found",
		Text: "If you own X-Wing Alliance (Steam or GOG), OpenTIE-VR can use its three TIE\n" +
			"cockpit models for the 3D cockpits (optional). " + noXWAText + "\n\n" +
			"If X-Wing Alliance is installed, choose its folder.",
		Options: []Option{
			{ID: "skip", Label: "Continue without", Key: "", Style: "primary"},
			{ID: "choose", Label: "Choose the X-Wing Alliance folder", Key: "c"},
		},
		Default:       "skip",
		ConsolePrompt: "Press Enter to continue without it, or c to choose its folder:",
	})
	if ans.Closed && appCtx.Err() != nil {
		return errClosed
	}
	if ans.ID != "choose" {
		return a.noXWA()
	}
	problem := ""
	for {
		ans := ui.Ask(Question{
			Kind:        "text",
			Title:       "Where is X-Wing Alliance?",
			Text:        "Choose the X-Wing Alliance folder (the one with the FlightModels folder).",
			Problem:     problem,
			Placeholder: `for example C:\GOG Games\Star Wars - X-Wing Alliance`,
			Browse:      true,
			Options: []Option{
				{ID: "ok", Label: "Use this folder", Style: "primary"},
				{ID: "skip", Label: "Continue without"},
			},
			Default:       "skip",
			ConsolePrompt: "Folder (or \"-\" to continue without):",
		})
		if ans.Closed {
			if appCtx.Err() != nil {
				return errClosed
			}
			return a.noXWA()
		}
		p := cleanUserPath(ans.Text)
		if ans.ID == "skip" || p == "-" || strings.EqualFold(p, "none") {
			return a.noXWA()
		}
		if r := findXWARoot(p, 2); r != "" {
			return a.setXWA(r)
		}
		problem = "The X-Wing Alliance cockpit models (FlightModels\\TIECOCKPIT.OPT) are not in: " + p
		say("%s", problem)
		if _, console := ui.(*consoleUI); console {
			problem = ""
		}
	}
}
