# zynq_bm_dma_controller
Zynq BareMetal DMA Controller

Attempt to abstract away the DMA handling code for use with apps. Register TX and RX callbacks with the API. TX callbacks I can't imagine doing much at the moment. RX callback useful for saving the data off to another memory area (ring buffers maybe) to process the data outside of the interrupt context.

Includes builds for the Zedboard and Zybo board for testing the code on. Built in Vivado 2018.2.
