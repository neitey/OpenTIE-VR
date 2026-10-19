package main

import (
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// The game folders on the headset (TIE98, TIE95, XWA) must belong to the
// OpenTIE-VR app. A folder made over USB (adb runs as the "shell" user)
// belongs to shell, and the app cannot read it or anything copied into it.
// So the launcher never creates them itself: the app does (its helper, or
// the first time it starts).

const shellUID = 2000

// owner is a file's owner as "stat -c" shows it.
type owner struct {
	name string
	uid  int // -1 if unknown
}

func (o owner) String() string {
	if o.uid < 0 {
		return o.name
	}
	return fmt.Sprintf("%s (uid %d)", o.name, o.uid)
}

// parseOwners reads "uid|name|path" lines from stat -c '%u|%U|%n' for the
// asked paths. stat prints the paths as given; if the names do not match
// but every path got a line, the lines are matched by order instead.
func parseOwners(out string, paths []string) map[string]owner {
	m := map[string]owner{}
	var order []owner
	for _, line := range strings.Split(out, "\n") {
		parts := strings.SplitN(strings.TrimRight(line, "\r"), "|", 3)
		if len(parts) != 3 {
			continue
		}
		uid, err := strconv.Atoi(strings.TrimSpace(parts[0]))
		if err != nil {
			continue
		}
		o := owner{name: strings.TrimSpace(parts[1]), uid: uid}
		m[parts[2]] = o
		order = append(order, o)
	}
	matched := 0
	for _, p := range paths {
		if _, ok := m[p]; ok {
			matched++
		}
	}
	if matched < len(paths) && len(order) == len(paths) {
		m = map[string]owner{}
		for i, p := range paths {
			m[p] = order[i]
		}
	}
	return m
}

var appUserRe = regexp.MustCompile(`^u\d+_a\d+$`)

// foreignOwner reports an owner the app cannot read as: the shell user, or a
// different uid than the app's own (when that is known).
func foreignOwner(o owner, appUID int) bool {
	if o.name == "shell" || o.uid == shellUID {
		return true
	}
	if appUID > 0 && o.uid >= 0 {
		return o.uid != appUID
	}
	if o.uid >= 0 && o.uid < 10000 {
		return !appUserRe.MatchString(o.name)
	}
	return false
}

// Android 13 and older print "userId=", Android 14 "appId=".
var userIDRe = regexp.MustCompile(`(?m)^\s*(?:userId|appId)=(\d+)`)

// parseAppUID reads the app's uid from dumpsys package output.
func parseAppUID(dumpsys string) int {
	i := strings.Index(dumpsys, "Package ["+appPackage+"]")
	if i < 0 {
		return 0
	}
	if m := userIDRe.FindStringSubmatch(dumpsys[i:]); m != nil {
		n, _ := strconv.Atoi(m[len(m)-1])
		return n
	}
	return 0
}

// appUID returns the app's Linux uid (0 if unknown), asked once.
func (a *app) appUID() int {
	if a.uid == 0 {
		out, _ := a.adb.shell(60*time.Second, "dumpsys package "+appPackage)
		a.uid = parseAppUID(out)
		logf("OpenTIE-VR app uid: %d", a.uid)
	}
	return a.uid
}

// remoteOwners stats the given headset paths (missing ones are left out).
func (a *app) remoteOwners(paths ...string) map[string]owner {
	var q []string
	for _, p := range paths {
		q = append(q, shQuote(p))
	}
	out, _ := a.adb.shell(60*time.Second, "stat -c '%u|%U|%n' "+strings.Join(q, " ")+" 2>/dev/null")
	m := parseOwners(out, paths)
	if len(m) == 0 && len(paths) > 0 {
		// No stat -c: fall back to ls -ld (owner name only).
		for _, p := range paths {
			o, _ := a.adb.shell(30*time.Second, "ls -ld "+shQuote(p))
			if f := strings.Fields(firstLine(o)); len(f) >= 3 && strings.HasPrefix(f[0], "d") || len(f) >= 3 && strings.HasPrefix(f[0], "-") {
				m[p] = owner{name: f[2], uid: -1}
			}
		}
	}
	return m
}

// gameRoots are the folders below the files folder that the game files go
// into; each must belong to the app.
var gameRoots = []string{tie98Folder, tie95Folder, "XWA"}

// prepareAppFolder is step 6: make sure the app's game folders exist and
// belong to the app. Folders made over USB are removed and made again by the
// app.
func (a *app) prepareAppFolder() error {
	fixed := false
	a.tryHelper() // also makes the folders when it has the helper
	for {
		if appCtx.Err() != nil {
			return errClosed
		}
		base := a.remoteBase()
		if !a.remoteHasEntry(base, tie98Folder) {
			if a.helper {
				if err := a.helperMkdirs(appFolders...); err != nil {
					return fail("OpenTIE-VR could not create its game folders on the headset:\n  %v", err)
				}
				if !a.remoteHasEntry(base, tie98Folder) {
					return fail("OpenTIE-VR created its game folders, but the launcher cannot see them.\n" +
						"Please collect the logs and report this, naming your headset model.")
				}
				continue
			}
			if err := a.waitForAppFolder(); err != nil {
				return err
			}
			continue
		}
		uid := a.appUID()
		paths := []string{base}
		for _, r := range gameRoots {
			paths = append(paths, base+"/"+r)
		}
		paths = append(paths, base+"/"+cockpitsFolder, a.remotePilots())
		owners := a.remoteOwners(paths...)
		logf("owners: %v", owners)
		var bad []string
		var badOwner owner
		for _, r := range append(append([]string{}, gameRoots...), cockpitsFolder) {
			o, ok := owners[base+"/"+r]
			if !ok || !foreignOwner(o, uid) {
				continue
			}
			if r == cockpitsFolder && contains(bad, "XWA") {
				continue // removed with XWA
			}
			bad = append(bad, r)
			badOwner = o
		}
		if len(bad) > 0 {
			if fixed {
				return fail("The OpenTIE-VR folders on the headset were made again, but %s still\n"+
					"belongs to %s instead of the OpenTIE-VR app. Please collect the logs and\n"+
					"report this, naming your headset model.", strings.Join(bad, ", "), badOwner)
			}
			if err := a.fixForeignFolder(bad, badOwner, owners); err != nil {
				return err
			}
			fixed = true
			continue
		}
		if _, known := owners[base+"/"+tie98Folder]; !known {
			logf("could not read the owner of %s; continuing", base+"/"+tie98Folder)
		}
		if p, ok := owners[a.remotePilots()]; ok && foreignOwner(p, uid) {
			logf("pilot-import belongs to %v; removing it", p)
			a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
		}
		break
	}
	a.cleanupUnreadableCopies()
	if a.helper {
		if err := a.removeShellLeftovers(); err != nil {
			return err
		}
	} else if err := a.probeCopyOwnership(); err != nil {
		return err
	}
	say("The OpenTIE-VR folders on the headset are ready.")
	return nil
}

// fixForeignFolder explains the problem, then removes the unusable folders
// (paths relative to the files folder).
func (a *app) fixForeignFolder(bad []string, o owner, owners map[string]owner) error {
	logf("%v belong to %v, not to the app (uid %d)", bad, o, a.uid)
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "The game folders on the headset cannot be used",
		Text: "These folders on the headset were created over USB, so the game cannot\n" +
			"read them (they belong to " + o.name + "): " + strings.Join(bad, ", ") + "\n\n" +
			"They will be removed and made again by OpenTIE-VR, then the game files are\n" +
			"copied again. Your pilots and settings in OpenTIE-VR are not touched.",
		Options: []Option{
			{ID: "yes", Label: "Remove and make them again", Key: "y", Style: "primary"},
			{ID: "no", Label: "Cancel", Key: "n"},
		},
		Default:       "yes",
		ConsolePrompt: "Remove the folders and make them again? (Y/n)",
	})
	if ans.Closed && appCtx.Err() != nil {
		return errClosed
	}
	if ans.ID != "yes" {
		return fail("The game folders on the headset cannot be read by OpenTIE-VR, so the game\n" +
			"files cannot be used. Try again and choose \"Remove and make them again\".")
	}
	say("Removing the unusable game folders from the headset...")
	var q []string
	for _, r := range bad {
		q = append(q, shQuote(a.remoteBase()+"/"+r))
	}
	if out, err := a.adb.shell(10*time.Minute, "rm -rf "+strings.Join(q, " ")); err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not remove the old folders on the headset:\n  %s", strings.TrimSpace(out))
	}
	if p, ok := owners[a.remotePilots()]; ok && foreignOwner(p, a.uid) {
		a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
	}
	if a.helper {
		// Let the app make all of them again (TIE98 may still be there).
		if err := a.helperMkdirs(appFolders...); err != nil {
			logf("helper mkdirs after removing: %v", err)
		}
	}
	return nil
}

