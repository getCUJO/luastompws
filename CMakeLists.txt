#
# Copyright (C) 2015-2019  CUJO LLC
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

pkg_check_modules(WEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
assert(CONDITION WEBSOCKETS_VERSION VERSION_GREATER_EQUAL "2.4"
       MESSAGE "has libwebsockets ${WEBSOCKETS_VERSION} needs 2.4")
add_lua_library(stompws "stompws.c" "frame.c" "stomp.c")
target_link_libraries(stompws PRIVATE PkgConfig::WEBSOCKETS)
