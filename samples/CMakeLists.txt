add_subdirectory(simple)
if(NOT ANDROID)
    # Other samples haven't been ported to NativeActivity entry yet.
    add_subdirectory(fluent)
    add_subdirectory(winding_test)
    add_subdirectory(scale)
    add_subdirectory(inspect)
    add_subdirectory(text_field)
endif()
