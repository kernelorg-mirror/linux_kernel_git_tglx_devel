/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IOBITMAP_DEFS_H
#define _ASM_X86_IOBITMAP_DEFS_H

/* IO-bitmap sizes */
#define IO_BITMAP_BITS			65536
#define IO_BITMAP_BYTES			(IO_BITMAP_BITS / BITS_PER_BYTE)
#define IO_BITMAP_LONGS			(IO_BITMAP_BYTES / sizeof(long))

#define IO_BITMAP_OFFSET_VALID_MAP				\
	(offsetof(struct tss_struct, io_bitmap.bitmap) -	\
	 offsetof(struct tss_struct, x86_tss))

#define IO_BITMAP_OFFSET_VALID_ALL				\
	(offsetof(struct tss_struct, io_bitmap.mapall) -	\
	 offsetof(struct tss_struct, x86_tss))

#ifdef CONFIG_X86_IOPL_IOPERM
/*
 * sizeof(unsigned long) coming from an extra "long" at the end of the
 * iobitmap. The limit is inclusive, i.e. the last valid byte.
 */
# define __KERNEL_TSS_LIMIT	\
	(IO_BITMAP_OFFSET_VALID_ALL + IO_BITMAP_BYTES + \
	 sizeof(unsigned long) - 1)
#else
# define __KERNEL_TSS_LIMIT	\
	(offsetof(struct tss_struct, x86_tss) + sizeof(struct x86_hw_tss) - 1)
#endif

/* Base offset outside of TSS_LIMIT so unpriviledged IO causes #GP */
#define IO_BITMAP_OFFSET_INVALID	(__KERNEL_TSS_LIMIT + 1)

#endif
