// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 *
 * Author: Huang Lee <Putin.li@rock-chips.com>
 */

#define pr_fmt(fmt) "rve: " fmt

#include "rve_job.h"
#include "rve_fence.h"
#include "rve_debugger.h"
#include "rve_reg.h"

struct rve_drvdata_t *rve_drvdata;

/* set hrtimer */
static struct hrtimer timer;
static ktime_t kt;

static const struct rve_backend_ops rve_ops = {
	.get_version = rve_get_version,
	.set_reg = rve_set_reg,
	.init_reg = rve_init_reg,
	.soft_reset = rve_soft_reset
};

static enum hrtimer_restart hrtimer_handler(struct hrtimer *timer)
{
	struct rve_drvdata_t *rve = rve_drvdata;
	struct rve_scheduler_t *scheduler = NULL;
	struct rve_job *job = NULL;
	unsigned long flags;
	int i;

	ktime_t now = ktime_get();

	for (i = 0; i < rve->num_of_scheduler; i++) {
		scheduler = rve->scheduler[i];

		spin_lock_irqsave(&scheduler->irq_lock, flags);

		/* if timer action on job running */
		job = scheduler->running_job;
		if (job) {
			scheduler->timer.busy_time += ktime_us_delta(now, job->hw_recoder_time);
			job->hw_recoder_time = now;
		}

		scheduler->timer.busy_time_record = scheduler->timer.busy_time;
		scheduler->timer.busy_time = 0;

		spin_unlock_irqrestore(&scheduler->irq_lock, flags);

		/* monitor */
		if (job && job->ctx)
			rve_get_monitor_info(job->ctx, scheduler);
	}

	hrtimer_forward_now(timer, kt);
	return HRTIMER_RESTART;
}

static void rve_init_timer(void)
{
	kt = ktime_set(0, RVE_LOAD_INTERVAL);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&timer, kt, HRTIMER_MODE_REL);
	timer.function = hrtimer_handler;
}

static void rve_cancel_timer(void)
{
	hrtimer_cancel(&timer);
}

#ifndef RVE_PD_AWAYS_ON
int rve_power_enable(struct rve_scheduler_t *scheduler)
{
	int ret = -EINVAL;
	int i;

	pm_runtime_get_sync(scheduler->dev);
	pm_stay_awake(scheduler->dev);

	for (i = 0; i < scheduler->num_clks; i++) {
		if (!IS_ERR(scheduler->clks[i])) {
			ret = clk_prepare_enable(scheduler->clks[i]);
			if (ret < 0)
				goto err_enable_clk;
		}
	}

	return 0;

err_enable_clk:
	for (--i; i >= 0; --i)
		if (!IS_ERR(scheduler->clks[i]))
			clk_disable_unprepare(scheduler->clks[i]);

	pm_relax(scheduler->dev);
	pm_runtime_put_sync_suspend(scheduler->dev);

	scheduler->pd_refcount++;

	return ret;
}

int rve_power_disable(struct rve_scheduler_t *scheduler)
{
	int i;

	for (i = scheduler->num_clks - 1; i >= 0; i--)
		if (!IS_ERR(scheduler->clks[i]))
			clk_disable_unprepare(scheduler->clks[i]);

	pm_relax(scheduler->dev);
	pm_runtime_put_sync_suspend(scheduler->dev);

	scheduler->pd_refcount--;

	return 0;
}

#endif //RVE_PD_AWAYS_ON

static long rve_ioctl_cmd_start(unsigned long arg)
{
	int rve_user_ctx_id;
	int ret = 0;

	rve_user_ctx_id = rve_internal_ctx_alloc_to_get_idr_id();

	if (copy_to_user((void *)arg, &rve_user_ctx_id, sizeof(int)))
		ret = -EFAULT;

	return ret;
}

