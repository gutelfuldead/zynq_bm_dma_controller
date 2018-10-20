/**
 * @brief  Provides API for abstracting control for handling bare-metal
 *         AXI-Stream DMA Engine configured in Scatter Gather (SG) Mode
 *         with Dynamic ReAlignment Engine (DRE) enabled  
 * @author Jason Gutel (github.com/gutelfuldead)
 */

#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_mmu.h"
#include "stdio.h"
#include "axis_dma_controller.h"

/******************** Constant Definitions **********************************/
/* Size of One MB as used by the TLB when marking mem area as noncacheable */
#define ONE_MB 0x100000U

/* Timeout loop counter for reset */
#define RESET_TIMEOUT_COUNTER 10000
#define DELAY_TIMER_COUNT 100

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/
#ifdef AXISDMA_ENABLE_PRINTS
	#define AXISDMA_ERROR_PRINT(fmt, args...) printf("ERROR: %s:%d(): " fmt, \
    		__func__,__LINE__, ##args)
	#define AXISDMA_DEBUG_PRINT(fmt, args...) printf("DEBUG: %s:%d(): " fmt, \
	    __func__,__LINE__, ##args)
#else
	#define AXISDMA_ERROR_PRINT(fmt, args...)
	#define AXISDMA_DEBUG_PRINT(fmt, args...) 
#endif

/************************** Function Prototypes ******************************/
static void axisDmaCtrl_txIntrHandler(void *callback);
static void axisDmaCtrl_rxIntrHandler(void *callback);
static int axisDmaCtrl_setupIntrSystem(XScuGic * intcInstancePtr, XAxiDma * axiDmaPtr, u16 txIntrId, u16 rxIntrId);
static void axisDmaCtrl_disableIntrSystem(XScuGic * intcInstancePtr, u16 txIntrId, u16 rxIntrId);
static int axisDmaCtrl_rxSetup(XAxiDma * axiDmaInstPtr);
static int axisDmaCtrl_txSetup(XAxiDma * axiDmaInstPtr);
static void axisDmaCtrl_txIrqBdHandler(XAxiDma_BdRing * txRingPtr);
static void axisDmaCtrl_rxIrqBdHandler(XAxiDma_BdRing * rxRingPtr);
static int axisDmaCtrl_markMemNoncache(struct axisDmaCtrl_params *mem);
static struct axisDmaCtrl_params axisDmaCtrl_copyParamsStruct(struct axisDmaCtrl_params * in);
static void axisDmaCtrl_emptyParamsStruct(struct axisDmaCtrl_params * in);

/************************** Variable Definitions *****************************/
static XAxiDma axiDma;

static tx_cb_t _tx_cb = NULL;
static rx_cb_t _rx_cb = NULL;
static struct axisDmaCtrl_params params;


int axisDmaCtrl_init(struct axisDmaCtrl_params *paramsIn, 
	XScuGic * intcInstancePtr,
	rx_cb_t rxCb,
	tx_cb_t txCb)
{
	int rc;
	XAxiDma_Config *config;

	rc = axisDmaCtrl_register_tx_cb(txCb);
	if(rc == XST_FAILURE){
		AXISDMA_ERROR_PRINT("axisDmaCtrl_register_tx_cb failed!\r\n");
		return rc;
	}

	axisDmaCtrl_register_rx_cb(rxCb);
	if(rc == XST_FAILURE){
		AXISDMA_ERROR_PRINT("axisDmaCtrl_register_rx_cb failed!\r\n");
		return rc;
	}

	params = axisDmaCtrl_copyParamsStruct(paramsIn);

	rc = axisDmaCtrl_markMemNoncache(paramsIn);
	if (rc) {
		AXISDMA_ERROR_PRINT("axisDmaCtrl_markMemNoncache failed!\r\n");
		return XST_FAILURE;
	}

	config = XAxiDma_LookupConfig(params.axisDma_dmaDevId);
	rc = XAxiDma_CfgInitialize(&axiDma, config);
	if(rc){
		AXISDMA_ERROR_PRINT("XAxiDma_CfgInitialize failed!\r\n");
		return XST_FAILURE;
	}

	if(!XAxiDma_HasSg(&axiDma)) {
		AXISDMA_ERROR_PRINT("DMA NOT in SG Mode...\r\n");
		return XST_FAILURE;
	}

	/* Set up TX/RX channels to be ready to transmit and receive packets */
	rc = axisDmaCtrl_txSetup(&axiDma);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed TX setup\r\n");
		return XST_FAILURE;
	}

	rc = axisDmaCtrl_rxSetup(&axiDma);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed RX setup\r\n");
		return XST_FAILURE;
	}

	/* Set up Interrupt system  */
	rc = axisDmaCtrl_setupIntrSystem(intcInstancePtr, &axiDma, params.axisDma_txIrqId, params.axisDma_rxIrqId);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed interrupt setup\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

