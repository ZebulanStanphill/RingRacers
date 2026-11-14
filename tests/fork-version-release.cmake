# Checks cmake/Modules/ForkVersion.cmake against temporary git repositories.
# Run in script mode: cmake -DGIT_EXECUTABLE=<git> -DRR_SOURCE_DIR=<checkout> -DWORK_DIR=<scratch> -P fork-version-release.cmake
cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH "${RR_SOURCE_DIR}/cmake/Modules")
include(ForkVersion)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Ignore the developer's global and system git configuration.
set(ENV{GIT_CONFIG_GLOBAL} "${WORK_DIR}/none")
set(ENV{GIT_CONFIG_NOSYSTEM} 1)

function(git repo)
	execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
		WORKING_DIRECTORY "${repo}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "git ${ARGN} failed: ${error}")
	endif()
endfunction()

set(failures 0)
function(expect name expected)
	set(repo "${WORK_DIR}/${name}")
	set(args ${ARGN})
	list(POP_FRONT args srb2version forkversion)
	fork_version_is_release(actual "${repo}" "${srb2version}" "${forkversion}")
	if(NOT actual STREQUAL expected)
		message(SEND_ERROR "${name}: expected ${expected}, got ${actual}")
	else()
		message(STATUS "${name}: ${actual}")
	endif()
endfunction()

# A repository with a commit, and a second commit on top that changes a tracked file.
function(make_repo name)
	set(repo "${WORK_DIR}/${name}")
	file(MAKE_DIRECTORY "${repo}")
	git("${repo}" init -q)
	git("${repo}" config user.name "Test")
	git("${repo}" config user.email "test@example.invalid")
	git("${repo}" config commit.gpgsign false)
	git("${repo}" config tag.gpgsign false)
	file(WRITE "${repo}/file.txt" "one\n")
	git("${repo}" add file.txt)
	git("${repo}" commit -q -m one)
	file(WRITE "${repo}/file.txt" "two\n")
	git("${repo}" commit -q -a -m two)
endfunction()

make_repo(no_tag)
expect(no_tag FALSE 2.4 1)

make_repo(tag_clean)
git("${WORK_DIR}/tag_clean" tag 2.4_Z1)
expect(tag_clean TRUE 2.4 1)

make_repo(tag_clean_v)
git("${WORK_DIR}/tag_clean_v" tag v2.4_Z1)
expect(tag_clean_v TRUE 2.4 1)

make_repo(tag_annotated_point)
git("${WORK_DIR}/tag_annotated_point" tag -a -m release v2.4_Z1.1)
expect(tag_annotated_point TRUE 2.4 1.1)

make_repo(tag_dirty)
git("${WORK_DIR}/tag_dirty" tag 2.4_Z1)
file(WRITE "${WORK_DIR}/tag_dirty/file.txt" "changed\n")
expect(tag_dirty FALSE 2.4 1)

make_repo(tag_on_parent)
git("${WORK_DIR}/tag_on_parent" tag 2.4_Z1 HEAD~1)
expect(tag_on_parent FALSE 2.4 1)

make_repo(other_fork_version)
git("${WORK_DIR}/other_fork_version" tag 2.4_Z2)
expect(other_fork_version FALSE 2.4 1)

make_repo(other_base_version)
git("${WORK_DIR}/other_base_version" tag 2.3_Z1)
expect(other_base_version FALSE 2.4 1)

make_repo(zero)
git("${WORK_DIR}/zero" tag 2.4_Z0)
expect(zero FALSE 2.4 0)

# Not a repository, and git missing.
file(MAKE_DIRECTORY "${WORK_DIR}/not_a_repo")
expect(not_a_repo FALSE 2.4 1)
set(real_git "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "")
expect(tag_clean FALSE 2.4 1)
set(GIT_EXECUTABLE "${real_git}")

# The versions are read from the source tree's headers.
file(MAKE_DIRECTORY "${WORK_DIR}/headers/src")
file(WRITE "${WORK_DIR}/headers/src/version.h" "#define SRB2VERSION \"2.4.1\"/* this must be the first line, for cmake !! */\n\n#define MODVERSION 13\n")
file(WRITE "${WORK_DIR}/headers/src/fork_version.h" "// comment\n#define FORKVERSION \"1.1\"\n")
fork_version_read("${WORK_DIR}/headers" read_base read_fork)
if(NOT read_base STREQUAL "2.4.1" OR NOT read_fork STREQUAL "1.1")
	message(SEND_ERROR "fork_version_read gave '${read_base}' and '${read_fork}'")
endif()
# And the real ones are well-formed.
fork_version_read("${RR_SOURCE_DIR}" real_base real_fork)
if(NOT real_base MATCHES "^[0-9]+(\\.[0-9]+)+$" OR NOT real_fork MATCHES "^[0-9]+(\\.[0-9]+)*$")
	message(SEND_ERROR "the checkout's versions look wrong: '${real_base}' and '${real_fork}'")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
