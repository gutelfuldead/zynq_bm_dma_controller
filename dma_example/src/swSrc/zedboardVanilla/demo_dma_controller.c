/**
  * author jason gutel
  */

/** @todo
 * 1. Set up ring buffer to dump tx packets into
 * 2. Timer interrupt at provided rate
 * 3. In interrupt take element from RB and send over tx chain
 */ 

#include "xaxidma.h"
#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_mmu.h"
#include "stdio.h"

#define ERROR_PRINT(fmt, args...) printf("ERROR  : %d:%s(): " fmt, \
    __LINE__, __func__, ##args)

#include "xscugic.h"

/******************** Constant Definitions **********************************/
/*
 *  * Device hardware build related constants.
 *   */

#define DMA_DEV_ID		XPAR_AXIDMA_0_DEVICE_ID

#define INTC_DEVICE_ID  XPAR_SCUGIC_SINGLE_DEVICE_ID
#define RX_INTR_ID		XPAR_FABRIC_AXIDMA_0_S2MM_INTROUT_VEC_ID
#define TX_INTR_ID		XPAR_FABRIC_AXIDMA_0_MM2S_INTROUT_VEC_ID

/* THE MEMORY SECTION NEEDS TO BE MARKED NON-CACHEABLE BY THE TLB */
#define MEM_BASE_ADDR		(XPAR_PS7_DDR_0_S_AXI_BASEADDR + 0x1000000) /* 0x00100000 - 0x001fffff */
#define RX_BD_SPACE_BASE	(MEM_BASE_ADDR)
#define RX_BD_SPACE_HIGH	(MEM_BASE_ADDR + 0x0000FFFF)
#define TX_BD_SPACE_BASE	(MEM_BASE_ADDR + 0x00010000)
#define TX_BD_SPACE_HIGH	(MEM_BASE_ADDR + 0x0001FFFF)
#define TX_BUFFER_BASE		(MEM_BASE_ADDR + 0x00100000)
#define RX_BUFFER_BASE		(MEM_BASE_ADDR + 0x00300000)
#define RX_BUFFER_HIGH		(MEM_BASE_ADDR + 0x004FFFFF)
#define ONE_MB              0x100000U


/* Timeout loop counter for reset */
#define RESET_TIMEOUT_COUNTER	10000

/* Buffer and Buffer Descriptor related constant definition */
#define MAX_PKT_LEN		 1102
#define NUMBER_OF_BDS_TO_TRANSFER 15000
static uint8_t txPkt[MAX_PKT_LEN];


/* The interrupt coalescing threshold and delay timer threshold
 * Valid range is 1 to 255
 */
#define COALESCING_COUNT		1
#define DELAY_TIMER_COUNT		100

#define INTC		    XScuGic
#define INTC_HANDLER	XScuGic_InterruptHandler

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

static void TxCallBack(XAxiDma_BdRing * TxRingPtr);
static void TxIntrHandler(void *Callback);
static void RxCallBack(XAxiDma_BdRing * RxRingPtr);
static void RxIntrHandler(void *Callback);
static int SetupIntrSystem(INTC * IntcInstancePtr,
			   XAxiDma * AxiDmaPtr, u16 TxIntrId, u16 RxIntrId);
static void DisableIntrSystem(INTC * IntcInstancePtr,
					u16 TxIntrId, u16 RxIntrId);
static int RxSetup(XAxiDma * AxiDmaInstPtr);
static int TxSetup(XAxiDma * AxiDmaInstPtr);
static int sendPacket(XAxiDma * AxiDmaInstPtr, const uint8_t * packetBuf);

/************************** Variable Definitions *****************************/
static XAxiDma AxiDma;
static INTC Intc;	/* Instance of the Interrupt Controller */

/*
 * Flags interrupt handlers use to notify the application context the events.
 */
static volatile int TxDone;
static volatile int RxDone;
static volatile int Error;

