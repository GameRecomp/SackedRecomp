# SPDX-License-Identifier: MIT
#
# rc_add_game(<id> TITLE <title> EXES <exe>...)
#
# Creates the executable `sacked-<id>` for one game (and one per other unpacked install, see
# below). The shell's main() is generated from cmake/game_main.c.in. Lifted C (generated/<id>/,
# produced by `recomp lift`, never committed) is compiled in and RC_HAVE_GENERATED is defined.
# Without lifted code the shell still builds and reports that the game has not been lifted.
#
# Lifting as part of the build (as in TropicoRecomp), so that building a game, from an IDE's
# debug action too, never runs code lifted by an older lifter or from older manifest hints:
#
# - the installs that are lifted are <ID>_GAME_DIRS (a list, e.g. CULTURES1_GAME_DIRS); empty
#   means orig/<id>/gog-en, else the first folder in orig/<id>/. Each lifted binary is about
#   40 MB of C, so more variants cost compile time;
# - a game without a folder in orig/<id>/ is unpacked while configuring from its installer,
#   CD image or zip in orig/ (`recomp extract --game`, the first variant the manifest lists
#   whose medium is there);
# - every other folder in orig/<id>/ gets a target sacked-<id>-<folder> (outside `all`) that
#   lifts and links only that install, e.g. sacked-sacked-en for debugging another
#   build; each target links its own set (`recomp lift --set`, in the build tree), and each
#   lifted binary is compiled once, as an object library the targets share;
# - a target without a set yet is lifted while configuring, so that the sources are there to be
#   found;
# - during the build recomp is built by cargo (nothing to do when nothing changed) and the
#   installs are lifted again when games/<id>/game.toml or recomp is newer than the last lift.
#   `recomp lift` rewrites only the files whose content changed, so only those are compiled
#   again.
#
# Off with -DRC_AUTO_LIFT=OFF; nothing happens without cargo or without the game (CI): then
# `recomp lift` is run by hand.

set(RC_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "Where `recomp lift` writes C")
option(RC_AUTO_LIFT "Lift a game's code while configuring and again when the lifter or its manifest changed" ON)

set(RC_RECOMP_DIR "${PROJECT_SOURCE_DIR}/recomp")
set(RC_RECOMP "${RC_RECOMP_DIR}/target/release/recomp${CMAKE_EXECUTABLE_SUFFIX}")
find_program(RC_CARGO cargo)

