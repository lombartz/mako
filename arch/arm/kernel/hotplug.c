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
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <linux/input.h>
#include <linux/jiffies.h>

#include <linux/earlysuspend.h>

#define HOTPLUG "hotplug"

//#define DEBUG

struct cpu_load_data {
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static u64 now;

static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;
static struct work_struct suspend;
static struct work_struct resume;

static unsigned int up_counter = 0;
static unsigned int down_counter = 0;

#define CPU_CORES 4

static struct hotplug_values {
	unsigned int up_threshold[CPU_CORES];
	unsigned int down_threshold[CPU_CORES];
	unsigned int max_up_counter[CPU_CORES];
	unsigned int max_down_counter[CPU_CORES];
	unsigned int sample_time_ms;
} 
boost_values = {
	.up_threshold = {55, 60, 65, 100},
	.down_threshold = {0, 20, 30, 40},
	.max_up_counter = {4, 6, 6, 0},
	.max_down_counter = {0, 150, 50, 40},
	.sample_time_ms = 20
}, busy_values = {
	.up_threshold = {60, 60, 65, 100},
	.down_threshold = {0, 30, 30, 40},
	.max_up_counter = {4, 5, 6, 0},
	.max_down_counter = {0, 100, 26, 18},
	.sample_time_ms = 30
}, idle_values = {
	.up_threshold = {80, 85, 90, 100},
	.down_threshold = {0, 40, 50, 60},
	.max_up_counter = {6, 10, 10, 0},
	.max_down_counter = {0, 30, 10, 6},
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
	pr_info("Core0:\t%d", load_array[0]);
	pr_info("Core1:\t%d", load_array[1]);
	pr_info("Core2:\t%d", load_array[2]);
	pr_info("Core3:\t%d", load_array[3]);
	pr_info("Av Load:\t%d", av_load);
	pr_info("-------------------------------");
	pr_info("Up count:\t%u -> %u\n",up_counter,
			values->max_up_counter[online_cpus]);
	pr_info("Dw count:\t%u -> %u\n",down_counter,
			values->max_down_counter[online_cpus]);

	pr_info("Gpu Idle:\t%s",(gpu_idle ? "true" : "false"));

	pr_info("Touch:\t%s",(boostpulse_endtime > now ?
						 "true" : "false"));
	
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
	pr_info("-----------------------------------------");
#endif

	queue_delayed_work(wq, &decide_hotplug, 
			msecs_to_jiffies(values->sample_time_ms));
}

static void hotplug_suspend(struct work_struct *work)
{
	int cpu;

	pr_info("Early Suspend stopping Hotplug work...\n");
	
	for_each_possible_cpu(cpu){
		if (cpu)
			cpu_down(cpu);
	}

	up_counter = 0;
	down_counter = 0;
}

static void __ref hotplug_resume(struct work_struct *work)
{
	int cpu;
	u64 now = ktime_to_ms(ktime_get());

	idle_counter = 0;
	gpu_idle = false;

	boostpulse_endtime = now + boostpulse_duration_val;

	for_each_possible_cpu(cpu){
		if (cpu){
			cpu_up(cpu);
			break;
		}
	}
	pr_info("Late Resume starting Hotplug work...\n");
}

static void hotplug_early_suspend(struct early_suspend *handler)
{   
	schedule_work(&suspend);
}

static void hotplug_late_resume(struct early_suspend *handler)
{   
	schedule_work(&resume);
}

static struct early_suspend early_suspend =
{
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = hotplug_early_suspend,
	.resume = hotplug_late_resume,
};

/*
 * Sysfs get/set entries start
 */

unsigned int *find_value(unsigned int value, unsigned int type)
{
	struct hotplug_values *values;
	unsigned int *ret;

	switch (type){
		case 0: values = &boost_values; break;
		case 1: values = &busy_values; break;
		case 2: values = &idle_values; break;
		default: return NULL;
	}

	switch (value){
		case 0: ret = values->up_threshold; break;
		case 1: ret = values->down_threshold; break;
		case 2: ret = values->max_up_counter; break;
		case 3: ret = values->max_down_counter; break;
		case 4: ret = &(values->sample_time_ms); break;
		default: return NULL;
	}
	return ret;
}

ssize_t show_array(unsigned int value, unsigned int type,
		char *buf)
{
	unsigned int *array;
	unsigned int i;
	ssize_t ret = 0;

	array = find_value(value, type);

	for (i = 0; i < CPU_CORES; i++)
		ret += sprintf(buf + ret, "%u%s", 
				array[i], 
				i + 1 < CPU_CORES ? " " : "");

	ret += sprintf(buf + ret, "\n");
	return ret;
}

ssize_t show_value(unsigned int value, unsigned int type,
						char *buf)
{
	return sprintf(buf, "%u\n", *find_value(value, type));
}

static bool process_array(const char *buf, unsigned int *values)
{
	unsigned int new_values[CPU_CORES];
	unsigned int space_count, num_len;
	long temp;
	const char *cp;
	bool inval = true;
	char valid[] = "0123456789 ";
	char *space = " ";
	char *val = valid;
	char number[3] = "";

	cp = buf;
	space_count = 0;
	num_len = 0;

	do {
		inval = true;
		val = valid;

		if (*cp != '\0'){
			while (*val != '\0'){
				if (*cp == *(val++)){
					inval = false;
					break;
				}
			}
			if (inval)
				goto err_inval;
		}
		
		if (*cp == *space || *cp == '\0'){
			if (space_count >= CPU_CORES)
				goto err_inval;
			
			if (kstrtol(number, 10, &temp) < 0)
				goto err_inval;

			new_values[space_count] = 
				(unsigned int) temp;
			
			space_count++;
			num_len = 0;
			strcpy(number, "");
		} else if (++num_len <= 3)
			strncat(number, cp, 1);
		else
			goto err_inval;
	
		cp++;

	} while (*(cp - 1) != '\0');

	if (space_count < CPU_CORES)
		goto err_inval;

	memcpy(values, new_values, sizeof(new_values));

	pr_info("Values changed.\n");
	return true;
err_inval:
	pr_info("Values invalid.\n");
	return false;
}

bool store_array(unsigned int value, unsigned int type,
					const char *buf)
{
	pr_info("Store hotplug values:\n");
	pr_info("value: %u, type: %u, string: '%s'\n",
		value, type, buf);

	return process_array(buf, find_value(value, type));
}

bool store_value(unsigned int value, unsigned int type,
					const char *buf)
{
	unsigned long val;
	unsigned int *data; 
	data = find_value(value, type);

	pr_info("Store hotplug value:\n");
	pr_info("value: %u, type: %u, string: '%s'\n",
		value, type, buf);

	if (kstrtoul(buf, 0, &val) < 0){
		pr_info("Value invalid.\n");
		return false;
	}

	if (value == 4 && val < 10)
		val = 10;

	*data = (unsigned int) val;
	pr_info("Value changed.\n");

	return true;
}

/*
 * Sysfs get/set entries end
 */

static int __devinit hotplug_probe(struct platform_device *pdev)
{
	int ret = 0;

	wq = alloc_workqueue("hotplug_workqueue", WQ_HIGHPRI 
					| WQ_FREEZABLE, 0);
    
	if (!wq){
		ret = -ENOMEM;
		goto err;
	}

	register_early_suspend(&early_suspend);

	INIT_WORK(&suspend, hotplug_suspend);
	INIT_WORK(&resume, hotplug_resume);
	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);

	queue_delayed_work_on(0, wq, &decide_hotplug, HZ * 30);
    
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

	if (ret){
		return ret;
	}

	ret = platform_device_register(&hotplug_device);

	if (ret){
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

