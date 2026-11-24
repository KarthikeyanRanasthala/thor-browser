ladybird_test_suite(Compositor)

if (ENABLE_SKIA)
    set(TEST_SOURCES
        TestBitmapExport.cpp
        TestCanvas2DCommandPlayer.cpp
        TestCanvasSurfaceRegistry.cpp
        TestVideoFrameHolds.cpp
        TestVSyncScheduler.cpp
    )

    # These drive Skia directly, and the compositor keeps its Skia dependency private.
    list(APPEND TEST_SOURCES
        TestForceDarkImageFilter.cpp
        TestPaintingSurface.cpp
        TestYUVData.cpp
    )

    if (APPLE)
        list(APPEND TEST_SOURCES TestVideoSurfaceImage.mm)
        target_link_libraries(TestCompositor PRIVATE "-framework CoreVideo")
    endif()

    target_link_libraries(TestCompositor PRIVATE skia)
else()
    set(TEST_SOURCES TestCanvasSurfaceRegistry.cpp TestVideoFrameHolds.cpp TestVSyncScheduler.cpp)
endif()
if (ENABLE_THORVG_RASTERIZER)
    ladybird_test("" "Compositor" NAME "TestThorVGPainter")
    target_sources(TestThorVGPainter PRIVATE TestDisplayListPlayerThorVG.cpp)
    target_link_libraries(TestThorVGPainter PRIVATE compositorservice LibCompositing LibGfx)
endif()
if (ENABLE_THORVG_GL)
    ladybird_test(TestThorVGGL.cpp "Compositor" LIBS compositorservice LibCompositing LibGfx)
    set_tests_properties(TestThorVGGL PROPERTIES LABELS "gpu")
endif()
target_sources(TestCompositor PRIVATE ${TEST_SOURCES})
target_link_libraries(TestCompositor PRIVATE compositorservice LibCompositing LibGfx)
