// SPDX-License-Identifier: GPL-2.0
#include <linux/cpu.h>

#include <asm/apic.h>
#include <asm/memtype.h>
#include <asm/processor.h>

#include "cpu.h"

enum topo_types {
	INVALID_TYPE	= 0,
	SMT_TYPE	= 1,
	CORE_TYPE	= 2,
	MODULE_TYPE	= 3,
	TILE_TYPE	= 4,
	DIE_TYPE	= 5,
	DIEGRP_TYPE	= 6,
	MAX_TYPE	= 7,
};

/*
 * Use a lookup table for the case that there are future types > 6 which
 * describe an intermediate domain level which does not exist today.
 *
 * A table will also be handy to parse the new AMD 0x80000026 leaf which
 * has defined different domain types, but otherwise uses the same layout
 * with some of the reserved bits used for new information.
 */
static const unsigned int topo_domain_map[MAX_TYPE] = {
	[SMT_TYPE]	= TOPO_SMT_DOMAIN,
	[CORE_TYPE]	= TOPO_CORE_DOMAIN,
	[MODULE_TYPE]	= TOPO_MODULE_DOMAIN,
	[TILE_TYPE]	= TOPO_TILE_DOMAIN,
	[DIE_TYPE]	= TOPO_DIE_DOMAIN,
	[DIEGRP_TYPE]	= TOPO_PKG_DOMAIN,
};

static inline bool topo_subleaf(struct topo_scan *tscan, u32 leaf, u32 subleaf)
{
	unsigned int dom, maxtype = leaf == 0xb ? CORE_TYPE + 1 : MAX_TYPE;
	struct {
		// eax
		u32	x2apic_shift	:  5, // Number of bits to shift APIC ID right
					      // for the topology ID at the next level
			__rsvd0		: 27; // Reserved
		// ebx
		u32	num_processors	: 16, // Number of processors at current level
			__rsvd1		: 16; // Reserved
		// ecx
		u32	level		:  8, // Current topology level. Same as sub leaf number
			type		:  8, // Level type. If 0, invalid
			__rsvd2		: 16; // Reserved
		// edx
		u32	x2apic_id	: 32; // X2APIC ID of the current logical processor
	} sl;

	cpuid_subleaf(leaf, subleaf, &sl);

	if (!sl.num_processors || sl.type == INVALID_TYPE)
		return false;

	if (sl.type >= maxtype) {
		/*
		 * As the subleafs are ordered in domain level order, this
		 * could be recovered in theory by propagating the
		 * information at the last parsed level.
		 *
		 * But if the infinite wisdom of hardware folks decides to
		 * create a new domain type between CORE and MODULE or DIE
		 * and DIEGRP, then that would overwrite the CORE or DIE
		 * information.
		 *
		 * It really would have been too obvious to make the domain
		 * type space sparse and leave a few reserved types between
		 * the points which might change instead of forcing
		 * software to either create a monstrosity of workarounds
		 * or just being up the creek without a paddle.
		 *
		 * Refuse to implement monstrosity, emit an error and try
		 * to survive.
		 */
		pr_err_once("Topology: leaf 0x%x:%d Unknown domain type %u\n",
			    leaf, subleaf, sl.type);
		return true;
	}

	dom = topo_domain_map[sl.type];
	if (!dom) {
		tscan->c->topo.initial_apicid = sl.x2apic_id;
	} else if (tscan->c->topo.initial_apicid != sl.x2apic_id) {
		pr_warn_once(FW_BUG "CPUID leaf 0x%x subleaf %d APIC ID mismatch %x != %x\n",
			     leaf, subleaf, tscan->c->topo.initial_apicid, sl.x2apic_id);
	}

	topology_set_dom(tscan, dom, sl.x2apic_shift, sl.num_processors);
	return true;
}

static bool parse_topology_leaf(struct topo_scan *tscan, u32 leaf)
{
	u32 subleaf;

	if (tscan->c->cpuid_level < leaf)
		return false;

	/* Read all available subleafs and populate the levels */
	for (subleaf = 0; topo_subleaf(tscan, leaf, subleaf); subleaf++);

	/* If subleaf 0 failed to parse, give up */
	if (!subleaf)
		return false;

	/*
	 * There are machines in the wild which have shift 0 in the subleaf
	 * 0, but advertise 2 logical processors at that level. They are
	 * truly SMT.
	 */
	if (!tscan->dom_shifts[TOPO_SMT_DOMAIN] && tscan->dom_ncpus[TOPO_SMT_DOMAIN] > 1) {
		unsigned int sft = get_count_order(tscan->dom_ncpus[TOPO_SMT_DOMAIN]);

		pr_warn_once(FW_BUG "CPUID leaf 0x%x subleaf 0 has shift level 0 but %u CPUs\n",
			     leaf, tscan->dom_ncpus[TOPO_SMT_DOMAIN]);
		topology_update_dom(tscan, TOPO_SMT_DOMAIN, sft, tscan->dom_ncpus[TOPO_SMT_DOMAIN]);
	}

	set_cpu_cap(tscan->c, X86_FEATURE_XTOPOLOGY);
	return true;
}

bool cpu_parse_topology_ext(struct topo_scan *tscan)
{
	/* Try lead 0x1F first. If not available try leaf 0x0b */
	if (parse_topology_leaf(tscan, 0x1f))
		return true;
	return parse_topology_leaf(tscan, 0x0b);
}
