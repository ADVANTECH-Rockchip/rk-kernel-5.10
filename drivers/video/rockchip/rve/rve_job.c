// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 *
 * Author: Huang Lee <Putin.li@rock-chips.com>
 */

#define pr_fmt(fmt) "rve_job: " fmt

#include "rve_job.h"
#include "rve_fence.h"
#include "rve_reg.h"

struct rve_job *
rve_scheduler_get_pending_job_list(struct rve_scheduler_t *scheduler)
{
	unsigned long flags;
	struct rve_job *job;

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	job = list_first_entry_or_null(&scheduler->todo_list,
		struct rve_job, head);

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	return job;
}

struct rve_job *
rve_scheduler_get_running_job(struct rve_scheduler_t *scheduler)
{
	unsigned long flags;
	struct rve_job *job;

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	job = scheduler->running_job;

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	return job;
}

struct rve_scheduler_t *rve_job_get_scheduler(struct rve_job *job)
{
	return job->scheduler;
}

struct rve_internal_ctx_t *rve_job_get_internal_ctx(struct rve_job *job)
{
	return job->ctx;
}

static void rve_job_free(struct rve_job *job)
{
#ifdef CONFIG_SYNC_FILE
	if (job->out_fence)
		dma_fence_put(job->out_fence);
#endif

	free_page((unsigned long)job);
}

static int rve_job_cleanup(struct rve_job *job)
{
	ktime_t now = ktime_get();

	if (DEBUGGER_EN(TIME)) {
		pr_info("(pid:%d) job clean use time = %lld\n", job->pid,
			ktime_us_delta(now, job->timestamp));
	}
	rve_job_free(job);

	return 0;
}

static struct rve_job *rve_job_alloc(struct rve_internal_ctx_t *ctx)
{
	struct rve_job *job = NULL;

	job = (struct rve_job *)get_zeroed_page(GFP_KERNEL | GFP_DMA32);
	if (!job)
		return NULL;

#ifdef CONFIG_SYNC_FILE
	spin_lock_init(&job->fence_lock);
#endif
	INIT_LIST_HEAD(&job->head);

	job->timestamp = ktime_get();
	job->pid = current->pid;
	job->regcmd_data = &ctx->regcmd_data[ctx->running_job_count];

	job->scheduler = rve_drvdata->scheduler[0];
	job->core = rve_drvdata->scheduler[0]->core;
	job->ctx = ctx;
	ctx->scheduler = job->scheduler;

	if (ctx->priority > 0) {
		if (ctx->priority > RVE_SCHED_PRIORITY_MAX)
			job->priority = RVE_SCHED_PRIORITY_MAX;
		else
			job->priority = ctx->priority;
	}

	return job;
}

static struct rve_internal_ctx_t *
rve_internal_ctx_lookup(struct rve_pending_ctx_manager *ctx_manager, uint32_t id)
{
	struct rve_internal_ctx_t *ctx = NULL;

	mutex_lock(&ctx_manager->lock);

	ctx = idr_find(&ctx_manager->ctx_id_idr, id);
	if (ctx == NULL)
		pr_err("can not find internal ctx from id[%d]", id);

	mutex_unlock(&ctx_manager->lock);

	return ctx;
}

/*
 * Called at driver close to release the internal ctx's id references.
 */
static int rve_internal_ctx_free_remove_idr_cb(int id, void *ptr, void *data)
{
	struct rve_internal_ctx_t *ctx = ptr;

	idr_remove(&rve_drvdata->pend_ctx_manager->ctx_id_idr, ctx->id);
	kfree(ctx);

	return 0;
}

static int rve_internal_ctx_free_remove_idr(struct rve_internal_ctx_t *ctx)
{
	struct rve_pending_ctx_manager *ctx_manager;

	ctx_manager = rve_drvdata->pend_ctx_manager;

	mutex_lock(&ctx_manager->lock);

	ctx_manager->ctx_count--;
	idr_remove(&ctx_manager->ctx_id_idr, ctx->id);
	kfree(ctx);

	mutex_unlock(&ctx_manager->lock);

	return 0;
}

