#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"
#include "dma_sg_intr.h"
#include "dma_sg_poll.h"
#include "dma_sg_cyclic_intr.h"
#include "demo_dma_standalone.h"
#include "axis_dma_controller_sample_exec.h"

int main()
{
    init_platform();

    xil_printf("\n\n\rStarting DMA Testing...\n\r");

// dma_sg_poll_exec();
// dma_sg_intr_exec();
// dma_sg_cyclic_intr_exec();
// demo_dma_loopback();
    axis_dma_controller_sample_exec(4000);

    cleanup_platform();
    return 0;
}
