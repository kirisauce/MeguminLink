/*
 * Copyright (c) 2013-2021 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ----------------------------------------------------------------------
 *
 * $Date:        16. June 2021
 * $Revision:    V2.1.0
 *
 * Project:      CMSIS-DAP Configuration
 * Title:        DAP_config.h CMSIS-DAP Configuration File (ESP32-S3 Port)
 *
 *---------------------------------------------------------------------------*/

#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__


//**************************************************************************************************
/**
\defgroup DAP_Config_Debug_gr CMSIS-DAP Debug Unit Information
\ingroup DAP_ConfigIO_gr
@{
Provides definitions about the hardware and configuration of the Debug Unit.

This information includes:
 - Definition of Cortex-M processor parameters used in CMSIS-DAP Debug Unit.
 - Debug Unit Identification strings (Vendor, Product, Serial Number).
 - Debug Unit communication packet size.
 - Debug Access Port supported modes and settings (JTAG/SWD and SWO).
 - Optional information about a connected Target Device (for Evaluation Boards).
*/

#ifdef _RTE_
#include "RTE_Components.h"
#include CMSIS_device_header
#else
// ESP32-S3 (ESP-IDF v6.0.1) port: replaces the Cortex-M device header of the
// upstream template. cmsis_compiler.h is pulled in explicitly because
// DAP_config.h is included before DAP.h (upstream gets the macros via the
// device header chain instead).
#include "cmsis_compiler.h"
#include "sdkconfig.h"          // CONFIG_MEGUMINLINK_DAP_*_IO GPIO 引脚
#include "esp_cpu.h"            // esp_cpu_get_cycle_count（TIMESTAMP_GET）
#include "esp_mac.h"            // esp_read_mac / MACSTR（序列号）
#include "hal/gpio_ll.h"        // 热路径 GPIO 位操作
#include "driver/gpio.h"        // PORT_* 初始化
#include <stdio.h>              // snprintf
#include <string.h>             // strcpy / strlen
#endif

/// Processor Clock of the Cortex-M MCU used in the Debug Unit.
/// This value is used to calculate the SWD/JTAG clock speed.
// NOTE: 声称值低于实际 CPU 频率（160MHz），使 SWJ 时钟计算产生更小的 clock_delay
// 与更慢的 SWCLK。实测：AP 访问（APnDP=1）在 SWCLK > ~10kHz 时目标 ACK 采样
// 失败（目标 AP 桥接输出延迟与快速位宽不匹配），~9kHz 以下稳定工作；
// 该声称值把 1MHz 设定映射到 ~5.7kHz 实际时钟，留足余量。
#define CPU_CLOCK               (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U)  ///< Specifies the CPU Clock in Hz.

/// Number of processor cycles for I/O Port write operations.
/// This value is used to calculate the SWD/JTAG clock speed that is generated with I/O
/// Port write operations in the Debug Unit by a Cortex-M MCU. Most Cortex-M processors
/// require 2 processor cycles for a I/O Port Write operation.  If the Debug Unit uses
/// a Cortex-M0+ processor with high-speed peripheral I/O only 1 processor cycle might be
/// required.
#define IO_PORT_WRITE_CYCLES    2U              ///< I/O Cycles: 2=default, 1=Cortex-M0+ fast I/0.

// Clock delay loop for SWD/JTAG clock generation.
// DAP.h provides a Cortex-M GCC implementation of PIN_DELAY_SLOW in ARM
// assembly (`subs`/`bne`), which does not assemble on Xtensa. The matching
// `#elif !defined(PIN_DELAY_SLOW)` guard in DAP.h skips that branch whenever
// this file (always included before DAP.h) provides its own portable version.
// DELAY_SLOW_CYCLES is the number of processor cycles consumed by one loop
// iteration. 注意：不能用 volatile 计数器循环——编译器会对每次 volatile
// 内存访问插入 memw 内存屏障（Xtensa memw 需等待 store buffer 排空，
// 每个 ~15-20 周期），实测每次迭代高达 ~50 周期，把 SWCLK 压到 ~114kHz。
// 改用"寄存器变量 + 空汇编内存屏障"循环：空 asm（""）是 GCC/Clang 全平台
// 通用语法，不产生任何指令，仅用 memory clobber 阻止编译器优化掉循环；
// 循环本体退化为 sub/test/branch，每迭代 ~3-4 周期，无内存访问、无 memw，
// 且不依赖任何平台特定指令（可移植回其他架构）。
// Defined as a function-like macro so the guard in DAP.h
// (`defined(PIN_DELAY_SLOW)`) can see it.
#define DELAY_SLOW_CYCLES       4U              // Number of cycles for one iteration
#define PIN_DELAY_SLOW(delay)                                                  \
  do {                                                                         \
    uint32_t __dap_delay_count = (uint32_t)(delay);                            \
    while (__dap_delay_count != 0U) {                                          \
      __asm__ __volatile__("" ::: "memory");                                   \
      __dap_delay_count--;                                                     \
    }                                                                          \
  } while (0)

