/**
  * author jason gutel
  */

/** @todo
 * 1. Set up ring buffer to dump tx packets into
 * 2. Timer interrupt at provided rate
 * 3. In interrupt take element from RB and send over tx chain
 */ 

#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_mmu.h"
#include "stdio.h"
#include "axis_dma_controller.h"

/******************** Constant Definitions **********************************/
/*
 *  * Device hardware build related constants.
 *   */
#define XScuGic_HANDLER	XScuGic_InterruptHandler
#define DMA_DEV_ID	XPAR_AXIDMA_0_DEVICE_ID
#define XScuGic_DEVICE_ID  XPAR_SCUGIC_SINGLE_DEVICE_ID
#define RX_INTR_ID	XPAR_FABRIC_AXIDMA_0_S2MM_INTROUT_VEC_ID
#define TX_INTR_ID	XPAR_FABRIC_AXIDMA_0_MM2S_INTROUT_VEC_ID

/* THE MEMORY SECTION NEEDS TO BE MARKED NON-CACHEABLE BY THE TLB */
#define ONE_MB              0x100000U

/* Timeout loop counter for reset */
#define RESET_TIMEOUT_COUNTER	10000


/* The interrupt coalescing threshold and delay timer threshold
 * Valid range is 1 to 255
 */
#define DELAY_TIMER_COUNT		100

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/
#define ERROR_PRINT(fmt, args...) printf("ERROR: %s:%d(): " fmt, \
    __func__,__LINE__, ##args)

#define DEBUG_PRINT(fmt, args...) printf("DEBUG: %s:%d(): " fmt, \
    __func__,__LINE__, ##args)

/************************** Function Prototypes ******************************/
static void TxIntrHandler(void *Callback);
static void RxIntrHandler(void *Callback);
static int SetupIntrSystem(XScuGic * IntcInstancePtr, XAxiDma * AxiDmaPtr, u16 TxIntrId, u16 RxIntrId);
static void DisableIntrSystem(XScuGic * IntcInstancePtr, u16 TxIntrId, u16 RxIntrId);
static int RxSetup(XAxiDma * AxiDmaInstPtr);
static int TxSetup(XAxiDma * AxiDmaInstPtr);
static void tx_irq_bd_handler(XAxiDma_BdRing * TxRingPtr);
static void rx_irq_bd_handler(XAxiDma_BdRing * RxRingPtr);
static int mark_mem_noncache(struct axisDmaCtrl_params *mem);
static struct axisDmaCtrl_params copy_axisDmaCtrl_params_struct(struct axisDmaCtrl_params * in);
static void empty_memspace(struct axisDmaCtrl_params * in);

/************************** Variable Definitions *****************************/
static XAxiDma AxiDma;

static tx_cb_t _tx_cb = NULL;
static rx_cb_t _rx_cb = NULL;
static struct axisDmaCtrl_params params;


int axisDmaCtrl_init(struct axisDmaCtrl_params *paramsIn, 
	XScuGic * IntcInstancePtr,
	rx_cb_t rxCb,
	tx_cb_t txCb)
{
	int rc;
	XAxiDma_Config *config;

	rc = axisDmaCtrl_register_tx_cb(txCb);
	if(rc == XST_FAILURE)
		return rc;

	axisDmaCtrl_register_rx_cb(rxCb);
	if(rc == XST_FAILURE)
		return rc;

	params = copy_axisDmaCtrl_params_struct(paramsIn);

	rc = mark_mem_noncache(&params);
	if (!rc) {
		return XST_FAILURE;
	}

	rc = XAxiDma_LookupConfig(DMA_DEV_ID);
	if (!rc) {
		ERROR_PRINT("No config found for %d\r\n", DMA_DEV_ID);
		return XST_FAILURE;
	}

	/* Initialize DMA engine */
	XAxiDma_CfgInitialize(&AxiDma, config);

	if(!XAxiDma_HasSg(&AxiDma)) {
		ERROR_PRINT("Device configured as Simple mode -- not scatter gather\r\n");
		return XST_FAILURE;
	}

	/* Set up TX/RX channels to be ready to transmit and receive packets */
	rc = TxSetup(&AxiDma);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed TX setup\r\n");
		return XST_FAILURE;
	}

	rc = RxSetup(&AxiDma);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed RX setup\r\n");
		return XST_FAILURE;
	}

	/* Set up Interrupt system  */
	rc = SetupIntrSystem(IntcInstancePtr, &AxiDma, TX_INTR_ID, RX_INTR_ID);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed interrupt setup\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