static int rve_internal_ctx_signal(struct rve_job *job)
{
	struct rve_internal_ctx_t *ctx;
	struct rve_scheduler_t *scheduler;
	int finished_job_count;
	unsigned long flags;

	scheduler = rve_job_get_scheduler(job);
	if (scheduler == NULL) {
		pr_err("failed to get scheduler, %s(%d)\n", __func__, __LINE__);
		return -EFAULT;
	}

	ctx = rve_job_get_internal_ctx(job);
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("can not find internal ctx");
		return -EINVAL;
	}

	ctx->regcmd_data = job->regcmd_data;

	spin_lock_irqsave(&ctx->lock, flags);

	finished_job_count = ++ctx->finished_job_count;

	spin_unlock_irqrestore(&ctx->lock, flags);

	if (finished_job_count >= ctx->cmd_num) {
#ifdef CONFIG_SYNC_FILE
		if (ctx->out_fence)
			dma_fence_signal(ctx->out_fence);
#endif

		job->flags |= RVE_JOB_DONE;

		if (job->flags & RVE_ASYNC)
			rve_job_cleanup(job);

		wake_up(&scheduler->job_done_wq);

		spin_lock_irqsave(&ctx->lock, flags);

		ctx->is_running = false;
		ctx->out_fence = NULL;

		spin_unlock_irqrestore(&ctx->lock, flags);
	}

	return 0;
}

static void rve_job_dump_info(struct rve_job *job)
{
	pr_info("job: priority = %d, core = %d\n",
		job->priority, job->core);
}

static int rve_job_run(struct rve_job *job)
{
	struct rve_scheduler_t *scheduler;
	int ret = 0;

	scheduler = rve_job_get_scheduler(job);

#ifndef RVE_PD_AWAYS_ON
	/* enable power */
	ret = rve_power_enable(scheduler);
	if (ret < 0) {
		pr_err("power enable failed");
		return ret;
	}
#endif

	ret = scheduler->ops->init_reg(job);
	if (ret < 0) {
		pr_err("init reg failed");
		goto failed;
	}

	ret = scheduler->ops->set_reg(job, scheduler);
	if (ret < 0) {
		pr_err("set reg failed");
		goto failed;
	}

	/* for debug */
	if (DEBUGGER_EN(MSG))
		rve_job_dump_info(job);

	return ret;

failed:
#ifndef RVE_PD_AWAYS_ON
	rve_power_disable(scheduler);
#endif

	return ret;
}

static void rve_job_next(struct rve_scheduler_t *scheduler)
{
	struct rve_job *job = NULL;
	unsigned long flags;

next_job:
	spin_lock_irqsave(&scheduler->irq_lock, flags);

	if (scheduler->running_job ||
		list_empty(&scheduler->todo_list)) {
		spin_unlock_irqrestore(&scheduler->irq_lock, flags);
		return;
	}

	job = list_first_entry(&scheduler->todo_list, struct rve_job, head);

	list_del_init(&job->head);

	scheduler->job_count--;

	scheduler->running_job = job;

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	job->ret = rve_job_run(job);

	/* If some error before hw run */
	if (job->ret < 0) {
		pr_err("some error on rve_job_run before hw start, %s(%d)\n",
			__func__, __LINE__);

		spin_lock_irqsave(&scheduler->irq_lock, flags);

		scheduler->running_job = NULL;

		spin_unlock_irqrestore(&scheduler->irq_lock, flags);

		rve_internal_ctx_signal(job);

		goto next_job;
	}
}

static void rve_job_finish_and_next(struct rve_job *job, int ret)
{
	ktime_t now = ktime_get();
	struct rve_scheduler_t *scheduler;

	job->ret = ret;

	scheduler = rve_job_get_scheduler(job);

	if (DEBUGGER_EN(TIME)) {
		pr_info("hw use time = %lld\n", ktime_us_delta(now, job->hw_running_time));
		pr_info("(pid:%d) job done use time = %lld\n", job->pid,
			ktime_us_delta(now, job->timestamp));
	}

	rve_internal_ctx_signal(job);

	rve_job_next(scheduler);

#ifndef RVE_PD_AWAYS_ON
	rve_power_disable(scheduler);
#endif
}

