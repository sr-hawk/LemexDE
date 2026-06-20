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

#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/mman.h>
#import <LindChain/litehook/litehook.h>
#import "LCMachOUtils.h"
#import "../utils.h"
#import <LindChain/ProcEnvironment/environment.h>
#import <LindChain/ProcEnvironment/syscall.h>

typedef struct {
    uint32_t platform;
    uint32_t version;
} dyld_build_version_t;

uint32_t lcImageIndex = 0;
uint32_t appMainImageIndex = 0;
uint32_t guestAppSdkVersion = 0;
uint32_t guestAppSdkVersionSet = 0;

void* appExecutableHandle = 0;
void overwriteAppExecutableFileType(void);

static inline int translateImageIndex(int origin)
{
    if(origin == lcImageIndex)
    {
        overwriteAppExecutableFileType();
        return appMainImageIndex;
    }
    return origin;
}

DEFINE_HOOK(_dyld_image_count, uint32_t, (void))
{
    return ORIG_FUNC(_dyld_image_count)() - 1;
}

DEFINE_HOOK(_dyld_get_image_header, const struct mach_header*, (uint32_t image_index))
{
    __attribute__((musttail)) return ORIG_FUNC(_dyld_get_image_header)(translateImageIndex(image_index));
}

DEFINE_HOOK(dlsym, void*, (void * __handle,
                           const char * __symbol))
{
    if(__handle == (void*)RTLD_MAIN_ONLY)
    {
        if(strcmp(__symbol, MH_EXECUTE_SYM) == 0)
        {
            overwriteAppExecutableFileType();
            return (void*)ORIG_FUNC(_dyld_get_image_header)(appMainImageIndex);
        }
        __handle = appExecutableHandle;
    }
    else if (__handle != (void*)RTLD_SELF && __handle != (void*)RTLD_NEXT)
    {
        void* ans = ORIG_FUNC(dlsym)(__handle, __symbol);
        if(!ans)
        {
            return 0;
        }
        for(int i = 0; i < gRebindCount; i++)
        {
            global_rebind rebind = gRebinds[i];
            if(ans == rebind.replacee)
            {
                return rebind.replacement;
            }
        }
        return ans;
    }
    
    __attribute__((musttail)) return ORIG_FUNC(dlsym)(__handle, __symbol);
}

DEFINE_HOOK(_dyld_get_image_vmaddr_slide, intptr_t, (uint32_t image_index))
{
    __attribute__((musttail)) return ORIG_FUNC(_dyld_get_image_vmaddr_slide)(translateImageIndex(image_index));
}

DEFINE_HOOK(_dyld_get_image_name, const char*, (uint32_t image_index))
{
    __attribute__((musttail)) return ORIG_FUNC(_dyld_get_image_name)(translateImageIndex(image_index));
}

void refreshFile(const char* path);
DEFINE_HOOK(dlopen, void *, (const char * __path,
                             int __mode))
{
    /* check CS */
    if(!checkCodeSignature(__path))
    {
        /* sign if invalid */
        if((int)environment_syscall(SYS_pectl, PECTL_CS_SIGN_PATH, __path, MACH_PORT_NULL) == 0)
        {
            refreshFile(__path);
        }
    }
    
    /* continue with opening */
    return ORIG_FUNC(dlopen)(__path, __mode);
}

bool hook_dyld_program_sdk_at_least(void* dyldApiInstancePtr,
                                    dyld_build_version_t version)
{
    /* we are targeting ios, so we hard code 2 */
    switch(version.platform)
    {
        case 0xffffffff:
            return version.version <= guestAppSdkVersionSet;
        case 2:
            return version.version <= guestAppSdkVersion;
        default:
            return false;
    }
}

uint32_t hook_dyld_get_program_sdk_version(void* dyldApiInstancePtr)
{
    return guestAppSdkVersion;
}

