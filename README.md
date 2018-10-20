# zynq_bm_dma_controller
Zynq BareMetal DMA Controller

Requires (AXI DMA)[https://www.xilinx.com/support/documentation/ip_documentation/axi_dma/v7_1/pg021_axi_dma.pdf] to be configured in Scatter Gather (SG) Mode with Dynamic Realignment Engine (DRE) enabled.

Attempt to abstract away the DMA handling code for use with apps. Register TX and RX callbacks with the API. TX callbacks I can't imagine doing much at the moment. RX callback useful for saving the data off to another memory area (ring buffers maybe) to process the data outside of the interrupt context.

Includes builds for the Zedboard and Zybo board for testing the code on. Built in Vivado 2018.2.