void rve_job_done(struct rve_scheduler_t *scheduler, int ret)
{
	struct rve_job *job;
	unsigned long flags;
	u32 error_flag;
	uint32_t *cmd_reg;
	int i;

	ktime_t now = ktime_get();

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	job = scheduler->running_job;
	scheduler->running_job = NULL;

	scheduler->timer.busy_time += ktime_us_delta(now, job->hw_recoder_time);

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	spin_lock_irqsave(&job->ctx->lock, flags);

	job->ctx->debug_info.max_cost_time_per_sec =
		max(job->ctx->debug_info.last_job_hw_use_time,
			job->ctx->debug_info.max_cost_time_per_sec);
	job->ctx->debug_info.last_job_hw_use_time = job->hw_running_time - now;
	job->ctx->debug_info.hw_time_total += job->ctx->debug_info.last_job_hw_use_time;
	job->ctx->debug_info.last_job_use_time = job->timestamp - now;

	spin_unlock_irqrestore(&job->ctx->lock, flags);

	/* record CFG REG copy to user */
	cmd_reg = job->regcmd_data->cmd_reg;
	for (i = 0; i < 40; i++)
		cmd_reg[18 + i] = rve_read(RVE_CFG_REG + i * 4, scheduler);

	error_flag = rve_read(RVE_SWREG6_IVE_WORK_STA, scheduler);

	if (DEBUGGER_EN(MSG))
		pr_err("irq thread work_status[%.8x]\n", error_flag);

	rve_job_finish_and_next(job, ret);
}

static void rve_job_timeout_clean(struct rve_scheduler_t *scheduler)
{
	unsigned long flags;
	struct rve_job *job = NULL;
	ktime_t now = ktime_get();

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	job = scheduler->running_job;
	if (job && (job->flags & RVE_ASYNC) &&
	   (ktime_to_ms(ktime_sub(now, job->hw_running_time)) >= RVE_ASYNC_TIMEOUT_DELAY)) {
		scheduler->running_job = NULL;

		spin_unlock_irqrestore(&scheduler->irq_lock, flags);

		scheduler->ops->soft_reset(scheduler);

		rve_internal_ctx_signal(job);

#ifndef RVE_PD_AWAYS_ON
		rve_power_disable(scheduler);
#endif
	} else {
		spin_unlock_irqrestore(&scheduler->irq_lock, flags);
	}
}

static struct rve_scheduler_t *rve_job_schedule(struct rve_job *job)
{
	unsigned long flags;
	struct rve_scheduler_t *scheduler = NULL;
	struct rve_job *job_pos;
	bool first_match = 0;

	scheduler = rve_job_get_scheduler(job);
	if (scheduler == NULL) {
		pr_err("failed to get scheduler, %s(%d)\n", __func__, __LINE__);
		return NULL;
	}

	/* Only async will timeout clean */
	rve_job_timeout_clean(scheduler);

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	/* priority policy set by userspace */
	if (list_empty(&scheduler->todo_list)
		|| (job->priority == RVE_SCHED_PRIORITY_DEFAULT)) {
		list_add_tail(&job->head, &scheduler->todo_list);
	} else {
		list_for_each_entry(job_pos, &scheduler->todo_list, head) {
			if (job->priority > job_pos->priority &&
					(!first_match)) {
				list_add(&job->head, &job_pos->head);
				first_match = true;
			}

			/*
			 * Increase the priority of subsequent tasks
			 * after inserting into the list
			 */
			if (first_match)
				job_pos->priority++;
		}

		if (!first_match)
			list_add_tail(&job->head, &scheduler->todo_list);
	}

	scheduler->job_count++;

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	rve_job_next(scheduler);

	return scheduler;
}

static void rve_running_job_abort(struct rve_job *job)
{
	unsigned long flags;
	struct rve_scheduler_t *scheduler;

	scheduler = rve_job_get_scheduler(job);

	spin_lock_irqsave(&scheduler->irq_lock, flags);

	/* invalid job */
	if (job == scheduler->running_job)
		scheduler->running_job = NULL;

	spin_unlock_irqrestore(&scheduler->irq_lock, flags);

	rve_job_cleanup(job);
}

static void rve_invalid_job_abort(struct rve_job *job)
{
	rve_job_cleanup(job);
}

