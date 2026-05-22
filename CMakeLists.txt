# Page Manager - Dual-mode CMakeLists
# ESP-IDF: used as a component (idf_component_register)
# Standalone: add_subdirectory() produces an imported interface + static library

set(PM_SOURCES "src/page_manager.c")
set(PM_INCLUDE_DIRS "include")

if(COMMAND idf_component_register)
    # ESP-IDF component mode
    idf_component_register(
        SRCS ${PM_SOURCES}
        INCLUDE_DIRS ${PM_INCLUDE_DIRS}
        REQUIRES lvgl
    )
else()
    # Standard CMake library mode
    add_library(page_manager STATIC ${PM_SOURCES})

    target_include_directories(page_manager PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/${PM_INCLUDE_DIRS}
    )

    # Propagate LVGL dependency (the consumer must provide an "lvgl" target)
    target_link_libraries(page_manager PUBLIC lvgl)
endif()