int demo_dma_loopback(void)
{
	int Status;
	XAxiDma_Config *Config;
	size_t i = 0;

	printf("\r\n--- Entering %s --- \r\n",__func__);

	/* MUST MARK BUFFER SPACE AS UNCACHEABLE */
	for(i = RX_BD_SPACE_BASE; i < RX_BUFFER_HIGH; i += ONE_MB)
		Xil_SetTlbAttributes(i, NORM_NONCACHE);

	Config = XAxiDma_LookupConfig(DMA_DEV_ID);
	if (!Config) {
		ERROR_PRINT("No config found for %d\r\n", DMA_DEV_ID);
		return XST_FAILURE;
	}

	/* Initialize DMA engine */
	XAxiDma_CfgInitialize(&AxiDma, Config);

	if(!XAxiDma_HasSg(&AxiDma)) {
		ERROR_PRINT("Device configured as Simple mode \r\n");
		return XST_FAILURE;
	}

	/* Set up TX/RX channels to be ready to transmit and receive packets */
	Status = TxSetup(&AxiDma);

	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed TX setup\r\n");
		return XST_FAILURE;
	}

	Status = RxSetup(&AxiDma);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed RX setup\r\n");
		return XST_FAILURE;
	}

	/* Set up Interrupt system  */
	Status = SetupIntrSystem(&Intc, &AxiDma, TX_INTR_ID, RX_INTR_ID);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed intr setup\r\n");
		return XST_FAILURE;
	}

	/* Initialize flags before start transfer test  */
	TxDone = 0;
	RxDone = 0;
	Error  = 0;
	for(i=0;i<MAX_PKT_LEN;i++)
		txPkt[i] = i % 255;

	/*
  	 * Wait TX done and RX done
     */
	while (((TxDone < NUMBER_OF_BDS_TO_TRANSFER) &&
			(RxDone < NUMBER_OF_BDS_TO_TRANSFER)) && !Error) {
			/* Send a packet */
			Status = sendPacket(&AxiDma, &txPkt[0]);
			if (Status != XST_SUCCESS) {
				ERROR_PRINT("Failed send packet\r\n");
				return XST_FAILURE;
			}
			// sleep(1);
	}
	if (Error) {
		ERROR_PRINT("Failed test transmit%s done, "
			"receive%s done\r\n", TxDone? "":" not",
					RxDone? "":" not");
		goto Done;

	}

	printf("%d packets sent, %d packets received\r\n",TxDone, RxDone);
	printf("Successfully ran %s Example\r\n",__func__);

Done:
	DisableIntrSystem(&Intc, TX_INTR_ID, RX_INTR_ID);
	printf("--- Exiting %s --- \r\n",__func__);

	return XST_SUCCESS;
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
static void TxCallBack(XAxiDma_BdRing * TxRingPtr)
{
	int BdCount;
	u32 BdSts;
	XAxiDma_Bd *BdPtr;
	XAxiDma_Bd *BdCurPtr;
	int Status;
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
			Error = 1;
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
	Status = XAxiDma_BdRingFree(TxRingPtr, BdCount, BdPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("\r\n");
		Error = 1;
	}

	if(!Error) {
		TxDone += BdCount;
	}
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
		ERROR_PRINT("\r\n");
		Error = 1;

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
		TxCallBack(TxRingPtr);
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
static void RxCallBack(XAxiDma_BdRing * RxRingPtr)
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
			Error = 1;
			break;
		}

		/* get memory offset of current bd */
		uint32_t addr   = XAxiDma_BdGetBufAddr(BdCurPtr);
		uint32_t pktLen = XAxiDma_BdGetActualLength(BdCurPtr,MAX_PKT_LEN);
		printf("-- %s: (BdCount %d) rx packet %04d @ 0x%x len %lu\r\n",__func__,BdCount,RxDone,(unsigned int)addr,pktLen);
		// XAxiDma_DumpBd(BdCurPtr);

		/* check data */
		uint8_t *rxPacket = (u8 *)addr;
		int j = 0;
		for(j = 0; j < MAX_PKT_LEN; j++){
			if(*(rxPacket+j) != txPkt[j]){
				ERROR_PRINT("[%04d]: tx %03d, rx %03d @ 0x%x; RxDone=%04d\r\n",j,txPkt[j],*(rxPacket+j),(rxPacket+j),RxDone);
				// XAxiDma_BdRingDumpRegs(RxRingPtr);
				XAxiDma_DumpBd(BdCurPtr);
				Error = 1;
				break;
			}
		}


		#if 0
		uint8_t rxPacket[MAX_PKT_LEN+1];
		memcpy((void *)(&rxPacket[0]), (void *)addr, (size_t)pktLen);
		int j = 0;
		for(j = 0; j < MAX_PKT_LEN; j++){
				ERROR_PRINT("[%04d]: tx %03d, rx %03d @ 0x%x; RxDone=%04d\r\n",j,txPkt[j],rxPacket[j],(addr+j),RxDone);
			if(rxPacket[j] != txPkt[j]){
				setError = 1;
			}
		}
		if(setError){
			Error = 1;
			break;
		}
		#endif

		/* clear bds memory area to verify this isn't all an illusion... */
		memset((void *)addr, 0, MAX_PKT_LEN);
		
		/* Find the next processed BD */
		if(Index != BdCount-1)
			BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(RxRingPtr, BdCurPtr);

		RxDone++;
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
		Error = 1;

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
		RxCallBack(RxRingPtr);
	}
}

