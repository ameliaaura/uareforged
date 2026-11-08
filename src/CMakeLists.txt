cmake_minimum_required(VERSION 3.5)
project(UA_source)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Anonymous usage stats (docs/usage-stats.md): the build identifier sent in crash reports (the source
# commit of a release build; "dev" for local builds) and the test-only crash switch (never in releases).
set(UA_BUILD_ID "dev" CACHE STRING "Build identifier for crash reports (e.g. the source commit)")
option(UA_TEST_HOOKS "Test builds: UA_TEST_CRASH=segv|abort forces a crash in the first mission" OFF)
if(APPLE)
	# the macOS sender uses NSURLSession (Foundation, a system framework SDL already loads)
	enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 14)

set(SOURCES
	3ds.cpp
	ade.cpp
	aiinfluence.cpp
	aituning.cpp
	area.cpp
	amesh.cpp
	bitmap.cpp
	base.cpp
	bmpAnm.cpp
	button.cpp
	crc32.cpp
	def_parser.cpp
	embed.cpp
	env.cpp
	fmtlib/format.cc
	font.cpp
	gui/root.cpp
	gui/uabutton.cpp
	gui/uacommon.cpp
	gui/uaempty.cpp
	gui/uamsgbox.cpp
	gui/uascroll.cpp
	gui/uawindow.cpp
	gui/widget.cpp
	idev.cpp
	ilbm.cpp
	image.cpp
	ini.cpp
	itimer.cpp
	iwimp.cpp
	log.cpp
	loaders.cpp
	locale/default.cpp
	locale/locale.cpp
	lstvw.cpp
	main.cpp
	IFFile.cpp
	network.cpp
	netlib/client.cpp
	netlib/client_threads.cpp
	netlib/server.cpp
	netlib/server_session.cpp
	netlib/server_threads.cpp
	netlib/single.cpp
	netlib/single_threads.cpp
	netlib/zndNet.cpp
	nucleas.cpp
	obj3d.cpp
	particle.cpp
	profile_patch.cpp
	usage_stats.cpp
	usage_stats_crash.cpp
	usage_stats_platform.cpp
	rsrc.cpp
	sample.cpp
	skeleton.cpp
	sklt.cpp
	system/fsmgr.cpp
	system/gfx.cpp
	system/ground.cpp
	system/gfx_glow.cpp
	system/atmos.cpp
	system/atmos_gl.cpp
	system/atmos_winmask.cpp
	system/perf.cpp
	system/glfuncs.cpp
	system/inivals.cpp
	system/inpt.cpp
	system/luascript.cpp
	system/movie.cpp
	system/radio.cpp
	system/sound.cpp
	system/stdshdr.cpp
	system/shadows.cpp
	system/system.cpp
	system/tform.cpp
	utils.cpp
	wav.cpp
	windp.cpp
	winp.cpp
	wintimer.cpp
	world/analyzer.cpp
	world/luaevents.cpp
	world/parsers.cpp
	world/nparticle.cpp
	world/protos.cpp
	world/saveparsers.cpp
	world/tools.cpp
	wrapal.cpp
	yw.cpp
	yw_brif.cpp
	yw_funcs.cpp
	reforge.cpp
	reforge_fx.cpp
	reforge_sky.cpp
	reforge_trails.cpp
	reforge_marks.cpp
	gfxsettings.cpp
	yw_func2.cpp
	yw_game.cpp
	yw_game_ui.cpp
	yw_net.cpp
	yw_save.cpp
	ypabact.cpp
	ypacar.cpp
	ypaflyer.cpp
	ypagun.cpp
	ypamissile.cpp
	yparobo.cpp
	ypatank.cpp
	ypaufo.cpp
)

# VR module (src/vr): built for the Quest app (Android) only by default. Desktop builds
# leave it out (the flat game); -DUA_VR=ON adds it for the desktop simulator (runtime
# switch UA_VR=sim).
if(ANDROID)
	option(UA_VR "Build the VR module (runtime switch UA_VR=sim on the desktop)" ON)
else()
	option(UA_VR "Build the VR module (runtime switch UA_VR=sim on the desktop)" OFF)
endif()
if(UA_VR)
	list(APPEND SOURCES vr/vr_engine.cpp vr/vr_comfort.cpp vr/vr_gl.cpp vr/vr_sim.cpp vr/vr_aim.cpp vr/vr_overlay.cpp vr/vr_command.cpp vr/vr_table.cpp vr/vr_field.cpp vr/vr_sites.cpp vr/vr_bridge.cpp vr/vr_station.cpp vr/vr_cockpit.cpp vr/vr_controls.cpp vr/vr_radio.cpp vr/vr_panels.cpp)
endif()


