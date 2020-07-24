// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/arch/arm/mm/init.c
 *
 *  Copyright (C) 1995-2005 Russell King
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/export.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/of_fdt.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/dma-contiguous.h>
#include <linux/sizes.h>
#include <linux/stop_machine.h>
#include <linux/swiotlb.h>

#include <asm/cp15.h>
#include <asm/mach-types.h>
#include <asm/memblock.h>
#include <asm/memory.h>
#include <asm/prom.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/system_info.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>
#include <asm/ptdump.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#ifdef CONFIG_BRCMSTB_MEMORY_API
#include <linux/brcmstb/memory_api.h>
#endif

#include "mm.h"

#ifdef CONFIG_ZONE_MOVABLE
unsigned long movable_start __initdata;
/*
 * Parses command line for movablebase= options
 */
static int __init movablebase_setup(char *str)
{
	phys_addr_t addr = 0;
	unsigned long movablebase;

	addr = memparse(str, &str);
	addr = PAGE_ALIGN(addr);

	movablebase = __phys_to_pfn(addr);
	if (movablebase && (!movable_start || movable_start > movablebase))
		movable_start = movablebase;

	return 0;
}
early_param("movablebase", movablebase_setup);
#endif /* CONFIG_ZONE_MOVABLE */

#ifdef CONFIG_CPU_CP15_MMU
unsigned long __init __clear_cr(unsigned long mask)
{
	cr_alignment = cr_alignment & ~mask;
	return cr_alignment;
}
#endif

#ifdef CONFIG_BLK_DEV_INITRD
static int __init parse_tag_initrd(const struct tag *tag)
{
	pr_warn("ATAG_INITRD is deprecated; "
		"please update your bootloader.\n");
	phys_initrd_start = __virt_to_phys(tag->u.initrd.start);
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD, parse_tag_initrd);

static int __init parse_tag_initrd2(const struct tag *tag)
{
	phys_initrd_start = tag->u.initrd.start;
	phys_initrd_size = tag->u.initrd.size;
	return 0;
}

__tagtable(ATAG_INITRD2, parse_tag_initrd2);
#endif

static void __init find_limits(unsigned long *min, unsigned long *max_low,
			       unsigned long *max_high)
{
	*max_low = PFN_DOWN(memblock_get_current_limit());
	*min = PFN_UP(memblock_start_of_DRAM());
	*max_high = PFN_DOWN(memblock_end_of_DRAM());
}

#ifdef CONFIG_ZONE_DMA

phys_addr_t arm_dma_zone_size __read_mostly;
EXPORT_SYMBOL(arm_dma_zone_size);

/*
 * The DMA mask corresponding to the maximum bus address allocatable
 * using GFP_DMA.  The default here places no restriction on DMA
 * allocations.  This must be the smallest DMA mask in the system,
 * so a successful GFP_DMA allocation will always satisfy this.
 */
phys_addr_t arm_dma_limit;
unsigned long arm_dma_pfn_limit;

static void __init arm_adjust_dma_zone(unsigned long *size, unsigned long *hole,
	unsigned long dma_size)
{
	if (size[0] <= dma_size)
		return;

	size[ZONE_NORMAL] = size[0] - dma_size;
	size[ZONE_DMA] = dma_size;
	hole[ZONE_NORMAL] = hole[0];
	hole[ZONE_DMA] = 0;
}
#endif

void __init setup_dma_zone(const struct machine_desc *mdesc)
{
#ifdef CONFIG_ZONE_DMA
	if (mdesc->dma_zone_size) {
		arm_dma_zone_size = mdesc->dma_zone_size;
		arm_dma_limit = PHYS_OFFSET + arm_dma_zone_size - 1;
	} else
		arm_dma_limit = 0xffffffff;
	arm_dma_pfn_limit = arm_dma_limit >> PAGE_SHIFT;
#endif
}

