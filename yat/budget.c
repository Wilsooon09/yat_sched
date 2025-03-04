#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/uaccess.h>

#include <yat/debug_trace.h>
#include <yat/yat.h>
#include <yat/preempt.h>
#include <yat/sched_plugin.h>
#include <yat/np.h>

#include <yat/budget.h>

struct enforcement_timer {
	/* The enforcement timer is used to accurately police
	 * slice budgets. */
	struct hrtimer		timer;
	int			armed;
};

DEFINE_PER_CPU(struct enforcement_timer, budget_timer);

static enum hrtimer_restart on_enforcement_timeout(struct hrtimer *timer)
{
	struct enforcement_timer* et = container_of(timer,
						    struct enforcement_timer,
						    timer);
	unsigned long flags;

	local_irq_save(flags);
	TRACE("enforcement timer fired.\n");
	et->armed = 0;
	/* activate scheduler */
	yat_reschedule_local();
	local_irq_restore(flags);

	return  HRTIMER_NORESTART;
}

/* assumes called with IRQs off */
static void cancel_enforcement_timer(struct enforcement_timer* et)
{
	int ret;

	TRACE("cancelling enforcement timer.\n");

	/* Since interrupts are disabled and et->armed is only
	 * modified locally, we do not need any locks.
	 */

	if (et->armed) {
		ret = hrtimer_try_to_cancel(&et->timer);
		/* Should never be inactive. */
		BUG_ON(ret == 0);
		/* Should never be running concurrently. */
		BUG_ON(ret == -1);

		et->armed = 0;
	}
}

/* assumes called with IRQs off */
static void arm_enforcement_timer(struct enforcement_timer* et,
				  struct task_struct* t)
{
	lt_t when_to_fire;
	TRACE_TASK(t, "arming enforcement timer.\n");

	WARN_ONCE(!hrtimer_is_hres_active(&et->timer),
		KERN_ERR "WARNING: no high resolution timers available!?\n");

	/* Calling this when there is no budget left for the task
	 * makes no sense, unless the task is non-preemptive. */
	BUG_ON(budget_exhausted(t) && (!is_np(t)));

	/* hrtimer_start_range_ns() cancels the timer
	 * anyway, so we don't have to check whether it is still armed */

	if (likely(!is_np(t))) {
		when_to_fire = yat_clock() + budget_remaining(t);
		hrtimer_start(&et->timer, ns_to_ktime(when_to_fire),
			HRTIMER_MODE_ABS_PINNED_HARD);
		et->armed = 1;
	}
}


/* expects to be called with IRQs off */
void update_enforcement_timer(struct task_struct* t)
{
	struct enforcement_timer* et = this_cpu_ptr(&budget_timer);

	if (t && budget_precisely_enforced(t)) {
		/* Make sure we call into the scheduler when this budget
		 * expires. */
		arm_enforcement_timer(et, t);
	} else if (et->armed) {
		/* Make sure we don't cause unnecessary interrupts. */
		cancel_enforcement_timer(et);
	}
}


static int __init init_budget_enforcement(void)
{
	int cpu;
	struct enforcement_timer* et;

	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		et = &per_cpu(budget_timer, cpu);
		hrtimer_init(&et->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);
		et->timer.function = on_enforcement_timeout;
	}
	return 0;
}

void yat_current_budget(lt_t *used_so_far, lt_t *remaining)
{
	struct task_struct *t = current;
	unsigned long flags;
	s64 delta;

	local_irq_save(flags);

	delta = sched_clock_cpu(smp_processor_id()) - t->se.exec_start;
	if (delta < 0)
		delta = 0;

	TRACE_CUR("current_budget: sc:%llu start:%llu lt_t:%llu delta:%lld exec-time:%llu rem:%llu\n",
		sched_clock_cpu(smp_processor_id()), t->se.exec_start,
		yat_clock(), delta,
		tsk_rt(t)->job_params.exec_time,
		budget_remaining(t));

	if (used_so_far)
		*used_so_far = tsk_rt(t)->job_params.exec_time + delta;

	if (remaining) {
		*remaining = budget_remaining(t);
		if (*remaining > delta)
			*remaining -= delta;
		else
			*remaining = 0;
	}

	local_irq_restore(flags);
}

asmlinkage long sys_get_current_budget(
	lt_t __user * _expended,
	lt_t __user *_remaining)
{
	lt_t expended = 0, remaining = 0;

	if (is_realtime(current))
		yat->current_budget(&expended, &remaining);

	if (_expended && put_user(expended, _expended))
		return -EFAULT;

	if (_remaining && put_user(remaining, _remaining))
		return -EFAULT;

	return 0;
}

module_init(init_budget_enforcement);
