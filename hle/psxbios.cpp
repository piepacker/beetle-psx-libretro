/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/*
 * Internal simulated HLE BIOS.
 */

// TODO: implement all system calls, count the exact CPU cycles of system calls.

#include "psxbios.h"
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>

#if !defined(PSXBIOS_LOG)
#   define PSXBIOS_LOG(...) (printf(__VA_ARGS__), fflush(nullptr))
//#   define PSXBIOS_LOG(...) (void(0))
#endif

#define HLE_FULL                0       // enables full ROM-less HLE support

// HLE exception handler depends on full HLE (everything in the list has to be 1)
#define HLE_ENABLE_EXCEPTION    (HLE_FULL && 0)

// Dev notes:
//  * Tekken 2/3 do not use threads
//  * Tekken 2/3 do not use root counters (rcnt)
//  * Tekken 2/3 do not use the Event system (DeliverEvent, etc)
//  * Tekken 2/3 do not use GPU APIs

#define HLE_ENABLE_HEAP         (HLE_FULL || 1)
#define HLE_ENABLE_FILEIO		(HLE_FULL || 0)        // fileio depends on HLE memcard ?
#define HLE_ENABLE_RCNT			(HLE_FULL || 1)
#define HLE_ENABLE_PAD			(HLE_FULL || 0)
#define HLE_ENABLE_GPU			(HLE_FULL || 1)
#define HLE_ENABLE_MCD			(HLE_FULL || 1)
#define HLE_ENABLE_LOADEXEC		(HLE_FULL || 0)       // depends on ISO9660 filesystem API
#define HLE_ENABLE_THREAD       (HLE_FULL || 1)
#define HLE_ENABLE_ENTRYINT     (HLE_FULL || 0)
#define HLE_ENABLE_EVENT        (HLE_FULL || 1)

// qsort needs to be rewritten before it can be enabled. And once rewritten, probably can remove
// the conditional build for it.. no good reason to disable it except right now it doesn't build --jstine

#define HLE_ENABLE_QSORT        0

static bool hle_config_get_bool(std::string opt) {
    auto woo = "PSX_HLE_CONFIG_" + opt;
    if (const char* const env = getenv(woo.c_str())) {
        auto walk = env;
        while(isspace(int(walk[0]))) ++walk;
        char bval = walk[0];
        while(isspace(int(walk[0]))) ++walk;

        if (!env[0] || env[1] || !isdigit(int(bval))) {
            fprintf(stderr, "env parse error at %s=%s\nValid boolean expressions are 0 or 1\n", woo.c_str(), env);
        }

        if (bval == '0') return 0;
        return 1;       // any non-zero value is boolean true
    }
    return 0;
}

static bool hle_config_env_rcnt		() { return hle_config_get_bool("RCNT"      ); }
static bool hle_config_env_pad		() { return hle_config_get_bool("PAD"       ); }
static bool hle_config_env_fileio	() { return hle_config_get_bool("FILEIO"    ); }
static bool hle_config_env_mcd		() { return hle_config_get_bool("MCD"       ); }
static bool hle_config_env_loadexec	() { return hle_config_get_bool("LOADEXEC"  ); }
static bool hle_config_env_gpu		() { return hle_config_get_bool("GPU"       ); }
static bool hle_config_env_thread   () { return hle_config_get_bool("THREAD"    ); }
static bool hle_config_env_entryint () { return hle_config_get_bool("ENTRYINT"  ); }
static bool hle_config_env_heap     () { return hle_config_get_bool("HEAP"      ); }
static bool hle_config_env_event    () { return hle_config_get_bool("EVENT"     ); }

using u32 = uint32_t;
using s32 = int32_t;
using u16 = uint16_t;
using s16 = int16_t;
using u8  = uint8_t;
using s8  = int8_t;
using uptr = uintptr_t;
using sptr = intptr_t;

struct EXE_HEADER {
    unsigned char id[8];
    u32 text;
    u32 data;
    u32 pc0;
    u32 gp0;
    u32 t_addr;
    u32 t_size;
    u32 d_addr;
    u32 d_size;
    u32 b_addr;
    u32 b_size;
    u32 s_addr;
    u32 s_size;
    u32 SavedSP;
    u32 SavedFP;
    u32 SavedGP;
    u32 SavedRA;
    u32 SavedS0;
};

#define SWAPu32(x) (x)
#define SWAP32(x)  (x)

#undef SysPrintf
#define SysPrintf printf

const char * const biosA0n[256] = {
// 0x00
    "open",		"lseek",	"read",		"write",
    "close",	"ioctl",	"exit",		"sys_a0_07",
    "getc",		"putc",		"todigit",	"atof",
    "strtoul",	"strtol",	"abs",		"labs",
// 0x10
    "atoi",		"atol",		"atob",		"setjmp",
    "longjmp",	"strcat",	"strncat",	"strcmp",
    "strncmp",	"strcpy",	"strncpy",	"strlen",
    "index",	"rindex",	"strchr",	"strrchr",
// 0x20
    "strpbrk",	"strspn",	"strcspn",	"strtok",
    "strstr",	"toupper",	"tolower",	"bcopy",
    "bzero",	"bcmp",		"memcpy",	"memset",
    "memmove",	"memcmp",	"memchr",	"rand",
// 0x30
    "srand",	"qsort",	"strtod",	"malloc",
    "free",		"lsearch",	"bsearch",	"calloc",
    "realloc",	"InitHeap",	"_exit",	"getchar",
    "putchar",	"gets",		"puts",		"printf",
// 0x40
    "sys_a0_40",		"LoadTest",					"Load",		"Exec",
    "FlushCache",		"InstallInterruptHandler",	"GPU_dw",	"mem2vram",
    "SendGPUStatus",	"GPU_cw",					"GPU_cwb",	"SendPackets",
    "sys_a0_4c",		"GetGPUStatus",				"GPU_sync",	"sys_a0_4f",
// 0x50
    "sys_a0_50",		"LoadExec",				"GetSysSp",		"sys_a0_53",
    "_96_init()",		"_bu_init()",			"_96_remove()",	"sys_a0_57",
    "sys_a0_58",		"sys_a0_59",			"sys_a0_5a",	"dev_tty_init",
    "dev_tty_open",		"sys_a0_5d",			"dev_tty_ioctl","dev_cd_open",
// 0x60
    "dev_cd_read",		"dev_cd_close",			"dev_cd_firstfile",	"dev_cd_nextfile",
    "dev_cd_chdir",		"dev_card_open",		"dev_card_read",	"dev_card_write",
    "dev_card_close",	"dev_card_firstfile",	"dev_card_nextfile","dev_card_erase",
    "dev_card_undelete","dev_card_format",		"dev_card_rename",	"dev_card_6f",
// 0x70
    "_bu_init",			"_96_init",		"_96_remove",		"sys_a0_73",
    "sys_a0_74",		"sys_a0_75",	"sys_a0_76",		"sys_a0_77",
    "_96_CdSeekL",		"sys_a0_79",	"sys_a0_7a",		"sys_a0_7b",
    "_96_CdGetStatus",	"sys_a0_7d",	"_96_CdRead",		"sys_a0_7f",
// 0x80
    "sys_a0_80",		"sys_a0_81",	"sys_a0_82",		"sys_a0_83",
    "sys_a0_84",		"_96_CdStop",	"sys_a0_86",		"sys_a0_87",
    "sys_a0_88",		"sys_a0_89",	"sys_a0_8a",		"sys_a0_8b",
    "sys_a0_8c",		"sys_a0_8d",	"sys_a0_8e",		"sys_a0_8f",
// 0x90
    "sys_a0_90",		"sys_a0_91",	"sys_a0_92",		"sys_a0_93",
    "sys_a0_94",		"sys_a0_95",	"AddCDROMDevice",	"AddMemCardDevide",
    "DisableKernelIORedirection",		"EnableKernelIORedirection", "sys_a0_9a", "sys_a0_9b",
    "SetConf",			"GetConf",		"sys_a0_9e",		"SetMem",
// 0xa0
    "_boot",			"SystemError",	"EnqueueCdIntr",	"DequeueCdIntr",
    "sys_a0_a4",		"ReadSector",	"get_cd_status",	"bufs_cb_0",
    "bufs_cb_1",		"bufs_cb_2",	"bufs_cb_3",		"_card_info",
    "_card_load",		"_card_auto",	"bufs_cd_4",		"sys_a0_af",
// 0xb0
    "sys_a0_b0",		"sys_a0_b1",	"do_a_long_jmp",	"sys_a0_b3",
    "?? sub_function",
};

const char * const biosB0n[256] = {
// 0x00
    "SysMalloc",		"sys_b0_01",	"sys_b0_02",	"sys_b0_03",
    "sys_b0_04",		"sys_b0_05",	"sys_b0_06",	"DeliverEvent",
    "OpenEvent",		"CloseEvent",	"WaitEvent",	"TestEvent",
    "EnableEvent",		"DisableEvent",	"OpenTh",		"CloseTh",
// 0x10
    "ChangeTh",			"sys_b0_11",	"InitPAD",		"StartPAD",
    "StopPAD",			"PAD_init",		"PAD_dr",		"ReturnFromExecption",
    "ResetEntryInt",	"HookEntryInt",	"sys_b0_1a",	"sys_b0_1b",
    "sys_b0_1c",		"sys_b0_1d",	"sys_b0_1e",	"sys_b0_1f",
// 0x20
    "UnDeliverEvent",	"sys_b0_21",	"sys_b0_22",	"sys_b0_23",
    "sys_b0_24",		"sys_b0_25",	"sys_b0_26",	"sys_b0_27",
    "sys_b0_28",		"sys_b0_29",	"sys_b0_2a",	"sys_b0_2b",
    "sys_b0_2c",		"sys_b0_2d",	"sys_b0_2e",	"sys_b0_2f",
// 0x30
    "sys_b0_30",		"sys_b0_31",	"open",			"lseek",
    "read",				"write",		"close",		"ioctl",
    "exit",				"sys_b0_39",	"getc",			"putc",
    "getchar",			"putchar",		"gets",			"puts",
// 0x40
    "cd",				"format",		"firstfile",	"nextfile",
    "rename",			"delete",		"undelete",		"AddDevice",
    "RemoteDevice",		"PrintInstalledDevices", "InitCARD", "StartCARD",
    "StopCARD",			"sys_b0_4d",	"_card_write",	"_card_read",
// 0x50
    "_new_card",		"Krom2RawAdd",	"sys_b0_52",	"sys_b0_53",
    "_get_errno",		"_get_error",	"GetC0Table",	"GetB0Table",
    "_card_chan",		"sys_b0_59",	"sys_b0_5a",	"ChangeClearPAD",
    "_card_status",		"_card_wait",
};

const char * const biosC0n[256] = {
// 0x00
    "InitRCnt",			  "InitException",		"SysEnqIntRP",		"SysDeqIntRP",
    "get_free_EvCB_slot", "get_free_TCB_slot",	"ExceptionHandler",	"InstallExeptionHandler",
    "SysInitMemory",	  "SysInitKMem",		"ChangeClearRCnt",	"SystemError",
    "InitDefInt",		  "sys_c0_0d",			"sys_c0_0e",		"sys_c0_0f",
// 0x10
    "sys_c0_10",		  "sys_c0_11",			"InstallDevices",	"FlushStfInOutPut",
    "sys_c0_14",		  "_cdevinput",			"_cdevscan",		"_circgetc",
    "_circputc",		  "ioabort",			"sys_c0_1a",		"KernelRedirect",
    "PatchAOTable",
};

#define HLE_MEDNAFEN_IFC 1
#define HLE_PCSX_IFC     0

#if HLE_PCSX_IFC
#define GPR_ARRAY (psxRegs.GPR)
#define pc0 (PSX_CPU->BACKED_PC)
#define lo  (PSX_CPU->LO)
#define hi  (PSX_CPU->HI)

//#define CP0_EPC      (psxRegs.CP0.n.EPC	  )
//#define CP0_CAUSE    (psxRegs.CP0.n.Cause   )
//#define CP0_STATUS   (psxRegs.CP0.n.Status  )
#endif

#if HLE_MEDNAFEN_IFC
#include "mednafen/psx/psx.h"

#define GPR_ARRAY (PSX_CPU->GPR)
#define pc0 (PSX_CPU->BACKED_PC)
#define lo  (PSX_CPU->LO)
#define hi  (PSX_CPU->HI)

#define CP0_EPC      (PSX_CPU->CP0.EPC	  )
#define CP0_CAUSE    (PSX_CPU->CP0.CAUSE  )
#define CP0_STATUS   (PSX_CPU->CP0.SR     )
#endif

//#define zr (GPR_ARRAY[0])
#define at (GPR_ARRAY[1])
#define v0 (GPR_ARRAY[2])
#define v1 (GPR_ARRAY[3])
#define a0 (GPR_ARRAY[4])
#define a1 (GPR_ARRAY[5])
#define a2 (GPR_ARRAY[6])
#define a3 (GPR_ARRAY[7])
#define t0 (GPR_ARRAY[8])
#define t1 (GPR_ARRAY[9])
#define t2 (GPR_ARRAY[10])
#define t3 (GPR_ARRAY[11])
#define t4 (GPR_ARRAY[12])
#define t5 (GPR_ARRAY[13])
#define t6 (GPR_ARRAY[14])
#define t7 (GPR_ARRAY[15])
#define t8 (GPR_ARRAY[16])
#define t9 (GPR_ARRAY[17])
#define s0 (GPR_ARRAY[18])
#define s1 (GPR_ARRAY[19])
#define s2 (GPR_ARRAY[20])
#define s3 (GPR_ARRAY[21])
#define s4 (GPR_ARRAY[22])
#define s5 (GPR_ARRAY[23])
#define s6 (GPR_ARRAY[24])
#define s7 (GPR_ARRAY[25])
#define k0 (GPR_ARRAY[26])
#define k1 (GPR_ARRAY[27])
#define gp (GPR_ARRAY[28])
#define sp (GPR_ARRAY[29])
#define fp (GPR_ARRAY[30])
#define ra (GPR_ARRAY[31])

static const uint32_t PS1_ICacheSize		= 0x00001000; // 4KB	(instruction cache)
static const uint32_t PS1_RamPhysicalSize	= 0x00200000; // 2MB	(physical)
static const uint32_t PS1_RamMirrorSize		= 0x00800000; // 8MB	(addressable, mirrored)
static const uint32_t PS1_FASTRAMSIZE		= 0x00000400; // 1KB
static const uint32_t PS1_BIOSSIZE			= 0x00080000; // 512KB
static const uint32_t PS1_BIOSRAMSIZE		= 0x00010000; // 512KB
static const uint32_t PS1_SegmentAddrMask	= 0x1fffffff; // masks away all segment information, useful since most emu operations don't need to care

static const uint32_t PS1_FastRamStart		= 0x1f800000;
static const uint32_t PS1_FastRamEnd		= 0x1f800000 + PS1_FASTRAMSIZE;
static const uint32_t PS1_BiosRomStart		= 0x1fc00000;
static const uint32_t PS1_BiosRomEnd		= 0x1fc00000 + PS1_BIOSSIZE;

#define PSX_RAM_START (MainRAM->data8)
#define PSX_ROM_START (BIOSROM->data8)



#if HLE_PCSX_IFC
#define RCNT_SetCount(rid, val)     psxRcntWcount (rid, val)
#define RCNT_SetMode(rid, val)      psxRcntWtarget(rid, val)
#define RCNT_SetTarget(rid, val)    psxRcntWtarget(rid, val)
#define RCNT_GetCount(rid)          psxRcntRcount (rid)
#define RCNT_GetMode(rid)           psxRcntRtarget(rid)
#define RCNT_GetTarget(rid)         psxRcntRtarget(rid)
#endif


#if HLE_MEDNAFEN_IFC
#include "mednafen/psx/timer.h"

