// This file provides the missing definition for the linker.
#include <cstdio>
#include "csp/csp4cmsis.h" // Includes the declaration

// Declare the test function defined in tests.cpp
extern void RunProcessingChainTest(void); 

// Define the required function with C linkage
extern "C" void csp_app_main_init(void) {
    printf("Application initialization (via csp_app_main_init) started.\r\n");

    // Call the C++ function that creates and runs the CSP tasks
    RunProcessingChainTest(); 

    printf("Application tasks created successfully.\r\n");
}