/// Indicate that Serial Wire Debug (SWD) communication mode is available at the Debug Access Port.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_SWD                 1               ///< SWD Mode:  1 = available, 0 = not available.

/// Indicate that JTAG communication mode is available at the Debug Port.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_JTAG                1               ///< JTAG Mode: 1 = available, 0 = not available.

/// Configure maximum number of JTAG devices on the scan chain connected to the Debug Access Port.
/// This setting impacts the RAM requirements of the Debug Unit. Valid range is 1 .. 255.
#define DAP_JTAG_DEV_CNT        8U              ///< Maximum number of JTAG devices on scan chain.

/// Default communication mode on the Debug Access Port.
/// Used for the command \ref DAP_Connect when Port Default mode is selected.
#define DAP_DEFAULT_PORT        1U              ///< Default JTAG/SWJ Port Mode: 1 = SWD, 2 = JTAG.

/// Default communication speed on the Debug Access Port for SWD and JTAG mode.
/// Used to initialize the default SWD/JTAG clock frequency.
/// The command \ref DAP_SWJ_Clock can be used to overwrite this default setting.
#define DAP_DEFAULT_SWJ_CLOCK   1000000U        ///< Default SWD/JTAG clock frequency in Hz.

/// Maximum Package Size for Command and Response data.
/// This configuration settings is used to optimize the communication performance with the
/// debugger and depends on the USB peripheral. Typical vales are 64 for Full-speed USB HID or WinUSB,
/// 1024 for High-speed USB HID and 512 for High-speed USB WinUSB.
#define DAP_PACKET_SIZE         64U             ///< Specifies Packet Size in bytes.

/// Maximum Package Buffers for Command and Response data.
/// This configuration settings is used to optimize the communication performance with the
/// debugger and depends on the USB peripheral. For devices with limited RAM or USB buffer the
/// setting can be reduced (valid range is 1 .. 255).
/// 复用 MEGUMINLINK_PACKET_BUFFER_DEPTH（默认 8）：此前写死 1，OpenOCD 报
/// "Packet Count = 1" 导致零流水线（发一个包等一个响应）；与 ringbuffer
/// 深度保持一致即可让 OpenOCD 的 pending FIFO 流水线化。该配置项已从
/// USB 支持中解耦（无 USB 目标同样存在），无线接收端同样按此取值。
#define DAP_PACKET_COUNT        CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH  ///< Specifies number of packets buffered.

/// Indicate that UART Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_UART                0               ///< SWO UART:  1 = available, 0 = not available.

/// USART Driver instance number for the UART SWO.
#define SWO_UART_DRIVER         0               ///< USART Driver instance number (Driver_USART#).

/// Maximum SWO UART Baudrate.
#define SWO_UART_MAX_BAUDRATE   10000000U       ///< SWO UART Maximum Baudrate in Hz.

/// Indicate that Manchester Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_MANCHESTER          0               ///< SWO Manchester:  1 = available, 0 = not available.

/// SWO Trace Buffer Size.
#define SWO_BUFFER_SIZE         4096U           ///< SWO Trace Buffer Size in bytes (must be 2^n).

/// SWO Streaming Trace.
#define SWO_STREAM              0               ///< SWO Streaming Trace: 1 = available, 0 = not available.

/// Clock frequency of the Test Domain Timer. Timer value is returned with \ref TIMESTAMP_GET.
#define TIMESTAMP_CLOCK         CPU_CLOCK       ///< Timestamp clock in Hz (0 = timestamps not supported).