/*
 * iOS 27 robust dyld-API vtable-slot finder.
 *
 * On iOS 27 Apple changed dyld's codegen: the load following the ADRP can be an
 * LDUR / pre-indexed LDR (not the plain "LDR Xt, [Xn, #imm]" the old scanner
 * required), and on arm64e there are ~20 extra instructions before the real
 * ADRP. The old fixed scanner never matched, walked a bad offset and
 * dereferenced near-null (EXC_BAD_ACCESS at 0x3). The helpers below decode the
 * instructions register-aware and bounds-check every dereference, so a miss
 * degrades to a clean `false` instead of a crash. Ported from
 * CherryFlavoredBleach/LiveContainer (LiveContainer PR #1397), adapted to
 * emexDE's existing aarch64_emulate_adrp_ldr / LCAddressRangeIsReadable.
 */

// Decode "ldr Xt, [Xn{, #imm}]" (unsigned offset, 64-bit). If expectedBaseReg
// is not UINT32_MAX, the base register Xn must match it.
static bool LCDecodeLdrUnsigned64(uint32_t instruction, uint32_t expectedBaseReg, uint32_t *targetReg, uint32_t *offset) {
    if((instruction & 0xFFC00000) != 0xF9400000) {
        return false;
    }

    uint32_t baseReg = (instruction >> 5) & 0x1F;
    if(expectedBaseReg != UINT32_MAX && baseReg != expectedBaseReg) {
        return false;
    }

    if(targetReg) {
        *targetReg = instruction & 0x1F;
    }
    if(offset) {
        *offset = ((instruction >> 10) & 0xFFF) << 3;
    }
    return true;
}

// Decode "ldr Xt, [Xn, #imm]!" (pre-index, 64-bit). offset is signed.
static bool LCDecodeLdrPreIndex64(uint32_t instruction, uint32_t expectedBaseReg, uint32_t *targetReg, int32_t *offset) {
    if((instruction & 0xFFE00C00) != 0xF8400C00) {
        return false;
    }

    uint32_t baseReg = (instruction >> 5) & 0x1F;
    if(expectedBaseReg != UINT32_MAX && baseReg != expectedBaseReg) {
        return false;
    }

    int32_t imm9 = (instruction >> 12) & 0x1FF;
    if(imm9 & 0x100) {
        imm9 |= ~0x1FF;
    }

    if(targetReg) {
        *targetReg = instruction & 0x1F;
    }
    if(offset) {
        *offset = imm9;
    }
    return true;
}

// Decode "movz Xd, #imm{, lsl #shift}".
static bool LCDecodeMovWideImmediate(uint32_t instruction, uint32_t *targetReg, uint64_t *value) {
    if((instruction & 0x7F800000) != 0x52800000) {
        return false;
    }

    uint64_t imm16 = (instruction & 0x1FFFE0) >> 5;
    uint32_t shift = ((instruction >> 21) & 0x3) * 16;
    if(targetReg) {
        *targetReg = instruction & 0x1F;
    }
    if(value) {
        *value = imm16 << shift;
    }
    return true;
}

// Decode "add Xd, Xn, Xm" (64-bit, shifted register form).
static bool LCDecodeAddRegister64(uint32_t instruction, uint32_t *targetReg, uint32_t *leftReg, uint32_t *rightReg) {
    if((instruction & 0xFF200000) != 0x8B000000) {
        return false;
    }

    if(targetReg) {
        *targetReg = instruction & 0x1F;
    }
    if(leftReg) {
        *leftReg = (instruction >> 5) & 0x1F;
    }
    if(rightReg) {
        *rightReg = (instruction >> 16) & 0x1F;
    }
    return true;
}

// Follow up to 4 unconditional "b" stubs at a function entry to the real body.
static uint32_t *LCFollowUnconditionalBranch(uint32_t *baseAddr) {
    uint32_t *target = baseAddr;
    for(int i = 0; i < 4 && LCAddressRangeIsReadable(target, sizeof(uint32_t)); i++) {
        uint32_t instruction = *target;
        if((instruction & 0x7C000000) != 0x14000000) {
            break;
        }

        int32_t imm26 = instruction & 0x03FFFFFF;
        if(imm26 & 0x02000000) {
            imm26 |= ~0x03FFFFFF;
        }
        target += imm26;
    }
    return target;
}

