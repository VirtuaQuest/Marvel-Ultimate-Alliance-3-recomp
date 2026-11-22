# MUA3 Recomp Linux build (SteamOS / Steam Deck and desktop Linux).
#   make setup-linux   create the local generator environment
#   make gen           recompile extracted/exefs (your own game files) into generated/
#   make -j            build the native runtime, game library and same SDL/ImGui launcher
comma := ,
B        ?= build
GEN      ?= generated
CC       := clang
CXX      := clang++
IMGUI    := third_party/imgui
PY       ?= .venv-linux/bin/python

# The release's version (its zip's name, the log's [info] line). RELEASE=1: the build `make dist` ships (no console
# window - the log goes to mua3recomp.log next to the exe - no debug info, the original icon, DEPS' LGPL FFmpeg).
VERSION  := 0.1.2
RELEASE  ?=
DEPS     ?=
PKG_CONFIG ?= pkg-config
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
RUNTIME_CFLAGS := $(shell $(PKG_CONFIG) --cflags vulkan shaderc libavformat libavcodec libavutil libswscale 2>/dev/null)

OSFLAGS  := -Ithird_party -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS \
            $(if $(DEPS),-I$(DEPS)/include) $(SDL_CFLAGS) $(RUNTIME_CFLAGS)
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h)
GENFLAGS := -O1 -fPIC -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime \
            -D_FILE_OFFSET_BITS=64 $(GENARCH)
RL_ACH   := third_party/rl_achievements
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn -I$(RL_ACH)/include $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -DIMGUI_DISABLE_SSE -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(OSFLAGS)
LDFLAGS  := -fuse-ld=lld $(if $(DEPS),-L$(DEPS)/lib) -Wl,-z,stack-size=16777216 -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan -lshaderc_shared -ldl -lm -pthread -Wl,--error-limit=0 -Wl,--export-dynamic

EXE      := $(B)/mua3recomp
GAMELIB  := $(B)/mua3game.so
MODULES  := game sdk glslc