/// Indicate that UART Communication Port is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_UART                0               ///< DAP UART:  1 = available, 0 = not available.

/// USART Driver instance number for the UART Communication Port.
#define DAP_UART_DRIVER         1               ///< USART Driver instance number (Driver_USART#).

/// UART Receive Buffer Size.
#define DAP_UART_RX_BUFFER_SIZE 1024U           ///< Uart Receive Buffer Size in bytes (must be 2^n).

/// UART Transmit Buffer Size.
#define DAP_UART_TX_BUFFER_SIZE 1024U           ///< Uart Transmit Buffer Size in bytes (must be 2^n).

/// Indicate that UART Communication via USB COM Port is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_UART_USB_COM_PORT   0               ///< USB COM Port:  1 = available, 0 = not available.

/// Debug Unit is connected to fixed Target Device.
/// The Debug Unit may be part of an evaluation board and always connected to a fixed
/// known device. In this case a Device Vendor, Device Name, Board Vendor and Board Name strings
/// are stored and may be used by the debugger or IDE to configure device parameters.
#define TARGET_FIXED            0               ///< Target: 1 = known, 0 = unknown;

#define TARGET_DEVICE_VENDOR    "Arm"           ///< String indicating the Silicon Vendor
#define TARGET_DEVICE_NAME      "Cortex-M"      ///< String indicating the Target Device
#define TARGET_BOARD_VENDOR     "Arm"           ///< String indicating the Board Vendor
#define TARGET_BOARD_NAME       "Arm board"     ///< String indicating the Board Name

#if TARGET_FIXED != 0
#include <string.h>
static const char TargetDeviceVendor [] = TARGET_DEVICE_VENDOR;
static const char TargetDeviceName   [] = TARGET_DEVICE_NAME;
static const char TargetBoardVendor  [] = TARGET_BOARD_VENDOR;
static const char TargetBoardName    [] = TARGET_BOARD_NAME;
#endif

// DAP I/O pins (ESP32-S3 GPIO numbers, configurable via Kconfig).
// Cast to gpio_num_t so the header also compiles from C++ TUs (usb.cc),
// where int -> enum conversions are rejected.
#define DAP_SWCLK_TCK_GPIO      ((gpio_num_t)CONFIG_MEGUMINLINK_DAP_SWCLK_IO)  // SWCLK/TCK:  Clock
#define DAP_SWDIO_TMS_GPIO      ((gpio_num_t)CONFIG_MEGUMINLINK_DAP_SWDIO_IO)  // SWDIO/TMS:  Data I/O
#define DAP_TDI_GPIO            ((gpio_num_t)CONFIG_MEGUMINLINK_DAP_TDI_IO)    // TDI:        Test Data Input
#define DAP_TDO_GPIO            ((gpio_num_t)CONFIG_MEGUMINLINK_DAP_TDO_IO)    // TDO:        Test Data Output
#define DAP_nRESET_GPIO         ((gpio_num_t)CONFIG_MEGUMINLINK_DAP_NRESET_IO) // nRESET:     Device Reset
#define DAP_nTRST_GPIO          DAP_nRESET_GPIO                                // nTRST:      Test Reset（与 nRESET 共用同一根复位线）

/** Get Vendor Name string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetVendorString (char *str) {
  strcpy(str, "MeguminLink");
  return ((uint8_t)(strlen("MeguminLink") + 1U));
}

/** Get Product Name string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetProductString (char *str) {
  strcpy(str, "MeguminLink CMSIS-DAP");
  return ((uint8_t)(strlen("MeguminLink CMSIS-DAP") + 1U));
}

/** Get Serial Number string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetSerNumString (char *str) {
  static char ser[18];                // MACSTR "aa:bb:cc:dd:ee:ff"（17 字符 + NUL）
  uint8_t     mac[6];

  esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
  snprintf(ser, sizeof(ser), MACSTR, MAC2STR(mac));
  strcpy(str, ser);
  return ((uint8_t)(strlen(ser) + 1U));
}

/** Get Target Device Vendor string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetDeviceVendor);
  len = (uint8_t)(strlen(TargetDeviceVendor) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/** Get Target Device Name string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetDeviceName);
  len = (uint8_t)(strlen(TargetDeviceName) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/** Get Target Board Vendor string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetBoardVendor);
  len = (uint8_t)(strlen(TargetBoardVendor) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/** Get Target Board Name string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetBoardName);
  len = (uint8_t)(strlen(TargetBoardName) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/** Get Product Firmware Version string.
\param str Pointer to buffer to store the string (max 60 characters).
\return String length (including terminating NULL character) or 0 (no string).
*/
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString (char *str) {
  (void)str;
  return (0U);
}