static void __init zone_sizes_init(unsigned long min, unsigned long max_low,
	unsigned long max_high)
{
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	struct memblock_region *reg;

	/*
	 * initialise the zones.
	 */
	memset(zone_size, 0, sizeof(zone_size));

#ifdef CONFIG_ZONE_MOVABLE
	if (movable_start) {
		WARN_ON(max_low > movable_start);
		zone_size[ZONE_MOVABLE] = max_high - movable_start;
		max_high = movable_start;
	}
#endif

	/*
	 * The memory size has already been determined.  If we need
	 * to do anything fancy with the allocation of this memory
	 * to the zones, now is the time to do it.
	 */
	zone_size[0] = max_low - min;
#ifdef CONFIG_HIGHMEM
	zone_size[ZONE_HIGHMEM] = max_high - max_low;
#endif

	/*
	 * Calculate the size of the holes.
	 *  holes = node_size - sum(bank_sizes)
	 */
	memcpy(zhole_size, zone_size, sizeof(zhole_size));
	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		if (start < max_low) {
			unsigned long low_end = min(end, max_low);
			zhole_size[0] -= low_end - start;
		}
#ifdef CONFIG_ZONE_MOVABLE
		if (end > max_high) {
			unsigned long high_start = max(start, max_high);
			zhole_size[ZONE_MOVABLE] -= end - high_start;
			if (start < max_high)
				end = max_high;
			else
				continue;
		}
#endif
#ifdef CONFIG_HIGHMEM
		if (end > max_low) {
			unsigned long high_start = max(start, max_low);
			zhole_size[ZONE_HIGHMEM] -= end - high_start;
		}
#endif
	}

#ifdef CONFIG_ZONE_DMA
	/*
	 * Adjust the sizes according to any special requirements for
	 * this machine type.
	 */
	if (arm_dma_zone_size)
		arm_adjust_dma_zone(zone_size, zhole_size,
			arm_dma_zone_size >> PAGE_SHIFT);
#endif

	free_area_init_node(0, zone_size, min, zhole_size);
}

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
int pfn_valid(unsigned long pfn)
{
	phys_addr_t addr = __pfn_to_phys(pfn);

	if (__phys_to_pfn(addr) != pfn)
		return 0;

#ifdef CONFIG_SPARSEMEM
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

	if (!valid_section(__nr_to_section(pfn_to_section_nr(pfn))))
		return 0;
#endif
	return memblock_is_map_memory(__pfn_to_phys(pfn));
}
EXPORT_SYMBOL(pfn_valid);
#endif

static bool arm_memblock_steal_permitted = true;

phys_addr_t __init arm_memblock_steal(phys_addr_t size, phys_addr_t align)
{
	phys_addr_t phys;

	BUG_ON(!arm_memblock_steal_permitted);

	phys = memblock_phys_alloc(size, align);
	if (!phys)
		panic("Failed to steal %pa bytes at %pS\n",
		      &size, (void *)_RET_IP_);

	memblock_free(phys, size);
	memblock_remove(phys, size);

	return phys;
}

static void __init arm_initrd_init(void)
{
#ifdef CONFIG_BLK_DEV_INITRD
	phys_addr_t start;
	unsigned long size;

	initrd_start = initrd_end = 0;

	if (!phys_initrd_size)
		return;

	/*
	 * Round the memory region to page boundaries as per free_initrd_mem()
	 * This allows us to detect whether the pages overlapping the initrd
	 * are in use, but more importantly, reserves the entire set of pages
	 * as we don't want these pages allocated for other purposes.
	 */
	start = round_down(phys_initrd_start, PAGE_SIZE);
	size = phys_initrd_size + (phys_initrd_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region - disabling initrd\n",
		       (u64)start, size);
		return;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region - disabling initrd\n",
		       (u64)start, size);
		return;
	}

	memblock_reserve(start, size);

	/* Now convert initrd to virtual addresses */
	initrd_start = __phys_to_virt(phys_initrd_start);
	initrd_end = initrd_start + phys_initrd_size;
#endif
}

#ifdef CONFIG_CPU_ICACHE_MISMATCH_WORKAROUND
void check_cpu_icache_size(int cpuid)
{
	u32 size, ctr;

	asm("mrc p15, 0, %0, c0, c0, 1" : "=r" (ctr));

	size = 1 << ((ctr & 0xf) + 2);
	if (cpuid != 0 && icache_size != size)
		pr_info("CPU%u: detected I-Cache line size mismatch, workaround enabled\n",
			cpuid);
	if (icache_size > size)
		icache_size = size;
}
#endif