// Scan the dyld-API call stub for the instruction that loads the vtable slot,
// handling both the arm64e (mov-imm + add + ldr / autda) and arm64 (ldr) forms.
// Returns the address of the slot inside the vtable, or NULL.
static void *LCFindDyldApiSlotFromStub(uint32_t *baseAddr, uint32_t scanStart, uint32_t scanEnd, uint32_t instanceReg, void *vtablePtr) {
    bool isVtableReg[32] = { false };
    bool hasImmediate[32] = { false };
    bool hasSlotOffset[32] = { false };
    uint64_t immediateByReg[32] = { 0 };
    uint64_t slotOffsetByReg[32] = { 0 };
    void *fallbackSlot = NULL;

    for(uint32_t i = scanStart; i < scanEnd && LCAddressRangeIsReadable(baseAddr + i, sizeof(uint32_t)); i++) {
        uint32_t instruction = baseAddr[i];
        uint32_t targetReg = 0;
        uint32_t offset = 0;

        if(LCDecodeLdrUnsigned64(instruction, instanceReg, &targetReg, &offset) && offset == 0 && targetReg != 31) {
            isVtableReg[targetReg] = true;
            continue;
        }

        for(uint32_t reg = 0; reg < 32; reg++) {
            if(!isVtableReg[reg]) {
                continue;
            }

            if(LCDecodeLdrUnsigned64(instruction, reg, &targetReg, &offset) && offset != 0) {
                return (uint8_t *)vtablePtr + offset;
            }

            int32_t signedOffset = 0;
            if(LCDecodeLdrPreIndex64(instruction, reg, &targetReg, &signedOffset) && signedOffset > 0) {
                return (uint8_t *)vtablePtr + signedOffset;
            }

            uint32_t addDst = 0;
            uint32_t addSrc = 0;
            uint32_t addImm = 0;
            if(aarch64_emulate_add_imm(instruction, &addDst, &addSrc, &addImm) && addSrc == reg && addImm != 0) {
                fallbackSlot = (uint8_t *)vtablePtr + addImm;
                hasSlotOffset[addDst] = true;
                slotOffsetByReg[addDst] = addImm;
                continue;
            }

            uint32_t addLeft = 0;
            uint32_t addRight = 0;
            if(LCDecodeAddRegister64(instruction, &addDst, &addLeft, &addRight) && addLeft == reg && addRight < 32 && hasImmediate[addRight]) {
                uint64_t slotOffset = immediateByReg[addRight];
                if(slotOffset != 0) {
                    fallbackSlot = (uint8_t *)vtablePtr + slotOffset;
                    hasSlotOffset[addDst] = true;
                    slotOffsetByReg[addDst] = slotOffset;
                }
                continue;
            }
        }

        uint32_t immediateReg = 0;
        uint64_t immediateValue = 0;
        if(LCDecodeMovWideImmediate(instruction, &immediateReg, &immediateValue) && immediateReg != 31) {
            hasImmediate[immediateReg] = true;
            immediateByReg[immediateReg] = immediateValue;
            continue;
        }

        for(uint32_t reg = 0; reg < 32; reg++) {
            if(!hasSlotOffset[reg]) {
                continue;
            }

            if(LCDecodeLdrUnsigned64(instruction, reg, &targetReg, &offset) && offset == 0) {
                return (uint8_t *)vtablePtr + slotOffsetByReg[reg];
            }
        }
    }

    return fallbackSlot;
}