///@}


//**************************************************************************************************
/**
\defgroup DAP_Config_PortIO_gr CMSIS-DAP Hardware I/O Pin Access
\ingroup DAP_ConfigIO_gr
@{

Standard I/O Pins of the CMSIS-DAP Hardware Debug Port support standard JTAG mode
and Serial Wire Debug (SWD) mode. In SWD mode only 2 pins are required to implement the debug
interface of a device. The following I/O Pins are provided:

JTAG I/O Pin                 | SWD I/O Pin          | CMSIS-DAP Hardware pin mode
---------------------------- | -------------------- | ---------------------------------------------
TCK: Test Clock              | SWCLK: Clock         | Output Push/Pull
TMS: Test Mode Select        | SWDIO: Data I/O      | Output Push/Pull; Input (for receiving data)
TDI: Test Data Input         |                      | Output Push/Pull
TDO: Test Data Output        |                      | Input
nTRST: Test Reset (optional) |                      | Output Open Drain with pull-up resistor
nRESET: Device Reset         | nRESET: Device Reset | Output Open Drain with pull-up resistor


DAP Hardware I/O Pin Access Functions
-------------------------------------
The various I/O Pins are accessed by functions that implement the Read, Write, Set, or Clear to
these I/O Pins.

For the SWDIO I/O Pin there are additional functions that are called in SWD I/O mode only.
This functions are provided to achieve faster I/O that is possible with some advanced GPIO
peripherals that can independently write/read a single I/O pin without affecting any other pins
of the same I/O port. The following SWDIO I/O Pin functions are provided:
 - \ref PIN_SWDIO_OUT_ENABLE to enable the output mode from the DAP hardware.
 - \ref PIN_SWDIO_OUT_DISABLE to enable the input mode to the DAP hardware.
 - \ref PIN_SWDIO_IN to read from the SWDIO I/O pin with utmost possible speed.
 - \ref PIN_SWDIO_OUT to write to the SWDIO I/O pin with utmost possible speed.
*/

// Configure DAP I/O pins ------------------------------

/** Setup JTAG I/O pins: TCK, TMS, TDI, TDO, nTRST, and nRESET.
Configures the DAP Hardware I/O pins for JTAG mode:
 - TCK, TMS, TDI, nTRST, nRESET to output mode and set to high level.
 - TDO to input mode.
*/
__STATIC_INLINE void PORT_JTAG_SETUP (void) {
  // TCK/TMS/TDI: 推挽输出，输入缓冲开启（DAP_SWJ_Pins 需读回），默认高
  gpio_config_t io;
  io.pin_bit_mask = (1ULL << DAP_SWCLK_TCK_GPIO) |
                    (1ULL << DAP_SWDIO_TMS_GPIO) |
                    (1ULL << DAP_TDI_GPIO);
  io.mode = GPIO_MODE_INPUT_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  gpio_set_level(DAP_SWCLK_TCK_GPIO, 1);
  gpio_set_level(DAP_SWDIO_TMS_GPIO, 1);
  gpio_set_level(DAP_TDI_GPIO, 1);
  // nRESET/nTRST（共用同一根复位线）: 开漏输出 + 上拉（输入缓冲开启以便读回），默认高（释放复位）
  gpio_config_t od;
  od.pin_bit_mask = (1ULL << DAP_nRESET_GPIO);
  od.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  od.pull_up_en = GPIO_PULLUP_ENABLE;
  od.pull_down_en = GPIO_PULLDOWN_DISABLE;
  od.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&od);
  gpio_set_level(DAP_nRESET_GPIO, 1);
  // TDO: 输入
  gpio_config_t tdo;
  tdo.pin_bit_mask = (1ULL << DAP_TDO_GPIO);
  tdo.mode = GPIO_MODE_INPUT;
  tdo.pull_up_en = GPIO_PULLUP_DISABLE;
  tdo.pull_down_en = GPIO_PULLDOWN_DISABLE;
  tdo.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&tdo);
}