#define RCNT_SetCount(rid, val)    TIMER_Write(0, ((rid) << 4) | 0x00, val)
#define RCNT_SetMode(rid, val)     TIMER_Write(0, ((rid) << 4) | 0x04, val)
#define RCNT_SetTarget(rid, val)   TIMER_Write(0, ((rid) << 4) | 0x08, val)
#define RCNT_GetCount(rid)         TIMER_Read (0, ((rid) << 4) | 0x00)
#define RCNT_GetMode(rid)          TIMER_Read (0, ((rid) << 4) | 0x04)
#define RCNT_GetTarget(rid)        TIMER_Read (0, ((rid) << 4) | 0x08)
#endif


// IRQ_Write / IRQ_Read
#if HLE_PCSX_IFC
static void Write_ISTAT(u32 val) { psxHwWrite32(0x1070, val); }
static void Write_IMASK(u32 val) { psxHwWrite32(0x1074, val); }
static u32 Read_ISTAT() { return psxHu32(0x1070); }
static u32 Read_IMASK() { return psxHu32(0x1074); }
#endif

#if HLE_MEDNAFEN_IFC
//  Weird APIs by Mednafen here... They take an address input, but only care about the 4 LSBs.
//  They are meant for accessing 0x1070 (ISTAT) and 0x1074 (IMASK) in the hardware register map.
//  I like to search on 1070 and 1074 in PSX emulators since it's a common pattern when
//  looking for ISTAT and IMASK, so I used those addresses in the function call helpers.. --jstine

// BUGGED? note that IRQ_Write and IRQ_Read as implemented by Mednafen are dodgy.
//   IRQ_Write is missing masking operations on MASK.
//   IRQ_Read is injecting random garbage on writes to unaligned addresses (1071, 1072, etc).
//     (fortunately writes to those addresses are rare or impossible, real HW ignored them --jstine).

static void Write_ISTAT(u32 val) { IRQ_Write(0x1070, val); }
static void Write_IMASK(u32 val) { IRQ_Write(0x1074, val); }
static u32 Read_ISTAT() { return IRQ_Read(0x1070); }
static u32 Read_IMASK() { return IRQ_Read(0x1074); }
#endif

#if HLE_PCSX_IFC
void VmcWriteNV(int port, int slot, const void* src, int size) {
    auto* dest = port ? Mcd2Data : Mcd1Data;
    memcpy(dest + a1 * 128, (uint8_t*)src, 128);
    SaveMcd(port ? Config.Mcd2 : Config.Mcd1, mcd_raw, a1 * 128, 128);
}

#endif

#if HLE_MEDNAFEN_IFC && HLE_ENABLE_MCD
#include "mednafen/psx/frontio.h"
extern FrontIO *PSX_FIO;        // defined by libretro. dunno why this isn't baked into the PSX core for mednafen. --jstine
void VmcWriteNV(int port, int slot, const void* src, int offset, int size) {
    auto mcd = PSX_FIO->GetMemcardDevice(port);
    mcd->WriteNV((const uint8_t*)src, offset, size);
}

void VmcReadNV(int port, int slot, void* dest, int offset, int size) {
    auto mcd = PSX_FIO->GetMemcardDevice(port);
    mcd->ReadNV((uint8_t*)dest, offset, size);
}
#endif


static u8* PSXM(u32 unmasked) {
    auto masked = unmasked & 0x1fff'ffff;

    u8* mem_ = MainRAM->data8;

    if (masked < PS1_RamMirrorSize) {
        return MainRAM->data8 + (masked & (PS1_RamPhysicalSize - 1));
    }
    else if (masked >= 0x1f800000 && masked < (0x1f800000 + PS1_FASTRAMSIZE)) {
        return ScratchRAM->data8 + (masked-0x1f800000);
    }
    else if (masked >= 0x1fc00000 && masked < (0x1fc00000 + PS1_BIOSSIZE)) {
        return BIOSROM->data8 + (masked-0x1fc00000);
    }
    else {
        assert(false);
    }
    //else if (auto* entry = ioHandlers_[HASH_IOADDR(addr)]) {
    //	return entry->ioRead32(addr);
    //}

    return MainRAM->data8 + masked;
}

static u32& psxMu32ref(u32 addr) {
    return (u32&)*PSXM(addr);
}

static u32 psxMu32(u32 addr) {
    return *(u32*)PSXM(addr);
}

#define Ra0 ((char *)PSXM(a0))
#define Ra1 ((char *)PSXM(a1))
#define Ra2 ((char *)PSXM(a2))
#define Ra3 ((char *)PSXM(a3))
#define Rv0 ((char *)PSXM(v0))
#define Rsp ((char *)PSXM(sp))

typedef struct {
    u32 desc;
    s32 status;
    s32 mode;
    u32 fhandler;
} EvCB[32];

#define EvStUNUSED	0x0000
#define EvStWAIT	0x1000
#define EvStACTIVE	0x2000
#define EvStALREADY 0x4000

#define EvMdINTR	0x1000
#define EvMdNOINTR	0x2000

/*
typedef struct {
    s32 next;
    s32 func1;
    s32 func2;
    s32 pad;
} SysRPst;
*/

typedef struct {
    s32 status;
    s32 mode;
    u32 reg[32];
    u32 func;
} TCB;

typedef struct {                   
    u32 _pc0;
    u32 gp0;
    u32 t_addr;
    u32 t_size;
    u32 d_addr;
    u32 d_size;
    u32 b_addr;
    u32 b_size;
    u32 S_addr;
    u32 s_size;
    u32 _sp, _fp, _gp, ret, base;
} EXEC;

struct DIRENTRY {
    char name[20];
    s32 attr;
    s32 size;
    u32 next;
    s32 head;
    char system[4];
};

typedef struct {
    char name[32];
    u32  mode;
    u32  offset;
    u32  size;
    u32  mcfile;
} FileDesc;

#if HLE_ENABLE_ENTRYINT
static u32 *jmp_int = NULL;
static u32 SysIntRP[8];
#endif

#if HLE_ENABLE_PAD
static int *pad_buf = NULL;
static char *pad_buf1 = NULL, *pad_buf2 = NULL;
static int pad_buf1len, pad_buf2len;
#endif

#if HLE_ENABLE_MCD
static int CardState = -1;
static u32 card_active_chan;
#endif

static u32 regs[36];

#if HLE_ENABLE_EVENT
static EvCB *EventCB;
static EvCB *HwEV; // 0xf0
static EvCB *EvEV; // 0xf1
static EvCB *RcEV; // 0xf2
static EvCB *UeEV; // 0xf3
static EvCB *SwEV; // 0xf4
static EvCB *ThEV; // 0xff
#endif

#if HLE_ENABLE_THREAD
static TCB ThreadCB[8];
static int CurThread = 0;
#endif

#if HLE_ENABLE_HEAP
static u32 heap_size = 0;
static u32 *heap_addr = NULL;
static u32 *heap_end = NULL;
#endif

static FileDesc FDesc[32];

// oh silly PCSX. they did the classic VM nono and just recursively called the interpreter
// in order to emulate softCalls. A miracle this ever worked.
// For our purposes in Mednafen, we need to do things properly, which means setting up the
// VM state and then exiting the current C code, and resuming C code (via special hook)
// after the interpreter has done its part.

//int hleSoftCall = FALSE;      // commented out because nesting interpreter is very uncool. --jstine

// Pick an unmapped area of PSX memory to treat as soft call return address.
static const u32 kSoftCallBaseRetAddr = 0x8100'0000;

enum SoftCallReturnId {
    SCRI_None = 0,

    SCRI_DeliverEvent_Resume,

    SCRI_psxBios_DeliverEvent_00,

    SCRI_psxBios__bu_init_00,
    SCRI_psxBios__bu_init_01,
    SCRI_psxBios__bu_init_02,

    SCRI_psxBios__card_load_00,
    SCRI_psxBios__card_info_00,

    SCRI_psxBios__card_read_00,
    SCRI_psxBios__card_write_00,

    SCRI_MAX_COUNT
};

// Proper SoftCall:
//  * save the current value of $ra onto the stack.
//  * set a special return address that identifies our HLE function and it's current yield state
//  * when the return address is detected from CPU, it bounces through HLE and resumes state
//    machine execution -- pops old $ra off the stack.
//
// Using the VM's stack machine is of critical importance to ensure proper handling of thread
// context switching which may occur during open-ended execution of interpreter.

// Pedantic: the PSX expects 16 bytes of shadow space below the current callstack.
//   Mostly things work without this, because it was only meant for use by debug builds to shadow values
//   passd by register ($a0 -> $a4).  --jstine

static void StackPush(u32 val) {
    sp -= 4;
    psxMu32ref(sp) = val;
}

static void StackPop(u32& val) {
    val = psxMu32ref(sp);
    sp += 4;
}

static SoftCallReturnId softCallYield(SoftCallReturnId id, u32 pc) {
    StackPush(ra);
    sp -= 0x10;     // shadow space (see notes earlier)

    // perform equivalent of JAL -- update $ra and set PC.
    ra = kSoftCallBaseRetAddr + (id*16);
    pc0 = pc;
    return id;
}


static void softCallResume() {
    sp += 0x10;
    StackPop(ra);
}

static void HleCallYield(SoftCallReturnId id) {
    StackPush(ra);
    ra = kSoftCallBaseRetAddr + (id*16);
}

static void HleCallResume() {
    StackPop(ra);
}

static bool IsHlePC(u32 pc) {
    return ((pc & 0xff00'0000) == kSoftCallBaseRetAddr);
}

static SoftCallReturnId HleGetCallId(u32 pc) {
    return (SoftCallReturnId)((pc - kSoftCallBaseRetAddr) / 16);
}

static bool HleYieldCheck(SoftCallReturnId id) {
    auto pc = pc0;
    if (IsHlePC(pc)) {
        if (id == SCRI_None) return 0;
        return (int)id == HleGetCallId(pc);
    }
    return 1;
}

#if HLE_ENABLE_EVENT
static void HLEcb_DeliverEvent_Resume() {
    assert (HleYieldCheck(SCRI_DeliverEvent_Resume));
    softCallResume();
    pc0 = ra;
}

static SoftCallReturnId DeliverEventYield(u32 ev, u32 spec) {
    if (EventCB[ev][spec].status != EvStACTIVE) return SCRI_None;

//	EventCB[ev][spec].status = EvStALREADY;
    if (EventCB[ev][spec].mode == EvMdINTR) {
        return softCallYield(SCRI_DeliverEvent_Resume, EventCB[ev][spec].fhandler);
    }
    else {
        EventCB[ev][spec].status = EvStALREADY;
        return SCRI_None;
    }
}
#endif

void psxBios_todigit(void) // 0x0a
{
	int c = a0;
#ifdef PSXBIOS_LOG
	PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x0a]);
#endif
	c &= 0xFF;
	if (c >= 0x30 && c < 0x3A) {
		c -= 0x30;
	}
	else if (c > 0x60 && c < 0x7B) {
		c -= 0x20;
	}
	else if (c > 0x40 && c < 0x5B) {
		c = c - 0x41 + 10;
	}
	else if (c >= 0x80) {
		c = -1;
	}
	else
	{
		c = 0x0098967F;
	}
	v0 = c;
	pc0 = ra;
}

void psxBios_abs() { // 0x0e
    if ((s32)a0 < 0) v0 = -(s32)a0;
    else v0 = a0;
    pc0 = ra;
}

void psxBios_labs() { // 0x0f
    psxBios_abs();
}

void psxBios_atoi() { // 0x10
    s32 n = 0, f = 0;
    char *p = (char *)Ra0;

    for (;;p++) {
        switch (*p) {
            case ' ': case '\t': continue;
            case '-': f++;
            case '+': p++;
        }
        break;
    }

    while (*p >= '0' && *p <= '9') {
        n = n * 10 + *p++ - '0';
    }

    v0 = (f ? -n : n);
    pc0 = ra;
}

void psxBios_atol() { // 0x11
    psxBios_atoi();
}

void psxBios_setjmp() { // 0x13
    u32 *jmp_buf = (u32 *)Ra0;
    int i;

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x13]);

    jmp_buf[0] = ra;
    jmp_buf[1] = sp;
    jmp_buf[2] = fp;
    for (i = 0; i < 8; i++) // s0-s7
        jmp_buf[3 + i] = GPR_ARRAY[16 + i];
    jmp_buf[11] = gp;

    v0 = 0; pc0 = ra;
}

void psxBios_longjmp() { // 0x14
    u32 *jmp_buf = (u32 *)Ra0;
    int i;

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x14]);

    ra = jmp_buf[0]; /* ra */
    sp = jmp_buf[1]; /* sp */
    fp = jmp_buf[2]; /* fp */
    for (i = 0; i < 8; i++) // s0-s7
        GPR_ARRAY[16 + i] = jmp_buf[3 + i];
    gp = jmp_buf[11]; /* gp */

    v0 = a1; pc0 = ra;
}

void psxBios_strcat() { // 0x15
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;

    PSXBIOS_LOG("psxBios_%s: %s, %s\n", biosA0n[0x15], Ra0, Ra1);

	if (!a0 || !a1) {
		v0 = 0;
		pc0 = ra;
		return;
	}

    while (*p1++);
    --p1;
    while ((*p1++ = *p2++) != '\0');

    v0 = a0; pc0 = ra;
}

void psxBios_strncat() { // 0x16
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;
    s32 n = a2;

    PSXBIOS_LOG("psxBios_%s: %s (%x), %s (%x), %d\n", biosA0n[0x16], Ra0, a0, Ra1, a1, a2);

	if (!a0 || !a1) {
		v0 = 0;
		pc0 = ra;
		return;
	}

    while (*p1++);
    --p1;
    while ((*p1++ = *p2++) != '\0') {
        if (--n < 0) {
            *--p1 = '\0';
            break;
        }
    }

    v0 = a0; pc0 = ra;
}

void psxBios_strcmp() { // 0x17
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;

    PSXBIOS_LOG("psxBios_%s: %s (%x), %s (%x)\n", biosA0n[0x17], Ra0, a0, Ra1, a1);

	if (a0 == 0 && a1 == 0)
	{
		v0 = 0;
		pc0 = ra;
		return;
	}
	else if (a0 == 0 && a1 != 0)
	{
		v0 = -1;
		pc0 = ra;
		return;
	}
	else if (a0 != 0 && a1 == 0)
	{
		v0 = 1;
		pc0 = ra;
		return;
	}

	s32 n=0;
	while (*p1 == *p2++) {
		n++;
		if (*p1++ == '\0') {
			v1=n-1;
			a0+=n;
			a1+=n;
			v0 = 0;
			pc0 = ra;
			return;
		}
	}

	v0  = (*p1 - *--p2);
	v1  = n;
	a0 += n;
	a1 += n;
	pc0 = ra;
}

void psxBios_strncmp() { // 0x18

    PSXBIOS_LOG("psxBios_%s: %s (%x), %s (%x), %d\n", biosA0n[0x18], Ra0, a0, Ra1, a1, a2);

	if (a0 == 0 && a1 == 0)
	{
		v0 = 0;
		pc0 = ra;
		return;
	}
	else if (a0 == 0 && a1 != 0)
	{
		v0 = -1;
		pc0 = ra;
		return;
	}
	else if (a0 != 0 && a1 == 0)
	{
		v0 = 1;
		pc0 = ra;
		return;
	}

    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;
    s32 n = a2;
	while (--n >= 0 && *p1 == *p2++) {
		if (*p1++ == '\0') {
			v0 = 0;
			pc0 = ra;
			v1 = a2 - ((a2-n) - 1);
			a0 += (a2-n) - 1;
			a1 += (a2-n) - 1;
			a2 = n;
			return;
		}
	}

	v0 = (n < 0 ? 0 : *p1 - *--p2);
	v1 = a2 - ((a2-n) - 1);
	a0 += (a2-n) - 1;
	a1 += (a2-n) - 1;
	a2 = n;

	pc0 = ra;
}