// Validate the ADRP at adrpOffset, find the following register-matching load
// (scanning +1..+4), emulate to the gDyld storage, safely walk
// storage -> instance -> vtable, then locate the API slot. Returns false (no
// crash) on any unreadable pointer or pattern mismatch.
static bool LCFindDyldApiSlotAtAdrpOffset(uint32_t *baseAddr, uint32_t adrpOffset, void **vtableFunctionPtr) {
    if(!LCAddressRangeIsReadable(baseAddr + adrpOffset, sizeof(uint32_t[2]))) {
        return false;
    }

    uint32_t adrpInst = baseAddr[adrpOffset];
    if((adrpInst & 0x9F000000) != 0x90000000) {
        return false;
    }

    uint32_t adrpReg = adrpInst & 0x1F;
    for(uint32_t ldrOffset = adrpOffset + 1; ldrOffset < adrpOffset + 5; ldrOffset++) {
        if(!LCAddressRangeIsReadable(baseAddr + ldrOffset, sizeof(uint32_t))) {
            return false;
        }

        uint32_t instanceReg = 0;
        uint32_t ignoredOffset = 0;
        if(!LCDecodeLdrUnsigned64(baseAddr[ldrOffset], adrpReg, &instanceReg, &ignoredOffset)) {
            continue;
        }

        void *gdyldStorage = (void *)aarch64_emulate_adrp_ldr(adrpInst, baseAddr[ldrOffset], (uint64_t)(baseAddr + adrpOffset));
        void *gdyldInstance = NULL;
        void *vtablePtr = NULL;
        if(!gdyldStorage ||
           !LCReadPointer(gdyldStorage, &gdyldInstance) ||
           !gdyldInstance ||
           !LCReadPointer(gdyldInstance, &vtablePtr) ||
           !vtablePtr) {
            continue;
        }

        void *slot = LCFindDyldApiSlotFromStub(baseAddr, ldrOffset + 1, ldrOffset + 48, instanceReg, vtablePtr);
        if(slot && LCAddressRangeIsReadable(slot, sizeof(void *))) {
            *vtableFunctionPtr = slot;
            return true;
        }
    }

    return false;
}

// Try the caller's preferred ADRP offset, then the arm64e 26.4b1+ "+20" shift,
// then a full 0..96 fallback scan.
static bool LCFindDyldApiSlot(uint32_t *baseAddr, uint32_t preferredAdrpOffset, void **vtableFunctionPtr) {
    uint32_t preferredOffsets[] = { preferredAdrpOffset, preferredAdrpOffset + 20 };
    for(size_t i = 0; i < sizeof(preferredOffsets) / sizeof(preferredOffsets[0]); i++) {
        if(LCFindDyldApiSlotAtAdrpOffset(baseAddr, preferredOffsets[i], vtableFunctionPtr)) {
            return true;
        }
    }

    for(uint32_t i = 0; i < 96; i++) {
        if(i == preferredOffsets[0] || i == preferredOffsets[1]) {
            continue;
        }
        if(LCFindDyldApiSlotAtAdrpOffset(baseAddr, i, vtableFunctionPtr)) {
            NSLog(@"[LC] Found dyld API slot using fallback scan at instruction offset %u", i);
            return true;
        }
    }

    return false;
}

bool performHookDyldApi(const char* functionName, uint32_t adrpOffset, void** origFunction, void* hookFunction) {

    uint32_t* baseAddr = dlsym(RTLD_DEFAULT, functionName);
    if(!baseAddr) {
        NSLog(@"[LC] Failed to find dyld API function %s", functionName);
        return false;
    }
    baseAddr = LCFollowUnconditionalBranch(baseAddr);

    void* vtableFunctionPtr = 0;
    if(!LCFindDyldApiSlot(baseAddr, adrpOffset, &vtableFunctionPtr)) {
        NSLog(@"[LC] Failed to resolve dyld API vtable slot for %s", functionName);
        return false;
    }

    void* currentFunction = NULL;
    if(!LCReadPointer(vtableFunctionPtr, &currentFunction) || !currentFunction) {
        NSLog(@"[LC] Refusing to hook %s because the resolved vtable slot is not readable", functionName);
        return false;
    }

    kern_return_t ret = builtin_vm_protect(mach_task_self(), (mach_vm_address_t)vtableFunctionPtr, sizeof(uintptr_t), false, PROT_READ | PROT_WRITE | VM_PROT_COPY);
    if(ret != KERN_SUCCESS)
    {
        if(!os_tpro_is_supported())
        {
            NSLog(@"[LC] Failed to make dyld API vtable slot writable for %s: %d", functionName, ret);
            return false;
        }
        os_thread_self_restrict_tpro_to_rw();
    }

    if(origFunction != NULL)
    {
        *origFunction = currentFunction;
    }

    *(uint64_t*)vtableFunctionPtr = (uint64_t)hookFunction;
    builtin_vm_protect(mach_task_self(), (mach_vm_address_t)vtableFunctionPtr, sizeof(uintptr_t), false, PROT_READ);
    if(ret != KERN_SUCCESS)
    {
        os_thread_self_restrict_tpro_to_ro();
    }
    return true;
}

