project(thumbnailer)
set(SRCS
	Thumbnailer.h Thumbnailer.cpp
)

engine_add_executable(TARGET ${PROJECT_NAME} SRCS ${SRCS} DESCRIPTION "Command line voxel thumbnailer")
engine_target_link_libraries(TARGET ${PROJECT_NAME} DEPENDENCIES app voxelrender voxelformat)

if (UNIX)
	enable_testing()
	configure_file(tests/tests.sh.in ${PROJECT_NAME}-tests.sh @ONLY)
	add_test(NAME shelltests-${PROJECT_NAME} COMMAND ${PROJECT_NAME}-tests.sh $<TARGET_FILE:${PROJECT_NAME}>)
	set_tests_properties(shelltests-${PROJECT_NAME} PROPERTIES DEPENDS ${PROJECT_NAME})
endif()

## TODO: fix this for mingw
if (MSVC)
	set(WIN_SRCS
		windows/ThumbnailerDLL.cpp
		windows/ThumbnailerDLL.def
		windows/ThumbnailerProvider.cpp windows/ThumbnailerProvider.h
		Thumbnailer.h Thumbnailer.cpp
	)
	engine_add_sharedlibrary(TARGET voxelthumb SRCS ${WIN_SRCS})
	target_compile_definitions(voxelthumb PRIVATE -DWINDOWS_THUMBNAILER_DLL)
	set_target_properties(voxelthumb PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
	target_link_libraries(voxelthumb app voxelrender voxelformat shlwapi)
	set(HAS_ICON 0)
	set(NAME "Vengi thumbnailer")
	set(DESCRIPTION "Voxel thumbnailer for Windows Explorer")
	set(ORIGINAL_FILENAME "${CMAKE_PROJECT_NAME}-voxthumb.dll")
	string(TIMESTAMP COPYRIGHT_YEAR "%Y")
	string(TIMESTAMP COPYRIGHT_MONTH "%B")
	configure_file(${ROOT_DIR}/contrib/installer/windows/application.rc.in ${CMAKE_CURRENT_BINARY_DIR}/voxthumb.rc @ONLY)
	target_sources(voxelthumb PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/voxthumb.rc)
endif()
