project(yabause-nx)

yab_port_start()

# The launcher (adapted from NaGa's Dolphin NX frontend) uses C++23
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++23")
# Frame pointers let the hang watchdog log call stacks
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")

set(PORT_INCLUDE_DIRS "../" )
# The app version shown in the homebrew menu, on HOME Menu shortcuts and to SteamGridDB
set(YAB_NX_RELEASE_VERSION "0.3.0")

# USB and SMB storage (third_party, built and installed beforehand by build_switch.sh)
set(YAB_NX_THIRD_PARTY_PREFIX "${CMAKE_BINARY_DIR}/third_party/install" CACHE PATH
	"Install prefix of yabause/src/nx/third_party (libsmb2, libusbhsfs)")

# Order matters for static linking: SDL_ttf/SDL_image before the font and image libraries they
# use, and SDL2 before the graphics stack.
set(PORT_LIBRARIES
	${YAB_NX_THIRD_PARTY_PREFIX}/lib/libsmb2.a ${YAB_NX_THIRD_PARTY_PREFIX}/lib/liblibusbhsfs.a ntfs-3g
	curl turbojpeg
	SDL2_ttf SDL2_image harfbuzz freetype bz2 png16 jpeg webpdemux webp
	SDL2 glad EGL glapi drm_nouveau nx stdc++fs z m )

set(yabause_nx_SOURCES
	main.cpp
	config.cpp
	input.cpp
	gl_shim.c
	overlay.cpp
)

# Dear ImGui (nx/imgui, the version NaGa's frontend uses) for the in-game quick menu
set(imgui_SOURCES
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_tables.cpp
	imgui/imgui_widgets.cpp
	imgui/backends/imgui_impl_opengl3.cpp
)
# The OpenGL backend uses the same glad functions as the renderer instead of its own loader
set_source_files_properties(imgui/backends/imgui_impl_opengl3.cpp PROPERTIES
	COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_LOADER_CUSTOM
	COMPILE_OPTIONS "-include;glad/glad.h")

set(launcher_SOURCES
	launcher/DolphinSwitch/CoverDownload.cpp
	launcher/DolphinSwitch/Forwarder.cpp
	launcher/DolphinSwitch/Launcher.cpp
	launcher/DolphinSwitch/Localization.cpp
	launcher/DolphinSwitch/RuntimeOverlay.cpp
	launcher/DolphinSwitch/SystemLanguage.cpp
	launcher/DolphinSwitch/Storage.cpp
	launcher/DolphinSwitch/UiAudio.cpp
	launcher/FrontendAudio.cpp
	launcher/compat/FileUtil.cpp
	launcher/compat/IniFile.cpp
	launcher/compat/SaturnGameFile.cpp
)
# Its sources include Dolphin-style paths ("DolphinSwitch/...", "Common/...", "UICommon/...")
set_source_files_properties(${launcher_SOURCES} PROPERTIES
	INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/launcher;${CMAKE_CURRENT_SOURCE_DIR}/launcher/compat;${YAB_NX_THIRD_PARTY_PREFIX}/include;/opt/devkitpro/portlibs/switch/include/SDL2")

# main.cpp reads a directly launched game's product number with the launcher's GameFile
set_source_files_properties(main.cpp PROPERTIES
	INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/launcher/compat")

include_directories( ${YABAUSE_INCLUDE_DIR} ${PORT_INCLUDE_DIRS} )
link_directories( /opt/devkitpro/devkitA64/lib/ /opt/devkitpro/libnx/lib/ /opt/devkitpro/portlibs/switch/lib/  )
add_executable(yabause-nx ${yabause_nx_SOURCES} ${launcher_SOURCES} ${imgui_SOURCES})
target_include_directories(yabause-nx PRIVATE launcher imgui)
target_compile_definitions(yabause-nx PRIVATE IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
	YAB_NX_RELEASE_VERSION="${YAB_NX_RELEASE_VERSION}")
target_link_libraries(yabause-nx yabause ${YABAUSE_LIBRARIES} ${LIBCHDR_LIBRARIES} ${PORT_LIBRARIES}  )

set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -g -Wl,-Map,memory.map" )

yab_port_success(yabause-nx)

# HOME Menu shortcuts: a small hbloader derivative (launcher/ForwarderPayload, from NaGa's
# Dolphin NX) that each installed shortcut runs to start this .nro with the game's arguments
set(forwarder_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/launcher/ForwarderPayload")
set(forwarder_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fwd")
add_custom_command(
	OUTPUT ${forwarder_OUTPUT}/hbl.nso ${forwarder_OUTPUT}/hbl.npdm ${forwarder_OUTPUT}/default.nacp
	COMMAND ${CMAKE_COMMAND} -E make_directory ${forwarder_OUTPUT}
	COMMAND ${CMAKE_C_COMPILER} ${forwarder_SOURCE}/main.c ${forwarder_SOURCE}/trampoline.s
		-o ${forwarder_OUTPUT}/hbl.elf
		-specs=/opt/devkitpro/libnx/switch.specs
		-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft
		-fPIE -O2 -DNDEBUG -ffunction-sections -fdata-sections -fno-ident
		-I/opt/devkitpro/libnx/include -D__SWITCH__ "-DVERSION=\"${YAB_NX_RELEASE_VERSION}\""
		-L/opt/devkitpro/libnx/lib -lnx -Wl,--gc-sections -Wl,--build-id=none -Wl,--wrap,exit
	COMMAND elf2nso ${forwarder_OUTPUT}/hbl.elf ${forwarder_OUTPUT}/hbl.nso
	COMMAND npdmtool ${forwarder_SOURCE}/hbl.json ${forwarder_OUTPUT}/hbl.npdm
	COMMAND nacptool --create "YabaSanshiro NX" "Thorhax" "${YAB_NX_RELEASE_VERSION}" ${forwarder_OUTPUT}/default.nacp
	DEPENDS ${forwarder_SOURCE}/main.c ${forwarder_SOURCE}/trampoline.s ${forwarder_SOURCE}/hbl.json
	COMMENT "Building the HOME Menu shortcut payload"
	VERBATIM)
add_custom_target(yabause-nx-forwarder DEPENDS ${forwarder_OUTPUT}/hbl.nso ${forwarder_OUTPUT}/hbl.npdm ${forwarder_OUTPUT}/default.nacp)
add_dependencies(yabause-nx yabause-nx-forwarder)

add_custom_command(TARGET yabause-nx
        POST_BUILD
        COMMAND /bin/bash ${CMAKE_CURRENT_SOURCE_DIR}/genSwitchExecutables.sh ${CMAKE_CURRENT_SOURCE_DIR} ${YAB_NX_RELEASE_VERSION} ${forwarder_OUTPUT}
				)

install(TARGETS yabause-nx DESTINATION "bin")
install(FILES "README.md" DESTINATION "." )
