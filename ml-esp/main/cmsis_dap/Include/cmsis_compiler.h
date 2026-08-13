/*
 * Copyright (c) 2026 MeguminLink. All rights reserved.
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
 * Project:      CMSIS-DAP Include (ESP32/Xtensa port)
 * Title:        cmsis_compiler.h CMSIS-Compiler (GCC/Xtensa)
 *
 * Minimal GCC/Xtensa implementation of the CMSIS compiler abstraction
 * macros used by CMSIS-DAP. Modeled on the GCC branch of the ARM
 * cmsis_compiler.h from the CMSIS-Core pack; only the macros actually
 * used by the compiled CMSIS-DAP sources are provided.
 *
 *---------------------------------------------------------------------------*/

#ifndef __CMSIS_COMPILER_H__
#define __CMSIS_COMPILER_H__

#include <stdint.h>

#define __ASM                __asm
#define __INLINE             inline
#define __STATIC_INLINE      static inline
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#define __NO_RETURN          __attribute__((__noreturn__))
#define __USED               __attribute__((used))
#define __WEAK               __attribute__((weak))
#define __PACKED             __attribute__((packed))
#define __PACKED_STRUCT      struct __attribute__((packed))
#define __NOP()              __ASM volatile ("nop")

#endif /* __CMSIS_COMPILER_H__ */
