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
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
 
#include <mach/cpufreq.h>

#define DEFAULT_FIRST_LEVEL 60
#define DEFAULT_THIRD_LEVEL 30
#define DEFAULT_SUSPEND_FREQ 702000
#define DEFAULT_CORES_ON_TOUCH 2
#define DEFAULT_COUNTER 10
#define SEC_THRESHOLD 200
#define BOOST_THRESHOLD 5000
#define TIMER HZ

static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static unsigned int online_cpus;
static unsigned int default_first_level;
static unsigned int default_third_level;
static unsigned int cores_on_touch;
static unsigned int suspend_frequency;
static unsigned long time_stamp;
static unsigned long now;
static bool core_boost[4];
static short first_counter = 0;
static short third_counter = 0;

void set_core_boost(int cpu, bool boost);

static void scale_interactive_tunables(unsigned int up_threshold,
	unsigned int timer_rate, 
	unsigned int min_sample_time)
{
	scale_up_threshold(up_threshold);
	scale_timer_rate(timer_rate);
	scale_min_sample_time(min_sample_time);
}

static bool online_core(void)
{
	unsigned int cpu;
	
	if (online_cpus > 3)
		return false;
	
	for_each_possible_cpu(cpu) 
	{
		if (!cpu_online(cpu)) 
		{
			cpu_up(cpu);
			break;
		}
	}
	
	first_counter = 4;
	third_counter = 0;
	
	return true;
}

static bool offline_core(unsigned int cpu)
{   
	if ((now - time_stamp < BOOST_THRESHOLD && 
			online_cpus == cores_on_touch) || !cpu)
		return false;	
	
	if(cpu)
	{
		cpu_down(cpu);
	}
	
	first_counter = 0;
	third_counter = 4;
	
	return true;  
}

static void __cpuinit decide_hotplug_func(struct work_struct *work)
{
	unsigned int cpu, cpu_boost, lowest_cpu = 0;
	unsigned int i = 0, load, av_load = 0, lowest_cpu_load = 100;
	//short load_array[4] = {};

	now = ktime_to_ms(ktime_get());
	online_cpus = num_online_cpus();

	if (is_touching)
	{
		time_stamp = now;
		
		if (online_cpus < cores_on_touch)
		{
			for(i = 0; i < (cores_on_touch - online_cpus); i++)
			{
				online_core();
			}
			goto end;
		}
	}

	for_each_online_cpu(cpu) 
	{
		load = report_load_at_max_freq(cpu);
		//load_array[cpu] = load;
		
		if (load < lowest_cpu_load && cpu &&
				!(core_boost[cpu] && is_touching))
		{
			lowest_cpu = cpu;
			lowest_cpu_load = load;
		}
		
		av_load += load;
	}
	
	av_load = av_load / online_cpus;
	
	if (av_load >= default_first_level)
	{
		if (first_counter < DEFAULT_COUNTER)
			first_counter += 2;
		
		if (third_counter > 0)
			third_counter -= 2;
			
		if (first_counter >= DEFAULT_COUNTER)
			online_core();	
	}
	else if (av_load <= default_third_level)
	{
		if (third_counter < DEFAULT_COUNTER)
			third_counter += 2;
		
		if (first_counter > 0)
			first_counter -= 2;
			
		if (third_counter >= DEFAULT_COUNTER)
			offline_core(lowest_cpu);	
	}
	else
	{
		if (first_counter > 0)
			first_counter--;
		
		if (third_counter > 0)
			third_counter--; 
	}
	
end:
	
	if (online_cpus != num_online_cpus())
	{
		i = 0;
	
		for_each_possible_cpu(cpu_boost)
		{
			if (cpu_online(cpu_boost) && i < cores_on_touch)
			{
				core_boost[cpu_boost] = true;
				i++;
			}
			else
			{
				core_boost[cpu_boost] = false;
			}
		}
	
		// above_hispeed_delay, timer_rate, min_sample_time
		switch(num_online_cpus())
		{
			case 1: scale_interactive_tunables(95, 30000, 10000); break;
			case 2: scale_interactive_tunables(90, 40000, 20000); break;
			case 3: scale_interactive_tunables(90, 30000, 40000); break;
			case 4: scale_interactive_tunables(90, 20000, 80000); break;
		}
	}
	
/*	cpu = 0;
	pr_info("----HOTPLUG DEBUG INFO----\n");
	pr_info("Cores on:\t%d", num_online_cpus());
	pr_info("Core0:\t%d", load_array[0]);
	pr_info("Core1:\t%d", load_array[1]);
	pr_info("Core2:\t%d", load_array[2]);
	pr_info("Core3:\t%d", load_array[3]);
	pr_info("Av Load:\t%d", av_load);
	pr_info("-------------------------");
	pr_info("Up count:\t%d\n",first_counter);
	pr_info("Dw count:\t%d\n",third_counter);*/
	
	queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void __cpuinit mako_hotplug_early_suspend(struct early_suspend *handler)
{	 
	unsigned int cpu;

	/* cancel the hotplug work when the screen is off and flush the WQ */
	cancel_delayed_work_sync(&decide_hotplug);
	flush_workqueue(wq);

	pr_info("Early Suspend stopping Hotplug work...\n");
	
	for_each_possible_cpu(cpu) 
	{
		if (cpu) {cpu_down(cpu);}
		core_boost[cpu] = false;
	}
	scale_interactive_tunables(95, 30000, 10000);
	
	is_touching = false;
	first_counter = 0;
	third_counter = 0;
	
	/* cap max frequency to 702MHz by default */
	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, 
			suspend_frequency);
	pr_info("Cpulimit: Early suspend - limit cpu%d max frequency to: %dMHz\n",
			0, suspend_frequency/1000);
}

