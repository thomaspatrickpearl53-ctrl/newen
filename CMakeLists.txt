cmake_minimum_required(VERSION 3.20)
project(newen C)

# Half-Life's game DLLs are 32-bit, so the engine must be too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "newen must be built 32-bit (configure with -A Win32)")
endif()

set(CMAKE_C_STANDARD 11)
set(OUT_DIR ${CMAKE_SOURCE_DIR}/bin)
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

# ---------------------------------------------------------------- SDL2
set(SDL2_DIR ${CMAKE_SOURCE_DIR}/third_party/SDL2)
add_library(SDL2 SHARED IMPORTED)
set_target_properties(SDL2 PROPERTIES
	IMPORTED_LOCATION ${SDL2_DIR}/lib/x86/SDL2.dll
	IMPORTED_IMPLIB ${SDL2_DIR}/lib/x86/SDL2.lib
	INTERFACE_INCLUDE_DIRECTORIES ${SDL2_DIR}/include)
add_library(SDL2main STATIC IMPORTED)
set_target_properties(SDL2main PROPERTIES IMPORTED_LOCATION ${SDL2_DIR}/lib/x86/SDL2main.lib)

# ---------------------------------------------------------------- engine
add_executable(newen WIN32
	src/engine/common.c
	src/engine/sys_win.c
	src/engine/cvar.c
	src/engine/cmd.c
	src/engine/console.c
	src/engine/keys.c
	src/engine/filesystem.c
	src/engine/vid_sdl.c
	src/engine/draw.c
	src/engine/font_win.c
	src/engine/host.c
	src/engine/lua_host.c
	src/engine/image.c
	src/engine/wad.c
	src/engine/bsp.c
	src/engine/r_world.c
	src/engine/r_main.c
	src/engine/model.c
	src/engine/r_studio.c
	src/engine/r_sprite.c
	src/engine/r_light.c
	src/engine/r_ents.c
	src/engine/sv_main.c
	src/engine/sv_engfuncs.c
	src/engine/sv_world.c
	src/engine/sv_phys.c
	src/engine/sv_pmove.c
	src/engine/cl_input.c
)
target_include_directories(newen PRIVATE src/engine third_party/stb)
target_compile_definitions(newen PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(newen PRIVATE lua54 SDL2main SDL2 opengl32 winmm gdi32 advapi32)
if(MSVC)
	target_compile_options(newen PRIVATE /W4 /wd4100 /wd4996)
	# SDL2main.lib was built against an older CRT
	target_link_options(newen PRIVATE /SAFESEH:NO)
endif()

# copy SDL2.dll and newen's own content next to the exe
add_custom_command(TARGET newen POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_DIR}/lib/x86/SDL2.dll ${OUT_DIR}/SDL2.dll
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/base ${OUT_DIR}/base)
