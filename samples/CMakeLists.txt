add_subdirectory(simple)
if(NOT ANDROID)
    # Other samples haven't been ported to NativeActivity entry yet.
    add_subdirectory(fluent)
    add_subdirectory(winding_test)
endif()