/** Setup SWD I/O pins: SWCLK, SWDIO, and nRESET.
Configures the DAP Hardware I/O pins for Serial Wire Debug (SWD) mode:
 - SWCLK, SWDIO, nRESET to output mode and set to default high level.
 - TDI, nTRST to HighZ mode (pins are unused in SWD mode).
*/
__STATIC_INLINE void PORT_SWD_SETUP (void) {
  // SWCLK/SWDIO: 推挽输出，输入缓冲开启（SWDIO 读取 / SWCLK 读回），默认高
  gpio_config_t io;
  io.pin_bit_mask = (1ULL << DAP_SWCLK_TCK_GPIO) | (1ULL << DAP_SWDIO_TMS_GPIO);
  io.mode = GPIO_MODE_INPUT_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  gpio_set_level(DAP_SWCLK_TCK_GPIO, 1);
  gpio_set_level(DAP_SWDIO_TMS_GPIO, 1);
  // nRESET: 开漏输出 + 上拉（输入缓冲开启以便读回），默认高（释放复位）
  gpio_config_t od;
  od.pin_bit_mask = (1ULL << DAP_nRESET_GPIO);
  od.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  od.pull_up_en = GPIO_PULLUP_ENABLE;
  od.pull_down_en = GPIO_PULLDOWN_DISABLE;
  od.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&od);
  gpio_set_level(DAP_nRESET_GPIO, 1);
  // TDI/TDO/nTRST: 高阻输入（SWD 模式未使用）
  gpio_config_t hz;
  hz.pin_bit_mask = (1ULL << DAP_TDI_GPIO) |
                    (1ULL << DAP_TDO_GPIO);
  hz.mode = GPIO_MODE_INPUT;
  hz.pull_up_en = GPIO_PULLUP_DISABLE;
  hz.pull_down_en = GPIO_PULLDOWN_DISABLE;
  hz.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&hz);
}

/** Disable JTAG/SWD I/O Pins.
Disables the DAP Hardware I/O pins which configures:
 - TCK/SWCLK, TMS/SWDIO, TDI, TDO, nTRST, nRESET to High-Z mode.
*/
__STATIC_INLINE void PORT_OFF (void) {
  // DAP 引脚禁用输入滤波：滤波窗口会滤掉快速 SWD 数据位（<~100µs），
  // 导致目标输出的 ACK/数据读不到；慢速时钟下位宽超过窗口才偶然通过
  gpio_ll_pin_filter_disable(&GPIO, DAP_SWCLK_TCK_GPIO);
  gpio_ll_pin_filter_disable(&GPIO, DAP_SWDIO_TMS_GPIO);
  gpio_ll_pin_filter_disable(&GPIO, DAP_TDI_GPIO);
  gpio_ll_pin_filter_disable(&GPIO, DAP_TDO_GPIO);
  gpio_ll_pin_filter_disable(&GPIO, DAP_nRESET_GPIO);
  // 全部 7 脚 → 输入高阻
  gpio_config_t io;
  io.pin_bit_mask = (1ULL << DAP_SWCLK_TCK_GPIO) |
                    (1ULL << DAP_SWDIO_TMS_GPIO) |
                    (1ULL << DAP_TDI_GPIO) |
                    (1ULL << DAP_TDO_GPIO);
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  // nRESET/nTRST: 高阻输入，保留弱上拉（复位线悬空时需拉高）
  gpio_config_t od;
  od.pin_bit_mask = (1ULL << DAP_nRESET_GPIO);
  od.mode = GPIO_MODE_INPUT;
  od.pull_up_en = GPIO_PULLUP_ENABLE;
  od.pull_down_en = GPIO_PULLDOWN_DISABLE;
  od.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&od);
}


// SWCLK/TCK I/O pin -------------------------------------

