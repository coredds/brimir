# Add Visual Studio filters to better organize the code
function(vs_set_filters)
	cmake_parse_arguments(ARG "" "TARGET;BASE_DIR" "" ${ARGN})
	if(MSVC)
		get_target_property(VS_TARGET_SOURCES ${ARG_TARGET} SOURCES)
		string(REPLACE "\\" "/" ARG_BASE_DIR "${ARG_BASE_DIR}")
		if(NOT "${ARG_BASE_DIR}" MATCHES "/$")
			string(APPEND ARG_BASE_DIR "/")
		endif()

	    # Organize files into folders (filters) mirroring the file system structure
		foreach(FILE IN ITEMS ${VS_TARGET_SOURCES}) 
			# Get file directory
			get_filename_component(FILE_DIR "${FILE}" DIRECTORY)

		    # Normalize path separators
		    string(REPLACE "\\" "/" FILE_DIR "${FILE_DIR}")

		    # Erase base directory from the beginning of the path
		    string(REGEX REPLACE "^${ARG_BASE_DIR}" "" FILE_DIR "${FILE_DIR}")

	    	# Put files into folders mirroring the file system structure
			source_group("${FILE_DIR}" FILES "${FILE}")
		endforeach()
	endif()
endfunction()