// waitForAppFolder starts OpenTIE-VR and waits (as long as it takes) until
// it has made its TIE98 folder. Horizon OS only really starts the app while
// the headset is worn, so the user is asked to put it on.
func (a *app) waitForAppFolder() error {
	say("OpenTIE-VR has to start once on the headset to create its own folders.\n" +
		"Put the headset on now; OpenTIE-VR starts by itself.")
	q := Question{
		Kind:  "buttons",
		Title: "Put the headset on",
		Text: "OpenTIE-VR has to start once on the headset to create its own folders.\n" +
			"Put the headset on; OpenTIE-VR starts by itself. If it does not, open it from\n" +
			"Library -> Unknown sources. The launcher continues as soon as it has started.",
		Options: []Option{
			{ID: "again", Label: "Try again", Style: "primary"},
			{ID: "cancel", Label: "Cancel"},
		},
	}
	if a.lepton != "" {
		q.Title = "Keep Lepton Development running"
		q.Text = "OpenTIE-VR has to start once to create its own folders. It starts in the\n" +
			"\"Lepton Development\" window on the headset; keep that window open (put the\n" +
			"headset on if needed). The launcher continues as soon as it has started."
	}
	offer := ui.Offer(q)
	defer ui.ClearOffer()
	var lastStart time.Time
	started := time.Now()
	for {
		if time.Since(lastStart) >= 15*time.Second {
			if !lastStart.IsZero() {
				if _, console := ui.(*consoleUI); console {
					say("Still waiting for OpenTIE-VR to start on the headset (put it on; Ctrl+C to stop)...")
				}
			}
			a.adb.shell(30*time.Second, "am start -n "+appActivity)
			lastStart = time.Now()
		}
		select {
		case ans := <-offer:
			if ans.ID == "cancel" {
				return fail("OpenTIE-VR has not started on the headset yet, so its folders do not exist.\n" +
					"Put the headset on, then choose Try again.")
			}
			lastStart = time.Time{} // start it again right now
			offer = ui.Offer(q)
		case <-time.After(time.Second):
		case <-appCtx.Done():
			return errClosed
		}
		if a.remoteHasEntry(a.remoteBase(), tie98Folder) {
			break
		}
	}
	logf("the app created its folders after %.0f s", time.Since(started).Seconds())
	sleepCtx(2 * time.Second) // let it finish starting before it is stopped
	a.adb.shell(30*time.Second, "am force-stop "+appPackage)
	say("OpenTIE-VR created its folders. It was closed again so the files can be copied;\n" +
		"you can take the headset off.")
	return nil
}