static long rve_ioctl_cmd_config(unsigned long arg)
{
	struct rve_user_ctx_t user_ctx;
	int ret = 0;

	if (unlikely(copy_from_user(&user_ctx, (struct rve_user_ctx_t *)arg,
			sizeof(user_ctx)))) {
		pr_err("rve_user_ctx copy_from_user failed!\n");
		return -EFAULT;
	}

/* TODO:
 *	if (rve_user_ctx.cmd_num > RVE_CMD_NUM_MAX) {
 *		pr_err("Cannot import more than %d buffers at a time!\n",
 *			RVE_CMD_NUM_MAX);
 *		return -EFBIG;
 *	}
 */

	if (user_ctx.id <= 0) {
		pr_err("ctx id[%d] is invalid", user_ctx.id);
		return -EINVAL;
	}

	if (DEBUGGER_EN(MSG))
		pr_info("config cmd id = %d", user_ctx.id);

	/* find internal_ctx to set cmd by user ctx (internal ctx id) */
	ret = rve_job_config_by_user_ctx(&user_ctx);
	if (ret < 0) {
		pr_err("config ctx id[%d] failed!\n", user_ctx.id);
		return -EFAULT;
	}

	return ret;
}

static long rve_ioctl_cmd_end(unsigned long arg)
{
	struct rve_user_ctx_t rve_user_ctx;
	int ret = 0;

	if (unlikely(copy_from_user(&rve_user_ctx, (uint32_t *)arg,
			sizeof(rve_user_ctx)))) {
		pr_err("rve_user_ctx copy_from_user failed!\n");
		return -EFAULT;
	}

	if (DEBUGGER_EN(MSG))
		pr_info("config end id = %d", rve_user_ctx.id);

	/* find internal_ctx to set cmd by user ctx (internal ctx id) */
	ret = rve_job_commit_by_user_ctx(&rve_user_ctx);
	if (ret < 0) {
		pr_err("commit ctx id[%d] failed!\n", rve_user_ctx.id);
		return -EFAULT;
	}

	if (copy_to_user((struct rve_user_ctx_t *)arg,
			&rve_user_ctx, sizeof(struct rve_user_ctx_t))) {
		pr_err("rve_user_ctx copy_to_user failed\n");
		return -EFAULT;
	}

	return ret;
}

static long rve_ioctl_cmd_cancel(unsigned long arg)
{
	uint32_t rve_user_ctx_id;
	int ret = 0;

	if (unlikely(copy_from_user(&rve_user_ctx_id, (uint32_t *)arg,
			sizeof(uint32_t)))) {
		pr_err("rve_user_ctx copy_from_user failed!\n");
		return -EFAULT;
	}

	if (DEBUGGER_EN(MSG))
		pr_info("config cancel id = %d", rve_user_ctx_id);

	/* find internal_ctx to set cmd by user ctx (internal ctx id) */
	ret = rve_job_cancel_by_user_ctx(rve_user_ctx_id);
	if (ret < 0) {
		pr_err("cancel ctx id[%d] failed!\n", rve_user_ctx_id);
		return -EFAULT;
	}

	return ret;
}