static void __cpuinit mako_hotplug_late_resume(struct early_suspend *handler)
{  
	struct cpufreq_policy policy;
	unsigned int cpu;

	/* 2 online cores and max freq when the screen goes online */
	for (cpu = 0; cpu < 2; cpu++)
	{
		if (!cpu_online(cpu))
			cpu_up(cpu);
			
		core_boost[cpu] = true;
		cpufreq_get_policy(&policy, cpu);
		__cpufreq_driver_target(&policy, 1026000, CPUFREQ_RELATION_H);
	}
	scale_interactive_tunables(90, 40000, 20000);
	
	freq_boosted_time = ktime_to_ms(ktime_get());
	is_touching = true;
	
	/* restore max frequency */
	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, MSM_CPUFREQ_NO_LIMIT);
	pr_info("Cpulimit: Late resume - restore cpu%d max frequency.\n", 0);
	
	pr_info("Late Resume starting Hotplug work...\n");
	queue_delayed_work(wq, &decide_hotplug, HZ);
}

static struct early_suspend mako_hotplug_suspend =
{
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = mako_hotplug_early_suspend,
	.resume = mako_hotplug_late_resume,
};

/* sysfs functions for external driver */

void update_first_level(unsigned int level)
{
	default_first_level = level;
}

void update_third_level(unsigned int level)
{
	default_third_level = level;
}

void update_suspend_frequency(unsigned int freq)
{
	suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
	cores_on_touch = num;
}

unsigned int get_first_level()
{
	return default_first_level;
}

unsigned int get_third_level()
{
	return default_third_level;
}

unsigned int get_suspend_frequency()
{
	return suspend_frequency;
}

unsigned int get_cores_on_touch()
{
	return cores_on_touch;
}

bool get_core_boost(unsigned int cpu)
{
	return core_boost[cpu];
}

/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
	pr_info("Mako Hotplug driver started.\n");
	
	/* init everything here */
	time_stamp = 0;
	online_cpus = num_online_cpus();
	default_first_level = DEFAULT_FIRST_LEVEL;
	default_third_level = DEFAULT_THIRD_LEVEL;
	suspend_frequency = DEFAULT_SUSPEND_FREQ;
	cores_on_touch = DEFAULT_CORES_ON_TOUCH;

	wq = alloc_workqueue("mako_hotplug_workqueue", 
					WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);
	
	if (!wq)
		return -ENOMEM;
	
	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	queue_delayed_work(wq, &decide_hotplug, HZ*25);
	
	register_early_suspend(&mako_hotplug_suspend);
	
	return 0;
}
late_initcall(mako_hotplug_init);

