#ifndef YAT_PREEMPT_H
#define YAT_PREEMPT_H

#include <linux/types.h>
#include <linux/cache.h>
#include <linux/percpu.h>
#include <asm/atomic.h>

#include <yat/debug_trace_common.h>

DECLARE_PER_CPU(bool, yat_preemption_in_progress);

/* is_current_running() is legacy macro (and a hack) that is used to make
 * the plugin logic, which still stems from the 2.6.20 era, work with current
 * kernels.
 *
 * It used to honor the flag in the preempt_count variable that was
 * set when scheduling is in progress. This doesn't exist anymore in recent
 * Linux versions. Instead, Linux has moved to passing a 'preempt' flag to
 * __schedule(). In particular, Linux ignores prev->state != TASK_RUNNING and
 * does *not* process self-suspensions if an interrupt (i.e., a preemption)
 * races with a task that is about to call schedule() anyway.
 *
 * The value of the 'preempt' flag in __schedule() is crucial
 * information for some of the YAT^RT plugins, which must re-add
 * soon-to-block tasks to the ready queue if the rest of the system doesn't
 * process the preemption yet. Unfortunately, the flag is not passed to
 * pick_next_task(). Hence, as a hack, we communicate it out of band via the
 * global, per-core variable yat_preemption_in_progress, which is set by
 * the scheduler in __schedule() and read by the plugins via the
 * is_current_running() macro.
 */
#define is_current_running() \
	((current)->state == TASK_RUNNING || \
	this_cpu_read(yat_preemption_in_progress))

DECLARE_PER_CPU_SHARED_ALIGNED(atomic_t, resched_state);

#ifdef CONFIG_PREEMPT_STATE_TRACE
const char* sched_state_name(int s);
#define TRACE_STATE(fmt, args...)					\
	sched_trace_log_message(TRACE_PREFIX "SCHED_STATE " fmt,	\
				TRACE_ARGS, ## args)
#else
#define TRACE_STATE(fmt, args...) /* ignore */
#endif

#define VERIFY_SCHED_STATE(x)						\
	do { int __s = get_sched_state();				\
		if ((__s & (int)(x)) == 0){					\
			TRACE_STATE("INVALID s=0x%x (%s) not "		\
				    "in 0x%x (%s) [%s]\n",		\
				    __s, sched_state_name(__s),		\
				    (int)(x), #x, __FUNCTION__);}		\
	} while (0);

#define TRACE_SCHED_STATE_CHANGE(x, y, cpu)				\
	TRACE_STATE("[P%d] 0x%x (%s) -> 0x%x (%s)\n",			\
		    cpu,  (x), sched_state_name(x),			\
		    (y), sched_state_name(y))


typedef enum scheduling_state {
	TASK_SCHEDULED    = (1 << 0),  /* The currently scheduled task is the one that
					* should be scheduled, and the processor does not
					* plan to invoke schedule(). */
	SHOULD_SCHEDULE   = (1 << 1),  /* A remote processor has determined that the
					* processor should reschedule, but this has not
					* been communicated yet (IPI still pending). */
	WILL_SCHEDULE     = (1 << 2),  /* The processor has noticed that it has to
					* reschedule and will do so shortly. */
	TASK_PICKED       = (1 << 3),  /* The processor is currently executing schedule(),
					* has selected a new task to schedule, but has not
					* yet performed the actual context switch. */
	PICKED_WRONG_TASK = (1 << 4),  /* The processor has not yet performed the context
					* switch, but a remote processor has already
					* determined that a higher-priority task became
					* eligible after the task was picked. */
} sched_state_t;

static inline sched_state_t get_sched_state_on(int cpu)
{
	return atomic_read(&per_cpu(resched_state, cpu));
}

static inline sched_state_t get_sched_state(void)
{
	return atomic_read(this_cpu_ptr(&resched_state));
}

static inline int is_in_sched_state(int possible_states)
{
	return get_sched_state() & possible_states;
}

static inline int cpu_is_in_sched_state(int cpu, int possible_states)
{
	return get_sched_state_on(cpu) & possible_states;
}

static inline void set_sched_state(sched_state_t s)
{
	TRACE_SCHED_STATE_CHANGE(get_sched_state(), s, smp_processor_id());
	atomic_set(this_cpu_ptr(&resched_state), s);
}

static inline int sched_state_transition(sched_state_t from, sched_state_t to)
{
	sched_state_t old_state;

	old_state = atomic_cmpxchg(this_cpu_ptr(&resched_state), from, to);
	if (old_state == from) {
		TRACE_SCHED_STATE_CHANGE(from, to, smp_processor_id());
		return 1;
	} else
		return 0;
}

static inline int sched_state_transition_on(int cpu,
					    sched_state_t from,
					    sched_state_t to)
{
	sched_state_t old_state;

	old_state = atomic_cmpxchg(&per_cpu(resched_state, cpu), from, to);
	if (old_state == from) {
		TRACE_SCHED_STATE_CHANGE(from, to, cpu);
		return 1;
	} else
		return 0;
}

/* Plugins must call this function after they have decided which job to
 * schedule next.  IMPORTANT: this function must be called while still holding
 * the lock that is used to serialize scheduling decisions.
 *
 * (Ideally, we would like to use runqueue locks for this purpose, but that
 * would lead to deadlocks with the migration code.)
 */
static inline void sched_state_task_picked(void)
{
	VERIFY_SCHED_STATE(WILL_SCHEDULE);

	/* WILL_SCHEDULE has only a local tansition => simple store is ok */
	set_sched_state(TASK_PICKED);
}

static inline void sched_state_entered_schedule(void)
{
	/* Update state for the case that we entered schedule() not due to
	 * set_tsk_need_resched() */
	set_sched_state(WILL_SCHEDULE);
}

/* Called by schedule() to check if the scheduling decision is still valid
 * after a context switch. Returns 1 if the CPU needs to reschedule. */
static inline int sched_state_validate_switch(void)
{
	int decision_ok = 0;

	VERIFY_SCHED_STATE(PICKED_WRONG_TASK | TASK_PICKED | WILL_SCHEDULE);

	if (is_in_sched_state(TASK_PICKED)) {
		/* Might be good; let's try to transition out of this
		 * state. This must be done atomically since remote processors
		 * may try to change the state, too. */
		decision_ok = sched_state_transition(TASK_PICKED, TASK_SCHEDULED);
	}

	if (!decision_ok){
		TRACE_STATE("validation failed (%s)\n", sched_state_name(get_sched_state()));
	}

	return !decision_ok;
}

/* State transition events. See yat/preempt.c for details. */
void sched_state_will_schedule(struct task_struct* tsk);
void sched_state_ipi(void);
/* Cause a CPU (remote or local) to reschedule. */
void yat_reschedule(int cpu);
void yat_reschedule_local(void);

#ifdef CONFIG_DEBUG_KERNEL
void sched_state_plugin_check(void);
#else
#define sched_state_plugin_check() /* no check */
#endif

#endif