void overwriteAppExecutableFileType(void)
{
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        struct mach_header_64 *header = (struct mach_header_64*) orig__dyld_get_image_header(appMainImageIndex);
        kern_return_t kr = builtin_vm_protect(mach_task_self(), (vm_address_t)header, sizeof(header), false, PROT_READ | PROT_WRITE | VM_PROT_COPY);
        if(kr != KERN_SUCCESS)
        {
            return;
        }
        header->filetype = MH_EXECUTE;
        builtin_vm_protect(mach_task_self(), appMainImageIndex, sizeof(struct mach_header), false, PROT_READ);
    });
}

static dyld_build_version_t getDyldImageBuildVersion(const struct mach_header *mh)
{
    dyld_build_version_t result = { .platform = 0xffffffff, .version = 0 };

    assert(mh != NULL);
    
    const uint8_t *ptr = ((const uint8_t *)mh) + sizeof(struct mach_header_64);
    uint32_t ncmds = mh->ncmds;

    for(uint32_t i = 0; i < ncmds; i++)
    {
        const struct load_command *lc = (const struct load_command *)ptr;

        if(lc->cmd == LC_BUILD_VERSION)
        {
            const struct build_version_command *bvc = (const struct build_version_command *)ptr;
            result.platform = bvc->platform;
            result.version  = bvc->sdk;
            return result;
        }
        ptr += lc->cmdsize;
    }

    ptr = ((const uint8_t *)mh) + sizeof(struct mach_header_64);

    for(uint32_t i = 0; i < ncmds; i++)
    {

        const struct load_command *lc = (const struct load_command *)ptr;

        if(lc->cmd == LC_VERSION_MIN_IPHONEOS ||
           lc->cmd == LC_VERSION_MIN_MACOSX)
        {
            const struct version_min_command *vm = (const struct version_min_command *)ptr;
            result.platform = 0xffffffff;
            result.version  = vm->sdk;
            return result;
        }

        ptr += lc->cmdsize;
    }

    return result;
}

void* getGuestAppHeader(void)
{
    return (void*)ORIG_FUNC(_dyld_get_image_header)(appMainImageIndex);
}

