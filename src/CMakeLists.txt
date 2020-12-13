#you sohuld have the libacfutils library in the main directory with redist built inside!
if (NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/libacfutils/")
	message(FATAL_ERROR "Can't find libacfutils")
endif ()


set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -O2 -g -W -Wall -Wextra -Werror -fvisibility=hidden \
	-Wunused-result -Wno-missing-field-initializers")

#definitions
add_definitions(-DXPLM200=1 -DXPLM210=1 -DXPLM300=1 -DXPLM301=1 -DXPLM302=1 -DXPLM_DEPRECATED=1)
add_definitions(-D_GNU_SOURCE)
add_definitions(-DDEBUG)
add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-DGL_GLEXT_PROTOTYPES)

if(APPLE)
	add_definitions(-DAPL=1 -DIBM=0 -DLIN=0)
elseif(UNIX)
	add_definitions(-DAPL=0 -DIBM=0 -DLIN=1)
elseif(WIN32)
	add_definitions(-DAPL=0 -DIBM=1 -DLIN=0)
endif()

#include directores
include_directories(
	"${PROJECT_SOURCE_DIR}/libacfutils/libacfutils-redist/include"
	"${PROJECT_SOURCE_DIR}/libacfutils/libacfutils-redist/lin64/include"
	"${PROJECT_SOURCE_DIR}/libacfutils/SDK/CHeaders/XPLM"
)

#source setup
set(SRC
	${CMAKE_CURRENT_LIST_DIR}/librain.c
	${CMAKE_CURRENT_LIST_DIR}/obj8.c
	${CMAKE_CURRENT_LIST_DIR}/surf_ice.c
)

add_library(rain SHARED ${SRC})

#gather library dependencies
if (APPLE)

	add_library(rain
	
	)

elseif (UNIX)

	target_link_libraries(rain
		"${PROJECT_SOURCE_DIR}/libacfutils/libacfutils-redist/lin64/lib/libacfutils.a"
		"${PROJECT_SOURCE_DIR}/libacfutils/libacfutils-redist/lin64/lib/libGLEWmx.a"
		"${PROJECT_SOURCE_DIR}/libacfutils/libacfutils-redist/lin64/lib/libpng16.a"
	)

elseif (WIN32)

	add_library(rain

	)

endif ()

#link plugin
if (APPLE)

elseif (UNIX)

	target_link_libraries(rain
		-static-libgcc
		-static-libstdc++
		-fPIC
		GL
		)

elseif (WIN32)

endif ()


set_target_properties(rain PROPERTIES PREFIX "")
set_target_properties(rain PROPERTIES SUFFIX "")
set_target_properties(rain PROPERTIES OUTPUT_NAME "librain.xpl")

if (APPLE)

elseif (UNIX)

set_target_properties(rain PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/lin_x64" )
set_target_properties(rain PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/lin_x64" )

elseif (WIN32)

endif ()



