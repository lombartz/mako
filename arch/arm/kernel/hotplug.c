/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Simple hot[un]plug driver for SMP
 *
 * rewritten by Patrick Dittrich <patrick90vhm@gmail.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <mach/cpufreq.h>
#include <linux/slab.h>

#define HOTPLUG "hotplug"

//#define DEBUG

struct cpu_load_data {
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static u64 now;

static struct workqueue_struct *wq;
static struct workqueue_struct *pm_wq;
static struct delayed_work decide_hotplug;
static struct work_struct resume;
static struct work_struct suspend;

static unsigned int up_counter = 0;
static unsigned int down_counter = 0;

#define CPU_CORES 4

static struct hotplug_values {
	unsigned int up_threshold[CPU_CORES];
	unsigned int down_threshold[CPU_CORES];
	unsigned int max_up_counter[CPU_CORES];
	unsigned int max_down_counter[CPU_CORES];
	unsigned int sample_time_ms;
	spinlock_t up_threshold_lock, down_threshold_lock,
		max_up_counter_lock, max_down_counter_lock;
} 
boost_values = {
	.up_threshold = {55, 60, 65, 100},
	.down_threshold = {0, 20, 30, 40},
	.max_up_counter = {4, 6, 6, 0},
	.max_down_counter = {0, 150, 50, 50},
	.sample_time_ms = 20
}, busy_values = {
	.up_threshold = {60, 60, 65, 100},
	.down_threshold = {0, 30, 30, 40},
	.max_up_counter = {4, 6, 6, 0},
	.max_down_counter = {0, 100, 10, 10},
	.sample_time_ms = 30
}, idle_values = {
	.up_threshold = {80, 85, 90, 100},
	.down_threshold = {0, 40, 50, 60},
	.max_up_counter = {6, 10, 10, 0},
	.max_down_counter = {0, 10, 6, 6},
	.sample_time_ms = 50
};

unsigned int get_cur_max(unsigned int cpu);

static inline int get_cpu_load(unsigned int cpu)
{
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	struct cpufreq_policy policy;
	u64 cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;
	unsigned int cur_load;
	unsigned int cur_max, max_freq, cur_freq;

	cpufreq_get_policy(&policy, cpu);
	
	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time,
						gpu_idle ? 0 : 1);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	/* get the correct max frequency and current freqency */
	cur_max = get_cur_max(policy.cpu);

	if (cur_max >= policy.max)
	{
		max_freq = policy.max;
		cur_freq = policy.cur;
	}
	else
	{
		max_freq = cur_max;
		cur_freq = policy.cur > cur_max ? cur_max : policy.cur;
	}

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	return (cur_load * cur_freq) / max_freq;
}

static void __ref online_core(void)
{
	unsigned int cpu;
		
	for_each_possible_cpu(cpu) 
	{
		if (!cpu_online(cpu)) 
		{
			cpu_up(cpu);
			break;
		}
	}
	
	up_counter = 0;
	down_counter = 0;
	
	return;
}

