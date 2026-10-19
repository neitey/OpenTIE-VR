//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// Minimal read-only registry access with the syscall package (no
// golang.org/x/sys). Paths are opened as given; a 64-bit process reads the
// 32-bit view through the explicit WOW6432Node paths.

var procRegEnumValueW = syscall.NewLazyDLL("advapi32.dll").NewProc("RegEnumValueW")

const errorMoreData = syscall.Errno(234)

func regOpen(root syscall.Handle, path string) (syscall.Handle, bool) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, false
	}
	var h syscall.Handle
	if err := syscall.RegOpenKeyEx(root, p, 0, syscall.KEY_READ, &h); err != nil {
		return 0, false
	}
	return h, true
}

// regString reads a REG_SZ / REG_EXPAND_SZ value.
func regString(h syscall.Handle, name string) (string, bool) {
	np, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return "", false
	}
	var typ, n uint32
	if err := syscall.RegQueryValueEx(h, np, nil, &typ, nil, &n); err != nil {
		return "", false
	}
	if typ != syscall.REG_SZ && typ != syscall.REG_EXPAND_SZ {
		return "", false
	}
	if n == 0 {
		return "", true
	}
	buf := make([]uint16, n/2+1)
	if err := syscall.RegQueryValueEx(h, np, nil, &typ, (*byte)(unsafe.Pointer(&buf[0])), &n); err != nil {
		return "", false
	}
	s := syscall.UTF16ToString(buf)
	if typ == syscall.REG_EXPAND_SZ {
		s = expandWindowsEnv(s)
	}
	return s, true
}

// regSubkeys lists the names of the direct sub-keys.
func regSubkeys(h syscall.Handle) []string {
	var out []string
	for i := uint32(0); ; i++ {
		var buf [256]uint16
		n := uint32(len(buf))
		err := syscall.RegEnumKeyEx(h, i, &buf[0], &n, nil, nil, nil, nil)
		if err != nil {
			break // ERROR_NO_MORE_ITEMS or a real error: stop either way
		}
		out = append(out, syscall.UTF16ToString(buf[:n]))
	}
	return out
}

// regStringValues returns all string values of a key (RegEnumValueW is not in
// the syscall package, so it is called through advapi32 directly).
func regStringValues(h syscall.Handle) []string {
	var out []string
	for i := uint32(0); ; i++ {
		name := make([]uint16, 16384)
		nameLen := uint32(len(name))
		data := make([]uint16, 8192)
		dataLen := uint32(len(data) * 2)
		var typ uint32
		r, _, _ := procRegEnumValueW.Call(uintptr(h), uintptr(i),
			uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(&nameLen)), 0,
			uintptr(unsafe.Pointer(&typ)), uintptr(unsafe.Pointer(&data[0])), uintptr(unsafe.Pointer(&dataLen)))
		if r == uintptr(errorMoreData) {
			continue // value too big to be a folder path
		}
		if r != 0 {
			break
		}
		if typ != syscall.REG_SZ && typ != syscall.REG_EXPAND_SZ {
			continue
		}
		s := syscall.UTF16ToString(data[:dataLen/2])
		if typ == syscall.REG_EXPAND_SZ {
			s = expandWindowsEnv(s)
		}
		out = append(out, s)
	}
	return out
}

// expandWindowsEnv replaces %NAME% with environment variables.
func expandWindowsEnv(s string) string {
	var b strings.Builder
	for {
		i := strings.IndexByte(s, '%')
		if i < 0 {
			break
		}
		j := strings.IndexByte(s[i+1:], '%')
		if j < 0 {
			break
		}
		name := s[i+1 : i+1+j]
		b.WriteString(s[:i])
		if v, ok := os.LookupEnv(name); ok && name != "" {
			b.WriteString(v)
		} else {
			b.WriteString(s[i : i+2+j])
		}
		s = s[i+2+j:]
	}
	b.WriteString(s)
	return b.String()
}

func windowsSteamRoots() []string {
	var roots []string
	if h, ok := regOpen(syscall.HKEY_CURRENT_USER, `Software\Valve\Steam`); ok {
		if s, ok := regString(h, "SteamPath"); ok && s != "" {
			roots = append(roots, filepath.FromSlash(s))
		}
		syscall.RegCloseKey(h)
	}
	for _, p := range []string{`SOFTWARE\WOW6432Node\Valve\Steam`, `SOFTWARE\Valve\Steam`} {
		if h, ok := regOpen(syscall.HKEY_LOCAL_MACHINE, p); ok {
			if s, ok := regString(h, "InstallPath"); ok && s != "" {
				roots = append(roots, s)
			}
			syscall.RegCloseKey(h)
		}
	}
	logf("Steam folders from the registry: %q", roots)
	return roots
}

// windowsRegistryGameDirs finds GOG and original CD installs whose name has
// one of the keywords (lower case).
func windowsRegistryGameDirs(keywords []string) []string {
	var dirs []string
	// GOG: one key per game with gameName / path / exe values.
	for _, base := range []string{`SOFTWARE\WOW6432Node\GOG.com\Games`, `SOFTWARE\GOG.com\Games`} {
		h, ok := regOpen(syscall.HKEY_LOCAL_MACHINE, base)
		if !ok {
			continue
		}
		for _, id := range regSubkeys(h) {
			g, ok := regOpen(h, id)
			if !ok {
				continue
			}
			name, _ := regString(g, "gameName")
			if hasKeyword(name, keywords) {
				if p, ok := regString(g, "path"); ok && p != "" {
					dirs = append(dirs, p)
				}
				if exe, ok := regString(g, "exe"); ok && exe != "" {
					dirs = append(dirs, filepath.Dir(exe))
				}
			}
			syscall.RegCloseKey(g)
		}
		syscall.RegCloseKey(h)
	}
	// Original CD install: LucasArts keys mentioning the game.
	for _, base := range []string{`SOFTWARE\WOW6432Node\LucasArts Entertainment Company LLC`, `SOFTWARE\LucasArts Entertainment Company LLC`} {
		h, ok := regOpen(syscall.HKEY_LOCAL_MACHINE, base)
		if !ok {
			continue
		}
		dirs = append(dirs, lucasArtsDirs(h, keywords, false, 0)...)
		syscall.RegCloseKey(h)
	}
	logf("game folders from the registry: %q", dirs)
	return dirs
}

func lucasArtsDirs(h syscall.Handle, keywords []string, inGame bool, depth int) []string {
	var dirs []string
	if inGame {
		for _, v := range regStringValues(h) {
			v = strings.TrimSpace(v)
			if v != "" && isDir(v) {
				dirs = append(dirs, v)
			}
		}
	}
	if depth >= 5 {
		return dirs
	}
	for _, name := range regSubkeys(h) {
		sub, ok := regOpen(h, name)
		if !ok {
			continue
		}
		dirs = append(dirs, lucasArtsDirs(sub, keywords, inGame || hasKeyword(name, keywords), depth+1)...)
		syscall.RegCloseKey(sub)
	}
	return dirs
}
