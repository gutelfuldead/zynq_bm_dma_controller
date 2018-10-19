#include "stdio.h"
#include "stdlib.h"
#include "axis_dma_controller.h"

#define MEM_BASE_ADDR		(XPAR_PS7_DDR_0_S_AXI_BASEADDR + 0x1000000) /* 0x00100000 - 0x001fffff */
#define MEM_REGION_BD_SIZE      (0x0000FFFF)
#define MEM_REGION_BUF_SIZE     (0x00020000)
#define BUFFER_SIZE             1102
#define MAX_PKT_SIZE            BUFFER_SIZE

static int tx_count;
static int rx_count;
static int error;

static XScuGic intc;
static uint8_t txPkt[MAX_PKT_SIZE];

static void tx_callback(void);
static void rx_callback(uint32_t buf_addr, uint32_t buf_len);

int axis_dma_controller_sample_exec(int numTestPkts)
{
	int rc;
	struct axisDmaCtrl_params params;
	int i;

	params.rx_bd_space_base = MEM_BASE_ADDR;
	params.rx_bd_space_high = params.rx_bd_space_base + MEM_REGION_BD_SIZE;
	params.tx_bd_space_base = params.rx_bd_space_high; 
	params.tx_bd_space_high = params.tx_bd_space_base + MEM_REGION_BD_SIZE;
	params.tx_buffer_base   = params.tx_bd_space_high; 
	params.tx_buffer_high   = params.tx_buffer_base + MEM_REGION_BUF_SIZE;
	params.rx_buffer_base   = params.tx_buffer_high; 
	params.rx_buffer_high   = params.rx_buffer_base + MEM_REGION_BUF_SIZE;
	params.bd_buf_size      = BUFFER_SIZE;
	params.coalesce_count   = 1;

	tx_count = 0;
	rx_count = 0;
	for(i=0; i<MAX_PKT_SIZE; i++)
		txPkt[i] = i % 255;

	rc = axisDmaCtrl_init(&params, &intc, rx_callback, tx_callback);

	while(rx_count < numTestPkts && !error){
		rc = axisDmaCtrl_sendPackets(&txPkt[0], MAX_PKT_SIZE);
		if(rc != XST_SUCCESS)
			return XST_FAILURE;
	}
	if(error){
		printf("Test Failed w/ error\r\n");
		return XST_FAILURE;
	}

	printf("Test succesful\r\n");
	return XST_SUCCESS;
	
}

static void tx_callback(void)
{
	tx_count++;
}

static void rx_callback(uint32_t buf_addr, uint32_t buf_len)
{
	/* check data */
	uint8_t *rxPacket = (u8 *)buf_addr;
	int j = 0;
	for(j = 0; j < buf_len; j++){
		if(*(rxPacket+j) != txPkt[j]){
			printf("[%04d]: tx %03d, rx %03d @ 0x%x\r\n",j,txPkt[j],*(rxPacket+j),(rxPacket+j));
			error = 1;
			break;
		}
	}
	rx_count++;
}