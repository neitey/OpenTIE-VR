package main

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestCompareVersions(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"1.0.0", "1.0.0", 0},
		{"v1.0.1", "1.0.0", 1},
		{"1.0.0", "1.0.0-rc3", 1},
		{"1.0.0-rc3", "1.0.0-rc2", 1},
		{"1.0.0-rc10", "1.0.0-rc9", 1},
		{"1.0.0-rc3", "1.0.0", -1},
		{"1.1", "1.0.9", 1},
		{"2.0.0-rc1", "1.9.9", 1},
		{"dev", "0.0.1", -1},
		{"dev", "0.0.0", 0},
		{"1.0.0", "dev", 1},
		{"1.0.0+build5", "1.0.0", 0},
	}
	for _, c := range cases {
		if got := compareVersions(c.a, c.b); got != c.want {
			t.Errorf("compareVersions(%q, %q) = %d, want %d", c.a, c.b, got, c.want)
		}
	}
}

func TestParseVDFPaths(t *testing.T) {
	newFmt := `"libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"   // comment
	}
}`
	got := parseVDFPaths(newFmt)
	want := []string{`C:\Program Files (x86)\Steam`, `D:\SteamLibrary`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("new format: got %q want %q", got, want)
	}
	oldFmt := `"LibraryFolders"
{
	"TimeNextStatsReport"		"1561832478"
	"ContentStatsID"		"-158337411836"
	"1"		"E:\\Games\\Steam"
}`
	got = parseVDFPaths(oldFmt)
	want = []string{`E:\Games\Steam`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("old format: got %q want %q", got, want)
	}
}

func TestSkipFileName(t *testing.T) {
	skip := []string{"TIE95.EXE", "ddraw.dll", "TOPACE10.TFR", "luke.tfr", "game.LNK", "goggame-1234.info", "unins000.dat",
		"steam_api.dll", "steam_api64.dll", "desktop.ini", "Thumbs.db", ".DS_Store", "x.tmp", "DOSBOX_ICO.ico", "site.url",
		"dosbox_tiecd.conf", "Manual.pdf", "TieFighterCompatibility.sdb"}
	keep := []string{"STRINGS.DAT", "TITLE.LFD", "game.ins", "game.fil", "Track02.ogg", "readme.txt", "exefile.dat", "tfr.txt",
		"ICONS640.ICO", "MAPICONS.ICO"}
	for _, n := range skip {
		if !skipFileName(n) {
			t.Errorf("%q should be skipped", n)
		}
	}
	for _, n := range keep {
		if skipFileName(n) {
			t.Errorf("%q should be kept", n)
		}
	}
}

func TestParseRemoteList(t *testing.T) {
	out := "12|./RESDATA.TXT\n" +
		"0|./MISSIONS/empty file.txt\n" +
		"345|./My Mods/it's here|odd.txt\n" +
		"stat: './x': Permission denied\n" +
		"garbage line\n" +
		listEndMarker + "\n"
	files, ok := parseRemoteList(out)
	if !ok {
		t.Fatal("marker not seen")
	}
	want := map[string]int64{"resdata.txt": 12, "missions/empty file.txt": 0, "my mods/it's here|odd.txt": 345}
	if !reflect.DeepEqual(files, want) {
		t.Errorf("got %v", files)
	}
	if _, ok := parseRemoteList("12|./a\n"); ok {
		t.Error("listing without marker must be incomplete")
	}
	local := []gameFile{{Rel: "RESDATA.TXT", Size: 12}, {Rel: "Missions/Empty File.txt", Size: 1}, {Rel: "NEW.TXT", Size: 1}}
	todo := filesToCopy(local, files)
	if len(todo) != 2 || todo[0].Rel != "Missions/Empty File.txt" || todo[1].Rel != "NEW.TXT" {
		t.Errorf("filesToCopy = %v", todo)
	}
}

