package main

import (
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

// TIE Fighter keeps each pilot in one <name>.TFR file (3856 bytes) in the
// game folder (remastered/ and collectors_cd/ on Steam; with Windows'
// VirtualStore when the game is installed below Program Files).

// pilotName is the pilot's name: the file name without .tfr.
func pilotName(file string) string {
	return strings.TrimSuffix(file, filepath.Ext(file))
}

// pilotFiles lists the *.tfr files at the top level of dir.
func pilotFiles(dir string) []gameFile {
	if dir == "" {
		return nil
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []gameFile
	for _, e := range entries {
		if !e.Type().IsRegular() || !strings.EqualFold(filepath.Ext(e.Name()), ".tfr") {
			continue
		}
		info, err := e.Info()
		if err != nil || info.Size() == 0 {
			continue
		}
		// The app takes names of 5 to 64 characters.
		if len(e.Name()) < 5 || len(e.Name()) > 64 {
			continue
		}
		out = append(out, gameFile{Rel: e.Name(), Abs: filepath.Join(dir, e.Name()), Size: info.Size()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Rel < out[j].Rel })
	return out
}

// virtualStoreDir is where Windows keeps files a game wrote below Program
// Files ("" elsewhere).
func virtualStoreDir(dir string) string {
	local := os.Getenv("LOCALAPPDATA")
	if runtime.GOOS != "windows" || local == "" || len(dir) < 3 || dir[1] != ':' {
		return ""
	}
	return filepath.Join(local, "VirtualStore", dir[3:])
}

// pilotDirs lists the folders searched for pilots: the 1998 edition first
// (its pilot wins when both editions have one with the same name).
func (a *app) pilotDirs() []string {
	var dirs []string
	for _, d := range []string{a.game.tie98, a.game.tie95} {
		if d == "" {
			continue
		}
		dirs = append(dirs, d)
		if v := virtualStoreDir(d); v != "" && isDir(v) {
			dirs = append(dirs, v)
		}
	}
	return dirs
}

// allPilotFiles collects the pilots of all editions; a name is only taken
// once.
func (a *app) allPilotFiles() []gameFile {
	seen := map[string]bool{}
	var out []gameFile
	for _, d := range a.pilotDirs() {
		for _, f := range pilotFiles(d) {
			k := strings.ToLower(f.Rel)
			if seen[k] {
				logf("pilot %s in %s skipped (one with that name was already found)", f.Rel, d)
				continue
			}
			seen[k] = true
			out = append(out, f)
		}
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Rel) < strings.ToLower(out[j].Rel) })
	return out
}

func pilotNames(files []gameFile) []string {
	var names []string
	for _, f := range files {
		names = append(names, pilotName(f.Rel))
	}
	return names
}

// copyPilots is step 7: optionally bring the PC pilots over. The app
// imports *.tfr from pilot-import when it starts (a pilot already on the
// headset is kept).
func (a *app) copyPilots() error {
	files := a.allPilotFiles()
	if len(files) == 0 {
		say("No pilot files in the game folder. Nothing to do.")
		return nil
	}
	logf("pilots: %v", pilotNames(files))
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "Copy your pilots?",
		Text: "Pilots found in your game folder: " + strings.Join(pilotNames(files), ", ") + ".\n" +
			"OpenTIE-VR keeps its own pilots. Copying brings your PC pilots over; they are\n" +
			"imported the next time OpenTIE-VR starts (a pilot with the same name that is\n" +
			"already on the headset is kept).",
		Options: []Option{
			{ID: "yes", Label: "Copy pilots", Key: "y"},
			{ID: "no", Label: "Don't copy", Key: "n", Style: "primary"},
		},
		Default:       "no",
		ConsolePrompt: "Copy these pilots to the headset? (y/N)",
	})
	if ans.ID != "yes" {
		say("Pilots are not copied.")
		return nil
	}
	if a.helper {
		if err := a.importFiles("Copying pilots", itemsOf(files), pilotsFolder); err != nil {
			return err
		}
		return a.pilotsDone()
	}
	// pilot-import is made by mkdir only where such folders belong to the app
	// (see probeCopyOwnership); otherwise adb push makes it.
	if a.mkdirOK {
		if out, err := a.adb.shell(30*time.Second, "mkdir -p "+shQuote(a.remotePilots())); err != nil {
			return fail("Could not create the pilot folder on the headset:\n  %s", strings.TrimSpace(out))
		}
	}
	for try := 1; try <= 3; try++ {
		var jobs []pushJob
		remote, err := a.listRemote(a.remotePilots())
		if err != nil {
			remote = map[string]int64{}
		}
		bad := filesToCopy(files, remote)
		if len(bad) == 0 {
			return a.pilotsDone()
		}
		for _, f := range bad {
			j := singleFileJob(a.remotePilots(), f)
			j.label = ""
			jobs = append(jobs, j)
		}
		if err := a.runPushes("Copying pilots", jobs, bad); err != nil {
			return err
		}
	}
	remote, _ := a.listRemote(a.remotePilots())
	if bad := filesToCopy(files, remote); len(bad) > 0 {
		var names []string
		for _, f := range bad {
			names = append(names, f.Rel)
		}
		say("WARNING: these pilot files could not be copied: %s", strings.Join(names, ", "))
		say("The game files are copied anyway; run the launcher again later to retry.")
		return nil
	}
	return a.pilotsDone()
}

// pilotsDone checks that the app can read pilot-import, then reports.
func (a *app) pilotsDone() error {
	m := a.remoteOwners(a.remotePilots())
	if o, ok := m[a.remotePilots()]; ok && foreignOwner(o, a.appUID()) {
		logf("pilot-import belongs to %v; removing it", o)
		a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
		say("WARNING: the pilots could not be copied in a way OpenTIE-VR can read on this\n" +
			"headset, so they were removed again. The game files are copied anyway.")
		return nil
	}
	say("Pilots copied. OpenTIE-VR brings them in the next time it starts.")
	return nil
}
