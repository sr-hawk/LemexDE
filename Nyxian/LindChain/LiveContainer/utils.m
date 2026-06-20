/*
 SPDX-License-Identifier: AGPL-3.0-or-later

 Copyright (C) 2023 - 2026 LiveContainer
 Copyright (C) 2026 emexlab

 This file is part of LiveContainer.

 LiveContainer is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 LiveContainer is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with Nyxian. If not, see <https://www.gnu.org/licenses/>.
*/

#import <LindChain/LiveContainer/utils.h>
#include <mach/mach.h>

#define ASM(...) __asm__(#__VA_ARGS__)

// Originated from _kernelrpc_mach_vm_protect_trap
ASM(
.global _builtin_vm_protect \n
_builtin_vm_protect:     \n
    mov x16, #-0xe       \n
    svc #0x80            \n
    ret
);

void __assert_rtn(const char* func, const char* file, int line, const char* failedexpr) {
    [NSException raise:NSInternalInconsistencyException format:@"Assertion failed: (%s), file %s, line %d.\n", failedexpr, file, line];
    abort(); // silent compiler warning
}

// https://github.com/pinauten/PatchfinderUtils/blob/master/Sources/CFastFind/CFastFind.c
//
//  CFastFind.c
//  CFastFind
//
//  Created by Linus Henze on 2021-10-16.
//  Copyright © 2021 Linus Henze. All rights reserved.
//

/**
 * Emulate an adrp instruction at the given pc value
 * Returns adrp destination
 */
uint64_t aarch64_emulate_adrp(uint32_t instruction, uint64_t pc) {
    // Check that this is an adrp instruction
    if ((instruction & 0x9F000000) != 0x90000000) {
        return 0;
    }
    
    // Calculate imm from hi and lo
    int32_t imm_hi_lo = (instruction & 0xFFFFE0) >> 3;
    imm_hi_lo |= (instruction & 0x60000000) >> 29;
    if (instruction & 0x800000) {
        // Sign extend
        imm_hi_lo |= 0xFFE00000;
    }
    
    // Build real imm
    int64_t imm = ((int64_t) imm_hi_lo << 12);
    
    // Emulate
    return (pc & ~(0xFFFULL)) + imm;
}

/**
 * Emulate an adrp and ldr instruction at the given pc value
 * Returns destination
 */

uint64_t aarch64_emulate_adrp_ldr(uint32_t instruction, uint32_t ldrInstruction, uint64_t pc) {
    uint64_t adrp_target = aarch64_emulate_adrp(instruction, pc);
    if (!adrp_target) {
        return 0;
    }
    
    if ((instruction & 0x1F) != ((ldrInstruction >> 5) & 0x1F)) {
        return 0;
    }
    
    if ((ldrInstruction & 0xFFC00000) != 0xF9400000) {
        return 0;
    }
    
    uint32_t imm12 = ((ldrInstruction >> 10) & 0xFFF) << 3;
    
    // Emulate
    return adrp_target + (uint64_t) imm12;
}

/**
 * Decode an "add Xd, Xn, #imm" (64-bit, immediate) instruction.
 * Returns false for anything that is not an add-immediate.
 */
bool aarch64_emulate_add_imm(uint32_t instruction, uint32_t *dst, uint32_t *src, uint32_t *imm) {
    // Check that this is an add instruction with immediate
    if ((instruction & 0xFF000000) != 0x91000000) {
        return false;
    }

    uint32_t imm12 = (instruction & 0x3FFC00) >> 10;

    uint8_t shift = (instruction & 0xC00000) >> 22;
    switch (shift) {
        case 0:
            *imm = imm12;
            break;

        case 1:
            *imm = imm12 << 12;
            break;

        default:
            return false;
    }

    *dst = instruction & 0x1F;
    *src = (instruction >> 5) & 0x1F;
    return true;
}

/**
 * Returns true only if the whole [address, address+length) range lives inside a
 * single mapped, readable region. This is what lets the dyld-API scanner probe
 * unknown instruction memory on iOS 27 without faulting (the EXC_BAD_ACCESS at
 * 0x3 the old fixed scanner produced).
 */
bool LCAddressRangeIsReadable(const void *address, size_t length) {
    if(!address || length == 0) {
        return false;
    }

    uintptr_t start = (uintptr_t)address;
    if(start < 0x4000 || UINTPTR_MAX - start < length - 1) {
        return false;
    }

    mach_vm_address_t region = (mach_vm_address_t)start;
    mach_vm_size_t regionLength = 0;
    struct vm_region_submap_short_info_64 info;
    mach_msg_type_number_t infoCount = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
    natural_t depth = 99999;
    kern_return_t kr = vm_region_recurse_64(mach_task_self(), &region, &regionLength, &depth, (vm_region_recurse_info_t)&info, &infoCount);
    if(kr != KERN_SUCCESS || !(info.protection & VM_PROT_READ)) {
        return false;
    }

    uintptr_t end = start + length;
    uintptr_t regionEnd = (uintptr_t)region + (uintptr_t)regionLength;
    return start >= (uintptr_t)region && end <= regionEnd;
}

/**
 * Read a single pointer from address, but only if address is readable.
 */
bool LCReadPointer(const void *address, void **value) {
    if(!value || !LCAddressRangeIsReadable(address, sizeof(void *))) {
        return false;
    }

    *value = *(void * const *)address;
    return true;
}
