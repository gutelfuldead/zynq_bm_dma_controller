#ifndef AXIS_DMA_CONTROLLER_H
#define AXIS_DMA_CONTROLLER_H

#include "xaxidma.h"
#include "xscugic.h"

#define E_NOBDS -2 /**< returned when not enough bds can be allocated */

typedef void (*rx_cb_t)(uint32_t buf_addr, uint32_t buf_len);
typedef void (*tx_cb_t)(void);

struct axisDmaCtrl_params{
	/* memory parameters */
	size_t rx_bd_space_base;
	size_t rx_bd_space_high;
	size_t tx_bd_space_base;
	size_t tx_bd_space_high;
	size_t tx_buffer_base;
	size_t tx_buffer_high;
	size_t rx_buffer_base;
	size_t rx_buffer_high;
	size_t bd_buf_size;
	uint8_t coalesce_count;	
};

int axisDmaCtrl_register_tx_cb(tx_cb_t cb);
int axisDmaCtrl_register_rx_cb(rx_cb_t cb);
int axisDmaCtrl_init(struct axisDmaCtrl_params *params, 
	XScuGic * IntcInstancePtr,
	rx_cb_t rxCb,
	tx_cb_t txCb);
int axisDmaCtrl_disable(XScuGic * IntcInstancePtr);
int axisDmaCtrl_sendPackets(uint8_t * packetBuf, size_t packetSize);

#endif // AXIS_DMA_CONTROLLER_H
