idf_component_register(SRCS "rcp_led_bridge.cpp" INCLUDE_DIRS "."
    REQUIRES openthread esp_timer)

# Decode vendor status after the existing HDLC/Spinel parser has validated it.
# Customize a build-local copy, never the installed ESP-IDF source.
idf_component_get_property(ot_dir openthread COMPONENT_DIR)
idf_component_get_property(ot_lib openthread COMPONENT_LIB)
set(original "${ot_dir}/openthread/src/lib/spinel/radio_spinel.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
file(READ "${original}" contents)
set(anchor "void RadioSpinel::HandleValueIs(spinel_prop_key_t aKey, const uint8_t *aBuffer, uint16_t aLength)\n{")
string(FIND "${contents}" "${anchor}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "Unsupported ESP-IDF RadioSpinel::HandleValueIs layout")
endif()
string(REPLACE "${anchor}" "${anchor}
    if (aKey == 15360) {
        thread_br_rcp_led_report(aBuffer, aLength);
        return;
    }" contents "${contents}")
set(patched "${CMAKE_CURRENT_BINARY_DIR}/radio_spinel_led.cpp")
file(WRITE "${patched}" "#include \"rcp_led_bridge.h\"\n${contents}")
get_target_property(ot_sources ${ot_lib} SOURCES)
set(source_found FALSE)
foreach(source IN LISTS ot_sources)
    if(source MATCHES "(^|/)radio_spinel\\.cpp$")
        list(REMOVE_ITEM ot_sources "${source}")
        set(source_found TRUE)
    endif()
endforeach()
if(NOT source_found)
    message(FATAL_ERROR "ESP-IDF OpenThread radio_spinel.cpp was not found")
endif()
set_property(TARGET ${ot_lib} PROPERTY SOURCES "${ot_sources}")
target_sources(${ot_lib} PRIVATE "${patched}")
target_include_directories(${ot_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