func TestShQuoteAndMkdir(t *testing.T) {
	if got := shQuote("it's a b"); got != `'it'\''s a b'` {
		t.Errorf("shQuote = %s", got)
	}
	files := []gameFile{
		{Rel: "A/B/C/f1"}, {Rel: "A/B/f2"}, {Rel: "A B/f3"}, {Rel: "root.txt"}, {Rel: "Q/it's/f"},
	}
	cmds := mkdirCommands("/base", files, 4000)
	want := []string{`mkdir -p '/base/A B' '/base/A/B/C' '/base/Q/it'\''s'`}
	if !reflect.DeepEqual(cmds, want) {
		t.Errorf("mkdir = %q", cmds)
	}
	// Splitting keeps every command under the limit.
	var many []gameFile
	for i := 0; i < 500; i++ {
		many = append(many, gameFile{Rel: strings.Repeat("d", 20) + "/" + string(rune('a'+i%26)) + strings.Repeat("x", i) + "/f"})
	}
	cmds = mkdirCommands("/sdcard/Android/data/org.opentie.quest/files", many, 4000)
	count := 0
	for _, c := range cmds {
		if len(c) > 4000 && strings.Count(c, "'/sdcard") > 1 {
			t.Errorf("command too long: %d", len(c))
		}
		count += strings.Count(c, "'/sdcard")
	}
	if count != 500 {
		t.Errorf("got %d dirs, want 500", count)
	}
}

func TestPlanPushes(t *testing.T) {
	g := filepath.Join(string(filepath.Separator)+"g", "remastered")
	var files []gameFile
	for i := 0; i < 250; i++ {
		rel := "IVFILES/sub/f" + string(rune('a'+i%26)) + strings.Repeat("1", i/26)
		files = append(files, gameFile{Rel: "TIE98/" + rel, Abs: filepath.Join(g, filepath.FromSlash(rel)), Size: 1})
	}
	files = append(files,
		gameFile{Rel: "TIE98/MISSION/x.tie", Abs: filepath.Join(g, "MISSION", "x.tie")},
		gameFile{Rel: "TIE98/STRINGS.DAT", Abs: filepath.Join(g, "STRINGS.DAT")},
		gameFile{Rel: "XWA/FlightModels/TIECOCKPIT.OPT", Abs: filepath.Join("/x", "FLIGHTMODELS", "TieCockpit.opt")})
	jobs := planPushes("/r", files)
	if len(jobs) != 4 {
		t.Fatalf("got %d jobs", len(jobs))
	}
	if jobs[0].remote != "/r/TIE98/STRINGS.DAT" || jobs[1].local != filepath.Join(g, "IVFILES")+string(filepath.Separator)+"." ||
		jobs[1].remote != "/r/TIE98/IVFILES/" || len(jobs[1].files) != 250 || jobs[2].remote != "/r/TIE98/MISSION/x.tie" ||
		jobs[3].remote != "/r/XWA/FlightModels/TIECOCKPIT.OPT" {
		t.Errorf("jobs: %+v %+v %+v %+v", jobs[0], jobs[1].local, jobs[2], jobs[3])
	}
	// A group whose PC path does not end like its headset path goes file by file.
	if l := localDirOf(gameFile{Rel: "A/B/c", Abs: filepath.Join("/x", "B", "C")}, "A/B"); l != "" {
		t.Errorf("localDirOf = %q", l)
	}
}

func TestDestFolders(t *testing.T) {
	files := []gameFile{{Rel: "XWA/FlightModels/TIECOCKPIT.OPT"}, {Rel: "TIE95/game.fil"}, {Rel: "TIE98/RESOURCE/TITLE.LFD"}, {Rel: "TIE98/STRINGS.DAT"}}
	dests, groups := byDest(files)
	if !reflect.DeepEqual(dests, []string{"TIE98", "TIE95", "XWA/FlightModels"}) {
		t.Errorf("dests = %q", dests)
	}
	if len(groups["TIE98"]) != 2 || groups["TIE98"][0].Rel != "RESOURCE/TITLE.LFD" || groups["XWA/FlightModels"][0].Rel != "TIECOCKPIT.OPT" ||
		groups["TIE95"][0].Rel != "game.fil" {
		t.Errorf("groups = %+v", groups)
	}
	// A file bigger than a batch goes in a zip of its own.
	b := importBatches([]importItem{{Rel: "a", Size: 10}, {Rel: "game.fil", Size: 623839776}, {Rel: "b", Size: 10}})
	if len(b) != 3 || len(b[1]) != 1 || b[1][0].Rel != "game.fil" {
		t.Errorf("batches: %v", b)
	}
}

