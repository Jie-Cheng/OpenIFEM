# serial tests
set(serial_tests acoustic_duct_wave
                 acoustic_pml
                 fluid_cavity
                 fluid_cylinder
                 fluid_cylinder_insimex
                 fluid_gravity
                 fluid_pipe
                 fluid_pressure_driven
                 fsi_cavity
                 fsi_gravity
                 fsi_leaflet
                 solid_beam_bending_linearelastic
                 solid_beam_bending_NeoHookean
                 solid_gravity_hyperelastic
                 solid_gravity_linearelastic)

# mpi tests
set(mpi_tests acoustic_duct_wave_mpi
              acoustic_pml_mpi
              fluid_cylinder_mpi
              fluid_cylinder_mpi_insimex
              fluid_pipe_mpi
              fsi_gravity_mpi
              fsi_leaflet_mpi
              solid_beam_bending_mpi_linearelastic
              solid_beam_bending_mpi_NeoHookean
              solid_beam_bending_mpi_shared_linearelastic
              solid_beam_bending_mpi_shared_NeoHookean)

set(rkpm-rk4_serial_tests rkpm-rk4-bending)

set(rkpm-rk4_mpi_tests rkpm-rk4-bending-mpi
                       rkpm-rk4-3D
                       fsi-rkpm-rk4
                       fsi-wall-3D)

# All tests
set(tests ${serial_tests} ${mpi_tests})

# Create a subdirectory and add an executible for each test
foreach(test ${tests})
  set(input ${CMAKE_CURRENT_SOURCE_DIR}/${test}/${test}.prm)
  set(output ${CMAKE_CURRENT_BINARY_DIR}/${test})
  file(MAKE_DIRECTORY ${output})
  add_executable(${test} ${CMAKE_CURRENT_SOURCE_DIR}/${test}/${test}.cpp)
  target_include_directories(${test} PUBLIC "${CMAKE_SOURCE_DIR}/include")
  deal_ii_setup_target(${test})
  target_link_libraries(${test} openifem stdc++fs)
  list(FIND mpi_tests ${test} index)
  if(${index} GREATER -1)
    # FIXME: it is not good practice to specify the number of processors in this way
    add_test(NAME ${test} COMMAND mpirun -n 2 ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test} ${input} WORKING_DIRECTORY ${output})
  else()
    add_test(NAME ${test} COMMAND ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test} ${input} WORKING_DIRECTORY ${output})
  endif()
endforeach()

if (OPENIFEM_WITH_rkpm-rk4)
  set(rkpm-rk4_tests ${rkpm-rk4_serial_tests} ${rkpm-rk4_mpi_tests})
  foreach(test ${rkpm-rk4_tests})
    set(input ${CMAKE_CURRENT_SOURCE_DIR}/${test}/${test}.prm)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/${test})
    file(MAKE_DIRECTORY ${output})
    add_executable(${test} ${CMAKE_CURRENT_SOURCE_DIR}/${test}/${test}.cpp)
    target_include_directories(${test} PUBLIC "${CMAKE_SOURCE_DIR}/include" ${rkpm-rk4_INCLUDE_DIR} ${EIGEN3_INCLUDE_DIR})
    deal_ii_setup_target(${test})
    target_link_libraries(${test} openifem stdc++fs ${rkpm-rk4_LIBRARY})
    list(FIND rkpm-rk4_mpi_tests ${test} index)
    if(${index} GREATER -1)
      # FIXME: it is not good practice to specify the number of processors in this way
      add_test(NAME ${test} COMMAND mpirun -n 2 ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test} ${input} WORKING_DIRECTORY ${output})
    else()
      add_test(NAME ${test} COMMAND ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test} ${input} WORKING_DIRECTORY ${output})
    endif()
  endforeach()
endif()
