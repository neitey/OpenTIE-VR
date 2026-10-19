package main

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

var apkNameRe = regexp.MustCompile(`(?i)^opentie-vr-(.+)\.apk$`)

// apkVersionFromName returns "1.0.0-rc3" for OpenTIE-VR-1.0.0-rc3.apk.
func apkVersionFromName(name string) string {
	if m := apkNameRe.FindStringSubmatch(name); m != nil {
		return m[1]
	}
	return ""
}

// findAPK picks the OpenTIE-VR APK next to the exe (the highest version if
// there are several). Done before anything else so a launcher started from
// inside the zip fails early with a clear message.
func (a *app) findAPK() error {
	entries, _ := os.ReadDir(a.exeDir)
	best, bestVer := "", ""
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".apk") {
			continue
		}
		v := apkVersionFromName(e.Name())
		if best == "" || compareVersions(v, bestVer) > 0 {
			best, bestVer = e.Name(), v
		}
	}
	if best == "" {
		return fail("%s", "The OpenTIE-VR app (OpenTIE-VR-<version>.apk) was not found next to the\n"+
			"launcher in:\n  "+a.exeDir+"\n"+
			"Please unzip the whole OpenTIE-VR zip file into a folder first, then start\n"+
			"OpenTIE-VR-Launcher.exe from that folder.")
	}
	a.apkPath = filepath.Join(a.exeDir, best)
	a.apkVersion = bestVer
	logf("APK: %s (version %q)", a.apkPath, bestVer)
	return nil
}

var versionNameRe = regexp.MustCompile(`versionName=(\S+)`)

// installedVersion parses "adb shell dumpsys package org.opentie.quest".
// installed is false when the package is not on the headset.
func installedVersion(dumpsys string) (ver string, installed bool) {
	i := strings.Index(dumpsys, "Package ["+appPackage+"]")
	if i < 0 {
		return "", false
	}
	if m := versionNameRe.FindStringSubmatch(dumpsys[i:]); m != nil {
		return m[1], true
	}
	return "", true
}

// installApp is step 4.
func (a *app) installApp() error {
	out, _ := a.adb.shell(60*time.Second, "dumpsys package "+appPackage)
	inst, installed := installedVersion(out)
	logf("installed version %q (installed=%v), bundled %q", inst, installed, a.apkVersion)

	bundled := a.apkVersion
	if bundled == "" {
		bundled = "(unknown version)"
	}
	downgrade := false
	switch {
	case !installed:
		say("Installing OpenTIE-VR %s...", bundled)
	case a.apkVersion != "" && inst != "" && compareVersions(inst, a.apkVersion) == 0:
		say("OpenTIE-VR %s is already installed and up to date.", inst)
		return nil
	case a.apkVersion != "" && inst != "" && compareVersions(inst, a.apkVersion) > 0:
		say("The headset has OpenTIE-VR %s, which is newer than the one that came with\nthis launcher (%s).", inst, a.apkVersion)
		if askYesNo("A newer OpenTIE-VR is installed", "Keep the newer one?", true) {
			say("Keeping OpenTIE-VR %s.", inst)
			return nil
		}
		downgrade = true
		say("Installing OpenTIE-VR %s...", bundled)
	default:
		if inst == "" {
			inst = "an older version"
		}
		say("Updating OpenTIE-VR from %s to %s...", inst, bundled)
	}

	args := []string{"install", "-r"}
	if downgrade {
		args = append(args, "-d")
	}
	args = append(args, a.apkPath)
	out, err := a.adb.run(10*time.Minute, args...)
	if strings.Contains(out, "INSTALL_FAILED_UPDATE_INCOMPATIBLE") {
		return fail("OpenTIE-VR could not be updated: the copy already on the headset was signed\n" +
			"differently (it came from another source or build).\n" +
			"\n" +
			"To fix this, OpenTIE-VR would have to be uninstalled on the headset first.\n" +
			"WARNING: uninstalling also deletes the game files and pilots that were\n" +
			"copied to the headset. The launcher does not do this for you. If you\n" +
			"want to: on the headset open Settings > Apps, uninstall OpenTIE-VR, then run\n" +
			"this launcher again.")
	}
	if err != nil || !strings.Contains(out, "Success") {
		reason := failureLine(out)
		switch {
		case strings.Contains(out, "INSTALL_FAILED_INSUFFICIENT_STORAGE"):
			return fail("The headset does not have enough free space to install OpenTIE-VR.\n" +
				"Please free up some space and run the launcher again.")
		case strings.Contains(out, "INSTALL_FAILED_VERSION_DOWNGRADE"):
			return fail("The headset has a newer OpenTIE-VR that cannot be replaced by this older one.\n" +
				"Download the newest launcher instead.")
		}
		return fail("Installing OpenTIE-VR failed:\n  %s\nCheck the USB cable and try again.", reason)
	}
	say("OpenTIE-VR %s is installed.", bundled)
	return nil
}

func failureLine(out string) string {
	for _, l := range strings.Split(out, "\n") {
		if strings.Contains(l, "Failure") || strings.Contains(l, "error") || strings.Contains(l, "failed") {
			return strings.TrimSpace(l)
		}
	}
	return strings.TrimSpace(firstLine(out))
}

// remoteHasEntry reports whether "ls <dir>" lists name (case-insensitive).
func (a *app) remoteHasEntry(dir, name string) bool {
	out, err := a.adb.shell(20*time.Second, "ls "+shQuote(dir))
	if strings.Contains(strings.ToLower(out), "permission denied") {
		logf("listing %s: permission denied", dir)
	}
	if err != nil {
		return false
	}
	for _, l := range strings.Split(out, "\n") {
		if strings.EqualFold(strings.TrimSpace(l), name) {
			return true
		}
	}
	return false
}
