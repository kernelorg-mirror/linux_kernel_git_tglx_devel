/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CPUINFO_H
#define _ASM_CPUINFO_H

#include <asm/cpufeatures.h>
#include <asm/vmxfeatures.h>
#include <asm/cpuid/types.h>

/*
 * CPU type and hardware bug flags. Kept separately for each CPU.
 */
struct cpuinfo_topology {
	// Real APIC ID read from the local APIC
	u32			apicid;
	// The initial APIC ID provided by CPUID
	u32			initial_apicid;

	// Physical package ID
	u32			pkg_id;

	// Physical die ID on AMD, Relative on Intel
	u32			die_id;

	// Compute unit ID - AMD specific
	u32			cu_id;

	// Core ID relative to the package
	u32			core_id;

	// Logical ID mappings
	u32			logical_pkg_id;
	u32			logical_die_id;
	u32			logical_core_id;

	// AMD Node ID and Nodes per Package info
	u32			amd_node_id;

	// Cache level topology IDs
	u32			llc_id;
	u32			l2c_id;

	// Hardware defined CPU-type
	union {
		u32		cpu_type;
		struct {
			// CPUID.1A.EAX[23-0]
			u32	intel_native_model_id	:24;
			// CPUID.1A.EAX[31-24]
			u32	intel_type		:8;
		};
		struct {
			// CPUID 0x80000026.EBX
			u32	amd_num_processors	:16,
				amd_power_eff_ranking	:8,
				amd_native_model_id	:4,
				amd_type		:4;
		};
	};
};

struct cpuinfo_x86 {
	union {
		/*
		 * The particular ordering (low-to-high) of (vendor,
		 * family, model) is done in case range of models, like
		 * it is usually done on AMD, need to be compared.
		 */
		struct {
			__u8	x86_model;
			/* CPU family */
			__u8	x86;
			/* CPU vendor */
			__u8	x86_vendor;
			__u8	x86_reserved;
		};
		/* combined vendor, family, model */
		__u32		x86_vfm;
	};
	__u8			x86_stepping;
	union {
		// MSR_IA32_PLATFORM_ID[52-50]
		__u8			intel_platform_id;
		__u8			amd_unused;
	};
#ifdef CONFIG_X86_64
	/* Number of 4K pages in DTLB/ITLB combined(in pages): */
	int			x86_tlbsize;
#endif
#ifdef CONFIG_X86_VMX_FEATURE_NAMES
	__u32			vmx_capability[NVMXINTS];
#endif
	__u8			x86_virt_bits;
	__u8			x86_phys_bits;
	/* Max extended CPUID function supported: */
	__u32			extended_cpuid_level;
	/* Maximum supported CPUID level, -1=no CPUID: */
	int			cpuid_level;
	/*
	 * Align to size of unsigned long because the x86_capability array
	 * is passed to bitops which require the alignment. Use unnamed
	 * union to enforce the array is aligned to size of unsigned long.
	 */
	union {
		__u32		x86_capability[NCAPINTS + NBUGINTS];
		unsigned long	x86_capability_alignment;
	};
	char			x86_vendor_id[16];
	char			x86_model_id[64];
	struct cpuinfo_topology	topo;
	struct cpuid_table	cpuid;
	/* in KB - valid for CPUS which support this call: */
	unsigned int		x86_cache_size;
	int			x86_cache_alignment;	/* In bytes */
	/* Cache QoS architectural values, valid only on the BSP: */
	int			x86_cache_max_rmid;	/* max index */
	int			x86_cache_occ_scale;	/* scale to bytes */
	int			x86_cache_mbm_width_offset;
	int			x86_power;
	unsigned long		loops_per_jiffy;
	/* protected processor identification number */
	u64			ppin;
	u16			x86_clflush_size;
	/* number of cores as seen by the OS: */
	u16			booted_cores;
	/* Index into per_cpu list: */
	u16			cpu_index;
	/*  Is SMT active on this core? */
	bool			smt_active;
	u32			microcode;
	/* Address space bits used by the cache internally */
	u8			x86_cache_bits;
	unsigned		initialized : 1;
} __randomize_layout;

#define X86_VENDOR_INTEL	0
#define X86_VENDOR_CYRIX	1
#define X86_VENDOR_AMD		2
#define X86_VENDOR_UMC		3
#define X86_VENDOR_CENTAUR	5
#define X86_VENDOR_TRANSMETA	7
#define X86_VENDOR_NSC		8
#define X86_VENDOR_HYGON	9
#define X86_VENDOR_ZHAOXIN	10
#define X86_VENDOR_VORTEX	11
#define X86_VENDOR_NUM		12

#define X86_VENDOR_UNKNOWN	0xff

/*
 * capabilities of CPUs
 */
extern struct cpuinfo_x86	boot_cpu_data;
extern struct cpuinfo_x86	new_cpu_data;

extern __u32			cpu_caps_cleared[NCAPINTS + NBUGINTS];
extern __u32			cpu_caps_set[NCAPINTS + NBUGINTS];

#endif /* _ASM_CPUINFO_H */
