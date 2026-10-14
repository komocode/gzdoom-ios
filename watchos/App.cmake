enable_language(Swift)
set(GZDOOM_WATCH_BUNDLE_ID "com.komocode.gzdoom.watch" CACHE STRING "Watch application bundle identifier")
add_executable(gzdoomwatch MACOSX_BUNDLE "${CMAKE_SOURCE_DIR}/watchos/WatchApp.swift")
set_target_properties(gzdoomwatch PROPERTIES
    OUTPUT_NAME gzdoom-watch
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/watchos/Info.plist.in"
    Swift_LANGUAGE_VERSION 5
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${GZDOOM_WATCH_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 4
    XCODE_ATTRIBUTE_WATCHOS_DEPLOYMENT_TARGET 26.0
)
target_compile_options(gzdoomwatch PRIVATE -parse-as-library -import-objc-header "${CMAKE_SOURCE_DIR}/watchos/WatchBridge.h")
# GZDoom registers engine types through static constructors in every object file.
target_link_options(gzdoomwatch PRIVATE "LINKER:-force_load,$<TARGET_FILE:zdoom>")
target_link_options(gzdoomwatch PRIVATE "LINKER:-syslibroot,${CMAKE_OSX_SYSROOT}")
target_link_libraries(gzdoomwatch PRIVATE zdoom "-lc++" "-framework SwiftUI")
# lights.pk3 gives the true-color software renderer its dynamic light definitions.
add_dependencies(gzdoomwatch gzdoom_pk3 game_support_pk3 game_widescreen_gfx_pk3 lights_pk3)
foreach(RESOURCE gzdoom.pk3 game_support.pk3 game_widescreen_gfx.pk3 lights.pk3)
    add_custom_command(TARGET gzdoomwatch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/${RESOURCE}" "$<TARGET_BUNDLE_DIR:gzdoomwatch>/${RESOURCE}")
endforeach()
option(GZDOOM_BUNDLE_TEST_WADS "Include local vendor IWADs for development testing" OFF)
add_custom_command(TARGET gzdoomwatch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_DIR:gzdoomwatch>/Games")
if(GZDOOM_BUNDLE_TEST_WADS)
    file(GLOB TEST_WADS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vendor/*.wad" "${CMAKE_SOURCE_DIR}/vendor/*.WAD")
    foreach(TEST_WAD IN LISTS TEST_WADS)
        add_custom_command(TARGET gzdoomwatch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:gzdoomwatch>/Games"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TEST_WAD}" "$<TARGET_BUNDLE_DIR:gzdoomwatch>/Games/")
    endforeach()
endif()
