foreach(t test_firmware test_lexichip test_boot test_larc2 test_cham)
	add_executable(${t} ${t}.cpp)
	target_link_libraries(${t} PRIVATE l960core)
	add_test(NAME ${t} COMMAND ${t})
	set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# LARC2 bring-up tool (not a pass/fail test)
add_executable(larc2_run larc2_run.cpp)
target_link_libraries(larc2_run PRIVATE l960core)

add_executable(l960_bridge l960_bridge.cpp)
target_link_libraries(l960_bridge PRIVATE l960core)

add_executable(lexdump_run lexdump_run.cpp)
target_link_libraries(lexdump_run PRIVATE l960core)

add_executable(lexichip_ir lexichip_ir.cpp)
target_link_libraries(lexichip_ir PRIVATE l960core)

add_executable(lexi_capture lexi_capture.cpp)
target_link_libraries(lexi_capture PRIVATE l960core)
