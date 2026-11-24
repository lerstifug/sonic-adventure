# The PS2 slice's rules: the game's C built for the PS2's Emotion Engine

# The slice compiles what the PSP slice compiles (psp.mk), by native.mk's
# lists and through make dc's float pass, with ps2dev's GCC for the EE. The
# EE's GCC keeps N32's 4-byte pointers and longs, as the SH-4 and the PSP
# do, so the game's C takes marks.py's narrow copy, which names the
# console's RAM through engine/psp/psp.h, as the PSP's does. The EE's FPU is
# single precision and has no fused multiply and add, so no product fuses,
# where the console's FMAC rounds once
#
# This first stage compiles the game's side alone: the game's C, the
# disc's content, the SDK's C, play math and the shipped assembly's C.
# make ps2-objects builds them, in build/engine/ps2/obj/. The runner, the
# link and the backend follow
#
# PS2DEV, from the environment, names the toolchain, ps2dev's, ~/ps2dev by
# default. PS2_OPT is the game's optimization
PS2DEV ?= $(HOME)/ps2dev
PS2_OPT ?= -O2
# PS2_MATH, from the environment, is play math's build (engine/mk/play_math.mk).
# fpu, the default, takes FMAC, FIPR, FTRV and FSRRA by the EE's own FPU, as
# the PSP's does, where fast.c's integers take some hundred instructions
# each and the game makes thousands a picture. the EE's FPU always rounds
# toward zero and flushes a denormal to zero, the mode the game holds the
# SH-4's in. it has no infinity or NaN and saturates where IEEE overflows,
# so a sum the console's would overflow differs. fast gives every host's
# bits, at that cost
PS2_MATH ?= fpu
PS2     := $(or $(PS2_OUT),build/engine/ps2)
PS2_CC  := $(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-gcc
PS2_CXX := $(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-g++
PS2_SDK := $(PS2DEV)/ps2sdk
PS2_GOALS := $(filter ps2 ps2-objects $(PS2)/%,$(MAKECMDGOALS))
ifneq ($(PS2_GOALS),)
ifeq ($(wildcard $(PS2_CC)),)
$(error $(PS2_CC): no EE compiler there; PS2DEV, from the environment, names ps2dev's toolchain)
endif
endif
# What the slice's outputs need of the makefiles: the request, and the
# flags each compile takes, recorded at this file's end, so an edit of the
# makefiles builds again only what a changed flag compiles. The links take
# the link's command too. A change to a rule's recipe alone builds nothing
# again: remove what it makes
PS2_NEEDS := $(PS2)/request.txt $(PS2)/flags.txt
PS2_GCC_INCLUDE := $(if $(PS2_GOALS),$(shell $(PS2_CC) -print-file-name=include 2> /dev/null))
ifneq ($(PS2_GOALS),)
$(call record,$(PS2)/request.txt,opt $(PS2_OPT) math $(PS2_MATH) gcc $(shell $(PS2_CC) --version 2> /dev/null))
endif

# The narrow copy of the game's C, as psp.mk keeps it
PS2_TREE := $(PS2)/src
ifneq ($(PS2_GOALS),)
$(call record,$(PS2)/sources.txt,$(sort $(NATIVE_SOURCES)))
$(PS2)/marked.mk: engine/tools/marks.py engine/dc/dreamcast.h $(NATIVE_SOURCES) $(PS2)/sources.txt $(PS2_NEEDS)
	@$(call status,marks,$(PS2_TREE))
	$(Q)python3 engine/tools/marks.py tree --narrow $(PS2_TREE)
	$(Q)find $(PS2_TREE) -name '*.[ch]' | while read -r copy; do \
		[ -e "$${copy#$(PS2_TREE)/}" ] || rm "$$copy" || exit 1; done
	$(Q)echo '# $(PS2_TREE) is current' > $@
include $(PS2)/marked.mk
endif

# How the slice compiles the game: as psp.mk does. The EE's GCC defines mips,
# a name of the adapter's, and gives small data a register the game's C
# can't keep in reach, so -G0 puts none there
PS2_CFLAGS = -std=gnu89 -ffreestanding -nostdinc -isystem $(PS2_GCC_INCLUDE) -DNONMATCHING -DSA_SMALL \
	-include engine/ps2/prelude.h $(PS2_OPT) -G0 -Umips -fcommon -fno-strict-aliasing -fno-delete-null-pointer-checks \
	-fwrapv -ffp-contract=off -fno-builtin -fms-extensions -frounding-math -ffunction-sections $(PS2_SECTIONS) -w
$(PS2)/obj/events/%.o: PS2_OPT := -Os
$(PS2)/obj/gen/%.o: PS2_SECTIONS := -fdata-sections -fno-toplevel-reorder
$(PS2)/obj/modules/summary/summary.o: PS2_SECTIONS := -fdata-sections
ps2_pass_flags = $(PS2_CFLAGS) $(if $(filter -fpu=single -double=float,$1),-Ddouble=float -Dfrexp=native_frexp)
# $(call ps2_pass,INCLUDES,RELEASE,SHC'S FLAGS) compiles $< to $@ through the
# float pass and marks.py's narrow one
ps2_pass = $(PS2_CC) $(call ps2_pass_flags,$3) $1 -E -MD -MP -MT $@ -MF $(@:.o=.d) -o $(@:.o=.i) $< && \
	python3 tools/floats.py --shc $2 $(filter -fpu=single -double=float -aggressive=2,$3) $(@:.o=.i) && \
	python3 engine/tools/marks.py narrow $(@:.o=.i) && \
	$(PS2_CC) $(call ps2_pass_flags,$3) -c -o $@ $(@:.o=.i)
PS2_GAME_INC := sdk/include $(addprefix $(PS2_TREE)/,src modules events) build/gen/src build/gen/modules \
	build/gen/events build/gen/sdk
PS2_GEN_INC  := sdk/include $(addprefix $(PS2_TREE)/,src modules)

# The game's side of the slice, by native.mk's lists
PS2_GAME_OBJ := $(NATIVE_GAME_C:%.c=$(PS2)/obj/%.o) $(NATIVE_GEN_C:build/%.c=$(PS2)/obj/%.o) \
	$(NATIVE_RUNTIME_C:%.c=$(PS2)/obj/%.o) $(NATIVE_SDK_C:%.c=$(PS2)/obj/%.o) \
	$(call play_math_$(PS2_MATH),$(PS2)/obj/engine/numeric) $(PS2)/asm.o
$(PS2)/obj/src/main.o: private PS2_DEFS := -Dmain=sa1_main
$(PS2)/obj/src/draw/model.o: private PS2_DEFS := -Iengine -I$(ENGINE_INCLUDE) -Iengine/sa1 -I.
$(PS2)/obj/src/system/pace.o: private PS2_DEFS := -I$(ENGINE_INCLUDE)
$(PS2)/obj/src/system/present.o: private PS2_DEFS := -I$(ENGINE_INCLUDE) -I.
$(PS2)/obj/sdk/ninja/debug.o: private PS2_DEFS := -Ibuild/gen/sdk
$(PS2)/obj/sdk/ninja/njSystem.o: private PS2_DEFS := -DnjInitSystem=ninja_init_system \
	-DnjChangeSystem=ninja_change_system -DnjInitVertexBuffer=ninja_init_vertex_buffer \
	-DnjWaitVSync=ninja_wait_vsync -DnjSetVSyncFunction=ninja_set_vsync_function -DnjExitSystem=ninja_exit_system
$(PS2)/obj/sdk/ninja/njTexture.o: private PS2_DEFS := -DnjLoadTexture=ninja_load_texture \
	-DnjInitTexture=ninja_init_texture
$(PS2)/obj/sdk/ninja/kmglobal_kamui_lib.o: private PS2_DEFS := -DkmSetBackGroundPlane=kamui_set_back_ground_plane
$(PS2)/obj/sdk/ninja/njDrawPolygon2D.o $(PS2)/obj/sdk/ninja/njDrawLine2D.o \
$(PS2)/obj/sdk/ninja/njDrawLine3D.o $(PS2)/obj/sdk/ninja/njModelE.o: private PS2_DEFS := -UNONMATCHING
$(foreach s,$(NATIVE_DOUBLE_FLOAT),$(eval $(PS2)/obj/$s.o: private SDK += -double=float))
PS2_PASSES := engine/ps2/prelude.h engine/psp/psp.h $(NATIVE_PRELUDE) config/compilers.mk $(FLOATS) \
	engine/tools/marks.py $(PS2_NEEDS)
$(NATIVE_GAME_C:%.c=$(PS2)/obj/%.o): $(PS2)/obj/%.o: $(PS2_TREE)/%.c $(PS2_PASSES)
	@$(call status,ps2,$*.c)
	$(Q)mkdir -p $(@D)
	$(Q)$(call ps2_pass,$(PS2_DEFS) $(addprefix -I,$(PS2_GAME_INC)),$(if $(call native_r10,$*.c),r10,r32),$(if \
		$(call native_r10,$*.c),$(SDK),$(GAME)))
$(NATIVE_RUNTIME_C:%.c=$(PS2)/obj/%.o): $(PS2)/obj/%.o: %.c $(PS2_PASSES)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(call ps2_pass,-Isdk/include -Isdk,r32,$(if $(filter $(NATIVE_WIDE_C),$<),,-fpu=single))
$(NATIVE_SDK_C:%.c=$(PS2)/obj/%.o): $(PS2)/obj/%.o: %.c $(PS2_PASSES)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(call ps2_pass,$(PS2_DEFS) -Isdk/include -Isdk,$(if $(call native_r32,$<),r32,r10),$(if \
		$(call native_r32,$<),$(GAME),$(SDK)))
$(PS2)/obj/gen/%.o: build/gen/%.c $(PS2_PASSES)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(call ps2_pass,$(addprefix -I,$(PS2_GEN_INC)),r32,$(GAME))

# The host's C, against ps2sdk's headers, as psp.mk's PSP_HOST_CFLAGS. The
# shipped assembly's C reads the runner's headers
PS2_HOST_CFLAGS := -std=gnu17 -O2 -G0 -DSA_SMALL -fno-strict-aliasing -ffp-contract=off -frounding-math -DNATIVE_DOMAIN=0U \
	-D__ptr32= -D__uptr= -Umips -D_EE -Wno-format -Iengine/ps2 -Iengine/psp -Iengine/native -Iengine/sa1 \
	-I$(ENGINE_INCLUDE) $(PLAY_MATH_INCLUDE) -I$(PS2_SDK)/ee/include -I$(PS2_SDK)/common/include
$(eval $(call play_math,$(PS2)/obj/engine/numeric,$$(PS2_CC) -std=c17 -O2 -G0 $$(PLAY_MATH_INCLUDE),$(PS2_NEEDS),ps2))
$(PS2)/asm.c: engine/tools/asm2c.py $(foreach f,$(NATIVE_ASM),$(firstword $(subst :, ,$f))) $(PS2_NEEDS)
	@$(call status,asm2c,$@)
	$(Q)mkdir -p $(@D)
	$(Q)python3 engine/tools/asm2c.py $@ $(NATIVE_ASM)
$(PS2)/asm.o: $(PS2)/asm.c $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)$(PS2_CC) $(PS2_HOST_CFLAGS) -O1 -w -MD -MP -c -o $@ $<
PS2_READ := $(filter-out $(call play_math_$(PS2_MATH),$(PS2)/obj/engine/numeric),$(filter $(PS2)/obj/%,$(PS2_GAME_OBJ)))
$(PS2_READ): $(PS2)/obj/%.o: $(PS2)/obj/%.d
$(PS2_READ:.o=.d) $(PS2)/asm.d: ;
-include $(PS2_READ:.o=.d) $(PS2)/asm.d
ifneq ($(PS2_GOALS),)
$(filter $(PS2_GAME_OBJ),$(BODY_C:%.c=$(PS2)/obj/%.o)): $(PS2)/obj/%.o: build/gen/%/bodies.txt
endif

ps2-objects: $(PS2_GAME_OBJ)
	@echo '$(PS2): $(words $(PS2_GAME_OBJ)) objects of the game built for the EE'

# The host's side. The runner and the headless drawer are the PS2's own, in
# engine/ps2/; the names and the loads are the PSP's (engine/psp/symbols.c,
# loads.c), which read and place the programs as the PS2's must. The shim's
# small-console paths are SA_SMALL's
PS2_SHIM_C := $(filter-out engine/native/runner.c engine/native/symbols.c engine/native/loads.c,$(NATIVE_SHIM_C)) \
	engine/psp/symbols.c engine/psp/loads.c $(wildcard engine/ps2/*.c)
PS2_HOST_OBJ := $(PS2_SHIM_C:%.c=$(PS2)/obj/%.o) $(NATIVE_SA1_C:%.c=$(PS2)/obj/%.o) $(PS2)/disc.o
PS2_OBJ := $(PS2_GAME_OBJ) $(PS2_HOST_OBJ)
$(PS2)/obj/engine/native/%.o $(PS2)/obj/engine/psp/%.o $(PS2)/obj/engine/ps2/%.o: PS2_HOST_WARN := -Wall -Wextra -Werror
$(PS2)/obj/engine/native/%.o: engine/native/%.c $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_CC) $(PS2_HOST_CFLAGS) $(PS2_HOST_WARN) -MD -MP -c -o $@ $<
$(PS2)/obj/engine/psp/%.o: engine/psp/%.c $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_CC) $(PS2_HOST_CFLAGS) $(PS2_HOST_WARN) -MD -MP -c -o $@ $<
$(PS2)/obj/engine/ps2/%.o: engine/ps2/%.c $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_CC) $(PS2_HOST_CFLAGS) $(PS2_HOST_WARN) -MD -MP -c -o $@ $<
# The adapter, as psp.mk compiles it: the C library's strupr under another
# name (engine/psp/libc.h), and floats in the records' doubles' place
# (engine/psp/single.h)
PS2_SA1_CFLAGS := $(PS2_HOST_CFLAGS) -fms-extensions -DNONMATCHING -include engine/psp/libc.h \
	-include engine/ps2/prelude.h -idirafter sdk/include -Isdk -Isrc -I$(dir $(PL_MPEG)) -DNATIVE_BUILD_IMAGES='"."'
$(PS2)/obj/engine/sa1/%.o: engine/sa1/%.c $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_CC) $(PS2_SA1_CFLAGS) $(PS2_SINGLE) -Wall -Wextra -MD -MP -c -o $@ $<
$(PS2)/obj/engine/sa1/records.o: PS2_SINGLE := -include engine/psp/single.h -fsingle-precision-constant
$(PS2)/obj/engine/sa1/movie.o: $(PL_MPEG_FETCHED)
# The build's disc, as native.mk's table of it
$(PS2)/disc.c: SHA256SUMS $(PS2_NEEDS)
	@$(call status,sums,$@)
	$(Q)mkdir -p $(@D)
	$(Q)awk 'BEGIN { print "#include \"native.h\"\nconst struct native_disc_sum native_disc_sums[] = {" } \
		sub(/^orig\//, "", $$2) { printf "    {\"%s\", \"%s\"},\n", $$2, $$1; rows++ } \
		END { print "};\nconst int native_disc_sum_count = sizeof native_disc_sums / sizeof *native_disc_sums;"; \
			exit !rows }' $< > $@.tmp || { echo '$<: no line names a file of orig/'; exit 1; }
	$(Q)mv $@.tmp $@
$(PS2)/disc.o: $(PS2)/disc.c engine/native/native.h $(PS2_NEEDS)
	$(Q)$(PS2_CC) $(PS2_HOST_CFLAGS) -c -o $@ $<
PS2_HOST_READ := $(filter $(PS2)/obj/%,$(PS2_HOST_OBJ))
$(PS2_HOST_READ): $(PS2)/obj/%.o: $(PS2)/obj/%.d
$(PS2_HOST_READ:.o=.d): ;
-include $(PS2_HOST_READ:.o=.d)

# The engine's C++: the runtime's core, SA1's client and the PC backend, as
# the PSP links them, with the PS2's raster, the GS's through gsKit
# (engine/ps2/gs.cpp), and its window. The PSP's scene, its texture loader
# and its operator new are the PS2's too: engine/ps2/pspkernel.h gives the
# scene the one call of the PSP's kernel it makes, ahead of the system's
# headers
GSKIT ?= $(PS2DEV)/gsKit
PS2_CXXFLAGS := -std=gnu++17 -DSA_SMALL -D_EE -fno-exceptions -fno-rtti -Wall -Wextra -Wno-format -Umips -O2 -G0 \
	-I$(ENGINE_INCLUDE) -Iengine/sa1 -Iengine/pc -Iengine/runtime -Iengine/ps2 -Iengine/psp \
	-I$(PS2_SDK)/ee/include -I$(PS2_SDK)/common/include -I$(GSKIT)/include
PS2_CXX_SOURCES := engine/runtime/engine.cpp \
	$(addprefix engine/pc/,backend.cpp world.cpp scene.cpp pvr.cpp ninja.cpp sound.cpp dsp.cpp disc.cpp \
	storage.cpp png.cpp) $(addprefix engine/psp/,memory.cpp direct.cpp) $(wildcard engine/ps2/*.cpp)
PS2_CXX_OBJ := $(PS2_CXX_SOURCES:%.cpp=$(PS2)/obj/%.o)
$(PS2_CXX_OBJ): $(PS2)/obj/%.o: %.cpp $(PS2_NEEDS)
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_CXX) $(PS2_CXXFLAGS) -MD -MP -c -o $@ $<
-include $(PS2_CXX_OBJ:.o=.d)
# VU1's program for the raster's strips, by the vector units' assembler
PS2_VU_SOURCES := $(wildcard engine/ps2/*.vsm)
PS2_VU_OBJ := $(PS2_VU_SOURCES:%.vsm=$(PS2)/obj/%.vu.o)
$(PS2_VU_OBJ): $(PS2)/obj/%.vu.o: %.vsm
	@$(call status,ps2,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2DEV)/dvp/bin/dvp-as -o $@ $<

# A program links alone first, as psp.mk's does, and keeps global only the
# names something else reads, by programs.py's rules
PS2_BIN := $(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-
ps2_program_obj = $(filter $(addprefix $(PS2)/obj/,$(patsubst %.c,%.o,$(filter-out src/%,$(filter %.c,$(shell \
	awk '!/^#/ && NF { print $$1 }' config/$1/splits.txt))))) $(PS2)/obj/gen/$1/%,$(PS2_OBJ))
PS2_PROGRAM_OBJ := $(foreach s,$(NATIVE_PROGRAMS),$(call ps2_program_obj,$s))
PS2_REST_OBJ := $(filter-out $(PS2_PROGRAM_OBJ),$(PS2_OBJ))
PS2_MEMORY := $(MEMORY_TOOLS) $(if $(PS2_GOALS),$(call from_tools,__import__("memory").SOURCES))
PS2_LINK_TOOLS := engine/tools/ps2link.py engine/tools/psplink.py
$(NATIVE_PROGRAMS:%=$(PS2)/program-%.r): $(PS2)/program-%.r: $(PS2_OBJ) $(PS2_LINK_TOOLS) $(PS2_MEMORY) \
		$(PS2_NEEDS)
	@$(call status,link,$@)
	$(Q)PS2DEV=$(PS2DEV) python3 engine/tools/ps2link.py partial $@ $* $(call ps2_program_obj,$*)
$(PS2)/programs.c: engine/tools/programs.py config/imports.txt config/1st_read/imports.txt $(PS2_MEMORY) \
		$(PS2_REST_OBJ) $(NATIVE_PROGRAMS:%=$(PS2)/program-%.r) $(PS2_NEEDS)
	@$(call status,programs,$@)
	$(Q)NM=$(PS2_BIN)nm python3 engine/tools/programs.py $(PS2) $(NATIVE_PROGRAMS) -- $(PS2_REST_OBJ)
$(NATIVE_PROGRAMS:%=$(PS2)/program-%.o): $(PS2)/program-%.o: $(PS2)/program-%.r $(PS2)/programs.c
	$(Q)$(PS2_BIN)objcopy --redefine-syms=$(PS2)/rename-$*.txt --keep-global-symbols=$(PS2)/keep-$*.txt $< $@

# The names the program's objects need that nothing defines, by their rows
# of engine/native/stubs.txt, as psp.mk's stubs: the PSP's rows speak for
# the small consoles' slice, the PS2's among them, and stubs.py's psp host
# for MIPS's ELF. The EE's C library and kernel stand for the host's
PS2_LIBC := $(PS2DEV)/ee/mips64r5900el-ps2-elf/lib
PS2_LIBRARIES := $(addprefix $(PS2_LIBC)/,libc.a libm.a libcglue.a libkernel.a libcdvd.a libpthread.a \
	libpthreadglue.a libstdc++.a) $(if $(PS2_GOALS),$(shell $(PS2_CC) -print-libgcc-file-name 2> /dev/null))
# play.c opens the engine, as on the PSP. The drawer's calls are the
# engine's rows, which the C++ defines out of stubs.py's sight, as the
# PSP's does
PS2_MAIN_OBJ := $(PS2_REST_OBJ) $(PS2_CXX_OBJ) $(PS2)/obj/engine/sa1/play.o $(PS2_VU_OBJ)
PS2_STUBBED := $(PS2_REST_OBJ)
-include $(PS2)/obj/engine/sa1/play.d
$(PS2)/stubs.c: $(PS2_STUBBED) $(NATIVE_PROGRAMS:%=$(PS2)/program-%.o) engine/tools/stubs.py \
		engine/native/stubs.txt $(PS2_MEMORY) config/1st_read/symbols.txt config/1st_read/imports.txt $(PS2_NEEDS)
	@$(call status,stubs,$@)
	$(Q)NM=$(PS2_BIN)nm READELF=$(PS2_BIN)readelf python3 engine/tools/stubs.py $@ $(PS2_STUBBED) \
		$(NATIVE_PROGRAMS:%=$(PS2)/program-%.o) --aliases $(PS2)/aliases.txt --host ps2 --placed $(PS2)/placed.txt \
		--libraries $(PS2_LIBRARIES)
$(PS2)/stubs.o: $(PS2)/stubs.c $(PS2_NEEDS)
	$(Q)$(PS2_CC) -std=gnu17 -O1 -G0 -w -c -o $@ $<

# The program: an ELF at ps2sdk's linkfile's addresses, and beside it what
# a run reads. A second name of 1ST_READ.BIN's is its symbol's by the link's
# word. The adapter hangs on the calls psp.mk's PSP_WRAPS names, as on the
# PSP, the sound's atomics are libatomic's, and the pads are libpad's
# (engine/ps2/window.cpp)
PS2_WRAPS := pad_scan frame_wait njDrawTriangle3D njDrawPolygon2D njDrawLine3D njDrawTexture njDrawPolygon kmRender
# ps2sdk's linkfile calls the code's start _ftext, which gdfs.c's test of a
# read into code knows by the GNU name
PS2_LINK = $(PS2_CXX) -T$(PS2_SDK)/ee/startup/linkfile -L$(PS2_SDK)/ee/lib -Wl,-zmax-page-size=128 \
	-Wl,--defsym,__executable_start=_ftext \
	$(PS2_WRAPS:%=-Wl$(comma)--wrap=%) $$(awk '{ printf " -Wl,--defsym,%s=%s", $$1, $$2 }' $(PS2)/aliases.txt) \
	$(PS2_MAIN_OBJ) $(PS2)/stubs.o -L$(GSKIT)/lib -lgskit -ldmakit -lpad -lstdc++ -latomic -lm
$(PS2)/main.elf: $(PS2_MAIN_OBJ) $(PS2)/stubs.o $(NATIVE_PROGRAMS:%=$(PS2)/program-%.o) $(PS2_LINK_TOOLS) \
		$(PS2_MEMORY) $(PS2_NEEDS) $(PS2)/link.txt
	@$(call status,link,$@)
	$(Q)PS2DEV=$(PS2DEV) python3 engine/tools/ps2link.py link $(PS2) $(PS2)/placed.txt $(NATIVE_PROGRAMS) -- $(PS2_LINK)
$(PS2)/game/SONICADV.ELF: $(PS2)/main.elf
	@$(call status,elf,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(PS2_BIN)strip -o $@ $<
# The index and the pack of the disc's textures, which the console reads
# beside the program: the host builds them from orig/'s disc, the index by
# the same source as a console's, and the pack of each texture made the
# GS's texels ahead of time, so the console decodes none
# (engine/tools/textures_pack.cpp)
PS2_PACK_SOURCES := engine/tools/textures_pack.cpp engine/pc/pvr.cpp engine/pc/disc.cpp
PS2_PACK_FLAGS := -std=gnu++17 -O2 -DSA_SMALL -I$(ENGINE_INCLUDE) -Iengine/pc -Iengine/ps2
$(PS2)/textures_pack: $(PS2_PACK_SOURCES) engine/pc/pvr.h engine/pc/disc.h engine/ps2/texels.h $(PS2_NEEDS)
	@$(call status,c++,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(CXX) $(PS2_PACK_FLAGS) -o $@ $(PS2_PACK_SOURCES)
$(PS2)/game/textures.pak: $(PS2)/textures_pack SHA256SUMS
	@$(call status,pack,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$< $(abspath orig) $(abspath $(@D))
$(PS2)/game/textures.bin: $(PS2)/game/textures.pak
ps2: $(PS2)/game/SONICADV.ELF $(PS2)/game/textures.pak
# And the disc's textures as TIM2 files in the GS's own formats, one for
# each name of the disc's texture files, game/textures/NAME.tm2, with a
# report of what each texture became (engine/ps2/TEXTURES.md,
# engine/tools/textures_tm2.cpp). the tool rewrites only the files whose
# bytes change, so a copy takes only those
PS2_TM2_SOURCES := engine/tools/textures_tm2.cpp engine/pc/pvr.cpp engine/pc/disc.cpp
$(PS2)/textures_tm2: $(PS2_TM2_SOURCES) engine/pc/pvr.h engine/pc/disc.h engine/ps2/texels.h engine/ps2/tim2.h \
		$(PS2_NEEDS)
	@$(call status,c++,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(CXX) $(PS2_PACK_FLAGS) -o $@ $(PS2_TM2_SOURCES)
$(PS2)/game/textures/report.csv: $(PS2)/textures_tm2 SHA256SUMS
	@$(call status,tm2,$(@D))
	$(Q)$< $(abspath orig) $(abspath $(PS2)/game)
ps2: $(PS2)/game/textures/report.csv

# PS2_OUT, from the environment, puts the slice's objects and links outside
# the tree: on WSL, its own filesystem, where a file's every open and read
# isn't a round trip to Windows, as /mnt/'s are. A run still reads its
# folder, build/engine/ps2/game/ in the tree or PS2_RUN from the
# environment, where an emulator on the host finds it: the program, its
# programs and names go there after each build, and the textures' index and
# pack where they changed. engine/tools/ps2wsl.sh builds so from a copy of
# the tree on WSL's own filesystem, into the Windows tree's folder
PS2_RUN ?= build/engine/ps2/game
ifneq ($(abspath $(PS2)),$(abspath build/engine/ps2))
ps2: $(PS2_RUN)/SONICADV.ELF $(PS2_RUN)/textures.pak $(PS2_RUN)/textures/report.csv
$(PS2_RUN)/textures/report.csv: $(PS2)/game/textures/report.csv
	@$(call status,copy,$(@D))
	$(Q)mkdir -p $(@D)
	$(Q)cp -u $(PS2)/game/textures/*.tm2 $(@D)/
	$(Q)cp $< $@
$(PS2_RUN)/SONICADV.ELF: $(PS2)/game/SONICADV.ELF
	@$(call status,copy,$(PS2_RUN))
	$(Q)mkdir -p $(PS2_RUN)/programs
	$(Q)cp $(PS2)/game/names.bin $(PS2_RUN)/names.bin
	$(Q)cp -u $(PS2)/game/programs/*.BIN $(PS2_RUN)/programs/
	$(Q)cp $< $@
$(PS2_RUN)/textures.pak: $(PS2)/game/textures.pak
	@$(call status,copy,$@)
	$(Q)mkdir -p $(@D)
	$(Q)cp $(PS2)/game/textures.bin $(PS2_RUN)/textures.bin
	$(Q)cp $< $@
endif

# The flags the compiles take and the link's command, as PS2_NEEDS says
ifneq ($(PS2_GOALS),)
$(call record,$(PS2)/flags.txt,game $(PS2_CFLAGS) host $(PS2_HOST_CFLAGS) adapter $(PS2_SA1_CFLAGS) c++ \
	$(PS2_CXXFLAGS) pack $(PS2_PACK_FLAGS))
$(call record,$(PS2)/link.txt,$(PS2_LINK))
endif
