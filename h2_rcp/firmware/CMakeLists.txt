idf_component_register(
    SRCS "esp_ot_rcp.c" "h2_leds.c" "h2_usb_log.c"
    INCLUDE_DIRS "."
    REQUIRES openthread nvs_flash esp_event vfs ot_examples_common esp_driver_gpio esp_timer esp_driver_usb_serial_jtag
)

# Compile the customized vendor hook in OpenThread's own target so its private
# headers and compile definitions match. Never modify the installed ESP-IDF.
idf_component_get_property(ot_lib openthread COMPONENT_LIB)
get_target_property(ot_sources ${ot_lib} SOURCES)
set(vendor_source_found FALSE)
foreach(source IN LISTS ot_sources)
    if(source MATCHES "(^|/)esp_openthread_ncp\\.cpp$")
        list(REMOVE_ITEM ot_sources "${source}")
        set(vendor_source_found TRUE)
    endif()
endforeach()
if(NOT vendor_source_found)
    message(FATAL_ERROR "ESP-IDF OpenThread NCP vendor source was not found")
endif()
set_property(TARGET ${ot_lib} PROPERTY SOURCES "${ot_sources}")
target_sources(${ot_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/esp_openthread_ncp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/h2_led_report.cpp")
target_include_directories(${ot_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Observe UART activity without replacing the UART driver or HDLC parser.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=otPlatUartReceived" "-Wl,--wrap=otPlatUartSend"
    "-Wl,--wrap=otPlatLogOutput" "-Wl,--wrap=otPlatLog"
    # These wrappers live in a separate archive member with no ordinary call
    # sites. Force extraction: --wrap alone does not reliably pull it in when
    # OpenThread's original log symbols are also defined in the link graph.
    "-Wl,--undefined=__wrap_otPlatLogOutput" "-Wl,--undefined=__wrap_otPlatLog")
