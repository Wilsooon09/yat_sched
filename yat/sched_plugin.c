/* sched_plugin.c -- core infrastructure for the scheduler plugin system
 *
 * This file includes the initialization of the plugin system, the no-op Linux
 * scheduler plugin, some dummy functions, and some helper functions.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#include <yat/yat.h>
#include <yat/sched_plugin.h>
#include <yat/preempt.h>
#include <yat/jobs.h>
#include <yat/budget.h>
#include <yat/np.h>

/*
 * Generic function to trigger preemption on either local or remote cpu
 * from scheduler plugins. The key feature is that this function is
 * non-preemptive section aware and does not invoke the scheduler / send
 * IPIs if the to-be-preempted task is actually non-preemptive.
 */
void preempt_if_preemptable(struct task_struct* t, int cpu)
{
	/* t is the real-time task executing on CPU on_cpu If t is NULL, then
	 * on_cpu is currently scheduling background work.
	 */

	int reschedule = 0;

	if (!t)
		/* move non-real-time task out of the way */
		reschedule = 1;
	else {
		if (smp_processor_id() == cpu) {
			/* local CPU case */
			/* check if we need to poke userspace */
			if (is_user_np(t))
				/* Yes, poke it. This doesn't have to be atomic since
				 * the task is definitely not executing. */
				request_exit_np(t);
			else if (!is_kernel_np(t))
				/* only if we are allowed to preempt the
				 * currently-executing task */
				reschedule = 1;
		} else {
			/* Remote CPU case.  Only notify if it's not a kernel
			 * NP section and if we didn't set the userspace
			 * flag. */
			reschedule = !(is_kernel_np(t) || request_exit_np_atomic(t));
		}
	}
	if (likely(reschedule))
		yat_reschedule(cpu);
}


/*************************************************************
 *                   Dummy plugin functions                  *
 *************************************************************/

static void yat_dummy_finish_switch(struct task_struct * prev)
{
}

static struct task_struct* yat_dummy_schedule(struct task_struct * prev)
{
	sched_state_task_picked();
	return NULL;
}

static bool yat_dummy_should_wait_for_stack(struct task_struct *next)
{
	return true; /* by default, wait indefinitely */
}

static void yat_dummy_next_became_invalid(struct task_struct *next)
{
}

static bool yat_dummy_post_migration_validate(struct task_struct *next)
{
	return true; /* by default, anything is ok */
}

static long yat_dummy_admit_task(struct task_struct* tsk)
{
	printk(KERN_CRIT "YAT^RT: Linux plugin rejects %s/%d.\n",
		tsk->comm, tsk->pid);
	return -EINVAL;
}

static bool yat_dummy_fork_task(struct task_struct* tsk)
{
	/* Default behavior: return false to demote to non-real-time task */
	return false;
}

static void yat_dummy_task_new(struct task_struct *t, int on_rq, int running)
{
}

static void yat_dummy_task_wake_up(struct task_struct *task)
{
}

static void yat_dummy_task_block(struct task_struct *task)
{
}

static void yat_dummy_task_exit(struct task_struct *task)
{
}

static void yat_dummy_task_cleanup(struct task_struct *task)
{
}

static long yat_dummy_complete_job(void)
{
	return -ENOSYS;
}

static long yat_dummy_activate_plugin(void)
{
	return 0;
}

static long yat_dummy_deactivate_plugin(void)
{
	return 0;
}

static long yat_dummy_get_domain_proc_info(struct domain_proc_info **d)
{
	*d = NULL;
	return 0;
}

static void yat_dummy_synchronous_release_at(lt_t time_zero)
{
	/* ignore */
}

static long yat_dummy_task_change_params(
	struct task_struct *task,
	struct rt_task *new_params)
{
	/* by default, do not allow changes to task parameters */
	return -EBUSY;
}

#ifdef CONFIG_YAT_LOCKING

// 默认锁分配
static long yat_dummy_allocate_lock(struct yat_lock **lock, int type,
				       void* __user config)
{
	return -ENXIO;
}

#endif

// 默认保留站创建
static long  yat_dummy_reservation_create(
	int reservation_type,
	void* __user config)
{
	return -ENOSYS;
}