/** SWCLK/TCK I/O pin: Get Input.
\return Current status of the SWCLK/TCK DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN  (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_SWCLK_TCK_GPIO));
}

/** SWCLK/TCK I/O pin: Set Output to High.
Set the SWCLK/TCK DAP hardware I/O pin to high level.
*/
__STATIC_FORCEINLINE void     PIN_SWCLK_TCK_SET (void) {
  gpio_ll_set_level(&GPIO, DAP_SWCLK_TCK_GPIO, 1);
}

/** SWCLK/TCK I/O pin: Set Output to Low.
Set the SWCLK/TCK DAP hardware I/O pin to low level.
*/
__STATIC_FORCEINLINE void     PIN_SWCLK_TCK_CLR (void) {
  gpio_ll_set_level(&GPIO, DAP_SWCLK_TCK_GPIO, 0);
}


// SWDIO/TMS Pin I/O --------------------------------------

/** SWDIO/TMS I/O pin: Get Input.
\return Current status of the SWDIO/TMS DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN  (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_SWDIO_TMS_GPIO));
}

/** SWDIO/TMS I/O pin: Set Output to High.
Set the SWDIO/TMS DAP hardware I/O pin to high level.
*/
__STATIC_FORCEINLINE void     PIN_SWDIO_TMS_SET (void) {
  gpio_ll_set_level(&GPIO, DAP_SWDIO_TMS_GPIO, 1);
}

/** SWDIO/TMS I/O pin: Set Output to Low.
Set the SWDIO/TMS DAP hardware I/O pin to low level.
*/
__STATIC_FORCEINLINE void     PIN_SWDIO_TMS_CLR (void) {
  gpio_ll_set_level(&GPIO, DAP_SWDIO_TMS_GPIO, 0);
}

/** SWDIO I/O pin: Get Input (used in SWD mode only).
\return Current status of the SWDIO DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN      (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_SWDIO_TMS_GPIO));
}

/** SWDIO I/O pin: Set Output (used in SWD mode only).
\param bit Output value for the SWDIO DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE void     PIN_SWDIO_OUT     (uint32_t bit) {
  // 上游 SW_DP.c 传入的 bit 可能是多位的（如 request>>0 = 0x02），
  // 只取最低位输出；否则 APnDP 等位会因非零值被误输出为 1
  gpio_ll_set_level(&GPIO, DAP_SWDIO_TMS_GPIO, bit & 1U);
}

/** SWDIO I/O pin: Switch to Output mode (used in SWD mode only).
Configure the SWDIO DAP hardware I/O pin to output mode. This function is
called prior \ref PIN_SWDIO_OUT function calls.
*/
__STATIC_FORCEINLINE void     PIN_SWDIO_OUT_ENABLE  (void) {
  gpio_ll_output_enable(&GPIO, DAP_SWDIO_TMS_GPIO);
}

/** SWDIO I/O pin: Switch to Input mode (used in SWD mode only).
Configure the SWDIO DAP hardware I/O pin to input mode. This function is
called prior \ref PIN_SWDIO_IN function calls.
*/
__STATIC_FORCEINLINE void     PIN_SWDIO_OUT_DISABLE (void) {
  gpio_ll_output_disable(&GPIO, DAP_SWDIO_TMS_GPIO);
}


// TDI Pin I/O ---------------------------------------------

/** TDI I/O pin: Get Input.
\return Current status of the TDI DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN  (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_TDI_GPIO));
}

/** TDI I/O pin: Set Output.
\param bit Output value for the TDI DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE void     PIN_TDI_OUT (uint32_t bit) {
  gpio_ll_set_level(&GPIO, DAP_TDI_GPIO, bit & 1U);
}


// TDO Pin I/O ---------------------------------------------

/** TDO I/O pin: Get Input.
\return Current status of the TDO DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN  (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_TDO_GPIO));
}


// nTRST Pin I/O -------------------------------------------

/** nTRST I/O pin: Get Input.
\return Current status of the nTRST DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN   (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_nTRST_GPIO));
}

/** nTRST I/O pin: Set Output.
\param bit JTAG TRST Test Reset pin status:
           - 0: issue a JTAG TRST Test Reset.
           - 1: release JTAG TRST Test Reset.
*/
__STATIC_FORCEINLINE void     PIN_nTRST_OUT  (uint32_t bit) {
  gpio_ll_set_level(&GPIO, DAP_nTRST_GPIO, bit);
}