static inline int rve_job_wait(struct rve_job *job)
{
	struct rve_scheduler_t *scheduler;

	int left_time;
	ktime_t now;
	int ret;

	scheduler = rve_job_get_scheduler(job);

	left_time = wait_event_interruptible_timeout(scheduler->job_done_wq,
		job->flags & RVE_JOB_DONE, RVE_SYNC_TIMEOUT_DELAY);

	switch (left_time) {
	case 0:
		pr_err("%s timeout", __func__);
		scheduler->ops->soft_reset(scheduler);
		ret = -EBUSY;
		break;
	case -ERESTARTSYS:
		ret = -ERESTARTSYS;
		break;
	default:
		ret = 0;
		break;
	}

	now = ktime_get();

	if (DEBUGGER_EN(TIME))
		pr_info("%s use time = %lld\n", __func__,
			 ktime_to_us(ktime_sub(now, job->hw_running_time)));

	return ret;
}

#ifdef CONFIG_SYNC_FILE
static void rve_input_fence_signaled(struct dma_fence *fence,
					 struct dma_fence_cb *_waiter)
{
	struct rve_fence_waiter *waiter = (struct rve_fence_waiter *)_waiter;
	struct rve_scheduler_t *scheduler = NULL;

	ktime_t now;

	now = ktime_get();

	if (DEBUGGER_EN(TIME))
		pr_err("rve job wait in_fence signal use time = %lld\n",
			ktime_to_us(ktime_sub(now, waiter->job->timestamp)));

	scheduler = rve_job_schedule(waiter->job);

	if (scheduler == NULL)
		pr_err("failed to get scheduler, %s(%d)\n", __func__, __LINE__);

	kfree(waiter);
}
#endif

int rve_internal_ctx_alloc_to_get_idr_id(void)
{
	struct rve_pending_ctx_manager *ctx_manager;
	struct rve_internal_ctx_t *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL) {
		pr_err("can not kzalloc for rve_pending_ctx_manager\n");
		return -ENOMEM;
	}

	ctx_manager = rve_drvdata->pend_ctx_manager;
	if (ctx_manager == NULL) {
		pr_err("rve_pending_ctx_manager is null!\n");
		kfree(ctx);
		return -EFAULT;
	}

	spin_lock_init(&ctx->lock);

	/*
	 * Get the user-visible handle using idr. Preload and perform
	 * allocation under our spinlock.
	 */

	mutex_lock(&ctx_manager->lock);

	idr_preload(GFP_KERNEL);
	ctx->id = idr_alloc(&ctx_manager->ctx_id_idr, ctx, 1, 0, GFP_KERNEL);
	idr_preload_end();

	ctx_manager->ctx_count++;

	kref_init(&ctx->refcount);
	ctx->debug_info.pid = current->pid;
	ctx->debug_info.timestamp = ktime_get();

	mutex_unlock(&ctx_manager->lock);

	return ctx->id;
}

int rve_job_config_by_user_ctx(struct rve_user_ctx_t *user_ctx)
{
	struct rve_pending_ctx_manager *ctx_manager;
	struct rve_internal_ctx_t *ctx;
	struct rve_cmd_reg_array_t *regcmd_data;
	int ret = 0;
	unsigned long flags;

	ctx_manager = rve_drvdata->pend_ctx_manager;

	ctx = rve_internal_ctx_lookup(ctx_manager, user_ctx->id);
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("can not find internal ctx from id[%d]", user_ctx->id);
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->lock, flags);

	if (ctx->is_running) {
		pr_err("can not re-config when ctx is running");
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EFAULT;
	}

	spin_unlock_irqrestore(&ctx->lock, flags);

	regcmd_data = kmalloc(sizeof(struct rve_cmd_reg_array_t), GFP_KERNEL);
	if (regcmd_data == NULL) {
		pr_err("regcmd_data alloc error!\n");
		return -ENOMEM;
	}

	/* TODO: user cmd_num */
	user_ctx->cmd_num = 1;

	if (unlikely(copy_from_user(regcmd_data,
					u64_to_user_ptr(user_ctx->regcmd_data),
				    sizeof(struct rve_cmd_reg_array_t) * user_ctx->cmd_num))) {
		pr_err("regcmd_data copy_from_user failed\n");
		ret = -EFAULT;

		goto err_free_regcmd_data;
	}

	ctx->sync_mode = user_ctx->sync_mode;
	ctx->cmd_num = user_ctx->cmd_num;
	ctx->regcmd_data = regcmd_data;
	ctx->priority = user_ctx->priority;
	ctx->in_fence_fd = user_ctx->in_fence_fd;

	/* TODO: cmd addr */

	return ret;