int axisDmaCtrl_disable(XScuGic * IntcInstancePtr)
{
	DisableIntrSystem(IntcInstancePtr, TX_INTR_ID, RX_INTR_ID);
	_tx_cb = NULL;
	_rx_cb = NULL;
	empty_memspace(&params);
}

int axisDmaCtrl_register_tx_cb(tx_cb_t cb)
{
	if(cb == NULL)
		return XST_FAILURE;
	_tx_cb = cb;
	return XST_SUCCESS;
}

int axisDmaCtrl_register_rx_cb(rx_cb_t cb)
{
	if(cb == NULL)
		return XST_FAILURE;
	_rx_cb = cb;
	return XST_SUCCESS;
}

int axisDmaCtrl_sendPackets(uint8_t * packetBuf, size_t packetSize)
{
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(&AxiDma);
	XAxiDma_Bd *BdPtr, *BdCurPtr;
	int rc;
	u32 BufferAddr;
	int reqBds = 0;
	int i = 0;

	/* find number of bds required */
	reqBds = packetSize / params.bd_buf_size;
	reqBds += (packetSize % params.bd_buf_size) ? 1 : 0;
	
	/* Flush the SrcBuffer before the DMA transfer, in case the Data Cache
         * is enabled
	 */
	Xil_DCacheFlushRange((UINTPTR)packetBuf, params.bd_buf_size * reqBds);

	rc = XAxiDma_BdRingAlloc(TxRingPtr, reqBds, &BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed bd alloc\r\n");
		return E_NOBDS;
	}

	BufferAddr = (UINTPTR)packetBuf;
	BdCurPtr = BdPtr;

	for(i = 0; i < reqBds; i++){
		u32 CrBits = 0;

		rc = XAxiDma_BdSetBufAddr(BdPtr, BufferAddr);
		if (rc != XST_SUCCESS) {
			ERROR_PRINT("Tx set buffer addr %x on BD %x failed %d\r\n",
			(unsigned int)BufferAddr,
			(UINTPTR)BdPtr, rc);
			return XST_FAILURE;
		}

		rc = XAxiDma_BdSetLength(BdPtr, params.bd_buf_size,
					TxRingPtr->MaxTransferLen);
		if (rc != XST_SUCCESS) {
			ERROR_PRINT("Tx set length %d on BD %x failed %d\r\n",
			params.bd_buf_size, (UINTPTR)BdPtr, rc);
			return XST_FAILURE;
		}

		if(i == 0)
			CrBits |= XAXIDMA_BD_CTRL_TXSOF_MASK;
		if(i == reqBds - 1)
			CrBits |= XAXIDMA_BD_CTRL_TXEOF_MASK;

		XAxiDma_BdSetCtrl(BdPtr, CrBits);
		XAxiDma_BdSetId(BdPtr, BufferAddr);

		BufferAddr += params.bd_buf_size;
		BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(TxRingPtr, BdCurPtr);
	}

	/* Give the BD to hardware */
	rc = XAxiDma_BdRingToHw(TxRingPtr, reqBds, BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed to hw, length %d\r\n",
			(int)XAxiDma_BdGetLength(BdPtr,
					TxRingPtr->MaxTransferLen));
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

static int mark_mem_noncache(struct axisDmaCtrl_params *mem)
{
	int i = 0;
	if(mem->rx_bd_space_base == NULL
		|| mem->rx_bd_space_high == NULL
		|| mem->tx_bd_space_base == NULL
		|| mem->tx_bd_space_high == NULL
		|| mem->tx_buffer_base == NULL
		|| mem->tx_buffer_high == NULL
		|| mem->rx_buffer_base == NULL
		|| mem->rx_buffer_high == NULL
		|| mem->bd_buf_size == NULL
		|| mem->coalesce_count == 0)
		return XST_FAILURE;
	for(i = mem->tx_bd_space_base; i < mem->tx_bd_space_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->rx_bd_space_base; i < mem->rx_bd_space_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->tx_buffer_base; i < mem->tx_buffer_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->rx_buffer_base; i < mem->rx_buffer_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	return XST_SUCCESS;
}

static struct axisDmaCtrl_params copy_axisDmaCtrl_params_struct(struct axisDmaCtrl_params * in)
{
	struct axisDmaCtrl_params tmp;
	tmp.rx_bd_space_base = in->rx_bd_space_base;
	tmp.rx_bd_space_high = in->rx_bd_space_high;
	tmp.tx_bd_space_base = in->tx_bd_space_base;
	tmp.tx_bd_space_high = in->tx_bd_space_high;
	tmp.tx_buffer_base = in->tx_buffer_base;
	tmp.tx_buffer_high = in->tx_buffer_high;
	tmp.rx_buffer_base = in->rx_buffer_base;
	tmp.rx_buffer_high = in->rx_buffer_high;
	tmp.bd_buf_size = in->bd_buf_size;
	tmp.coalesce_count = in->coalesce_count;
	return tmp;
}

static void empty_memspace(struct axisDmaCtrl_params * in)
{
	in->rx_bd_space_base = NULL;
	in->rx_bd_space_high = NULL;
	in->tx_bd_space_base = NULL;
	in->tx_bd_space_high = NULL;
	in->tx_buffer_base   = NULL;
	in->tx_buffer_high   = NULL;
	in->rx_buffer_base   = NULL;
	in->rx_buffer_high   = NULL;
	in->bd_buf_size      = 0;	
	in->coalesce_count   = 1;	
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA TX callback function to be called by TX interrupt handler.
 * * This function handles BDs finished by hardware.
 * *
 * * @param	TxRingPtr is a pointer to TX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void tx_irq_bd_handler(XAxiDma_BdRing * TxRingPtr)
{
	int BdCount;
	u32 BdSts;
	XAxiDma_Bd *BdPtr;
	XAxiDma_Bd *BdCurPtr;
	int rc;
	int Index;

	/* Get all processed BDs from hardware */
	BdCount = XAxiDma_BdRingFromHw(TxRingPtr, XAXIDMA_ALL_BDS, &BdPtr);

	/* Handle the BDs */
	BdCurPtr = BdPtr;
	for (Index = 0; Index < BdCount; Index++) {

		/*
 		 * Check the status in each BD
 		 * If error happens, the DMA engine will be halted after this
 		 * BD processing stops.
 		 */
		BdSts = XAxiDma_BdGetSts(BdCurPtr);
		if ((BdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
		    (!(BdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
			ERROR_PRINT("\r\n");
			break;
		}

		/*
  		 * Here we don't need to do anything. But if a RTOS is being
  		 * used, we may need to free the packet buffer attached to
  		 * the processed BD
  		 */

		/* Find the next processed BD */
		BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(TxRingPtr, BdCurPtr);
	}

	/* Free all processed BDs for future transmission */
	rc = XAxiDma_BdRingFree(TxRingPtr, BdCount, BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("\r\n");
	}

	_tx_cb();
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA TX Interrupt handler function.
 * *
 * * It gets the interrupt status from the hardware, acknowledges it, and if any
 * * error happens, it resets the hardware. Otherwise, if a completion interrupt
 * * presents, then it calls the callback function.
 * *
 * * @param	Callback is a pointer to TX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void TxIntrHandler(void *Callback)
{
	XAxiDma_BdRing *TxRingPtr = (XAxiDma_BdRing *) Callback;
	u32 IrqStatus;
	int TimeOut;

	/* Read pending interrupts */
	IrqStatus = XAxiDma_BdRingGetIrq(TxRingPtr);

	/* Acknowledge pending interrupts */
	XAxiDma_BdRingAckIrq(TxRingPtr, IrqStatus);

	/* If no interrupt is asserted, we do not do anything */
	if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK)) {
		return;
	}

	/*
  	 * If error interrupt is asserted, raise error flag, reset the
  	 * hardware to recover from the error, and return with no further
  	 * processing.
  	 */
	if ((IrqStatus & XAXIDMA_IRQ_ERROR_MASK)) {

		XAxiDma_BdRingDumpRegs(TxRingPtr);
		ERROR_PRINT("XAXIDMA_IRQ_ERROR_MASK\r\n");

		/*
 		 * Reset should never fail for transmit channel
 		 */
		XAxiDma_Reset(&AxiDma);

		TimeOut = RESET_TIMEOUT_COUNTER;
		while (TimeOut) {
			if (XAxiDma_ResetIsDone(&AxiDma)) {
				break;
			}
			TimeOut -= 1;
		}
		return;
	}

	/*
  	 * If Transmit done interrupt is asserted, call TX call back function
  	 * to handle the processed BDs and raise the according flag
  	 */
	if ((IrqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		tx_irq_bd_handler(TxRingPtr);
	}
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA RX callback function called by the RX interrupt handler.
 * * This function handles finished BDs by hardware, attaches new buffers to those
 * * BDs, and give them back to hardware to receive more incoming packets
 * *
 * * @param	RxRingPtr is a pointer to RX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void rx_irq_bd_handler(XAxiDma_BdRing * RxRingPtr)
{
	int BdCount;
	XAxiDma_Bd *BdPtr;
	XAxiDma_Bd *BdCurPtr;
	u32 BdSts;
	int Index;
	u32 FreeBdCount;
	int rc;

	/* Get finished BDs from hardware */
	BdCount = XAxiDma_BdRingFromHw(RxRingPtr, XAXIDMA_ALL_BDS, &BdPtr);
	if(BdCount == 0){
		return;
	}


	BdCurPtr = BdPtr;
	for (Index = 0; Index < BdCount; Index++) {
		
		/*
 		* Check the flags set by the hardware for status
		* If error happens, processing stops, because the DMA engine
 		* is halted after this BD.
		*/
		BdSts = XAxiDma_BdGetSts(BdCurPtr);
		if ((BdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
		    (!(BdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
			ERROR_PRINT("XAxiDma_BdGetSts\r\n");
			break;
		}

		/* get memory offset of current bd */
		uint32_t addr   = XAxiDma_BdGetBufAddr(BdCurPtr);
		uint32_t pktLen = XAxiDma_BdGetActualLength(BdCurPtr,params.bd_buf_size);
//		printf("-- %s: (BdCount %d) rx packet %04d @ 0x%x len %lu\r\n",__func__,BdCount,RxDone,(unsigned int)addr,pktLen);
		// XAxiDma_DumpBd(BdCurPtr);

		_rx_cb(addr, pktLen);

		#if 0
		/* check data */
		uint8_t *rxPacket = (u8 *)addr;
		int j = 0;
		for(j = 0; j < params.bd_buf_size; j++){
			if(*(rxPacket+j) != txPkt[j]){
				ERROR_PRINT("[%04d]: tx %03d, rx %03d @ 0x%x; RxDone=%04d\r\n",j,txPkt[j],*(rxPacket+j),(rxPacket+j),RxDone);
				// XAxiDma_BdRingDumpRegs(RxRingPtr);
				XAxiDma_DumpBd(BdCurPtr);
				break;
			}
		}


		/* clear bds memory area to verify this isn't all an illusion... */
		memset((void *)addr, 0, params.bd_buf_size);
		#endif
		
		/* Find the next processed BD */
		if(Index != BdCount-1)
			BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(RxRingPtr, BdCurPtr);
	}

	
	rc = XAxiDma_BdRingFree(RxRingPtr, BdCount, BdPtr);
	if(rc != XST_SUCCESS)
		ERROR_PRINT("XAxiDma_BdRingFree rc %d\r\n",rc);

	/* Return processed BDs to RX channel so we are ready to receive new
	 * packets:
	 *    - Allocate all free RX BDs
	 *    - Pass the BDs to RX channel
	 */
	FreeBdCount = XAxiDma_BdRingGetFreeCnt(RxRingPtr);
	rc = XAxiDma_BdRingAlloc(RxRingPtr, FreeBdCount, &BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("XAxiDma_BdRingAlloc failed %d - FreeBdCount %u\r\n",rc,FreeBdCount);
	}

	rc = XAxiDma_BdRingToHw(RxRingPtr, FreeBdCount, BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("XAxiDma_BdRingToHw failed %d\r\n",rc);
	}
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA RX interrupt handler function
 * *
 * * It gets the interrupt status from the hardware, acknowledges it, and if any
 * * error happens, it resets the hardware. Otherwise, if a completion interrupt
 * * presents, then it calls the callback function.
 * *
 * * @param	Callback is a pointer to RX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void RxIntrHandler(void *Callback)
{
	XAxiDma_BdRing *RxRingPtr = (XAxiDma_BdRing *) Callback;
	u32 IrqStatus;
	int TimeOut;

	/* Read pending interrupts */
	IrqStatus = XAxiDma_BdRingGetIrq(RxRingPtr);

	/* Acknowledge pending interrupts */
	XAxiDma_BdRingAckIrq(RxRingPtr, IrqStatus);

	/*
  	 * If no interrupt is asserted, we do not do anything
  	 */
	if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK)) {
		return;
	}

	/*
 	 * If error interrupt is asserted, raise error flag, reset the
  	 * hardware to recover from the error, and return with no further
  	 * processing.
  	 */
	if ((IrqStatus & XAXIDMA_IRQ_ERROR_MASK)) {

		XAxiDma_BdRingDumpRegs(RxRingPtr);
		ERROR_PRINT("\r\n");

		/* Reset could fail and hang
  		 * NEED a way to handle this or do not call it??
  		 */
		XAxiDma_Reset(&AxiDma);

		TimeOut = RESET_TIMEOUT_COUNTER;

		while (TimeOut) {
			if(XAxiDma_ResetIsDone(&AxiDma)) {
				break;
			}

			TimeOut -= 1;
		}

		return;
	}

	/*
   	 * If completion interrupt is asserted, call RX call back function
  	 * to handle the processed BDs and then raise the according flag.
  	 */
	if ((IrqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		rx_irq_bd_handler(RxRingPtr);
	}
}

/*****************************************************************************/
/*
 * *
 * * This function setups the interrupt system so interrupts can occur for the
 * * DMA, it assumes XScuGic component exists in the hardware system.
 * *
 * * @param	IntcInstancePtr is a pointer to the instance of the XScuGic.
 * * @param	AxiDmaPtr is a pointer to the instance of the DMA engine
 * * @param	TxIntrId is the TX channel Interrupt ID.
 * * @param	RxIntrId is the RX channel Interrupt ID.
 * *
 * * @return
 * *		- XST_SUCCESS if successful,
 * *		- XST_FAILURE.if not succesful
 * *
 * * @note		None.
 * *
 * ******************************************************************************/

static int SetupIntrSystem(XScuGic * IntcInstancePtr,
			   XAxiDma * AxiDmaPtr, u16 TxIntrId, u16 RxIntrId)
{
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(&AxiDma);
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDma);
	int rc;

	XScuGic_Config *IntcConfig;


	/*
  	 * Initialize the interrupt controller driver so that it is ready to
  	 * use.
  	 */
	IntcConfig = XScuGic_LookupConfig(XScuGic_DEVICE_ID);
	if (NULL == IntcConfig) {
		return XST_FAILURE;
	}

	rc = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
					IntcConfig->CpuBaseAddress);
	if (rc != XST_SUCCESS) {
		return XST_FAILURE;
	}


	XScuGic_SetPriorityTriggerType(IntcInstancePtr, TxIntrId, 0xA0, 0x3);

	XScuGic_SetPriorityTriggerType(IntcInstancePtr, RxIntrId, 0xA0, 0x3);
	/*
  	 * Connect the device driver handler that will be called when an
  	 * interrupt for the device occurs, the handler defined above performs
  	 * the specific interrupt processing for the device.
  	 */
	rc = XScuGic_Connect(IntcInstancePtr, TxIntrId,
				(Xil_InterruptHandler)TxIntrHandler,
				TxRingPtr);
	if (rc != XST_SUCCESS) {
		return rc;
	}

	rc = XScuGic_Connect(IntcInstancePtr, RxIntrId,
				(Xil_InterruptHandler)RxIntrHandler,
				RxRingPtr);
	if (rc != XST_SUCCESS) {
		return rc;
	}

	XScuGic_Enable(IntcInstancePtr, TxIntrId);
	XScuGic_Enable(IntcInstancePtr, RxIntrId);

	/* Enable interrupts from the hardware */
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)XScuGic_HANDLER,
			(void *)IntcInstancePtr);

	Xil_ExceptionEnable();

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
 * *
 * * This function disables the interrupts for DMA engine.
 * *
 * * @param	IntcInstancePtr is the pointer to the XScuGic component instance
 * * @param	TxIntrId is interrupt ID associated w/ DMA TX channel
 * * @param	RxIntrId is interrupt ID associated w/ DMA RX channel
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void DisableIntrSystem(XScuGic * IntcInstancePtr,
					u16 TxIntrId, u16 RxIntrId)
{
	XScuGic_Disconnect(IntcInstancePtr, TxIntrId);
	XScuGic_Disconnect(IntcInstancePtr, RxIntrId);
}

/*****************************************************************************/
/*
 * *
 * * This function sets up RX channel of the DMA engine to be ready for packet
 * * reception
 * *
 * * @param	AxiDmaInstPtr is the pointer to the instance of the DMA engine.
 * *
 * * @return	- XST_SUCCESS if the setup is successful.
 * *		- XST_FAILURE if fails.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static int RxSetup(XAxiDma * AxiDmaInstPtr)
{
	XAxiDma_BdRing *RxRingPtr;
	int rc;
	XAxiDma_Bd BdTemplate;
	XAxiDma_Bd *BdPtr;
	XAxiDma_Bd *BdCurPtr;
	int BdCount;
	int FreeBdCount;
	u32 RxBufferPtr;
	int Index;

	RxRingPtr = XAxiDma_GetRxRing(AxiDmaInstPtr);

	/* Disable all RX interrupts before RxBD space setup */
	XAxiDma_BdRingIntDisable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Setup Rx BD space */
	BdCount = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
				params.rx_bd_space_high - params.rx_bd_space_base + 1);

	rc = XAxiDma_BdRingCreate(RxRingPtr, params.rx_bd_space_base,
					params.rx_bd_space_base,
					XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx bd create failed with %d\r\n", rc);
		return XST_FAILURE;
	}
	printf("%d rx bds created\r\n",BdCount);

	/*
  	 * Setup a BD template for the Rx channel. Then copy it to every RX BD.
  	 */
	XAxiDma_BdClear(&BdTemplate);
	rc = XAxiDma_BdRingClone(RxRingPtr, &BdTemplate);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx bd clone failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	/* Attach buffers to RxBD ring so we are ready to receive packets */
	FreeBdCount = XAxiDma_BdRingGetFreeCnt(RxRingPtr);

	rc = XAxiDma_BdRingAlloc(RxRingPtr, FreeBdCount, &BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx bd alloc failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	BdCurPtr = BdPtr;
	RxBufferPtr = params.rx_buffer_base;

	for (Index = 0; Index < FreeBdCount; Index++) {

		rc = XAxiDma_BdSetBufAddr(BdCurPtr, RxBufferPtr);
		if (rc != XST_SUCCESS) {
			ERROR_PRINT("Rx set buffer addr %x on BD %x failed %d\r\n",
			(unsigned int)RxBufferPtr,
			(UINTPTR)BdCurPtr, rc);

			return XST_FAILURE;
		}

		rc = XAxiDma_BdSetLength(BdCurPtr, params.bd_buf_size,
					RxRingPtr->MaxTransferLen);
		if (rc != XST_SUCCESS) {
			ERROR_PRINT("Rx set length %d on BD %x failed %d\r\n",
			    params.bd_buf_size, (UINTPTR)BdCurPtr, rc);

			return XST_FAILURE;
		}

		/* Receive BDs do not need to set anything for the control
  		 * The hardware will set the SOF/EOF bits per stream status
  		 */
		XAxiDma_BdSetCtrl(BdCurPtr, 0);

		XAxiDma_BdSetId(BdCurPtr, RxBufferPtr);

		RxBufferPtr += params.bd_buf_size;
		BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(RxRingPtr, BdCurPtr);
	}

	rc = XAxiDma_BdRingSetCoalesce(RxRingPtr, params.coalesce_count,
			DELAY_TIMER_COUNT);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx set coalesce failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingToHw(RxRingPtr, FreeBdCount, BdPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx ToHw failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingCheck(RxRingPtr);
	if(rc != XST_SUCCESS){
		ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",rc);
		return XST_FAILURE;
	}

	/* Enable all RX interrupts */
	XAxiDma_BdRingIntEnable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Enable Cyclic DMA mode */
	// XAxiDma_BdRingEnableCyclicDMA(RxRingPtr);
	// XAxiDma_SelectCyclicMode(AxiDmaInstPtr, XAXIDMA_DEVICE_TO_DMA, 1);

	/* Start RX DMA channel */
	rc = XAxiDma_BdRingStart(RxRingPtr);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Rx start BD ring failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

/*****************************************************************************/
/*
 * *
 * * This function sets up the TX channel of a DMA engine to be ready for packet
 * * transmission.
 * *
 * * @param	AxiDmaInstPtr is the pointer to the instance of the DMA engine.
 * *
 * * @return	- XST_SUCCESS if the setup is successful.
 * *		- XST_FAILURE otherwise.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static int TxSetup(XAxiDma * AxiDmaInstPtr)
{
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(AxiDmaInstPtr);
	XAxiDma_Bd BdTemplate;
	int rc;
	u32 BdCount;

	/* Disable all TX interrupts before TxBD space setup */
	XAxiDma_BdRingIntDisable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Setup TxBD space  */
	BdCount = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
			(u32)params.tx_bd_space_high - (u32)params.tx_bd_space_base + 1);
	DEBUG_PRINT("%d tx bds created\r\n",(int)BdCount);

	rc = XAxiDma_BdRingCreate(TxRingPtr, params.tx_bd_space_base,
				     params.tx_bd_space_base,
				     XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);
	if (rc != XST_SUCCESS) {

		ERROR_PRINT("Failed create BD ring\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Like the RxBD space, we create a template and set all BDs to be the
  	 * same as the template. The sender has to set up the BDs as needed.
	 */
	XAxiDma_BdClear(&BdTemplate);
	rc = XAxiDma_BdRingClone(TxRingPtr, &BdTemplate);
	if (rc != XST_SUCCESS) {

		ERROR_PRINT("Failed clone BDs\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Set the coalescing threshold, so only one transmit interrupt
  	 * occurs for this example
  	 *
  	 * If you would like to have multiple interrupts to happen, change
  	 * the params.coalesce_count to be a smaller value
  	 */
	rc = XAxiDma_BdRingSetCoalesce(TxRingPtr, params.coalesce_count,
			DELAY_TIMER_COUNT);
	if (rc != XST_SUCCESS) {
		ERROR_PRINT("Failed set coalescing"
		" %d/%d\r\n",params.coalesce_count, DELAY_TIMER_COUNT);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingCheck(TxRingPtr);
	if(rc != XST_SUCCESS){
		ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",rc);
		return XST_FAILURE;
	}

	/* Enable all TX interrupts */
	XAxiDma_BdRingIntEnable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Start the TX channel */
	rc = XAxiDma_BdRingStart(TxRingPtr);
	if (rc != XST_SUCCESS) {

		ERROR_PRINT("Failed bd start\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}
