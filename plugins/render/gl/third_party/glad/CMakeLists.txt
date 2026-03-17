add_library(glad2 SHARED
    src/gl.c
    include/glad/gl.h
    include/KHR/khrplatform.h
)

target_include_directories(glad2
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Export GL function pointers from the DLL so all consumers share the same globals.
# GLAD_API_CALL_EXPORT enables dllexport/dllimport decoration.
# GLAD_API_CALL_EXPORT_BUILD selects dllexport (set only when building glad2 itself).
target_compile_definitions(glad2
    PRIVATE GLAD_API_CALL_EXPORT GLAD_API_CALL_EXPORT_BUILD
    PUBLIC  GLAD_API_CALL_EXPORT
)
