#
# Makefile for YAT^RT
#

obj-y     = sched_plugin.o yat.o \
	    preempt.o \
	    yat_proc.o \
	    budget.o \
	    clustered.o \
	    jobs.o \
	    sync.o \
	    rt_domain.o \
	    edf_common.o \
	    fp_common.o \
	    fdso.o \
	    locking.o \
	    srp.o \
	    bheap.o \
	    binheap.o \
	    ctrldev.o \
	    uncachedev.o \
		sched_fpfps_rs.o

obj-$(CONFIG_FEATHER_TRACE) += ft_event.o ftdev.o
obj-$(CONFIG_SCHED_TASK_TRACE) += sched_task_trace.o
obj-$(CONFIG_SCHED_DEBUG_TRACE) += sched_trace.o
obj-$(CONFIG_SCHED_OVERHEAD_TRACE) += trace.o


obj-y += reservations/
