package main

import (
	"context"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Steam Frame: adb over USB reaches the headset's Linux system (SteamOS),
// where Android commands and "adb install" do not work. OpenTIE-VR runs in the
// Android layer "Lepton", which listens for adb on TCP port 5555 (each
// running Lepton instance has its own port in 5555-5599) while a Lepton
// session runs ("Lepton Development" in the Steam Library). The launcher
// forwards that port over USB (adb forward + adb connect localhost:5555), or
// connects over Wi-Fi (adb connect frame:5555), and then uses that serial for
// everything else.

const leptonPortFirst, leptonPortLast = 5555, 5599

// isSteamOS checks whether the current serial is a SteamOS (Linux) shell.
func (a *app) isSteamOS() bool {
	osr, _ := a.adb.shell(20*time.Second, "cat /etc/os-release")
	un, _ := a.adb.shell(20*time.Second, "uname -a")
	logf("SteamOS check for %s:\nos-release:\n%s\nuname: %s", a.adb.Serial, osr, strings.TrimSpace(un))
	l := strings.ToLower(osr + "\n" + un)
	return strings.Contains(l, "id=steamos") || strings.Contains(l, "steamos") || strings.Contains(l, "valve")
}

// isAndroid reports whether serial answers getprop with an SDK level.
func (a *app) isAndroid(serial string) bool {
	c := *a.adb
	c.Serial = serial
	out, err := c.shell(20*time.Second, "getprop ro.build.version.sdk")
	_, perr := strconv.Atoi(firstLine(out))
	return err == nil && perr == nil
}

// pickAndroid chooses among several ready devices when exactly one of them
// is Android (for example a Steam Frame listed both over USB and as
// localhost:5555). Returns "" if that does not decide it.
func (a *app) pickAndroid(ready []adbDevice) string {
	var android []string
	for _, d := range ready {
		if a.isAndroid(d.serial) {
			android = append(android, d.serial)
		}
	}
	logf("ready devices %v; Android among them: %v", ready, android)
	if len(android) == 1 {
		return android[0]
	}
	if len(android) == 0 {
		for _, d := range ready {
			if !strings.Contains(d.serial, ":") {
				return d.serial // a USB device; checkDevice looks at it further
			}
		}
	}
	return ""
}

// connectLepton asks the user to start Lepton and connects to it.
func (a *app) connectLepton(usb string) error {
	a.frameUSB = usb
	say("Steam Frame found (its USB connection is the SteamOS side).")
	problem := ""
	for {
		ans := ui.Ask(Question{
			Kind:  "buttons",
			Title: "Steam Frame found",
			Text: "OpenTIE-VR runs in the Steam Frame's Android layer (Lepton).\n\n" +
				"Once, before starting it: in the Steam Library, open the Properties of\n" +
				"\"Lepton Development\" and set its Launch Options to:\n" +
				"    LEPTON_NO_CLEANUP=1 %command%\n\n" +
				"Without this, closing Lepton Development deletes OpenTIE-VR and the copied\n" +
				"game files; with it they should stay. (If it is running already, close it\n" +
				"first so the setting applies.)\n\n" +
				"Then start \"Lepton Development\" on the headset (keep it running) and\n" +
				"press Continue.\n\n" +
				"Developer Mode must be on. A PC USB port may not power the headset for\n" +
				"long; a powered USB-C cable or charger helps.",
			Problem: problem,
			Options: []Option{
				{ID: "continue", Label: "Continue", Key: "", Style: "primary"},
				{ID: "wifi", Label: "Use Wi-Fi instead", Key: "w"},
				{ID: "cancel", Label: "Cancel", Key: "x"},
			},
			Default: "continue",
		})
		if ans.Closed {
			if appCtx.Err() != nil {
				return errClosed
			}
			return fail("The Steam Frame's Android layer (Lepton) was not connected.")
		}
		var err error
		switch ans.ID {
		case "cancel":
			return fail("OpenTIE-VR needs the Steam Frame's Android layer. Start \"Lepton Development\"\n" +
				"on the headset and try again.")
		case "wifi":
			err = a.leptonViaWiFi()
		default:
			err = a.leptonViaUSB(usb)
		}
		if err == nil {
			return nil
		}
		if err == errClosed {
			return err
		}
		problem = err.Error()
		logf("Lepton connection failed: %s", problem)
	}
}

var portRe = regexp.MustCompile(`[:.](\d{4})\s`)

// leptonPorts lists Lepton adb ports (5555-5599) that SteamOS listens on.
func (a *app) leptonPorts(usb string) []int {
	c := *a.adb
	c.Serial = usb
	out, _ := c.shell(20*time.Second, "ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null")
	ports := parseLeptonPorts(out)
	logf("Lepton ports listening on the headset: %v", ports)
	return ports
}

// parseLeptonPorts finds listening ports 5555-5599 in ss/netstat output.
func parseLeptonPorts(out string) []int {
	seen := map[int]bool{}
	var ports []int
	for _, line := range strings.Split(out, "\n") {
		if !strings.Contains(strings.ToUpper(line), "LISTEN") {
			continue
		}
		for _, m := range portRe.FindAllStringSubmatch(line+" ", -1) {
			p, _ := strconv.Atoi(m[1])
			if p >= leptonPortFirst && p <= leptonPortLast && !seen[p] {
				seen[p] = true
				ports = append(ports, p)
			}
		}
	}
	sort.Ints(ports)
	return ports
}

// leptonViaUSB forwards the Lepton port over USB and connects to it.
func (a *app) leptonViaUSB(usb string) error {
	say("Connecting to the Steam Frame's Android layer over USB...")
	tried := map[int]bool{}
	ports := []int{leptonPortFirst}
	var last error
	for len(ports) > 0 {
		p := ports[0]
		ports = ports[1:]
		if tried[p] {
			continue
		}
		tried[p] = true
		port := strconv.Itoa(p)
		c := *a.adb
		c.Serial = usb
		if out, err := c.run(20*time.Second, "forward", "tcp:"+port, "tcp:"+port); err != nil {
			if err == errClosed {
				return err
			}
			logf("forward of port %d failed: %s", p, strings.TrimSpace(out))
			last = fail("Could not forward the Lepton port %d over USB.", p)
			continue
		}
		serial := "localhost:" + port
		if err := a.leptonConnect(serial, 20*time.Second); err == nil {
			a.forwardPort = p
			return nil
		} else {
			last = err
			if err == errClosed {
				return err
			}
		}
		c.run(20*time.Second, "forward", "--remove", "tcp:"+port)
		if p == leptonPortFirst {
			ports = append(ports, a.leptonPorts(usb)...)
		}
	}
	if last == nil {
		last = fail("The Android layer (Lepton) is not running.")
	}
	return fail("%s.\nMake sure \"Lepton Development\" is running on the headset (Steam Library),\n"+
		"then press Continue again.", strings.TrimSuffix(last.Error(), "."))
}

// leptonViaWiFi asks for the headset's name or IP address and connects.
func (a *app) leptonViaWiFi() error {
	ans := ui.Ask(Question{
		Kind:  "text",
		Title: "Connect over Wi-Fi",
		Text: "Enter the headset's name or IP address (shown in Quick Settings or in\n" +
			"Steam Settings > Internet). The PC must be on the same network.",
		Value:         "frame",
		Placeholder:   "frame or 192.168.1.23",
		Options:       []Option{{ID: "ok", Label: "Connect", Style: "primary"}},
		ConsolePrompt: "Headset name or IP address (Enter = frame):",
	})
	if ans.Closed && appCtx.Err() != nil {
		return errClosed
	}
	host := strings.TrimSpace(ans.Text)
	if host == "" {
		host = "frame"
	}
	if !strings.Contains(host, ":") {
		host += ":" + strconv.Itoa(leptonPortFirst)
	}
	say("Connecting to %s over Wi-Fi...", host)
	return a.leptonConnect(host, 20*time.Second)
}

// leptonConnect runs "adb connect target" and waits until target answers as
// an Android device. An unauthorized connection waits for the user to allow
// it in the Lepton window.
func (a *app) leptonConnect(target string, wait time.Duration) error {
	c := *a.adb
	c.Serial = ""
	deadline := time.Now().Add(wait)
	shownAuth := false
	for attempt := 1; ; attempt++ {
		out, err := c.run(20*time.Second, "connect", target)
		if err == errClosed {
			return err
		}
		connected := strings.Contains(out, "connected to") && !strings.Contains(out, "failed") && !strings.Contains(out, "cannot")
		logf("adb connect %s (attempt %d): connected=%v", target, attempt, connected)
		if connected {
			l := *a.adb
			l.Serial = target
			sdk, _ := l.shell(20*time.Second, "getprop ro.build.version.sdk")
			if _, perr := strconv.Atoi(firstLine(sdk)); perr == nil {
				a.adb.Serial = target
				a.lepton = target
				logf("Lepton connected at %s (SDK %s)", target, firstLine(sdk))
				return nil
			}
			if strings.Contains(strings.ToLower(sdk), "unauthorized") {
				if !shownAuth {
					say("Look at the Lepton window on the headset and choose Allow (tick \"Always\n" +
						"allow from this computer\").")
					shownAuth = true
					deadline = time.Now().Add(90 * time.Second)
				}
			}
		}
		if time.Now().After(deadline) {
			c.run(10*time.Second, "disconnect", target)
			if shownAuth {
				return fail("The Android layer at %s did not allow this computer.", target)
			}
			if strings.Contains(out, "refused") || strings.Contains(out, "failed") || strings.Contains(out, "cannot") {
				logf("adb connect %s failed: %s", target, strings.TrimSpace(out))
				return fail("The Android layer (Lepton) did not answer at %s (%s)", target, connectReason(out))
			}
			return fail("The Android layer at %s does not answer.", target)
		}
		if !sleepCtx(2 * time.Second) {
			return errClosed
		}
	}
}

// reconnectLepton redoes the forward and connect after adb was restarted.
func (a *app) reconnectLepton() {
	if a.lepton == "" {
		return
	}
	logf("reconnecting Lepton at %s", a.lepton)
	if a.forwardPort > 0 && a.frameUSB != "" {
		c := *a.adb
		c.Serial = a.frameUSB
		p := "tcp:" + strconv.Itoa(a.forwardPort)
		c.run(20*time.Second, "forward", p, p)
	}
	c := *a.adb
	c.Serial = ""
	c.run(20*time.Second, "connect", a.lepton)
}

// cleanupLepton removes the connection and port forward again at exit. It
// runs even when the launcher is stopping (window closed).
func (a *app) cleanupLepton() {
	if a.adb == nil || (a.lepton == "" && a.forwardPort == 0) {
		return
	}
	c := *a.adb
	c.ctx = context.Background()
	if a.lepton != "" {
		c.Serial = ""
		c.run(10*time.Second, "disconnect", a.lepton)
	}
	if a.forwardPort > 0 && a.frameUSB != "" {
		c.Serial = a.frameUSB
		c.run(10*time.Second, "forward", "--remove", "tcp:"+strconv.Itoa(a.forwardPort))
	}
}

// connectReason shortens adb connect's error ("Connection refused", ...).
func connectReason(out string) string {
	l := firstLine(out)
	if i := strings.LastIndex(l, ": "); i >= 0 {
		l = l[i+2:]
	}
	return strings.ToLower(strings.TrimSuffix(l, "."))
}