void psxBios_strcpy() { // 0x19
	if (!a0 || !a1) {
		v0 = 0;
		pc0 = ra;
		return;
	}

    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;
    while ((*p1++ = *p2++) != '\0');

    v0 = a0; pc0 = ra;
}

void psxBios_strncpy() { // 0x1a
	if (!a0 || !a1) {
		v0 = 0;
		pc0 = ra;
		return;
	}

    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;
    s32 n = a2, i;

    for (i = 0; i < n; i++) {
        if ((*p1++ = *p2++) == '\0') {
            while (++i < n) {
                *p1++ = '\0';
            }
            v0 = a0; pc0 = ra;
            return;
        }
    }

    v0 = a0; pc0 = ra;
}

void psxBios_strlen() { // 0x1b
	if (a0) {
        char *p = (char *)Ra0;
        v0 = 0;
        while (*p++) v0++;
    }
    pc0 = ra;
}

void psxBios_index() { // 0x1c
	if (a0) {
        char *p = (char *)Ra0;

        do {
            if (*p == a1) {
                v0 = a0 + (p - (char *)Ra0);
                pc0 = ra;
                return;
            }
        } while (*p++ != '\0');
    }

    v0 = 0; pc0 = ra;
}

void psxBios_rindex() { // 0x1d
	if (a0) {
        char *p = (char *)Ra0;

        v0 = 0;

        do {
            if (*p == a1)
                v0 = a0 + (p - (char *)Ra0);
        } while (*p++ != '\0');
    }

    pc0 = ra;
}

void psxBios_strchr() { // 0x1e
    psxBios_index();
}

void psxBios_strrchr() { // 0x1f
    psxBios_rindex();
}

void psxBios_strpbrk() { // 0x20
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1, *scanp, c, sc;

    while ((c = *p1++) != '\0') {
        for (scanp = p2; (sc = *scanp++) != '\0';) {
            if (sc == c) {
                v0 = a0 + (p1 - 1 - (char *)Ra0);
                pc0 = ra;
                return;
            }
        }
    }

    // BUG: return a0 instead of NULL if not found
    v0 = a0; pc0 = ra;
}

void psxBios_strspn() { // 0x21
    char *p1, *p2;

    for (p1 = (char *)Ra0; *p1 != '\0'; p1++) {
        for (p2 = (char *)Ra1; *p2 != '\0' && *p2 != *p1; p2++);
        if (*p2 == '\0') break;
    }

    v0 = p1 - (char *)Ra0; pc0 = ra;
}

void psxBios_strcspn() { // 0x22
    char *p1, *p2;

    for (p1 = (char *)Ra0; *p1 != '\0'; p1++) {
        for (p2 = (char *)Ra1; *p2 != '\0' && *p2 != *p1; p2++);
        if (*p2 != '\0') break;
    }

    v0 = p1 - (char *)Ra0; pc0 = ra;
}

void psxBios_strtok() { // 0x23
    char *pcA0 = (char *)Ra0;
    char *pcRet = strtok(pcA0, (char *)Ra1);
    if (pcRet)
        v0 = a0 + pcRet - pcA0;
    else
        v0 = 0;
    pc0 = ra;
}

void psxBios_strstr() { // 0x24
    char *p = (char *)Ra0, *p1, *p2;

    while (*p != '\0') {
        p1 = p;
        p2 = (char *)Ra1;

        while (*p1 != '\0' && *p2 != '\0' && *p1 == *p2) {
            p1++; p2++;
        }

        if (*p2 == '\0') {
            v0 = a0 + (p - (char *)Ra0);
            pc0 = ra;
            return;
        }

        p++;
    }

    v0 = 0; pc0 = ra;
}

void psxBios_toupper() { // 0x25
    v0 = (s8)(a0 & 0xff);
    if (v0 >= 'a' && v0 <= 'z') v0 -= 'a' - 'A';
    pc0 = ra;
}

void psxBios_tolower() { // 0x26
    v0 = (s8)(a0 & 0xff);
    if (v0 >= 'A' && v0 <= 'Z') v0 += 'a' - 'A';
    pc0 = ra;
}

void psxBios_bcopy() { // 0x27
    char *p1 = (char *)Ra1, *p2 = (char *)Ra0;
	v0 = a0;
	if (a0 == 0 || a2 > 0x7FFFFFFF)
	{
		pc0 = ra;
		return;
	}
	while ((s32)a2-- > 0) *p1++ = *p2++;
	a2 = 0;
    pc0 = ra;
}

void psxBios_bzero() { // 0x28
    char *p = (char *)Ra0;
  	v0 = a0;
	/* Same as memset here (See memset below) */
	if (a1 > 0x7FFFFFFF || a1 == 0)
	{
		v0 = 0;
		pc0 = ra;
		return;
	}
	else if (a0 == 0)
	{
		pc0 = ra;
		return;
	}
	while ((s32)a1-- > 0) *p++ = '\0';
	a1 = 0;

    pc0 = ra;
}

void psxBios_bcmp() { // 0x29
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;

    if (a0 == 0 || a1 == 0) { v0 = 0; pc0 = ra; return; }

    while ((s32)a2-- > 0) {
        if (*p1++ != *p2++) {
            v0 = *p1 - *p2; // BUG: compare the NEXT byte
            pc0 = ra;
            return;
        }
    }

    v0 = 0; pc0 = ra;
}

void psxBios_memcpy() { // 0x2a
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;
	v0 = a0;
	if (a0 == 0 || a2 > 0x7FFFFFFF)
	{
		pc0 = ra;
		return;
	}
	while ((s32)a2-- > 0) {
		*p1++ = *p2++;
	}
	a2 = 0;
	pc0 = ra;
}

void psxBios_memset() { // 0x2b
	v0 = a0;
	if (a2 > 0x7FFFFFFF || a2 == 0)
	{
		v0 = 0;
		pc0 = ra;
		return;
	}
	if (a0 == 0)
	{
		pc0 = ra;
		return;
	}

    char *p = (char *)Ra0;
    while ((s32)a2-- > 0) *p++ = (char)a1;

    a2 = 0;
    v0 = a0; pc0 = ra;
}

void psxBios_memmove() { // 0x2c
    char *p1 = (char *)Ra0, *p2 = (char *)Ra1;

	v0 = a0;
	if (a0 == 0 || a2 > 0x7FFFFFFF)
	{
		pc0 = ra;
		return;
	}

    if (p2 <= p1 && p2 + a2 > p1) {
        a2++; // BUG: copy one more byte here
        p1 += a2;
        p2 += a2;
        while ((s32)a2-- > 0) *--p1 = *--p2;
    } else {
        while ((s32)a2-- > 0) *p1++ = *p2++;
    }

    pc0 = ra;
}

void psxBios_memcmp() { // 0x2d
    psxBios_bcmp();
}

void psxBios_memchr() { // 0x2e
    char *p = (char *)Ra0;

	if (a0 == 0 || a2 > 0x7FFFFFFF)
	{
		pc0 = ra;
		return;
	}

    while ((s32)a2-- > 0) {
        if (*p++ != (s8)a1) continue;
        v0 = a0 + (p - (char *)Ra0 - 1);
        pc0 = ra;
        return;
    }

    v0 = 0; pc0 = ra;
}

void psxBios_rand() { // 0x2f
    u32 s = psxMu32(0x9010) * 1103515245 + 12345;
    v0 = (s >> 16) & 0x7fff;
    psxMu32ref(0x9010) = SWAPu32(s);
    pc0 = ra;
}

void psxBios_srand() { // 0x30
    psxMu32ref(0x9010) = SWAPu32(a0);
    pc0 = ra;
}

#if HLE_ENABLE_QSORT

// qsort implementation requires implementing a yieldable/iterable algorthm with all qsort state 
// information allocated via the PSX Stackframe. --jstine

static u32 qscmpfunc, qswidth;

template<int call_base>
static inline int qscmp(int call_id, char *a, char *b) {

    auto sa0 = a0;
    a0 = sa0 + (a - (char *)PSXM(sa0));
    a1 = sa0 + (b - (char *)PSXM(sa0));

    StackPush(ra);
    StackPush(a0);      // PCSX impl of HLE preserves a0, though this seems weird to me --jstine
    softCall(qscmpfunc);
    StackPop(a0);
    StackPop(ra);

    return (s32)v0;
}

static inline void qexchange(char *i, char *j) {
    char t;
    int n = qswidth;

    do {
        t = *i;
        *i++ = *j;
        *j++ = t;
    } while (--n);
}

static inline void q3exchange(char *i, char *j, char *k) {
    char t;
    int n = qswidth;

    do {
        t = *i;
        *i++ = *k;
        *k++ = *j;
        *j++ = t;
    } while (--n);
}

static void qsort_main(char *a, char *l) {
    char *i, *j, *lp, *hp;
    int c;
    unsigned int n;

start:
    if ((n = l - a) <= qswidth)
        return;
    n = qswidth * (n / (2 * qswidth));
    hp = lp = a + n;
    i = a;
    j = l - qswidth;
    while (TRUE) {
        if (i < lp) {
            if ((c = qscmp(i, lp)) == 0) {
                qexchange(i, lp -= qswidth);
                continue;
            }
            if (c < 0) {
                i += qswidth;
                continue;
            }
        }

loop:
        if (j > hp) {
            if ((c = qscmp(hp, j)) == 0) {
                qexchange(hp += qswidth, j);
                goto loop;
            }
            if (c > 0) {
                if (i == lp) {
                    q3exchange(i, hp += qswidth, j);
                    i = lp += qswidth;
                    goto loop;
                }
                qexchange(i, j);
                j -= qswidth;
                i += qswidth;
                continue;
            }
            j -= qswidth;
            goto loop;
        }

        if (i == lp) {
            if (lp - a >= l - hp) {
                qsort_main(hp + qswidth, l);
                l = lp;
            } else {
                qsort_main(a, lp);
                a = hp + qswidth;
            }
            goto start;
        }

        q3exchange(j, lp -= qswidth, i);
        j = hp -= qswidth;
    }
}

void psxBios_qsort() { // 0x31
    StackReserve();

    qswidth = a2;
    qscmpfunc = a3;
    qsort_main((char *)Ra0, (char *)Ra0 + (a1 * a2));

    StackRelease();
    pc0 = ra;
}
#endif

#if HLE_ENABLE_HEAP
void psxBios_malloc() { // 0x33
    unsigned int *chunk, *newchunk = NULL;
    unsigned int dsize = 0, csize, cstat;
    int colflag;
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x33]);

	if (!a0 || (!heap_size || !heap_addr)) {
		v0 = 0;
		pc0 = ra;
		return;
	}

	// scan through heap and combine free chunks of space
	chunk = heap_addr;
	colflag = 0;
	while(chunk < heap_end) {
		// get size and status of actual chunk
		csize = ((u32)*chunk) & 0xfffffffc;
		cstat = ((u32)*chunk) & 1;

		// most probably broken heap descriptor
		// this fixes Burning Road
		if (*chunk == 0) {
			newchunk = chunk;
			dsize = ((sptr)heap_end - (sptr)chunk) - 4;
			colflag = 1;
			break;
		}

		// it's a free chunk
		if(cstat == 1) {
			if(colflag == 0) {
				newchunk = chunk;
				dsize = csize;
				colflag = 1;			// let's begin a new collection of free memory
			}
			else dsize += (csize+4);	// add the new size including header
		}
		// not a free chunk: did we start a collection ?
		else {
			if(colflag == 1) {			// collection is over
				colflag = 0;
				*newchunk = SWAP32(dsize | 1);
			}
		}

		// next chunk
		chunk = (u32*)((uptr)chunk + csize + 4);
	}
	// if neccessary free memory on end of heap
	if (colflag == 1)
		*newchunk = SWAP32(dsize | 1);

	chunk = heap_addr;
	csize = ((u32)*chunk) & 0xfffffffc;
	cstat = ((u32)*chunk) & 1;
	dsize = (a0 + 3) & 0xfffffffc;

	// exit on uninitialized heap
	if (chunk == NULL) {
		printf("malloc %x,%x: Uninitialized Heap!\n", v0, a0);
		v0 = 0;
		pc0 = ra;
		return;
	}

	// search an unused chunk that is big enough until the end of the heap
	while ((dsize > csize || cstat==0) && chunk < heap_end ) {
		chunk = (u32*)((sptr)chunk + csize + 4);

			// catch out of memory
			if(chunk >= heap_end) {
				printf("malloc %x,%x: Out of memory error!\n",
					v0, a0);
				v0 = 0; pc0 = ra;
				return;
			}

		csize = ((u32)*chunk) & 0xfffffffc;
		cstat = ((u32)*chunk) & 1;
	}

	// allocate memory
	if(dsize == csize) {
		// chunk has same size
		*chunk &= 0xfffffffc;
	} else if (dsize > csize) {
		v0 = 0; pc0 = ra;
		return;
	} else {
		// split free chunk
		*chunk = SWAP32(dsize);
		newchunk = (u32*)((sptr)chunk + dsize + 4);
		*newchunk = SWAP32(((csize - dsize - 4) & 0xfffffffc) | 1);
	}

	// return pointer to allocated memory
	v0 = ((sptr)chunk - (sptr)PSX_RAM_START) + 4;
	v0|= 0x80000000;
	//printf ("malloc %x,%x\n", v0, a0);
	pc0 = ra;
}

void psxBios_free() { // 0x34

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x34]);

    SysPrintf("free %x: %x bytes\n", a0, *(u32*)(Ra0-4));

    if (a0) {
        *(u32*)(Ra0-4) |= 1;	// set chunk to free
    }
    pc0 = ra;
}

void psxBios_calloc() { // 0x37
    void *pv0;
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x37]);

    a0 = a0 * a1;
    psxBios_malloc();
    pv0 = Rv0;
    if (pv0)
        memset(pv0, 0, a0);
}

void psxBios_realloc() { // 0x38
    u32 block = a0;
    u32 size = a1;

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x38]);

    a0 = block;
	/* If "old_buf" is zero, executes malloc(new_size), and returns r2=new_buf (or 0=failed). */
	if (block == 0)
	{
		psxBios_malloc();
	}
	/* Else, if "new_size" is zero, executes free(old_buf), and returns r2=garbage. */
	else if (size == 0)
	{
		psxBios_free();
	}
	/* Else, executes malloc(new_size), bcopy(old_buf,new_buf,new_size), and free(old_buf), and returns r2=new_buf (or 0=failed). */
	/* Note that it is not quite implemented this way here. */
	else
	{
		psxBios_free();
		a0 = size;
		psxBios_malloc();
	}
}


/* InitHeap(void *block , int n) */
void psxBios_InitHeap() { // 0x39
    unsigned int size;

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x39]);

    if (((a0 & 0x1fffff) + a1)>= 0x200000) size = 0x1ffffc - (a0 & 0x1fffff);
    else size = a1;

    size &= 0xfffffffc;

    heap_addr = (u32 *)Ra0;
	heap_size = size;
	heap_end = (u32 *)((u8 *)heap_addr + heap_size);
	/* HACKFIX: Commenting out this line fixes GTA2 crash */
	//*heap_addr = SWAP32(size | 1);

    SysPrintf("InitHeap %x,%x : %x %x\n",a0,a1, (int)((uptr)heap_addr-(uptr)PSX_RAM_START), size);

    pc0 = ra;
}
#endif

// stdoutbuf is not strictly necessary - we chould use native putc instead.
// But it can be helpful as a rule, especially if the emulator becomes threaded/asyncronous later.
// It keeps PSX stdout from corrupting logging coming from other threads.
//
// currently not handled by savesatate but also only user-facing (won't affect state determininism)
// Recommended savestate behavior is to simply ensure this is initialized to 0.
static std::string stdoutbuf;