err_free_regcmd_data:
	kfree(regcmd_data);
	return ret;
}

int rve_job_commit_by_user_ctx(struct rve_user_ctx_t *user_ctx)
{
	struct rve_pending_ctx_manager *ctx_manager;
	struct rve_internal_ctx_t *ctx;
	int ret = 0;
	unsigned long flags;
	int i;

	ctx_manager = rve_drvdata->pend_ctx_manager;

	ctx = rve_internal_ctx_lookup(ctx_manager, user_ctx->id);
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("can not find internal ctx from id[%d]", user_ctx->id);
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->lock, flags);

	if (ctx->is_running) {
		pr_err("can not re-config when ctx is running");
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EFAULT;
	}

	/* Reset */
	ctx->finished_job_count = 0;
	ctx->running_job_count = 0;
	ctx->is_running = true;

	spin_unlock_irqrestore(&ctx->lock, flags);

	for (i = 0; i < ctx->cmd_num; i++) {
		ret = rve_job_commit(ctx);
		if (ret < 0) {
			pr_err("rve_job_commit failed, i = %d\n", i);
			return -EFAULT;
		}

		ctx->running_job_count++;
	}

	user_ctx->out_fence_fd = ctx->out_fence_fd;

	if (unlikely(copy_to_user(u64_to_user_ptr(user_ctx->regcmd_data),
				  ctx->regcmd_data,
				  sizeof(struct rve_cmd_reg_array_t) * ctx->cmd_num))) {
		pr_err("ctx->regcmd_data copy_to_user failed\n");
		return -EFAULT;
	}

	return ret;
}

void rve_internal_ctx_kref_release(struct kref *ref)
{
	struct rve_internal_ctx_t *ctx;
	struct rve_scheduler_t *scheduler = NULL;
	struct rve_job *job_pos, *job_q, *job;
	int i;
	bool need_reset = false;
	unsigned long flags;
	ktime_t now = ktime_get();

	ctx = container_of(ref, struct rve_internal_ctx_t, refcount);

	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->is_running || ctx->finished_job_count >= ctx->cmd_num) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		goto free_ctx;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	for (i = 0; i < rve_drvdata->num_of_scheduler; i++) {
		scheduler = rve_drvdata->scheduler[i];

		spin_lock_irqsave(&scheduler->irq_lock, flags);

		list_for_each_entry_safe(job_pos, job_q, &scheduler->todo_list, head) {
			if (ctx->id == job_pos->ctx->id) {
				job = job_pos;
				list_del_init(&job_pos->head);

				scheduler->job_count--;
			}
		}

		/* for load */
		if (scheduler->running_job) {
			job = scheduler->running_job;

			if (job->ctx->id == ctx->id) {
				scheduler->running_job = NULL;
				scheduler->timer.busy_time += ktime_us_delta(now, job->hw_recoder_time);
				need_reset = true;
			}
		}

		spin_unlock_irqrestore(&scheduler->irq_lock, flags);

		if (need_reset) {
			pr_err("reset core[%d] by user cancel", scheduler->core);
			scheduler->ops->soft_reset(scheduler);

			rve_job_finish_and_next(job, 0);
		}
	}

	kfree(ctx->regcmd_data);

free_ctx:
	rve_internal_ctx_free_remove_idr(ctx);
}

int rve_job_cancel_by_user_ctx(uint32_t ctx_id)
{
	struct rve_pending_ctx_manager *ctx_manager;
	struct rve_internal_ctx_t *ctx;
	int ret = 0;

	ctx_manager = rve_drvdata->pend_ctx_manager;

	ctx = rve_internal_ctx_lookup(ctx_manager, ctx_id);
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("can not find internal ctx from id[%d]", ctx_id);
		return -EINVAL;
	}

	kref_put(&ctx->refcount, rve_internal_ctx_kref_release);

	return ret;
}

