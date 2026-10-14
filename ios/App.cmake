if(GZDOOM_TVOS)
    set(GZDOOM_BUNDLE_ID "com.komocode.gzdoom.tv" CACHE STRING "Apple application bundle identifier")
    set(MVK_PLATFORM tvos)
    set(MVK_DEVICE_ARCH arm64_arm64e)
    set(APPLE_DEVICE_FAMILY 3)
    set(APPLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/ios/TVInfo.plist.in")
else()
    set(GZDOOM_BUNDLE_ID "com.komocode.gzdoom.ios" CACHE STRING "Apple application bundle identifier")
    set(MVK_PLATFORM ios)
    set(MVK_DEVICE_ARCH arm64)
    set(APPLE_DEVICE_FAMILY "1,2")
    set(APPLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/ios/Info.plist.in")
endif()
set(GZDOOM_DEVELOPMENT_TEAM "" CACHE STRING "Apple signing team")
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(MVK_SLICE ${MVK_PLATFORM}-arm64_x86_64-simulator)
else()
    set(MVK_SLICE ${MVK_PLATFORM}-${MVK_DEVICE_ARCH})
endif()
set(MVK_FRAMEWORK "${GZDOOM_MOLTENVK_ROOT}/MoltenVK/dynamic/MoltenVK.xcframework/${MVK_SLICE}/MoltenVK.framework")
if(NOT EXISTS "${MVK_FRAMEWORK}")
    message(FATAL_ERROR "Run scripts/apple.py first to fetch MoltenVK.")
endif()
target_link_libraries(zdoom SDL2::SDL2main "-framework UIKit" "-framework Metal" "-framework QuartzCore" "-framework AVFoundation" "-framework GameController")
if(NOT GZDOOM_TVOS)
    target_link_libraries(zdoom "-framework UniformTypeIdentifiers")
endif()
set_target_properties(zdoom PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${APPLE_INFO_PLIST}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${GZDOOM_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${APPLE_DEVICE_FAMILY}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "18.0"
    XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST "NO"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${GZDOOM_DEVELOPMENT_TEAM}"
    BUILD_RPATH "@executable_path/Frameworks"
)
set_source_files_properties(
    common/platform/ios/ios_platform.mm common/platform/ios/ios_controls.mm common/platform/ios/tvos_controls.mm
    common/platform/ios/ios_hardware.mm common/platform/ios/ios_specialpaths.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc -fobjc-exceptions")
add_custom_command(TARGET zdoom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:zdoom>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MVK_FRAMEWORK}" "$<TARGET_BUNDLE_DIR:zdoom>/Frameworks/MoltenVK.framework"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ios/Resources" "$<TARGET_BUNDLE_DIR:zdoom>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_DIR:zdoom>/Games"
)
option(GZDOOM_BUNDLE_TEST_WADS "Include local vendor IWADs for development testing" OFF)
if(GZDOOM_BUNDLE_TEST_WADS)
    file(GLOB TEST_WADS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vendor/*.wad" "${CMAKE_SOURCE_DIR}/vendor/*.WAD")
    foreach(TEST_WAD IN LISTS TEST_WADS)
        add_custom_command(TARGET zdoom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:zdoom>/Games"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TEST_WAD}" "$<TARGET_BUNDLE_DIR:zdoom>/Games/")
    endforeach()
endif()
