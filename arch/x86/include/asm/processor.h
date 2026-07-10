/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PROCESSOR_H
#define _ASM_X86_PROCESSOR_H

#include <asm/processor-flags.h>

/* Forward declaration, a strange C thing */
struct task_struct;
struct mm_struct;
struct io_bitmap;
struct vm86;

#include <asm/cpufeatures.h>
#include <asm/cpuid/types.h>
#include <asm/cpuinfo.h>
#include <asm/current.h>
#include <asm/desc_defs.h>
#include <asm/fpu/types.h>
#include <asm/io_bitmap_defs.h>
#include <asm/math_emu.h>
#include <asm/nops.h>
#include <asm/page.h>
#include <asm/percpu.h>
#include <asm/pgtable_types.h>
#include <asm/segment.h>
#include <asm/shstk.h>
#include <asm/special_insns.h>
#include <asm/types.h>
#include <asm/unwind_hints.h>
#include <asm/vdso/processor.h>
#include <asm/vmxfeatures.h>
#include <uapi/asm/sigcontext.h>

#include <linux/cache.h>
#include <linux/err.h>
#include <linux/irqflags.h>
#include <linux/math64.h>
#include <linux/mem_encrypt.h>
#include <linux/personality.h>
#include <linux/threads.h>

/*
 * We handle most unaligned accesses in hardware.  On the other hand
 * unaligned DMA can be quite expensive on some Nehalem processors.
 *
 * Based on this we disable the IP header alignment in network drivers.
 */
#define NET_IP_ALIGN	0

#define HBP_NUM 4

/*
 * These alignment constraints are for performance in the vSMP case,
 * but in the task_struct case we must also meet hardware imposed
 * alignment requirements of the FPU state:
 */
#ifdef CONFIG_X86_VSMP
# define ARCH_MIN_TASKALIGN		(1 << INTERNODE_CACHE_SHIFT)
# define ARCH_MIN_MMSTRUCT_ALIGN	(1 << INTERNODE_CACHE_SHIFT)
#else
# define ARCH_MIN_TASKALIGN		__alignof__(union fpregs_state)
# define ARCH_MIN_MMSTRUCT_ALIGN	0
#endif

/*
 * CPU type and hardware bug flags. Kept separately for each CPU.
 */
/*
 * capabilities of CPUs
 */

DECLARE_PER_CPU_READ_MOSTLY(struct cpuinfo_x86, cpu_info);
#define cpu_data(cpu)		per_cpu(cpu_info, cpu)

static inline unsigned long long l1tf_pfn_limit(void)
{
	return BIT_ULL(boot_cpu_data.x86_cache_bits - 1 - PAGE_SHIFT);
}

/*
 * Friendlier CR3 helpers.
 */
static inline unsigned long read_cr3_pa(void)
{
	return __read_cr3() & CR3_ADDR_MASK;
}

static inline unsigned long native_read_cr3_pa(void)
{
	return __native_read_cr3() & CR3_ADDR_MASK;
}

static inline void load_cr3(pgd_t *pgdir)
{
	write_cr3(__sme_pa(pgdir));
}

/*
 * Note that while the legacy 'TSS' name comes from 'Task State Segment',
 * on modern x86 CPUs the TSS also holds information important to 64-bit mode,
 * unrelated to the task-switch mechanism:
 */
#ifdef CONFIG_X86_32
/* This is the TSS defined by the hardware. */
struct x86_hw_tss {
	unsigned short		back_link, __blh;
	unsigned long		sp0;
	unsigned short		ss0, __ss0h;
	unsigned long		sp1;

	/*
	 * We don't use ring 1, so ss1 is a convenient scratch space in
	 * the same cacheline as sp0.  We use ss1 to cache the value in
	 * MSR_IA32_SYSENTER_CS.  When we context switch
	 * MSR_IA32_SYSENTER_CS, we first check if the new value being
	 * written matches ss1, and, if it's not, then we wrmsr the new
	 * value and update ss1.
	 *
	 * The only reason we context switch MSR_IA32_SYSENTER_CS is
	 * that we set it to zero in vm86 tasks to avoid corrupting the
	 * stack if we were to go through the sysenter path from vm86
	 * mode.
	 */
	unsigned short		ss1;	/* MSR_IA32_SYSENTER_CS */

