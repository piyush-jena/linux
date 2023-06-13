#include "sched.h"
#include <linux/sched/myrr.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/sched/task.h>

void init_myrr_rq(struct myrr_rq *myrr_rq)
{
	INIT_LIST_HEAD(&myrr_rq->task_list);
	myrr_rq->myrr_nr_running = 0;
}

static inline int on_myrr_rq(struct sched_myrr_entity *myrr_se)
{
	return myrr_se->on_rq;
}

static inline struct task_struct *myrr_task_of(struct sched_myrr_entity *myrr_se)
{
	return container_of(myrr_se, struct task_struct, myrr_se);
}

static void
enqueue_myrr_entity(struct rq *rq, struct sched_myrr_entity *myrr_se, bool head)
{
	struct list_head *queue = &rq->myrr_rq.task_list;

	if (head)
		list_add(&myrr_se->task_list, queue);
	else
		list_add_tail(&myrr_se->task_list, queue);

	myrr_se->on_rq = 1;

	++rq->myrr_rq.myrr_nr_running;
}

static void
dequeue_myrr_entity(struct rq *rq, struct sched_myrr_entity *myrr_se)
{
	list_del_init(&myrr_se->task_list);
	myrr_se->on_rq = 0;
	--rq->myrr_rq.myrr_nr_running;
}

static void enqueue_task_myrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_myrr_entity *myrr_se = &p->myrr_se;

	if (flags & ENQUEUE_WAKEUP)
		myrr_se->timeout = 0;

	enqueue_myrr_entity(rq, myrr_se, flags & ENQUEUE_HEAD);
	add_nr_running(rq, 1);
}

static void update_curr_myrr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	delta_exec = rq_clock_task(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
			max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq_clock_task(rq);
	cgroup_account_cputime(curr, delta_exec);
}

static void dequeue_task_myrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_myrr_entity *myrr_se = &p->myrr_se;

	update_curr_myrr(rq);
	dequeue_myrr_entity(rq, myrr_se);
	sub_nr_running(rq, 1);
}

static void requeue_task_myrr(struct rq *rq, struct task_struct *p)
{
	list_move_tail(&p->myrr_se.task_list, &rq->myrr_rq.task_list);
}

static void yield_task_myrr(struct rq *rq)
{
	requeue_task_myrr(rq, rq->curr);
}

/* No preemption */
static void check_preempt_curr_myrr(struct rq *rq, struct task_struct *p, int flags)
{
}

static inline void set_next_task_myrr(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);
}

static struct task_struct *pick_next_task_myrr(struct rq *rq)
{
	struct task_struct *next;
	struct sched_myrr_entity *next_se;

	if (!rq->myrr_rq.myrr_nr_running)
		return NULL;

	next_se = list_first_entry(&rq->myrr_rq.task_list, struct sched_myrr_entity, task_list);
	next = myrr_task_of(next_se);
	if (!next)
		return NULL;

	next->se.exec_start = rq_clock_task(rq);
	set_next_task_myrr(rq, next, true);
	return next;
}

static void task_tick_myrr(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_myrr_entity *myrr_se = &p->myrr_se;

	update_curr_myrr(rq);

	if (p->policy != SCHED_MYRR)
		return;

	if (--myrr_se->time_slice)
		return;

	myrr_se->time_slice = MYRR_TIMESLICE;

	if (myrr_se->task_list.prev != myrr_se->task_list.next) {
		requeue_task_myrr(rq, p);
		resched_curr(rq);
		return;
	}
}

