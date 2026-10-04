cmake_minimum_required(VERSION 3.16)
project(reset_ui_preview LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${APP_ROOT}/managed_components/lvgl__lvgl/CMakeLists.txt")
    message(FATAL_ERROR "LVGL source is missing. Resolve the pinned IDF dependencies first.")
endif()
add_subdirectory("${APP_ROOT}/managed_components/lvgl__lvgl" lvgl)
add_executable(reset_ui_preview
    preview.c
    "${APP_ROOT}/main/reset_ui.c"
    "${APP_ROOT}/main/reset_text.c"
    "${APP_ROOT}/main/reset_presenter.c"
    "${APP_ROOT}/main/reset_hold.c"
    "${APP_ROOT}/main/reset_settings.c"
    "${APP_ROOT}/main/reset_feed.c"
    "${APP_ROOT}/tests/vendor/cjson/cJSON.c"
    "${APP_ROOT}/components/bsp/src/bsp_display_rounding.c"
    "${APP_ROOT}/assets/fonts/reset_font_16.c"
    "${APP_ROOT}/assets/fonts/reset_font_12.c"
    "${APP_ROOT}/assets/fonts/reset_font_24.c")
target_include_directories(reset_ui_preview PRIVATE "${APP_ROOT}/main"
    "${APP_ROOT}/tests/vendor/cjson" "${APP_ROOT}/components/bsp/src")
# Read the actual board macros without importing ESP-IDF-specific declarations.
function(board_constant output header macro)
    file(STRINGS "${header}" definition REGEX "^#define[ \t]+${macro}[ \t]+[0-9]+")
    if(NOT definition)
        message(FATAL_ERROR "Missing board geometry macro ${macro}")
    endif()
    string(REGEX REPLACE ".*${macro}[ \t]+([0-9]+).*" "\\1" value "${definition}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()
board_constant(PREVIEW_WIDTH "${APP_ROOT}/components/bsp/include/bsp_pins.h" BSP_LCD_W)
board_constant(PREVIEW_HEIGHT "${APP_ROOT}/components/bsp/include/bsp_pins.h" BSP_LCD_H)
board_constant(PREVIEW_RADIUS "${APP_ROOT}/components/bsp/include/bsp_display.h" BSP_LVGL_SCREEN_RADIUS)
target_compile_definitions(reset_ui_preview PRIVATE
    PREVIEW_WIDTH=${PREVIEW_WIDTH} PREVIEW_HEIGHT=${PREVIEW_HEIGHT} PREVIEW_RADIUS=${PREVIEW_RADIUS}
    RESET_FONT_INVENTORY="${APP_ROOT}/assets/fonts/reset_font_text.txt")
target_compile_options(reset_ui_preview PRIVATE -Wall -Wextra -Werror)
target_link_libraries(reset_ui_preview PRIVATE lvgl::lvgl m)
