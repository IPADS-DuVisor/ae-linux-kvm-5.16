// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Atish Patra <atish.patra@wdc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_timer.h>

#define SBI_VERSION_MAJOR			0
#define SBI_VERSION_MINOR			1

static void kvm_riscv_vcpu_sbi_forward(struct kvm_vcpu *vcpu,
				       struct kvm_run *run)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;

	vcpu->arch.sbi_context.return_handled = 0;
	vcpu->stat.ecall_exit_stat++;
	run->exit_reason = KVM_EXIT_RISCV_SBI;
	run->riscv_sbi.extension_id = cp->a7;
	run->riscv_sbi.function_id = cp->a6;
	run->riscv_sbi.args[0] = cp->a0;
	run->riscv_sbi.args[1] = cp->a1;
	run->riscv_sbi.args[2] = cp->a2;
	run->riscv_sbi.args[3] = cp->a3;
	run->riscv_sbi.args[4] = cp->a4;
	run->riscv_sbi.args[5] = cp->a5;
	run->riscv_sbi.ret[0] = cp->a0;
	run->riscv_sbi.ret[1] = cp->a1;
}

int kvm_riscv_vcpu_sbi_return(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;

	/* Handle SBI return only once */
	if (vcpu->arch.sbi_context.return_handled)
		return 0;
	vcpu->arch.sbi_context.return_handled = 1;

	/* Update return values */
	cp->a0 = run->riscv_sbi.ret[0];
	cp->a1 = run->riscv_sbi.ret[1];

	/* Move to next instruction */
	vcpu->arch.guest_context.sepc += 4;

	return 0;
}

#ifdef CONFIG_RISCV_SBI_V01

static void kvm_sbi_system_shutdown(struct kvm_vcpu *vcpu,
				    struct kvm_run *run, u32 type)
{
	int i;
	struct kvm_vcpu *tmp;

	kvm_for_each_vcpu(i, tmp, vcpu->kvm)
		tmp->arch.power_off = true;
	kvm_make_all_cpus_request(vcpu->kvm, KVM_REQ_SLEEP);

	memset(&run->system_event, 0, sizeof(run->system_event));
	run->system_event.type = type;
	run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
}

extern bool stat_vmexit;
extern unsigned long cause_cnt[16];
extern unsigned long cause_time[16];

static inline
void clrvipi0(unsigned long val) {
    register long vipi_id asm("a0") = ~val;

    asm volatile ("\n"
            ".option push\n"
            ".option norvc\n"

            /* clr_vipi0 */
            ".word 0xc8a02077\n"

            ".option pop"
            :
            : "r"(vipi_id)
            : "memory");
}

static inline
unsigned long rdvipi0(void) {
    register long vipi_id asm("a0");

    asm volatile ("\n"
            ".option push\n"
            ".option norvc\n"

            /* rdvipi0 */
            ".word 0xc8101577\n"

            ".option pop"
            : "=r"(vipi_id)
            :
            : "memory");

    return vipi_id;
}

#define SBI_TEST_TIMING_START (0xC200000)
#define SBI_TEST_TIMING_END (0xC200001)
#define SBI_TEST_LOCAL_SBI (0xC200002)
#define SBI_TEST_SEND_PRINT (0xC200003)
#define SBI_TEST_RECV_PRINT (0xC200004)
static unsigned long start_cycle, end_cycle;

#include <linux/kthread.h>
extern unsigned long *vinterrupts_mmio;
extern volatile int *vplic_sm;

static int vplic_thread(void *arg)
{
    struct kvm_vcpu *vcpu = (struct kvm_vcpu *)arg;
    unsigned long start, total = 0, cnt = 0, cur, min = -1, max = 0;
    int irq = IRQ_VS_EXT, val, flags;
    while (!kthread_should_stop()) {
        cond_resched();
        local_irq_save(flags);

        start = csr_read(CSR_CYCLE);
        kvm_riscv_vcpu_set_interrupt(vcpu, irq);
        smp_rmb();
        while (vplic_sm[0] != (cnt + 1)) {
            smp_rmb();
        }
        cur = csr_read(CSR_CYCLE) - start;

        local_irq_restore(flags);
        total += cur;
        min = min > cur ? cur : min;
        max = max < cur ? cur : max;
        if (++cnt % 1000 == 0) {
            pr_err("\t cur cycle %lu cnt %lu min %lu max %lu\n",
                    total, cnt, min, max);
            min = -1;
            max = 0;
        }
        if (cnt == 10000) {
            pr_err("%s:%d total cycle %lu cnt %lu avg %lu\n",
                    __func__, __LINE__, total, cnt, total / cnt);
            break;
        }
        smp_rmb();
        while (vplic_sm[1] != cnt) {
            //pr_err("\t line %d vplic_sm[0] %lu vplic_sm[1] %lu cnt %lu\n",
            //        __LINE__, vplic_sm[0], vplic_sm[1], cnt);
            smp_rmb();
        }
    }
    return 0;
}

