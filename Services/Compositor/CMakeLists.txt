set(SOURCES
    AnimatedImage.cpp
    BackingStoreFactory.cpp
    CompositorCore.cpp
    CompositorState.cpp
    SurfaceReadback.cpp
    ContextRasterTargets.cpp
    DrawnResources.cpp
    ConnectionFromClient.cpp
    ConnectionFromPresentation.cpp
    ConnectionFromWebContent.cpp
    PresentableSurface.cpp
    RenderClockConnection.cpp
    VideoFrameHolds.cpp
    VSyncScheduler.cpp
    WindowServer.cpp
)

set(GENERATED_SOURCES
    CompositorControlClientEndpoint.h
    CompositorControlServerEndpoint.h
    CompositorPresentationClientEndpoint.h
    CompositorPresentationServerEndpoint.h
    CompositorWebContentClientEndpoint.h
    CompositorWebContentServerEndpoint.h
    RenderClockClientEndpoint.h
    RenderClockServerEndpoint.h
)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME compositor_core FEATURES allocator FFI_HEADER CompositorCoreRustFFI.h)
target_link_libraries(compositor_core INTERFACE LibGfx)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME compositor_core)

add_library(compositorservice STATIC ${SOURCES} ${GENERATED_SOURCES})
ladybird_generated_sources(compositorservice)

if (ENABLE_THORVG_RASTERIZER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ThorVG REQUIRED IMPORTED_TARGET thorvg-1>=1.2)
    target_sources(compositorservice PRIVATE DisplayListPlayerThorVG.cpp ThorVGPath.cpp)
    target_link_libraries(compositorservice PRIVATE PkgConfig::ThorVG)
    target_compile_definitions(compositorservice PUBLIC ENABLE_THORVG_RASTERIZER=1)
endif()

if (ENABLE_SKIA)
    target_sources(compositorservice PRIVATE
        BitmapExport.cpp
        Canvas2DCommandPlayer.cpp
        CanvasHost.cpp
        ColorSpaceSkia.cpp
        DisplayListPlayerSkia.cpp
        DisplayListRasterCache.cpp
        FontSkia.cpp
        OpenGLContext.cpp
        PaintingSurface.cpp
        SkiaBackendContext.cpp
        SkiaUtils.cpp
        VideoFrameSkia.cpp
    )
    target_link_libraries(compositorservice PRIVATE skia)
else()
    target_sources(compositorservice PRIVATE PaintingSurfaceCPU.cpp)
    target_compile_definitions(compositorservice PUBLIC LADYBIRD_WITHOUT_SKIA=1)
endif()

if (ENABLE_THORVG_GL)
    # ThorVG's private GL dispatch table defines symbols such as glGetString. Linking its archive
    # into the compositor would resolve our system GL function calls to those data symbols.
    get_target_property(thorvg_link_libraries PkgConfig::ThorVG INTERFACE_LINK_LIBRARIES)
    if (thorvg_link_libraries MATCHES "libthorvg-1\\.a(;|$)")
        message(FATAL_ERROR "The ThorVG OpenGL compositor requires a shared ThorVG library (--default-library=shared)")
    endif()
    target_sources(compositorservice PRIVATE ThorVGGLTarget.cpp)
    target_compile_definitions(compositorservice PUBLIC ENABLE_THORVG_GL=1)
    target_link_libraries(compositorservice PRIVATE "-framework OpenGL" "-framework IOSurface")
endif()

add_executable(Compositor main.cpp)

if (LINUX)
    target_sources(Compositor PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(Compositor PRIVATE SandboxMacOS.cpp)
else()
    target_sources(Compositor PRIVATE SandboxUnimplemented.cpp)
endif()

target_include_directories(compositorservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(compositorservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(Compositor PRIVATE compositorservice LibCore LibGfx LibMain LibPlatformFeatures LibSandbox)
# ANGLE must precede Skia so macOS GLES symbols bind to ANGLE rather than the
# OpenGL framework pulled in by Skia.
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibImageDecoderClient LibIPC LibMedia LibCompositing compositor_core)
if (ENABLE_SKIA)
    target_link_libraries(compositorservice PRIVATE ${ANGLE_TARGETS})
endif()

if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(compositorservice INTERFACE LINKER:--allow-multiple-definition)
endif()

if (HAS_FONTCONFIG)
    target_link_libraries(compositorservice PRIVATE Fontconfig::Fontconfig)
    target_link_libraries(Compositor PRIVATE Fontconfig::Fontconfig)
endif()

if (HAS_VULKAN)
    target_sources(compositorservice PRIVATE SkiaVulkanMemoryAllocator.cpp)
    target_link_libraries(compositorservice PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    target_sources(compositorservice PRIVATE VideoFrameSkiaVulkan.cpp)
endif()

if (APPLE)
    if (ENABLE_SKIA)
        target_sources(compositorservice PRIVATE CoreAnimationWindowServer.mm VideoFrameSkia.mm)
    endif()
    target_sources(compositorservice PRIVATE MainDispatchQueue.mm)
    target_link_libraries(compositorservice PUBLIC "-framework AVFoundation" "-framework CoreGraphics" "-framework CoreMedia" "-framework CoreVideo" "-framework QuartzCore")
endif()

if (WIN32 AND HAS_DIRECTX)
    target_sources(compositorservice PRIVATE DirectCompositionWindowServer.cpp)
    # dcomp.h declares composition textures only from NTDDI_WIN10_NI on.
    set_source_files_properties(DirectCompositionWindowServer.cpp PROPERTIES COMPILE_DEFINITIONS NTDDI_VERSION=0x0A00000C)
    target_link_libraries(compositorservice PRIVATE LibThreading d3d11 dcomp)
endif()

if (WIN32)
    ladybird_windows_bin(Compositor CONSOLE)
endif()

ladybird_sign_helper_process(Compositor)
