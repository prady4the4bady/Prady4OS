# PRADYOS — CMake/Makefile hybrid build (Group 1 item 3, DDR-859)
#
# WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT.
#
# The Makefile remains canonical. It defines the flags, it owns the 138 gate
# recipes, and it is what proves a release. CMake does NOT restate any of that.
# It QUERIES the Makefile (`make print-flags`) at configure time and builds real
# CMake targets from the answer.
#
# WHY, precisely. The hazard in a hybrid build is not "two build systems" — it
# is TWO SETS OF FLAGS. A CMakeLists that spelled out -mcmodel=kernel and
# -mno-red-zone itself would still produce a kernel.elf; the gates would keep
# passing against the Makefile's binary; and CMake would ship a different one
# built with subtly different flags. Nothing in either system would report it.
# That is this project's recurring defect — silent drift that looks like success
# — so the flags have exactly one definition and `make cmake-check` asserts the
# captured copy still matches.
#
# SCOPE, stated so it is not over-read:
#   * builds  — kernel objects + kernel.elf, from the Makefile's own flags
#   * gates   — DELEGATED to the Makefile, never reimplemented. A CMake target
#               that re-derived a gate would be a second definition of what
#               "passing" means.
#   * image   — delegated (mtools/dosfstools sequencing lives in the Makefile)
#
# So this is a genuine hybrid: CMake can compile the kernel and drive the gates,
# and it cannot silently disagree with the build that ships.

cmake_minimum_required(VERSION 3.22)
project(pradyos C ASM_NASM)

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(MAKE_EXECUTABLE NAMES make gmake REQUIRED)

# --------------------------------------------------------------------------
# Query the Makefile for the canonical flags. Configure FAILS if this does not
# work — a CMake build that silently fell back to defaults would be exactly the
# divergence this file exists to prevent.
# --------------------------------------------------------------------------
execute_process(
  COMMAND ${MAKE_EXECUTABLE} --no-print-directory print-flags
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE PRADYOS_FLAGS_RAW
  RESULT_VARIABLE PRADYOS_FLAGS_RC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT PRADYOS_FLAGS_RC EQUAL 0)
  message(FATAL_ERROR
    "`make print-flags` failed (${PRADYOS_FLAGS_RC}). Refusing to configure: "
    "falling back to built-in defaults would produce a kernel built with "
    "different flags than the one the gates prove.")
endif()

string(REPLACE "\n" ";" PRADYOS_FLAG_LINES "${PRADYOS_FLAGS_RAW}")
foreach(line IN LISTS PRADYOS_FLAG_LINES)
  if(line MATCHES "^([A-Z0-9_]+)=(.*)$")
    set(PRADYOS_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

foreach(required KCFLAGS USER_C_CFLAGS X64_TRIPLE KERNEL_LD KERNEL_ELF)
  if(NOT DEFINED PRADYOS_${required})
    message(FATAL_ERROR
      "print-flags did not report ${required}. The Makefile's flag contract "
      "changed; update print-flags rather than hardcoding it here.")
  endif()
endforeach()

# Persist what was captured so `cmake-check` can diff it against the Makefile
# later, without re-configuring.
file(WRITE ${CMAKE_BINARY_DIR}/captured-flags.txt "${PRADYOS_FLAGS_RAW}\n")

message(STATUS "PRADYOS: flags taken from the Makefile (${PRADYOS_X64_TRIPLE})")

# --------------------------------------------------------------------------
# Kernel sources — asked for by name, not globbed here.
#
# A CMake glob would be a SECOND source list that drifts from KERNEL_CS. That
# exact defect (a hand-maintained list going stale) cost this project four DDRs
# — 822, 825, 833, 835 — so the count is asserted against the Makefile's own.
# --------------------------------------------------------------------------
execute_process(
  COMMAND ${MAKE_EXECUTABLE} --no-print-directory print-kernel-sources
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE PRADYOS_KERNEL_CS_RAW
  RESULT_VARIABLE PRADYOS_SRC_RC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT PRADYOS_SRC_RC EQUAL 0)
  message(FATAL_ERROR "`make print-kernel-sources` failed (${PRADYOS_SRC_RC}).")
endif()
string(REPLACE " " ";" PRADYOS_KERNEL_CS "${PRADYOS_KERNEL_CS_RAW}")
list(REMOVE_ITEM PRADYOS_KERNEL_CS "")

list(LENGTH PRADYOS_KERNEL_CS _n_sources)
if(NOT _n_sources EQUAL PRADYOS_KERNEL_CS_COUNT)
  message(FATAL_ERROR
    "kernel source count disagrees: CMake sees ${_n_sources}, the Makefile "
    "reports ${PRADYOS_KERNEL_CS_COUNT}. One of them is building a different "
    "kernel.")
endif()

separate_arguments(PRADYOS_KCFLAGS_LIST UNIX_COMMAND "${PRADYOS_KCFLAGS}")

add_library(pradyos_kernel_objs OBJECT ${PRADYOS_KERNEL_CS})
target_compile_options(pradyos_kernel_objs PRIVATE ${PRADYOS_KCFLAGS_LIST})
set_target_properties(pradyos_kernel_objs PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE OFF
)

# --------------------------------------------------------------------------
# Gates and image: DELEGATED. Never reimplemented.
#
# A CMake target that re-derived a gate's QEMU invocation would be a second
# definition of what "passing" means, and the two would diverge on the first
# change to a sentinel. These shell out, so there is one definition.
# --------------------------------------------------------------------------
foreach(delegated image smoke smoke-selftest toolchain-check ci-shard-check)
  string(REPLACE "-" "_" _tgt "${delegated}")
  add_custom_target(mk_${_tgt}
    COMMAND ${MAKE_EXECUTABLE} ${delegated}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "delegating `${delegated}` to the Makefile (single definition)"
    USES_TERMINAL
  )
endforeach()

add_custom_target(parity
  COMMAND ${CMAKE_COMMAND} -E echo "run `make cmake-check` from the source dir"
  COMMENT "flag-parity guard lives in tools/ci/cmake_parity_check.sh"
)