func TestParseDf(t *testing.T) {
	toybox := "Filesystem     1K-blocks    Used Available Use% Mounted on\n/dev/fuse      110000000 9000000 101000000   9% /storage/emulated\n"
	if n, ok := parseDfAvailable(toybox); !ok || n != 101000000 {
		t.Errorf("toybox: %d %v", n, ok)
	}
	wrapped := "Filesystem 1K-blocks Used Available Use% Mounted on\n/dev/very/long/name\n 1000 400 600 40% /x\n"
	if n, ok := parseDfAvailable(wrapped); !ok || n != 600 {
		t.Errorf("wrapped: %d %v", n, ok)
	}
	if _, ok := parseDfAvailable("df: /x: No such file"); ok {
		t.Error("error output must not parse")
	}
}

func TestParseSHA256Sums(t *testing.T) {
	h := strings.Repeat("ab", 32)
	text := h + "  OpenTIE-VR-1.0.1.zip\n" + strings.Repeat("CD", 32) + " *OpenTIE-VR-1.0.1.apk\r\nbad line\n"
	sums := parseSHA256Sums(strings.ReplaceAll(text, "\r", ""))
	if sums["OpenTIE-VR-1.0.1.zip"] != h || sums["OpenTIE-VR-1.0.1.apk"] != strings.Repeat("cd", 32) || len(sums) != 2 {
		t.Errorf("sums = %v", sums)
	}
}