bool initGuestSDKVersionInfo(void)
{
    /*
     * SDK-version spoofing is an enhancement, not a launch requirement: a guest
     * compiled on-device against the current SDK runs without it. So every
     * failure path here returns false (the caller logs and continues) instead of
     * aborting the process. This is what stops the second iOS 27 crash, where
     * Apple removed/renamed sVersionMap and the old hard asserts killed the app.
     */
    void* dyldBase = getDyldBase();
    if(!dyldBase) {
        return false;
    }
    /*
     * it seems Apple is constantly changing findVersionSetEquivalent's
     * signature so we directly search sVersionMap instead. Apple renamed the
     * symbol on iOS 27 (dropped the internal-linkage 'L').
     */
    const char* dyldPath = "/usr/lib/dyld";
    uint64_t offset;
    if(@available(iOS 27.0, *)) {
        offset = LCFindSymbolOffset(dyldPath, "__ZN5dyld311sVersionMapE");
    } else {
        offset = LCFindSymbolOffset(dyldPath, "__ZN5dyld3L11sVersionMapE");
    }
    uint32_t *versionMapPtr = dyldBase + offset;

    /*
     * however sVersionMap's struct size is also unknown, but we can figure it out
     * we assume the size is 10K so we won't need to change this line until maybe iOS 40
     */
    uint32_t* versionMapEnd = versionMapPtr + 2560;
    /*
     * Bounds-check before touching versionMapPtr: a missing/renamed symbol makes
     * LCFindSymbolOffset return a garbage offset (it computes result-header, so a
     * NULL result wraps rather than returning 0), which would otherwise be a wild
     * dereference on iOS 27.
     */
    /* ensure the first is versionSet and the third is iOS version (5.0.0) */
    if(!LCAddressRangeIsReadable(versionMapPtr, sizeof(uint32_t[3])) ||
       versionMapPtr[0] != 0x07db0901 || versionMapPtr[2] != 0x00050000) {
        NSLog(@"[LC] sVersionMap not found or layout changed; skipping SDK version spoofing");
        return false;
    }
    /* get struct size. we assume size is smaller then 128. appearently Apple won't have so many platforms */
    uint32_t size = 0;
    for(int i = 1; i < 128; ++i)
    {
        if(!LCAddressRangeIsReadable(versionMapPtr + i, sizeof(uint32_t))) {
            break;
        }
        /* find the next versionSet (for 6.0.0) */
        if(versionMapPtr[i] == 0x07dc0901) {
            size = i;
            break;
        }
    }
    if(!size) {
        NSLog(@"[LC] could not determine sVersionMap stride; skipping SDK version spoofing");
        return false;
    }

    NSOperatingSystemVersion currentVersion = [[NSProcessInfo processInfo] operatingSystemVersion];
    uint32_t maxVersion = ((uint32_t)currentVersion.majorVersion << 16) | ((uint32_t)currentVersion.minorVersion << 8);
    uint32_t candidateVersion = 0;
    uint32_t candidateVersionEquivalent = 0;
    uint32_t newVersionSetVersion = 0;
    for(uint32_t* nowVersionMapItem = versionMapPtr; nowVersionMapItem < versionMapEnd; nowVersionMapItem += size)
    {
        if(!LCAddressRangeIsReadable(nowVersionMapItem, sizeof(uint32_t[3]))) {
            break;
        }
        newVersionSetVersion = nowVersionMapItem[2];
        if(newVersionSetVersion > guestAppSdkVersion)
        {
            break;
        }
        candidateVersion = newVersionSetVersion;
        candidateVersionEquivalent = nowVersionMapItem[0];
        if(newVersionSetVersion >= maxVersion)
        {
            break;
        }
    }
    
    if(newVersionSetVersion == 0xffffffff && candidateVersion == 0)
    {
        candidateVersionEquivalent = newVersionSetVersion;
    }

    guestAppSdkVersionSet = candidateVersionEquivalent;
    
    return true;
}

void DyldHooksInit(void)
{
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        PEEntitlement ownEntitlements = environment_syscall(SYS_getent);
        if(entitlement_got_entitlement(ownEntitlements, PEEntitlementDyldHideLiveProcess))
        {
            int imageCount = _dyld_image_count();
            for(int i = 0; i < imageCount; ++i)
            {
                const struct mach_header* currentImageHeader = _dyld_get_image_header(i);
                if(currentImageHeader->filetype == MH_EXECUTE)
                {
                    lcImageIndex = i;
                    break;
                }
            }
            
            DO_HOOK_GLOBAL(dlsym);
            DO_HOOK_GLOBAL(_dyld_image_count);
            DO_HOOK_GLOBAL(_dyld_get_image_header);
            DO_HOOK_GLOBAL(_dyld_get_image_vmaddr_slide);
            DO_HOOK_GLOBAL(_dyld_get_image_name);
            DO_HOOK_GLOBAL(dlopen);
        }
        
        guestAppSdkVersion = getDyldImageBuildVersion(getGuestAppHeader()).version;
        /*
         * SDK-version spoofing is optional: guests compiled on-device against the
         * current SDK launch without it. On iOS 27, where sVersionMap may be gone
         * or the dyld API slot unresolvable, skip spoofing and continue rather
         * than exit(0)-ing the whole guest.
         */
        if(!initGuestSDKVersionInfo() ||
           !performHookDyldApi("dyld_program_sdk_at_least", 1, NULL, hook_dyld_program_sdk_at_least) ||
           !performHookDyldApi("dyld_get_program_sdk_version", 0, NULL, hook_dyld_get_program_sdk_version))
        {
            NSLog(@"[LC] SDK version spoofing unavailable on this OS; continuing without it");
        }
        return;
    });
}

