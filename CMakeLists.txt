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

# One object library holds the codec, shared by the relay binary and both tests. A second
# implementation of the framing would drift, and nothing would notice until two peers disagreed on
# the wire.
add_library(rw_codec STATIC
  relay/src/wire.cpp
  relay/src/protocol.cpp)
target_include_directories(rw_codec PUBLIC "${CMAKE_SOURCE_DIR}/relay/include")

# The relay: the codec plus the transport and routing on top of it.
add_library(rw_relay STATIC
  relay/src/stream.cpp
  relay/src/relay.cpp
  relay/src/credential.cpp)
target_link_libraries(rw_relay PUBLIC rw_codec)
target_include_directories(rw_relay PUBLIC "${CMAKE_SOURCE_DIR}/relay/include")

if(WIN32)
  # Winsock, not libc sockets -- so the accept() failure check compares against INVALID_SOCKET rather
  # than 0, because SOCKET is unsigned and `raw < 0` never fires. And bcrypt for BCryptGenRandom:
  # std::random_device is not required to be a CSPRNG, and a predictable token would make every
  # check around it decorative.
  target_link_libraries(rw_relay PUBLIC ws2_32 bcrypt)
endif()

foreach(target rw_codec rw_relay)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

enable_testing()

add_executable(vectors_test relay/tests/vectors_test.cpp)
target_link_libraries(vectors_test PRIVATE rw_codec)
add_dependencies(vectors_test protocol_vectors)
add_test(NAME protocol_vectors_cpp
         COMMAND vectors_test "${PROTOCOL_VECTORS}")

add_executable(relay_e2e_test relay/tests/relay_e2e_test.cpp)
target_link_libraries(relay_e2e_test PRIVATE rw_relay)
add_test(NAME relay_e2e COMMAND relay_e2e_test)

add_executable(credential_test relay/tests/credential_test.cpp)
target_link_libraries(credential_test PRIVATE rw_relay)
add_test(NAME credentials COMMAND credential_test)

add_executable(remote-worker-relay relay/src/main.cpp)
target_link_libraries(remote-worker-relay PRIVATE rw_relay)

# A short poll cadence: the relay is a router, so its only work happens when a peer speaks, and
# there is no timer of its own to wait on.
set_target_properties(remote-worker-relay PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