GEN_SRC  := $(strip $(foreach m,$(MODULES),$(wildcard $(GEN)/$(m)/*.c)))
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(B)/obj/%.o,$(GEN_SRC)) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/linux.c
RT_HDRS  := $(wildcard runtime/*.h runtime/platform/*.h runtime/nvn/*.h runtime/nvn/maxwell/*.h \
                       runtime/nvn/vulkan/*.h runtime/ui/*.h third_party/vma/*.h)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
UI_SRC   := $(wildcard runtime/ui/*.mm)
RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
# Recomp Achievements (RecompLauncher's; runtime/achievements.c): the SDK and its rcheevos / cJSON, as shipped upstream
RL_SRC   := $(RL_ACH)/src/rl_achievements.c $(RL_ACH)/third_party/cjson/cJSON.c \
            $(wildcard $(RL_ACH)/third_party/rcheevos/src/runtime/*.c $(RL_ACH)/third_party/rcheevos/src/util/*.c)
RL_OBJ   := $(patsubst $(RL_ACH)/%.c,$(B)/rl/%.o,$(RL_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o $(B)/imgui/imgui_impl_vulkan.o

# what the game code library is built from: a library with another id is never loaded (runtime/gamelib.h)
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/simd_c.py \
                tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h runtime/simd_fp.h \
                runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)
GAMELIB_BUILD := $(shell od -An -tx1 -j64 -N8 extracted/exefs/main 2>/dev/null | tr -d ' \n' | tr a-f A-F)
GAMELIB_FULL_ID := $(GAMELIB_ID)-$(or $(GAMELIB_BUILD),unknown)

# the icon: a source build uses the game's own from your dump (extracted/control); the release, the original one in
# docs/icon (tools/make_original_icon.py) - nothing of the game is shipped
ICON_SRC := $(if $(RELEASE),,$(firstword $(wildcard extracted/control/icon_AmericanEnglish.dat extracted/control/icon_*.dat)))
ICON_ICO := $(if $(ICON_SRC),$(B)/icon.ico,$(if $(RELEASE),docs/icon/icon.ico))
ICON_RES := $(if $(ICON_ICO),$(B)/icon_res.o)

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

# ---- the game code: generated C from the game's own NSOs (never committed)
gen:
	cd tools && ../$(PY) gen.py game ../extracted/exefs/main 0x7000000000 ../$(GEN)/game --simd c
	cd tools && ../$(PY) gen.py sdk ../extracted/exefs/sdk 0x7006000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt --simd c
	cd tools && ../$(PY) gen.py glslc ../extracted/exefs/subsdk1 0x7007000000 ../$(GEN)/glslc --simd c --optnone

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS) extracted/exefs/main
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_FULL_ID)"' -c $< -o $@

check-game-code:
	@test -f extracted/exefs/main && test -n "$(GEN_SRC)" || { echo "Game code is missing: run INSTALL or make gen before building the game library."; exit 1; }

$(GAMELIB): $(GEN_OBJ) | $(EXE) check-game-code
	$(CC) -shared -fPIC -o $@.tmp $^ -Wl,--allow-shlib-undefined
	mv -f $@.tmp $@
	echo $(GAMELIB_FULL_ID) > $@.id

# ---- the runtime
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/hle/game_hooks.o: RTFLAGS += -DMUA3_VERSION='"$(VERSION)$(if $(RELEASE),,-dev)"'
$(B)/rt/hle/game_hooks.o: Makefile

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/inc/shader_variants.inc: runtime/nvn/shader_variants.txt
	@mkdir -p $(dir $@)
	(LC_ALL=C tr -d '\r' < $< | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/'; echo '""') > $@
$(B)/rt/nvn/shader_prep.o: $(B)/inc/shader_variants.inc
$(B)/inc/shader_code_locs.inc: runtime/nvn/shader_code_locs.txt
	@mkdir -p $(dir $@)
	(LC_ALL=C tr -d '\r' < $< | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/'; echo '""') > $@
$(B)/rt/nvn/shader_prep.o: $(B)/inc/shader_code_locs.inc
TRANSLATOR_ID := $(shell cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | sha256sum | cut -c1-12)
$(B)/rt/nvn/shader_prep.o: RTFLAGS += -I$(B)/inc -DSHADER_TRANSLATOR_ID='"$(TRANSLATOR_ID)"'
$(B)/rt/nvn/shader_prep.o: runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/rl/%.o: $(RL_ACH)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -DRC_DISABLE_LUA -I$(RL_ACH)/include -I$(RL_ACH)/third_party/rcheevos/include \
	    -I$(RL_ACH)/third_party/rcheevos/src -I$(RL_ACH)/third_party/cjson $(OSFLAGS) -c $< -o $@

$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ) $(RL_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# ---- the icon (the window's and the executables'): from the game's own icon in extracted/control (your dump), when
# it's there - nothing of the game is committed
$(B)/icon.png $(B)/icon.ico &: $(ICON_SRC) tools/make_icon.py
	@mkdir -p $(B)
	$(PY) -I tools/make_icon.py extracted/control $(B)
$(B)/icon_res.o: $(ICON_ICO)
	@mkdir -p $(B)
	printf '1 ICON "%s"\n' "$(ICON_ICO)" > $(B)/icon.rc
	windres -i $(B)/icon.rc -o $@

# ---- the launcher (launcher/launcher.cpp): game files and setup, the display / sound settings, the controls, then the game
LAUNCHER := $(B)/mua3launcher
LAUNCHER_IMGUI := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
                  $(B)/imgui/imgui_impl_sdl3.o $(B)/imgui/imgui_impl_sdlrenderer3.o
$(B)/launcher/launcher.o: launcher/launcher.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(LAUNCHER): $(B)/launcher/launcher.o $(LAUNCHER_IMGUI) $(if $(ICON_SRC),$(B)/icon.png)
	$(CXX) -o $@ $(filter %.o,$^) -lSDL3 -pthread
launcher: $(LAUNCHER)
all: $(LAUNCHER)

# ---- the MSYS2 runtime DLLs the game and the launcher need, copied next to them, so they start outside an MSYS2 shell
# (from Explorer or the launcher): whatever ldd finds under /ucrt64 (SDL3, FFmpeg, shaderc, libstdc++...)
DLL_STAMP := $(B)/.dlls
$(DLL_STAMP): $(wildcard $(EXE) $(LAUNCHER))
	@for f in $$(ldd $^ | grep -io '/ucrt64/bin/[^ ]*\.dll' | sort -u); do cp -u "$$f" $(B)/; done
	@echo "runtime DLLs copied into $(B)/"
	@touch $@
dlls: $(EXE) $(LAUNCHER)
	@touch $(DLL_STAMP)

setup-linux:
	sh tools/setup_python_linux.sh
	.venv-linux/bin/python -m pip install --disable-pip-version-check --progress-bar off --upgrade pip setuptools wheel
	.venv-linux/bin/python -m pip install --disable-pip-version-check --progress-bar off --only-binary=:all: -r tools/requirements-linux.txt

# ---- the release: build/dist/MUA3-Recomp-v<VERSION>-Windows-x64.zip, top folder "MUA3 Recomp/", for players and
# RecompLauncher (recomplauncher.json). It holds no game code and no game files: the launcher's prepare step
# (mua3launcher --prepare) extracts the player's own game, recompiles it with mua3gen and builds the game code library
# with zig on their machine. Needs, once (MSYS2 UCRT64 shell, with nasm and zip installed too):
#   make deps-windows       an LGPL-only FFmpeg (tools/build_deps_windows.sh; MSYS2's is a GPL build)
#   make gen-helper         mua3gen: tools/nsp_extract.py + tools/gen.py frozen by PyInstaller (no Python needed)
#   tools/fetch_zig.sh      the pinned zig (the C compiler for the player's machine), slimmed by tools/zig_slim.sh
DIST     := build/dist/MUA3 Recomp
DIST_ZIP := MUA3-Recomp-v$(VERSION)-Windows-x64.zip
WIN_B    := build/win-release
GEN_PY   ?= py -V:3.12
GEN_VPY  := build/gen-venv/Scripts/python.exe

deps-windows:
	tools/build_deps_windows.sh

gen-helper: build/gen-venv/.ok
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/mua3gen.spec
	$(GEN_VPY) tools/pe_no_cfg.py build/helper/mua3gen/mua3gen.exe
	@du -sh build/helper/mua3gen
build/gen-venv/.ok:
	$(GEN_PY) -m venv build/gen-venv
	$(GEN_VPY) -m pip install -q unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pyinstaller==6.22.3
	touch $@

dist:
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -f build/helper/mua3gen/mua3gen.exe ] || { echo "make gen-helper first (the extractor and generator)"; exit 1; }
	@! find $(GAMELIB_SRCS) tools/nsp_extract.py tools/mua3gen.py -newer build/helper/mua3gen/mua3gen.exe | grep . || \
	    { echo "^ changed since mua3gen was frozen: make gen-helper again"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 GEN=build/no-generated-code \
	    $(WIN_B)/mua3recomp.exe $(WIN_B)/mua3launcher.exe
	! nm $(WIN_B)/mua3recomp.exe | grep -E ' f_70[0-9a-f]{8}' >/dev/null  # (no game code inside)
	tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	rm -rf "build/dist/MUA3 Recomp" && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/mua3recomp.exe $(WIN_B)/mua3recomp.dll.a $(WIN_B)/mua3launcher.exe "$(DIST)/"
	cp -R build/helper/mua3gen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	printf '%s\n%s\n' '$(GAMELIB_ID)' '$(strip $(filter-out -Iruntime,$(GENFLAGS)))' > "$(DIST)/gamelib-src/gamelib.id"
	cp docs/icon/icon.png "$(DIST)/icon.png"
	cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$(DIST)/"
	cp third_party/fonts/DejaVu/LICENSE "$(DIST)/DejaVu-LICENSE.txt"
	cp third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf "$(DIST)/"
	cp third_party/fonts/BIZUDPGothic/OFL.txt "$(DIST)/BIZUDPGothic-OFL.txt"
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$(DIST)/"
	cp third_party/fonts/Baloo2/OFL.txt "$(DIST)/Baloo2-OFL.txt"
	cp $(RL_ACH)/LICENSE "$(DIST)/RecompAchievements-LICENSE.txt"
	cp $(RL_ACH)/third_party/rcheevos/LICENSE "$(DIST)/rcheevos-LICENSE.txt"
	cp $(RL_ACH)/third_party/cjson/LICENSE "$(DIST)/cJSON-LICENSE.txt"
	cp README.md LICENSE.LASRecomp recomplauncher.json "$(DIST)/"
	cp docs/THIRD_PARTY.md "$(DIST)/THIRD_PARTY.txt"
	PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/mua3recomp.exe $(WIN_B)/mua3launcher.exe | \
	    awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}' | sort -u | xargs -I{} cp -u {} "$(DIST)/"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc|libssl|gnutls'  # (no GPL codec libraries in the release)
	# (no game files, generated code or game code library, no keys; no path of this machine inside)
	! find "$(DIST)" \( -name extracted -o -name 'generated*' -o -name exefs -o -name romfs -o -name 'mua3game*' \
	    -o -name '*.nso' -o -name '*.nca' -o -name '*.nsp' -o -name '*.xci' -o -name '*.keys' -o -name 'romfs.src' \
	    -o -name 'launcher.ini' -o -name 'larecomp.ini' \) | grep .
	u="$${USERNAME:-$$(basename "$$HOME")}"; [ -n "$$u" ] || { echo "no user name for the path check"; exit 1; }; \
	    h="$${USERPROFILE:-C:\\Users\\$$u}"; \
	    ! grep -rlaiF -e "$(CURDIR)" -e "$$(cygpath -m '$(CURDIR)')" -e "$$(cygpath -w '$(CURDIR)')" \
	    -e "$$(cygpath -w "$$h")" -e "$$(cygpath -m "$$h")" -e "$$(cygpath -w "$$HOME")" -e "$$(cygpath -m "$$HOME")" \
	    "$(DIST)"
	cd build/dist && rm -f "$(DIST_ZIP)" && zip -qr9 "$(DIST_ZIP)" "MUA3 Recomp"
	@du -sh "build/dist/$(DIST_ZIP)"

runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

clean:
	rm -rf $(B)

.PHONY: all gen runtime-objs clean launcher dlls setup-linux check-game-code deps-windows gen-helper dist