/*****************************************************************************/
/*
 * *
 * * This function setups the interrupt system so interrupts can occur for the
 * * DMA, it assumes INTC component exists in the hardware system.
 * *
 * * @param	IntcInstancePtr is a pointer to the instance of the INTC.
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

static int SetupIntrSystem(INTC * IntcInstancePtr,
			   XAxiDma * AxiDmaPtr, u16 TxIntrId, u16 RxIntrId)
{
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(AxiDmaPtr);
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(AxiDmaPtr);
	int Status;

	XScuGic_Config *IntcConfig;


	/*
  	 * Initialize the interrupt controller driver so that it is ready to
  	 * use.
  	 */
	IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
	if (NULL == IntcConfig) {
		return XST_FAILURE;
	}

	Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
					IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}


	XScuGic_SetPriorityTriggerType(IntcInstancePtr, TxIntrId, 0xA0, 0x3);

	XScuGic_SetPriorityTriggerType(IntcInstancePtr, RxIntrId, 0xA0, 0x3);
	/*
  	 * Connect the device driver handler that will be called when an
  	 * interrupt for the device occurs, the handler defined above performs
  	 * the specific interrupt processing for the device.
  	 */
	Status = XScuGic_Connect(IntcInstancePtr, TxIntrId,
				(Xil_InterruptHandler)TxIntrHandler,
				TxRingPtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	Status = XScuGic_Connect(IntcInstancePtr, RxIntrId,
				(Xil_InterruptHandler)RxIntrHandler,
				RxRingPtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	XScuGic_Enable(IntcInstancePtr, TxIntrId);
	XScuGic_Enable(IntcInstancePtr, RxIntrId);

	/* Enable interrupts from the hardware */
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)INTC_HANDLER,
			(void *)IntcInstancePtr);

	Xil_ExceptionEnable();

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
 * *
 * * This function disables the interrupts for DMA engine.
 * *
 * * @param	IntcInstancePtr is the pointer to the INTC component instance
 * * @param	TxIntrId is interrupt ID associated w/ DMA TX channel
 * * @param	RxIntrId is interrupt ID associated w/ DMA RX channel
 * *
 * * @return	None.
 * *
 * * @note		None.
 * *
 * ******************************************************************************/
