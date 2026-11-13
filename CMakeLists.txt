cmake_minimum_required(VERSION 3.20)
project(newen C)

# Half-Life's game DLLs are 32-bit, so the engine must be too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "newen must be built 32-bit (Windows: configure with -A Win32; Linux: use build.sh, which passes -m32)")
endif()

set(CMAKE_C_STANDARD 11)
# where the exe and base/ go; a second build (another compiler, a release) can point somewhere else
if(NOT DEFINED OUT_DIR)
	set(OUT_DIR ${CMAKE_SOURCE_DIR}/bin)
endif()
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
	string(TOUPPER ${cfg} CFG)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${OUT_DIR})
endforeach()

# ---------------------------------------------------------------- Lua 5.4
file(GLOB LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/src/*.c)
list(REMOVE_ITEM LUA_SOURCES
	${CMAKE_SOURCE_DIR}/third_party/lua/src/lua.c
	${CMAKE_SOURCE_DIR}/third_party/lua/src/luac.c)
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua/src)
target_compile_definitions(lua54 PRIVATE _CRT_SECURE_NO_WARNINGS)
if(UNIX)
	target_compile_definitions(lua54 PRIVATE LUA_USE_POSIX)   # mkstemp instead of tmpnam
endif()

# ---------------------------------------------------------------- SDL2
if(WIN32)
	set(SDL2_DIR ${CMAKE_SOURCE_DIR}/third_party/SDL2)
	add_library(SDL2 SHARED IMPORTED)
	set_target_properties(SDL2 PROPERTIES
		IMPORTED_LOCATION ${SDL2_DIR}/lib/x86/SDL2.dll
		IMPORTED_IMPLIB ${SDL2_DIR}/lib/x86/SDL2.lib
		INTERFACE_INCLUDE_DIRECTORIES ${SDL2_DIR}/include)
	add_library(SDL2main STATIC IMPORTED)
	set_target_properties(SDL2main PROPERTIES IMPORTED_LOCATION ${SDL2_DIR}/lib/x86/SDL2main.lib)
else()
	# the system's 32-bit SDL2 (Arch: lib32-sdl2-compat), or any libSDL2-2.0.so given with -DSDL2_LIBRARY
	find_library(SDL2_LIBRARY NAMES SDL2-2.0 SDL2 PATHS /usr/lib32 /usr/lib/i386-linux-gnu NO_DEFAULT_PATH)
	find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
	if(NOT SDL2_LIBRARY OR NOT SDL2_INCLUDE_DIR)
		message(FATAL_ERROR "32-bit SDL2 not found: install lib32-sdl2-compat (Arch) or libsdl2-dev:i386 (Debian/Ubuntu)")
	endif()
	add_library(SDL2 SHARED IMPORTED)
	set_target_properties(SDL2 PROPERTIES
		IMPORTED_LOCATION ${SDL2_LIBRARY}
		INTERFACE_INCLUDE_DIRECTORIES ${SDL2_INCLUDE_DIR})
endif()

# ---------------------------------------------------------------- engine
add_executable(newen WIN32
	src/engine/common.c
	src/engine/cvar.c
	src/engine/cmd.c
	src/engine/console.c
	src/engine/keys.c
	src/engine/filesystem.c
	src/engine/vid_sdl.c
	src/engine/draw.c
	src/engine/host.c
	src/engine/lua_host.c
	src/engine/lua_game.c
	src/engine/lua_fx.c
	src/engine/lua_render.c
	src/engine/lua_ext.c
	src/engine/lua_ents.c
	src/engine/lua_ui.c
	src/engine/image.c
	src/engine/wad.c
	src/engine/bsp.c
	src/engine/r_world.c
	src/engine/r_main.c
	src/engine/model.c
	src/engine/src_fs.c
	src/engine/src_material.c
	src/engine/src_studio.c
	src/engine/src_anim.c
	src/engine/r_studio.c
	src/engine/r_sprite.c
	src/engine/r_light.c
	src/engine/r_shader.c
	src/engine/r_modern.c
	src/engine/r_shadow.c
	src/engine/r_ents.c
	src/engine/sv_main.c
	src/engine/sv_gameobj.c
	src/engine/sv_damage.c
	src/engine/sv_engfuncs.c
	src/engine/sv_world.c
	src/engine/sv_phys.c
	src/engine/sv_pmove.c
	src/engine/sv_save.c
	src/engine/cl_input.c
	src/engine/cl_main.c
	src/engine/cl_event.c
	src/engine/cl_tent.c
	src/engine/cl_tri.c
	src/engine/cl_studio.c
	src/engine/cl_view.c
	src/engine/cl_caption.c
	src/engine/r_part.c
	src/engine/r_beam.c
	src/engine/r_decal.c
	src/engine/snd_main.c
	src/engine/snd_mem.c
	src/engine/snd_codecs.c
	src/engine/vgui_bridge.c
	src/engine/gameui.c
	src/engine/vgui2_surface.c
	src/engine/vgui2_fs.c
)
if(WIN32)
	target_sources(newen PRIVATE src/engine/sys_win.c src/engine/font_win.c)
else()
	target_sources(newen PRIVATE src/engine/sys_linux.c src/engine/font_stb.c)
endif()
target_include_directories(newen PRIVATE src/engine third_party/stb third_party/dr_libs)
target_compile_definitions(newen PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
	# the vendored decoders aren't ours: no warnings from them
	set_source_files_properties(src/engine/snd_codecs.c PROPERTIES COMPILE_OPTIONS "/W0")
	target_compile_options(newen PRIVATE /W4 /wd4100 /wd4996)
	# SDL2main.lib was built against an older CRT
	target_link_options(newen PRIVATE /SAFESEH:NO)
else()
	set_source_files_properties(src/engine/snd_codecs.c src/engine/font_stb.c PROPERTIES COMPILE_OPTIONS "-w")
	target_compile_options(newen PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function -fno-strict-aliasing)
endif()
if(WIN32)
	if(MSVC)
		target_link_libraries(newen PRIVATE lua54 SDL2main SDL2 opengl32 winmm gdi32 advapi32 dbghelp)
	else()
		# MinGW (cross-builds from Linux): SDL2main.lib is Visual C++'s, so main() is the engine's own
		target_compile_definitions(newen PRIVATE SDL_MAIN_HANDLED)
		target_link_libraries(newen PRIVATE lua54 SDL2 opengl32 winmm gdi32 advapi32 dbghelp)
		target_link_options(newen PRIVATE -static-libgcc)
	endif()
	# copy SDL2.dll and newen's own content next to the exe
	add_custom_command(TARGET newen POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_DIR}/lib/x86/SDL2.dll ${OUT_DIR}/SDL2.dll
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/base ${OUT_DIR}/base)
else()
	# -rdynamic: crash reports can name the engine's functions
	target_link_libraries(newen PRIVATE lua54 SDL2 GL dl m pthread)
	target_link_options(newen PRIVATE -rdynamic)
	add_custom_command(TARGET newen POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/base ${OUT_DIR}/base)
endif()