int rve_job_commit(struct rve_internal_ctx_t *ctx)
{
	struct rve_job *job = NULL;
	struct rve_scheduler_t *scheduler = NULL;
#ifdef CONFIG_SYNC_FILE
	struct dma_fence *in_fence;
#endif
	int ret = 0;

	/* TODO: remove */
	ctx->sync_mode = RVE_SYNC;

	job = rve_job_alloc(ctx);
	if (!job) {
		pr_err("failed to alloc rve job!\n");
		return -ENOMEM;
	}

	if (ctx->sync_mode == RVE_ASYNC) {
#ifdef CONFIG_SYNC_FILE
		job->flags |= RVE_ASYNC;

		if (ctx->out_fence) {
			job->out_fence = ctx->out_fence;
		} else {
			ret = rve_out_fence_alloc(job);
			if (ret) {
				rve_job_free(job);
				return ret;
			}

			ctx->out_fence = job->out_fence;
		}

		ctx->out_fence_fd = rve_out_fence_get_fd(job);

		if (DEBUGGER_EN(MSG))
			pr_info("in_fence_fd = %d", ctx->in_fence_fd);

		/* if input fence is valiable */
		if (ctx->in_fence_fd > 0) {
			in_fence = rve_get_input_fence(
				ctx->in_fence_fd);
			if (!in_fence) {
				pr_err("%s: failed to get input dma_fence\n",
					 __func__);
				rve_job_free(job);
				return ret;
			}

			/* close input fence fd */
			ksys_close(ctx->in_fence_fd);

			ret = dma_fence_get_status(in_fence);
			/* ret = 1: fence has been signaled */
			if (ret == 1) {
				scheduler = rve_job_schedule(job);

				if (scheduler == NULL) {
					pr_err("failed to get scheduler, %s(%d)\n",
						 __func__, __LINE__);
					goto invalid_job;
				}
				/* if input fence is valid */
			} else if (ret == 0) {
				ret = rve_add_dma_fence_callback(job,
					in_fence, rve_input_fence_signaled);
				if (ret < 0) {
					pr_err("%s: failed to add fence callback\n",
						 __func__);
					rve_job_free(job);
					return ret;
				}
			} else {
				pr_err("%s: fence status error\n", __func__);
				rve_job_free(job);
				return ret;
			}
		} else {
			scheduler = rve_job_schedule(job);

			if (scheduler == NULL) {
				pr_err("failed to get scheduler, %s(%d)\n",
					 __func__, __LINE__);
				goto invalid_job;
			}
		}

		return ret;
#else
		pr_err("can not support ASYNC mode, please enable CONFIG_SYNC_FILE");
		return -EFAULT;
#endif

	/* RVE_SYNC: wait until job finish */
	} else if (ctx->sync_mode == RVE_SYNC) {
		scheduler = rve_job_schedule(job);

		if (scheduler == NULL) {
			pr_err("failed to get scheduler, %s(%d)\n", __func__,
				 __LINE__);
			goto invalid_job;
		}

		ret = job->ret;
		if (ret < 0) {
			pr_err("some error on job, %s(%d)\n", __func__,
				 __LINE__);
			goto running_job_abort;
		}

		ret = rve_job_wait(job);
		if (ret < 0)
			goto running_job_abort;

		rve_job_cleanup(job);
	}
	return ret;

invalid_job:
	rve_invalid_job_abort(job);
	return ret;

/* only used by SYNC mode */
running_job_abort:
	rve_running_job_abort(job);
	return ret;
}

int rve_ctx_manager_init(struct rve_pending_ctx_manager **ctx_manager_session)
{
	struct rve_pending_ctx_manager *ctx_manager = NULL;

	*ctx_manager_session = kzalloc(sizeof(struct rve_pending_ctx_manager), GFP_KERNEL);
	if (*ctx_manager_session == NULL) {
		pr_err("can not kzalloc for rve_pending_ctx_manager\n");
		return -ENOMEM;
	}

	ctx_manager = *ctx_manager_session;

	mutex_init(&ctx_manager->lock);

	idr_init_base(&ctx_manager->ctx_id_idr, 1);

	return 0;
}

int rve_ctx_manager_remove(struct rve_pending_ctx_manager **ctx_manager_session)
{
	struct rve_pending_ctx_manager *ctx_manager = *ctx_manager_session;

	mutex_lock(&ctx_manager->lock);

	idr_for_each(&ctx_manager->ctx_id_idr, &rve_internal_ctx_free_remove_idr_cb, ctx_manager);
	idr_destroy(&ctx_manager->ctx_id_idr);

	mutex_unlock(&ctx_manager->lock);

	kfree(*ctx_manager_session);

	*ctx_manager_session = NULL;

	return 0;
}
