#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"
#include "axis_dma_controller_sample_exec.h"

int main()
{
    init_platform();

    xil_printf("\n\n\rStarting DMA Testing...\n\r");

    axis_dma_controller_sample_exec(1000);

    cleanup_platform();
    return 0;
}
