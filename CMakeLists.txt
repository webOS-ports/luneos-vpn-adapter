# @@@LICENSE
#
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@

cmake_minimum_required(VERSION 2.8.7)

project(luneos-vpn-adapter C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(0 1 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

pkg_check_modules(GIO2 REQUIRED gio-2.0)
include_directories(${GIO2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO2_CFLAGS})

pkg_check_modules(GIO-UNIX REQUIRED gio-unix-2.0)
include_directories(${GIO-UNIX_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO-UNIX_CFLAGS_OTHER})

pkg_check_modules(GOBJECT2 REQUIRED gobject-2.0)
include_directories(${GOBJECT2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GOBJECT2_CFLAGS})

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LUNASERVICE2_CFLAGS})

pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)
include_directories(${PBNJSON_C_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${PBNJSON_C_CFLAGS_OTHER})

# Where connman keeps its VPN plugins. Which .so files exist there is how
# getAgents decides what this device can actually do - the modern equivalent of
# legacy webOS scanning /usr/lib/vpn/agents for plugin directories.
set(CONNMAN_VPN_PLUGIN_DIR "${WEBOS_INSTALL_LIBDIR}/connman/plugins-vpn"
    CACHE STRING "Directory holding connman's VPN plugins")

set(LUNEOS_VPN_DATA_DIR "${WEBOS_INSTALL_DATADIR}/luneos-vpn-adapter"
    CACHE STRING "Directory holding this service's data files")

add_definitions(-DCONNMAN_VPN_PLUGIN_DIR="${CONNMAN_VPN_PLUGIN_DIR}")
add_definitions(-DLUNEOS_VPN_DATA_DIR="${LUNEOS_VPN_DATA_DIR}")

find_program(GDBUS_CODEGEN_EXECUTABLE NAMES gdbus-codegen DOC "gdbus-codegen executable")
if(NOT GDBUS_CODEGEN_EXECUTABLE)
	message(FATAL_ERROR "Executable gdbus-codegen not found")
endif(GDBUS_CODEGEN_EXECUTABLE)

set(GDBUS_IF_DIR ${WEBOS_BINARY_CONFIGURED_DIR}/src)

file(MAKE_DIRECTORY ${GDBUS_IF_DIR})

execute_process(COMMAND ${GDBUS_CODEGEN_EXECUTABLE} --c-namespace VpnInterface
						--generate-c-code connman-vpn-interface
						--interface-prefix net.connman.vpn.
						${CMAKE_CURRENT_SOURCE_DIR}/files/xml/connman-vpn.xml
						WORKING_DIRECTORY ${GDBUS_IF_DIR}
						RESULT_VARIABLE codegen_failed)
if(codegen_failed)
		message(FATAL_ERROR "Error in generating code for the connman-vpn interface using gdbus-codegen")
endif()

# net.connman.vpn.Agent is the reverse direction of the other two: we export
# this one on our own bus connection and connman-vpnd calls it. Same
# --c-namespace/--interface-prefix as above so the generated type is named
# consistently (VpnInterfaceAgent, alongside VpnInterfaceManager and
# VpnInterfaceConnection); a separate gdbus-codegen invocation because it is a
# distinct XML file with --generate-c-code needing its own basename.
execute_process(COMMAND ${GDBUS_CODEGEN_EXECUTABLE} --c-namespace VpnInterface
						--generate-c-code connman-vpn-agent-interface
						--interface-prefix net.connman.vpn.
						${CMAKE_CURRENT_SOURCE_DIR}/files/xml/connman-vpn-agent.xml
						WORKING_DIRECTORY ${GDBUS_IF_DIR}
						RESULT_VARIABLE codegen_failed)
if(codegen_failed)
		message(FATAL_ERROR "Error in generating code for the connman-vpn agent interface using gdbus-codegen")
endif()

include_directories(src ${GDBUS_IF_DIR})
file(GLOB SOURCE_FILES src/*.c ${GDBUS_IF_DIR}/connman-vpn-interface.c
						${GDBUS_IF_DIR}/connman-vpn-agent-interface.c)

webos_add_compiler_flags(ALL -Wall)
webos_add_linker_options(ALL --no-undefined)

add_executable(luneos-vpn-adapter ${SOURCE_FILES})
target_link_libraries(luneos-vpn-adapter
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS} ${PBNJSON_C_LDFLAGS}
    ${GIO2_LDFLAGS} ${GIO-UNIX_LDFLAGS} ${GOBJECT2_LDFLAGS}
    rt pthread)

install(DIRECTORY files/formfields DESTINATION ${LUNEOS_VPN_DATA_DIR})

webos_build_daemon()
webos_build_system_bus_files()
