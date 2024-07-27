/*
 * yat.c -- 实现 of the YAT 系统调用,
 *             the YAT 初始化代码,
 *             and the procfs interface..
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/sysrq.h>
#include <linux/uaccess.h>
#include <uapi/linux/sched/types.h>

#include <yat/debug_trace.h>
#include <yat/yat.h>
#include <yat/bheap.h>
#include <yat/trace.h>
#include <yat/rt_domain.h>
#include <yat/yat_proc.h>
#include <yat/sched_trace.h>

#ifdef CONFIG_SCHED_CPU_AFFINITY
#include <yat/affinity.h>
#endif

#ifdef CONFIG_SCHED_YAT_TRACEPOINT
#define CREATE_TRACE_POINTS
#include <trace/events/yat.h>
#endif

/* 系统中当前存在的实时任务数 */
atomic_t rt_task_count 		= ATOMIC_INIT(0);

#ifdef CONFIG_RELEASE_MASTER
/* current master CPU for handling timer IRQs */
atomic_t release_master_cpu = ATOMIC_INIT(NO_CPU);
#endif

static struct kmem_cache * bheap_node_cache;
extern struct kmem_cache * release_heap_cache;

struct bheap_node* bheap_node_alloc(int gfp_flags)
{
	return kmem_cache_alloc(bheap_node_cache, gfp_flags);
}

void bheap_node_free(struct bheap_node* hn)
{
	kmem_cache_free(bheap_node_cache, hn);
}

struct release_heap* release_heap_alloc(int gfp_flags);
void release_heap_free(struct release_heap* rh);

/**
 * Get the quantum alignment as a cmdline option.
 * Default is aligned quanta..
 */
static bool aligned_quanta = 1;
module_param(aligned_quanta, bool, 0644);

u64 cpu_stagger_offset(int cpu)
{
	u64 offset = 0;

	if (!aligned_quanta) {
		offset = YAT_QUANTUM_LENGTH_NS;
		do_div(offset, num_possible_cpus());
		offset *= cpu;
	}
	return offset;
}

// the non-preemption budget finished. The holder is now running with the local ceiling.
static enum hrtimer_restart on_preemption_enforcement_timeout(struct hrtimer *timer) {
	struct non_preemption_budget_timer* bt = container_of(timer, struct non_preemption_budget_timer, timer);

//	printk( "task %d is going to exit np section, current time %llu. task prio : %d on %d queued: %d \n", bt->t->pid, yat_clock(),
//		get_priority(bt->t), get_partition(bt->t), is_queued(bt->t));
	if (bt->t) {
		BUG_ON(bt->t->rt_param.task_params.saved_current_priority == -1);
		bt->t->rt_param.task_params.priority = bt->t->rt_param.task_params.saved_current_priority;
	}

//	printk( "task %d exited np section, current time %llu. task prio : %d on %d queued: %d \n", bt->t->pid, yat_clock(), get_priority(bt->t),
//		get_partition(bt->t), is_queued(bt->t));

//	TRACE("task %d exit np section, current time %llu. task prio : %d on %d \n", bt->t->pid, yat_clock(), get_priority(bt->t), get_partition(bt->t));

	return HRTIMER_NORESTART;
}

void arm_non_preemption_budget_timer(struct task_struct* t) {
	if (t && t->rt_param.task_params.helper != NULL && t->rt_param.task_params.migrated_time >= 0) {

		struct non_preemption_budget_timer* bt = t->rt_param.task_params.np_timer;

//		printk("holder %d migrated. enter non-preemption budget length %d. current time %llu. \n", t->pid, t->rt_param.task_params.np_len, yat_clock());
//		TRACE("holder %d migrated. enter non-preemption budget length %d. current time %llu. \n", t->pid, t->rt_param.task_params.np_len, yat_clock());

		hrtimer_start(&bt->timer, ns_to_ktime(yat_clock() + t->rt_param.task_params.np_len), HRTIMER_MODE_ABS_PINNED);
		t->rt_param.task_params.migrated_time = -1;
	}
}