static void __ref offline_core(void)
{   
	unsigned int cpu;
	
	for (cpu = 3; cpu; cpu--)
	{
		if (cpu_online(cpu)) 
		{
			cpu_down(cpu);
			break;
		}
	}
	
	up_counter = 0;
	down_counter = 0;
	
	return;
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
	unsigned int cpu, load, av_load = 0;
	unsigned short online_cpus;
	struct hotplug_values *values;

#ifdef DEBUG
	short load_array[4] = {};
	int cpu_debug = 0;
	struct cpufreq_policy policy;
#endif

	now = ktime_to_ms(ktime_get());
	online_cpus = num_online_cpus() - 1;

	if (gpu_idle)
		values = &idle_values;
	else if (boostpulse_endtime > now)
		values = &boost_values;
	else
		values = &busy_values;

	for_each_online_cpu(cpu) 
	{
		load = get_cpu_load(cpu);
		av_load += load;
		
#ifdef DEBUG
		load_array[cpu] = load;
#endif		
	}

	av_load /= (online_cpus + 1);

	if (av_load >= values->up_threshold[online_cpus])
	{
		if (up_counter < values->max_up_counter[online_cpus])
			up_counter++;
		
		if (down_counter > 0)
			down_counter--;
			
		if (up_counter >= values->max_up_counter[online_cpus]
				&& online_cpus + 1 < CPU_CORES)
			online_core();
	}
	else if (av_load <= values->down_threshold[online_cpus])
	{
		if (down_counter < values->max_down_counter[online_cpus])
			down_counter++;
		
		if (up_counter > 0)
			up_counter--;
			
		if (down_counter >= values->max_down_counter[online_cpus]
				&& online_cpus > 0)
			offline_core();	
	}
	else
	{
		if (up_counter > 0)
			up_counter--;
		
		if (down_counter > 0)
			down_counter--; 
	}

#ifdef DEBUG
	cpu = 0;
	pr_info("------HOTPLUG DEBUG INFO------\n");
	pr_info("Cores on:\t%d", online_cpus + 1);
	pr_info("Core0:\t\t%d", load_array[0]);
	pr_info("Core1:\t\t%d", load_array[1]);
	pr_info("Core2:\t\t%d", load_array[2]);
	pr_info("Core3:\t\t%d", load_array[3]);
	pr_info("Av Load:\t\t%d", av_load);
	pr_info("-------------------------------");
	pr_info("Up count:\t%d\n",up_counter);
	pr_info("Dw count:\t%d\n",down_counter);

	if (gpu_idle)
		pr_info("Gpu Idle:\ttrue");
	else
		pr_info("Gpu Idle:\tfalse");
	if (boostpulse_endtime > now)
		pr_info("Touch:\t\ttrue");
	else
		pr_info("Touch:\t\tfalse");
	
	for_each_possible_cpu(cpu_debug)
	{
		if (cpu_online(cpu_debug))
		{
			cpufreq_get_policy(&policy, cpu_debug);
			pr_info("cpu%d:\t\t%d MHz",
					cpu_debug,policy.cur/1000);
		}
		else
			pr_info("cpu%d:\t\toff",cpu_debug);
	}
	pr_info("up_threshold:\t%d", 
		values->max_up_counter[online_cpus]);
	pr_info("down_threshold:\t%d", 
		values->max_down_counter[online_cpus]);
	pr_info("-----------------------------------------");
	kfree(load_array);
#endif

	queue_delayed_work(wq, &decide_hotplug, 
			msecs_to_jiffies(values->sample_time_ms));
}

static void suspend_func(struct work_struct *work)
{
	int cpu;

	/* cancel the hotplug work when the screen is off and flush the WQ */
	flush_workqueue(wq);
	cancel_delayed_work_sync(&decide_hotplug);

	pr_info("Early Suspend stopping Hotplug work...\n");
	
	for_each_possible_cpu(cpu) 
	{
		if (cpu)
			cpu_down(cpu);
		
	}

	up_counter = 0;
	down_counter = 0;
}

static void __ref resume_func(struct work_struct *work)
{
	int cpu, onlined = 0;
	u64 now = ktime_to_ms(ktime_get());

	idle_counter = 0;
	gpu_idle = false;

	boostpulse_endtime = now + boostpulse_duration_val;

	for_each_possible_cpu(cpu) 
	{
		if (cpu) 
		{
			cpu_up(cpu);
			if (++onlined == 2)
				break;
		}
	}
	
	pr_info("Late Resume starting Hotplug work...\n");
	queue_delayed_work(wq, &decide_hotplug, HZ);	
}

static void hotplug_early_suspend(struct early_suspend *handler)
{	 
	queue_work_on(0, pm_wq, &suspend);
}

static void hotplug_early_resume(struct early_suspend *handler)
{  
	queue_work_on(0, pm_wq, &resume);
}

static struct early_suspend hotplug_suspend =
{
	.suspend = hotplug_early_suspend,
	.resume = hotplug_early_resume,
};

/*
 * Sysfs get/set entries start
 */

static ssize_t up_threshold_show(struct hotplug_values *values, 
	char *buf)
{
	int i;
	ssize_t ret = 0;

	for (i = 0; i < CPU_CORES; i++)
		ret += sprintf(buf + ret, "%u%s", 
				values->up_threshold[i], 
				i + 1 < CPU_CORES ? " " : "");

	ret += sprintf(buf + ret, "\n");
	return ret;
}

static ssize_t boost_up_threshold_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	return up_threshold_show(&boost_values, buf);
}

static ssize_t busy_up_threshold_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	return up_threshold_show(&busy_values, buf);
}

static ssize_t idle_up_threshold_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	return up_threshold_show(&idle_values, buf);
}

