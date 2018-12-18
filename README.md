# zynq_bm_dma_controller
Zynq BareMetal DMA Controller

Requires [AXI DMA](https://www.xilinx.com/support/documentation/ip_documentation/axi_dma/v7_1/pg021_axi_dma.pdf) to be configured in Scatter Gather (SG) Mode with Dynamic Realignment Engine (DRE) enabled.

Attempt to abstract away the DMA handling code for use with apps. Register TX and RX callbacks with the API. TX callbacks I can't imagine doing much at the moment. RX callback useful for saving the data off to another memory area (ring buffers maybe) to process the data outside of the interrupt context.

Includes builds for the Zedboard and Zybo board for testing the code on. Built in Vivado 2018.2.

## fw

Contains reference design with DMA in loopback w/ a FIFO for ZedBoard and
[Zybo](https://github.com/Digilent/vivado-boards/tree/master/old/board_parts/zynq/zybo/1.0)

run `make` to see commands. Requires Vivado 2018.2 to use auto-build.

## sw

### src

Contains API files,

```
sw/src
.
├── axis_dma_controller.c
└── axis_dma_controller.h
```

### test_code

Contains bare-metal test code to exercise interface and show sample toy application
```
sw/test_code
.
├── axis_dma_controller_sample_exec.c
├── axis_dma_controller_sample_exec.h
├── lscript.ld
├── main.c
├── platform.c
├── platform_config.h
└── platform.h
```
