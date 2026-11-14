# Fork version: decides whether this build is a release of the fork.
# See "Versioning" in AGENTS.md.

if(__ForkVersion)
	return()
endif()

set(__ForkVersion ON)

include("${CMAKE_CURRENT_LIST_DIR}/GitUtilities.cmake")

# Reads SRB2VERSION (first line of src/version.h) and FORKVERSION
# (src/fork_version.h) from the source tree in source_dir.
function(fork_version_read source_dir srb2version_var forkversion_var)
	file(STRINGS "${source_dir}/src/version.h" first_line LIMIT_COUNT 1)
	string(REGEX REPLACE "^#define SRB2VERSION \"([^\"]*)\".*$" "\\1" srb2version "${first_line}")
	file(STRINGS "${source_dir}/src/fork_version.h" fork_line REGEX "^#define FORKVERSION ")
	string(REGEX REPLACE "^#define FORKVERSION \"([^\"]*)\".*$" "\\1" forkversion "${fork_line}")
	set(${srb2version_var} "${srb2version}" PARENT_SCOPE)
	set(${forkversion_var} "${forkversion}" PARENT_SCOPE)
endfunction()

# Sets variable to TRUE only if all hold: forkversion is not "0", the working
# tree of the repository in source_dir is clean, and a tag named
# <srb2version>_Z<forkversion> or v<srb2version>_Z<forkversion> points at HEAD.
# Anything else, including git being unavailable, gives FALSE.
# Needs GIT_EXECUTABLE.
function(fork_version_is_release variable source_dir srb2version forkversion)
	set(${variable} FALSE PARENT_SCOPE)

	if("${forkversion}" STREQUAL "" OR "${forkversion}" STREQUAL "0" OR "${srb2version}" STREQUAL "")
		return()
	endif()
	if("${GIT_EXECUTABLE}" STREQUAL "")
		return()
	endif()

	# The git helpers run in CMAKE_CURRENT_SOURCE_DIR.
	set(CMAKE_CURRENT_SOURCE_DIR "${source_dir}")

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse --verify -q HEAD
		WORKING_DIRECTORY "${source_dir}"
		RESULT_VARIABLE head_result
		OUTPUT_QUIET ERROR_QUIET
	)
	if(NOT head_result EQUAL 0)
		return()
	endif()

	git_working_tree_dirty(dirty)
	if(dirty)
		return()
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
		WORKING_DIRECTORY "${source_dir}"
		RESULT_VARIABLE tag_result
		OUTPUT_VARIABLE tags
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT tag_result EQUAL 0)
		return()
	endif()

	string(REPLACE "\n" ";" tags "${tags}")
	set(release_tag "${srb2version}_Z${forkversion}")
	foreach(tag IN LISTS tags)
		string(STRIP "${tag}" tag)
		if(tag STREQUAL "${release_tag}" OR tag STREQUAL "v${release_tag}")
			set(${variable} TRUE PARENT_SCOPE)
			return()
		endif()
	endforeach()
endfunction()
