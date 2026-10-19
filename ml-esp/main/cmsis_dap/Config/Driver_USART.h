/*
 * Copyright (c) 2013-2021 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CMSIS-Driver USART API (subset): the definitions used by the SWO UART
 * implementation (SWO.c, upstream) and this port's Driver_USART0. Value
 * encodings follow the ARM CMSIS-Driver interface so a full driver header
 * can replace this file verbatim; both sides of the ABI in this firmware
 * include this same copy.
 */
#ifndef DRIVER_USART_H__
#define DRIVER_USART_H__

#include <stdint.h>

/* General return codes */
#define ARM_DRIVER_OK                 0
#define ARM_DRIVER_ERROR              (-1)
#define ARM_DRIVER_ERROR_BUSY         (-2)
#define ARM_DRIVER_ERROR_UNSUPPORTED  (-3)
#define ARM_DRIVER_ERROR_PARAMETER    (-4)

/* Power states (ARM_POWER_*) */
#define ARM_POWER_OFF                 0
#define ARM_POWER_LOW                 1
#define ARM_POWER_FULL                2

/****** USART Control Codes *****/

#define ARM_USART_CONTROL_Pos           0UL
#define ARM_USART_MODE_ASYNCHRONOUS     (0x01UL << ARM_USART_CONTROL_Pos)
#define ARM_USART_MODE_SYNCHRONOUS_MASTER (0x02UL << ARM_USART_CONTROL_Pos)
#define ARM_USART_MODE_SYNCHRONOUS_SLAVE  (0x03UL << ARM_USART_CONTROL_Pos)
#define ARM_USART_MODE_SINGLE_WIRE_MASTER (0x04UL << ARM_USART_CONTROL_Pos)
#define ARM_USART_MODE_SINGLE_WIRE_SLAVE  (0x05UL << ARM_USART_CONTROL_Pos)

#define ARM_USART_DATA_BITS_Pos         8UL
#define ARM_USART_DATA_BITS_Msk         (0x7UL << ARM_USART_DATA_BITS_Pos)
#define ARM_USART_DATA_BITS_5           (5UL << ARM_USART_DATA_BITS_Pos)
#define ARM_USART_DATA_BITS_6           (6UL << ARM_USART_DATA_BITS_Pos)
#define ARM_USART_DATA_BITS_7           (7UL << ARM_USART_DATA_BITS_Pos)
#define ARM_USART_DATA_BITS_8           (0UL << ARM_USART_DATA_BITS_Pos)
#define ARM_USART_DATA_BITS_9           (1UL << ARM_USART_DATA_BITS_Pos)

#define ARM_USART_PARITY_Pos            12UL
#define ARM_USART_PARITY_Msk            (0x3UL << ARM_USART_PARITY_Pos)
#define ARM_USART_PARITY_NONE           (0UL << ARM_USART_PARITY_Pos)
#define ARM_USART_PARITY_EVEN           (1UL << ARM_USART_PARITY_Pos)
#define ARM_USART_PARITY_ODD            (2UL << ARM_USART_PARITY_Pos)

#define ARM_USART_STOP_BITS_Pos         14UL
#define ARM_USART_STOP_BITS_Msk         (0x3UL << ARM_USART_STOP_BITS_Pos)
#define ARM_USART_STOP_BITS_1           (0UL << ARM_USART_STOP_BITS_Pos)
#define ARM_USART_STOP_BITS_2           (1UL << ARM_USART_STOP_BITS_Pos)
#define ARM_USART_STOP_BITS_1_5         (2UL << ARM_USART_STOP_BITS_Pos)
#define ARM_USART_STOP_BITS_0_5         (3UL << ARM_USART_STOP_BITS_Pos)