void __init arm_memblock_init(const struct machine_desc *mdesc)
{
	/* Register the kernel text, kernel data and initrd with memblock. */
	memblock_reserve(__pa(KERNEL_START), KERNEL_END - KERNEL_START);

	arm_initrd_init();

	arm_mm_memblock_reserve();

	/* reserve any platform specific memblock areas */
	if (mdesc->reserve)
		mdesc->reserve();

	early_init_fdt_reserve_self();
	early_init_fdt_scan_reserved_mem();

#ifdef CONFIG_BRCMSTB_MEMORY_API
	brcmstb_memory_init();
#endif

	/* reserve memory for DMA contiguous allocations */
	dma_contiguous_reserve(arm_dma_limit);

	arm_memblock_steal_permitted = false;
	memblock_dump_all();
}

void __init bootmem_init(void)
{
	memblock_allow_resize();

	find_limits(&min_low_pfn, &max_low_pfn, &max_pfn);

	early_memtest((phys_addr_t)min_low_pfn << PAGE_SHIFT,
		      (phys_addr_t)max_low_pfn << PAGE_SHIFT);

	/*
	 * Sparsemem tries to allocate bootmem in memory_present(),
	 * so must be done after the fixed reservations
	 */
	memblocks_present();

	/*
	 * sparse_init() needs the bootmem allocator up and running.
	 */
	sparse_init();

	/*
	 * Now free the memory - free_area_init_node needs
	 * the sparse mem_map arrays initialized by sparse_init()
	 * for memmap_init_zone(), otherwise all PFNs are invalid.
	 */
	zone_sizes_init(min_low_pfn, max_low_pfn, max_pfn);
}

/*
 * Poison init memory with an undefined instruction (ARM) or a branch to an
 * undefined instruction (Thumb).
 */
static inline void poison_init_mem(void *s, size_t count)
{
	u32 *p = (u32 *)s;
	for (; count != 0; count -= 4)
		*p++ = 0xe7fddef0;
}

static inline void __init
free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	phys_addr_t pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and
	 * round start upwards and end downwards.
	 */
	pg = PAGE_ALIGN(__pa(start_pg));
	pgend = __pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these,
	 * free the section of the memmap array.
	 */
	if (pg < pgend)
		memblock_free_early(pg, pgend - pg);
}

/*
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, prev_end = 0;
	struct memblock_region *reg;

	/*
	 * This relies on each bank being in address order.
	 * The banks are sorted previously in bootmem_init().
	 */
	for_each_memblock(memory, reg) {
		start = memblock_region_memory_base_pfn(reg);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist
		 * due to SPARSEMEM sections which aren't present.
		 */
		start = min(start,
				 ALIGN(prev_end, PAGES_PER_SECTION));
#else
		/*
		 * Align down here since the VM subsystem insists that the
		 * memmap entries are valid from the bank start aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		start = round_down(start, MAX_ORDER_NR_PAGES);
#endif
		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_end = ALIGN(memblock_region_memory_end_pfn(reg),
				 MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION))
		free_memmap(prev_end,
			    ALIGN(prev_end, PAGES_PER_SECTION));
#endif
}

#ifdef CONFIG_HIGHMEM
static inline void free_area_high(unsigned long pfn, unsigned long end)
{
	for (; pfn < end; pfn++)
		free_highmem_page(pfn_to_page(pfn));
}
#endif

static void __init free_highpages(void)
{
#ifdef CONFIG_HIGHMEM
	unsigned long max_low = max_low_pfn;
	struct memblock_region *mem, *res;

	/* set highmem page free */
	for_each_memblock(memory, mem) {
		unsigned long start = memblock_region_memory_base_pfn(mem);
		unsigned long end = memblock_region_memory_end_pfn(mem);

		/* Ignore complete lowmem entries */
		if (end <= max_low)
			continue;

		if (memblock_is_nomap(mem))
			continue;

		/* Truncate partial highmem entries */
		if (start < max_low)
			start = max_low;

		/* Find and exclude any reserved regions */
		for_each_memblock(reserved, res) {
			unsigned long res_start, res_end;

			res_start = memblock_region_reserved_base_pfn(res);
			res_end = memblock_region_reserved_end_pfn(res);

			if (res_end < start)
				continue;
			if (res_start < start)
				res_start = start;
			if (res_start > end)
				res_start = end;
			if (res_end > end)
				res_end = end;
			if (res_start != start)
				free_area_high(start, res_start);
			start = res_end;
			if (start == end)
				break;
		}

		/* And now free anything which remains */
		if (start < end)
			free_area_high(start, end);
	}
#endif
}