	unsigned short		__ss1h;
	unsigned long		sp2;
	unsigned short		ss2, __ss2h;
	unsigned long		__cr3;
	unsigned long		ip;
	unsigned long		flags;
	unsigned long		ax;
	unsigned long		cx;
	unsigned long		dx;
	unsigned long		bx;
	unsigned long		sp;
	unsigned long		bp;
	unsigned long		si;
	unsigned long		di;
	unsigned short		es, __esh;
	unsigned short		cs, __csh;
	unsigned short		ss, __ssh;
	unsigned short		ds, __dsh;
	unsigned short		fs, __fsh;
	unsigned short		gs, __gsh;
	unsigned short		ldt, __ldth;
	unsigned short		trace;
	unsigned short		io_bitmap_base;

} __attribute__((packed));
#else
struct x86_hw_tss {
	u32			reserved1;
	u64			sp0;
	u64			sp1;

	/*
	 * Since Linux does not use ring 2, the 'sp2' slot is unused by
	 * hardware.  entry_SYSCALL_64 uses it as scratch space to stash
	 * the user RSP value.
	 */
	u64			sp2;

	u64			reserved2;
	u64			ist[7];
	u32			reserved3;
	u32			reserved4;
	u16			reserved5;
	u16			io_bitmap_base;

} __attribute__((packed));
#endif

struct entry_stack {
	char	stack[PAGE_SIZE];
};

struct entry_stack_page {
	struct entry_stack stack;
} __aligned(PAGE_SIZE);

/*
 * All IO bitmap related data stored in the TSS:
 */
struct x86_io_bitmap {
	/* The sequence number of the last active bitmap. */
	u64			prev_sequence;

	/*
	 * Store the dirty size of the last io bitmap offender. The next
	 * one will have to do the cleanup as the switch out to a non io
	 * bitmap user will just set x86_tss.io_bitmap_base to a value
	 * outside of the TSS limit. So for sane tasks there is no need to
	 * actually touch the io_bitmap at all.
	 */
	unsigned int		prev_max;

	/*
	 * The extra 1 is there because the CPU will access an
	 * additional byte beyond the end of the IO permission
	 * bitmap. The extra byte must be all 1 bits, and must
	 * be within the limit.
	 */
	unsigned long		bitmap[IO_BITMAP_LONGS + 1];

	/*
	 * Special I/O bitmap to emulate IOPL(3). All bytes zero,
	 * except the additional byte at the end.
	 */
	unsigned long		mapall[IO_BITMAP_LONGS + 1];
};

struct tss_struct {
	/*
	 * The fixed hardware portion.  This must not cross a page boundary
	 * at risk of violating the SDM's advice and potentially triggering
	 * errata.
	 */
	struct x86_hw_tss	x86_tss;

	struct x86_io_bitmap	io_bitmap;
} __aligned(PAGE_SIZE);

DECLARE_PER_CPU_PAGE_ALIGNED(struct tss_struct, cpu_tss_rw);

/* Per CPU interrupt stacks */
struct irq_stack {
	char		stack[IRQ_STACK_SIZE];
} __aligned(IRQ_STACK_SIZE);

DECLARE_PER_CPU_CACHE_HOT(struct irq_stack *, hardirq_stack_ptr);
#ifdef CONFIG_X86_64
DECLARE_PER_CPU_CACHE_HOT(bool, hardirq_stack_inuse);
#else
DECLARE_PER_CPU_CACHE_HOT(struct irq_stack *, softirq_stack_ptr);
#endif

DECLARE_PER_CPU_CACHE_HOT(unsigned long, cpu_current_top_of_stack);
/* const-qualified alias provided by the linker. */
DECLARE_PER_CPU_CACHE_HOT(const unsigned long __percpu_seg_override,
			  const_cpu_current_top_of_stack);

#ifdef CONFIG_X86_64
static inline unsigned long cpu_kernelmode_gs_base(int cpu)
{
#ifdef CONFIG_SMP
	return per_cpu_offset(cpu);
#else
	return 0;
#endif
}

extern asmlinkage void entry_SYSCALL32_ignore(void);

/* Save actual FS/GS selectors and bases to current->thread */
void current_save_fsgs(void);
#endif	/* X86_64 */

struct perf_event;

