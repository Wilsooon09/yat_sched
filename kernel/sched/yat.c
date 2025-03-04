/* This file is included from kernel/sched.c */

#include "sched.h"

#include <yat/trace.h>
#include <yat/sched_trace.h>

#include <yat/debug_trace.h>
#include <yat/yat.h>
#include <yat/budget.h>
#include <yat/sched_plugin.h>
#include <yat/preempt.h>
#include <yat/np.h>

static void update_time_yat(struct rq *rq, struct task_struct *p)
{
	u64 delta = rq->clock - p->se.exec_start;
	if (unlikely((s64)delta < 0))
		delta = 0;
	/* per job counter */
	p->rt_param.job_params.exec_time += delta;
	/* task counter */
	p->se.sum_exec_runtime += delta;
	if (delta) {
		TRACE_TASK(p, "charged %llu exec time (total:%llu, rem:%llu)\n",
			delta, p->rt_param.job_params.exec_time, budget_remaining(p));
	}
	/* sched_clock() */
	p->se.exec_start = rq->clock;
	cpuacct_charge(p, delta);
}

static void double_rq_lock(struct rq *rq1, struct rq *rq2);
static void double_rq_unlock(struct rq *rq1, struct rq *rq2);

static struct task_struct *
yat_schedule(struct rq *rq, struct task_struct *prev)
{
	struct task_struct *next;

#ifdef CONFIG_SMP
	struct rq* other_rq;
	long was_running;
	int from_where;
	lt_t _maybe_deadlock = 0;
#endif

	/* let the plugin schedule */
	next = yat->schedule(prev);

	sched_state_plugin_check();

#ifdef CONFIG_SMP
	/* check if a global plugin pulled a task from a different RQ */
	if (next && task_rq(next) != rq) {
		/* we need to migrate the task */
		other_rq = task_rq(next);
		from_where = other_rq->cpu;
		TRACE_TASK(next, "migrate from %d\n", from_where);

		/* while we drop the lock, the prev task could change its
		 * state
		 */
		BUG_ON(prev != current);
		was_running = is_current_running();

		/* Don't race with a concurrent switch.  This could deadlock in
		 * the case of cross or circular migrations.  It's the job of
		 * the plugin to make sure that doesn't happen.
		 */
		TRACE_TASK(next, "stack_in_use=%d\n",
			   next->rt_param.stack_in_use);
		if (next->rt_param.stack_in_use != NO_CPU) {
			TRACE_TASK(next, "waiting to deschedule\n");
			_maybe_deadlock = yat_clock();
		}

		raw_spin_unlock(&rq->lock);

		while (next->rt_param.stack_in_use != NO_CPU) {
			cpu_relax();
			mb();
			if (next->rt_param.stack_in_use == NO_CPU)
				TRACE_TASK(next,"descheduled. Proceeding.\n");

			if (!yat->should_wait_for_stack(next)) {
				/* plugin aborted the wait */
				TRACE_TASK(next,
				           "plugin gave up waiting for stack\n");
				next = NULL;
				/* Make sure plugin is given a chance to
				 * reconsider. */
				yat_reschedule_local();
				/* give up */
				raw_spin_lock(&rq->lock);
				goto out;
			}

			if (from_where != task_rq(next)->cpu) {
				/* The plugin should not give us something
				 * that other cores are trying to pull, too */
				TRACE_TASK(next, "next invalid: task keeps "
				                 "shifting around!? "
				                 "(%d->%d)\n",
				                 from_where,
				                 task_rq(next)->cpu);

				/* bail out */
				raw_spin_lock(&rq->lock);
				yat->next_became_invalid(next);
				yat_reschedule_local();
				next = NULL;
				goto out;
			}

			if (lt_before(_maybe_deadlock + 1000000000L,
				      yat_clock())) {
				/* We've been spinning for 1s.
				 * Something can't be right!
				 * Let's abandon the task and bail out; at least
				 * we will have debug info instead of a hard
				 * deadlock.
				 */
#ifdef CONFIG_BUG_ON_MIGRATION_DEADLOCK
				BUG();
#else
				TRACE_TASK(next,"stack too long in use. "
					   "Deadlock?\n");
				next = NULL;

				/* bail out */
				raw_spin_lock(&rq->lock);
				goto out;
#endif
			}
		}
#ifdef  __ARCH_WANT_UNLOCKED_CTXSW
		if (next->on_cpu)
			TRACE_TASK(next, "waiting for !oncpu");
		while (next->on_cpu) {
			cpu_relax();
			mb();
		}
#endif
		double_rq_lock(rq, other_rq);
		if (other_rq == task_rq(next) &&
		    next->rt_param.stack_in_use == NO_CPU) {
			/* ok, we can grab it */
			set_task_cpu(next, rq->cpu);
			/* release the other CPU's runqueue, but keep ours */
			raw_spin_unlock(&other_rq->lock);
		} else {
			/* Either it moved or the stack was claimed; both is
			 * bad and forces us to abort the migration. */
			TRACE_TASK(next, "next invalid: no longer available\n");
			raw_spin_unlock(&other_rq->lock);
			yat->next_became_invalid(next);
			next = NULL;
			goto out;
		}

		if (!yat->post_migration_validate(next)) {
			TRACE_TASK(next, "plugin deems task now invalid\n");
			yat_reschedule_local();
			next = NULL;
		}
	}
#endif

	/* check if the task became invalid while we dropped the lock */
	if (next && (!is_realtime(next) || !tsk_rt(next)->present)) {
		TRACE_TASK(next,
			"BAD: next (no longer?) valid\n");
		yat->next_became_invalid(next);
		yat_reschedule_local();
		next = NULL;
	}

	if (next) {
#ifdef CONFIG_SMP
		next->rt_param.stack_in_use = rq->cpu;
#else
		next->rt_param.stack_in_use = 0;
#endif
		update_rq_clock(rq);
		next->se.exec_start = rq->clock;
	}

out:
	update_enforcement_timer(next);

	if (next && (next->rt_param.task_params.num_mrsp_locks > 0 || next->rt_param.task_params.requesting_lock != NULL) && next->rt_param.task_params.np_len > 0
	&& next->rt_param.task_params.helper != NULL && next->rt_param.task_params.migrated_time > 0)
	arm_non_preemption_budget_timer(next);

	return next;
}

