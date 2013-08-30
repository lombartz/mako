/* Copyright (c) 2010-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Modified by Paul Reioux (Faux123)
 * 2013-06-20: Added KGSL Simple GPU Governor
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>
#include <linux/module.h>
#include <linux/hotplug.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

#define TZ_GOVERNOR_PERFORMANCE 0
#define TZ_GOVERNOR_ONDEMAND    1
#define TZ_GOVERNOR_SIMPLE	2

struct tz_priv {
	int governor;
	unsigned int no_switch_cnt;
	unsigned int skip_cnt;
	struct kgsl_power_stats bin;
};
spinlock_t tz_lock;

/* FLOOR is 5msec to capture up to 3 re-draws
 * per frame for 60fps content.
 */
#define FLOOR			5000
/* CEILING is 50msec, larger than any standard
 * frame length, but less than the idle timer.
 */
#define CEILING			50000

#define TZ_RESET_ID		0x3
#define TZ_UPDATE_ID		0x4

#if 0
#ifdef CONFIG_MSM_SCM
/* Trap into the TrustZone, and call funcs there. */
static int __secure_tz_entry(u32 cmd, u32 val, u32 id)
{
	int ret;
	spin_lock(&tz_lock);
	__iowmb();
	ret = scm_call_atomic2(SCM_SVC_IO, cmd, val, id);
	spin_unlock(&tz_lock);
	return ret;
}
#else
static int __secure_tz_entry(u32 cmd, u32 val, u32 id)
{
	return 0;
}
#endif /* CONFIG_MSM_SCM */
#endif

static ssize_t tz_governor_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	struct tz_priv *priv = pwrscale->priv;
	int ret;

	if (priv->governor == TZ_GOVERNOR_ONDEMAND)
		ret = snprintf(buf, 10, "ondemand\n");
    	else if (priv->governor == TZ_GOVERNOR_SIMPLE)
		ret = snprintf(buf, 8, "simple\n");
	else
		ret = snprintf(buf, 13, "performance\n");

	return ret;
}

static ssize_t tz_governor_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				 const char *buf, size_t count)
{
	char str[20];
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	int ret;

	ret = sscanf(buf, "%20s", str);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&device->mutex);

	if (!strncmp(str, "ondemand", 8))
		priv->governor = TZ_GOVERNOR_SIMPLE;
    	else if (!strncmp(str, "simple", 6))
		priv->governor = TZ_GOVERNOR_SIMPLE;
	else if (!strncmp(str, "performance", 11))
		priv->governor = TZ_GOVERNOR_SIMPLE;

	if (priv->governor == TZ_GOVERNOR_PERFORMANCE)
		kgsl_pwrctrl_pwrlevel_change(device, pwr->max_pwrlevel);

	mutex_unlock(&device->mutex);
	return count;
}

PWRSCALE_POLICY_ATTR(governor, 0644, tz_governor_show, tz_governor_store);

static struct attribute *tz_attrs[] = {
	&policy_attr_governor.attr,
	NULL
};

static struct attribute_group tz_attr_group = {
	.attrs = tz_attrs,
};

static void tz_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv = pwrscale->priv;
	if (device->state != KGSL_STATE_NAP &&
		priv->governor == TZ_GOVERNOR_ONDEMAND)
		kgsl_pwrctrl_pwrlevel_change(device,
					device->pwrctrl.default_pwrlevel);
}

#define HISTORY_SIZE 10

static int ramp_up_threshold = 7000;
module_param_named(simple_ramp_threshold, ramp_up_threshold, int, 0664);

static unsigned int history[HISTORY_SIZE] = {0};
static unsigned short load_counter = 0;
static unsigned int full_load = 0;
/* extern var */
bool gpu_idle;
unsigned short idle_counter;