#pragma mark - Fix black screen
static void *lockPtrToIgnore;
void hook_libdyld_os_unfair_recursive_lock_lock_with_options(void *ptr, void* lock, uint32_t options)
{
    if(!lockPtrToIgnore)
        lockPtrToIgnore = lock;
    if(lock != lockPtrToIgnore)
        os_unfair_recursive_lock_lock_with_options(lock, options);
}
void hook_libdyld_os_unfair_recursive_lock_unlock(void *ptr, void* lock)
{
    if(lock != lockPtrToIgnore)
        os_unfair_recursive_lock_unlock(lock);
}

void *dlopenBypassingLock(const char *path, int mode)
{
    /* this shit made by Duy Tran costs 20~30 ms, making this faster would save those */
    const char *libdyldPath = "/usr/lib/system/libdyld.dylib";
    mach_header_u *libdyldHeader = LCGetLoadedImageHeader(0, libdyldPath);
    assert(libdyldHeader != NULL);
    void **lockUnlockPtr = NULL;
    void **vtableLibSystemHelpers = litehook_find_dsc_symbol(libdyldPath, "__ZTVN5dyld416LibSystemHelpersE");
    void *lockFunc = litehook_find_dsc_symbol(libdyldPath, "__ZNK5dyld416LibSystemHelpers42os_unfair_recursive_lock_lock_with_optionsEP26os_unfair_recursive_lock_s24os_unfair_lock_options_t");
    void *unlockFunc = litehook_find_dsc_symbol(libdyldPath, "__ZNK5dyld416LibSystemHelpers31os_unfair_recursive_lock_unlockEP26os_unfair_recursive_lock_s");
    while(!lockUnlockPtr)
    {
        if(vtableLibSystemHelpers[0] == lockFunc)
        {
            lockUnlockPtr = vtableLibSystemHelpers;
            NSCAssert(vtableLibSystemHelpers[1] == unlockFunc, @"dyld has changed: lock and unlock functions are not next to each other");
            break;
        }
        vtableLibSystemHelpers++;
    }
    kern_return_t ret;
    ret = builtin_vm_protect(mach_task_self(), (mach_vm_address_t)lockUnlockPtr, sizeof(uintptr_t[2]), false, PROT_READ | PROT_WRITE | VM_PROT_COPY);
    assert(ret == KERN_SUCCESS);
    void *origLockPtr = lockUnlockPtr[0], *origUnlockPtr = lockUnlockPtr[1];
    lockUnlockPtr[0] = hook_libdyld_os_unfair_recursive_lock_lock_with_options;
    lockUnlockPtr[1] = hook_libdyld_os_unfair_recursive_lock_unlock;
    void *result = dlopen(path, mode);
    ret = builtin_vm_protect(mach_task_self(), (mach_vm_address_t)lockUnlockPtr, sizeof(uintptr_t[2]), false, PROT_READ | PROT_WRITE);
    assert(ret == KERN_SUCCESS);
    lockUnlockPtr[0] = origLockPtr;
    lockUnlockPtr[1] = origUnlockPtr;
    ret = builtin_vm_protect(mach_task_self(), (mach_vm_address_t)lockUnlockPtr, sizeof(uintptr_t[2]), false, PROT_READ);
    assert(ret == KERN_SUCCESS);
    return result;
}
