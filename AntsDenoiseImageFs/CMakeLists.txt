project(AntsDenoiseImageFs)

include_directories(${FS_INCLUDE_DIRS})

if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
 add_compile_options(-Wno-self-assign-field)
endif()

add_test_script(NAME AntsDenoiseImageFs_test SCRIPT test.sh)

add_executable(AntsDenoiseImageFs AntsDenoiseImageFs.cpp)
add_help(AntsDenoiseImageFs AntsDenoiseImageFs.help.xml)
target_link_libraries(AntsDenoiseImageFs utils "ITKCommon")  # only needs ITKCommon

install(TARGETS AntsDenoiseImageFs DESTINATION bin)