/*
 * mem_init() marks the free areas in the mem_map and tells us how much
 * memory is free.  This is done after various parts of the system have
 * claimed their memory after the kernel image.
 */
void __init mem_init(void)
{
#ifdef CONFIG_ARM_LPAE
	swiotlb_init(1);
#endif

	set_max_mapnr(pfn_to_page(max_pfn) - mem_map);

	/* this will put all unused low memory onto the freelists */
	free_unused_memmap();
	memblock_free_all();

#ifdef CONFIG_SA1111
	/* now that our DMA memory is actually so designated, we can free it */
	free_reserved_area(__va(PHYS_OFFSET), swapper_pg_dir, -1, NULL);
#endif

	free_highpages();

	mem_init_print_info(NULL);

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can
	 * be detected at build time already.
	 */
#ifdef CONFIG_MMU
	BUILD_BUG_ON(TASK_SIZE				> MODULES_VADDR);
	BUG_ON(TASK_SIZE 				> MODULES_VADDR);
#endif

#ifdef CONFIG_HIGHMEM
	BUILD_BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE > PAGE_OFFSET);
	BUG_ON(PKMAP_BASE + LAST_PKMAP * PAGE_SIZE	> PAGE_OFFSET);
#endif
}

#ifdef CONFIG_STRICT_KERNEL_RWX
struct section_perm {
	const char *name;
	unsigned long start;
	unsigned long end;
	pmdval_t mask;
	pmdval_t prot;
	pmdval_t clear;
};

/* First section-aligned location at or after __start_rodata. */
extern char __start_rodata_section_aligned[];

static struct section_perm nx_perms[] = {
	/* Make pages tables, etc before _stext RW (set NX). */
	{
		.name	= "pre-text NX",
		.start	= PAGE_OFFSET,
		.end	= (unsigned long)_stext,
		.mask	= ~PMD_SECT_XN,
		.prot	= PMD_SECT_XN,
	},
	/* Make init RW (set NX). */
	{
		.name	= "init NX",
		.start	= (unsigned long)__init_begin,
		.end	= (unsigned long)_sdata,
		.mask	= ~PMD_SECT_XN,
		.prot	= PMD_SECT_XN,
	},
	/* Make rodata NX (set RO in ro_perms below). */
	{
		.name	= "rodata NX",
		.start  = (unsigned long)__start_rodata_section_aligned,
		.end    = (unsigned long)__init_begin,
		.mask   = ~PMD_SECT_XN,
		.prot   = PMD_SECT_XN,
	},
};

static struct section_perm ro_perms[] = {
	/* Make kernel code and rodata RX (set RO). */
	{
		.name	= "text/rodata RO",
		.start  = (unsigned long)_stext,
		.end    = (unsigned long)__init_begin,
#ifdef CONFIG_ARM_LPAE
		.mask   = ~(L_PMD_SECT_RDONLY | PMD_SECT_AP2),
		.prot   = L_PMD_SECT_RDONLY | PMD_SECT_AP2,
#else
		.mask   = ~(PMD_SECT_APX | PMD_SECT_AP_WRITE),
		.prot   = PMD_SECT_APX | PMD_SECT_AP_WRITE,
		.clear  = PMD_SECT_AP_WRITE,
#endif
	},
};

/*
 * Updates section permissions only for the current mm (sections are
 * copied into each mm). During startup, this is the init_mm. Is only
 * safe to be called with preemption disabled, as under stop_machine().
 */
