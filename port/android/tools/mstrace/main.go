// mstrace: a tiny strace for Android, where no strace binary exists.
//
//	mstrace [-all] PROGRAM [ARGS...]
//
// Runs PROGRAM under ptrace (threads included). On exit it prints how the
// program ended and the last 40 syscalls with their results to stderr; with
// -all every syscall is printed as it happens. Built for linux/arm64 only.
package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

const (
	ptraceSetOptions = 0x4200
	ptraceGetRegSet  = 0x4204
	ntPrstatus       = 1
	optTraceSysgood  = 1
	optTraceClone    = 8
	optExitKill      = 0x100000
	waitAll          = 0x40000000
	ringSize         = 40
)

var names = map[uint64]string{
	17: "getcwd", 23: "dup", 24: "dup3", 25: "fcntl", 29: "ioctl", 34: "mkdirat", 35: "unlinkat",
	43: "statfs", 46: "ftruncate", 48: "faccessat", 56: "openat", 57: "close", 61: "getdents64",
	62: "lseek", 63: "read", 64: "write", 66: "writev", 67: "pread64", 73: "ppoll", 78: "readlinkat",
	79: "newfstatat", 80: "fstat", 93: "exit", 94: "exit_group", 96: "set_tid_address", 98: "futex",
	99: "set_robust_list", 113: "clock_gettime", 115: "clock_nanosleep", 129: "kill", 131: "tgkill",
	132: "sigaltstack", 134: "rt_sigaction", 135: "rt_sigprocmask", 160: "uname", 167: "prctl",
	169: "gettimeofday", 172: "getpid", 178: "gettid", 214: "brk", 215: "munmap", 216: "mremap",
	220: "clone", 221: "execve", 222: "mmap", 226: "mprotect", 233: "madvise", 261: "prlimit64",
	278: "getrandom", 279: "memfd_create", 293: "rseq", 435: "clone3", 436: "close_range",
}

// Syscalls with a path argument worth printing: number -> argument index.
var pathArg = map[uint64]int{
	34: 1, 35: 1, 43: 0, 48: 1, 56: 1, 78: 1, 79: 1, 221: 0,
}

type iovec struct {
	base unsafe.Pointer
	n    uintptr
}

type thread struct {
	inSyscall bool
	nr        uint64
	args      [4]uint64
	path      string
}

func getRegs(tid int, r *[34]uint64) error {
	iov := iovec{unsafe.Pointer(&r[0]), unsafe.Sizeof(*r)}
	_, _, e := syscall.Syscall6(syscall.SYS_PTRACE, ptraceGetRegSet, uintptr(tid), ntPrstatus,
		uintptr(unsafe.Pointer(&iov)), 0, 0)
	if e != 0 {
		return e
	}
	return nil
}

func readString(tid int, addr uint64) string {
	var out []byte
	for len(out) < 120 {
		var w [8]byte
		if _, err := syscall.PtracePeekData(tid, uintptr(addr)+uintptr(len(out)), w[:]); err != nil {
			break
		}
		for _, c := range w {
			if c == 0 {
				return string(out)
			}
			out = append(out, c)
		}
	}
	return string(out)
}

// scanOutput digs unflushed stdio text out of the tracee's writable memory. qemu-user prints
// its loader errors with printf and leaves through _exit, so with stdout redirected to a file
// the message never reaches it.
func scanOutput(pid int) []string {
	maps, err := os.ReadFile(fmt.Sprintf("/proc/%d/maps", pid))
	if err != nil {
		return nil
	}
	mem, err := os.Open(fmt.Sprintf("/proc/%d/mem", pid))
	if err != nil {
		return nil
	}
	defer mem.Close()
	keys := [][]byte{[]byte("Error while loading"), []byte("qemu: "), []byte("qemu-arm: ")}
	var found []string
	for _, line := range strings.Split(string(maps), "\n") {
		f := strings.Fields(line)
		if len(f) < 5 || !strings.HasPrefix(f[1], "rw") || (len(f) >= 6 && f[5] != "[heap]") {
			continue
		}
		var lo, hi int64
		if _, err := fmt.Sscanf(f[0], "%x-%x", &lo, &hi); err != nil || hi-lo > 256<<20 {
			continue
		}
		buf := make([]byte, hi-lo)
		n, _ := mem.ReadAt(buf, lo)
		buf = buf[:n]
		for _, k := range keys {
			for off := 0; off < len(buf); {
				i := bytes.Index(buf[off:], k)
				if i < 0 {
					break
				}
				start := off + i
				end := start
				for end < len(buf) && end-start < 300 && buf[end] != '\n' && buf[end] != 0 {
					end++
				}
				found = append(found, string(buf[start:end]))
				off = end + 1
			}
		}
	}
	return found
}