/* No preemption so no priority */
static void
prio_changed_myrr(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void switched_to_myrr(struct rq *rq, struct task_struct *p)
{
}

static unsigned int get_rr_interval_myrr(struct rq *rq, struct task_struct *task)
{
	return MYRR_TIMESLICE;
}

static void put_prev_task_myrr(struct rq *rq, struct task_struct *p)
{
	if (on_myrr_rq(&p->myrr_se))
		update_curr_myrr(rq);
}

#ifdef CONFIG_SMP

static int can_migrate_task(struct task_struct *p, struct rq *src_rq, struct rq *dst_rq)
{
	// p's policy has to be MY_RR
	if (p->policy != SCHED_MYRR)
		return 0;
	// if cpu is offline then don't move
	if (!cpu_active(dst_rq->cpu))
		return 0;
	// Do not steal a task from CPUs with fewer than 2 tasks.
	if (src_rq->myrr_rq.myrr_nr_running < 2)
		return 0;
	// Make sure to respect the CPU affinity of a given task.
	if (!cpumask_test_cpu(dst_rq->cpu, p->cpus_ptr))
		return 0;
	// Do not move tasks that are currently running on a CPU (obviously).
	if (task_running(src_rq, p))
		return 0;
	// Make sure to respect per-CPU kthreads. These should remain on their specified CPUs.
	if (kthread_is_per_cpu(p))
		return 0;
	// if not in current cpu then don't move
	if (task_cpu(p) != src_rq->cpu)
		return 0;

	return 1;
}

static int
select_task_rq_myrr(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct rq *rq;
	int cpus;
	int min;
	int best_cpu;
	cpumask_t cpumask = p->cpus_mask;

	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		return cpu;

	rcu_read_lock();

	min = -1;

	for_each_cpu(cpus, &cpumask) {
		rq = cpu_rq(cpus);

		if ((min == -1 || min > rq->nr_running) && cpu_online(cpus)) {
			min = rq->nr_running;
			best_cpu = cpus;
		}
	}

	rcu_read_unlock();

	if (min == -1)
		return cpu;

	return best_cpu;
}

static struct task_struct *pick_loadable_task(struct rq *rq, struct rq *dst_rq)
{
	struct list_head *head = &rq->myrr_rq.task_list;
	struct task_struct *p;
	struct sched_myrr_entity *se;

	// Do not steal a task from CPUs with fewer than 2 tasks.
	if (rq->myrr_rq.myrr_nr_running < 2)
		return NULL;

	se = list_last_entry(&rq->myrr_rq.task_list, struct sched_myrr_entity, task_list);
	p = myrr_task_of(se);

	list_for_each_entry(se, head, task_list) {
		p = myrr_task_of(se);
		if (can_migrate_task(p, rq, dst_rq))
			return p;
	}

	return NULL;
}

static int balance_myrr(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	int max_nr_running = 0;
	int this_cpu = rq->cpu, cpu;
	struct task_struct *p;
	struct rq *src_rq, *busiest_rq;

	rq_unpin_lock(rq, rf);

	if (rq->nr_running != 0) {
		rq_repin_lock(rq, rf);
		return 0;
	}

	for_each_online_cpu(cpu) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		if (max_nr_running >= src_rq->myrr_rq.myrr_nr_running)
			continue;

		max_nr_running = src_rq->myrr_rq.myrr_nr_running;
		busiest_rq = cpu_rq(cpu);
	}

	if (max_nr_running != 0) {
		double_lock_balance(rq, busiest_rq);

		p = pick_loadable_task(busiest_rq, rq);

		if (!p) {
			double_unlock_balance(rq, busiest_rq);
			goto out;
		}

		deactivate_task(busiest_rq, p, 0);
		set_task_cpu(p, this_cpu);
		activate_task(rq, p, 0);

		double_unlock_balance(rq, busiest_rq);

		resched_curr(rq);

		rq_repin_lock(rq, rf);
		return 1;
	}

out:
	rq_repin_lock(rq, rf);

	return 0;
}

static void rq_online_myrr(struct rq *rq)
{
}

static void rq_offline_myrr(struct rq *rq)
{
}

static void task_woken_myrr(struct rq *rq, struct task_struct *p)
{
}

static void switched_from_myrr(struct rq *rq, struct task_struct *p)
{
}

#endif /* CONFIG_SMP */

const struct sched_class myrr_sched_class
	__section("__myrr_sched_class") = {
	.enqueue_task		= enqueue_task_myrr,
	.dequeue_task		= dequeue_task_myrr,
	.yield_task		= yield_task_myrr,

	.check_preempt_curr	= check_preempt_curr_myrr,

	.pick_next_task		= pick_next_task_myrr,
	.put_prev_task		= put_prev_task_myrr,
	.set_next_task          = set_next_task_myrr,

#ifdef CONFIG_SMP
	.balance		= balance_myrr,
	.select_task_rq		= select_task_rq_myrr,

	.rq_online		= rq_online_myrr,
	.rq_offline		= rq_offline_myrr,
	.task_woken		= task_woken_myrr,
	.set_cpus_allowed	= set_cpus_allowed_common,

	.switched_from		= switched_from_myrr,
#endif

	.task_tick		= task_tick_myrr,

	.switched_to		= switched_to_myrr,
	.prio_changed		= prio_changed_myrr,
	.get_rr_interval	= get_rr_interval_myrr,

	.update_curr		= update_curr_myrr,
#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};