static inline void section_update(unsigned long addr, pmdval_t mask,
				  pmdval_t prot, struct mm_struct *mm)
{
	pmd_t *pmd;

	pmd = pmd_offset(pud_offset(pgd_offset(mm, addr), addr), addr);

#ifdef CONFIG_ARM_LPAE
	pmd[0] = __pmd((pmd_val(pmd[0]) & mask) | prot);
#else
	if (addr & SECTION_SIZE)
		pmd[1] = __pmd((pmd_val(pmd[1]) & mask) | prot);
	else
		pmd[0] = __pmd((pmd_val(pmd[0]) & mask) | prot);
#endif
	flush_pmd_entry(pmd);
	local_flush_tlb_kernel_range(addr, addr + SECTION_SIZE);
}

/* Make sure extended page tables are in use. */
static inline bool arch_has_strict_perms(void)
{
	if (cpu_architecture() < CPU_ARCH_ARMv6)
		return false;

	return !!(get_cr() & CR_XP);
}

void set_section_perms(struct section_perm *perms, int n, bool set,
			struct mm_struct *mm)
{
	size_t i;
	unsigned long addr;

	if (!arch_has_strict_perms())
		return;

	for (i = 0; i < n; i++) {
		if (!IS_ALIGNED(perms[i].start, SECTION_SIZE) ||
		    !IS_ALIGNED(perms[i].end, SECTION_SIZE)) {
			pr_err("BUG: %s section %lx-%lx not aligned to %lx\n",
				perms[i].name, perms[i].start, perms[i].end,
				SECTION_SIZE);
			continue;
		}

		for (addr = perms[i].start;
		     addr < perms[i].end;
		     addr += SECTION_SIZE)
			section_update(addr, perms[i].mask,
				set ? perms[i].prot : perms[i].clear, mm);
	}

}

/**
 * update_sections_early intended to be called only through stop_machine
 * framework and executed by only one CPU while all other CPUs will spin and
 * wait, so no locking is required in this function.
 */
static void update_sections_early(struct section_perm perms[], int n)
{
	struct task_struct *t, *s;

	for_each_process(t) {
		if (t->flags & PF_KTHREAD)
			continue;
		for_each_thread(t, s)
			if (s->mm)
				set_section_perms(perms, n, true, s->mm);
	}
	set_section_perms(perms, n, true, current->active_mm);
	set_section_perms(perms, n, true, &init_mm);
}

static int __fix_kernmem_perms(void *unused)
{
	update_sections_early(nx_perms, ARRAY_SIZE(nx_perms));
	return 0;
}

static void fix_kernmem_perms(void)
{
	stop_machine(__fix_kernmem_perms, NULL, NULL);
}

static int __mark_rodata_ro(void *unused)
{
	update_sections_early(ro_perms, ARRAY_SIZE(ro_perms));
	return 0;
}

static int kernel_set_to_readonly __read_mostly;

void mark_rodata_ro(void)
{
	kernel_set_to_readonly = 1;
	stop_machine(__mark_rodata_ro, NULL, NULL);
	debug_checkwx();
}

void set_kernel_text_rw(void)
{
	if (!kernel_set_to_readonly)
		return;

	set_section_perms(ro_perms, ARRAY_SIZE(ro_perms), false,
				current->active_mm);
}

void set_kernel_text_ro(void)
{
	if (!kernel_set_to_readonly)
		return;

	set_section_perms(ro_perms, ARRAY_SIZE(ro_perms), true,
				current->active_mm);
}

#else
static inline void fix_kernmem_perms(void) { }
#endif /* CONFIG_STRICT_KERNEL_RWX */

void free_initmem(void)
{
	fix_kernmem_perms();

	poison_init_mem(__init_begin, __init_end - __init_begin);
	if (!machine_is_integrator() && !machine_is_cintegrator())
		free_initmem_default(-1);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (start == initrd_start)
		start = round_down(start, PAGE_SIZE);
	if (end == initrd_end)
		end = round_up(end, PAGE_SIZE);

	poison_init_mem((void *)start, PAGE_ALIGN(end) - start);
	free_reserved_area((void *)start, (void *)end, -1, "initrd");
}
#endif