func TestDevicesAndNames(t *testing.T) {
	out := "List of devices attached\n" +
		"1WMHH000000000\tdevice usb:1-1 product:eureka model:Quest_3 device:eureka transport_id:3\n" +
		"PA7L10MGF\tunauthorized usb:1-2 transport_id:4\n" +
		"0123\tno permissions (user in plugdev group); see [http://developer.android.com/tools/device.html]\n\n"
	devs := parseDevices(out)
	if len(devs) != 3 || devs[0].model != "Quest 3" || devs[1].state != "unauthorized" || devs[2].state != "no permissions" {
		t.Errorf("devices = %+v", devs)
	}
	if waitState(devs) != "unauthorized" || waitState(nil) != "none" || waitState([]adbDevice{{state: "weird"}}) != "weird" ||
		waitState([]adbDevice{{state: "offline"}, {state: "host"}}) != "offline" {
		t.Error("waitState")
	}
	for _, st := range []string{"none", "unauthorized", "offline", "authorizing", "connecting", "no permissions",
		"recovery", "sideload", "bootloader", "host", "weird"} {
		if waitMessage(st) == "" {
			t.Errorf("no message for %s", st)
		}
	}
	if !strings.Contains(waitMessage("unauthorized"), "Allow") || !strings.Contains(waitMessage("none"), "USB-C") ||
		!strings.Contains(waitMessage("unauthorized"), "Steam Frame") || !strings.Contains(waitMessage("weird"), "weird") {
		t.Error("wait messages")
	}
	if !strings.Contains(shellProblem("error: closed"), "closed") || !strings.Contains(shellProblem("adb: more than one device/emulator"), "Unplug") {
		t.Error("shellProblem")
	}
	names := map[[2]string]string{
		{"Oculus", "Quest 3"}:  "Meta Quest 3",
		{"Meta", "Quest 3S"}:   "Meta Quest 3S",
		{"Oculus", "Quest"}:    "Meta Quest",
		{"Pico", "PICO 4"}:     "PICO 4",
		{"Valve", "Frame"}:     "Valve Frame",
		{"", ""}:               "headset",
		{"Oculus", "Oculus X"}: "Oculus X",
	}
	for in, want := range names {
		if got := deviceDisplayName(in[0], in[1]); got != want {
			t.Errorf("deviceDisplayName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestInstalledVersionAndPilots(t *testing.T) {
	dump := "Activity Resolver Table:\n  versionName=9.9 (other)\nPackages:\n  Package [org.opentie.quest] (c0ffee):\n    versionCode=7 minSdk=29\n    versionName=1.0.0-rc3\n"
	if v, ok := installedVersion(dump); !ok || v != "1.0.0-rc3" {
		t.Errorf("installedVersion = %q %v", v, ok)
	}
	if _, ok := installedVersion("Unable to find package: org.opentie.quest"); ok {
		t.Error("not installed expected")
	}
	if apkVersionFromName("OpenTIE-VR-1.0.0-rc3.apk") != "1.0.0-rc3" || apkVersionFromName("OpenXWA-quest-1.0.0.apk") != "" ||
		apkVersionFromName("other.apk") != "" {
		t.Error("apkVersionFromName")
	}
	names := pilotNames([]gameFile{{Rel: "MAAREK.TFR"}, {Rel: "TOPACE10.TFR"}})
	if !reflect.DeepEqual(names, []string{"MAAREK", "TOPACE10"}) {
		t.Errorf("pilotNames = %q", names)
	}
	// Pilots: TIE98 wins over TIE95 for the same name; short names and empty
	// files are left out.
	dir := t.TempDir()
	r, c := filepath.Join(dir, "remastered"), filepath.Join(dir, "collectors_cd")
	os.MkdirAll(r, 0o755)
	os.MkdirAll(c, 0o755)
	tfr := make([]byte, 3856)
	os.WriteFile(filepath.Join(r, "MAAREK.TFR"), tfr, 0o644)
	os.WriteFile(filepath.Join(c, "maarek.tfr"), tfr[:100], 0o644)
	os.WriteFile(filepath.Join(c, "TOPACE10.TFR"), tfr, 0o644)
	os.WriteFile(filepath.Join(c, "EMPTY.TFR"), nil, 0o644)
	os.WriteFile(filepath.Join(c, ".tfr"), tfr, 0o644)
	a := &app{game: tieInstall{root: dir, tie98: r, tie95: c}}
	got := a.allPilotFiles()
	if len(got) != 2 || got[0].Rel != "MAAREK.TFR" || got[0].Size != 3856 || got[1].Rel != "TOPACE10.TFR" {
		t.Errorf("allPilotFiles = %+v", got)
	}
}

func writeFiles(t *testing.T, root string, rels ...string) {
	t.Helper()
	for _, f := range rels {
		p := filepath.Join(root, filepath.FromSlash(f))
		if strings.HasSuffix(f, "/") {
			os.MkdirAll(p, 0o755)
			continue
		}
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// fakeTie makes a TIE Fighter install in the Steam layout below dir.
func fakeTie(t *testing.T, dir string) string {
	game := filepath.Join(dir, "STAR WARS Tie Fighter")
	common := []string{"STRINGS.DAT", "RESOURCE/TITLE.LFD", "RESOURCE/mainmenu.lfd", "MISSION/B1M1.TIE", "RES320/", "CP320/x.lfd"}
	var r98, r95, classic []string
	for _, f := range common {
		r98 = append(r98, "remastered/"+f)
		r95 = append(r95, "collectors_cd/"+f)
		classic = append(classic, "classic/"+f)
	}
	r98 = append(r98, "remastered/RESOURCE/MM640.LFD", "remastered/IVFILES/TIE.OPT", "remastered/TIE95.EXE", "remastered/MAAREK.TFR")
	r95 = append(r95, "collectors_cd/RES640/", "collectors_cd/game.ins", "collectors_cd/DOSBOX/dosbox.exe", "collectors_cd/dosbox_tiecd.conf")
	writeFiles(t, game, append(append(r98, r95...), classic...)...)
	return game
}

func TestGameDetection(t *testing.T) {
	dir := t.TempDir()
	game := fakeTie(t, dir)
	ti := findTieInstall(game, 2)
	if ti.tie98 != filepath.Join(game, "remastered") || ti.tie95 != filepath.Join(game, "collectors_cd") || ti.root != game {
		t.Errorf("findTieInstall = %+v", ti)
	}
	if classifyEdition(filepath.Join(game, "classic")) != "" {
		t.Error("the floppy version must not be used")
	}
	// The floppy version under another name: no RES640, no CD image.
	writeFiles(t, dir, "floppy/STRINGS.DAT", "floppy/RESOURCE/TITLE.LFD", "floppy/RESOURCE/MAINMENU.LFD", "floppy/MISSION/", "floppy/RES320/", "floppy/CP320/")
	if e := classifyEdition(filepath.Join(dir, "floppy")); e != "" {
		t.Errorf("floppy classified as %q", e)
	}
	// A folder that is an edition itself is used alone.
	if ti := findTieInstall(filepath.Join(game, "remastered"), 2); ti.tie98 == "" || ti.tie95 != "" {
		t.Errorf("edition folder: %+v", ti)
	}
	if _, prob := tryUserFolder(filepath.Join(dir, "floppy")); prob == "" {
		t.Error("floppy folder accepted")
	}
	// Files to copy: editions mapped to TIE98/TIE95, programs, pilots and
	// DOSBox left out, cockpits added.
	xwa := filepath.Join(dir, "Star Wars X-Wing Alliance")
	writeFiles(t, xwa, "FlightModels/TieCockpit.opt", "FlightModels/TIEBOMBERCOCKPIT.OPT", "FlightModels/XWING.OPT")
	a := &app{game: ti}
	a.game = findTieInstall(game, 2)
	if r := findXWARoot(dir, 2); r != xwa {
		t.Errorf("findXWARoot = %q", r)
	}
	a.cockpits = xwaCockpitFiles(xwa)
	files, _, err := a.gameFiles()
	if err != nil {
		t.Fatal(err)
	}
	var rels []string
	for _, f := range files {
		rels = append(rels, f.Rel)
	}
	want := []string{"TIE95/CP320/x.lfd", "TIE95/MISSION/B1M1.TIE", "TIE95/RESOURCE/TITLE.LFD", "TIE95/RESOURCE/mainmenu.lfd",
		"TIE95/STRINGS.DAT", "TIE95/game.ins",
		"TIE98/CP320/x.lfd", "TIE98/IVFILES/TIE.OPT", "TIE98/MISSION/B1M1.TIE", "TIE98/RESOURCE/MM640.LFD", "TIE98/RESOURCE/TITLE.LFD",
		"TIE98/RESOURCE/mainmenu.lfd", "TIE98/STRINGS.DAT",
		"XWA/FlightModels/TIEBOMBERCOCKPIT.OPT", "XWA/FlightModels/TIECOCKPIT.OPT"}
	if !reflect.DeepEqual(rels, want) {
		t.Errorf("gameFiles =\n%q\nwant\n%q", rels, want)
	}
	// Steam library with libraryfolders.vdf pointing to a second library.
	steam := filepath.Join(dir, "steam")
	lib := filepath.Join(dir, "lib2")
	os.MkdirAll(filepath.Join(steam, "steamapps"), 0o755)
	os.MkdirAll(filepath.Join(lib, "steamapps", "common"), 0o755)
	os.Rename(game, filepath.Join(lib, "steamapps", "common", "STAR WARS Tie Fighter"))
	os.Rename(xwa, filepath.Join(lib, "steamapps", "common", "Star Wars X-Wing Alliance"))
	vdf := `"libraryfolders" { "1" { "path" "` + strings.ReplaceAll(lib, `\`, `\\`) + `" } }`
	os.WriteFile(filepath.Join(steam, "steamapps", "libraryfolders.vdf"), []byte(vdf), 0o644)
	dirs := steamGameDirs([]string{steam}, tieKeywords)
	if len(dirs) != 1 || !strings.HasSuffix(dirs[0], "STAR WARS Tie Fighter") {
		t.Errorf("steamGameDirs = %q", dirs)
	}
	dirs = steamGameDirs([]string{steam}, xwaKeywords)
	if len(dirs) != 1 || !strings.HasSuffix(dirs[0], "Star Wars X-Wing Alliance") {
		t.Errorf("steamGameDirs (XWA) = %q", dirs)
	}
	if got := cleanUserPath(`  "C:\Games\TIE"  `); got != `C:\Games\TIE` {
		t.Errorf("cleanUserPath = %q", got)
	}
}

func TestSafeZipPath(t *testing.T) {
	dest := t.TempDir()
	for _, bad := range []string{"../x", "a/../../x", "/etc/passwd", `C:\x`, `..\x`} {
		if _, err := safeZipPath(dest, bad); err == nil {
			t.Errorf("%q must be rejected", bad)
		}
	}
	if p, err := safeZipPath(dest, "platform-tools/adb"); err != nil || p != filepath.Join(dest, "platform-tools", "adb") {
		t.Errorf("good path: %q %v", p, err)
	}
	// A zip with a bad entry is rejected before anything is written.
	zp := filepath.Join(t.TempDir(), "bad.zip")
	f, _ := os.Create(zp)
	w := zip.NewWriter(f)
	ww, _ := w.Create("ok.txt")
	ww.Write([]byte("ok"))
	w.Create("../evil.txt")
	w.Close()
	f.Close()
	if _, err := extractZip(zp, dest); err == nil {
		t.Error("zip slip not rejected")
	}
	if _, err := os.Stat(filepath.Join(dest, "ok.txt")); err == nil {
		t.Error("files written before rejecting")
	}
}

func TestSettings(t *testing.T) {
	p := filepath.Join(t.TempDir(), "launcher.ini")
	s := loadSettings(p)
	if !s.getBool("check_updates", false) || s.getBool("include_test_versions", true) || s.get("skip_version", "x") != "" {
		t.Error("defaults")
	}
	if err := s.set("skip_version", "1.0.1"); err != nil {
		t.Fatal(err)
	}
	s2 := loadSettings(p)
	if s2.get("skip_version", "") != "1.0.1" {
		t.Error("skip_version not saved")
	}
	data, _ := os.ReadFile(p)
	if !strings.Contains(string(data), "# include_test_versions") {
		t.Error("comments lost")
	}
}

func TestFetchNewestRelease(t *testing.T) {
	releases := []ghRelease{
		{TagName: "v1.0.0", PublishedAt: "2026-10-01T10:00:00Z"},
		{TagName: "v1.0.2-rc1", Prerelease: true},
		{TagName: "v9.0.0", Draft: true},
		{TagName: "v1.0.1"},
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") == "" || r.Header.Get("Accept") != "application/vnd.github+json" {
			w.WriteHeader(400)
			return
		}
		switch r.URL.Path {
		case releasesPath + "/latest":
			json.NewEncoder(w).Encode(releases[3])
		case releasesPath:
			json.NewEncoder(w).Encode(releases)
		default:
			w.WriteHeader(404)
		}
	}))
	defer srv.Close()
	r, err := fetchNewestRelease(srv.URL, false)
	if err != nil || r.version() != "1.0.1" {
		t.Errorf("stable: %v %v", r, err)
	}
	r, err = fetchNewestRelease(srv.URL, true)
	if err != nil || r.version() != "1.0.2-rc1" {
		t.Errorf("test versions: %v %v", r, err)
	}
	if _, err := fetchNewestRelease("http://127.0.0.1:1", false); err == nil {
		t.Error("offline must fail")
	}
	if d := (&ghRelease{PublishedAt: "2026-10-01T10:00:00Z"}).releaseDate(); d != "1 Oct 2026" {
		t.Errorf("date %q", d)
	}
}

func TestOwnership(t *testing.T) {
	out := "10225|u0_a225|/sdcard/x/XWA\n2000|shell|/sdcard/x/XWA/FLIGHTMODELS\nstat: missing\n"
	m := parseOwners(out, []string{"/sdcard/x/XWA", "/sdcard/x/XWA/FLIGHTMODELS"})
	if m["/sdcard/x/XWA"].uid != 10225 || m["/sdcard/x/XWA/FLIGHTMODELS"].name != "shell" {
		t.Errorf("parseOwners = %v", m)
	}
	// Names printed differently: matched by order when every path got a line.
	m = parseOwners("10225|u0_a225|/storage/emulated/0/a\n", []string{"/sdcard/a"})
	if m["/sdcard/a"].uid != 10225 {
		t.Errorf("by order: %v", m)
	}
	cases := []struct {
		o      owner
		app    int
		foreig bool
	}{
		{owner{"shell", 2000}, 10225, true},
		{owner{"shell", -1}, 0, true},
		{owner{"u0_a225", 10225}, 10225, false},
		{owner{"u0_a99", 10099}, 10225, true},
		{owner{"u0_a225", 10225}, 0, false},
		{owner{"root", 0}, 0, true},
		{owner{"u0_a225", -1}, 10225, false},
	}
	for _, c := range cases {
		if got := foreignOwner(c.o, c.app); got != c.foreig {
			t.Errorf("foreignOwner(%v, %d) = %v", c.o, c.app, got)
		}
	}
	dump := "Packages:\n  Package [org.opentie.quest] (4d2b1c):\n    userId=10225\n    versionName=1.0\n"
	if parseAppUID(dump) != 10225 || parseAppUID("nothing") != 0 {
		t.Error("parseAppUID")
	}
}

func TestLeptonPorts(t *testing.T) {
	ss := "State  Recv-Q Send-Q Local Address:Port Peer Address:Port\n" +
		"LISTEN 0      128          0.0.0.0:22         0.0.0.0:*\n" +
		"LISTEN 0      4096       127.0.0.1:5557       0.0.0.0:*\n" +
		"LISTEN 0      4096            [::]:5555          [::]:*\n" +
		"ESTAB  0      0          127.0.0.1:5560     127.0.0.1:40000\n"
	if got := parseLeptonPorts(ss); !reflect.DeepEqual(got, []int{5555, 5557}) {
		t.Errorf("ss: %v", got)
	}
	netstat := "Proto Recv-Q Send-Q Local Address  Foreign Address State\ntcp 0 0 0.0.0.0:5556 0.0.0.0:* LISTEN\n"
	if got := parseLeptonPorts(netstat); !reflect.DeepEqual(got, []int{5556}) {
		t.Errorf("netstat: %v", got)
	}
}

func TestReflow(t *testing.T) {
	in := "OpenXWA could not be updated: the copy\nwas signed differently.\n\nWARNING: uninstalling\ndeletes files.\nThese files:\n  a.txt\n  b.txt"
	want := "OpenXWA could not be updated: the copy was signed differently.\n\nWARNING: uninstalling deletes files. These files:\n  a.txt\n  b.txt"
	if got := reflow(in); got != want {
		t.Errorf("reflow:\n%q\nwant\n%q", got, want)
	}
}

func TestParseBundle(t *testing.T) {
	r, ok := parseBundle("Result: Bundle[{made=1, asked=1, base=/storage/emulated/0/Android/data/org.opentie.quest/files/}]\n")
	if !ok || r["made"] != "1" || r["asked"] != "1" || r["base"] != "/storage/emulated/0/Android/data/org.opentie.quest/files/" {
		t.Errorf("parseBundle: %v %v", r, ok)
	}
	r, ok = parseBundle("Result: Bundle[{error=java.io.FileNotFoundException: /x/y (Permission denied), base=/b/}]")
	if !ok || r["error"] != "java.io.FileNotFoundException: /x/y (Permission denied)" || r["base"] != "/b/" {
		t.Errorf("parseBundle error: %v", r)
	}
	if _, ok := parseBundle("Result: null"); ok {
		t.Error("null result parsed")
	}
	if _, ok := parseBundle("Error while accessing provider:org.openxwa.quest.setup\njava.lang.IllegalArgumentException: Unknown authority"); ok {
		t.Error("error parsed")
	}
}

func TestImportBatches(t *testing.T) {
	var items []importItem
	for i := 0; i < 6000; i++ {
		items = append(items, importItem{Rel: fmt.Sprint(i), Size: 10})
	}
	items = append(items, importItem{Rel: "big1", Size: importBatchSize - 5}, importItem{Rel: "big2", Size: importBatchSize + 5})
	b := importBatches(items)
	n := 0
	for _, x := range b {
		n += len(x)
		if len(x) > importBatchMax {
			t.Errorf("batch of %d files", len(x))
		}
	}
	if n != len(items) || len(b) != 5 {
		t.Errorf("%d items in %d batches", n, len(b))
	}
}

func TestReleaseAssets(t *testing.T) {
	rel := &ghRelease{Assets: []ghAsset{{Name: "OpenTIE-VR-1.0.0.apk"}, {Name: "OpenXWA-Quest-1.0.0.zip"}, {Name: "OpenTIE-VR-1.0.0.zip"}, {Name: "SHA256SUMS.txt"}}}
	z, s := findReleaseAssets(rel)
	if z == nil || z.Name != "OpenTIE-VR-1.0.0.zip" || s == nil {
		t.Errorf("findReleaseAssets = %v %v", z, s)
	}
}

// TestRealInstall checks the detection on a real install when
// OPENTIE_TEST_TIE (the "STAR WARS Tie Fighter" folder) and optionally
// OPENTIE_TEST_XWA are set; skipped otherwise.
func TestRealInstall(t *testing.T) {
	dir := os.Getenv("OPENTIE_TEST_TIE")
	if dir == "" {
		t.Skip("OPENTIE_TEST_TIE not set")
	}
	a := &app{game: findTieInstall(dir, 2)}
	if x := os.Getenv("OPENTIE_TEST_XWA"); x != "" {
		a.cockpits = xwaCockpitFiles(findXWARoot(x, 2))
	}
	files, skipped, err := a.gameFiles()
	if err != nil || !a.game.ok() {
		t.Fatalf("%+v %v", a.game, err)
	}
	dests, groups := byDest(files)
	for _, d := range dests {
		t.Logf("%s: %d files, %s", d, len(groups[d]), humanSize(totalSize(groups[d])))
	}
	t.Logf("install %+v; %d files, %d skipped; pilots %v", a.game, len(files), skipped, pilotNames(a.allPilotFiles()))
}