// nRESET Pin I/O------------------------------------------

/** nRESET I/O pin: Get Input.
\return Current status of the nRESET DAP hardware I/O pin.
*/
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN  (void) {
  return ((uint32_t)gpio_ll_get_level(&GPIO, DAP_nRESET_GPIO));
}

/** nRESET I/O pin: Set Output.
\param bit target device hardware reset pin status:
           - 0: issue a device hardware reset.
           - 1: release device hardware reset.
*/
__STATIC_FORCEINLINE void     PIN_nRESET_OUT (uint32_t bit) {
  gpio_ll_set_level(&GPIO, DAP_nRESET_GPIO, bit);
}

///@}


//**************************************************************************************************
/**
\defgroup DAP_Config_LEDs_gr CMSIS-DAP Hardware Status LEDs
\ingroup DAP_ConfigIO_gr
@{

CMSIS-DAP Hardware may provide LEDs that indicate the status of the CMSIS-DAP Debug Unit.

It is recommended to provide the following LEDs for status indication:
 - Connect LED: is active when the DAP hardware is connected to a debugger.
 - Running LED: is active when the debugger has put the target device into running state.
*/

/** Debug Unit: Set status of Connected LED.
\param bit status of the Connect LED.
           - 1: Connect LED ON: debugger is connected to CMSIS-DAP Debug Unit.
           - 0: Connect LED OFF: debugger is not connected to CMSIS-DAP Debug Unit.
*/
__STATIC_INLINE void LED_CONNECTED_OUT (uint32_t bit) {}

/** Debug Unit: Set status Target Running LED.
\param bit status of the Target Running LED.
           - 1: Target Running LED ON: program execution in target started.
           - 0: Target Running LED OFF: program execution in target stopped.
*/
__STATIC_INLINE void LED_RUNNING_OUT (uint32_t bit) {}

///@}


//**************************************************************************************************
/**
\defgroup DAP_Config_Timestamp_gr CMSIS-DAP Timestamp
\ingroup DAP_ConfigIO_gr
@{
Access function for Test Domain Timer.

The value of the Test Domain Timer in the Debug Unit is returned by the function \ref TIMESTAMP_GET. By
default, the DWT timer is used.  The frequency of this timer is configured with \ref TIMESTAMP_CLOCK.

*/

/** Get timestamp of Test Domain Timer.
\return Current timestamp value.
*/
__STATIC_INLINE uint32_t TIMESTAMP_GET (void) {
  return ((uint32_t)esp_cpu_get_cycle_count());
}

///@}


//**************************************************************************************************
/**
\defgroup DAP_Config_Initialization_gr CMSIS-DAP Initialization
\ingroup DAP_ConfigIO_gr
@{

CMSIS-DAP Hardware I/O and LED Pins are initialized with the function \ref DAP_SETUP.
*/

/** Setup of the Debug Unit I/O pins and LEDs (called when Debug Unit is initialized).
This function performs the initialization of the CMSIS-DAP Hardware I/O Pins and the
Status LEDs. In detail the operation of Hardware I/O and LED pins are enabled and set:
 - I/O clock system enabled.
 - all I/O pins: input buffer enabled, output pins are set to HighZ mode.
 - for nTRST, nRESET a weak pull-up (if available) is enabled.
 - LED output pins are enabled and LEDs are turned off.
*/
__STATIC_INLINE void DAP_SETUP (void) {
  PORT_OFF();  // 引脚初始高阻（nRESET/nTRST 保留弱上拉）
}

/** Reset Target Device with custom specific I/O pin or command sequence.
This function allows the optional implementation of a device specific reset sequence.
It is called when the command \ref DAP_ResetTarget and is for example required
when a device needs a time-critical unlock sequence that enables the debug port.
\return 0 = no device specific reset sequence is implemented.\n
        1 = a device specific reset sequence is implemented.
*/
__STATIC_INLINE uint8_t RESET_TARGET (void) {
  return (0U);             // change to '1' when a device reset sequence is implemented
}

///@}


#endif /* __DAP_CONFIG_H__ */
