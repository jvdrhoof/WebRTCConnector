
set(SOURCE_FILES
	log.cpp
	pch.cpp
	plugin.cpp
)

# Add the header files to the project
set(HEADER_FILES
	framework.h
	log.h
	pch.h
	plugin.h
	audio_buffer.hpp
	data_parser.hpp	
	received_control.hpp
	buffer.hpp
	packet_data.hpp
	received_tile.hpp
	client_receiver.hpp
	received_audio.hpp
)

# Create the shared library (DLL)
add_library(WebRTCConnector SHARED ${HEADER_FILES} ${SOURCE_FILES})
install(
	TARGETS WebRTCConnector
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