static void raw_putc(char c) {
    if (c == '\n') {
        if (!stdoutbuf.empty()) {
            SysPrintf("STDOUT: %s\n", stdoutbuf.c_str());
        }

        stdoutbuf.clear();
    }
    else {
        if (c != '\r') {
            stdoutbuf += c;
        }
    }
}

static void raw_puts(const char* cstr) {
    if (!cstr) return;

    if (!cstr[0]) {
        // game putting out an intentionally-blank line
        SysPrintf("STDOUT: \n");
        return;
    }
    for(const char* eol = cstr; eol[0]; ++eol) {
        if (eol[0] == '\r') {
            // ignore 'em
        }
        else if (eol[0] == '\n') {
            SysPrintf("STDOUT: %s\n", stdoutbuf.c_str());
            stdoutbuf.clear();
        }
        else {
            stdoutbuf += eol[0];
        }
    }
}

void psxBios_getchar() { //0x3b
    v0 = getchar();
    pc0 = ra;
}

void psxBios_putchar() { // 3d
    raw_putc(a0);
    pc0 = ra;
}

void psxBios_puts() { // 3e/3f
    raw_puts(Ra0);
    pc0 = ra;
}

void psxBios_printf() { // 0x3f
    const int t2len = 64;
    char tmp2[t2len];
    char ptmp[512];      // FIXME: remove this and replace with StringUtila nd format directly into std string.
    u32 save[4];
    int n=1, i=0, j;
    void *psp;

    psp = PSXM(sp);
    if (psp) {
        memcpy(save, psp, 4 * 4);
        psxMu32ref(sp + 0 ) = SWAP32((u32)a0);
        psxMu32ref(sp + 4 ) = SWAP32((u32)a1);
        psxMu32ref(sp + 8 ) = SWAP32((u32)a2);
        psxMu32ref(sp + 12) = SWAP32((u32)a3);
    }

    const char* fmt = Ra0;

    while (fmt[i]) {
        if (fmt[i] == '%') {
            if (fmt[i+1] == '%') {
                raw_putc('%');
                ++i; continue;
            }

            j = 0;
            tmp2[j++] = '%';
_start:
            // safeguiard - if things run past the end of the buffer, give up.
            if (j > t2len-2) {
                tmp2[j] = 0;
                //raw_puts(tmp2);
                fprintf(stderr, "Funky printf formatting at 0x%06x, msg=%s\n", a0 & 0x1fff'ffff, fmt);
                continue;   // resume processing.
            }

            switch (fmt[++i]) {
                case '.':
                case 'l': {
                    tmp2[j++] = fmt[i];
                } goto _start;

                default: {
                    if (fmt[i] >= '0' && fmt[i] <= '9') {
                        tmp2[j++] = fmt[i];
                        goto _start;
                    }
                } break;
            }
            tmp2[j++] = fmt[i];
            tmp2[j] = 0;

            switch (fmt[i]) {
                case 'f': case 'F':
                    snprintf(ptmp, sizeof(ptmp), tmp2, (float)psxMu32(sp + n * 4)); n++; 
                    raw_puts(ptmp);
                break;

                case 'a': case 'A':
                case 'e': case 'E':
                case 'g': case 'G':
                    snprintf(ptmp, sizeof(ptmp), tmp2, (double)psxMu32(sp + n * 4)); n++;
                    raw_puts(ptmp);
                break;

                case 'p':
                case 'i': case 'u':
                case 'd': case 'D':
                case 'o': case 'O':
                case 'x': case 'X':
                    snprintf(ptmp, sizeof(ptmp), tmp2, (unsigned int)psxMu32(sp + n * 4)); n++;
                    raw_puts(ptmp);
                break;

                case 'c':
                    raw_putc(psxMu32(sp + n * 4)); n++;
                break;

                case 's':
                    raw_puts((char*)PSXM(psxMu32(sp + n * 4))); n++;
                break;

                case '%':
                    fprintf(stderr, "Funky printf formatting at 0x%06x, msg=%s\n", a0 & 0x1fff'ffff, fmt);
                break;
            }
            i++;
        }
        else {
            raw_putc(fmt[i++]);
        }
    }

    if (psp)
        memcpy(psp, save, 4 * 4);

    pc0 = ra;
}

#if 0
void psxBios_format() { // 0x41
    if (strcmp(Ra0, "bu00:") == 0 && Config.Mcd1[0] != '\0')
    {
        CreateMcd(Config.Mcd1);
        LoadMcd(1, Config.Mcd1);
        v0 = 1;
    }
    else if (strcmp(Ra0, "bu10:") == 0 && Config.Mcd2[0] != '\0')
    {
        CreateMcd(Config.Mcd2);
        LoadMcd(2, Config.Mcd2);
        v0 = 1;
    }
    else
    {
        v0 = 0;
    }
    pc0 = ra;
}
#endif

/*
 *	long Load(char *name, struct EXEC *header);
 */

#if HLE_ENABLE_LOADEXEC
void psxBios_Load() { // 0x42
    EXE_HEADER eheader;

    PSXBIOS_LOG("psxBios_%s: %s, %x\n", biosA0n[0x42], Ra0, a1);

    void* pa1 = Ra1;
    if (pa1 && LoadCdromFile(Ra0, &eheader) == 0) {
        memcpy(pa1, ((char*)&eheader)+16, sizeof(EXEC));
        v0 = 1;
    } else v0 = 0;

    pc0 = ra;
}
#endif

/*
 *	int Exec(struct EXEC *header , int argc , char **argv);
 */

void psxBios_Exec() { // 43
    EXEC *header = (EXEC*)Ra0;
    u32 tmp;

    PSXBIOS_LOG("psxBios_%s: %x, %x, %x\n", biosA0n[0x43], a0, a1, a2);

    header->_sp = sp;
    header->_fp = fp;
    header->_sp = sp;
    header->_gp = gp;
    header->ret = ra;
    header->base = s0;

    if (header->S_addr != 0) {
        tmp = header->S_addr + header->s_size;
        sp = tmp;
        fp = sp;
    }

    gp = header->gp0;

    s0 = a0;

    a0 = a1;
    a1 = a2;

    ra = 0x8000;
    pc0 = header->_pc0;
}

void psxBios_FlushCache() { // 44
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x44]);

    pc0 = ra;
}


#if HLE_PCSX_IFC
#define GPU_W_DATA(dat)     GPU_writeData  (dat)
#define GPU_W_STATUS(dat)   GPU_writeStatus(dat)
#define GPU_R_STATUS()      GPU_readStatus ()
#define DMA_W(addr, val)    psxHwWrite32(addr, val)
#define DMA_R(addr)         psxHwRead32 (addr)
#endif

#if HLE_MEDNAFEN_IFC
#define GPU_W_DATA(dat)     GPU_Write(0, 0, dat)
#define GPU_W_STATUS(dat)   GPU_Write(0, 4, dat)
#define GPU_R_STATUS()      GPU_Read(0, 4)
#define DMA_W(addr, val)    DMA_Write(0, addr, val)
#define DMA_R(addr)         DMA_Read (0, addr)
#endif

#if HLE_ENABLE_GPU
void psxBios_GPU_dw() { // 0x46
    int size;
    s32 *ptr;

    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x46]);

    GPU_W_DATA(0xa0000000);
    GPU_W_DATA((a1<<16)|(a0&0xffff));
    GPU_W_DATA((a3<<16)|(a2&0xffff));
    size = (a2*a3+1)/2;
    ptr = (s32*)PSXM(Rsp[4]);  //that is correct?
    do {
        GPU_W_DATA(SWAP32(*ptr));
        ptr++;
    } while(--size);

    pc0 = ra;
}  

void psxBios_mem2vram() { // 0x47
    int size;

    GPU_W_DATA(0xa0000000);
    GPU_W_DATA((a1<<16)|(a0&0xffff));
    GPU_W_DATA((a3<<16)|(a2&0xffff));
    size = (a2*a3+1)/2;
    GPU_W_STATUS(0x04000002);

    DMA_W(0x1f8010f4,0);
    DMA_W(0x1f8010f0, DMA_R(0x1f8010f0)|0x800);
    DMA_W(0x1f8010a0, Rsp[4]);//might have a buggy...
    DMA_W(0x1f8010a4, ((size/16)<<16)|16);
    DMA_W(0x1f8010a8, 0x01000201);

    pc0 = ra;
}

void psxBios_SendGPU() { // 0x48
    GPU_W_STATUS(a0);
    //gpuSyncPluginSR();
    pc0 = ra;
}

void psxBios_GPU_cw() { // 0x49
    GPU_W_DATA(a0);
    pc0 = ra;
}

void psxBios_GPU_cwb() { // 0x4a
    s32 *ptr = (s32*)Ra0;
    int size = a1;
    while(size--) {
        GPU_W_DATA(SWAP32(*ptr));
        ptr++;
    }

    pc0 = ra;
}
   
void psxBios_GPU_SendPackets() { //4b:	
    GPU_W_STATUS(0x04000002);
    DMA_W(0x1f8010f4, 0);
    DMA_W(0x1f8010f0, DMA_R(0x1f8010f0)|0x800);
    DMA_W(0x1f8010a0, a0);
    DMA_W(0x1f8010a4, 0);
    DMA_W(0x1f8010a8, 0x010000401);
    pc0 = ra;
}

void psxBios_sys_a0_4c() { // 0x4c GPU relate
    DMA_W(0x1f8010a8,0x00000401);
    GPU_W_DATA(0x0400000);
    GPU_W_DATA(0x0200000);
    GPU_W_DATA(0x0100000);

    pc0 = ra;
}

void psxBios_GPU_GetGPUStatus() { // 0x4d
    v0 = GPU_R_STATUS();
    pc0 = ra;
}
#endif

#if 0
void psxBios_LoadExec() { // 51
    EXEC *header = (EXEC*)PSXM(0xf000);
    u32 s_addr, s_size;

    PSXBIOS_LOG("psxBios_%s: %s: %x,%x\n", biosA0n[0x51], Ra0, a1, a2);
    s_addr = a1; s_size = a2;

    a1 = 0xf000;	
    psxBios_Load();

    header->S_addr = s_addr;
    header->s_size = s_size;

    a0 = 0xf000; a1 = 0; a2 = 0;
    psxBios_Exec();
}
#endif

#if HLE_ENABLE_EVENT
void psxBios__bu_init() { // 70
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x70]);

    // Impl Note: honestly this would be easier rewritten as MIPS assembly.
    //   It's just two JALs, but in order to maintain stack-engine state info at the HLE 
    //   level, we need to implement a ton of paperwork that the interpreter would simply
    //   handle for us via its own MIPS state machine. --jstine

    if (HleYieldCheck(SCRI_None)) {
        HleCallYield(SCRI_psxBios__bu_init_00);
        if (DeliverEventYield(0x11, 0x2)) { // 0xf0000011, 0x0004
            return;
        }
        pc0 = ra;       // allows fallthrough to next HleYield state
    }

    if (HleYieldCheck(SCRI_psxBios__bu_init_00)) {
        HleCallResume();
        HleCallYield(SCRI_psxBios__bu_init_01);
        if (DeliverEventYield(0x81, 0x2)) { // 0xf4000001, 0x0004
            return;
        }
        pc0 = ra;
    }

    if (HleYieldCheck(SCRI_psxBios__bu_init_01)) {
        HleCallResume();
        pc0 = ra;
    }
    else {
        assert(false);
    }
}
#endif

void psxBios__96_init() { // 71
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x71]);

    pc0 = ra;
}

void psxBios__96_remove() { // 72
    PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x72]);

    pc0 = ra;
}

#if 0
void psxBios_SetMem() { // 9f
    u32 nx = psxHu32(0x1060);

    PSXBIOS_LOG("psxBios_%s: %x, %x\n", biosA0n[0x9f], a0, a1);

    switch(a0) {
        case 2:
            psxHu32ref(0x1060) = SWAP32(nx);
            psxMu32ref(0x060) = a0;
            SysPrintf("Change effective memory : %d MBytes\n",a0);
            break;

        case 8:
            psxHu32ref(0x1060) = SWAP32(nx | 0x300);
            psxMu32ref(0x060) = a0;
            SysPrintf("Change effective memory : %d MBytes\n",a0);
    
        default:
            SysPrintf("Effective memory must be 2/8 MBytes\n");
        break;
    }

    pc0 = ra;
}
#endif

#if HLE_ENABLE_EVENT && HLE_ENABLE_MCD
void psxBios__card_info() { // ab
    PSXBIOS_LOG("psxBios_%s: %x\n", biosA0n[0xab], a0);

    card_active_chan = a0;

//	DeliverEvent(0x11, 0x2); // 0xf0000011, 0x0004
    
    if (HleYieldCheck(SCRI_None)) {
        HleCallYield(SCRI_psxBios__card_info_00);
        if (DeliverEventYield(0x81, 0x2)) { // 0xf4000001, 0x0004
            return;
        }
        pc0 = ra;
    }

    if (HleYieldCheck(SCRI_psxBios__card_info_00)) {
        HleCallResume();
        v0 = 1;
        pc0 = ra;
    }
    else {
        assert(false);
    }
}

void psxBios__card_load() { // ac
    PSXBIOS_LOG("psxBios_%s: %x\n", biosA0n[0xac], a0);

    card_active_chan = a0;

//	DeliverEvent(0x11, 0x2); // 0xf0000011, 0x0004

    if (HleYieldCheck(SCRI_None)) {
        HleCallYield(SCRI_psxBios__card_load_00);
        if (DeliverEventYield(0x81, 0x2)) { // 0xf4000001, 0x0004
            return;
        }
        pc0 = ra;
    }

    if (HleYieldCheck(SCRI_psxBios__card_load_00)) {
        HleCallResume();
        v0 = 1;
        pc0 = ra;
    }
    else {
        assert(false);
    }
}
#endif

/* System calls B0 */

#if HLE_ENABLE_RCNT
void psxBios_SetRCnt() { // 02
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x02]);

    a0&= 0x3;
    if (a0 != 3) {
        u32 mode=0;

        if (a2&0x1000) mode|= 0x050; // Interrupt Mode
        if (a2&0x0100) mode|= 0x008; // Count to 0xffff
        if (a2&0x0010) mode|= 0x001; // Timer stop mode
        if (a0 == 2) { if (a2&0x0001) mode|= 0x200; } // System Clock mode
        else         { if (a2&0x0001) mode|= 0x100; } // System Clock mode

        RCNT_SetMode (a0, mode);
        RCNT_SetCount(a0, a1);
    }
    pc0 = ra;
}

void psxBios_GetRCnt() { // 03
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x03]);

    a0&= 0x3;
    v0 = 0;
    if (a0 < 3) v0 = RCNT_GetCount(a0);
    pc0 = ra;
}

void psxBios_StartRCnt() { // 04
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x04]);

    a0&= 0x3;

    auto imask = Read_IMASK();
    if (a0 != 3) { imask |= SWAP32((u32)((1<<(a0+4)))); }
    else         { imask |= SWAPu32(0x1); }
    Write_IMASK(imask);

    v0 = 1;
    pc0 = ra;
}

void psxBios_StopRCnt() { // 05
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x05]);

    a0&= 0x3;

    auto imask = Read_IMASK();
    if (a0 != 3) { imask &= ~SWAP32((u32)((1<<(a0+4)))); }
    else         { imask &= ~SWAPu32(0x1); }
    Write_IMASK(imask);

    pc0 = ra;
}

void psxBios_ResetRCnt() { // 06
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x06]);

    a0&= 0x3;
    if (a0 != 3) {
        RCNT_SetCount (a0, 0);
        RCNT_SetMode  (a0, 0);
        RCNT_SetTarget(a0, 0);
    }
    pc0 = ra;
}

