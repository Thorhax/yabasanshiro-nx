project(yabause-nx)

yab_port_start()

# The launcher (adapted from NaGa's Dolphin NX frontend) uses C++23
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++23")
# Frame pointers let the hang watchdog log call stacks
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")

set(PORT_INCLUDE_DIRS "../" )
# Order matters for static linking: SDL_ttf/SDL_image before the font and image libraries they
# use, and SDL2 before the graphics stack.
set(PORT_LIBRARIES
	SDL2_ttf SDL2_image harfbuzz freetype bz2 png16 jpeg webpdemux webp
	SDL2 glad EGL glapi drm_nouveau nx stdc++fs z m )

set(yabause_nx_SOURCES
	main.cpp
	config.cpp
	input.cpp
	gl_shim.c
)

set(launcher_SOURCES
	launcher/DolphinSwitch/Launcher.cpp
	launcher/DolphinSwitch/Localization.cpp
	launcher/DolphinSwitch/SystemLanguage.cpp
	launcher/DolphinSwitch/UiAudio.cpp
	launcher/FrontendAudio.cpp
	launcher/StorageSD.cpp
	launcher/compat/FileUtil.cpp
	launcher/compat/IniFile.cpp
	launcher/compat/SaturnGameFile.cpp
)
# Its sources include Dolphin-style paths ("DolphinSwitch/...", "Common/...", "UICommon/...")
set_source_files_properties(${launcher_SOURCES} PROPERTIES
	INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/launcher;${CMAKE_CURRENT_SOURCE_DIR}/launcher/compat;/opt/devkitpro/portlibs/switch/include/SDL2")

include_directories( ${YABAUSE_INCLUDE_DIR} ${PORT_INCLUDE_DIRS} )
link_directories( /opt/devkitpro/devkitA64/lib/ /opt/devkitpro/libnx/lib/ /opt/devkitpro/portlibs/switch/lib/  )
add_executable(yabause-nx ${yabause_nx_SOURCES} ${launcher_SOURCES})
target_include_directories(yabause-nx PRIVATE launcher)
target_link_libraries(yabause-nx yabause ${YABAUSE_LIBRARIES} ${LIBCHDR_LIBRARIES} ${PORT_LIBRARIES}  )

set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -g -Wl,-Map,memory.map" )

yab_port_success(yabause-nx)

add_custom_command(TARGET yabause-nx
        POST_BUILD
        COMMAND /bin/bash ${CMAKE_CURRENT_SOURCE_DIR}/genSwitchExecutables.sh ${CMAKE_CURRENT_SOURCE_DIR}
				)

install(TARGETS yabause-nx DESTINATION "bin")
install(FILES "README.md" DESTINATION "." )