static long rve_ioctl(struct file *file, uint32_t cmd, unsigned long arg)
{
	struct rve_drvdata_t *rve = rve_drvdata;

	int ret = 0;
	int i = 0;
	struct rve_version_t driver_version;
	struct rve_hw_versions_t hw_versions;

	if (!rve) {
		pr_err("rve_drvdata is null, rve is not init\n");
		return -ENODEV;
	}

	//if (DEBUGGER_EN(NONUSE))
	//	return 0;

	switch (cmd) {
	case RVE_IOC_GET_HW_VER:
		/* RVE hardware version */
		hw_versions.size = rve->num_of_scheduler > RVE_HW_SIZE ?
			RVE_HW_SIZE : rve->num_of_scheduler;

		for (i = 0; i < hw_versions.size; i++) {
			memcpy(&hw_versions.version[i], &rve->scheduler[i]->version,
				sizeof(rve->scheduler[i]->version));
		}

		if (copy_to_user((void *)arg, &hw_versions, sizeof(hw_versions)))
			ret = -EFAULT;
		else
			ret = true;

		break;

	case RVE_IOC_GET_VER:
		/* Driver version */
		driver_version.major = DRIVER_MAJOR_VERSION;
		driver_version.minor = DRIVER_MINOR_VERSION;
		driver_version.revision = DRIVER_REVISION_VERSION;
		driver_version.prod_num = 0;
		strncpy((char *)driver_version.str, DRIVER_VERSION, sizeof(driver_version.str));

		if (copy_to_user((void *)arg, &driver_version, sizeof(driver_version)))
			ret = -EFAULT;
		else
			ret = true;

		break;

	case RVE_IOC_START_CONFIG:
		ret = rve_ioctl_cmd_start(arg);

		break;

	case RVE_IOC_END_CONFIG:
		ret = rve_ioctl_cmd_end(arg);

		break;

	case RVE_IOC_CMD_CONFIG:
		ret = rve_ioctl_cmd_config(arg);

		break;

	case RVE_IOC_CANCEL_CONFIG:
		ret = rve_ioctl_cmd_cancel(arg);

		break;

	default:
		pr_err("unknown ioctl cmd!\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef CONFIG_ROCKCHIP_RVE_DEBUGGER
static int rve_debugger_init(struct rve_debugger **debugger_p)
{
	struct rve_debugger *debugger;

	*debugger_p = kzalloc(sizeof(struct rve_debugger), GFP_KERNEL);
	if (*debugger_p == NULL) {
		pr_err("can not alloc for rve debugger\n");
		return -ENOMEM;
	}

	debugger = *debugger_p;

#ifdef CONFIG_ROCKCHIP_RVE_DEBUG_FS
	mutex_init(&debugger->debugfs_lock);
	INIT_LIST_HEAD(&debugger->debugfs_entry_list);
#endif

#ifdef CONFIG_ROCKCHIP_RVE_PROC_FS
	mutex_init(&debugger->procfs_lock);
	INIT_LIST_HEAD(&debugger->procfs_entry_list);
#endif

	rve_debugfs_init();
	rve_procfs_init();

	return 0;
}

static int rve_debugger_remove(struct rve_debugger **debugger_p)
{
	rve_debugfs_remove();
	rve_procfs_remove();

	kfree(*debugger_p);
	*debugger_p = NULL;

	return 0;
}
#endif

static int rve_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static int rve_release(struct inode *inode, struct file *file)
{
	pid_t pid;
	int ctx_id;
	struct rve_pending_ctx_manager *ctx_manager;
	struct rve_internal_ctx_t *ctx;

	pid = current->pid;

	ctx_manager = rve_drvdata->pend_ctx_manager;

	mutex_lock(&ctx_manager->lock);

	idr_for_each_entry(&ctx_manager->ctx_id_idr, ctx, ctx_id) {

		mutex_unlock(&ctx_manager->lock);

		if (pid == ctx->debug_info.pid) {
			pr_err("[pid:%d] destroy ctx[%d] when the user exits", pid, ctx->id);
			kref_put(&ctx->refcount, rve_internal_ctx_kref_release);
		}

		mutex_lock(&ctx_manager->lock);
	}

	mutex_unlock(&ctx_manager->lock);

	return 0;
}

static irqreturn_t rve_irq_handler(int irq, void *data)
{
	struct rve_scheduler_t *scheduler = data;
	u32 error_flag;

	error_flag = rve_read(RVE_SWREG6_IVE_WORK_STA, scheduler);

	if (error_flag & 0x6) {
		pr_err("irq thread work_status[%x]\n", error_flag);

		if (error_flag & 0x2)
			pr_err("irq: bus error");
		else if (error_flag & 0x4)
			pr_err("irq: timeout error");

		scheduler->ops->soft_reset(scheduler);
	}

	/* clear INT */
	rve_write(0x30000, RVE_SWREG1_IVE_IRQ, scheduler);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t rve_irq_thread(int irq, void *data)
{
	struct rve_scheduler_t *scheduler = data;
	struct rve_job *job;
	u32 error_flag;

	job = scheduler->running_job;
	scheduler->total_int_cnt++;

	if (!job) {
		pr_err("running job is invalid on irq thread\n");
		return IRQ_HANDLED;
	}

	if (DEBUGGER_EN(INT_FLAG)) {
		error_flag = rve_read(RVE_SWREG6_IVE_WORK_STA, scheduler);

		if (error_flag & 0x1) {
			pr_err("irq thread work_status[%x]\n", error_flag);

			if (error_flag & 0x2)
				pr_err("irq: bus error");
			else if (error_flag & 0x4)
				pr_err("irq: timeout error");
		}
	}

	rve_job_done(scheduler, 0);

	return IRQ_HANDLED;
}

const struct file_operations rve_fops = {
	.owner = THIS_MODULE,
	.open = rve_open,
	.release = rve_release,
	.unlocked_ioctl = rve_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = rve_ioctl,
#endif
};

static struct miscdevice rve_dev = {
	.name = "rve",
	.fops = &rve_fops,
};

static const char *const rve_clks[] = {
	"aclk_rve",
	"hclk_rve",
};

static const struct rve_irqs_data_t rve_irqs[] = {
	{"rve_irq", rve_irq_handler, rve_irq_thread}
};

static const struct rve_match_data_t rve_match_data = {
	.clks = rve_clks,
	.num_clks = ARRAY_SIZE(rve_clks),
	.irqs = rve_irqs,
	.num_irqs = ARRAY_SIZE(rve_irqs)
};

static const struct of_device_id rve_dt_ids[] = {
	{
	 .compatible = "rockchip,rve",
	 .data = &rve_match_data,
	},
	{},
};

static void init_scheduler(struct rve_scheduler_t *scheduler,
			 const char *name)
{
	spin_lock_init(&scheduler->irq_lock);
	INIT_LIST_HEAD(&scheduler->todo_list);
	init_waitqueue_head(&scheduler->job_done_wq);

	if (!strcmp(name, "rve")) {
		scheduler->ops = &rve_ops;
		scheduler->core = RVE_SCHEDULER_CORE0;
	}
}

static int rve_drv_probe(struct platform_device *pdev)
{
	struct rve_drvdata_t *data = rve_drvdata;
	struct resource *res;
	int ret = 0;
	const struct of_device_id *match = NULL;
	struct device *dev = &pdev->dev;
	const struct rve_match_data_t *match_data;
	int i = 0, irq;
	struct rve_scheduler_t *scheduler = NULL;

	if (!pdev->dev.of_node)
		return -EINVAL;

	if (!strcmp(dev_driver_string(dev), "rve"))
		match = of_match_device(rve_dt_ids, dev);

	if (!match) {
		dev_err(dev, "%s missing DT entry!\n", dev_driver_string(dev));
		return -EINVAL;
	}

	scheduler =
		devm_kzalloc(&pdev->dev, sizeof(struct rve_scheduler_t),
			GFP_KERNEL);
	if (scheduler == NULL) {
		pr_err("failed to allocate scheduler. dev name = %s\n",
			dev_driver_string(dev));
		return -ENOMEM;
	}

	init_scheduler(scheduler, dev_driver_string(dev));

	scheduler->dev = &pdev->dev;

	/* map the registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("get memory resource failed.\n");
		return -ENXIO;
	}

	scheduler->rve_base =
		devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!scheduler->rve_base) {
		pr_err("ioremap failed\n");
		ret = -ENOENT;
		return ret;
	}

	/* get the IRQ */
	match_data = match->data;

	/* there are irq names in dts */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "no irq %s in dts\n", match_data->irqs[0].name);
		return irq;
	}

	scheduler->irq = irq;

	pr_info("%s, irq = %d, match scheduler\n",
			match_data->irqs[0].name, irq);

	ret = devm_request_threaded_irq(dev, irq,
			match_data->irqs[0].irq_hdl,
			match_data->irqs[0].irq_thread, IRQF_SHARED,
			dev_driver_string(dev), scheduler);
	if (ret < 0) {
		pr_err("request irq name: %s failed: %d\n",
				match_data->irqs[0].name, ret);
		return ret;
	}

#ifndef RVE_PD_AWAYS_ON
	for (i = 0; i < match_data->num_clks; i++) {
		struct clk *clk = devm_clk_get(dev, match_data->clks[i]);

		if (IS_ERR(clk))
			pr_err("failed to get %s\n", match_data->clks[i]);

		scheduler->clks[i] = clk;
	}
	scheduler->num_clks = match_data->num_clks;
#endif

	platform_set_drvdata(pdev, scheduler);

	device_init_wakeup(dev, true);

	/* PM init */
#ifndef RVE_PD_AWAYS_ON
	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_get_sync(scheduler->dev);
	if (ret < 0) {
		pr_err("failed to get pm runtime, ret = %d\n",
			 ret);
		goto failed;
	}

	for (i = 0; i < scheduler->num_clks; i++) {
		if (!IS_ERR(scheduler->clks[i])) {
			ret = clk_prepare_enable(scheduler->clks[i]);
			if (ret < 0) {
				pr_err("failed to enable clk\n");
				goto failed;
			}
		}
	}
#endif //RVE_PD_AWAYS_ON

	scheduler->ops->get_version(scheduler);
	pr_info("Driver loaded successfully rve[%d] ver:%s\n", i,
		scheduler->version.str);

	data->scheduler[data->num_of_scheduler] = scheduler;

	data->num_of_scheduler++;

#ifndef RVE_PD_AWAYS_ON
	for (i = scheduler->num_clks - 1; i >= 0; i--)
		if (!IS_ERR(scheduler->clks[i]))
			clk_disable_unprepare(scheduler->clks[i]);

	pm_runtime_put_sync(&pdev->dev);
#endif //RVE_PD_AWAYS_ON

	pr_info("probe successfully\n");

	return 0;

#ifndef RVE_PD_AWAYS_ON
failed:
	device_init_wakeup(dev, false);
	pm_runtime_disable(dev);

	return ret;
#endif //RVE_PD_AWAYS_ON
}

static int rve_drv_remove(struct platform_device *pdev)
{
	device_init_wakeup(&pdev->dev, false);
#ifndef RVE_PD_AWAYS_ON
	pm_runtime_disable(&pdev->dev);
#endif //RVE_PD_AWAYS_ON

	return 0;
}

static struct platform_driver rve_driver = {
	.probe = rve_drv_probe,
	.remove = rve_drv_remove,
	.driver = {
		 .name = "rve",
		 .of_match_table = of_match_ptr(rve_dt_ids),
		 },
};

static int __init rve_init(void)
{
	int ret;

	rve_drvdata = kzalloc(sizeof(struct rve_drvdata_t), GFP_KERNEL);
	if (rve_drvdata == NULL) {
		pr_err("failed to allocate driver data.\n");
		return -ENOMEM;
	}

	mutex_init(&rve_drvdata->lock);

	wake_lock_init(&rve_drvdata->wake_lock, WAKE_LOCK_SUSPEND, "rve");

	ret = platform_driver_register(&rve_driver);
	if (ret != 0) {
		pr_err("Platform device rve3_core0_driver register failed (%d).\n", ret);
		return ret;
	}

	rve_init_timer();

#ifdef CONFIG_SYNC_FILE
	rve_drvdata->fence_ctx = rve_fence_context_alloc();
	if (IS_ERR(rve_drvdata->fence_ctx)) {
		pr_err("failed to allocate fence context for RVE\n");
		ret = PTR_ERR(rve_drvdata->fence_ctx);
		return ret;
	}
#endif

	ret = misc_register(&rve_dev);
	if (ret) {
		pr_err("cannot register miscdev (%d)\n", ret);
		return ret;
	}

	rve_ctx_manager_init(&rve_drvdata->pend_ctx_manager);

#ifdef CONFIG_ROCKCHIP_RVE_DEBUGGER
	rve_debugger_init(&rve_drvdata->debugger);
#endif

	pr_info("Module initialized. v%s\n", DRIVER_VERSION);

	return 0;
}

static void __exit rve_exit(void)
{
#ifdef CONFIG_ROCKCHIP_RVE_DEBUGGER
	rve_debugger_remove(&rve_drvdata->debugger);
#endif

	rve_ctx_manager_remove(&rve_drvdata->pend_ctx_manager);

	wake_lock_destroy(&rve_drvdata->wake_lock);

#ifdef CONFIG_SYNC_FILE
	rve_fence_context_free(rve_drvdata->fence_ctx);
#endif

	rve_cancel_timer();

	platform_driver_unregister(&rve_driver);

	misc_deregister(&(rve_drvdata->miscdev));

	kfree(rve_drvdata);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
#ifdef CONFIG_ROCKCHIP_THUNDER_BOOT
module_init(rve_init);
#else
late_initcall(rve_init);
#endif
#else
fs_initcall(rve_init);
#endif
module_exit(rve_exit);

/* Module information */
MODULE_AUTHOR("putin.li@rock-chips.com");
MODULE_DESCRIPTION("Driver for rve device");
MODULE_LICENSE("GPL");
