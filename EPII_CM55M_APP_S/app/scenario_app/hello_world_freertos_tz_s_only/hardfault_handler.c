/*
 Non-TrustZone HardFault handler for Cortex-M55
 Standard FreeRTOS / non-secure build
*/

#include <stdio.h>
#include <stdint.h>
#include "WE2_device.h"  // device header for SCB definitions

void HardFault_Handler(void)
{
    printf("\r\nEntering HardFault_Handler interrupt!\r\n");

    // Print useful SCB fault status registers
    printf("SCB->CFSR: 0x%08lx\n", (unsigned long)SCB->CFSR);
    printf("SCB->HFSR: 0x%08lx\n", (unsigned long)SCB->HFSR);
    printf("SCB->BFAR: 0x%08lx\n", (unsigned long)SCB->BFAR);
    printf("SCB->MMFAR: 0x%08lx\n", (unsigned long)SCB->MMFAR);

    // Trap the CPU in an infinite loop
    for (;;)
        ;
}

void NMI_Handler(void)
{
    printf("\r\nEntering NMI_Handler interrupt!\r\n");
    for (;;)
        ;
}

void MemManage_Handler(void)
{
    printf("\r\nEntering MemManage_Handler interrupt!\r\n");
    for (;;)
        ;
}

void BusFault_Handler(void)
{
    printf("\r\nEntering BusFault_Handler interrupt!\r\n");
    printf("SCB->CFSR: 0x%08lx\n", (unsigned long)SCB->CFSR);
    printf("SCB->BFAR: 0x%08lx\n", (unsigned long)SCB->BFAR);
    printf("SCB->HFSR: 0x%08lx\n", (unsigned long)SCB->HFSR);
    for (;;)
        ;
}

void UsageFault_Handler(void)
{
    printf("\r\nEntering UsageFault_Handler interrupt!\r\n");
    for (;;)
        ;
}