static void enqueue_task_yat(struct rq *rq, struct task_struct *p,
				int flags)
{
//	if (p && p->rt_param.task_params.num_mrsp_locks > 0) {
//		printk(KERN_EMERG"try to enqueue MrsP task: %d. flags: %d, enqueue wake up %d. \n", p->pid, flags, ENQUEUE_WAKEUP);
//	}
	tsk_rt(p)->present = 1;
	if (flags & ENQUEUE_WAKEUP) {
		sched_trace_task_resume(p);
		/* YAT^RT plugins need to update the state
		 * _before_ making it available in global structures.
		 * Linux gets away with being lazy about the task state
		 * update. We can't do that, hence we update the task
		 * state already here.
		 *
		 * WARNING: this needs to be re-evaluated when porting
		 *          to newer kernel versions.
		 */
		p->state = TASK_RUNNING;
		yat->task_wake_up(p);

		rq->yat.nr_running++;
//		if (p && p->rt_param.task_params.num_mrsp_locks > 0)
//			printk(KERN_EMERG"MrsP task: %d enqueued. \n", p->pid);
	} else {
		TRACE_TASK(p, "ignoring an enqueue, not a wake up.\n");
		p->se.exec_start = rq->clock;
//		if (p && p->rt_param.task_params.num_mrsp_locks > 0)
//			printk(KERN_EMERG"MrsP task: %d not enqueued. \n", p->pid);
	}
}

static void dequeue_task_yat(struct rq *rq, struct task_struct *p,
				int flags)
{
	if (flags & DEQUEUE_SLEEP) {
#ifdef CONFIG_SCHED_TASK_TRACE
		tsk_rt(p)->job_params.last_suspension = yat_clock();
#endif
		yat->task_block(p);
		tsk_rt(p)->present = 0;
		sched_trace_task_block(p);

		rq->yat.nr_running--;
	} else
		TRACE_TASK(p, "ignoring a dequeue, not going to sleep.\n");
}