func hasUnreadablePrefix(name string) bool {
	for _, r := range gameRoots {
		if strings.HasPrefix(name, r+"-unreadable-") {
			return true
		}
	}
	return false
}

// cleanupUnreadableCopies offers to delete TIE98-unreadable-* (and TIE95-,
// XWA-) folders: the app moves a folder it cannot read aside under that name.
func (a *app) cleanupUnreadableCopies() {
	out, err := a.adb.shell(30*time.Second, "ls "+shQuote(a.remoteBase()))
	if err != nil {
		return
	}
	var dirs []string
	for _, l := range strings.Split(out, "\n") {
		if l = strings.TrimSpace(l); hasUnreadablePrefix(l) {
			dirs = append(dirs, a.remoteBase()+"/"+l)
		}
	}
	if len(dirs) == 0 {
		return
	}
	var q []string
	for _, d := range dirs {
		q = append(q, shQuote(d))
	}
	size := ""
	if du, err := a.adb.shell(5*time.Minute, "du -sk "+strings.Join(q, " ")); err == nil {
		var kb int64
		for _, l := range strings.Split(du, "\n") {
			if f := strings.Fields(l); len(f) >= 1 {
				n, _ := strconv.ParseInt(f[0], 10, 64)
				kb += n
			}
		}
		size = " (" + humanSize(kb*1024) + ")"
	}
	logf("unreadable old copies: %q%s", dirs, size)
	copies := "1 old copy"
	if len(dirs) > 1 {
		copies = fmt.Sprintf("%d old copies", len(dirs))
	}
	if !askYesNo("Old unreadable copies found",
		fmt.Sprintf("The headset has %s of a game folder that OpenTIE-VR could not read%s.\nDelete to free the space?",
			copies, size), true) {
		return
	}
	if _, err := a.adb.shell(10*time.Minute, "rm -rf "+strings.Join(q, " ")); err == nil {
		say("Deleted the old copies.")
	}
}