struct thread_struct {
	/* Cached TLS descriptors: */
	struct desc_struct	tls_array[GDT_ENTRY_TLS_ENTRIES];
#ifdef CONFIG_X86_32
	unsigned long		sp0;
#endif
	unsigned long		sp;
#ifdef CONFIG_X86_32
	unsigned long		sysenter_cs;
#else
	unsigned short		es;
	unsigned short		ds;
	unsigned short		fsindex;
	unsigned short		gsindex;
#endif

#ifdef CONFIG_X86_64
	unsigned long		fsbase;
	unsigned long		gsbase;
#else
	/*
	 * XXX: this could presumably be unsigned short.  Alternatively,
	 * 32-bit kernels could be taught to use fsindex instead.
	 */
	unsigned long fs;
	unsigned long gs;
#endif

	/* Save middle states of ptrace breakpoints */
	struct perf_event	*ptrace_bps[HBP_NUM];
	/* Debug status used for traps, single steps, etc... */
	unsigned long           virtual_dr6;
	/* Keep track of the exact dr7 value set by the user */
	unsigned long           ptrace_dr7;
	/* Fault info: */
	unsigned long		cr2;
	unsigned long		trap_nr;
	unsigned long		error_code;
#ifdef CONFIG_VM86
	/* Virtual 86 mode info */
	struct vm86		*vm86;
#endif
	/* IO permissions: */
	struct io_bitmap	*io_bitmap;

	/*
	 * IOPL. Privilege level dependent I/O permission which is
	 * emulated via the I/O bitmap to prevent user space from disabling
	 * interrupts.
	 */
	unsigned long		iopl_emul;

	unsigned int		iopl_warn:1;

	/*
	 * Protection Keys Register for Userspace.  Loaded immediately on
	 * context switch. Store it in thread_struct to avoid a lookup in
	 * the tasks's FPU xstate buffer. This value is only valid when a
	 * task is scheduled out. For 'current' the authoritative source of
	 * PKRU is the hardware itself.
	 */
	u32			pkru;

#ifdef CONFIG_X86_USER_SHADOW_STACK
	unsigned long		features;
	unsigned long		features_locked;

	struct thread_shstk	shstk;
#endif
};

#ifdef CONFIG_X86_DEBUG_FPU
extern struct fpu *x86_task_fpu(struct task_struct *task);
#else
# define x86_task_fpu(task)	((struct fpu *)((void *)(task) + sizeof(*(task))))
#endif

extern void fpu_thread_struct_whitelist(unsigned long *offset, unsigned long *size);

static inline void arch_thread_struct_whitelist(unsigned long *offset,
						unsigned long *size)
{
	fpu_thread_struct_whitelist(offset, size);
}

static inline void
native_load_sp0(unsigned long sp0)
{
	this_cpu_write(cpu_tss_rw.x86_tss.sp0, sp0);
}

static __always_inline void native_swapgs(void)
{
#ifdef CONFIG_X86_64
	asm volatile("swapgs" ::: "memory");
#endif
}

static __always_inline unsigned long current_top_of_stack(void)
{
	/*
	 *  We can't read directly from tss.sp0: sp0 on x86_32 is special in
	 *  and around vm86 mode and sp0 on x86_64 is special because of the
	 *  entry trampoline.
	 */
	if (IS_ENABLED(CONFIG_USE_X86_SEG_SUPPORT))
		return this_cpu_read_const(const_cpu_current_top_of_stack);

	return this_cpu_read_stable(cpu_current_top_of_stack);
}

static __always_inline bool on_thread_stack(void)
{
	return (unsigned long)(current_top_of_stack() -
			       current_stack_pointer) < THREAD_SIZE;
}

#ifdef CONFIG_PARAVIRT_XXL
#include <asm/paravirt.h>
#else

static inline void load_sp0(unsigned long sp0)
{
	native_load_sp0(sp0);
}

#endif /* CONFIG_PARAVIRT_XXL */

unsigned long __get_wchan(struct task_struct *p);

extern void select_idle_routine(void);
extern void amd_e400_c1e_apic_setup(void);

extern unsigned long		boot_option_idle_override;

enum idle_boot_override {IDLE_NO_OVERRIDE=0, IDLE_HALT, IDLE_NOMWAIT,
			 IDLE_POLL};

extern void enable_sep_cpu(void);


/* Defined in head.S */
extern struct desc_ptr		early_gdt_descr;

extern void switch_gdt_and_percpu_base(int);
extern void load_direct_gdt(int);
extern void load_fixmap_gdt(int);
extern void cpu_init(void);
extern void cpu_init_exception_handling(bool boot_cpu);
extern void cpu_init_replace_early_idt(void);
extern void cr4_init(void);

extern void set_task_blockstep(struct task_struct *task, bool on);

