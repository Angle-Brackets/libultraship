#=================== ImGui ===================
# switch-sdl2 and switch-mesa ship as static portlibs. The order matters for the
# single-pass linker: glad -> EGL -> glapi -> drm_nouveau -> nx.
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_library(GLAD_LIBRARY glad REQUIRED)

target_link_libraries(ImGui PUBLIC SDL2::SDL2 ${GLAD_LIBRARY} OpenGL::EGL nx)
target_include_directories(ImGui PRIVATE ${DEVKITPRO}/portlibs/switch/include/)

# ImGui's bundled GL loader dlopen()s libGL, which Horizon doesn't have. glad
# provides the entry points instead, as the pre-#531 imconfig.h did.
target_compile_definitions(ImGui PRIVATE IMGUI_IMPL_OPENGL_LOADER_CUSTOM GL_GLEXT_PROTOTYPES=1)
# The default open-in-shell handler fork/execs; Horizon has neither.
target_compile_definitions(ImGui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    TARGET_DIRECTORY ImGui PROPERTIES COMPILE_OPTIONS "-include;glad/glad.h")