# The installs of game `id` to lift, in `out`: <ID>_GAME_DIRS, or the default (see above).
function(rc_game_dirs id out)
  string(TOUPPER "${id}" ID)
  set(${ID}_GAME_DIRS "" CACHE STRING
      "Installs of ${id} the build lifts (list; empty: orig/${id}/gog-en, else the first folder in orig/${id})")
  set(dirs "")
  if(${ID}_GAME_DIRS)
    foreach(d IN LISTS ${ID}_GAME_DIRS)
      get_filename_component(d "${d}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
      if(IS_DIRECTORY "${d}")
        list(APPEND dirs "${d}")
      else()
        message(WARNING "${id}: ${ID}_GAME_DIRS names ${d}, which is not a folder")
      endif()
    endforeach()
  elseif(IS_DIRECTORY "${PROJECT_SOURCE_DIR}/orig/${id}/gog-en")
    set(dirs "${PROJECT_SOURCE_DIR}/orig/${id}/gog-en")
  else()
    file(GLOB found LIST_DIRECTORIES true "${PROJECT_SOURCE_DIR}/orig/${id}/*")
    list(SORT found)
    foreach(d IN LISTS found)
      if(IS_DIRECTORY "${d}")
        set(dirs "${d}")
        break()
      endif()
    endforeach()
  endif()
  set(${out} "${dirs}" PARENT_SCOPE)
endfunction()

# recomp, built by cargo: a target every game's lift depends on.
function(rc_recomp_target)
  if(TARGET recomp_cli)
    return()
  endif()
  add_custom_target(recomp_cli
    # Without Make's jobserver variables, which cargo cannot use from a recipe (it warns).
    COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS
            "${RC_CARGO}" build --release --quiet -p rc-cli --manifest-path "${RC_RECOMP_DIR}/Cargo.toml"
    BYPRODUCTS "${RC_RECOMP}"
    COMMENT "Building recomp (cargo)"
    USES_TERMINAL)
endfunction()

# Build recomp while configuring (once per run); `ok` says whether it is there.
function(rc_recomp_now ok)
  get_property(state GLOBAL PROPERTY RC_RECOMP_NOW)
  if(NOT state)
    execute_process(
      COMMAND "${RC_CARGO}" build --release --quiet -p rc-cli --manifest-path "${RC_RECOMP_DIR}/Cargo.toml"
      RESULT_VARIABLE failed)
    if(failed)
      set(state FAILED)
    else()
      set(state OK)
    endif()
    set_property(GLOBAL PROPERTY RC_RECOMP_NOW ${state})
  endif()
  if(state STREQUAL "OK")
    set(${ok} ON PARENT_SCOPE)
  else()
    set(${ok} OFF PARENT_SCOPE)
  endif()
endfunction()

# Unpack game `id` while configuring when orig/<id>/ holds no install yet but orig/ holds one of
# its media.
function(rc_extract_now id)
  file(GLOB unpacked LIST_DIRECTORIES true "${PROJECT_SOURCE_DIR}/orig/${id}/*")
  if(unpacked OR NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/orig")
    return()
  endif()
  rc_recomp_now(ok)
  if(NOT ok)
    return()
  endif()
  # Without the game's media this fails quietly: the shell then builds without lifted code.
  execute_process(
    COMMAND "${RC_RECOMP}" extract --game ${id} "${PROJECT_SOURCE_DIR}/orig"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err
    RESULT_VARIABLE failed)
  if(NOT failed)
    message(STATUS "sacked-${id}: unpacked its medium from orig/ into orig/${id}/")
  elseif(NOT err MATCHES "none of the media")
    message(WARNING "sacked-${id}: unpacking failed: ${err}")
  endif()
endfunction()

# Lift `dirs` while configuring when the target has no set file yet (`recomp lift --set`), or
# when the game's manifest is newer than it (a binary may have been added: its object library
# must exist before the build). `id` is rc_add_game's.
function(rc_lift_now target dirs set_file)
  set(manifest "${PROJECT_SOURCE_DIR}/games/${id}/game.toml")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
  if(EXISTS "${set_file}" AND NOT "${manifest}" IS_NEWER_THAN "${set_file}")
    return()
  endif()
  message(STATUS "${target}: lifting ${dirs}")
  rc_recomp_now(ok)
  set(failed 1)
  if(ok)
    execute_process(
      COMMAND "${RC_RECOMP}" lift --set "${set_file}" ${dirs}
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_QUIET
      RESULT_VARIABLE failed)
  endif()
  if(failed)
    message(WARNING "${target}: lifting failed; run recomp lift by hand (docs/WORKFLOWS.md)")
  endif()
endfunction()

# The lifted binaries a set file names (its `/* dir: NAME */` lines), in `out`.
function(rc_set_dirs set_file out)
  set(dirs "")
  if(EXISTS "${set_file}")
    file(STRINGS "${set_file}" lines REGEX "^/\\* dir: .* \\*/$")
    foreach(l IN LISTS lines)
      string(REGEX REPLACE "^/\\* dir: (.*) \\*/$" "\\1" d "${l}")
      list(APPEND dirs "${d}")
    endforeach()
  endif()
  set(${out} "${dirs}" PARENT_SCOPE)
endfunction()

# One lifted binary (generated/<id>/<dir>/) as an object library, shared by the targets that
# link it; its name in `out`.
function(rc_lifted_objects id dir out)
  string(MAKE_C_IDENTIFIER "lifted_${id}_${dir}" lib)
  set(${out} ${lib} PARENT_SCOPE)
  if(TARGET ${lib})
    return()
  endif()
  set(d "${RC_GENERATED_DIR}/${id}/${dir}")
  file(GLOB sources CONFIGURE_DEPENDS "${d}/*.c")
  add_library(${lib} OBJECT ${sources})
  target_include_directories(${lib} PRIVATE "${d}")
  target_link_libraries(${lib} PRIVATE rc_shell rc_warnings)
  # Lifted code is not debugged at the C level and is slow unoptimised (as in TropicoRecomp).
  if(NOT MSVC)
    target_compile_options(${lib} PRIVATE -O1)
  endif()
  set_property(GLOBAL APPEND PROPERTY RC_LIFTED_${id} "${d}")
endfunction()

# The game's shell: an executable that links the lifted binaries of `set_file`.
# The game's icon (tools/icons/<id>.svg, made into build/icons/<id>.ico by tools/make-icons) in
# the Windows program, as resource 1, which Windows also shows in the window; none if the file is
# missing.
function(rc_game_icon id target)
  set(ico "${PROJECT_SOURCE_DIR}/build/icons/${id}.ico")
  if(WIN32 AND EXISTS "${ico}")
    enable_language(RC)
    file(TO_CMAKE_PATH "${ico}" ico)
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    file(WRITE "${rc}" "1 ICON \"${ico}\"\n")
    target_sources(${target} PRIVATE "${rc}")
  endif()
endfunction()

function(rc_add_shell id target main_c set_file)
  add_executable(${target} "${main_c}" ${G_PATCHES})  # G_PATCHES: rc_add_game's
  target_link_libraries(${target} PRIVATE rc_shell rc_warnings)
  rc_game_icon(${id} ${target})
  rc_set_dirs("${set_file}" dirs)
  if(dirs)
    # Configure again when a lift changes which binaries the set holds.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${set_file}")
    foreach(d IN LISTS dirs)
      rc_lifted_objects(${id} "${d}" lib)
      target_sources(${target} PRIVATE $<TARGET_OBJECTS:${lib}>)
      set_property(GLOBAL APPEND PROPERTY RC_LIFTED_LIBS_${id} ${lib})
    endforeach()
    target_sources(${target} PRIVATE "${set_file}")
    target_compile_definitions(${target} PRIVATE RC_HAVE_GENERATED=1)
    list(LENGTH dirs n)
    message(STATUS "${target}: ${n} lifted binaries (${dirs})")
    set(${target}_LIFTED ON PARENT_SCOPE)
  else()
    message(STATUS "${target}: no lifted code (shell only)")
    set(${target}_LIFTED OFF PARENT_SCOPE)
  endif()
endfunction()

# Lift again during the build, before anything lifted of the game is compiled, when its manifest
# or the lifter changed: every target's installs (`lifts`: "target|set_file|dir;dir..." items,
# with ',' between dirs), in one step, since targets share lifted binaries.
function(rc_lift_build id lifts)
  rc_recomp_target()
  set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${id}-lift.stamp")
  set(commands "")
  set(byproducts "")
  foreach(item IN LISTS lifts)
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 1 set_file)
    list(GET parts 2 dirs)
    string(REPLACE "," ";" dirs "${dirs}")
    list(APPEND commands COMMAND "${RC_RECOMP}" lift --set "${set_file}" ${dirs})
    list(APPEND byproducts "${set_file}")
  endforeach()
  get_property(lifted_dirs GLOBAL PROPERTY RC_LIFTED_${id})
  foreach(d IN LISTS lifted_dirs)
    file(GLOB files "${d}/*.c" "${d}/*.h")
    list(APPEND byproducts ${files})
  endforeach()
  add_custom_command(
    OUTPUT "${stamp}"
    BYPRODUCTS ${byproducts}
    ${commands}
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS "${PROJECT_SOURCE_DIR}/games/${id}/game.toml" "${RC_RECOMP}" recomp_cli
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Lifting ${id} again (its manifest or the lifter changed)"
    USES_TERMINAL)
  add_custom_target(${id}_lift DEPENDS "${stamp}")
  get_property(libs GLOBAL PROPERTY RC_LIFTED_LIBS_${id})
  list(REMOVE_DUPLICATES libs)
  foreach(lib IN LISTS libs)
    add_dependencies(${lib} ${id}_lift)
  endforeach()
  foreach(item IN LISTS lifts)
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 0 target)
    add_dependencies(${target} ${id}_lift)
  endforeach()
