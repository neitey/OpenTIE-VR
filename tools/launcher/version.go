package main

import (
	"strconv"
	"strings"
)

// semver is a simplified version: major.minor.patch with an optional
// pre-release suffix such as "-rc3". Anything unparsable (like "dev")
// counts as 0.0.0.
type semver struct {
	nums   [3]int
	pre    bool // has a suffix: older than the same numbers without one
	preNum int  // the number in the suffix: rc3 -> 3
}

func parseVersion(s string) semver {
	var v semver
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(strings.TrimPrefix(s, "v"), "V")
	if i := strings.IndexByte(s, '+'); i >= 0 { // build metadata is ignored
		s = s[:i]
	}
	main, suffix, hasSuffix := strings.Cut(s, "-")
	parts := strings.Split(main, ".")
	for i := 0; i < len(parts) && i < 3; i++ {
		n, err := strconv.Atoi(parts[i])
		if err != nil {
			return semver{}
		}
		v.nums[i] = n
	}
	if hasSuffix {
		v.pre = true
		digits := strings.Map(func(r rune) rune {
			if r >= '0' && r <= '9' {
				return r
			}
			return -1
		}, suffix)
		v.preNum, _ = strconv.Atoi(digits)
	}
	return v
}

// compareVersions returns -1, 0 or 1. 1.0.0 > 1.0.0-rc3 > 1.0.0-rc2.
func compareVersions(a, b string) int {
	va, vb := parseVersion(a), parseVersion(b)
	for i := 0; i < 3; i++ {
		if va.nums[i] != vb.nums[i] {
			return cmpInt(va.nums[i], vb.nums[i])
		}
	}
	if va.pre != vb.pre {
		if va.pre {
			return -1
		}
		return 1
	}
	return cmpInt(va.preNum, vb.preNum)
}

func cmpInt(a, b int) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}