func describe(t *thread, tid int, ret int64) string {
	name, ok := names[t.nr]
	if !ok {
		name = fmt.Sprintf("syscall_%d", t.nr)
	}
	s := fmt.Sprintf("[%d] %s(%#x, %#x, %#x, %#x)", tid, name, t.args[0], t.args[1], t.args[2], t.args[3])
	if t.path != "" {
		s += fmt.Sprintf(" path=%q", t.path)
	}
	if ret < 0 && ret > -4096 {
		return fmt.Sprintf("%s = -1 %s", s, syscall.Errno(-ret))
	}
	return fmt.Sprintf("%s = %#x", s, uint64(ret))
}

func main() {
	runtime.LockOSThread()
	args := os.Args[1:]
	all := len(args) > 0 && args[0] == "-all"
	if all {
		args = args[1:]
	}
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: mstrace [-all] PROGRAM [ARGS...]")
		os.Exit(2)
	}

	cmd := exec.Command(args[0], args[1:]...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Ptrace: true}
	if err := cmd.Start(); err != nil {
		fmt.Fprintln(os.Stderr, "mstrace: start:", err)
		os.Exit(127)
	}
	pid := cmd.Process.Pid

	var ws syscall.WaitStatus
	if _, err := syscall.Wait4(pid, &ws, 0, nil); err != nil || !ws.Stopped() {
		fmt.Fprintln(os.Stderr, "mstrace: child did not stop at exec:", err, ws)
		os.Exit(127)
	}
	if _, _, e := syscall.Syscall6(syscall.SYS_PTRACE, ptraceSetOptions, uintptr(pid), 0,
		optTraceSysgood|optTraceClone|optExitKill, 0, 0); e != 0 {
		fmt.Fprintln(os.Stderr, "mstrace: PTRACE_SETOPTIONS:", e)
	}
	syscall.PtraceSyscall(pid, 0)

	threads := map[int]*thread{}
	var ring []string
	record := func(s string) {
		if all {
			fmt.Fprintln(os.Stderr, s)
		}
		ring = append(ring, s)
		if len(ring) > ringSize {
			ring = ring[1:]
		}
	}

	status := "unknown"
	exitCode := 1
loop:
	for {
		tid, err := syscall.Wait4(-1, &ws, waitAll, nil)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			status = "wait4: " + err.Error()
			break
		}
		switch {
		case ws.Exited() || ws.Signaled():
			if tid != pid {
				delete(threads, tid)
				continue
			}
			if ws.Exited() {
				exitCode = ws.ExitStatus()
				status = fmt.Sprintf("exited with status %d", exitCode)
			} else {
				exitCode = 128 + int(ws.Signal())
				status = fmt.Sprintf("killed by signal %d (%s)", int(ws.Signal()), ws.Signal())
			}
			break loop
		case ws.Stopped():
			sig := ws.StopSignal()
			t := threads[tid]
			if t == nil {
				t = &thread{}
				threads[tid] = t
			}
			switch sig {
			case syscall.SIGTRAP | 0x80:
				var r [34]uint64
				if getRegs(tid, &r) == nil {
					if !t.inSyscall {
						t.inSyscall = true
						t.nr = r[8]
						copy(t.args[:], r[:4])
						t.path = ""
						if i, ok := pathArg[t.nr]; ok {
							t.path = readString(tid, t.args[i])
						}
						if (t.nr == 93 || t.nr == 94) && t.args[0] != 0 {
							for _, s := range scanOutput(tid) {
								record("[unflushed stdout/stderr] " + s)
							}
						}
					} else {
						t.inSyscall = false
						record(describe(t, tid, int64(r[0])))
					}
				}
				syscall.PtraceSyscall(tid, 0)
			case syscall.SIGTRAP, syscall.SIGSTOP:
				syscall.PtraceSyscall(tid, 0)
			default:
				record(fmt.Sprintf("[%d] --- signal %d (%s) ---", tid, int(sig), sig))
				syscall.PtraceSyscall(tid, int(sig))
			}
		}
	}

	fmt.Fprintf(os.Stderr, "\nmstrace: %s %s\n--- last %d syscalls:\n", args[0], status, len(ring))
	for _, s := range ring {
		fmt.Fprintln(os.Stderr, s)
	}
	os.Exit(exitCode)
}
