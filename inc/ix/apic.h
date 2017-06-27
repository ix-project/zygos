#pragma once

#include <dune.h>

#define APIC_DEST_PHYSICAL 0x00000
#define APIC_DM_FIXED 0x00000
#define APIC_EOI 0xB0
#define APIC_EOI_ACK 0x0
#define APIC_ICR 0x300
#define APIC_ICR2 0x310
#define APIC_ICR_BUSY 0x01000
#define SET_APIC_DEST_FIELD(x) ((x) << 24)

static inline void apic_write(uint32_t reg, uint32_t v)
{
	volatile uint32_t *addr = (volatile uint32_t *)(APIC_BASE + reg);

	asm volatile("movl %0, %P1" : "=r" (v), "=m" (*addr) : "i" (0), "0" (v), "m" (*addr));
}

static inline uint32_t apic_read(uint32_t reg)
{
	return *((volatile uint32_t *)(APIC_BASE + reg));
}

static inline unsigned int __prepare_ICR(unsigned int shortcut, int vector, unsigned int dest)
{
	return shortcut | dest | APIC_DM_FIXED | vector;
}

static inline int __prepare_ICR2(unsigned int mask)
{
	return SET_APIC_DEST_FIELD(mask);
}

static inline void __xapic_wait_icr_idle(void)
{
	while (apic_read(APIC_ICR) & APIC_ICR_BUSY)
		cpu_relax();
}

static inline void __default_send_IPI_dest_field(unsigned int mask, int vector, unsigned int dest)
{
	__xapic_wait_icr_idle();
	apic_write(APIC_ICR2, __prepare_ICR2(mask));
	apic_write(APIC_ICR, __prepare_ICR(0, vector, dest));
}

static inline void apic_send_ipi(int cpu, int vector)
{
	__default_send_IPI_dest_field(percpu_get_remote(apicid, cpu), vector, APIC_DEST_PHYSICAL);
}

static inline void apic_eoi(void)
{
	apic_write(APIC_EOI, APIC_EOI_ACK);
}
