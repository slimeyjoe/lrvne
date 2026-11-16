# SPDX-License-Identifier: GPL-3.0-or-later
# The rigs' checks of the Amiga family's targets (included at the end of mk/lrvne.mk through
# LRVNE_TARGET_POST, set in mk/amiga.mk; rigs/README.md):
#
#   make TARGET=amiga|warpos|aros lrvne-present      the presentation test on the machine
#                                                     (WinUAE, WinUAE + WarpOS, hosted AROS)
#                                                     against the headless build, made first
#   make TARGET=amiga|aros lrvne-dist-check           the release archive (lrvne-dist) unpacked
#                                                     and run on the machine
#   make lrvne-rig-tools                              the guest helpers (vbcc, the AROS gcc)
#
# An engine sets, as it needs:
#
#   PAK                      the pack the program runs with (also the engine's own variable)
#   LRVNE_HOST_EXE           the headless build (default: host/ beside $(BUILD))
#   LRVNE_PRESENT_ARGS       more options for rigs/present.py (--configs window, --guest-args ..)
#   LRVNE_DIST_ARGS          the program options of the dist run (default AUDIO=none FAST
#                            MAXTICKS=200; AROS without FAST)
#
# The machines' settings (where WinUAE, the ROMs, the Workbench tree and AROS are) come from the
# environment or the rig configuration (rigs/rig.conf.example).

LRVNE_RIGS     := $(LRVNE)/rigs
LRVNE_HOST_EXE ?= $(dir $(BUILD))host/$(APP_NAME)
LRVNE_RIG_PY   ?= python3 -I
# hosted AROS run directories (a copy of the system each, pruned after the run)
export RIG_RUNS ?= $(abspath $(dir $(BUILD)))/rig-runs

ifeq ($(TARGET),aros)
LRVNE_RIG_MACHINE := aros
else ifeq ($(TARGET),warpos)
LRVNE_RIG_MACHINE := warpos
else
LRVNE_RIG_MACHINE := winuae
endif

.PHONY: lrvne-present lrvne-dist-check lrvne-rig-tools
lrvne-rig-tools:
	$(Q)$(if $(filter aros,$(TARGET)),$(MAKE) --no-print-directory -C $(LRVNE_RIGS)/aros,$(MAKE) --no-print-directory -C $(LRVNE_RIGS)/winuae)

lrvne-present: $(EXE) lrvne-rig-tools
	$(Q)$(MAKE) --no-print-directory TARGET=host
	$(Q)$(LRVNE_RIG_PY) $(LRVNE_RIGS)/present.py $(LRVNE_RIG_MACHINE) --host-bin $(LRVNE_HOST_EXE) \
	    --program $(EXE) $(if $(PAK),--pak $(PAK)) --out $(BUILD)/present $(LRVNE_PRESENT_ARGS)

lrvne-dist-check: lrvne-dist lrvne-rig-tools
	$(Q)$(if $(filter warpos,$(TARGET)),echo "lrvne-dist-check: no WarpOS rig for the archive"; exit 1,true)
	$(Q)$(if $(LRVNE_DIST_ARGS),DIST_ARGS="$(LRVNE_DIST_ARGS)") \
	    $(LRVNE_RIGS)/$(if $(filter aros,$(TARGET)),aros,winuae)/check-dist.sh $(LRVNE_DIST) \
	    $(LRVNE_DIST_STAGE) $(LRVNE_DIST_DRAWER) $(APP_NAME) $(APP_VERSION) $(PAK)