static void array_store(const char *buf, unsigned int *values)
{
	unsigned int new_values[CPU_CORES];
	unsigned int i;
	const char *cp;
	bool inval = true;
	char valid[] = "0123456789 ";
	char *val = valid;
	char number[4];
	long temp;

	cp = buf;
	i = 0;

	pr_info("Let's go!");
	while (*cp != '\0')
	{
		inval = true;
		val = valid;

		pr_info("Checking now: %c",*cp);
		while (*val != '\0')
		{
			if (*cp == *(val++))
			{
				inval = false;
				break;
			}
		}

		if (inval)
			goto err_inval;
		pr_info("Checking now: valid");
		
		if(*cp == ' ')
		{
			pr_info("' ' detected");
			if(i >= CPU_CORES)
				goto err_inval;
			
			if (kstrtol(number, 10, &temp) != 0)
				goto err_inval;

			new_values[i] = (unsigned int) temp;
			pr_info("new_values[%d] = %ld",i,temp);
			i++;
		}
		else
			strncat(number, cp, 1);
			
		cp++;
	}

	memcpy(values, new_values, sizeof(new_values));

err_inval:
	kfree(new_values);
	return;
}

static ssize_t boost_up_threshold_store(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	pr_info("%s, %d",buf,*boost_values.up_threshold);
	array_store(buf, boost_values.up_threshold);
	return size;
}

static ssize_t busy_up_threshold_store(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	array_store(buf, busy_values.up_threshold);
	return size;
}

static ssize_t idle_up_threshold_store(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	array_store(buf, idle_values.up_threshold);
	return size;
}

static DEVICE_ATTR(boost_up_threshold, 0664, boost_up_threshold_show, 
						boost_up_threshold_store);
static DEVICE_ATTR(busy_up_threshold, 0664, busy_up_threshold_show, 
						busy_up_threshold_store);
static DEVICE_ATTR(idle_up_threshold, 0664, idle_up_threshold_show, 
						idle_up_threshold_store);

static struct attribute *hotplug_control_attributes[] =
{
	&dev_attr_boost_up_threshold.attr,
	&dev_attr_busy_up_threshold.attr,
	&dev_attr_idle_up_threshold.attr,
	NULL
};

static struct attribute_group hotplug_control_group =
{
	.attrs  = hotplug_control_attributes,
};

static struct miscdevice hotplug_control_device =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hotplug_control",
};

/*
 * Sysfs get/set entries end
 */

static int __devinit hotplug_probe(struct platform_device *pdev)
{
	int ret = 0;
	pr_info("Hotplug driver started.\n");

	wq = alloc_workqueue("hotplug_workqueue", WQ_HIGHPRI | WQ_FREEZABLE, 1);
	
	if (!wq)
	{
		ret = -ENOMEM;
		goto err;
	}

	ret = misc_register(&hotplug_control_device);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	ret = sysfs_create_group(&hotplug_control_device.this_device->kobj,
			&hotplug_control_group);

	pm_wq = alloc_workqueue("pm_workqueue", 0, 1);
	
	if (!pm_wq)
		ret = -ENOMEM;

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	INIT_WORK(&resume, resume_func);
	INIT_WORK(&suspend, suspend_func);
	queue_delayed_work(wq, &decide_hotplug, HZ*20);
	
	register_early_suspend(&hotplug_suspend);

err:
	return ret;
}

static struct platform_device hotplug_device = {
	.name = HOTPLUG,
	.id = -1,
};

static int hotplug_remove(struct platform_device *pdev)
{
	destroy_workqueue(wq);
	destroy_workqueue(pm_wq);

	return 0;
}

static struct platform_driver hotplug_driver = {
	.probe = hotplug_probe,
	.remove = hotplug_remove,
	.driver = {
		.name = HOTPLUG,
		.owner = THIS_MODULE,
	},
};

static int __init hotplug_init(void)
{
	int ret;

	ret = platform_driver_register(&hotplug_driver);

	if (ret)
	{
		return ret;
	}

	ret = platform_device_register(&hotplug_device);

	if (ret)
	{
		return ret;
	}

	pr_info("%s: init\n", HOTPLUG);

	return ret;
}

static void __exit hotplug_exit(void)
{
	platform_device_unregister(&hotplug_device);
	platform_driver_unregister(&hotplug_driver);
}

late_initcall(hotplug_init);
module_exit(hotplug_exit);