static unsigned long vipi_send_cnt = 0;
unsigned long vipi_send_cycle = 0;
unsigned long vipi_cycle = 0;
bool vipi_sent = 0;
int kvm_riscv_vcpu_sbi_ecall(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	ulong hmask;
	int i, ret = 1;
	u64 next_cycle;
	struct kvm_vcpu *rvcpu;
	bool next_sepc = true;
	struct cpumask cm, hm;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_cpu_trap utrap = { 0 };
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;

	if (!cp)
		return -EINVAL;

	switch (cp->a7) {
	case SBI_EXT_0_1_CONSOLE_GETCHAR:
	case SBI_EXT_0_1_CONSOLE_PUTCHAR:
		/*
		 * The CONSOLE_GETCHAR/CONSOLE_PUTCHAR SBI calls cannot be
		 * handled in kernel so we forward these to user-space
		 */
		kvm_riscv_vcpu_sbi_forward(vcpu, run);
		next_sepc = false;
		ret = 0;
		break;
	case SBI_EXT_0_1_SET_TIMER:
#if __riscv_xlen == 32
		next_cycle = ((u64)cp->a1 << 32) | (u64)cp->a0;
#else
		next_cycle = (u64)cp->a0;
#endif
		kvm_riscv_vcpu_timer_next_event(vcpu, next_cycle);
		break;
	case SBI_EXT_0_1_CLEAR_IPI:
#if 0
        pr_err("--- SBI_EXT_0_1_CLEAR_IPI %lu %lu %lu %lu %u\n",
                cp->a0, cp->a1, cp->a2, cp->a3, vplic_sm[0]);
        vplic_sm[0] = 0;
        csr_write(CSR_VSIP, 0);
        writel(0, vinterrupts_mmio);
        wake_up_process(
                kthread_create_on_cpu(vplic_thread, (void *)cp->a0,
                    4, "vplic_thread"));
#else
		kvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_EXT);
        csr_write(CSR_VSIP, 0);
        //pr_err("--- [%d] SBI_EXT_0_1_CLEAR_IPI %lu %lu %lu %lu %u\n",
        //        smp_processor_id(),
        //        cp->a0, cp->a1, cp->a2, cp->a3, vplic_sm[0]);
#endif
		break;
	case SBI_EXT_0_1_SEND_IPI:
#if 0
        //pr_err("--- line %lu: %lu %lu %lu rdvipi0 %lx\n",
        //        cp->a0, cp->a1, cp->a2, cp->a3, rdvipi0());
        pr_err("--- SBI_EXT_0_1_SEND_IPI %lu %lu %lu %lu vint %x\n",
                cp->a0, cp->a1, cp->a2, cp->a3, readl(vinterrupts_mmio));
        //vipi_send_cnt++;
        //rvcpu = kvm_get_vcpu_by_id(vcpu->kvm, cp->a0);
        //vipi_send_cycle = csr_read(CSR_CYCLE);
        //kvm_riscv_vcpu_set_interrupt(rvcpu, IRQ_VS_SOFT);
        //smp_rmb();
        //while (!vipi_sent)
        //    smp_rmb();
        //vipi_cycle += csr_read(CSR_CYCLE) - vipi_send_cycle;
        //vipi_sent = false;
        //if (vipi_send_cnt == 10000)
        //    pr_err("--- %lu %lu %lu %lu vipi_send_cycle %lu\n",
        //            cp->a0, cp->a1, cp->a2, cp->a3, vipi_cycle);
#else
		if (cp->a0)
			hmask = kvm_riscv_vcpu_unpriv_read(vcpu, false, cp->a0,
							   &utrap);
		else
			hmask = (1UL << atomic_read(&kvm->online_vcpus)) - 1;
		if (utrap.scause) {
			utrap.sepc = cp->sepc;
			kvm_riscv_vcpu_trap_redirect(vcpu, &utrap);
			next_sepc = false;
			break;
		}
		for_each_set_bit(i, &hmask, BITS_PER_LONG) {
			rvcpu = kvm_get_vcpu_by_id(vcpu->kvm, i);
			kvm_riscv_vcpu_set_interrupt(rvcpu, IRQ_VS_SOFT);
		}
#endif
		break;
	case SBI_EXT_0_1_SHUTDOWN:
		kvm_sbi_system_shutdown(vcpu, run, KVM_SYSTEM_EVENT_SHUTDOWN);
		next_sepc = false;
		ret = 0;
		break;
	case SBI_EXT_0_1_REMOTE_FENCE_I:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA:
	case SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID:
		if (cp->a0)
			hmask = kvm_riscv_vcpu_unpriv_read(vcpu, false, cp->a0,
							   &utrap);
		else
			hmask = (1UL << atomic_read(&kvm->online_vcpus)) - 1;
		if (utrap.scause) {
			utrap.sepc = cp->sepc;
			kvm_riscv_vcpu_trap_redirect(vcpu, &utrap);
			next_sepc = false;
			break;
		}
		cpumask_clear(&cm);
		for_each_set_bit(i, &hmask, BITS_PER_LONG) {
			rvcpu = kvm_get_vcpu_by_id(vcpu->kvm, i);
			if (rvcpu->cpu < 0)
				continue;
			cpumask_set_cpu(rvcpu->cpu, &cm);
		}
		riscv_cpuid_to_hartid_mask(&cm, &hm);
		if (cp->a7 == SBI_EXT_0_1_REMOTE_FENCE_I)
			sbi_remote_fence_i(cpumask_bits(&hm));
		else if (cp->a7 == SBI_EXT_0_1_REMOTE_SFENCE_VMA)
			sbi_remote_hfence_vvma(cpumask_bits(&hm),
						cp->a1, cp->a2);
		else
			sbi_remote_hfence_vvma_asid(cpumask_bits(&hm),
						cp->a1, cp->a2, cp->a3);
		break;
	case SBI_EXT_0_1_DEBUG_START: {
        int i = 0;
        for (; i < 16; i++) {
            cause_cnt[i] = 0;
            cause_time[i] = 0;
        }
        stat_vmexit = true;
		break;
    }
	case SBI_EXT_0_1_DEBUG_END:
        stat_vmexit = false;
        printk("DEBUG vmexit total time %lu, cnt %lu, avg %lu\n",
                cause_time[0], cause_cnt[0], cause_time[0] / cause_cnt[0]);
        printk("time %lu, %lu, %lu, %lu \n\t %lu, %lu, %lu, %lu\n",
                cause_time[1], cause_time[2], cause_time[3], cause_time[4],
                cause_time[5], cause_time[6], cause_time[7], cause_time[8]);
        printk("cnt %lu, %lu, %lu, %lu \n\t %lu, %lu, %lu, %lu\n",
                cause_cnt[1], cause_cnt[2], cause_cnt[3], cause_cnt[4],
                cause_cnt[5], cause_cnt[6], cause_cnt[7], cause_cnt[8]);
		break;
	case SBI_TEST_TIMING_START:
		printk("--- SBI_TEST_TIMING_START [%ld]\n",
                smp_processor_id());
		start_cycle = csr_read(CSR_CYCLE);
		break;
	case SBI_TEST_TIMING_END:
		end_cycle = csr_read(CSR_CYCLE);
		printk("--- SBI_TEST_TIMING_END [%ld] cycles %llu\n",
                smp_processor_id(), end_cycle - start_cycle);
		break;
	case SBI_TEST_SEND_PRINT:
        pr_err("--- SEND_PRINT: line %lu: %lx %lu %lu rdvipi0 %lx\n",
                //cp->a0, cp->a1, cp->a2, cp->a3, rdvipi0());
                cp->a0, cp->a1, cp->a2, cp->a3, smp_processor_id());
		break;
	case SBI_TEST_RECV_PRINT:
        pr_err("--- RECV_PRINT: line %lu: %lx %lu %lu rdvipi0 %lx vsip %lx\n",
                //cp->a0, cp->a1, cp->a2, cp->a3, rdvipi0(), csr_read(CSR_VSIP));
                cp->a0, cp->a1, cp->a2, cp->a3, smp_processor_id(), csr_read(CSR_VSIP));
        csr_write(CSR_VSIP, 0);
		break;
	case SBI_TEST_LOCAL_SBI:
        vplic_sm[0] = 0;
        csr_write(CSR_VSIP, 0);
		kvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_EXT);
        wake_up_process(
                kthread_create_on_cpu(vplic_thread, (void *)vcpu,
                    4, "vplic_thread"));
		break;
	default:
		/* Return error for unsupported SBI calls */
		cp->a0 = SBI_ERR_NOT_SUPPORTED;
		break;
	}

	if (next_sepc)
		cp->sepc += 4;

	return ret;
}

#else

int kvm_riscv_vcpu_sbi_ecall(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	kvm_riscv_vcpu_sbi_forward(vcpu, run);
	return 0;
}

#endif