static void yield_task_yat(struct rq *rq)
{
	TS_SYSCALL_IN_START;
	TS_SYSCALL_IN_END;

	BUG_ON(rq->curr != current);
	/* sched_yield() is called to trigger delayed preemptions.
	 * Thus, mark the current task as needing to be rescheduled.
	 * This will cause the scheduler plugin to be invoked, which can
	 * then determine if a preemption is still required.
	 */
	clear_exit_np(current);
	yat_reschedule_local();

	TS_SYSCALL_OUT_START;
}

/* Plugins are responsible for this.
 */
static void check_preempt_curr_yat(struct rq *rq, struct task_struct *p, int flags)
{
}

static void put_prev_task_yat(struct rq *rq, struct task_struct *p)
{
}

/* pick_next_task_yat() - yat_schedule() function
 *
 * prev and rf are deprecated by our caller and unused
 * returns the next task to be scheduled
 */
static struct task_struct *pick_next_task_yat(struct rq *rq,
	struct task_struct *prev, struct rq_flags *rf)
{
	struct task_struct *next;

	if (is_realtime(rq->curr))
		update_time_yat(rq, rq->curr);

	/* yat_schedule() should be wrapped by the rq_upin_lock() and
	 * rq_repin_lock() annotations. Unfortunately, these annotations can
	 * not presently be added meaningfully as we are not passed rf->cookie.
	 */
	TS_PLUGIN_SCHED_START;
	next = yat_schedule(rq, rq->curr);
	TS_PLUGIN_SCHED_END;

	return next;
}

static void task_tick_yat(struct rq *rq, struct task_struct *p, int queued)
{
	if (is_realtime(p) && !queued) {
		update_time_yat(rq, p);
		/* budget check for QUANTUM_ENFORCEMENT tasks */
		if (budget_enforced(p) && budget_exhausted(p)) {
			yat_reschedule_local();
		}
	}
}

static void switched_to_yat(struct rq *rq, struct task_struct *p)
{
}

static void prio_changed_yat(struct rq *rq, struct task_struct *p,
				int oldprio)
{
}

unsigned int get_rr_interval_yat(struct rq *rq, struct task_struct *p)
{
	/* return infinity */
	return 0;
}

/* This is called when a task became a real-time task, either due to a SCHED_*
 * class transition or due to PI mutex inheritance. We don't handle Linux PI
 * mutex inheritance yet (and probably never will). Use YAT provided
 * synchronization primitives instead.
 */
static void set_next_task_yat(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq->clock;
}


#ifdef CONFIG_SMP
static int
balance_yat(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return 1;
}

/* execve tries to rebalance task in this scheduling domain.
 * We don't care about the scheduling domain; can gets called from
 * exec, fork, wakeup.
 */
static int
select_task_rq_yat(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	/* preemption is already disabled.
	 * We don't want to change cpu here
	 */
	return task_cpu(p);
}
#endif

static void update_curr_yat(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	if (!is_realtime(p))
		return;

	update_time_yat(rq, p);
}

const struct sched_class yat_sched_class = {
	/* From 34f971f6 the stop/migrate worker threads have a class on
	 * their own, which is the highest prio class. We don't support
	 * cpu-hotplug or cpu throttling. Allows Yat to use up to 1.0
	 * CPU capacity.
	 */
#ifdef CONFIG_SMP
	.next			= &stop_sched_class,
#else
	.next			= &dl_sched_class,
#endif
	.enqueue_task		= enqueue_task_yat,
	.dequeue_task		= dequeue_task_yat,
	.yield_task		= yield_task_yat,

	.check_preempt_curr	= check_preempt_curr_yat,

	.pick_next_task		= pick_next_task_yat,
	.put_prev_task		= put_prev_task_yat,

#ifdef CONFIG_SMP
	.balance		= balance_yat,
	.select_task_rq		= select_task_rq_yat,
#endif

	.set_next_task		= set_next_task_yat,
	.task_tick		= task_tick_yat,

	.get_rr_interval	= get_rr_interval_yat,

	.prio_changed		= prio_changed_yat,
	.switched_to		= switched_to_yat,

	.update_curr		= update_curr_yat,
};