/* Boot loader type from the setup header: */
extern int			bootloader_type;
extern int			bootloader_version;

extern char			ignore_fpu_irq;

#define HAVE_ARCH_PICK_MMAP_LAYOUT 1
#define ARCH_HAS_PREFETCHW

#ifdef CONFIG_X86_32
# define BASE_PREFETCH		""
# define ARCH_HAS_PREFETCH
#else
# define BASE_PREFETCH		"prefetcht0 %1"
#endif

/*
 * Prefetch instructions for Pentium III (+) and AMD Athlon (+)
 *
 * It's not worth to care about 3dnow prefetches for the K6
 * because they are microcoded there and very slow.
 */
static inline void prefetch(const void *x)
{
	alternative_input(BASE_PREFETCH, "prefetchnta %1",
			  X86_FEATURE_XMM,
			  "m" (*(const char *)x));
}

/*
 * 3dnow prefetch to get an exclusive cache line.
 * Useful for spinlocks to avoid one state transition in the
 * cache coherency protocol:
 */
static __always_inline void prefetchw(const void *x)
{
	alternative_input(BASE_PREFETCH, "prefetchw %1",
			  X86_FEATURE_3DNOWPREFETCH,
			  "m" (*(const char *)x));
}

#define TOP_OF_INIT_STACK ((unsigned long)&init_stack + sizeof(init_stack) - \
			   TOP_OF_KERNEL_STACK_PADDING)

#define task_top_of_stack(task) ((unsigned long)(task_pt_regs(task) + 1))

#define task_pt_regs(task) \
({									\
	unsigned long __ptr = (unsigned long)task_stack_page(task);	\
	__ptr += THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;		\
	((struct pt_regs *)__ptr) - 1;					\
})

#ifdef CONFIG_X86_32
#define INIT_THREAD  {							  \
	.sp0			= TOP_OF_INIT_STACK,			  \
	.sysenter_cs		= __KERNEL_CS,				  \
}

#else
extern unsigned long __top_init_kernel_stack[];

#define INIT_THREAD {							\
	.sp	= (unsigned long)&__top_init_kernel_stack,		\
}

#endif /* CONFIG_X86_64 */

extern void start_thread(struct pt_regs *regs, unsigned long new_ip,
					       unsigned long new_sp);

/*
 * This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define __TASK_UNMAPPED_BASE(task_size)	(PAGE_ALIGN(task_size / 3))
#define TASK_UNMAPPED_BASE		__TASK_UNMAPPED_BASE(TASK_SIZE_LOW)

#define KSTK_EIP(task)		(task_pt_regs(task)->ip)
#define KSTK_ESP(task)		(task_pt_regs(task)->sp)

/* Get/set a process' ability to use the timestamp counter instruction */
#define GET_TSC_CTL(adr)	get_tsc_mode((adr))
#define SET_TSC_CTL(val)	set_tsc_mode((val))

extern int get_tsc_mode(unsigned long adr);
extern int set_tsc_mode(unsigned int val);

DECLARE_PER_CPU(u64, msr_misc_features_shadow);

static inline u32 per_cpu_llc_id(unsigned int cpu)
{
	return per_cpu(cpu_info.topo.llc_id, cpu);
}

static inline u32 per_cpu_l2c_id(unsigned int cpu)
{
	return per_cpu(cpu_info.topo.l2c_id, cpu);
}

static inline u32 per_cpu_core_id(unsigned int cpu)
{
	return per_cpu(cpu_info.topo.core_id, cpu);
}

#ifdef CONFIG_CPU_SUP_AMD
/*
 * Issue a DIV 0/1 insn to clear any division data from previous DIV
 * operations.
 */
static __always_inline void amd_clear_divider(void)
{
	asm volatile(ALTERNATIVE("", "div %2\n\t", X86_BUG_DIV0)
		     :: "a" (0), "d" (0), "r" (1));
}
#else
static inline void amd_clear_divider(void)		{ }
#endif

extern unsigned long arch_align_stack(unsigned long sp);
void free_init_pages(const char *what, unsigned long begin, unsigned long end);
extern void free_kernel_image_pages(const char *what, void *begin, void *end);

void __noreturn stop_this_cpu(void *dummy);
extern bool x86_hypervisor_present;
void store_cpu_caps(struct cpuinfo_x86 *info);

DECLARE_PER_CPU(bool, cache_state_incoherent);

#endif /* _ASM_X86_PROCESSOR_H */