endfunction()

# rc_add_game(<id> TITLE <title> EXES <exe>... [GUEST_DIR <dir>])
#
# sacked-<id> lifts and links <ID>_GAME_DIRS (default: orig/<id>/gog-en, else the first folder);
# every other folder in orig/<id>/ gets a target sacked-<id>-<folder> of its own (e.g.
# sacked-sacked-en), outside `all`, which lifts and links only that install, for
# debugging another build. Without lifting (RC_AUTO_LIFT off, no cargo: CI) sacked-<id> compiles
# whatever `recomp lift` put into generated/<id>/.
function(rc_add_game id)
  # PATCHES: the game project's patch sources (games/<id>/patches.c and what it uses); they
  # define <id>_patches(), which the shell calls at start (rc_game_info.patches).
  cmake_parse_arguments(G "" "TITLE;GUEST_DIR" "EXES;PATCHES" ${ARGN})
  if(NOT G_TITLE OR NOT G_EXES)
    message(FATAL_ERROR "rc_add_game(${id}): TITLE and EXES are required")
  endif()

  set(RC_GAME_ID "${id}")
  set(RC_GAME_TITLE "${G_TITLE}")
  if(NOT G_GUEST_DIR)
    set(G_GUEST_DIR "C:\\\\Games\\\\${id}")
  endif()
  set(RC_GAME_GUEST_DIR "${G_GUEST_DIR}")
  set(RC_GAME_EXES "")
  foreach(exe IN LISTS G_EXES)
    string(APPEND RC_GAME_EXES "\"${exe}\", ")
  endforeach()
  if(G_PATCHES)
    set(RC_GAME_PATCHES_DECL "void ${id}_patches(void);")
    set(RC_GAME_PATCHES_FN "${id}_patches")
    list(TRANSFORM G_PATCHES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" REGEX "^[^/]")
  else()
    set(RC_GAME_PATCHES_DECL "")
    set(RC_GAME_PATCHES_FN "NULL")
  endif()
  set(main_c "${CMAKE_CURRENT_BINARY_DIR}/main_${id}.c")
  configure_file("${PROJECT_SOURCE_DIR}/cmake/game_main.c.in" "${main_c}" @ONLY)

  set(target "sacked-${id}")
  set(gen_dir "${RC_GENERATED_DIR}/${id}")
  set(can_lift OFF)
  if(RC_AUTO_LIFT AND RC_CARGO AND NOT CMAKE_CROSSCOMPILING)
    rc_extract_now(${id})
    rc_game_dirs(${id} game_dirs)
    if(game_dirs)
      set(can_lift ON)
    endif()
  endif()

  if(can_lift)
    # The default target, then one per other unpacked install.
    set(lifts "")
    set(targets "${target}")
    set(dirs_of_${target} "${game_dirs}")
    file(GLOB found LIST_DIRECTORIES true "${PROJECT_SOURCE_DIR}/orig/${id}/*")
    list(SORT found)
    foreach(d IN LISTS found)
      if(IS_DIRECTORY "${d}" AND NOT d IN_LIST game_dirs)
        get_filename_component(variant "${d}" NAME)
        list(APPEND targets "${target}-${variant}")
        set(dirs_of_${target}-${variant} "${d}")
      endif()
    endforeach()
    foreach(t IN LISTS targets)
      set(set_file "${CMAKE_CURRENT_BINARY_DIR}/${t}-set.c")
      rc_lift_now(${t} "${dirs_of_${t}}" "${set_file}")
      rc_add_shell(${id} ${t} "${main_c}" "${set_file}")
      if(${t}_LIFTED)
        string(REPLACE ";" "," dirs "${dirs_of_${t}}")
        list(APPEND lifts "${t}|${set_file}|${dirs}")
      endif()
      if(NOT t STREQUAL target)
        set_target_properties(${t} PROPERTIES EXCLUDE_FROM_ALL ON)
      endif()
    endforeach()
    if(lifts)
      rc_lift_build(${id} "${lifts}")
    endif()
    set(gen_sources ${${target}_LIFTED})
  elseif(ANDROID)
    # On Android the shell is libmain.so, which SDL's activity loads and starts at SDL_main
    # (tools/build-android); it runs the native code the converter puts into the APK.
    add_library(${target} SHARED "${main_c}" ${G_PATCHES})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    target_compile_definitions(${target} PRIVATE main=SDL_main)
    target_link_libraries(${target} PRIVATE rc_shell rc_warnings)
    set(gen_sources "")
  elseif(EMSCRIPTEN)
    # The web shell: an ES module (createGame) that web/play.js starts after it has loaded the
    # game's code (rcLoadCode) and mounted its files. The game owns its loop, so main runs on
    # JSPI and gives the browser its turn (rc/web.h).
    add_executable(${target} "${main_c}" ${G_PATCHES})
    target_link_libraries(${target} PRIVATE rc_shell rc_warnings)
    target_link_options(${target} PRIVATE
      -sJSPI=1
      "-sJSPI_EXPORTS=['main','rc_web_thread_entry']"
      -sALLOW_MEMORY_GROWTH=1
      -sMAXIMUM_MEMORY=4GB
      -sALLOW_TABLE_GROWTH=1
      -sSTACK_SIZE=8MB
      -sINITIAL_MEMORY=128MB
      -sMODULARIZE=1
      -sEXPORT_ES6=1
      -sEXPORT_NAME=createGame
      -sENVIRONMENT=web
      -sINVOKE_RUN=0
      -sEXIT_RUNTIME=0
      -sFORCE_FILESYSTEM=1
      "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_rc_web_thread_entry','_rc_wasm_helpers','_rc_wasm_register']"
      "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','callMain','HEAPU8','HEAPU32','addRunDependency','removeRunDependency']"
      "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$rcLoadCode','$rcGameFS']"
      -lidbfs.js)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${id})
    set(gen_sources "")
  else()
    add_executable(${target} "${main_c}" ${G_PATCHES})
    target_link_libraries(${target} PRIVATE rc_shell rc_warnings)
    rc_game_icon(${id} ${target})
    file(GLOB_RECURSE gen_sources CONFIGURE_DEPENDS "${gen_dir}/*.c")
    if(gen_sources)
      list(LENGTH gen_sources n)
      message(STATUS "${target}: ${n} generated source files")
      target_sources(${target} PRIVATE ${gen_sources})
      if(NOT MSVC)
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-O1")
      endif()
      target_include_directories(${target} PRIVATE "${gen_dir}")
      target_compile_definitions(${target} PRIVATE RC_HAVE_GENERATED=1)
    else()
      message(STATUS "${target}: no generated code (shell only)")
    endif()
  endif()

  if(RC_BUILD_TESTS)
    add_test(NAME ${target}.version COMMAND ${target} --version)
    add_test(NAME ${target}.help COMMAND ${target} --help)
    if(NOT gen_sources)
      # Exit code 3 = "no lifted code in this build".
      add_test(NAME ${target}.not_lifted COMMAND ${target})
      set_tests_properties(${target}.not_lifted PROPERTIES
        PASS_REGULAR_EXPRESSION "no lifted code")
    endif()
  endif()
endfunction()
