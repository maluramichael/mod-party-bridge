if(TARGET modules)
  find_package(hiredis CONFIG QUIET)
  if(hiredis_FOUND)
    # Same hiredis wiring as mod-telemetry-redis: RelWithDebInfo has no import config of
    # its own, so map it to Release (guarded, the target may already be configured).
    if(TARGET hiredis::hiredis)
      set_target_properties(hiredis::hiredis PROPERTIES MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
    endif()
    target_link_libraries(modules PRIVATE hiredis::hiredis)
    target_compile_definitions(modules PRIVATE PARTY_BRIDGE_AVAILABLE)
    target_include_directories(modules PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/src
      ${CMAKE_CURRENT_LIST_DIR}/deps)
    if(WIN32)
      target_link_libraries(modules PRIVATE ws2_32)
    endif()
  else()
    message(WARNING "[mod-party-bridge] hiredis not found - party bridge disabled")
  endif()
endif()
