cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME 2017H)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/ad9959.c
    Core/Src/dds_console.c
    Core/Src/ads1256.c
    Core/Src/adc_console.c
)

option(ENABLE_DRDY_DIAG "Measure ADS1256 DRDY with TIM3_CH3" OFF)
option(ENABLE_AUTO_SWEEP "Run one headless 1-40 MHz sweep on reset" OFF)
option(ENABLE_DETECTOR_TEST "Measure DDS on/off at 100 kHz instead of sweeping" OFF)
if(ENABLE_DRDY_DIAG AND ENABLE_AUTO_SWEEP)
    message(FATAL_ERROR "Choose either DRDY diagnostics or auto sweep")
endif()
if(ENABLE_DRDY_DIAG)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/drdy_diag.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_DRDY_DIAG)
endif()
if(ENABLE_AUTO_SWEEP)
    set(SWEEP_AMPLITUDE_CODE "16" CACHE STRING "DDS amplitude code used by the sweep")
    if(NOT SWEEP_AMPLITUDE_CODE MATCHES "^[1-9][0-9]*$" OR SWEEP_AMPLITUDE_CODE GREATER 1023)
        message(FATAL_ERROR "SWEEP_AMPLITUDE_CODE must be an integer from 1 to 1023")
    endif()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/auto_sweep.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_AUTO_SWEEP SWEEP_AMPLITUDE_CODE=${SWEEP_AMPLITUDE_CODE})
    if(ENABLE_DETECTOR_TEST)
        set(DETECTOR_ADC_CHANNEL "0" CACHE STRING "Chip input used by the detector test")
        if(NOT DETECTOR_ADC_CHANNEL MATCHES "^[01]$")
            message(FATAL_ERROR "DETECTOR_ADC_CHANNEL must be 0 or 1")
        endif()
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_DETECTOR_TEST DETECTOR_ADC_CHANNEL=${DETECTOR_ADC_CHANNEL})
    endif()
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    VERBATIM
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