void cancel_non_preemption_budget_timer(struct task_struct* t) {
	struct non_preemption_budget_timer* bt = t->rt_param.task_params.np_timer;
	// we need to cancel the timer here if it is not triggered.

	//	printk( "task %d cancel np section at %llu", t->pid, yat_clock());
	while (hrtimer_try_to_cancel(&bt->timer) == -1) {

	}
	t->rt_param.task_params.migrated_time = -1;

}


/*
 * sys_set_task_rt_param 设置任务的实时参数
 * @pid: Pid of the task which scheduling parameters must be changed
 * @param: New real-time extension parameters such as the execution cost and
 *         period
 * Syscall for manipulating with task rt extension params
 * Returns EFAULT  if param is NULL.
 *         ESRCH   if pid is not corrsponding to a valid task.
 *	       EINVAL  if either period or execution cost is <=0
 *	       EPERM   if pid is a real-time task
 *	       0       if success 成功
 *
 * Only non-real-time tasks may be configured with this system call to avoid races with the scheduler.
 * 只有非实时任务能被配置
 * In practice, this means that a task's parameters must be set _before_ calling sys_prepare_rt_task()
 *
 * find_task_by_vpid() assumes that we are in the same namespace of the target.
 */
asmlinkage long sys_set_rt_task_param(pid_t pid, struct rt_task __user * param)
{
	struct rt_task tp;
	struct task_struct *target;
	int retval = -EINVAL;
	int i;

	printk("Setting up rt task parameters for process %d.\n", pid);

	if (pid < 0 || param == 0) {
		goto out;
	}
	if (copy_from_user(&tp, param, sizeof(tp))) {
		retval = -EFAULT;
		goto out;
	}

	/* Task search and manipulation must be protected */
	read_lock_irq(&tasklist_lock);
	rcu_read_lock();
	if (!(target = find_task_by_vpid(pid))) {
		retval = -ESRCH;
		rcu_read_unlock();
		goto out_unlock;
	}
	rcu_read_unlock();

	/* set relative deadline to be implicit if left unspecified */
	if (tp.relative_deadline == 0)
		tp.relative_deadline = tp.period;

	if (tp.exec_cost <= 0)
		goto out_unlock;
	if (tp.period <= 0)
		goto out_unlock;
	if (min(tp.relative_deadline, tp.period) < tp.exec_cost) /*density check*/
	{
		printk(KERN_INFO "yat: real-time task %d rejected "
		       "because task density > 1.0\n", pid);
		goto out_unlock;
	}
	if (tp.cls != RT_CLASS_HARD &&
	    tp.cls != RT_CLASS_SOFT &&
	    tp.cls != RT_CLASS_BEST_EFFORT)
	{
		printk(KERN_INFO "yat: real-time task %d rejected "
				 "because its class is invalid\n", pid);
		goto out_unlock;
	}
	if (tp.budget_policy != NO_ENFORCEMENT &&
	    tp.budget_policy != QUANTUM_ENFORCEMENT &&
	    tp.budget_policy != PRECISE_ENFORCEMENT)
	{
		printk(KERN_INFO "yat: real-time task %d rejected "
		       "because unsupported budget enforcement policy "
		       "specified (%d)\n",
		       pid, tp.budget_policy);
		goto out_unlock;
	}

	if (is_realtime(target)) {
		/* 若该任务已经是一个实时任务: 让插件决定是否支持运行时参数更改 */
		retval = yat->task_change_params(target, &tp);
	} else {
		target->rt_param.task_params = tp;

		target->rt_param.task_params.original_cpu = target->rt_param.task_params.cpu;
		target->rt_param.task_params.original_priority = target->rt_param.task_params.priority;

		/********** init mrsp variables **********/

		target->rt_param.task_params.mrsp_locks = kmalloc(sizeof(struct mrsp_semaphore*) * target->rt_param.task_params.max_nested_degree, GFP_KERNEL);
		for (i = 0; i < target->rt_param.task_params.max_nested_degree; i++) {
			target->rt_param.task_params.mrsp_locks[i] = NULL;
		}

		target->rt_param.task_params.num_mrsp_locks = 0;
		target->rt_param.task_params.requesting_lock = NULL;

		target->rt_param.task_params.leaving_priority = -1;
		target->rt_param.task_params.helper = NULL;
		target->rt_param.task_params.migrate_back = 0;
		target->rt_param.task_params.preempted = 0;

		target->rt_param.task_params.migrated_time = -1;
		target->rt_param.task_params.saved_current_priority = -1;

		target->rt_param.task_params.np_timer = (struct non_preemption_budget_timer *) kmalloc(sizeof(struct non_preemption_budget_timer), GFP_KERNEL);
		hrtimer_init(&target->rt_param.task_params.np_timer->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
		target->rt_param.task_params.np_timer->timer.function = on_preemption_enforcement_timeout;
		target->rt_param.task_params.np_timer->t = target;

		/********** init mrsp variables end **********/

		/********** init mrsp variables **********/
		target->rt_param.task_params.msrp_locks = kmalloc(sizeof(struct msrp_semaphore*) * target->rt_param.task_params.msrp_max_nested_degree, GFP_KERNEL);
		for (i = 0; i < target->rt_param.task_params.msrp_max_nested_degree; i++) {
			target->rt_param.task_params.msrp_locks[i] = NULL;
		}
		target->rt_param.task_params.num_msrp_locks = 0;
		/********** init mrsp variables end **********/

		/********** init fifop variables **********/
		target->rt_param.task_params.fifop_locks = kmalloc(sizeof(struct fifop_semaphore*) * target->rt_param.task_params.fifop_max_nested_degree, GFP_KERNEL);
		target->rt_param.task_params.fifop_requesting_lock = NULL;
		for (i = 0; i < target->rt_param.task_params.fifop_max_nested_degree; i++) {
			target->rt_param.task_params.fifop_locks[i] = NULL;
		}
		target->rt_param.task_params.num_fifop_locks = 0;
		target->rt_param.task_params.fifo_ticket = 0;
		target->rt_param.task_params.need_re_request = 0;
		target->rt_param.task_params.next = NULL;
		/********** init fifop variables end **********/

		retval = 0;
	}
      out_unlock:
	read_unlock_irq(&tasklist_lock);
      out:
	return retval;
}

/*
 * Getter of task's RT params
 *   returns EINVAL if param or pid is NULL
 *   returns ESRCH  if pid does not correspond to a valid task
 *   returns EFAULT if copying of parameters has failed.
 *
 *   find_task_by_vpid() assumes that we are in the same namespace of the target.
 */
asmlinkage long sys_get_rt_task_param(pid_t pid, struct rt_task __user * param)
{
	int retval = -EINVAL;
	struct task_struct *source;
	struct rt_task lp;

	if (param == 0 || pid < 0)
		goto out;

	read_lock_irq(&tasklist_lock);
	rcu_read_lock();
	source = find_task_by_vpid(pid);
	rcu_read_unlock();
	if (!source) {
		retval = -ESRCH;
		read_unlock_irq(&tasklist_lock);
		goto out;
	}
	lp = source->rt_param.task_params;
	read_unlock_irq(&tasklist_lock);
	/* Do copying outside the lock */
	retval =
	    copy_to_user(param, &lp, sizeof(lp)) ? -EFAULT : 0;
      out:
	return retval;

}

/*
 *	This is the crucial function for periodic task implementation,
 *	It checks if a task is periodic, checks if such kind of sleep
 *	is permitted and calls plugin-specific sleep, which puts the
 *	task into a wait array.
 *	returns 0 on successful wakeup
 *	returns EPERM if current conditions do not permit such sleep
 *	returns EINVAL if current task is not able to go to sleep
 */
asmlinkage long sys_complete_job(void)
{
	int retval = -EPERM;
	if (!is_realtime(current)) {
		retval = -EINVAL;
		goto out;
	}
	/* Task with negative or zero period cannot sleep */
	if (get_rt_period(current) <= 0) {
		retval = -EINVAL;
		goto out;
	}
	/* The plugin has to put the task into an
	 * appropriate queue and call schedule
	 */
	retval = yat->complete_job();
      out:
	return retval;
}

/*	This is an "improved" version of sys_complete_job that
 *      addresses the problem of unintentionally missing a job after
 *      an overrun.
 *
 *	returns 0 on successful wakeup
 *	returns EPERM if current conditions do not permit such sleep
 *	returns EINVAL if current task is not able to go to sleep
 */
asmlinkage long sys_wait_for_job_release(unsigned int job)
{
	int retval = -EPERM;
	if (!is_realtime(current)) {
		retval = -EINVAL;
		goto out;
	}

	/* Task with negative or zero period cannot sleep */
	if (get_rt_period(current) <= 0) {
		retval = -EINVAL;
		goto out;
	}

	retval = 0;

	/* first wait until we have "reached" the desired job
	 *
	 * This implementation has at least two problems:
	 *
	 * 1) It doesn't gracefully handle the wrap around of
	 *    job_no. Since YAT is a prototype, this is not much
	 *    of a problem right now.
	 *
	 * 2) It is theoretically racy if a job release occurs
	 *    between checking job_no and calling sleep_next_period().
	 *    A proper solution would requiring adding another callback
	 *    in the plugin structure and testing the condition with
	 *    interrupts disabled.
	 *
	 * FIXME: At least problem 2 should be taken care of eventually.
	 */
	while (!retval && job > current->rt_param.job_params.job_no)
		/* If the last job overran then job <= job_no and we
		 * don't send the task to sleep.
		 */
		retval = yat->complete_job();
      out:
	return retval;
}

/*	This is a helper syscall to query the current job sequence number.
 *
 *	returns 0 on successful query
 *	returns EPERM if task is not a real-time task.
 *      returns EFAULT if &job is not a valid pointer.
 */
asmlinkage long sys_query_job_no(unsigned int __user *job)
{
	int retval = -EPERM;
	if (is_realtime(current))
		retval = put_user(current->rt_param.job_params.job_no, job);

	return retval;
}

/* sys_null_call() is only used for determining raw system call
 * overheads (kernel entry, kernel exit). It has no useful side effects.
 * If ts is non-NULL, then the current Feather-Trace time is recorded.
 */
asmlinkage long sys_null_call(cycles_t __user *ts)
{
	long ret = 0;
	cycles_t now;

	if (ts) {
		now = get_cycles();
		ret = put_user(now, ts);
	}

	return ret;
}

asmlinkage long sys_reservation_create(int type, void __user *config)
{
	return yat->reservation_create(type, config);
}

asmlinkage long sys_reservation_destroy(unsigned int reservation_id, int cpu)
{
	return yat->reservation_destroy(reservation_id, cpu);
}

/* p is a real-time task. Re-init its state as a best-effort task. */
static void reinit_yat_state(struct task_struct* p, int restore)
{
	struct rt_task  user_config = {};
	void*  ctrl_page     = NULL;

	if (restore) {
		/* Safe user-space provided configuration data.
		 * and allocated page. */
		user_config = p->rt_param.task_params;
		ctrl_page   = p->rt_param.ctrl_page;
	}

	/* We probably should not be inheriting any task's priority
	 * at this point in time.
	 */
	WARN_ON(p->rt_param.inh_task);

	/* Cleanup everything else. */
	memset(&p->rt_param, 0, sizeof(p->rt_param));

	/* Restore preserved fields. */
	if (restore) {
		p->rt_param.task_params = user_config;
		p->rt_param.ctrl_page   = ctrl_page;
	}
}

static long __yat_admit_task(struct task_struct* tsk)
{
	long err;

	INIT_LIST_HEAD(&tsk_rt(tsk)->list);

	/* allocate heap node for this task */
	tsk_rt(tsk)->heap_node = bheap_node_alloc(GFP_ATOMIC);
	tsk_rt(tsk)->rel_heap = release_heap_alloc(GFP_ATOMIC);

	if (!tsk_rt(tsk)->heap_node || !tsk_rt(tsk)->rel_heap) {
		printk(KERN_WARNING "yat: no more heap node memory!?\n");

		return -ENOMEM;
	} else {
		bheap_node_init(&tsk_rt(tsk)->heap_node, tsk);
	}

	preempt_disable();

	err = yat->admit_task(tsk);

	if (!err) {
		sched_trace_task_name(tsk);
		sched_trace_task_param(tsk);
		atomic_inc(&rt_task_count);
	}

	preempt_enable();

	return err;
}

long yat_admit_task(struct task_struct* tsk)
{
	long retval = 0;

	BUG_ON(is_realtime(tsk));

	tsk_rt(tsk)->heap_node = NULL;
	tsk_rt(tsk)->rel_heap = NULL;

	if (get_rt_relative_deadline(tsk) == 0 ||
	    get_exec_cost(tsk) >
			min(get_rt_relative_deadline(tsk), get_rt_period(tsk)) ) {
		TRACE_TASK(tsk,
			"yat admit: invalid task parameters "
			"(e = %lu, p = %lu, d = %lu)\n",
			get_exec_cost(tsk), get_rt_period(tsk),
			get_rt_relative_deadline(tsk));
		retval = -EINVAL;
		goto out;
	}

	retval = __yat_admit_task(tsk);

out:
	if (retval) {
		if (tsk_rt(tsk)->heap_node)
			bheap_node_free(tsk_rt(tsk)->heap_node);
		if (tsk_rt(tsk)->rel_heap)
			release_heap_free(tsk_rt(tsk)->rel_heap);
	}
	return retval;
}

void yat_clear_state(struct task_struct* tsk)
{
    BUG_ON(bheap_node_in_heap(tsk_rt(tsk)->heap_node));
    bheap_node_free(tsk_rt(tsk)->heap_node);
    release_heap_free(tsk_rt(tsk)->rel_heap);

    atomic_dec(&rt_task_count);
    reinit_yat_state(tsk, 1);
}

/* called from sched_setscheduler() */
void yat_exit_task(struct task_struct* tsk)
{
	if (is_realtime(tsk)) {
		sched_trace_task_completion(tsk, 1);

		/********** free mrsp variables **********/
		kfree(tsk->rt_param.task_params.mrsp_locks);
		kfree(tsk->rt_param.task_params.np_timer);
		/********** free mrsp variables end **********/

		kfree(tsk->rt_param.task_params.msrp_locks);
		kfree(tsk->rt_param.task_params.fifop_locks);

		yat->task_exit(tsk);
	}
}

static DECLARE_RWSEM(plugin_switch_mutex);

void yat_plugin_switch_disable(void)
{
	down_read(&plugin_switch_mutex);
}

void yat_plugin_switch_enable(void)
{
	up_read(&plugin_switch_mutex);
}

// 调度器变更
static int __do_plugin_switch(struct sched_plugin* plugin)
{
	int ret;

	/* 如果存在正在活动的实时任务，则不能进行切换 */
	if (atomic_read(&rt_task_count) == 0) {
		TRACE("deactivating plugin %s\n", yat->plugin_name);
		ret = yat->deactivate_plugin();
		if (0 != ret)
			goto out;

		TRACE("activating plugin %s\n", plugin->plugin_name);
		ret = plugin->activate_plugin();
		if (0 != ret) {
			printk(KERN_INFO "Can't activate %s (%d).\n",
			       plugin->plugin_name, ret);
			plugin = &linux_sched_plugin;
		}

		printk(KERN_INFO "Switching to yat_sched plugin %s.\n", plugin->plugin_name);
		yat = plugin;
	} else
		ret = -EBUSY;
out:
	TRACE("do_plugin_switch() => %d\n", ret);
	return ret;
}

static atomic_t ready_to_switch;

static int do_plugin_switch(void *_plugin)
{
	unsigned long flags;
	int ret = 0;

	local_save_flags(flags);
	local_irq_disable();
	hard_irq_disable();

	if (atomic_dec_and_test(&ready_to_switch))
	{
		ret = __do_plugin_switch((struct sched_plugin*) _plugin);
		atomic_set(&ready_to_switch, INT_MAX);
	}

	do {
		cpu_relax();
	} while (atomic_read(&ready_to_switch) != INT_MAX);

	local_irq_restore(flags);
	return ret;
}

/* Switching a plugin in use is tricky.
 * We must watch out that no real-time tasks exists
 * (and that none is created in parallel) and that the plugin is not
 * currently in use on any processor (in theory).
 */
int switch_sched_plugin(struct sched_plugin* plugin)
{
	int err;
	struct domain_proc_info* domain_info;

	BUG_ON(!plugin);

	if (atomic_read(&rt_task_count) == 0) {
		down_write(&plugin_switch_mutex);

		deactivate_domain_proc();

		get_online_cpus();
		atomic_set(&ready_to_switch, num_online_cpus());
		err = stop_cpus(cpu_online_mask, do_plugin_switch, plugin);
		put_online_cpus();

		if (!yat->get_domain_proc_info(&domain_info))
			activate_domain_proc(domain_info);

		up_write(&plugin_switch_mutex);
		return err;
	} else
		return -EBUSY;
}

/* Called upon fork.
 * p is the newly forked task.
 */
void yat_fork(struct task_struct* p)
{
	/* non-rt tasks might have ctrl_page set */
	tsk_rt(p)->ctrl_page = NULL;

	if (is_realtime(p)) {
		reinit_yat_state(p, 1);
		if (yat->fork_task(p)) {
			if (__yat_admit_task(p))
				/* something went wrong, give up */
				p->sched_reset_on_fork = 1;
		} else {
			/* clean out any yat related state */
			reinit_yat_state(p, 0);

			TRACE_TASK(p, "fork: real-time status denied\n");
			/* Don't let the child be a real-time task. */
			p->sched_reset_on_fork = 1;
		}
	}

	/* od tables are never inherited across a fork */
	p->od_table = NULL;
}

/* Called upon execve().
 * current is doing the exec.
 * Don't let address space specific stuff leak.
 */
void yat_exec(void)
{
	struct task_struct* p = current;

	if (is_realtime(p)) {
		WARN_ON(p->rt_param.inh_task);
		if (tsk_rt(p)->ctrl_page) {
			free_page((unsigned long) tsk_rt(p)->ctrl_page);
			tsk_rt(p)->ctrl_page = NULL;
		}
	}
}

/* Called when dead_tsk is being deallocated
 */
void exit_yat(struct task_struct *dead_tsk)
{
	/* We also allow non-RT tasks to
	 * allocate control pages to allow
	 * measurements with non-RT tasks.
	 * So check if we need to free the page
	 * in any case.
	 */
	if (tsk_rt(dead_tsk)->ctrl_page) {
		TRACE_TASK(dead_tsk,
			   "freeing ctrl_page %p\n",
			   tsk_rt(dead_tsk)->ctrl_page);
		free_page((unsigned long) tsk_rt(dead_tsk)->ctrl_page);
	}

	/* Tasks should not be real-time tasks any longer at this point. */
	BUG_ON(is_realtime(dead_tsk));
}

void yat_do_exit(struct task_struct *exiting_tsk)
{
	/* This task called do_exit(), but is still a real-time task. To avoid
	 * complications later, we force it to be a non-real-time task now. */

	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

	TRACE_TASK(exiting_tsk, "exiting, demoted to SCHED_FIFO\n");
	sched_setscheduler_nocheck(exiting_tsk, SCHED_FIFO, &param);
}

void yat_dealloc(struct task_struct *tsk)
{
	/* tsk is no longer a real-time task */
	TRACE_TASK(tsk, "Deallocating real-time task data\n");
	yat->task_cleanup(tsk);
	yat_clear_state(tsk);
}

/* move current non-RT task to a specific CPU */
int yat_be_migrate_to(int cpu)
{
	struct cpumask single_cpu_aff;

	cpumask_clear(&single_cpu_aff);
	cpumask_set_cpu(cpu, &single_cpu_aff);
	return sched_setaffinity(current->pid, &single_cpu_aff);
}

#ifdef CONFIG_MAGIC_SYSRQ
static void sysrq_handle_kill_rt_tasks(int key)
{
	struct task_struct *t;
	read_lock(&tasklist_lock);
	// We do this like the sysrq kill handler in drivers/tty/sysrq.c
	for_each_process(t) {
		if (is_realtime(t)) {
			do_send_sig_info(SIGKILL, SEND_SIG_PRIV, t, PIDTYPE_MAX);
		}
	}
	read_unlock(&tasklist_lock);
}

static struct sysrq_key_op sysrq_kill_rt_tasks_op = {
	.handler	= sysrq_handle_kill_rt_tasks,
	.help_msg	= "quit-rt-tasks(x)",
	.action_msg	= "sent SIGKILL to all YAT^RT real-time tasks",
};
#endif

extern struct sched_plugin linux_sched_plugin;

static int yat_shutdown_nb(struct notifier_block *unused1,
				unsigned long unused2, void *unused3)
{
	/* Attempt to switch back to regular Linux scheduling.
	 * Forces the active plugin to clean up.
	 */
	if (yat != &linux_sched_plugin) {
		int ret = switch_sched_plugin(&linux_sched_plugin);
		if (ret) {
			printk("Auto-shutdown of active Yat plugin failed.\n");
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block shutdown_notifier = {
	.notifier_call = yat_shutdown_nb,
};

/**
 * Triggering hrtimers on specific cpus as required by arm_release_timer(_on)
 */
#ifdef CONFIG_SMP

/**
 *  hrtimer_pull - smp_call_function_single_async callback on remote cpu
 */
void hrtimer_pull(void *csd_info)
{
	struct hrtimer_start_on_info *info = csd_info;
	TRACE("pulled timer 0x%x\n", info->timer);
	hrtimer_start_range_ns(info->timer, info->time, 0, info->mode);
}

/**
 *  hrtimer_start_on - trigger timer arming on remote cpu
 *  @cpu:	remote cpu
 *  @info:	save timer information for enqueuing on remote cpu
 *  @timer:	timer to be pulled
 *  @time:	expire time
 *  @mode:	timer mode
 */
void hrtimer_start_on(int cpu, struct hrtimer_start_on_info *info,
		struct hrtimer *timer, ktime_t time,
		const enum hrtimer_mode mode)
{
	info->timer = timer;
	info->time  = time;
	info->mode  = mode;

	/* initialize call_single_data struct */
	info->csd.func  = &hrtimer_pull;
	info->csd.info  = info;
	info->csd.flags = 0;

	/* initiate pull  */
	preempt_disable();
	if (cpu == smp_processor_id()) {
		/* start timer locally; we may get called
		* with rq->lock held, do not wake up anything
		*/
		TRACE("hrtimer_start_on: starting on local CPU\n");
		hrtimer_start(info->timer, info->time, info->mode);
	} else {
		/* call hrtimer_pull() on remote cpu
		* to start remote timer asynchronously
		*/
		TRACE("hrtimer_start_on: pulling to remote CPU\n");
		smp_call_function_single_async(cpu, &info->csd);
	}
	preempt_enable();
}

#endif /* CONFIG_SMP */

static int __init _init_yat(void)
{
	/*      Common initializers,
	 *      mode change lock is used to enforce single mode change
	 *      operation.
	 */
	printk("Starting YAT^RT kernel\n");

	register_sched_plugin(&linux_sched_plugin);

	bheap_node_cache    = KMEM_CACHE(bheap_node, SLAB_PANIC);
	release_heap_cache = KMEM_CACHE(release_heap, SLAB_PANIC);

#ifdef CONFIG_MAGIC_SYSRQ
	/* offer some debugging help */
	if (!register_sysrq_key('x', &sysrq_kill_rt_tasks_op))
		printk("Registered kill rt tasks magic sysrq.\n");
	else
		printk("Could not register kill rt tasks magic sysrq.\n");
#endif

	init_yat_proc();

	register_reboot_notifier(&shutdown_notifier);

	return 0;
}

static void _exit_yat(void)
{
	unregister_reboot_notifier(&shutdown_notifier);

	exit_yat_proc();
	kmem_cache_destroy(bheap_node_cache);
	kmem_cache_destroy(release_heap_cache);
}

module_init(_init_yat);
module_exit(_exit_yat);
