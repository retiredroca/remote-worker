cmake_minimum_required(VERSION 3.21)
project(remote_worker_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# The wire format is defined by tools/protocol_vectors.py, which is the normative encoder, and
# checked in every language against the bytes it generates. Regenerate on every build so a format
# change cannot sit in a stale committed copy.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PROTOCOL_VECTORS "${CMAKE_BINARY_DIR}/protocol/vectors.txt")
add_custom_command(
  OUTPUT "${PROTOCOL_VECTORS}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/protocol_vectors.py" "${PROTOCOL_VECTORS}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/protocol_vectors.py"
  COMMENT "Generating wire-format vectors"
  VERBATIM)
# add_custom_command(OUTPUT ...) produces a file target, which add_dependencies cannot name, so the
# rule needs a task target of its own to hang the dependency on.
add_custom_target(protocol_vectors DEPENDS "${PROTOCOL_VECTORS}")

# One object library holds the codec, shared by the agent binary and the tests. A second
# implementation of the framing would drift, and nothing would notice until two peers disagreed on
# the wire.
add_library(rw_codec STATIC
  agent/src/wire.cpp
  agent/src/protocol.cpp)
target_include_directories(rw_codec PUBLIC "${CMAKE_SOURCE_DIR}/agent/include")

# Transport plus the endpoint agent. This is what runs on a watched machine, so it is the piece the
# release actually ships.
add_library(rw_agent STATIC
  agent/src/stream.cpp
  agent/src/agent.cpp
  agent/src/credential.cpp)
target_link_libraries(rw_agent PUBLIC rw_codec)
target_include_directories(rw_agent PUBLIC "${CMAKE_SOURCE_DIR}/agent/include")

if(WIN32)
  # Winsock, not libc sockets -- so the accept() failure check compares against INVALID_SOCKET rather
  # than 0, because SOCKET is unsigned and `raw < 0` never fires. And bcrypt for BCryptGenRandom:
  # std::random_device is not required to be a CSPRNG, and a predictable token would make every
  # check around it decorative.
  target_link_libraries(rw_agent PUBLIC ws2_32 bcrypt)
endif()

foreach(target rw_codec rw_agent)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

enable_testing()

add_executable(vectors_test agent/tests/vectors_test.cpp)
target_link_libraries(vectors_test PRIVATE rw_codec)
add_dependencies(vectors_test protocol_vectors)
add_test(NAME protocol_vectors_cpp
         COMMAND vectors_test "${PROTOCOL_VECTORS}")

add_executable(credential_test agent/tests/credential_test.cpp)
target_link_libraries(credential_test PRIVATE rw_agent)
add_test(NAME credentials COMMAND credential_test)

# The agent is the security boundary: nothing else authenticates a controller any more, so its
# accept-or-refuse behaviour is tested against a real peer over loopback rather than assumed.
add_executable(agent_test agent/tests/agent_test.cpp)
target_link_libraries(agent_test PRIVATE rw_agent)
add_test(NAME agent COMMAND agent_test)

add_executable(remote-worker agent/src/main.cpp)
target_link_libraries(remote-worker PRIVATE rw_agent)

set_target_properties(remote-worker PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Normalise the output path, because a multi-config generator (Visual Studio) puts the binary in
# bin/<Config>/ and a single-config one (Ninja, MinGW-make) does not. Gradle stages and names the
# release artifact, so it needs one predictable place to copy from rather than a glob over both.
if(WIN32)
  set(RW_AGENT_NAME "remote-worker.exe")
else()
  set(RW_AGENT_NAME "remote-worker")
endif()

add_custom_command(TARGET remote-worker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/staged"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:remote-worker>" "${CMAKE_BINARY_DIR}/staged/${RW_AGENT_NAME}"
  COMMENT "Staging the agent binary")