static int simple_governor(struct kgsl_device *device, int idle_stat)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	unsigned int total = 0;
	
	full_load -= history[load_counter];
	history[load_counter] = idle_stat;
	
	full_load += idle_stat;

	if (unlikely(++load_counter >= HISTORY_SIZE))
		load_counter = 0;

	total = full_load / HISTORY_SIZE;
	
	/*pr_info("Num Pwrlevels:\t%d",pwr->num_pwrlevels);
	pr_info("Active Pwrlevel:\t%d",pwr->active_pwrlevel);
	pr_info("Current Load:\t%d",total);
	pr_info("---------------------------------");
	if(gpu_idle){pr_info("GPU IDLE");}
	else{pr_info("GPU BUSY");}
	pr_info("---------------------------------");*/
		
	if (pwr->active_pwrlevel == 3 && total > 15000)
	{
		if (idle_counter < 10)
			idle_counter += 1;
				
		if (idle_counter >= 10)			
			gpu_idle = true;
			
		return 1;
	}
	else
	{
		if (idle_counter > 0)
			idle_counter -= 1;
			
		if (idle_counter <= 0)
		{
			if (gpu_idle && is_touching)
				touchboost_func();
			gpu_idle = false;
		}
	}
    
	/* it's currently busy */
	if (total < ramp_up_threshold)
	{
		if ((pwr->active_pwrlevel > 0) &&
			(pwr->active_pwrlevel <= (pwr->num_pwrlevels - 1)))
        /* bump up to next pwrlevel */
			return -1;
	}
	/* idle case */
	else
	{	
		if ((pwr->active_pwrlevel >= 0) &&
			(pwr->active_pwrlevel < (pwr->num_pwrlevels - 1)))
			return 1;
	}
    
	return 0;
}

static void __cpuinit tz_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct tz_priv *priv = pwrscale->priv;
	struct kgsl_power_stats stats;
	int val, idle;

	/* In "performance" mode the clock speed always stays
	   the same */
	if (priv->governor == TZ_GOVERNOR_PERFORMANCE)
		return;

	device->ftbl->power_stats(device, &stats);
	priv->bin.total_time += stats.total_time;
	priv->bin.busy_time += stats.busy_time;
	/* Do not waste CPU cycles running this algorithm if
	 * the GPU just started, or if less than FLOOR time
	 * has passed since the last run.
	 */
	if ((stats.total_time == 0) ||
			(priv->bin.total_time < FLOOR))
		return;

	/* If there is an extended block of busy processing,
	 * increase frequency.  Otherwise run the normal algorithm.
	 */
	if (priv->bin.busy_time > CEILING) {
		val = -1;
	} else {
		idle = priv->bin.total_time - priv->bin.busy_time;
		idle = (idle > 0) ? idle : 0;
		val = simple_governor(device, idle);
	}
	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;
	if (val)
		kgsl_pwrctrl_pwrlevel_change(device,
					     pwr->active_pwrlevel + val);
}
static void tz_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void tz_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv = pwrscale->priv;

    /*
     * Why in the hell were we calling a secure_tz func sleeping the device
     * at 320MHz on the GPU? Makes no sense to me. Lets change the pwrlevel
     * directly and sleep at its lowest frequency 128MHz.
     */
    gpu_idle = true;
    idle_counter = 10;
	kgsl_pwrctrl_pwrlevel_change(device, 3);
	priv->no_switch_cnt = 0;
	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;
}

#ifdef CONFIG_MSM_SCM
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct tz_priv *priv;

	priv = pwrscale->priv = kzalloc(sizeof(struct tz_priv), GFP_KERNEL);
	if (pwrscale->priv == NULL)
		return -ENOMEM;

	priv->governor = TZ_GOVERNOR_SIMPLE;
	spin_lock_init(&tz_lock);
	kgsl_pwrscale_policy_add_files(device, pwrscale, &tz_attr_group);

	return 0;
}
#else
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return -EINVAL;
}
#endif /* CONFIG_MSM_SCM */

static void tz_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_remove_files(device, pwrscale, &tz_attr_group);
	kfree(pwrscale->priv);
	pwrscale->priv = NULL;
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_tz = {
	.name = "trustzone",
	.init = tz_init,
	.busy = tz_busy,
	.idle = tz_idle,
	.sleep = tz_sleep,
	.wake = tz_wake,
	.close = tz_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_tz);