# Quest / GLES:
#  * ANDROID (NDK toolchain): builds libuavr.so for the NativeActivity shell in
#    ../platform/quest (OpenXR backend, EGL from the shell, GLES 3.2, static
#    deps from UAVR_PREFIX, FMV stubbed).
#  * UA_GLES=ON on the Mac: the same GLES renderer path on ANGLE (Metal), to
#    catch GLES shader/API errors without a headset (tools/vr/build_angle.sh).
option(UA_GLES "Build the GLES 3.x renderer path" OFF)
set(UA_TARGET UA_source)

if(ANDROID)
	set(UA_GLES ON)
	set(UA_TARGET uavr)
	set(UAVR_PREFIX "" CACHE PATH "Static Android deps (platform/quest/deps/build_deps.sh)")
	set(OPENXR_AAR_DIR "" CACHE PATH "Extracted openxr_loader_for_android AAR")
	set(QUEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../platform/quest)

	list(REMOVE_ITEM SOURCES system/movie.cpp)
	list(APPEND SOURCES system/movie_stub.cpp vr/vr_openxr.cpp
		${QUEST_DIR}/src/platform/android/egl_context.cpp
		${QUEST_DIR}/src/platform/android/ua_main_android.cpp
		${QUEST_DIR}/src/platform/xr/xr_platform.cpp
		${QUEST_DIR}/src/platform/xr/xr_input.cpp
		${QUEST_DIR}/src/platform/xr/frame_stats.cpp
		${QUEST_DIR}/src/setup/setup_core.cpp
		${QUEST_DIR}/src/setup/setup_flow.cpp
		${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

	add_library(${UA_TARGET} SHARED ${SOURCES})

	add_library(openxr_loader SHARED IMPORTED)
	set_target_properties(openxr_loader PROPERTIES
		IMPORTED_LOCATION "${OPENXR_AAR_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
		INTERFACE_INCLUDE_DIRECTORIES "${OPENXR_AAR_DIR}/prefab/modules/headers/include")

	target_include_directories(${UA_TARGET} PUBLIC
		${PROJECT_SOURCE_DIR}
		${QUEST_DIR}/src
		${UAVR_PREFIX}/include
		${ANDROID_NDK}/sources/android/native_app_glue)
	set(UAVR_APP_VERSION "0.9.0" CACHE STRING "App version shown on the start screens")
	target_compile_definitions(${UA_TARGET} PUBLIC UA_NO_FMV=1 AL_LIBTYPE_STATIC=1 UAVR_APP_VERSION="${UAVR_APP_VERSION}"
		# SDL's message box needs SDLActivity (JNI); the shell logs instead.
		SDL_ShowSimpleMessageBox=UA_ShowSimpleMessageBox)
	# Some engine sources still use non-standard VLA / narrowing idioms GCC/Apple
	# clang accept; keep the NDK clang quiet about them, not about real errors.
	target_compile_options(${UA_TARGET} PRIVATE -Wno-c++11-narrowing -Wno-vla-cxx-extension
		-Wno-deprecated-declarations -Wno-format-security)
	set(P ${UAVR_PREFIX}/lib)
	target_link_libraries(${UA_TARGET}
		${P}/libSDL2_image.a ${P}/libSDL2_ttf.a ${P}/libfreetype.a ${P}/libSDL2_net.a ${P}/libSDL2.a
		${P}/libvorbisfile.a ${P}/libvorbis.a ${P}/libogg.a ${P}/libopenal.a ${P}/liblua.a
		openxr_loader android log EGL GLESv3 GLESv1_CM OpenSLES z m dl) # GLESv1_CM: SDL2s unused GLES1 renderer
	# Keep ANativeActivity_onCreate (from the glue) exported; 16 KB pages.
	target_link_options(${UA_TARGET} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
else()
find_package(PkgConfig REQUIRED)

pkg_search_module(SDL2 REQUIRED sdl2)
link_directories(${SDL2_LIBRARY_DIRS})

pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
link_directories(${SDL2_IMAGE_LIBRARY_DIRS})

pkg_search_module(SDL2_TTF REQUIRED SDL2_ttf)
link_directories(${SDL2_TTF_LIBRARY_DIRS})

pkg_search_module(SDL2_NET REQUIRED SDL2_net)
link_directories(${SDL2_NET_LIBRARY_DIRS})

pkg_search_module(VORBISFILE REQUIRED vorbisfile)
link_directories(${VORBISFILE_LIBRARY_DIRS})

#find_package(FFmpeg REQUIRED avformat avcodec avutil swscale swresample OPTIONAL avresample)
find_package(FFmpeg REQUIRED)

find_package(OpenGL REQUIRED)

find_package(OpenAL REQUIRED)

find_package(Lua REQUIRED)


if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    enable_language("RC")
    set (WIN32_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../svg/icon/icon.rc")
endif()


if (WIN32)
  add_executable(${UA_TARGET} WIN32
                       ${SOURCES}
                       ${WIN32_RESOURCES}
                )
else()
  add_executable(${UA_TARGET} ${SOURCES})
endif()




target_link_libraries(${UA_TARGET} ${SDL2_LIBRARIES}
							 ${SDL2_IMAGE_LIBRARIES}
							 ${SDL2_TTF_LIBRARIES}
							 ${SDL2_NET_LIBRARIES}
							 ${OPENGL_LIBRARIES}
							 ${VORBISFILE_LIBRARIES}
							 ${OPENAL_LIBRARY}
							 ${LUA_LIBRARIES}
							 FFmpeg::avcodec
							 FFmpeg::avformat
							 FFmpeg::avutil
							 FFmpeg::swscale
							 FFmpeg::swresample)

target_include_directories(${UA_TARGET} PUBLIC
										${PROJECT_SOURCE_DIR} 
										${SDL2_INCLUDE_DIRS} 
										${SDL2_IMAGE_INCLUDE_DIRS} 
										${SDL2_TTF_INCLUDE_DIRS}
										${SDL2_NET_INCLUDE_DIRS}
										${OpenGL_INCLUDE_DIR}
										${VORBISFILE_INCLUDE_DIRS}
										${OPENAL_INCLUDE_DIR}
										${LUA_INCLUDE_DIR})

endif()

if(UA_GLES)
	target_compile_definitions(${UA_TARGET} PUBLIC UA_GLES=1)
endif()

if(UA_GLES AND NOT ANDROID)
	# Mac ANGLE test build: GLES headers (copied from the NDK sysroot, GLES2/3
	# + KHR only) and ANGLE's libEGL/libGLESv2 instead of OpenGL.framework.
	set(UA_GLES_HEADERS "" CACHE PATH "Directory with GLES2/ GLES3/ KHR/ headers")
	set(UA_ANGLE_DIR "" CACHE PATH "Directory with ANGLE libEGL.dylib + libGLESv2.dylib")
	target_include_directories(${UA_TARGET} BEFORE PUBLIC ${UA_GLES_HEADERS})
	get_target_property(_ua_libs ${UA_TARGET} LINK_LIBRARIES)
	list(FILTER _ua_libs EXCLUDE REGEX "OpenGL")
	set_property(TARGET ${UA_TARGET} PROPERTY LINK_LIBRARIES ${_ua_libs})
	target_link_libraries(${UA_TARGET} ${UA_ANGLE_DIR}/libGLESv2.dylib ${UA_ANGLE_DIR}/libEGL.dylib)
	set_target_properties(${UA_TARGET} PROPERTIES BUILD_RPATH ${UA_ANGLE_DIR})
endif()

if(UA_VR)
	target_compile_definitions(${UA_TARGET} PUBLIC UA_VR_ENABLED=1)
endif()

# UA_VR_PREVIEW: VR MODE defaults to COMMAND DECK (the release Quest app sets it).
option(UA_VR_PREVIEW "VR build whose default VR MODE is COMMAND DECK" OFF)
if(UA_VR AND UA_VR_PREVIEW)
	target_compile_definitions(${UA_TARGET} PUBLIC UA_VR_PREVIEW=1)
endif()
option(UA_VR_NEWLOOK "VR build with the New Look graphics (preset VR, LOOK row in the pause settings)" OFF)
if(UA_VR AND UA_VR_NEWLOOK)
	target_compile_definitions(${UA_TARGET} PUBLIC UA_VR_NEWLOOK=1)
endif()

if(CMAKE_BUILD_TYPE MATCHES [dD][eE][bB][uU][gG])
   target_compile_options(${UA_TARGET} PUBLIC "-g" "-O0")
   
   if(NOT WIN32)
      target_compile_options(${UA_TARGET} PUBLIC "-fsanitize=address")
      target_link_libraries(${UA_TARGET} "-fsanitize=address")
   endif()
elseif(ANDROID AND NOT CMAKE_BUILD_TYPE STREQUAL "")
	# Quest: Release / RelWithDebInfo / MinSizeRel honour CMake's per-config flags (-O3 / -O2 -g / -Os);
	# the headset needs the optimiser for its frame budget.
else()
	# Desktop (and no build type): the engine's historical flags, unchanged.
	target_compile_options(${UA_TARGET} PUBLIC "-g" "-O0")
	#target_compile_options(${UA_TARGET} PUBLIC "-O2" "-s")
endif()

# Reforge atmos night windows: the window detector runs at level load on night maps; the engine
# builds at -O0 (above), this pure-CPU file at -O2 (~45 ms -> ~6 ms per trim). Last -O wins on clang.
set_source_files_properties(system/atmos_winmask.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# anonymous usage stats: platform parts (system libraries only)
if(APPLE)
	target_sources(${UA_TARGET} PRIVATE usage_stats_net_apple.mm)
	target_link_libraries(${UA_TARGET} "-framework Foundation")
elseif(WIN32)
	target_link_libraries(${UA_TARGET} winhttp bcrypt)
endif()
target_compile_definitions(${UA_TARGET} PRIVATE UA_BUILD_ID="${UA_BUILD_ID}")
if(UA_TEST_HOOKS)
	target_compile_definitions(${UA_TARGET} PRIVATE UA_TEST_HOOKS=1)
endif()