static void DisableIntrSystem(INTC * IntcInstancePtr,
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
	int Status;
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
				RX_BD_SPACE_HIGH - RX_BD_SPACE_BASE + 1);

	Status = XAxiDma_BdRingCreate(RxRingPtr, RX_BD_SPACE_BASE,
					RX_BD_SPACE_BASE,
					XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx bd create failed with %d\r\n", Status);
		return XST_FAILURE;
	}
	printf("%d rx bds created\r\n",BdCount);

	/*
  	 * Setup a BD template for the Rx channel. Then copy it to every RX BD.
  	 */
	XAxiDma_BdClear(&BdTemplate);
	Status = XAxiDma_BdRingClone(RxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx bd clone failed with %d\r\n", Status);
		return XST_FAILURE;
	}

	/* Attach buffers to RxBD ring so we are ready to receive packets */
	FreeBdCount = XAxiDma_BdRingGetFreeCnt(RxRingPtr);

	Status = XAxiDma_BdRingAlloc(RxRingPtr, FreeBdCount, &BdPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx bd alloc failed with %d\r\n", Status);
		return XST_FAILURE;
	}

	BdCurPtr = BdPtr;
	RxBufferPtr = RX_BUFFER_BASE;

	for (Index = 0; Index < FreeBdCount; Index++) {

		Status = XAxiDma_BdSetBufAddr(BdCurPtr, RxBufferPtr);
		if (Status != XST_SUCCESS) {
			ERROR_PRINT("Rx set buffer addr %x on BD %x failed %d\r\n",
			(unsigned int)RxBufferPtr,
			(UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		Status = XAxiDma_BdSetLength(BdCurPtr, MAX_PKT_LEN,
					RxRingPtr->MaxTransferLen);
		if (Status != XST_SUCCESS) {
			ERROR_PRINT("Rx set length %d on BD %x failed %d\r\n",
			    MAX_PKT_LEN, (UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		/* Receive BDs do not need to set anything for the control
  		 * The hardware will set the SOF/EOF bits per stream status
  		 */
		XAxiDma_BdSetCtrl(BdCurPtr, 0);

		XAxiDma_BdSetId(BdCurPtr, RxBufferPtr);

		RxBufferPtr += MAX_PKT_LEN;
		BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(RxRingPtr, BdCurPtr);
	}

	Status = XAxiDma_BdRingSetCoalesce(RxRingPtr, COALESCING_COUNT,
			DELAY_TIMER_COUNT);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx set coalesce failed with %d\r\n", Status);
		return XST_FAILURE;
	}

	Status = XAxiDma_BdRingToHw(RxRingPtr, FreeBdCount, BdPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx ToHw failed with %d\r\n", Status);
		return XST_FAILURE;
	}

	Status = XAxiDma_BdRingCheck(RxRingPtr);
	if(Status != XST_SUCCESS){
		ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",Status);
		return XST_FAILURE;
	}

	/* Enable all RX interrupts */
	XAxiDma_BdRingIntEnable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Enable Cyclic DMA mode */
	// XAxiDma_BdRingEnableCyclicDMA(RxRingPtr);
	// XAxiDma_SelectCyclicMode(AxiDmaInstPtr, XAXIDMA_DEVICE_TO_DMA, 1);

	/* Start RX DMA channel */
	Status = XAxiDma_BdRingStart(RxRingPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Rx start BD ring failed with %d\r\n", Status);
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
	int Status;
	u32 BdCount;

	/* Disable all TX interrupts before TxBD space setup */
	XAxiDma_BdRingIntDisable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Setup TxBD space  */
	BdCount = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
			(u32)TX_BD_SPACE_HIGH - (u32)TX_BD_SPACE_BASE + 1);
	printf("%d tx bds created\r\n",(int)BdCount);

	Status = XAxiDma_BdRingCreate(TxRingPtr, TX_BD_SPACE_BASE,
				     TX_BD_SPACE_BASE,
				     XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);
	if (Status != XST_SUCCESS) {

		ERROR_PRINT("Failed create BD ring\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Like the RxBD space, we create a template and set all BDs to be the
  	 * same as the template. The sender has to set up the BDs as needed.
	 */
	XAxiDma_BdClear(&BdTemplate);
	Status = XAxiDma_BdRingClone(TxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {

		ERROR_PRINT("Failed clone BDs\r\n");
		return XST_FAILURE;
	}

	/*
  	 * Set the coalescing threshold, so only one transmit interrupt
  	 * occurs for this example
  	 *
  	 * If you would like to have multiple interrupts to happen, change
  	 * the COALESCING_COUNT to be a smaller value
  	 */
	Status = XAxiDma_BdRingSetCoalesce(TxRingPtr, COALESCING_COUNT,
			DELAY_TIMER_COUNT);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed set coalescing"
		" %d/%d\r\n",COALESCING_COUNT, DELAY_TIMER_COUNT);
		return XST_FAILURE;
	}

	Status = XAxiDma_BdRingCheck(TxRingPtr);
	if(Status != XST_SUCCESS){
		ERROR_PRINT("Failed XAxiDma_BdRingCheck %d\r\n",Status);
		return XST_FAILURE;
	}

	/* Enable all TX interrupts */
	XAxiDma_BdRingIntEnable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/* Start the TX channel */
	Status = XAxiDma_BdRingStart(TxRingPtr);
	if (Status != XST_SUCCESS) {

		ERROR_PRINT("Failed bd start\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

static int sendPacket(XAxiDma * AxiDmaInstPtr, const uint8_t * packetBuf)
{
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(AxiDmaInstPtr);
	u8 *TxPacket;
	XAxiDma_Bd *BdPtr;
	int Status;
	u32 BufferAddr;
	u32 CrBits = 0;

	TxPacket = (u8 *) packetBuf;

	/* Flush the SrcBuffer before the DMA transfer, in case the Data Cache
     * is enabled
	 */
	Xil_DCacheFlushRange((UINTPTR)TxPacket, MAX_PKT_LEN);

	Status = XAxiDma_BdRingAlloc(TxRingPtr, 1, &BdPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed bd alloc\r\n");
		return XST_FAILURE;
	}

	BufferAddr = (UINTPTR)packetBuf;

	Status = XAxiDma_BdSetBufAddr(BdPtr, BufferAddr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Tx set buffer addr %x on BD %x failed %d\r\n",
		(unsigned int)BufferAddr,
		(UINTPTR)BdPtr, Status);
		return XST_FAILURE;
	}

	Status = XAxiDma_BdSetLength(BdPtr, MAX_PKT_LEN,
				TxRingPtr->MaxTransferLen);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Tx set length %d on BD %x failed %d\r\n",
		MAX_PKT_LEN, (UINTPTR)BdPtr, Status);
		return XST_FAILURE;
	}

	/* set both SoF and EoF for single packet mode */
	CrBits |= XAXIDMA_BD_CTRL_TXSOF_MASK;
	CrBits |= XAXIDMA_BD_CTRL_TXEOF_MASK;

	XAxiDma_BdSetCtrl(BdPtr, CrBits);
	XAxiDma_BdSetId(BdPtr, BufferAddr);

	// printf("tx bd\r\n");
	// XAxiDma_DumpBd(BdPtr);

	/* Give the BD to hardware */
	Status = XAxiDma_BdRingToHw(TxRingPtr, 1, BdPtr);
	if (Status != XST_SUCCESS) {
		ERROR_PRINT("Failed to hw, length %d\r\n",
			(int)XAxiDma_BdGetLength(BdPtr,
					TxRingPtr->MaxTransferLen));
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}
