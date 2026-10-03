# gsf2wav: renders GBA music rips (GSF / minigsf) to WAV. See README.md.

# Shown by --version. Taken from git when the tree is one; harmless otherwise.
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} describe --always --dirty
                OUTPUT_VARIABLE GSF2WAV_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GSF2WAV_VERSION)
	set(GSF2WAV_VERSION "unknown")
endif()

set(GSF2WAV_SRC
	${CMAKE_CURRENT_SOURCE_DIR}/main.c
	${CMAKE_CURRENT_SOURCE_DIR}/options.c
	${CMAKE_CURRENT_SOURCE_DIR}/overrides.c
	${CMAKE_CURRENT_SOURCE_DIR}/render.c
	${CMAKE_CURRENT_SOURCE_DIR}/capture.c
	${CMAKE_CURRENT_SOURCE_DIR}/hooks.c
	${CMAKE_CURRENT_SOURCE_DIR}/wav.c
	${CMAKE_CURRENT_SOURCE_DIR}/hifi.c
	${CMAKE_CURRENT_SOURCE_DIR}/blmix.c
	${CMAKE_CURRENT_SOURCE_DIR}/mp2k.c
	${CMAKE_CURRENT_SOURCE_DIR}/alphadream.c
	${CMAKE_CURRENT_SOURCE_DIR}/psf.c
	${CMAKE_CURRENT_SOURCE_DIR}/msglog.c
	${CMAKE_CURRENT_SOURCE_DIR}/platform.c)

add_executable(gsf2wav ${GSF2WAV_SRC})
target_link_libraries(gsf2wav ${BINARY_NAME} ${ZLIB_LIBRARIES} m)
set_target_properties(gsf2wav PROPERTIES COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES};GSF2WAV_VERSION=\"${GSF2WAV_VERSION}\"")
install(TARGETS gsf2wav DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${BINARY_NAME}-gsf2wav)

# Reverse-engineering aid for unknown sound drivers (see gsftrace.c)
add_executable(gsftrace
	${CMAKE_CURRENT_SOURCE_DIR}/gsftrace.c
	${CMAKE_CURRENT_SOURCE_DIR}/psf.c
	${CMAKE_CURRENT_SOURCE_DIR}/msglog.c
	${CMAKE_CURRENT_SOURCE_DIR}/platform.c)
target_link_libraries(gsftrace ${BINARY_NAME} ${ZLIB_LIBRARIES} m)
set_target_properties(gsftrace PROPERTIES COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}")