// 默认保留站销毁
static long yat_dummy_reservation_destroy(unsigned int reservation_id, int cpu)
{
	return -ENOSYS;
}

/* The default scheduler plugin. It doesn't do anything and lets Linux do its job.
 */
struct sched_plugin linux_sched_plugin = {
	.plugin_name 		    = "Linux",
	.task_new   		    = yat_dummy_task_new,
	.task_exit 			    = yat_dummy_task_exit,
	.task_wake_up 		    = yat_dummy_task_wake_up,
	.task_block 		    = yat_dummy_task_block,
	.complete_job 		    = yat_dummy_complete_job,
	.schedule 			    = yat_dummy_schedule,
	.finish_switch 		    = yat_dummy_finish_switch,
	.activate_plugin 	    = yat_dummy_activate_plugin,
	.deactivate_plugin      = yat_dummy_deactivate_plugin,
	.get_domain_proc_info   = yat_dummy_get_domain_proc_info,
	.synchronous_release_at = yat_dummy_synchronous_release_at,
	.should_wait_for_stack  = yat_dummy_should_wait_for_stack,
	.next_became_invalid    = yat_dummy_next_became_invalid,
	.task_cleanup 			= yat_dummy_task_cleanup,
#ifdef CONFIG_YAT_LOCKING
	.allocate_lock 			= yat_dummy_allocate_lock,
#endif
	.admit_task 			= yat_dummy_admit_task,
	.fork_task 				= yat_dummy_fork_task,
};

/*
 *	The reference to current plugin that is used to schedule tasks within
 *	the system. It stores references to actual function implementations
 *	Should be initialized by calling "init_***_plugin()"
 */
struct sched_plugin *yat = &linux_sched_plugin;

/* the list of registered scheduling plugins */
static LIST_HEAD(sched_plugins);
static DEFINE_RAW_SPINLOCK(sched_plugins_lock);

#define CHECK(func) {\
	if (!plugin->func) \
		plugin->func = yat_dummy_ ## func;}

/* FIXME: get reference to module  */
int register_sched_plugin(struct sched_plugin* plugin)
{
	printk(KERN_INFO "Registering YAT^RT plugin %s.\n",
	       plugin->plugin_name);

	/* make sure we don't trip over null pointers later */
	CHECK(finish_switch);
	CHECK(schedule);
	CHECK(should_wait_for_stack);
	CHECK(post_migration_validate);
	CHECK(next_became_invalid);
	CHECK(task_wake_up);
	CHECK(task_exit);
	CHECK(task_cleanup);
	CHECK(task_block);
	CHECK(task_new);
	CHECK(task_change_params);
	CHECK(complete_job);
	CHECK(activate_plugin);
	CHECK(deactivate_plugin);
	CHECK(get_domain_proc_info);
#ifdef CONFIG_YAT_LOCKING
	CHECK(allocate_lock);
#endif
	CHECK(admit_task);
	CHECK(fork_task);
	CHECK(synchronous_release_at);
	CHECK(reservation_destroy);
	CHECK(reservation_create);

	if (!plugin->wait_for_release_at)
		plugin->wait_for_release_at = default_wait_for_release_at;

	if (!plugin->current_budget)
		plugin->current_budget = yat_current_budget;

	raw_spin_lock(&sched_plugins_lock);
	list_add(&plugin->list, &sched_plugins);
	raw_spin_unlock(&sched_plugins_lock);

	return 0;
}


/* FIXME: reference counting, etc. */
struct sched_plugin* find_sched_plugin(const char* name)
{
	struct list_head *pos;
	struct sched_plugin *plugin;

	raw_spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		if (!strcmp(plugin->plugin_name, name))
		    goto out_unlock;
	}
	plugin = NULL;

out_unlock:
	raw_spin_unlock(&sched_plugins_lock);
	return plugin;
}

void print_sched_plugins(struct seq_file *m)
{
	struct list_head *pos;
	struct sched_plugin *plugin;

	raw_spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		seq_printf(m, "%s\n", plugin->plugin_name);
	}
	raw_spin_unlock(&sched_plugins_lock);
}