void psxBios_ChangeClearRCnt() { // 0a
    u32 *ptr;

    PSXBIOS_LOG("psxBios_%s: %x, %x\n", biosC0n[0x0a], a0, a1);

    ptr = (u32*)PSXM((a0 << 2) + 0x8600);
    v0 = *ptr;
    *ptr = a1;

//	psxRegs.CP0.n.Status|= 0x404;
    pc0 = ra;
}
#endif


#if HLE_ENABLE_EVENT
/* gets ev for use with Event */
#define GetEv() \
    ev = (a0 >> 24) & 0xf; \
    if (ev == 0xf) ev = 0x5; \
    ev*= 32; \
    ev+= a0&0x1f;

/* gets spec for use with Event */
#define GetSpec() \
    spec = 0; \
    switch (a1) { \
        case 0x0301: spec = 16; break; \
        case 0x0302: spec = 17; break; \
        default: \
            for (i=0; i<16; i++) if (a1 & (1 << i)) { spec = i; break; } \
            break; \
    }

void psxBios_DeliverEvent() { // 07
    int ev, spec;
    int i;

    GetEv();
    GetSpec();

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x07], ev, spec);

    HleCallYield(SCRI_psxBios_DeliverEvent_00);
    if (DeliverEventYield(ev, spec)) {
        return;
    }

    if (HleYieldCheck(SCRI_psxBios_DeliverEvent_00)) {
        HleCallResume();
        pc0 = ra;
    }
    else {
        assert(false);
    }
}

void psxBios_OpenEvent() { // 08
    int ev, spec;
    int i;

    GetEv();
    GetSpec();

    PSXBIOS_LOG("psxBios_%s %x,%x (class:%x, spec:%x, mode:%x, func:%x)\n", biosB0n[0x08], ev, spec, a0, a1, a2, a3);

    EventCB[ev][spec].status = EvStWAIT;
    EventCB[ev][spec].mode = a2;
    EventCB[ev][spec].fhandler = a3;

    v0 = ev | (spec << 8);
    pc0 = ra;
}

void psxBios_CloseEvent() { // 09
    int ev, spec;

    ev   = a0 & 0xff;
    spec = (a0 >> 8) & 0xff;

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x09], ev, spec);

    EventCB[ev][spec].status = EvStUNUSED;

    v0 = 1; pc0 = ra;
}

void psxBios_WaitEvent() { // 0a
    int ev, spec;

    ev   = a0 & 0xff;
    spec = (a0 >> 8) & 0xff;

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x0a], ev, spec);

    assert(spec < 32);

    EventCB[ev][spec].status = EvStACTIVE;

    v0 = 1; pc0 = ra;
}

void psxBios_TestEvent() { // 0b
    int ev, spec;

    ev   = a0 & 0xff;
    spec = (a0 >> 8) & 0xff;

    if (EventCB[ev][spec].status == EvStALREADY) {
        EventCB[ev][spec].status = EvStACTIVE; v0 = 1;
    } else v0 = 0;

    PSXBIOS_LOG("psxBios_%s %x,%x: %x\n", biosB0n[0x0b], ev, spec, v0);

    pc0 = ra;
}

void psxBios_EnableEvent() { // 0c
    int ev, spec;

    ev   = a0 & 0xff;
    spec = (a0 >> 8) & 0xff;

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x0c], ev, spec);

    EventCB[ev][spec].status = EvStACTIVE;

    v0 = 1; pc0 = ra;
}

void psxBios_DisableEvent() { // 0d
    int ev, spec;

    ev   = a0 & 0xff;
    spec = (a0 >> 8) & 0xff;

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x0d], ev, spec);

    EventCB[ev][spec].status = EvStWAIT;

    v0 = 1; pc0 = ra;
}
#endif


#if HLE_ENABLE_THREAD
/*
 *	long OpenTh(long (*func)(), unsigned long sp, unsigned long gp);
 */

void psxBios_OpenTh() { // 0e
    int th;

    for (th=1; th<8; th++)
        if (ThreadCB[th].status == 0) break;

    PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x0e], th);

    ThreadCB[th].status = 1;
    ThreadCB[th].func    = a0;
    ThreadCB[th].reg[29] = a1;
    ThreadCB[th].reg[28] = a2;

    v0 = th; pc0 = ra;
}

/*
 *	int CloseTh(long thread);
 */

void psxBios_CloseTh() { // 0f
    int th = a0 & 0xff;

    PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x0f], th);

    if (ThreadCB[th].status == 0) {
        v0 = 0;
    } else {
        ThreadCB[th].status = 0;
        v0 = 1;
    }

    pc0 = ra;
}

/*
 *	int ChangeTh(long thread);
 */

void psxBios_ChangeTh() { // 10
    int th = a0 & 0xff;

//	PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x10], th);

    if (ThreadCB[th].status == 0 || CurThread == th) {
        v0 = 0;

        pc0 = ra;
    } else {
        v0 = 1;

        if (ThreadCB[CurThread].status == 2) {
            ThreadCB[CurThread].status = 1;
            ThreadCB[CurThread].func = ra;
            memcpy(ThreadCB[CurThread].reg, GPR_ARRAY, sizeof(ThreadCB[CurThread].reg));
        }

        memcpy(GPR_ARRAY, ThreadCB[th].reg, sizeof(ThreadCB[CurThread].reg));
        pc0 = ThreadCB[th].func;
        ThreadCB[th].status = 2;
        CurThread = th;
    }
}
#endif


#if HLE_ENABLE_PAD
void psxBios_InitPAD() { // 0x12
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x12]);

    pad_buf1 = (char*)Ra0;
    pad_buf1len = a1;
    pad_buf2 = (char*)Ra2;
    pad_buf2len = a3;

    v0 = 1; pc0 = ra;
}

void psxBios_StartPAD() { // 13
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x13]);

    psxHwWrite16(0x1f801074, (unsigned short)(psxHwRead16(0x1f801074) | 0x1));
    CP0_Status |= 0x401;
    pc0 = ra;
}

void psxBios_StopPAD() { // 14
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x14]);

    pad_buf1 = NULL;
    pad_buf2 = NULL;
    pc0 = ra;
}

void psxBios_PAD_init() { // 15
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x15]);
    psxHwWrite16(0x1f801074, (u16)(psxHwRead16(0x1f801074) | 0x1));
    pad_buf = (int *)Ra1;
    *pad_buf = -1;
    CP0_Status |= 0x401;
    pc0 = ra;
}

void psxBios_PAD_dr() { // 16
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x16]);

    v0 = -1; pc0 = ra;
}

void psxBios_ChangeClearPad() { // 5b
    PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x5b], a0);

    pc0 = ra;
}
#endif

#if HLE_ENABLE_ENTRYINT
static unsigned interrupt_r26 = 0x8004E8B0;

static inline void SaveRegs() {
    memcpy(regs, PSX_CPU->GPR, sizeof(PSX_CPU->GPR));
    regs[33] = lo;
    regs[34] = hi;
    regs[35] = pc0;
}

static inline void LoadRegs() {
    memcpy(PSX_CPU->GPR, regs, sizeof(PSX_CPU->GPR));
    lo = regs[33];
    hi = regs[34];
}

void psxBios_ReturnFromException() { // 17
    LoadRegs();

    pc0 = CP0_EPC;
    if (CP0_CAUSE & 0x80000000) pc0 += 4;

    CP0_STATUS = (CP0_STATUS & 0xfffffff0) |
                ((CP0_STATUS & 0x3c) >> 2);
}

void psxBios_ResetEntryInt() { // 18
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x18]);

    jmp_int = NULL;
    pc0 = ra;
}

void psxBios_HookEntryInt() { // 19
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x19]);

    jmp_int = (u32*)Ra0;
    pc0 = ra;
}
#endif

#if HLE_ENABLE_EVENT
void psxBios_UnDeliverEvent() { // 0x20
    int ev, spec;
    int i;

    GetEv();
    GetSpec();

    PSXBIOS_LOG("psxBios_%s %x,%x\n", biosB0n[0x20], ev, spec);

    if (EventCB[ev][spec].status == EvStALREADY &&
        EventCB[ev][spec].mode == EvMdNOINTR)
        EventCB[ev][spec].status = EvStACTIVE;

    pc0 = ra;
}
#endif

#if HLE_ENABLE_FILEIO
#define buread(Ra1, mcd, length) { \
	SysPrintf("read %d: %x,%x (%s)\n", FDesc[1 + mcd].mcfile, FDesc[1 + mcd].offset, a2, Mcd##mcd##Data + 128 * FDesc[1 + mcd].mcfile + 0xa); \
	ptr = Mcd##mcd##Data + 8192 * FDesc[1 + mcd].mcfile + FDesc[1 + mcd].offset; \
	memcpy(Ra1, ptr, length); \
	if (FDesc[1 + mcd].mode & 0x8000) { \
	DeliverEvent(0x11, 0x2); /* 0xf0000011, 0x0004 */ \
	DeliverEvent(0x81, 0x2); /* 0xf4000001, 0x0004 */ \
	v0 = 0; } \
	else v0 = length; \
	FDesc[1 + mcd].offset += v0; \
}

#define buwrite(Ra1, mcd, length) { \
	u32 offset =  + 8192 * FDesc[1 + mcd].mcfile + FDesc[1 + mcd].offset; \
	SysPrintf("write %d: %x,%x\n", FDesc[1 + mcd].mcfile, FDesc[1 + mcd].offset, a2); \
	ptr = Mcd##mcd##Data + offset; \
	memcpy(ptr, Ra1, length); \
	FDesc[1 + mcd].offset += length; \
	SaveMcd(Config.Mcd##mcd, Mcd##mcd##Data, offset, length); \
	if (FDesc[1 + mcd].mode & 0x8000) { \
	DeliverEvent(0x11, 0x2); /* 0xf0000011, 0x0004 */ \
	DeliverEvent(0x81, 0x2); /* 0xf4000001, 0x0004 */ \
	v0 = 0; } \
	else v0 = length; \
}

#define buopen(mcd) { \
    strcpy(FDesc[1 + mcd].name, Ra0+5); \
    FDesc[1 + mcd].offset = 0; \
    FDesc[1 + mcd].mode   = a1; \
 \
    for (i=1; i<16; i++) { \
        ptr = Mcd##mcd##Data + 128 * i; \
        if ((*ptr & 0xF0) != 0x50) continue; \
        if (strcmp(FDesc[1 + mcd].name, ptr+0xa)) continue; \
        FDesc[1 + mcd].mcfile = i; \
        SysPrintf("open %s\n", ptr+0xa); \
        v0 = 1 + mcd; \
        break; \
    } \
    if (a1 & 0x200 && v0 == -1) { /* FCREAT */ \
        for (i=1; i<16; i++) { \
            int j, xor = 0; \
 \
            ptr = Mcd##mcd##Data + 128 * i; \
            if ((*ptr & 0xF0) == 0x50) continue; \
            ptr[0] = 0x50 | (u8)(a1 >> 16); \
            ptr[4] = 0x00; \
            ptr[5] = 0x20; \
            ptr[6] = 0x00; \
            ptr[7] = 0x00; \
            ptr[8] = 'B'; \
            ptr[9] = 'I'; \
            strcpy(ptr+0xa, FDesc[1 + mcd].name); \
            for (j=0; j<127; j++) xor^= ptr[j]; \
            ptr[127] = xor; \
            FDesc[1 + mcd].mcfile = i; \
            SysPrintf("openC %s\n", ptr); \
            v0 = 1 + mcd; \
            SaveMcd(Config.Mcd##mcd, Mcd##mcd##Data, 128 * i, 128); \
            break; \
        } \
    } \
}

/* Internally redirects to "FileRead(fd,tempbuf,1)".*/
/* For some strange reason, the returned character is sign-expanded; */
/* So if a return value of FFFFFFFFh could mean either character FFh, or error. */
/* TODO FIX ME : Properly implement this behaviour */
void psxBios_getc(void) // 0x03, 0x35
{
	char *ptr;
	void *pa1 = Ra1;
#ifdef PSXBIOS_LOG
	PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x03]);
#endif
	v0 = -1;

	if (pa1) {
		switch (a0) {
			case 2: buread(pa1, 1, 1); break;
			case 3: buread(pa1, 2, 1); break;
		}
	}

	pc0 = ra;
}

/* Copy of psxBios_write, except size is 1. */
void psxBios_putc(void) // 0x09, 0x3B
{
	char *ptr;
	void *pa1 = Ra1;
#ifdef PSXBIOS_LOG
	PSXBIOS_LOG("psxBios_%s\n", biosA0n[0x09]);
#endif
	v0 = -1;
	if (!pa1) {
		pc0 = ra;
		return;
	}

	if (a0 == 1) { // stdout
		char *ptr = (char *)pa1;

		v0 = a2;
		while (a2 > 0) {
			printf("%c", *ptr++); a2--;
		}
		pc0 = ra; return;
	}

	switch (a0) {
		case 2: buwrite(pa1, 1, 1); break;
		case 3: buwrite(pa1, 2, 1); break;
	}

	pc0 = ra;
}

/*
 *	int open(char *name , int mode);
 */

void psxBios_open() { // 0x32
    int i;
    char *ptr;
    auto *pa0 = (char*)Ra0;

    PSXBIOS_LOG("psxBios_%s: %s,%x\n", biosB0n[0x32], Ra0, a1);

    v0 = -1;

    if (pa0) {
        if (!strncmp(pa0, "bu00", 4)) {
            buopen(1);
        }

        if (!strncmp(pa0, "bu10", 4)) {
            buopen(2);
        }
    }

    pc0 = ra;
}

/*
 *	int lseek(int fd , int offset , int whence);
 */

void psxBios_lseek() { // 0x33
    PSXBIOS_LOG("psxBios_%s: %x, %x, %x\n", biosB0n[0x33], a0, a1, a2);

    switch (a2) {
        case 0: // SEEK_SET
            FDesc[a0].offset = a1;
            v0 = a1;
//			DeliverEvent(0x11, 0x2); // 0xf0000011, 0x0004
//			DeliverEvent(0x81, 0x2); // 0xf4000001, 0x0004
            break;

        case 1: // SEEK_CUR
            FDesc[a0].offset+= a1;
            v0 = FDesc[a0].offset;
            break;
    }

    pc0 = ra;
}

#define buread(Ra1, mcd) { \
    SysPrintf("read %d: %x,%x (%s)\n", FDesc[1 + mcd].mcfile, FDesc[1 + mcd].offset, a2, Mcd##mcd##Data + 128 * FDesc[1 + mcd].mcfile + 0xa); \
    ptr = Mcd##mcd##Data + 8192 * FDesc[1 + mcd].mcfile + FDesc[1 + mcd].offset; \
    memcpy(Ra1, ptr, a2); \
    if (FDesc[1 + mcd].mode & 0x8000) v0 = 0; \
    else v0 = a2; \
    FDesc[1 + mcd].offset += v0; \
    DeliverEvent(0x11, 0x2); /* 0xf0000011, 0x0004 */ \
    DeliverEvent(0x81, 0x2); /* 0xf4000001, 0x0004 */ \
}

/*
 *	int read(int fd , void *buf , int nbytes);
 */

void psxBios_read() { // 0x34
    char *ptr;
    void *pa1 = Ra1;

    PSXBIOS_LOG("psxBios_%s: %x, %x, %x\n", biosB0n[0x34], a0, a1, a2);

    v0 = -1;

    if (pa1) {
        switch (a0) {
            case 2: buread(pa1, 1); break;
            case 3: buread(pa1, 2); break;
        }
    }
        
    pc0 = ra;
}

#define buwrite(Ra1, mcd) { \
    u32 offset =  + 8192 * FDesc[1 + mcd].mcfile + FDesc[1 + mcd].offset; \
    SysPrintf("write %d: %x,%x\n", FDesc[1 + mcd].mcfile, FDesc[1 + mcd].offset, a2); \
    ptr = Mcd##mcd##Data + offset; \
    memcpy(ptr, Ra1, a2); \
    FDesc[1 + mcd].offset += a2; \
    SaveMcd(Config.Mcd##mcd, Mcd##mcd##Data, offset, a2); \
    if (FDesc[1 + mcd].mode & 0x8000) v0 = 0; \
    else v0 = a2; \
    DeliverEvent(0x11, 0x2); /* 0xf0000011, 0x0004 */ \
    DeliverEvent(0x81, 0x2); /* 0xf4000001, 0x0004 */ \
}

