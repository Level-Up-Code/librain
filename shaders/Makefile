# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#
# Copyright 2018 Saso Kiselkov. All rights reserved.

SPVS = \
    generic.vert.spv \
    ws_temp.frag.spv \
    nil.frag.spv \
    rain_stage1.frag.spv \
    rain_stage2.frag.spv \
    ws_rain.frag.spv \
    ws_smudge.frag.spv

OUTDIR=bin
SPIRVX_TGT_VERSION=130

ifeq ($(V),1)
	VERB=
define logMsg
endef
else	# Not Verbose
	VERB=@
define logMsg
	@echo $(1)
endef
endif

GLSLANG=glslang
define BUILD_SHADER
	$(call logMsg,-n \	[GLSLANG]\	)
	$(VERB) $(GLSLANG) -G -o $@ $^

	$(call logMsg,\	[SPIRVX]\	$(notdir $(@:%.$(1).spv=%.$(1).glsl)))
	$(VERB) spirv-cross --version $(SPIRVX_TGT_VERSION) \
	    --output $(@:%.$(1).spv=%.$(1).glsl) $@
endef

SPVS_OUT=$(addprefix $(OUTDIR)/,$(SPVS))
all : $(SPVS_OUT)

clean :
	rm -f $(SPVS_OUT) $(patsubst %.spv,%.glsl,$(SPVS_OUT))

$(OUTDIR)/%.vert.spv : %.vert
	$(call BUILD_SHADER,vert)

$(OUTDIR)/%.frag.spv : %.frag
	$(call BUILD_SHADER,frag)

$(addprefix $(OUTDIR)/,$(SPVS)) : | $(OUTDIR)

$(OUTDIR) :
	mkdir -p $(OUTDIR)