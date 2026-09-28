if(HANDHELD_HWTEST)
    idf_component_register(SRCS "main.c" REQUIRES driver)
else()
    get_filename_component(UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    get_filename_component(SF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../solar-fast" ABSOLUTE)
    string(TIMESTAMP HANDHELD_BUILD_EPOCH "%s" UTC)
    idf_component_register(
        SRCS "app.c"
             "${UI_ROOT}/ui_gfx.c"
             "${UI_ROOT}/ui_app.c"
             "${UI_ROOT}/ui_view.c"
             "${SF_ROOT}/core/sf_fixed.c"
             "${SF_ROOT}/core/sf_series.c"
             "${SF_ROOT}/core/sf_solve.c"
             "${SF_ROOT}/calendar/sf_calendar.c"
             "${SF_ROOT}/calendar/sf_ganzhi.c"
             "${SF_ROOT}/rise/sf_rise.c"
             "${SF_ROOT}/qishuo/sf_qishuo.c"
             "${SF_ROOT}/bazi/sf_bazi_rules.c"
             "${SF_ROOT}/bazi/sf_bazi.c"
             "${SF_ROOT}/yijing/sf_yijing.c"
        INCLUDE_DIRS "." "${UI_ROOT}" "${SF_ROOT}" "${SF_ROOT}/include"
                     "${SF_ROOT}/core" "${SF_ROOT}/calendar" "${SF_ROOT}/rise"
                     "${SF_ROOT}/qishuo" "${SF_ROOT}/bazi" "${SF_ROOT}/yijing"
        REQUIRES driver esp_timer esp_hw_support bootloader_support
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HANDHELD_BUILD_EPOCH=${HANDHELD_BUILD_EPOCH})
    target_link_libraries(${COMPONENT_LIB} PUBLIC m)
endif()