/*
 *	int write(int fd , void *buf , int nbytes);
 */

void psxBios_write() { // 0x35/0x03
    char *ptr;
    void *pa1 = Ra1;

    PSXBIOS_LOG("psxBios_%s: %x,%x,%x\n", biosB0n[0x35], a0, a1, a2);

    v0 = -1;
    if (!pa1) {
        pc0 = ra;
        return;
    }

    if (a0 == 1) { // stdout
        char *ptr = pa1;

        v0 = a2;
        while (a2 > 0) {
            SysPrintf("%c", *ptr++); a2--;
        }
        pc0 = ra; return;
    }

    switch (a0) {
        case 2: buwrite(pa1, 1); break;
        case 3: buwrite(pa1, 2); break;
    }

    pc0 = ra;
}

/*
 *	int close(int fd);
 */

void psxBios_close() { // 0x36
    PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x36], a0);

    v0 = a0;
    pc0 = ra;
}

char ffile[64], *pfile;
int nfile;

#define bufile(mcd) { \
    while (nfile < 16) { \
        int match=1; \
 \
        ptr = Mcd##mcd##Data + 128 * nfile; \
        nfile++; \
        if ((*ptr & 0xF0) != 0x50) continue; \
        ptr+= 0xa; \
        if (pfile[0] == 0) { \
            strncpy(dir->name, ptr, sizeof(dir->name)); \
            dir->name[sizeof(dir->name) - 1] = '\0'; \
        } else for (i=0; i<20; i++) { \
            if (pfile[i] == ptr[i]) { \
                dir->name[i] = ptr[i]; \
                if (ptr[i] == 0) break; else continue; } \
            if (pfile[i] == '?') { \
                dir->name[i] = ptr[i]; continue; } \
            if (pfile[i] == '*') { \
                strcpy(dir->name+i, ptr+i); break; } \
            match = 0; break; \
        } \
        SysPrintf("%d : %s = %s + %s (match=%d)\n", nfile, dir->name, pfile, ptr, match); \
        if (match == 0) continue; \
        dir->size = 8192; \
        v0 = _dir; \
        break; \
    } \
}

/*
 *	struct DIRENTRY* firstfile(char *name,struct DIRENTRY *dir);
 */
 
void psxBios_firstfile() { // 42
    struct DIRENTRY *dir = (struct DIRENTRY *)Ra1;
    void *pa0 = Ra0;
    u32 _dir = a1;
    char *ptr;
    int i;

    PSXBIOS_LOG("psxBios_%s: %s\n", biosB0n[0x42], Ra0);

    v0 = 0;

    if (pa0) {
        strcpy(ffile, pa0);
        pfile = ffile+5;
        nfile = 1;
        if (!strncmp(pa0, "bu00", 4)) {
            bufile(1);
        } else if (!strncmp(pa0, "bu10", 4)) {
            bufile(2);
        }
    }

    // firstfile() calls _card_read() internally, so deliver it's event
    DeliverEvent(0x11, 0x2);

    pc0 = ra;
}

/*
 *	struct DIRENTRY* nextfile(struct DIRENTRY *dir);
 */

void psxBios_nextfile() { // 43
    struct DIRENTRY *dir = (struct DIRENTRY *)Ra0;
    u32 _dir = a0;
    char *ptr;
    int i;

    PSXBIOS_LOG("psxBios_%s: %s\n", biosB0n[0x43], dir->name);

    v0 = 0;

    if (!strncmp(ffile, "bu00", 4)) {
        bufile(1);
    }

    if (!strncmp(ffile, "bu10", 4)) {
        bufile(2);
    }

    pc0 = ra;
}

#define burename(mcd) { \
    for (i=1; i<16; i++) { \
        int namelen, j, xor = 0; \
        ptr = Mcd##mcd##Data + 128 * i; \
        if ((*ptr & 0xF0) != 0x50) continue; \
        if (strcmp(Ra0+5, ptr+0xa)) continue; \
        namelen = strlen(Ra1+5); \
        memcpy(ptr+0xa, Ra1+5, namelen); \
        memset(ptr+0xa+namelen, 0, 0x75-namelen); \
        for (j=0; j<127; j++) xor^= ptr[j]; \
        ptr[127] = xor; \
        SaveMcd(Config.Mcd##mcd, Mcd##mcd##Data, 128 * i + 0xa, 0x76); \
        v0 = 1; \
        break; \
    } \
}

/*
 *	int rename(char *old, char *new);
 */

void psxBios_rename() { // 44
    void *pa0 = Ra0;
    void *pa1 = Ra1;
    char *ptr;
    int i;

    PSXBIOS_LOG("psxBios_%s: %s,%s\n", biosB0n[0x44], Ra0, Ra1);

    v0 = 0;

    if (pa0 && pa1) {
        if (!strncmp(pa0, "bu00", 4) && !strncmp(pa1, "bu00", 4)) {
            burename(1);
        }

        if (!strncmp(pa0, "bu10", 4) && !strncmp(pa1, "bu10", 4)) {
            burename(2);
        }
    }

    pc0 = ra;
}


#define budelete(mcd) { \
    for (i=1; i<16; i++) { \
        ptr = Mcd##mcd##Data + 128 * i; \
        if ((*ptr & 0xF0) != 0x50) continue; \
        if (strcmp(Ra0+5, ptr+0xa)) continue; \
        *ptr = (*ptr & 0xf) | 0xA0; \
        SaveMcd(Config.Mcd##mcd, Mcd##mcd##Data, 128 * i, 1); \
        SysPrintf("delete %s\n", ptr+0xa); \
        v0 = 1; \
        break; \
    } \
}

/*
 *	int delete(char *name);
 */

void psxBios_delete() { // 45
    void *pa0 = Ra0;
    char *ptr;
    int i;

    PSXBIOS_LOG("psxBios_%s: %s\n", biosB0n[0x45], Ra0);

    v0 = 0;

    if (pa0) {
        if (!strncmp(pa0, "bu00", 4)) {
            budelete(1);
        }

        if (!strncmp(pa0, "bu10", 4)) {
            budelete(2);
        }
    }

    pc0 = ra;
}
#endif


#if HLE_ENABLE_MCD
void psxBios_InitCARD() { // 4a
    PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x4a], a0);

    CardState = 0;

    pc0 = ra;
}

void psxBios_StartCARD() { // 4b
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x4b]);

    if (CardState == 0) CardState = 1;

    pc0 = ra;
}

void psxBios_StopCARD() { // 4c
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x4c]);

    if (CardState == 1) CardState = 0;

    pc0 = ra;
}

void psxBios__card_write() { // 0x4e

    assert(PSX_FIO);

    PSXBIOS_LOG("psxBios_%s: %x,%x,%x\n", biosB0n[0x4e], a0, a1, a2);

    if (HleYieldCheck(SCRI_None)) {
        // is card_active_chan supposed to be slot? --jstine
        card_active_chan = a0;
        int port = a0 >> 4;
        int slot = 0;

        if (auto *pa2 = Ra2) {
            auto mcd = PSX_FIO->GetMemcardDevice(port);
            VmcWriteNV(port, slot, pa2, a1 * 128, 128);
        }

        HleCallYield(SCRI_psxBios__card_write_00);
        if (DeliverEventYield(0x11, 0x2)) { // 0xf0000011, 0x0004
            return;
        }
        pc0 = ra;
    }

    if (HleYieldCheck(SCRI_psxBios__card_write_00)) {
        HleCallResume();
        v0 = 1;
        pc0 = ra;
    }
    else {
        assert(false);
    }

//	DeliverEvent(0x81, 0x2); // 0xf4000001, 0x0004
}

void psxBios__card_read() { // 0x4f

    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x4f]);

    if (HleYieldCheck(SCRI_None)) {
        card_active_chan = a0;
        int port = a0 >> 4;
        int slot = 0;

        if (auto *pa2 = Ra2) {
            auto mcd = PSX_FIO->GetMemcardDevice(port);
            VmcReadNV(port, slot, pa2, a1 * 128, 128);
        }

        HleCallYield(SCRI_psxBios__card_read_00);
        if (DeliverEventYield(0x11, 0x2)) { // 0xf0000011, 0x0004
            return;
        }
        pc0 = ra;
    }

    if (HleYieldCheck(SCRI_psxBios__card_read_00)) {
        HleCallResume();
    }
    else {
        assert(false);
    }

//	DeliverEvent(0x81, 0x2); // 0xf4000001, 0x0004

    v0 = 1; pc0 = ra;
}

void psxBios__new_card() { // 0x50
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x50]);

    pc0 = ra;
}
#endif

void psxBios_Krom2RawAdd() { // 0x51
    int i = 0;

    const u32 table_8140[][2] = {
        {0x8140, 0x0000}, {0x8180, 0x0762}, {0x81ad, 0x0cc6}, {0x81b8, 0x0ca8},
        {0x81c0, 0x0f00}, {0x81c8, 0x0d98}, {0x81cf, 0x10c2}, {0x81da, 0x0e6a},
        {0x81e9, 0x13ce}, {0x81f0, 0x102c}, {0x81f8, 0x1590}, {0x81fc, 0x111c},
        {0x81fd, 0x1626}, {0x824f, 0x113a}, {0x8259, 0x20ee}, {0x8260, 0x1266},
        {0x827a, 0x24cc}, {0x8281, 0x1572}, {0x829b, 0x28aa}, {0x829f, 0x187e},
        {0x82f2, 0x32dc}, {0x8340, 0x2238}, {0x837f, 0x4362}, {0x8380, 0x299a},
        {0x8397, 0x4632}, {0x839f, 0x2c4c}, {0x83b7, 0x49f2}, {0x83bf, 0x2f1c},
        {0x83d7, 0x4db2}, {0x8440, 0x31ec}, {0x8461, 0x5dde}, {0x8470, 0x35ca},
        {0x847f, 0x6162}, {0x8480, 0x378c}, {0x8492, 0x639c}, {0x849f, 0x39a8},
        {0xffff, 0}
    };

    const u32 table_889f[][2] = {
        {0x889f, 0x3d68},  {0x8900, 0x40ec},  {0x897f, 0x4fb0},  {0x8a00, 0x56f4},
        {0x8a7f, 0x65b8},  {0x8b00, 0x6cfc},  {0x8b7f, 0x7bc0},  {0x8c00, 0x8304},
        {0x8c7f, 0x91c8},  {0x8d00, 0x990c},  {0x8d7f, 0xa7d0},  {0x8e00, 0xaf14},
        {0x8e7f, 0xbdd8},  {0x8f00, 0xc51c},  {0x8f7f, 0xd3e0},  {0x9000, 0xdb24},
        {0x907f, 0xe9e8},  {0x9100, 0xf12c},  {0x917f, 0xfff0},  {0x9200, 0x10734},
        {0x927f, 0x115f8}, {0x9300, 0x11d3c}, {0x937f, 0x12c00}, {0x9400, 0x13344},
        {0x947f, 0x14208}, {0x9500, 0x1494c}, {0x957f, 0x15810}, {0x9600, 0x15f54},
        {0x967f, 0x16e18}, {0x9700, 0x1755c}, {0x977f, 0x18420}, {0x9800, 0x18b64},
        {0xffff, 0}
    };

    if (a0 >= 0x8140 && a0 <= 0x84be) {
        while (table_8140[i][0] <= a0) i++;
        a0 -= table_8140[i - 1][0];
        v0 = 0xbfc66000 + (a0 * 0x1e + table_8140[i - 1][1]);
    } else if (a0 >= 0x889f && a0 <= 0x9872) {
        while (table_889f[i][0] <= a0) i++;
        a0 -= table_889f[i - 1][0];
        v0 = 0xbfc66000 + (a0 * 0x1e + table_889f[i - 1][1]);
    } else {
        v0 = 0xffffffff;
    }

    pc0 = ra;
}

void psxBios_GetC0Table() { // 56
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x56]);

    v0 = 0x674; pc0 = ra;
}

void psxBios_GetB0Table() { // 57
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x57]);

    v0 = 0x874; pc0 = ra;
}

#if HLE_ENABLE_MCD
void psxBios__card_chan() { // 0x58
    PSXBIOS_LOG("psxBios_%s\n", biosB0n[0x58]);

    v0 = card_active_chan;
    pc0 = ra;
}

void psxBios__card_status() { // 5c
   PSXBIOS_LOG("psxBios_%s: %x\n", biosB0n[0x5c], a0);

   v0 = 1;
   pc0 = ra;
}
#endif


/* System calls C0 */

#if HLE_ENABLE_ENTRYINT
/*
 * int SysEnqIntRP(int index , long *queue);
 */

void psxBios_SysEnqIntRP() { // 02
    PSXBIOS_LOG("psxBios_%s: %x\n", biosC0n[0x02] ,a0);

    SysIntRP[a0] = a1;

    v0 = 0; pc0 = ra;
}

/*
 * int SysDeqIntRP(int index , long *queue);
 */

void psxBios_SysDeqIntRP() { // 03
    PSXBIOS_LOG("psxBios_%s: %x\n", biosC0n[0x03], a0);

    SysIntRP[a0] = 0;

    v0 = 0; pc0 = ra;
}
#endif

using VoidFnptr = void (*)();

using HLE_BIOS_TABLE = VoidFnptr[256]; 

HLE_BIOS_TABLE biosA0 = {};
HLE_BIOS_TABLE biosB0 = {};
HLE_BIOS_TABLE biosC0 = {};


VoidFnptr HLE_Call_Table[SCRI_MAX_COUNT];

void HleInitCallTable() {

    HLE_Call_Table[SCRI_DeliverEvent_Resume     ] = HLEcb_DeliverEvent_Resume;
    HLE_Call_Table[SCRI_psxBios_DeliverEvent_00 ] = psxBios_DeliverEvent     ;
    HLE_Call_Table[SCRI_psxBios__bu_init_00     ] = psxBios__bu_init         ;
    HLE_Call_Table[SCRI_psxBios__bu_init_01     ] = psxBios__bu_init         ;
    HLE_Call_Table[SCRI_psxBios__bu_init_02     ] = psxBios__bu_init         ;
    HLE_Call_Table[SCRI_psxBios__card_load_00   ] = psxBios__card_load       ;
    HLE_Call_Table[SCRI_psxBios__card_info_00   ] = psxBios__card_info       ;
    HLE_Call_Table[SCRI_psxBios__card_read_00   ] = psxBios__card_read       ;
    HLE_Call_Table[SCRI_psxBios__card_write_00  ] = psxBios__card_write      ;

};

#include "sjisfont.h"

void psxBiosResetToNone() {
    for(int i = 0; i < 256; i++) {
        biosA0[i] = NULL;
        biosB0[i] = NULL;
        biosC0[i] = NULL;
    }
}

