# config.mk - kernel/include/sys
#
# kbuild-style per-directory config (tc216 Makefile/config.mk style).
# This directory is NOT part of the current kernel build (legacy 4.5BSD
# tree / placeholder). Sources are recorded as COMMENTS for reference.
#
# current file directory
MAKEFILE_DIR_include_sys_mk := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include_sys_OBJ_DIR = $(patsubst ${MAKEFILE_DIR_project_mk}%,${OBJ_DIR}/%,$(MAKEFILE_DIR_include_sys_mk))
$(shell mkdir -p ${include_sys_OBJ_DIR})

# CPPFLAGS
CPPFLAGS := $(CPPFLAGS) -I$(MAKEFILE_DIR_include_sys_mk)

# ---- NOT BUILT: source inventory (reference only) ----
# headers: acct.h buf.h callout.h cdefs.h clist.h conf.h device.h dir.h dirent.h disk.h disklabel.h dkbad.h dkstat.h dmap.h domain.h errno.h exec.h fbio.h fcntl.h file.h filedesc.h filio.h gmon.h ioccom.h ioctl.h ioctl_compat.h ipc.h kernel.h kproc.h ktrace.h lock.h malloc.h map.h mbuf.h mman.h mount.h msgbuf.h mtio.h namei.h param.h platform.h kprocess.h protosw.h ptrace.h queue.h reboot.h resource.h resourcevar.h select.h signal.h signalvar.h smp.h socket.h socketvar.h sockio.h stat.h syscall.h syscallargs.h sysctl.h syslimits.h syslog.h systm.h tablet.h termios.h time.h timeb.h times.h tprintf.h trace.h tty.h ttychars.h ttycom.h ttydefaults.h ttydev.h types.h ucred.h uio.h un.h unistd.h unpcb.h user.h utsname.h vadvise.h vcmd.h vlimit.h vmmeter.h vnode.h vsio.h wait.h

include_sys_OBJS :=

clean::
