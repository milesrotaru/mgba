add_executable(gsf2wav
	${CMAKE_CURRENT_SOURCE_DIR}/main.c
	${CMAKE_CURRENT_SOURCE_DIR}/alphadream.c
	${CMAKE_CURRENT_SOURCE_DIR}/blmix.c
	${CMAKE_CURRENT_SOURCE_DIR}/mp2k.c
	${CMAKE_CURRENT_SOURCE_DIR}/hifi.c
	${CMAKE_CURRENT_SOURCE_DIR}/psf.c)
target_link_libraries(gsf2wav ${BINARY_NAME} ${ZLIB_LIBRARIES} m)
set_target_properties(gsf2wav PROPERTIES COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}")
install(TARGETS gsf2wav DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${BINARY_NAME}-gsf2wav)

# Reverse-engineering aid for unknown sound drivers (see gsftrace.c)
add_executable(gsftrace
	${CMAKE_CURRENT_SOURCE_DIR}/gsftrace.c
	${CMAKE_CURRENT_SOURCE_DIR}/psf.c)
target_link_libraries(gsftrace ${BINARY_NAME} ${ZLIB_LIBRARIES} m)
set_target_properties(gsftrace PROPERTIES COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}")
