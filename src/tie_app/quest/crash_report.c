/* Native crash report for the Quest build: on a fatal signal, writes the
 * fault and a backtrace (module + offset per frame) to a text file next to
 * opentie.log, so a tester can send it back without adb. The file is only
 * written on a crash and survives the next launch (opentie.log does not). */

#include "crash_report.h"

#if defined(__ANDROID__) || defined(__linux__)

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#ifndef TIE_VERSION_STRING
#define TIE_VERSION_STRING "unknown"
#endif

#define CR_MAX_FRAMES 48

static char s_path[1024];
static volatile sig_atomic_t s_in_handler;
static struct sigaction s_old[NSIG];
static char s_alt_stack[64 * 1024];

static const int k_signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };

/* ---- async-signal-safe formatting -------------------------------------- */

typedef struct CrBuf {
	char text[8192];
	size_t n;
} CrBuf;

static void cr_str(CrBuf* b, const char* s) {
	while (s && *s && b->n + 1 < sizeof b->text) {
		b->text[b->n++] = *s++;
	}
}

static void cr_hex(CrBuf* b, uintptr_t v) {
	char tmp[2 + 16];
	int n = 0;
	cr_str(b, "0x");
	do {
		tmp[n++] = "0123456789abcdef"[v & 15u];
		v >>= 4;
	} while (v && n < 16);
	while (n > 0 && b->n + 1 < sizeof b->text) {
		b->text[b->n++] = tmp[--n];
	}
}

static void cr_dec(CrBuf* b, long v) {
	char tmp[24];
	int n = 0;
	unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
	if (v < 0) {
		cr_str(b, "-");
	}
	do {
		tmp[n++] = (char)('0' + u % 10u);
		u /= 10u;
	} while (u && n < 23);
	while (n > 0 && b->n + 1 < sizeof b->text) {
		b->text[b->n++] = tmp[--n];
	}
}

static const char* cr_signame(int sig) {
	switch (sig) {
		case SIGSEGV:
			return "SIGSEGV";
		case SIGBUS:
			return "SIGBUS";
		case SIGFPE:
			return "SIGFPE";
		case SIGILL:
			return "SIGILL";
		case SIGABRT:
			return "SIGABRT";
		case SIGTRAP:
			return "SIGTRAP";
		default:
			return "signal";
	}
}

/* One frame: "#nn pc 0x... module+0xoffset (symbol+0x..)". */
static void cr_frame(CrBuf* b, int index, uintptr_t pc) {
	Dl_info info;
	cr_str(b, "#");
	if (index < 10) {
		cr_str(b, "0");
	}
	cr_dec(b, index);
	cr_str(b, " pc ");
	cr_hex(b, pc);
	memset(&info, 0, sizeof info);
	if (pc && dladdr((void*)pc, &info) && info.dli_fname) {
		const char* name = strrchr(info.dli_fname, '/');
		cr_str(b, " ");
		cr_str(b, name ? name + 1 : info.dli_fname);
		cr_str(b, "+");
		cr_hex(b, pc - (uintptr_t)info.dli_fbase);
		if (info.dli_sname) {
			cr_str(b, " (");
			cr_str(b, info.dli_sname);
			cr_str(b, "+");
			cr_hex(b, pc - (uintptr_t)info.dli_saddr);
			cr_str(b, ")");
		}
	}
	cr_str(b, "\n");
}

typedef struct CrUnwind {
	uintptr_t pcs[CR_MAX_FRAMES];
	int count;
} CrUnwind;

static _Unwind_Reason_Code cr_unwind_cb(struct _Unwind_Context* ctx, void* arg) {
	CrUnwind* u = (CrUnwind*)arg;
	const uintptr_t pc = (uintptr_t)_Unwind_GetIP(ctx);
	if (pc) {
		u->pcs[u->count++] = pc;
	}
	return u->count >= CR_MAX_FRAMES ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void cr_handler(int sig, siginfo_t* info, void* uctx) {
	CrBuf b;
	CrUnwind u;
	if (s_in_handler) {
		_exit(128 + sig);
	}
	s_in_handler = 1;
	b.n = 0;
	cr_str(&b, "OpenTIE-VR " TIE_VERSION_STRING " crashed: ");
	cr_str(&b, cr_signame(sig));
	cr_str(&b, " (signal ");
	cr_dec(&b, sig);
	cr_str(&b, ", code ");
	cr_dec(&b, info ? info->si_code : 0);
	cr_str(&b, ") fault address ");
	cr_hex(&b, info ? (uintptr_t)info->si_addr : 0);
	cr_str(&b, "\nthread ");
	cr_dec(&b, (long)gettid());
	cr_str(&b, "\n");
#if defined(__aarch64__)
	if (uctx) {
		const ucontext_t* uc = (const ucontext_t*)uctx;
		cr_str(&b, "registers: pc/lr at the fault\n");
		cr_frame(&b, 0, (uintptr_t)uc->uc_mcontext.pc);
		cr_frame(&b, 1, (uintptr_t)uc->uc_mcontext.regs[30]);
	}
#else
	(void)uctx;
#endif
	cr_str(&b, "backtrace:\n");
	u.count = 0;
	_Unwind_Backtrace(cr_unwind_cb, &u);
	for (int i = 0; i < u.count; ++i) {
		cr_frame(&b, i, u.pcs[i]);
	}
	if (s_path[0]) {
		const int fd = open(s_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			ssize_t w = write(fd, b.text, b.n);
			(void)w;
			close(fd);
		}
	}
	/* Also into the system log / tombstone flow: hand the signal back to
	 * the previous handler (Android's debuggerd) or the default action. */
	sigaction(sig, &s_old[sig], NULL);
	raise(sig);
}

void TieCrashReport_Install(const char* path) {
	stack_t ss;
	if (!path || !path[0]) {
		return;
	}
	strncpy(s_path, path, sizeof s_path - 1);
	memset(&ss, 0, sizeof ss);
	ss.ss_sp = s_alt_stack;
	ss.ss_size = sizeof s_alt_stack;
	sigaltstack(&ss, NULL);
	for (size_t i = 0; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = cr_handler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(k_signals[i], &sa, &s_old[k_signals[i]]);
	}
}


#else

void TieCrashReport_Install(const char* path) { (void)path; }

#endif
