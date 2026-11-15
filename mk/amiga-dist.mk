# SPDX-License-Identifier: GPL-3.0-or-later
# Release archives of the Amiga family's targets (included at the end of mk/lrvne.mk through
# LRVNE_TARGET_POST, set in mk/amiga.mk).
#
#   make TARGET=amiga|warpos|aros lrvne-dist
#
# makes $(BUILD)/<APP_NAME>-<APP_VERSION>-<LRVNE_DIST_NAME>.lha, which unpacks to a drawer with
# the program in it and, where the engine supplies them, the icons and a ReadMe:
#
#   <drawer>.info            the drawer's icon                   LRVNE_DIST_DRAWER_ICON
#   <drawer>/<APP_NAME>      the program (stripped where the target says how)
#   <drawer>/<APP_NAME>.info its icon, with the tool types       LRVNE_DIST_ICON
#   <drawer>/ReadMe.txt      the parts, concatenated             LRVNE_DIST_README_PARTS
#   <drawer>/...             anything else                       LRVNE_DIST_FILES
#
# An engine sets, before including mk/lrvne.mk, what it has of
#
#   APP_VERSION              the version in the archive's name (required for lrvne-dist)
#   LRVNE_DIST_DRAWER        the drawer's name (default: APP_NAME)
#   LRVNE_DIST_ICON, LRVNE_DIST_DRAWER_ICON    .info files; they may be made by rules of the
#                            engine (icons are artwork: lrvne has none and makes none)
#   LRVNE_DIST_README_PARTS  text files, in order; plain ASCII reads everywhere on an Amiga.
#                            Typically the parts all flavours share and the parts of one
#                            (requirements, installation), chosen by $(TARGET)
#   LRVNE_DIST_FILES         more files for the drawer
#   LRVNE_DIST_UPLOAD_README a template of an upload description (.readme next to the archive,
#                            as Amiga software archives want one); @VERSION@ is replaced
#
# No game data belongs in such an archive: a pack is made by the player from their own copy.
# The archive is written with the host's lha: -lh5- compression and level-$(LRVNE_LHA_LEVEL)
# headers, which LhA 2.x on the Amiga and AROS's lha unpack, the program with its `e` bit set.

LRVNE_DIST_DRAWER ?= $(APP_NAME)
LRVNE_LHA         ?= lha
LRVNE_LHA_LEVEL   ?= 1
LRVNE_DIST        := $(BUILD)/$(APP_NAME)-$(APP_VERSION)-$(LRVNE_DIST_NAME).lha
LRVNE_DIST_STAGE  := $(BUILD)/dist

ifneq ($(strip $(LRVNE_DIST_README_PARTS)),)
$(BUILD)/ReadMe.txt: $(LRVNE_DIST_README_PARTS)
	@mkdir -p $(dir $@)
	$(Q)cat $^ >$@
LRVNE_DIST_README := $(BUILD)/ReadMe.txt
endif

.PHONY: lrvne-dist
lrvne-dist: $(EXE) $(LRVNE_DIST_ICON) $(LRVNE_DIST_DRAWER_ICON) $(LRVNE_DIST_README) $(LRVNE_DIST_FILES)
	$(Q)test -n "$(APP_VERSION)" || { echo "lrvne-dist: set APP_VERSION"; exit 1; }
	$(Q)rm -rf $(LRVNE_DIST_STAGE) $(LRVNE_DIST) $(LRVNE_DIST:.lha=.readme)
	$(Q)mkdir -p $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)
	$(Q)cp $(EXE) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/$(APP_NAME)
	$(Q)chmod +x $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/$(APP_NAME)
	$(if $(LRVNE_DIST_STRIP),$(Q)$(LRVNE_DIST_STRIP) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/$(APP_NAME))
	$(if $(LRVNE_DIST_ICON),$(Q)cp $(LRVNE_DIST_ICON) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/$(APP_NAME).info)
	$(if $(LRVNE_DIST_README),$(Q)cp $(LRVNE_DIST_README) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/ReadMe.txt)
	$(if $(strip $(LRVNE_DIST_FILES)),$(Q)cp $(LRVNE_DIST_FILES) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER)/)
	$(if $(LRVNE_DIST_DRAWER_ICON),$(Q)cp $(LRVNE_DIST_DRAWER_ICON) $(LRVNE_DIST_STAGE)/$(LRVNE_DIST_DRAWER).info)
	@echo "  LHA   $(LRVNE_DIST)"
	$(Q)cd $(LRVNE_DIST_STAGE) && $(LRVNE_LHA) a$(LRVNE_LHA_LEVEL)qo5 $(abspath $(LRVNE_DIST)) \
	    $(if $(LRVNE_DIST_DRAWER_ICON),$(LRVNE_DIST_DRAWER).info) $(LRVNE_DIST_DRAWER)
	$(Q)$(LRVNE_LHA) l $(LRVNE_DIST)
	$(if $(LRVNE_DIST_UPLOAD_README),$(Q)sed 's/@VERSION@/$(APP_VERSION)/g' $(LRVNE_DIST_UPLOAD_README) >$(LRVNE_DIST:.lha=.readme))