// probeCopyOwnership checks what owner new folders get on this headset:
// made with mkdir, and made by adb push. Copying uses the way that gives the
// app's ownership; if neither does, copying would be useless, so stop.
func (a *app) probeCopyOwnership() error {
	uid := a.appUID()
	probe := a.remoteBase() + "/" + tie98Folder + "/.opentie-probe"
	rm := "rm -rf " + shQuote(probe)
	a.adb.shell(30*time.Second, rm)
	defer a.adb.shell(30*time.Second, rm)

	a.adb.shell(30*time.Second, "mkdir "+shQuote(probe))
	m := a.remoteOwners(probe)
	o, ok := m[probe]
	a.mkdirOK = ok && !foreignOwner(o, uid)
	logf("probe: a folder made with mkdir belongs to %v (usable: %v)", o, a.mkdirOK)
	a.adb.shell(30*time.Second, rm)

	tmp, err := os.CreateTemp("", "opentie-probe-*.txt")
	if err != nil {
		return nil
	}
	tmp.WriteString("probe\n")
	tmp.Close()
	defer os.Remove(tmp.Name())
	dir := probe + "/sub"
	a.adb.run(60*time.Second, "push", tmp.Name(), dir+"/probe.txt")
	m = a.remoteOwners(dir, dir+"/probe.txt")
	d, okD := m[dir]
	f, okF := m[dir+"/probe.txt"]
	pushOK := okD && okF && !foreignOwner(d, uid) && !foreignOwner(f, uid)
	logf("probe: adb push made a folder owned by %v and a file owned by %v (usable: %v)", d, f, pushOK)
	if !okD || !okF {
		logf("probe: could not read the owners; assuming copying works")
		return nil
	}
	if !pushOK {
		return fail("This headset gives files copied over USB to the wrong owner (%s), so\n"+
			"OpenTIE-VR could not read them. Nothing was copied. Please collect the logs and\n"+
			"report this, naming your headset model.", f)
	}
	return nil
}

// finalOwnershipCheck runs before "All done": each game folder (TIE98, TIE95,
// XWA), each folder one level below it, each STRINGS.DAT and one deep file
// must belong to the app. bad lists the wrong entries one level below a game
// folder ("TIE98/RESOURCE"); rootBad is set when a game folder itself is.
func (a *app) finalOwnershipCheck(files []gameFile) (bad []string, rootBad bool, err error) {
	base := a.remoteBase()
	var lsArgs []string
	for _, r := range gameRoots {
		lsArgs = append(lsArgs, shQuote(base+"/"+r))
	}
	ld, _ := a.adb.shell(30*time.Second, "ls -ld "+shQuote(base)+" "+strings.Join(lsArgs, " ")+" 2>/dev/null")
	logf("final check:\n%s", ld)
	roots := map[string]bool{}
	seconds := map[string]bool{}
	var extra []string
	deepest := ""
	for _, f := range files {
		parts := strings.Split(f.Rel, "/")
		roots[parts[0]] = true
		if len(parts) >= 3 {
			seconds[parts[0]+"/"+parts[1]] = true
		}
		if len(parts) == 2 && strings.EqualFold(parts[1], "STRINGS.DAT") {
			extra = append(extra, f.Rel)
		}
		if strings.Count(f.Rel, "/") > strings.Count(deepest, "/") {
			deepest = f.Rel
		}
	}
	var rels []string
	for _, m := range []map[string]bool{roots, seconds} {
		var l []string
		for k := range m {
			l = append(l, k)
		}
		sort.Strings(l)
		rels = append(rels, l...)
	}
	rels = append(rels, extra...)
	if deepest != "" {
		rels = append(rels, deepest)
	}
	var paths []string
	for _, r := range rels {
		paths = append(paths, base+"/"+r)
	}
	uid := a.appUID()
	owners := a.remoteOwners(paths...)
	var msgs []string
	badSet := map[string]bool{}
	for i, p := range paths {
		o, ok := owners[p]
		if !ok {
			logf("final check: no owner for %s", p)
			continue
		}
		if !foreignOwner(o, uid) {
			continue
		}
		rel := rels[i]
		parts := strings.SplitN(rel, "/", 3)
		if len(parts) == 1 {
			rootBad = true
		} else {
			key := parts[0] + "/" + parts[1]
			if !badSet[key] {
				badSet[key] = true
				bad = append(bad, key)
			}
		}
		msgs = append(msgs, rel+" belongs to "+o.String())
	}
	logf("final check: %d paths, %d not owned by the app", len(owners), len(msgs))
	if len(msgs) > 0 {
		sort.Strings(msgs)
		if len(msgs) > 10 {
			msgs = append(msgs[:10], "...")
		}
		e := &userError{retryStep: prepareStep, msg: fmt.Sprintf("The game files were copied, but OpenTIE-VR cannot read them because they\n"+
			"do not belong to the app:\n  %s\n\nTry again: the launcher removes the unusable folders and copies the files\n"+
			"again. If this keeps happening, please collect the logs and report it.", strings.Join(msgs, "\n  "))}
		return bad, rootBad, e
	}
	say("Checked: the game files belong to OpenTIE-VR, so the game can read them.")
	return nil, false, nil
}