void psxBiosInit_StdLib() {

    HleInitCallTable();

    int i;

    biosA0[0x3e] = psxBios_puts;
    biosA0[0x3f] = psxBios_printf;

    biosB0[0x3d] = psxBios_putchar;
    biosB0[0x3f] = psxBios_puts;

#if HLE_ENABLE_FILEIO
    biosA0[0x00] = psxBios_open;
    biosA0[0x01] = psxBios_lseek;
    biosA0[0x02] = psxBios_read;
    biosA0[0x03] = psxBios_write;
    biosA0[0x04] = psxBios_close;
#endif

    //biosA0[0x05] = psxBios_ioctl;
    //biosA0[0x06] = psxBios_exit;
    //biosA0[0x07] = psxBios_sys_a0_07;
    //biosA0[0x08] = psxBios_getc;
    //biosA0[0x09] = psxBios_putc;
    //biosA0[0x0a] = psxBios_todigit;
    //biosA0[0x0b] = psxBios_atof;
    //biosA0[0x0c] = psxBios_strtoul;
    //biosA0[0x0d] = psxBios_strtol;
    biosA0[0x0e] = psxBios_abs;
    biosA0[0x0f] = psxBios_labs;
    biosA0[0x10] = psxBios_atoi;
    biosA0[0x11] = psxBios_atol;
    //biosA0[0x12] = psxBios_atob;
    biosA0[0x13] = psxBios_setjmp;
    biosA0[0x14] = psxBios_longjmp;
    biosA0[0x15] = psxBios_strcat;
    biosA0[0x16] = psxBios_strncat;
    biosA0[0x17] = psxBios_strcmp;
    biosA0[0x18] = psxBios_strncmp;
    biosA0[0x19] = psxBios_strcpy;
    biosA0[0x1a] = psxBios_strncpy;
    biosA0[0x1b] = psxBios_strlen;
    biosA0[0x1c] = psxBios_index;
    biosA0[0x1d] = psxBios_rindex;
    biosA0[0x1e] = psxBios_strchr;
    biosA0[0x1f] = psxBios_strrchr;
    biosA0[0x20] = psxBios_strpbrk;
    biosA0[0x21] = psxBios_strspn;
    biosA0[0x22] = psxBios_strcspn;
    biosA0[0x23] = psxBios_strtok;
    biosA0[0x24] = psxBios_strstr;
    biosA0[0x25] = psxBios_toupper;
    biosA0[0x26] = psxBios_tolower;
    biosA0[0x27] = psxBios_bcopy;
    biosA0[0x28] = psxBios_bzero;
    biosA0[0x29] = psxBios_bcmp;
    biosA0[0x2a] = psxBios_memcpy;
    biosA0[0x2b] = psxBios_memset;
    biosA0[0x2c] = psxBios_memmove;
    biosA0[0x2d] = psxBios_memcmp;
    biosA0[0x2e] = psxBios_memchr;
    biosA0[0x2f] = psxBios_rand;
    biosA0[0x30] = psxBios_srand;

#if HLE_ENABLE_QSORT
    biosA0[0x31] = psxBios_qsort;
#endif

    //biosA0[0x32] = psxBios_strtod;

#if HLE_ENABLE_HEAP
    if (hle_config_env_heap()) {
        biosA0[0x33] = psxBios_malloc;
        biosA0[0x34] = psxBios_free;
        //biosA0[0x35] = psxBios_lsearch;
        //biosA0[0x36] = psxBios_bsearch;
        biosA0[0x37] = psxBios_calloc;
        biosA0[0x38] = psxBios_realloc;
        biosA0[0x39] = psxBios_InitHeap;
    }
#endif

    //biosA0[0x3a] = psxBios__exit;
    biosA0[0x3b] = psxBios_getchar;
    biosA0[0x3c] = psxBios_putchar;	
    //biosA0[0x3d] = psxBios_gets;

    biosB0[0x56] = psxBios_GetC0Table;
    biosB0[0x57] = psxBios_GetB0Table;

    biosA0[0x44] = psxBios_FlushCache;
}

void psxBiosInitFull() {

    //biosA0[0x40] = psxBios_sys_a0_40;
    //biosA0[0x41] = psxBios_LoadTest;

#if HLE_ENABLE_LOADEXEC
    biosA0[0x42] = psxBios_Load;
    biosA0[0x51] = psxBios_LoadExec;
    biosA0[0x43] = psxBios_Exec;
#endif


    //biosA0[0x45] = psxBios_InstallInterruptHandler;

#if HLE_ENABLE_GPU
    biosA0[0x46] = psxBios_GPU_dw;
    biosA0[0x47] = psxBios_mem2vram;
    biosA0[0x48] = psxBios_SendGPU;
    biosA0[0x49] = psxBios_GPU_cw;
    biosA0[0x4a] = psxBios_GPU_cwb;
    biosA0[0x4b] = psxBios_GPU_SendPackets;
    biosA0[0x4c] = psxBios_sys_a0_4c;
    biosA0[0x4d] = psxBios_GPU_GetGPUStatus;
#endif

    //biosA0[0x4e] = psxBios_GPU_sync;	
    //biosA0[0x4f] = psxBios_sys_a0_4f;
    //biosA0[0x50] = psxBios_sys_a0_50;
    //biosA0[0x52] = psxBios_GetSysSp;
    //biosA0[0x53] = psxBios_sys_a0_53;
    //biosA0[0x54] = psxBios__96_init_a54;
    //biosA0[0x55] = psxBios__bu_init_a55;
    //biosA0[0x56] = psxBios__96_remove_a56;
    //biosA0[0x57] = psxBios_sys_a0_57;
    //biosA0[0x58] = psxBios_sys_a0_58;
    //biosA0[0x59] = psxBios_sys_a0_59;
    //biosA0[0x5a] = psxBios_sys_a0_5a;
    //biosA0[0x5b] = psxBios_dev_tty_init;
    //biosA0[0x5c] = psxBios_dev_tty_open;
    //biosA0[0x5d] = psxBios_sys_a0_5d;
    //biosA0[0x5e] = psxBios_dev_tty_ioctl;
    //biosA0[0x5f] = psxBios_dev_cd_open;
    //biosA0[0x60] = psxBios_dev_cd_read;
    //biosA0[0x61] = psxBios_dev_cd_close;
    //biosA0[0x62] = psxBios_dev_cd_firstfile;
    //biosA0[0x63] = psxBios_dev_cd_nextfile;
    //biosA0[0x64] = psxBios_dev_cd_chdir;
    //biosA0[0x65] = psxBios_dev_card_open;
    //biosA0[0x66] = psxBios_dev_card_read;
    //biosA0[0x67] = psxBios_dev_card_write;
    //biosA0[0x68] = psxBios_dev_card_close;
    //biosA0[0x69] = psxBios_dev_card_firstfile;
    //biosA0[0x6a] = psxBios_dev_card_nextfile;
    //biosA0[0x6b] = psxBios_dev_card_erase;
    //biosA0[0x6c] = psxBios_dev_card_undelete;
    //biosA0[0x6d] = psxBios_dev_card_format;
    //biosA0[0x6e] = psxBios_dev_card_rename;
    //biosA0[0x6f] = psxBios_dev_card_6f;

    //biosA0[0x73] = psxBios_sys_a0_73;
    //biosA0[0x74] = psxBios_sys_a0_74;
    //biosA0[0x75] = psxBios_sys_a0_75;
    //biosA0[0x76] = psxBios_sys_a0_76;
    //biosA0[0x77] = psxBios_sys_a0_77;
    //biosA0[0x78] = psxBios__96_CdSeekL;
    //biosA0[0x79] = psxBios_sys_a0_79;
    //biosA0[0x7a] = psxBios_sys_a0_7a;
    //biosA0[0x7b] = psxBios_sys_a0_7b;
    //biosA0[0x7c] = psxBios__96_CdGetStatus;
    //biosA0[0x7d] = psxBios_sys_a0_7d;
    //biosA0[0x7e] = psxBios__96_CdRead;
    //biosA0[0x7f] = psxBios_sys_a0_7f;
    //biosA0[0x80] = psxBios_sys_a0_80;
    //biosA0[0x81] = psxBios_sys_a0_81;
    //biosA0[0x82] = psxBios_sys_a0_82;		
    //biosA0[0x83] = psxBios_sys_a0_83;
    //biosA0[0x84] = psxBios_sys_a0_84;
    //biosA0[0x85] = psxBios__96_CdStop;	
    //biosA0[0x86] = psxBios_sys_a0_86;
    //biosA0[0x87] = psxBios_sys_a0_87;
    //biosA0[0x88] = psxBios_sys_a0_88;
    //biosA0[0x89] = psxBios_sys_a0_89;
    //biosA0[0x8a] = psxBios_sys_a0_8a;
    //biosA0[0x8b] = psxBios_sys_a0_8b;
    //biosA0[0x8c] = psxBios_sys_a0_8c;
    //biosA0[0x8d] = psxBios_sys_a0_8d;
    //biosA0[0x8e] = psxBios_sys_a0_8e;
    //biosA0[0x8f] = psxBios_sys_a0_8f;
    //biosA0[0x90] = psxBios_sys_a0_90;
    //biosA0[0x91] = psxBios_sys_a0_91;
    //biosA0[0x92] = psxBios_sys_a0_92;
    //biosA0[0x93] = psxBios_sys_a0_93;
    //biosA0[0x94] = psxBios_sys_a0_94;
    //biosA0[0x95] = psxBios_sys_a0_95;
    //biosA0[0x96] = psxBios_AddCDROMDevice;
    //biosA0[0x97] = psxBios_AddMemCardDevide;
    //biosA0[0x98] = psxBios_DisableKernelIORedirection;
    //biosA0[0x99] = psxBios_EnableKernelIORedirection;
    //biosA0[0x9a] = psxBios_sys_a0_9a;
    //biosA0[0x9b] = psxBios_sys_a0_9b;
    //biosA0[0x9c] = psxBios_SetConf;
    //biosA0[0x9d] = psxBios_GetConf;
    //biosA0[0x9e] = psxBios_sys_a0_9e;

#if 0
    biosA0[0x9f] = psxBios_SetMem;
#endif

    //biosA0[0xa0] = psxBios__boot;
    //biosA0[0xa1] = psxBios_SystemError;
    //biosA0[0xa2] = psxBios_EnqueueCdIntr;
    //biosA0[0xa3] = psxBios_DequeueCdIntr;
    //biosA0[0xa4] = psxBios_sys_a0_a4;
    //biosA0[0xa5] = psxBios_ReadSector;
    //biosA0[0xa6] = psxBios_get_cd_status;
    //biosA0[0xa7] = psxBios_bufs_cb_0;
    //biosA0[0xa8] = psxBios_bufs_cb_1;
    //biosA0[0xa9] = psxBios_bufs_cb_2;
    //biosA0[0xaa] = psxBios_bufs_cb_3;

#if HLE_ENABLE_EVENT
    if (hle_config_env_event()) {
        biosA0[0x70] = psxBios__bu_init;
        biosB0[0x07] = psxBios_DeliverEvent;
        biosB0[0x08] = psxBios_OpenEvent;
        biosB0[0x09] = psxBios_CloseEvent;
        biosB0[0x0a] = psxBios_WaitEvent;
        biosB0[0x0b] = psxBios_TestEvent;
        biosB0[0x0c] = psxBios_EnableEvent;
        biosB0[0x0d] = psxBios_DisableEvent;
        biosB0[0x20] = psxBios_UnDeliverEvent;
    }

#if HLE_ENABLE_MCD
    if (hle_config_env_mcd()) {
        biosA0[0xab] = psxBios__card_info;
        biosA0[0xac] = psxBios__card_load;
    }
#endif
#endif

    //biosA0[0axd] = psxBios__card_auto;
    //biosA0[0xae] = psxBios_bufs_cd_4;
    //biosA0[0xaf] = psxBios_sys_a0_af;
    //biosA0[0xb0] = psxBios_sys_a0_b0;
    //biosA0[0xb1] = psxBios_sys_a0_b1;
    //biosA0[0xb2] = psxBios_do_a_long_jmp
    //biosA0[0xb3] = psxBios_sys_a0_b3;
    //biosA0[0xb4] = psxBios_sub_function;
//*******************B0 CALLS****************************
    //biosB0[0x00] = psxBios_SysMalloc;
    //biosB0[0x01] = psxBios_sys_b0_01;

#if HLE_ENABLE_RCNT
    if (hle_config_env_rcnt()) {
        biosB0[0x02] = psxBios_SetRCnt;
        biosB0[0x03] = psxBios_GetRCnt;
        biosB0[0x04] = psxBios_StartRCnt;
        biosB0[0x05] = psxBios_StopRCnt;
        biosB0[0x06] = psxBios_ResetRCnt;
        biosC0[0x0a] = psxBios_ChangeClearRCnt;	
    }
#endif

#if HLE_ENABLE_THREAD
    if (hle_config_env_thread()) {
        biosB0[0x0e] = psxBios_OpenTh;
        biosB0[0x0f] = psxBios_CloseTh;
        biosB0[0x10] = psxBios_ChangeTh;
        //biosB0[0x11] = psxBios_psxBios_b0_11;
    }
#endif

#if HLE_ENABLE_PAD
    biosB0[0x12] = psxBios_InitPAD;
    biosB0[0x13] = psxBios_StartPAD;
    biosB0[0x14] = psxBios_StopPAD;
    biosB0[0x15] = psxBios_PAD_init;
    biosB0[0x16] = psxBios_PAD_dr;
    biosB0[0x5b] = psxBios_ChangeClearPad;
#endif

#if HLE_ENABLE_ENTRYINT
    biosB0[0x18] = psxBios_ResetEntryInt;
    biosB0[0x19] = psxBios_HookEntryInt;
#endif

    //biosB0[0x1a] = psxBios_sys_b0_1a;
    //biosB0[0x1b] = psxBios_sys_b0_1b;
    //biosB0[0x1c] = psxBios_sys_b0_1c;
    //biosB0[0x1d] = psxBios_sys_b0_1d;
    //biosB0[0x1e] = psxBios_sys_b0_1e;
    //biosB0[0x1f] = psxBios_sys_b0_1f;
    //biosB0[0x21] = psxBios_sys_b0_21;
    //biosB0[0x22] = psxBios_sys_b0_22;
    //biosB0[0x23] = psxBios_sys_b0_23;
    //biosB0[0x24] = psxBios_sys_b0_24;
    //biosB0[0x25] = psxBios_sys_b0_25;
    //biosB0[0x26] = psxBios_sys_b0_26;
    //biosB0[0x27] = psxBios_sys_b0_27;
    //biosB0[0x28] = psxBios_sys_b0_28;
    //biosB0[0x29] = psxBios_sys_b0_29;
    //biosB0[0x2a] = psxBios_sys_b0_2a;
    //biosB0[0x2b] = psxBios_sys_b0_2b;
    //biosB0[0x2c] = psxBios_sys_b0_2c;
    //biosB0[0x2d] = psxBios_sys_b0_2d;
    //biosB0[0x2e] = psxBios_sys_b0_2e;
    //biosB0[0x2f] = psxBios_sys_b0_2f;
    //biosB0[0x30] = psxBios_sys_b0_30;
    //biosB0[0x31] = psxBios_sys_b0_31;

#if HLE_ENABLE_FILEIO
    biosB0[0x32] = psxBios_open;
    biosB0[0x33] = psxBios_lseek;
    biosB0[0x34] = psxBios_read;
    biosB0[0x35] = psxBios_write;
    biosB0[0x36] = psxBios_close;

    //biosB0[0x37] = psxBios_ioctl;
    //biosB0[0x38] = psxBios_exit;
    //biosB0[0x39] = psxBios_sys_b0_39;
    //biosB0[0x3a] = psxBios_getc;
    //biosB0[0x3b] = psxBios_putc;
    biosB0[0x3c] = psxBios_getchar;
    //biosB0[0x3e] = psxBios_gets;
    //biosB0[0x40] = psxBios_cd;
    biosB0[0x41] = psxBios_format;
    biosB0[0x42] = psxBios_firstfile;
    biosB0[0x43] = psxBios_nextfile;
    biosB0[0x44] = psxBios_rename;
    biosB0[0x45] = psxBios_delete;
    //biosB0[0x46] = psxBios_undelete;
    //biosB0[0x47] = psxBios_AddDevice;
    //biosB0[0x48] = psxBios_RemoteDevice;
    //biosB0[0x49] = psxBios_PrintInstalledDevices;
#endif

#if HLE_ENABLE_MCD
    biosB0[0x4a] = psxBios_InitCARD;
    biosB0[0x4b] = psxBios_StartCARD;
    biosB0[0x4c] = psxBios_StopCARD;
    //biosB0[0x4d] = psxBios_sys_b0_4d;
    biosB0[0x4e] = psxBios__card_write;
    biosB0[0x4f] = psxBios__card_read;
    biosB0[0x50] = psxBios__new_card;
    biosB0[0x5c] = psxBios__card_status;
#endif

    biosB0[0x51] = psxBios_Krom2RawAdd;
    //biosB0[0x52] = psxBios_sys_b0_52;
    //biosB0[0x53] = psxBios_sys_b0_53;
    //biosB0[0x54] = psxBios__get_errno;
    //biosB0[0x55] = psxBios__get_error;

#if HLE_ENABLE_MCD
    biosB0[0x58] = psxBios__card_chan;
#endif

    //biosB0[0x59] = psxBios_sys_b0_59;
    //biosB0[0x5a] = psxBios_sys_b0_5a;
    //biosB0[0x5d] = psxBios__card_wait;
//*******************C0 CALLS****************************
    //biosC0[0x00] = psxBios_InitRCnt;
    //biosC0[0x01] = psxBios_InitException;

#if HLE_ENABLE_ENTRYINT
    biosB0[0x17] = psxBios_ReturnFromException;
    biosC0[0x02] = psxBios_SysEnqIntRP;
    biosC0[0x03] = psxBios_SysDeqIntRP;
#endif

    //biosC0[0x04] = psxBios_get_free_EvCB_slot;
    //biosC0[0x05] = psxBios_get_free_TCB_slot;
    //biosC0[0x06] = psxBios_ExceptionHandler;
    //biosC0[0x07] = psxBios_InstallExeptionHandler;
    //biosC0[0x08] = psxBios_SysInitMemory;
    //biosC0[0x09] = psxBios_SysInitKMem;
    //biosC0[0x0b] = psxBios_SystemError;
    //biosC0[0x0c] = psxBios_InitDefInt;
    //biosC0[0x0d] = psxBios_sys_c0_0d;
    //biosC0[0x0e] = psxBios_sys_c0_0e;
    //biosC0[0x0f] = psxBios_sys_c0_0f;
    //biosC0[0x10] = psxBios_sys_c0_10;
    //biosC0[0x11] = psxBios_sys_c0_11;
    //biosC0[0x12] = psxBios_InstallDevices;
    //biosC0[0x13] = psxBios_FlushStfInOutPut;
    //biosC0[0x14] = psxBios_sys_c0_14;
    //biosC0[0x15] = psxBios__cdevinput;
    //biosC0[0x16] = psxBios__cdevscan;
    //biosC0[0x17] = psxBios__circgetc;
    //biosC0[0x18] = psxBios__circputc;
    //biosC0[0x19] = psxBios_ioabort;
    //biosC0[0x1a] = psxBios_sys_c0_1a
    //biosC0[0x1b] = psxBios_KernelRedirect;
    //biosC0[0x1c] = psxBios_PatchAOTable;
//************** THE END ***************************************
/**/


#if HLE_ENABLE_EVENT
    u32 base;
    int size;
    uLongf len;

    base = 0x1000;
    size = sizeof(EvCB) * 32;
    EventCB = (EvCB *)(PSX_ROM_START + base); base += size * 6;
    if (HLE_FULL) {
        memset(EventCB, 0, size * 6);
    }
    HwEV = EventCB;
    EvEV = EventCB + 32;
    RcEV = EventCB + 32 * 2;
    UeEV = EventCB + 32 * 3;
    SwEV = EventCB + 32 * 4;
    ThEV = EventCB + 32 * 5;
#endif

#if HLE_ENABLE_THREAD
    memset(ThreadCB, 0, sizeof(ThreadCB));
    ThreadCB[0].status = 2; // main thread
    CurThread = 0;
#endif

#if HLE_ENABLE_ENTRYINT
    memset(SysIntRP, 0, sizeof(SysIntRP));
    jmp_int = NULL;
#endif

#if HLE_ENABLE_PAD
    pad_buf = NULL;
    pad_buf1 = NULL;
    pad_buf2 = NULL;
    pad_buf1len = pad_buf2len = 0;
    CardState = -1;
#endif

#if HLE_ENABLE_HEAP
    heap_addr = NULL;
    heap_end = NULL;
#endif

#if HLE_ENABLE_FILEIO
    memset(FDesc, 0, sizeof(FDesc));
#endif

#if HLE_ENABLE_EMPTY_ROM
    if (hle_config_env_norom()) {
        // not sure about these, the HLE seems to skip them which, I expect, is only wise
        // if we're bypassing BIOS entirely. --jstine

        biosA0[0x71] = psxBios__96_init;
        biosA0[0x72] = psxBios__96_remove;

        // I'm not quite sure what this is about ... it's setting up some values into B0/C0 table, so I assume
        // it should only be performed when bypassing BIOS entirely --jstine

        auto* ptr = (u32 *)PSXM(0x0874); // b0 table
        ptr[0] = SWAPu32(0x4c54 - 0x884);

        ptr = (u32 *)PSXM(0x0674); // c0 table
        ptr[6] = SWAPu32(0xc80);

        psxMu32ref(0x0150) = SWAPu32(0x160);
        psxMu32ref(0x0154) = SWAPu32(0x320);
        psxMu32ref(0x0160) = SWAPu32(0x248);
        strcpy((char *)PSXM(0x248), "bu");
    /*	psxMu32ref(0x0ca8) = SWAPu32(0x1f410004);
        psxMu32ref(0x0cf0) = SWAPu32(0x3c020000);
        psxMu32ref(0x0cf4) = SWAPu32(0x2442641c);
        psxMu32ref(0x09e0) = SWAPu32(0x43d0);
        psxMu32ref(0x4d98) = SWAPu32(0x946f000a);
    */
        // opcode HLE
        (u32&)PSX_ROM_START[0x0000] = SWAPu32((0x3b << 26) | 4);
        psxMu32ref(0x0000) = SWAPu32((0x3b << 26) | 0);
        psxMu32ref(0x00a0) = SWAPu32((0x3b << 26) | 1);
        psxMu32ref(0x00b0) = SWAPu32((0x3b << 26) | 2);
        psxMu32ref(0x00c0) = SWAPu32((0x3b << 26) | 3);
        psxMu32ref(0x4c54) = SWAPu32((0x3b << 26) | 0);
        psxMu32ref(0x8000) = SWAPu32((0x3b << 26) | 5);
        psxMu32ref(0x07a0) = SWAPu32((0x3b << 26) | 0);
        psxMu32ref(0x0884) = SWAPu32((0x3b << 26) | 0);
        psxMu32ref(0x0894) = SWAPu32((0x3b << 26) | 0);

        // initial stack pointer for BIOS interrupt
        psxMu32ref(0x6c80) = SWAPu32(0x000085c8);

        // initial RNG seed
        psxMu32ref(0x9010) = SWAPu32(0xac20cc00);

        // fonts
        uLongf len;
        len = 0x80000 - 0x66000;
        uncompress((Bytef *)(PSX_ROM_START + 0x66000), &len, font_8140, sizeof(font_8140));
        len = 0x80000 - 0x69d68;
        uncompress((Bytef *)(PSX_ROM_START + 0x69d68), &len, font_889f, sizeof(font_889f));

        // memory size 2 MB
        // (mednafen doesn't seem to bother to set this...)
        //psxHu32ref(0x1060) = SWAPu32(0x00000b88);
    }
#endif
}

