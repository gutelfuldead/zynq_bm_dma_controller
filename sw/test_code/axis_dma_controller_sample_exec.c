/**
 * @brief
 * @author Jason Gutel (github.com/gutelfuldead)
 */

#include "stdio.h"
#include "stdlib.h"
#include "axis_dma_controller.h"

#define MEM_BASE_ADDR		(XPAR_PS7_DDR_0_S_AXI_BASEADDR + 0x1000000) /* 0x00100000 - 0x001fffff */
#define MEM_REGION_BD_SIZE      (0x0000FFF)
#define MEM_REGION_BUF_SIZE     (0x0001FFF)
int BD_BUF_SIZE;
int MAX_PKT_SIZE;

#define DMA_DEV_ID	   XPAR_AXIDMA_0_DEVICE_ID
#define XScuGic_DEVICE_ID  XPAR_SCUGIC_SINGLE_DEVICE_ID
#define RX_INTR_ID	   XPAR_FABRIC_AXIDMA_0_S2MM_INTROUT_VEC_ID
#define TX_INTR_ID	   XPAR_FABRIC_AXIDMA_0_MM2S_INTROUT_VEC_ID

static int tx_bd_count;
static int rx_bd_count;
static int rx_pkt_count;
static int error;
static int pkt_complete = 1;
static int pkt_bytes_rx = 0;

static XScuGic intc;
static uint8_t txPkt[1024*1024];

static int gic_init(void);
static void gic_enable(void);
static void tx_callback(void);
static void rx_callback(uint32_t buf_addr, uint32_t buf_len);

/**
  * This application exec sends toy data through the DMA engine --> FIFO --> DMA Engine and
  * checks that the data is received in order with no errors
  *
  * @param numTestPkts Number of packets to run through before claiming success/failure
  * @param pktSize     Number of bytes in a single packet
  * @param bufSize     Size of the DMA BD buffer in bytes
  */
int axis_dma_controller_sample_exec(int numTestPkts, int pktSize, int bufSize)
{
	int rc;
	struct axisDmaCtrl_params params;
	int i;

	printf("%s : Starting w/ %d packets of %d bytes using buffers of %d bytes\r\n",
		__func__,numTestPkts, pktSize, bufSize);

	BD_BUF_SIZE  = bufSize;
	MAX_PKT_SIZE = pktSize;

	params.rx_bd_space_base = MEM_BASE_ADDR;
	params.rx_bd_space_high = params.rx_bd_space_base + MEM_REGION_BD_SIZE;
	params.tx_bd_space_base = params.rx_bd_space_high + 1;
	params.tx_bd_space_high = params.tx_bd_space_base + MEM_REGION_BD_SIZE;
	params.tx_buffer_base   = params.tx_bd_space_high + 1;
	params.tx_buffer_high   = params.tx_buffer_base + MEM_REGION_BUF_SIZE;
	params.rx_buffer_base   = params.tx_buffer_high + 1;
	params.rx_buffer_high   = params.rx_buffer_base + MEM_REGION_BUF_SIZE;
	params.bd_buf_size      = BD_BUF_SIZE;
	params.coalesce_count   = 1;
	params.txIrqPriority    = 0xA0;
	params.rxIrqPriority    = 0xA0;
	params.txIrqId          = TX_INTR_ID;
	params.rxIrqId          = RX_INTR_ID;
	params.dmaDevId         = DMA_DEV_ID;
	params.txEn             = 1;
	params.rxEn             = 1;

	axisDmaCtrl_printParams(&params);

	tx_bd_count  = 0;
	rx_bd_count  = 0;
	rx_pkt_count = 0;
	error    = 0;
	pkt_complete = 1;
	pkt_bytes_rx = 0;

	for(i=0; i<MAX_PKT_SIZE; i++)
		txPkt[i] = i % 255;

	rc = gic_init();
	if (rc)
		return -1;

	/* DMA Setup */
	rc = axisDmaCtrl_init(&params, &intc, rx_callback, tx_callback);
	if(rc){
		printf("axisDmaCtrl_init failed!\r\n");
		return XST_FAILURE;
	}

	gic_enable();
	
	while (rx_pkt_count < numTestPkts){}

	printf("Done!\r\n");
	printf("tx_bds : %d, rx_bds %d, rx_packets %d\r\n",tx_bd_count,rx_bd_count,rx_pkt_count);
	if(error){
		printf("!! Test Failed w/ error !!\r\n");
		return XST_FAILURE;
	}
	printf("Test successful\r\n\n");

	axisDmaCtrl_disable(&intc);

	return XST_SUCCESS;
}

static int gic_init(void)
{
	int rc;
	XScuGic_Config *intcConfig;
	/*
  	 * Initialize the interrupt controller driver so that it is ready to
  	 * use.
  	 */
	intcConfig = XScuGic_LookupConfig(DMA_DEV_ID);
	if (NULL == intcConfig) {
		return XST_FAILURE;
	}

	rc = XScuGic_CfgInitialize(&intc, intcConfig,
					intcConfig->CpuBaseAddress);
	if (rc != XST_SUCCESS) {
		return XST_FAILURE;
	}
	return 0;
}

static void gic_enable(void)
{
	/* Enable interrupts from the hardware */
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)XScuGic_InterruptHandler,
			(void *)&intc);
	Xil_ExceptionEnable();
}

static void tx_callback(void)
{
	tx_bd_count++;
}

static void rx_callback(uint32_t buf_addr, uint32_t buf_len)
{
	int j = 0;
	uint8_t *rxPacket;

	// printf("pkt_complete : %s, pkt_bytes_rx : %d, buf_len : %d\r\n",pkt_complete?"T":"F", (int)pkt_bytes_rx, (int)buf_len);

	rxPacket = (u8 *)buf_addr;

	if (pkt_complete){
		/* check data */
		for(j = 0; j < buf_len; j++){
			if(*(rxPacket+j) != txPkt[j]){
				printf("%s ERROR : pkt %d : tx[%04d]=%03d, rx[%04d]=%03d @ 0x%x\r\n",
					__func__,rx_bd_count,j,txPkt[j],j,*(rxPacket+j),(unsigned int)(rxPacket+j));
				error = 1;
				break;
			}
		}
	} else {
		int txOffset = pkt_bytes_rx;
		/* check data */
		for(j = 0; j < buf_len; j++){
			if(*(rxPacket+j) != txPkt[j+txOffset]){
				 printf("%s ERROR : pkt %d : tx[%04d]=%03d, rx[%04d]=%03d @ 0x%x\r\n",
					 __func__,rx_bd_count,j+txOffset,txPkt[j+txOffset],j,*(rxPacket+j),(unsigned int)(rxPacket+j));
				error = 1;
				break;
			}
		}
	}

	pkt_bytes_rx += buf_len;
	if(error || pkt_bytes_rx >= MAX_PKT_SIZE){
		pkt_complete = 1;
		pkt_bytes_rx = 0;
		rx_pkt_count++;
	} else {
		pkt_complete = 0;
	}

	memset((void *)buf_addr, 0, buf_len);
	rx_bd_count++;
}
