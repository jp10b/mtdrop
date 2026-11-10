/* SPDX-License-Identifier: MIT */
/*
 * Shared between the mtdrop register-proxy driver and its clients.
 *
 * Derived from winawdl (https://github.com/raph559/winawdl) @ 0db7031,
 * driver/regproxy/regproxy_ioctl.h, Copyright (c) 2026 Raphael Richaud, MIT.
 * Changes: own interface GUID and names; version reset.
 */
#pragma once
#include <initguid.h>

/* {6f0c2b0e-7d43-4c55-9a51-3e1f6d2a9b74} */
DEFINE_GUID(GUID_DEVINTERFACE_MTDROP_REGPROXY, 0x6f0c2b0e, 0x7d43, 0x4c55,
            0x9a, 0x51, 0x3e, 0x1f, 0x6d, 0x2a, 0x9b, 0x74);

#define REGPROXY_VERSION 1u
#define REGPROXY_MAX_OPS 4096u
#define REGPROXY_MAX_CLOSE_OPS 32u

#define IOCTL_REGPROXY_GET_INFO                                                \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_REGPROXY_BATCH                                                   \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Stores register writes the driver replays when the handle closes, so DMA a
 * client enabled is stopped however it dies. An empty input clears the list. */
#define IOCTL_REGPROXY_SET_CLOSE_OPS                                           \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum {
  REGPROXY_READ = 0,
  REGPROXY_WRITE = 1,
  REGPROXY_SET_BITS = 2,   /* register |= value */
  REGPROXY_CLEAR_BITS = 3, /* register &= ~value */
};

#pragma pack(push, 1)
typedef struct _REGPROXY_INFO {
  unsigned int version;
  unsigned int bar_len;
  unsigned long long bar_phys; /* translated, as the CPU sees it */
  unsigned int direct_mapped;  /* 1 = host maps the BAR, 0 = system calls */
} REGPROXY_INFO;

/* Offsets must be naturally aligned for their width; clients split the rest. */
typedef struct _REGPROXY_OP {
  unsigned int offset;
  unsigned int value;  /* in for writes and bit ops, out for reads */
  unsigned char width; /* 1, 2 or 4 */
  unsigned char kind;  /* REGPROXY_READ and friends */
  unsigned short reserved;
} REGPROXY_OP;
#pragma pack(pop)