void psxBiosShutdown() {
}

#define psxBios_PADpoll(pad) { \
    PAD##pad##_startPoll(pad); \
    pad_buf##pad[0] = 0; \
    pad_buf##pad[1] = PAD##pad##_poll(0x42); \
    if (!(pad_buf##pad[1] & 0x0f)) { \
        bufcount = 32; \
    } else { \
        bufcount = (pad_buf##pad[1] & 0x0f) * 2; \
    } \
    PAD##pad##_poll(0); \
    i = 2; \
    while (bufcount--) { \
        pad_buf##pad[i++] = PAD##pad##_poll(0); \
    } \
}

#if HLE_ENABLE_EXCEPTION
void biosInterrupt() {
    int i, bufcount;

//	if (psxHu32(0x1070) & 0x1) { // Vsync
        if (pad_buf != NULL) {
            u32 *buf = (u32*)pad_buf;

            if (!Config.UseNet) {
                PAD1_startPoll(1);
                if (PAD1_poll(0x42) == 0x23) {
                    PAD1_poll(0);
                    *buf = PAD1_poll(0) << 8;
                    *buf |= PAD1_poll(0);
                    PAD1_poll(0);
                    *buf &= ~((PAD1_poll(0) > 0x20) ? 1 << 6 : 0);
                    *buf &= ~((PAD1_poll(0) > 0x20) ? 1 << 7 : 0);
                } else {
                    PAD1_poll(0);
                    *buf = PAD1_poll(0) << 8;
                    *buf|= PAD1_poll(0);
                }

                PAD2_startPoll(2);
                if (PAD2_poll(0x42) == 0x23) {
                    PAD2_poll(0);
                    *buf |= PAD2_poll(0) << 24;
                    *buf |= PAD2_poll(0) << 16;
                    PAD2_poll(0);
                    *buf &= ~((PAD2_poll(0) > 0x20) ? 1 << 22 : 0);
                    *buf &= ~((PAD2_poll(0) > 0x20) ? 1 << 23 : 0);
                } else {
                    PAD2_poll(0);
                    *buf |= PAD2_poll(0) << 24;
                    *buf |= PAD2_poll(0) << 16;
                }
            } else {
                u16 data;

                PAD1_startPoll(1);
                PAD1_poll(0x42);
                PAD1_poll(0);
                data = PAD1_poll(0) << 8;
                data |= PAD1_poll(0);

                if (NET_sendPadData(&data, 2) == -1)
                    netError();

                if (NET_recvPadData(&((u16*)buf)[0], 1) == -1)
                    netError();
                if (NET_recvPadData(&((u16*)buf)[1], 2) == -1)
                    netError();
            }
        }
        if (Config.UseNet && pad_buf1 != NULL && pad_buf2 != NULL) {
            psxBios_PADpoll(1);

            if (NET_sendPadData(pad_buf1, i) == -1)
                netError();

            if (NET_recvPadData(pad_buf1, 1) == -1)
                netError();
            if (NET_recvPadData(pad_buf2, 2) == -1)
                netError();
        } else {
            if (pad_buf1) {
                psxBios_PADpoll(1);
            }

            if (pad_buf2) {
                psxBios_PADpoll(2);
            }
        }

    if (psxHu32(0x1070) & 0x1) { // Vsync
        if (RcEV[3][1].status == EvStACTIVE) {
            softCall(RcEV[3][1].fhandler);
//			hwWrite32(0x1f801070, ~(1));
        }
    }

    if (psxHu32(0x1070) & 0x70) { // Rcnt 0,1,2
        int i;

        for (i = 0; i < 3; i++) {
            if (psxHu32(0x1070) & (1 << (i + 4))) {
                if (RcEV[i][1].status == EvStACTIVE) {
                    softCall(RcEV[i][1].fhandler);
                }
                psxHwWrite32(0x1f801070, ~(1 << (i + 4)));
            }
        }
    }
}
#endif 

#if HLE_ENABLE_EXCEPTION
void psxBiosException() {
    int i;

    switch (CP0_CAUSE & 0x3c) {
        case 0x00: // Interrupt
            SaveRegs();

            sp = psxMu32(0x6c80); // create new stack for interrupt handlers

            biosInterrupt();

            for (i = 0; i < 8; i++) {
                if (SysIntRP[i]) {
                    u32 *queue = (u32 *)PSXM(SysIntRP[i]);

                    s0 = queue[2];
                    softCall(queue[1]);
                }
            }

            if (jmp_int != NULL) {
                int i;

                Write_ISTAT(0xffffffff);

                ra = jmp_int[0];
                sp = jmp_int[1];
                fp = jmp_int[2];
                for (i = 0; i < 8; i++) // s0-s7
                     GPR_ARRAY[16 + i] = jmp_int[3 + i];
                gp = jmp_int[11];

                v0 = 1;
                pc0 = ra;
                return;
            }
            Write_ISTAT(0);
            break;

        case 0x20: // Syscall
#ifdef PSXCPU_LOG
            PSXCPU_LOG("syscall exp %x\n", a0);
#endif
            switch (a0) {
                case 1: // EnterCritical - disable irq's
                    CP0_STATUS &= ~0x404; 
                    v0=1;	// HDHOSHY experimental patch: Spongebob, Coldblood, fearEffect, Medievil2, Martian Gothic
                    break;

                case 2: // ExitCritical - enable irq's
                    CP0_STATUS |= 0x404; 
                    break;
            }
            pc0 = CP0_EPC + 4;

            CP0_STATUS = (CP0_STATUS & 0xfffffff0) |
                        ((CP0_STATUS & 0x3c) >> 2);
            return;

        default:
            PSXBIOS_LOG("unknown bios exception!\n");
            break;
    }

    pc0 = CP0_EPC;
    if (CP0_CAUSE & 0x80000000) pc0+=4;

    CP0_STATUS = (CP0_STATUS & 0xfffffff0) |
                ((CP0_STATUS & 0x3c) >> 2);
}
#endif

bool psxbios_invoke_any(const HLE_BIOS_TABLE& table, const char * const names[256]) {
    int call = t1 & 0xff;

    if (table[call]) {
        PSXBIOS_LOG("PSX_BIOS: %s\n", names[call]);
        table[call]();
        return 1;
    }

    return 0;
}

bool psxbios_invoke_A0() { return psxbios_invoke_any(biosA0, biosA0n); }
bool psxbios_invoke_B0() { return psxbios_invoke_any(biosB0, biosB0n); }
bool psxbios_invoke_C0() { return psxbios_invoke_any(biosC0, biosC0n); }

bool HleDispatchCall(u32 pc) {

    if (IsHlePC(pc)) {
        auto id = HleGetCallId(pc);
        assert((u32)id < SCRI_MAX_COUNT);
        HLE_Call_Table[id]();
        return 1;
    }

    if(pc == 0xA0) {
        return psxbios_invoke_A0();
    }

    if(pc == 0xB0)
    {
        return psxbios_invoke_B0();
    }

    if(pc == 0xC0)
    {
        return psxbios_invoke_C0();
    }

    return 0;
}


// SAVESTATE

#if 0
#define bfreeze(ptr, size) { \
    if (Mode == 1) memcpy(&psxR[base], ptr, size); \
    if (Mode == 0) memcpy(ptr, &psxR[base], size); \
    base += size; \
}

#define bfreezes(ptr) bfreeze(ptr, sizeof(ptr))
#define bfreezel(ptr) bfreeze(ptr, sizeof(*ptr))

#define bfreezepsxMptr(ptr, type) { \
    if (Mode == 1) { \
        if (ptr) psxRu32ref(base) = SWAPu32((s8 *)(ptr) - psxM); \
        else psxRu32ref(base) = 0; \
    } else { \
        if (psxRu32(base) != 0) ptr = (type *)(psxM + psxRu32(base)); \
        else (ptr) = NULL; \
    } \
    base += sizeof(u32); \
}

void psxBiosFreeze(int Mode) {
    u32 base = 0x40000;

    bfreezepsxMptr(jmp_int, u32);
    bfreezepsxMptr(pad_buf, int);
    bfreezepsxMptr(pad_buf1, char);
    bfreezepsxMptr(pad_buf2, char);
    bfreezepsxMptr(heap_addr, u32);
    bfreezel(&pad_buf1len);
    bfreezel(&pad_buf2len);
    bfreezes(regs);
    bfreezes(SysIntRP);
    bfreezel(&CardState);
    bfreezes(ThreadCB);
    bfreezel(&CurThread);
    bfreezes(FDesc);
    bfreezel(&card_active_chan);
}
#endif
