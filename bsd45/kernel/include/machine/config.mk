# config.mk - kernel/include/machine
#
# kbuild-style per-directory config (tc216 Makefile/config.mk style).
# This directory is NOT part of the current kernel build (legacy 4.5BSD
# tree / placeholder). Sources are recorded as COMMENTS for reference.
#
# current file directory
MAKEFILE_DIR_include_machine_mk := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
include_machine_OBJ_DIR = $(patsubst ${MAKEFILE_DIR_project_mk}%,${OBJ_DIR}/%,$(MAKEFILE_DIR_include_machine_mk))
$(shell mkdir -p ${include_machine_OBJ_DIR})

# CPPFLAGS
CPPFLAGS := $(CPPFLAGS) -I$(MAKEFILE_DIR_include_machine_mk)

# ---- NOT BUILT: source inventory (reference only) ----
# headers: ansi.h cpu.h dbg.h dkio.h exec.h float.h frame.h limits.h mtpr.h npx.h param.h pcb.h pmap.h kprocess.h profile.h psl.h pte.h ptrace.h reg.h segments.h signal.h specialreg.h trap.h tss.h types.h vmparam.h

include_machine_OBJS :=

clean::
