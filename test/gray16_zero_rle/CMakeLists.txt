set(GFX_GRAY16_SOURCES
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Utf8/IndicReorder.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
)

set(GFX_GRAY16_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/minibidi_arabic/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)

add_executable(Gray16ZeroRleTest Gray16ZeroRleTest.cpp ${GFX_GRAY16_SOURCES})
target_include_directories(Gray16ZeroRleTest PRIVATE ${GFX_GRAY16_INCLUDES})
target_link_libraries(Gray16ZeroRleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(Gray16ZeroRleTest)

add_executable(Gray16DisabledTest Gray16DisabledTest.cpp ${GFX_GRAY16_SOURCES})
target_include_directories(Gray16DisabledTest PRIVATE ${GFX_GRAY16_INCLUDES})
target_compile_definitions(Gray16DisabledTest PRIVATE FREEINK_DRIVER_UC8253_X3=0)
target_link_libraries(Gray16DisabledTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(Gray16DisabledTest)