#define ARM_USART_FLOW_CONTROL_Pos      16UL
#define ARM_USART_FLOW_CONTROL_NONE     (0UL << ARM_USART_FLOW_CONTROL_Pos)
#define ARM_USART_FLOW_CONTROL_RTS      (1UL << ARM_USART_FLOW_CONTROL_Pos)
#define ARM_USART_FLOW_CONTROL_CTS      (2UL << ARM_USART_FLOW_CONTROL_Pos)
#define ARM_USART_FLOW_CONTROL_RTS_CTS  (3UL << ARM_USART_FLOW_CONTROL_Pos)

/****** USART Send Control Codes *****/

#define ARM_USART_CONTROL_TX            ((int32_t)(1UL << 18))
#define ARM_USART_CONTROL_RX            ((int32_t)(1UL << 19))
#define ARM_USART_ABORT_SEND            ((int32_t)(1UL << 20))
#define ARM_USART_ABORT_RECEIVE         ((int32_t)(1UL << 21))

/****** USART Signal Events *****/

#define ARM_USART_EVENT_SEND_COMPLETE       (1UL << 0)
#define ARM_USART_EVENT_RECEIVE_COMPLETE    (1UL << 1)
#define ARM_USART_EVENT_TRANSFER_COMPLETE   (1UL << 2)
#define ARM_USART_EVENT_TX_COMPLETE         (1UL << 3)
#define ARM_USART_EVENT_SEND_BREAK          (1UL << 4)
#define ARM_USART_EVENT_RX_TIMEOUT          (1UL << 5)
#define ARM_USART_EVENT_RX_OVERFLOW         (1UL << 6)
#define ARM_USART_EVENT_RX_BREAK            (1UL << 7)
#define ARM_USART_EVENT_RX_FRAMING_ERROR    (1UL << 8)
#define ARM_USART_EVENT_RX_PARITY_ERROR     (1UL << 9)
#define ARM_USART_EVENT_CTS                 (1UL << 10)
#define ARM_USART_EVENT_DCD                 (1UL << 11)
#define ARM_USART_EVENT_DSR                 (1UL << 12)
#define ARM_USART_EVENT_RI                  (1UL << 13)

/* USART Status */
typedef struct _ARM_USART_STATUS {
  uint32_t tx_busy          : 1;
  uint32_t rx_busy          : 1;
  uint32_t tx_underflow     : 1;
  uint32_t rx_overflow      : 1;
  uint32_t rx_break         : 1;
  uint32_t rx_breaking      : 1;
  uint32_t rx_framing_error : 1;
  uint32_t rx_parity_error  : 1;
} ARM_USART_STATUS;

/* USART Modem Control */
typedef enum _ARM_USART_MODEM_CONTROL {
  ARM_USART_MODEM_CONTROL_RTS = 0,
  ARM_USART_MODEM_CONTROL_DTR = 1
} ARM_USART_MODEM_CONTROL;

/* USART Modem Status */
typedef struct _ARM_USART_MODEM_STATUS {
  uint32_t cts : 1;
  uint32_t dsr : 1;
  uint32_t dcd : 1;
  uint32_t ri  : 1;
  uint32_t reserved : 28;
} ARM_USART_MODEM_STATUS;

typedef void (*ARM_USART_SignalEvent_t)(uint32_t event);

typedef struct _ARM_DRIVER_USART {
  int32_t (*Initialize)      (ARM_USART_SignalEvent_t cb_event);
  int32_t (*Uninitialize)    (void);
  int32_t (*PowerControl)    (uint32_t state);
  int32_t (*Send)            (const void *data, uint32_t num);
  int32_t (*Receive)         (void *data, uint32_t num);
  int32_t (*Transfer)        (const void *data_out, void *data_in, uint32_t num);
  uint32_t (*GetTxCount)     (void);
  uint32_t (*GetRxCount)     (void);
  int32_t (*Control)         (uint32_t control, uint32_t arg);
  ARM_USART_STATUS (*GetStatus) (void);
  int32_t (*SetModemControl) (ARM_USART_MODEM_CONTROL control);
  ARM_USART_MODEM_STATUS (*GetModemStatus) (void);
} ARM_DRIVER_USART;

#endif /* DRIVER_USART_H__ */
