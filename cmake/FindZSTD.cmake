# BAREOS® - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
# This program is Free Software; you can redistribute it and/or modify it under
# the terms of version three of the GNU Affero General Public License as
# published by the Free Software Foundation and included in the file LICENSE.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

#[=======================================================================[.rst:
FindZSTD
-----------

Find Zstandard (zstd) headers and libraries.

IMPORTED Targets
^^^^^^^^^^^^^^^^

The following :prop_tgt:`IMPORTED` targets may be defined:

``ZSTD::ZSTD``
ZSTD library.

Result variables
^^^^^^^^^^^^^^^^

This module will set the following variables in your project:

``ZSTD_FOUND``
True if ZSTD found.
``ZSTD_INCLUDE_DIR``
  Where to find zstd.h.
``ZSTD_LIBRARIES``
List of libraries when using ZSTD.

#]=======================================================================]

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
mark_as_advanced(ZSTD_INCLUDE_DIR)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(ZSTD_LIBRARY "${HOMEBREW_PREFIX}/opt/zstd/lib/libzstd.a")
  mark_as_advanced(ZSTD_LIBRARY)
else()
  find_library(ZSTD_LIBRARY NAMES zstd)
  mark_as_advanced(ZSTD_LIBRARY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  ZSTD REQUIRED_VARS ZSTD_LIBRARY ZSTD_INCLUDE_DIR
)

if(ZSTD_FOUND AND NOT TARGET ZSTD::ZSTD)
  add_library(ZSTD::ZSTD UNKNOWN IMPORTED)
  set_target_properties(
    ZSTD::ZSTD PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
  )
  set_property(
    TARGET ZSTD::ZSTD
    APPEND
    PROPERTY IMPORTED_LOCATION "${ZSTD_LIBRARY}"
  )
endif()