void axisDmaCtrl_printParams(struct axisDmaCtrl_params *in)
{
	printf("rx_bd_space_base : 0x%x\r\n",(unsigned int)in->rx_bd_space_base);
	printf("rx_bd_space_high : 0x%x\r\n",(unsigned int)in->rx_bd_space_high);
	printf("tx_bd_space_base : 0x%x\r\n",(unsigned int)in->tx_bd_space_base);
	printf("tx_bd_space_high : 0x%x\r\n",(unsigned int)in->tx_bd_space_high);
	printf("tx_buffer_base   : 0x%x\r\n",(unsigned int)in->tx_buffer_base);
	printf("tx_buffer_high   : 0x%x\r\n",(unsigned int)in->tx_buffer_high);
	printf("rx_buffer_base   : 0x%x\r\n",(unsigned int)in->rx_buffer_base);
	printf("rx_buffer_high   : 0x%x\r\n",(unsigned int)in->rx_buffer_high);
	printf("bd_buf_size      : 0x%x\r\n",(unsigned int)in->bd_buf_size);
	printf("coalesce_count   : %u\r\n",in->coalesce_count);
}

void axisDmaCtrl_disable(XScuGic * intcInstancePtr)
{
	axisDmaCtrl_disableIntrSystem(intcInstancePtr, params.axisDma_txIrqId, params.axisDma_rxIrqId);
	_tx_cb = NULL;
	_rx_cb = NULL;
	axisDmaCtrl_emptyParamsStruct(&params);
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
	XAxiDma_BdRing *txRingPtr = XAxiDma_GetTxRing(&axiDma);
	XAxiDma_Bd *bdPtr, *bdCurPtr;
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

	rc = XAxiDma_BdRingAlloc(txRingPtr, reqBds, &bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed bd alloc\r\n");
		return E_AXISDMA_NOBDS;
	}

	BufferAddr = (UINTPTR)packetBuf;
	bdCurPtr = bdPtr;

	for(i = 0; i < reqBds; i++){
		u32 CrBits = 0;

		rc = XAxiDma_BdSetBufAddr(bdPtr, BufferAddr);
		if (rc != XST_SUCCESS) {
			AXISDMA_ERROR_PRINT("Tx set buffer addr %x on BD %x failed %d\r\n",
			(unsigned int)BufferAddr,
			(UINTPTR)bdPtr, rc);
			return XST_FAILURE;
		}

		rc = XAxiDma_BdSetLength(bdPtr, params.bd_buf_size,
					txRingPtr->MaxTransferLen);
		if (rc != XST_SUCCESS) {
			AXISDMA_ERROR_PRINT("Tx set length %d on BD %x failed %d\r\n",
			params.bd_buf_size, (UINTPTR)bdPtr, rc);
			return XST_FAILURE;
		}

		if(i == 0)
			CrBits |= XAXIDMA_BD_CTRL_TXSOF_MASK;
		if(i == reqBds - 1)
			CrBits |= XAXIDMA_BD_CTRL_TXEOF_MASK;

		XAxiDma_BdSetCtrl(bdPtr, CrBits);
		XAxiDma_BdSetId(bdPtr, BufferAddr);

		BufferAddr += params.bd_buf_size;
		bdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(txRingPtr, bdCurPtr);
	}

	/* Give the BD to hardware */
	rc = XAxiDma_BdRingToHw(txRingPtr, reqBds, bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed to hw, length %d\r\n",
			(int)XAxiDma_BdGetLength(bdPtr,
					txRingPtr->MaxTransferLen));
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

static int axisDmaCtrl_markMemNoncache(struct axisDmaCtrl_params *mem)
{
	size_t i = 0;
	int invalid_struct = 0;
	invalid_struct |= mem->rx_bd_space_base    == 0
		       || mem->rx_bd_space_high    == 0
		       || mem->tx_bd_space_base    == 0
		       || mem->tx_bd_space_high    == 0
		       || mem->tx_buffer_base      == 0
		       || mem->tx_buffer_high      == 0
		       || mem->rx_buffer_base      == 0
		       || mem->rx_buffer_high      == 0
		       || mem->bd_buf_size         == 0
		       || (int)mem->coalesce_count == 0;
	if(invalid_struct)
		return XST_FAILURE;
	for(i = mem->rx_bd_space_base; i <= mem->rx_bd_space_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->tx_bd_space_base; i <= mem->tx_bd_space_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->tx_buffer_base; i <= mem->tx_buffer_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	for(i = mem->rx_buffer_base; i <= mem->rx_buffer_high; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);
	return XST_SUCCESS;
}

static struct axisDmaCtrl_params axisDmaCtrl_copyParamsStruct(struct axisDmaCtrl_params * in)
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
	tmp.axisDma_txIrqPriority = in->axisDma_txIrqPriority;
	tmp.axisDma_rxIrqPriority = in->axisDma_rxIrqPriority;
	tmp.axisDma_rxIrqId = in->axisDma_rxIrqId;
	tmp.axisDma_txIrqId = in->axisDma_txIrqId;
	tmp.axisDma_dmaDevId = in->axisDma_dmaDevId;
	return tmp;
}

static void axisDmaCtrl_emptyParamsStruct(struct axisDmaCtrl_params * in)
{
	in->rx_bd_space_base = 0;
	in->rx_bd_space_high = 0;
	in->tx_bd_space_base = 0;
	in->tx_bd_space_high = 0;
	in->tx_buffer_base   = 0;
	in->tx_buffer_high   = 0;
	in->rx_buffer_base   = 0;
	in->rx_buffer_high   = 0;
	in->bd_buf_size      = 0;	
	in->coalesce_count   = 1;	
	in->axisDma_rxIrqPriority = 0xff;
	in->axisDma_txIrqPriority = 0xff;
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA TX callback function to be called by TX interrupt handler.
 * * This function handles BDs finished by hardware.
 * *
 * * @param	txRingPtr is a pointer to TX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void axisDmaCtrl_txIrqBdHandler(XAxiDma_BdRing * txRingPtr)
{
	int bdCount;
	u32 bdSts;
	XAxiDma_Bd *bdPtr;
	XAxiDma_Bd *bdCurPtr;
	int rc;
	int i;

	/* Get all processed BDs from hardware */
	bdCount = XAxiDma_BdRingFromHw(txRingPtr, XAXIDMA_ALL_BDS, &bdPtr);

	/* Handle the BDs */
	bdCurPtr = bdPtr;
	for (i = 0; i < bdCount; i++) {

		/*
 		 * Check the status in each BD
 		 * If error happens, the DMA engine will be halted after this
 		 * BD processing stops.
 		 */
		bdSts = XAxiDma_BdGetSts(bdCurPtr);
		if ((bdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
		    (!(bdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
			AXISDMA_ERROR_PRINT("\r\n");
			break;
		}

		/*
  		 * Here we don't need to do anything. But if a RTOS is being
  		 * used, we may need to free the packet buffer attached to
  		 * the processed BD
  		 */

		/* Find the next processed BD */
		bdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(txRingPtr, bdCurPtr);
	}

	/* Free all processed BDs for future transmission */
	rc = XAxiDma_BdRingFree(txRingPtr, bdCount, bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("XAxiDma_BdRingFree\r\n");
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
 * * @param	callback is a pointer to TX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void axisDmaCtrl_txIntrHandler(void *callback)
{
	XAxiDma_BdRing *txRingPtr = (XAxiDma_BdRing *) callback;
	u32 irqStatus;
	int timeOut;

	/* Read pending interrupts */
	irqStatus = XAxiDma_BdRingGetIrq(txRingPtr);

	/* Acknowledge pending interrupts */
	XAxiDma_BdRingAckIrq(txRingPtr, irqStatus);

	/* If no interrupt is asserted, we do not do anything */
	if (!(irqStatus & XAXIDMA_IRQ_ALL_MASK)) {
		return;
	}

	/*
  	 * If error interrupt is asserted, raise error flag, reset the
  	 * hardware to recover from the error, and return with no further
  	 * processing.
  	 */
	if ((irqStatus & XAXIDMA_IRQ_ERROR_MASK)) {

		XAxiDma_BdRingDumpRegs(txRingPtr);
		AXISDMA_ERROR_PRINT("XAXIDMA_IRQ_ERROR_MASK\r\n");

		/*
 		 * Reset should never fail for transmit channel
 		 */
		XAxiDma_Reset(&axiDma);

		/** @todo raise an error flag */
		timeOut = RESET_TIMEOUT_COUNTER;
		while (timeOut) {
			if (XAxiDma_ResetIsDone(&axiDma)) {
				break;
			}
			timeOut -= 1;
		}
		return;
	}

	/*
  	 * If Transmit done interrupt is asserted, call TX call back function
  	 * to handle the processed BDs and raise the according flag
  	 */
	if ((irqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		axisDmaCtrl_txIrqBdHandler(txRingPtr);
	}
}

/*****************************************************************************/
/*
 * *
 * * This is the DMA RX callback function called by the RX interrupt handler.
 * * This function handles finished BDs by hardware, attaches new buffers to those
 * * BDs, and give them back to hardware to receive more incoming packets
 * *
 * * @param	rxRingPtr is a pointer to RX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void axisDmaCtrl_rxIrqBdHandler(XAxiDma_BdRing * rxRingPtr)
{
	int bdCount;
	XAxiDma_Bd *bdPtr;
	XAxiDma_Bd *bdCurPtr;
	u32 bdSts;
	int i;
	u32 freeBdCount;
	int rc;

	/* Get finished BDs from hardware */
	bdCount = XAxiDma_BdRingFromHw(rxRingPtr, XAXIDMA_ALL_BDS, &bdPtr);
	if(bdCount == 0){
		return;
	}


	bdCurPtr = bdPtr;
	for (i = 0; i < bdCount; i++) {
		
		/*
 		* Check the flags set by the hardware for status
		* If error happens, processing stops, because the DMA engine
 		* is halted after this BD.
		*/
		bdSts = XAxiDma_BdGetSts(bdCurPtr);
		if ((bdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
		    (!(bdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
			AXISDMA_ERROR_PRINT("XAxiDma_BdGetSts\r\n");
			break;
		}

		/* get memory offset of current bd */
		uint32_t addr   = XAxiDma_BdGetBufAddr(bdCurPtr);
		uint32_t pktLen = XAxiDma_BdGetActualLength(bdCurPtr,params.bd_buf_size);
		// XAxiDma_DumpBd(bdCurPtr);

		_rx_cb(addr, pktLen);
		
		/* Find the next processed BD */
		if(i != bdCount-1)
			bdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(rxRingPtr, bdCurPtr);
	}

	
	rc = XAxiDma_BdRingFree(rxRingPtr, bdCount, bdPtr);
	if(rc != XST_SUCCESS)
		AXISDMA_ERROR_PRINT("XAxiDma_BdRingFree rc %d\r\n",rc);

	/* Return processed BDs to RX channel so we are ready to receive new
	 * packets:
	 *    - Allocate all free RX BDs
	 *    - Pass the BDs to RX channel
	 */
	freeBdCount = XAxiDma_BdRingGetFreeCnt(rxRingPtr);
	rc = XAxiDma_BdRingAlloc(rxRingPtr, freeBdCount, &bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("XAxiDma_BdRingAlloc failed %d - freeBdCount %u\r\n",rc,freeBdCount);
	}

	rc = XAxiDma_BdRingToHw(rxRingPtr, freeBdCount, bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("XAxiDma_BdRingToHw failed %d\r\n",rc);
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
 * * @param	callback is a pointer to RX channel of the DMA engine.
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void axisDmaCtrl_rxIntrHandler(void *callback)
{
	XAxiDma_BdRing *rxRingPtr = (XAxiDma_BdRing *) callback;
	u32 irqStatus;
	int timeOut;

	/* Read pending interrupts */
	irqStatus = XAxiDma_BdRingGetIrq(rxRingPtr);

	/* Acknowledge pending interrupts */
	XAxiDma_BdRingAckIrq(rxRingPtr, irqStatus);

	/*
  	 * If no interrupt is asserted, we do not do anything
  	 */
	if (!(irqStatus & XAXIDMA_IRQ_ALL_MASK)) {
		return;
	}

	/*
 	 * If error interrupt is asserted, raise error flag, reset the
  	 * hardware to recover from the error, and return with no further
  	 * processing.
  	 */
	if ((irqStatus & XAXIDMA_IRQ_ERROR_MASK)) {

		XAxiDma_BdRingDumpRegs(rxRingPtr);
		AXISDMA_ERROR_PRINT("\r\n");

		/* Reset could fail and hang
  		 * NEED a way to handle this or do not call it??
  		 */
		XAxiDma_Reset(&axiDma);

		timeOut = RESET_TIMEOUT_COUNTER;

		while (timeOut) {
			if(XAxiDma_ResetIsDone(&axiDma)) {
				break;
			}

			timeOut -= 1;
		}

		return;
	}

	/*
   	 * If completion interrupt is asserted, call RX call back function
  	 * to handle the processed BDs and then raise the according flag.
  	 */
	if ((irqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		axisDmaCtrl_rxIrqBdHandler(rxRingPtr);
	}
}

/*****************************************************************************/
/*
 * *
 * * This function setups the interrupt system so interrupts can occur for the
 * * DMA, it assumes XScuGic component exists in the hardware system.
 * *
 * * @param	intcInstancePtr is a pointer to the instance of the XScuGic.
 * * @param	axiDmaPtr is a pointer to the instance of the DMA engine
 * * @param	txIntrId is the TX channel Interrupt ID.
 * * @param	rxIntrId is the RX channel Interrupt ID.
 * *
 * * @return
 * *		- XST_SUCCESS if successful,
 * *		- XST_FAILURE.if not succesful
 * *
 * * @note		None.
 * *
 * ******************************************************************************/

static int axisDmaCtrl_setupIntrSystem(XScuGic * intcInstancePtr,
			   XAxiDma * axiDmaPtr, u16 txIntrId, u16 rxIntrId)
{
	XAxiDma_BdRing *txRingPtr = XAxiDma_GetTxRing(&axiDma);
	XAxiDma_BdRing *rxRingPtr = XAxiDma_GetRxRing(&axiDma);
	int rc;

	XScuGic_Config *intcConfig;


	/*
  	 * Initialize the interrupt controller driver so that it is ready to
  	 * use.
  	 */
	intcConfig = XScuGic_LookupConfig(params.axisDma_XscuGic_DevId);
	if (NULL == intcConfig) {
		return XST_FAILURE;
	}

	rc = XScuGic_CfgInitialize(intcInstancePtr, intcConfig,
					intcConfig->CpuBaseAddress);
	if (rc != XST_SUCCESS) {
		return XST_FAILURE;
	}


	XScuGic_SetPriorityTriggerType(intcInstancePtr, txIntrId, params.axisDma_txIrqPriority, 0x3);

	XScuGic_SetPriorityTriggerType(intcInstancePtr, rxIntrId, params.axisDma_rxIrqPriority, 0x3);
	/*
  	 * Connect the device driver handler that will be called when an
  	 * interrupt for the device occurs, the handler defined above performs
  	 * the specific interrupt processing for the device.
  	 */
	rc = XScuGic_Connect(intcInstancePtr, txIntrId,
				(Xil_InterruptHandler)axisDmaCtrl_txIntrHandler,
				txRingPtr);
	if (rc != XST_SUCCESS) {
		return rc;
	}

	rc = XScuGic_Connect(intcInstancePtr, rxIntrId,
				(Xil_InterruptHandler)axisDmaCtrl_rxIntrHandler,
				rxRingPtr);
	if (rc != XST_SUCCESS) {
		return rc;
	}

	XScuGic_Enable(intcInstancePtr, txIntrId);
	XScuGic_Enable(intcInstancePtr, rxIntrId);

	/* Enable interrupts from the hardware */
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)XScuGic_InterruptHandler,
			(void *)intcInstancePtr);

	Xil_ExceptionEnable();

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
 * *
 * * This function disables the interrupts for DMA engine.
 * *
 * * @param	intcInstancePtr is the pointer to the XScuGic component instance
 * * @param	txIntrId is interrupt ID associated w/ DMA TX channel
 * * @param	rxIntrId is interrupt ID associated w/ DMA RX channel
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void axisDmaCtrl_disableIntrSystem(XScuGic * intcInstancePtr,
					u16 txIntrId, u16 rxIntrId)
{
	XScuGic_Disconnect(intcInstancePtr, txIntrId);
	XScuGic_Disconnect(intcInstancePtr, rxIntrId);
}

/*****************************************************************************/
/*
 * *
 * * This function sets up RX channel of the DMA engine to be ready for packet
 * * reception
 * *
 * * @param	axiDmaInstPtr is the pointer to the instance of the DMA engine.
 * *
 * * @return	- XST_SUCCESS if the setup is successful.
 * *		- XST_FAILURE if fails.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static int axisDmaCtrl_rxSetup(XAxiDma * axiDmaInstPtr)
{
	XAxiDma_BdRing *rxRingPtr;
	int rc;
	XAxiDma_Bd bdTemplate;
	XAxiDma_Bd *bdPtr;
	XAxiDma_Bd *bdCurPtr;
	int bdCount;
	int freeBdCount;
	u32 rxBufferPtr;
	int i;

	rxRingPtr = XAxiDma_GetRxRing(axiDmaInstPtr);

	/* Disable all RX interrupts before RxBD space setup */
	XAxiDma_BdRingIntDisable(rxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Setup Rx BD space */
	bdCount = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
				params.rx_bd_space_high - params.rx_bd_space_base + 1);

	rc = XAxiDma_BdRingCreate(rxRingPtr, params.rx_bd_space_base,
					params.rx_bd_space_base,
					XAXIDMA_BD_MINIMUM_ALIGNMENT, bdCount);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx bd create failed with %d\r\n", rc);
		return XST_FAILURE;
	}
	AXISDMA_DEBUG_PRINT("%d rx bds created\r\n",bdCount);

	/*
  	 * Setup a BD template for the Rx channel. Then copy it to every RX BD.
  	 */
	XAxiDma_BdClear(&bdTemplate);
	rc = XAxiDma_BdRingClone(rxRingPtr, &bdTemplate);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx bd clone failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	/* Attach buffers to RxBD ring so we are ready to receive packets */
	freeBdCount = XAxiDma_BdRingGetFreeCnt(rxRingPtr);

	rc = XAxiDma_BdRingAlloc(rxRingPtr, freeBdCount, &bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx bd alloc failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	bdCurPtr = bdPtr;
	rxBufferPtr = params.rx_buffer_base;

	for (i = 0; i < freeBdCount; i++) {

		rc = XAxiDma_BdSetBufAddr(bdCurPtr, rxBufferPtr);
		if (rc != XST_SUCCESS) {
			AXISDMA_ERROR_PRINT("Rx set buffer addr %x on BD %x failed %d\r\n",
			(unsigned int)rxBufferPtr,
			(UINTPTR)bdCurPtr, rc);

			return XST_FAILURE;
		}

		rc = XAxiDma_BdSetLength(bdCurPtr, params.bd_buf_size,
					rxRingPtr->MaxTransferLen);
		if (rc != XST_SUCCESS) {
			AXISDMA_ERROR_PRINT("Rx set length %d on BD %x failed %d\r\n",
			    params.bd_buf_size, (UINTPTR)bdCurPtr, rc);

			return XST_FAILURE;
		}

		/* Receive BDs do not need to set anything for the control
  		 * The hardware will set the SOF/EOF bits per stream status
  		 */
		XAxiDma_BdSetCtrl(bdCurPtr, 0);

		XAxiDma_BdSetId(bdCurPtr, rxBufferPtr);

		rxBufferPtr += params.bd_buf_size;
		bdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(rxRingPtr, bdCurPtr);
	}

	rc = XAxiDma_BdRingSetCoalesce(rxRingPtr, params.coalesce_count,
			DELAY_TIMER_COUNT);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx set coalesce failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingToHw(rxRingPtr, freeBdCount, bdPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx ToHw failed with %d\r\n", rc);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingCheck(rxRingPtr);
	if(rc != XST_SUCCESS){
		AXISDMA_ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",rc);
		return XST_FAILURE;
	}

	/* Enable all RX interrupts */
	XAxiDma_BdRingIntEnable(rxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Enable Cyclic DMA mode */
	// XAxiDma_BdRingEnableCyclicDMA(rxRingPtr);
	// XAxiDma_SelectCyclicMode(axiDmaInstPtr, XAXIDMA_DEVICE_TO_DMA, 1);

	/* Start RX DMA channel */
	rc = XAxiDma_BdRingStart(rxRingPtr);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Rx start BD ring failed with %d\r\n", rc);
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
 * * @param	axiDmaInstPtr is the pointer to the instance of the DMA engine.
 * *
 * * @return	- XST_SUCCESS if the setup is successful.
 * *		- XST_FAILURE otherwise.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static int axisDmaCtrl_txSetup(XAxiDma * axiDmaInstPtr)
{
	XAxiDma_BdRing *txRingPtr = XAxiDma_GetTxRing(axiDmaInstPtr);
	XAxiDma_Bd bdTemplate;
	int rc;
	u32 bdCount;

	/* Disable all TX interrupts before TxBD space setup */
	XAxiDma_BdRingIntDisable(txRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Setup TxBD space  */
	bdCount = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
			(u32)params.tx_bd_space_high - (u32)params.tx_bd_space_base + 1);
	AXISDMA_DEBUG_PRINT("%d tx bds created\r\n",(int)bdCount);

	rc = XAxiDma_BdRingCreate(txRingPtr, params.tx_bd_space_base,
				     params.tx_bd_space_base,
				     XAXIDMA_BD_MINIMUM_ALIGNMENT, bdCount);
	if (rc != XST_SUCCESS) {

		AXISDMA_ERROR_PRINT("Failed create BD ring\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Like the RxBD space, we create a template and set all BDs to be the
  	 * same as the template. The sender has to set up the BDs as needed.
	 */
	XAxiDma_BdClear(&bdTemplate);
	rc = XAxiDma_BdRingClone(txRingPtr, &bdTemplate);
	if (rc != XST_SUCCESS) {

		AXISDMA_ERROR_PRINT("Failed clone BDs\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Set the coalescing threshold, so only one transmit interrupt
  	 * occurs for this example
  	 *
  	 * If you would like to have multiple interrupts to happen, change
  	 * the params.coalesce_count to be a smaller value
  	 */
	rc = XAxiDma_BdRingSetCoalesce(txRingPtr, params.coalesce_count,
			DELAY_TIMER_COUNT);
	if (rc != XST_SUCCESS) {
		AXISDMA_ERROR_PRINT("Failed set coalescing"
		" %d/%d\r\n",params.coalesce_count, DELAY_TIMER_COUNT);
		return XST_FAILURE;
	}

	rc = XAxiDma_BdRingCheck(txRingPtr);
	if(rc != XST_SUCCESS){
		AXISDMA_ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",rc);
		return XST_FAILURE;
	}

	/* Enable all TX interrupts */
	XAxiDma_BdRingIntEnable(txRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Start the TX channel */
	rc = XAxiDma_BdRingStart(txRingPtr);
	if (rc != XST_SUCCESS) {

		AXISDMA_ERROR_PRINT("Failed bd start\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}
