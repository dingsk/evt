file(GLOB HEADERS "include/libevt/*.h")
add_library( libevt SHARED 
    evt.cpp
    evt_ecc.cpp
    evt_abi.cpp
    evt_address.cpp
    ${HEADERS} )
set_target_properties( libevt PROPERTIES OUTPUT_NAME evt )

target_link_libraries( libevt
        PRIVATE evt_chain_lite fc_lite ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

target_include_directories( libevt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
set_target_properties( libevt PROPERTIES COMPILE_FLAGS "-Wl,-rpath,'$ORIGIN'")

install( TARGETS
   libevt

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

if(ENABLE_TESTING)
    add_subdirectory( test )
endif()