#include <linux/sched.h>
#include <yat/yat.h>
#include <yat/fdso.h>
#include <yat/debug_trace.h>

#ifdef CONFIG_YAT_LOCKING

#include <linux/sched.h>
#include <yat/yat.h>
#include <yat/sched_plugin.h>
#include <yat/trace.h>
#include <yat/wait.h>

static int create_generic_lock(void** obj_ref, obj_type_t type, void* __user arg);
static int open_generic_lock(struct od_table_entry* entry, void* __user arg);
static int close_generic_lock(struct od_table_entry* entry);
static void destroy_generic_lock(obj_type_t type, void* sem);

struct fdso_ops generic_lock_ops = {
	.create  = create_generic_lock,
	.open    = open_generic_lock,
	.close   = close_generic_lock,
	.destroy = destroy_generic_lock
};

static inline bool is_lock(struct od_table_entry* entry)
{
	return entry->class == &generic_lock_ops;
}

static inline struct yat_lock* get_lock(struct od_table_entry* entry)
{
	BUG_ON(!is_lock(entry));
	return (struct yat_lock*) entry->obj->obj;
}

static  int create_generic_lock(void** obj_ref, obj_type_t type, void* __user arg)
{
	struct yat_lock* lock;
	int err;

	err = yat->allocate_lock(&lock, type, arg);
	if (err == 0)
		*obj_ref = lock;
	return err;
}

static int open_generic_lock(struct od_table_entry* entry, void* __user arg)
{
	struct yat_lock* lock = get_lock(entry);
	if (lock->ops->open)
		return lock->ops->open(lock, arg);
	else
		return 0; /* default: any task can open it */
}

static int close_generic_lock(struct od_table_entry* entry)
{
	struct yat_lock* lock = get_lock(entry);
	if (lock->ops->close)
		return lock->ops->close(lock);
	else
		return 0; /* default: closing succeeds */
}

static void destroy_generic_lock(obj_type_t type, void* obj)
{
	struct yat_lock* lock = (struct yat_lock*) obj;
	lock->ops->deallocate(lock);
}

asmlinkage long sys_yat_lock(int lock_od)
{
	long err = -EINVAL;
	struct od_table_entry* entry;
	struct yat_lock* l;

	TS_SYSCALL_IN_START;

	TS_SYSCALL_IN_END;

	// TS_LOCK_START;

	entry = get_entry_for_od(lock_od);
	if (entry && is_lock(entry)) {
		l = get_lock(entry);
		TRACE_CUR("attempts to lock 0x%p\n", l);
		err = l->ops->lock(l);
	}

	/* Note: task my have been suspended or preempted in between!  Take
	 * this into account when computing overheads. */
	// TS_LOCK_END;

	TS_SYSCALL_OUT_START;

	return err;
}

asmlinkage long sys_yat_unlock(int lock_od)
{
	long err = -EINVAL;
	struct od_table_entry* entry;
	struct yat_lock* l;

	TS_SYSCALL_IN_START;

	TS_SYSCALL_IN_END;

	// TS_UNLOCK_START;

	entry = get_entry_for_od(lock_od);
	if (entry && is_lock(entry)) {
		l = get_lock(entry);
		TRACE_CUR("attempts to unlock 0x%p\n", l);
		err = l->ops->unlock(l);
	}

	/* Note: task my have been preempted in between!  Take this into
	 * account when computing overheads. */
	// TS_UNLOCK_END;

	TS_SYSCALL_OUT_START;

	return err;
}

struct task_struct* __waitqueue_remove_first(wait_queue_head_t *wq)
{
	wait_queue_entry_t* q;
	struct task_struct* t = NULL;

	if (waitqueue_active(wq)) {
		q = list_entry(wq->head.next,
			       wait_queue_entry_t, entry);
		t = (struct task_struct*) q->private;
		__remove_wait_queue(wq, q);
	}
	return(t);
}

unsigned int __add_wait_queue_prio_exclusive(
	wait_queue_head_t* head,
	prio_wait_queue_t *new)
{
	struct list_head *pos;
	unsigned int passed = 0;

	new->wq.flags |= WQ_FLAG_EXCLUSIVE;

	/* find a spot where the new entry is less than the next */
	list_for_each(pos, &head->head) {
		prio_wait_queue_t* queued = list_entry(pos, prio_wait_queue_t,
						       wq.entry);

		if (unlikely(lt_before(new->priority, queued->priority) ||
			     (new->priority == queued->priority &&
			      new->tie_breaker < queued->tie_breaker))) {
			/* pos is not less than new, thus insert here */
			__list_add(&new->wq.entry, pos->prev, pos);
			goto out;
		}
		passed++;
	}

	/* if we get to this point either the list is empty or every entry
	 * queued element is less than new.
	 * Let's add new to the end. */
	list_add_tail(&new->wq.entry, &head->head);
out:
	return passed;
}


#else

struct fdso_ops generic_lock_ops = {};

asmlinkage long sys_yat_lock(int sem_od)
{
	return -ENOSYS;
}

asmlinkage long sys_yat_unlock(int sem_od)
{
	return -ENOSYS;
}

#endif
