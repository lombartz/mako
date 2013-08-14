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

#define DEFAULT_FIRST_LEVEL 90
#define DEFAULT_SECOND_LEVEL 60
#define DEFAULT_THIRD_LEVEL 30
#define DEFAULT_FOURTH_LEVEL 10
#define DEFAULT_SUSPEND_FREQ 702000
#define DEFAULT_CORES_ON_TOUCH 2
#define DEFAULT_COUNTER 10
#define SEC_THRESHOLD 200
#define HEATWAVE_COUNTER 20
#define BOOST_THRESHOLD 5000
#define TIMER HZ

struct cpu_stats
{
	unsigned int online_cpus;
	unsigned int default_first_level;
	unsigned int default_second_level;
	unsigned int default_third_level;
	unsigned int default_fourth_level;
	unsigned int cores_on_touch;
	unsigned int suspend_frequency;
	unsigned long time_stamp[2];
	unsigned long now;
	bool heatwave;
	unsigned short heatwave_counter;
};

static struct cpu_stats stats;
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static short second_counter = 0, third_counter = 0;

void set_core_boost(int cpu, bool boost);

static void scale_interactive_tunables(unsigned int above_hispeed_delay,
	unsigned int timer_rate, 
	unsigned int min_sample_time)
{
	scale_above_hispeed_delay(above_hispeed_delay);
	scale_timer_rate(timer_rate);
	scale_min_sample_time(min_sample_time);
}

static bool online_core(void)
{
	int cpu;
	
	if (stats.online_cpus > 3)
		return false;
	
	for_each_possible_cpu(cpu) 
	{
		if (cpu && !cpu_online(cpu)) 
		{
			cpu_up(cpu);
			break;
		}
	}
	
	second_counter = 4;
	third_counter = 0;
	
	return true;
}

static bool offline_core(int cpu)
{   
	if (stats.now - stats.time_stamp[1] < BOOST_THRESHOLD && 
	stats.online_cpus == stats.cores_on_touch)
		return false;	
	
	if(cpu)
	{
		cpu_down(cpu);
	}
	
	second_counter = 0;
	third_counter = 4;
	
	return true;  
}

static void __cpuinit decide_hotplug_func(struct work_struct *work)
{
	int cpu, cpu_boost, lowest_cpu = 0;
	unsigned int i = 0, load, av_load = 0, lowest_cpu_load = 100;
	//int load_array[4] = {};

	stats.now = ktime_to_ms(ktime_get());
	stats.online_cpus = num_online_cpus();

	if (is_touching)
	{
		stats.time_stamp[1] = stats.now;
		
		if (stats.online_cpus < stats.cores_on_touch)
		{
			for(i = 0; i < (stats.cores_on_touch - stats.online_cpus); i++)
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
		
		if (load >= stats.default_first_level 
			&& stats.now - stats.time_stamp[0] > SEC_THRESHOLD)
		{
			if (online_core())
			{
				stats.time_stamp[0] = stats.now;
				goto end;
			}
		}
		else if (load <= stats.default_fourth_level && cpu != 0 && 
		stats.now - stats.time_stamp[0] > SEC_THRESHOLD)
		{
			if (offline_core(cpu))
			{
				stats.time_stamp[0] = stats.now;
				goto end;
			}
		}
		
		if (load < lowest_cpu_load && cpu != 0)
		{
			lowest_cpu = cpu;
			lowest_cpu_load = load;
		}
		
		av_load += load;
	}
	
	av_load = av_load / stats.online_cpus;
	
	if (av_load >= stats.default_second_level)
	{
		if (second_counter < DEFAULT_COUNTER)
			second_counter += 2;
		
		if (third_counter > 0)
			third_counter -= 2;
			
		if (second_counter >= DEFAULT_COUNTER)
			online_core();	
	}
	else if (av_load <= stats.default_third_level)
	{
		if (third_counter < DEFAULT_COUNTER)
			third_counter += 2;
		
		if (second_counter > 0)
			second_counter -= 2;
			
		if (third_counter >= DEFAULT_COUNTER)
			offline_core(lowest_cpu);	
	}
	else
	{
		if (second_counter > 0)
			second_counter--;
		
		if (third_counter > 0)
			third_counter--; 
	}
	
end:
		
	if (stats.online_cpus > 3)
	{
		if (av_load >= 90)
		{
			if (stats.heatwave_counter <= HEATWAVE_COUNTER)
				stats.heatwave_counter += 2;
			
			if (stats.heatwave_counter >= HEATWAVE_COUNTER / 2)
				stats.heatwave = true;
		}
		else
		{
			if (stats.heatwave_counter > 0)
				stats.heatwave_counter--;
			
			if (stats.heatwave_counter <= 0)
				stats.heatwave = false;
		}
	}
	else
	{
		stats.heatwave = false;
	}
	
	if (stats.online_cpus != num_online_cpus())
	{
		i = 0;
	
		for_each_possible_cpu(cpu_boost)
		{
			if (cpu_online(cpu_boost) && i < stats.cores_on_touch)
			{
				set_core_boost(cpu_boost, true);
				i++;
			}
			else
			{
				set_core_boost(cpu_boost, false);
			}
		}
	
		// above_hispeed_delay, timer_rate, min_sample_time
		switch(num_online_cpus())
		{
			case 1: scale_interactive_tunables(10000, 20000, 40000); break;
			case 2: scale_interactive_tunables(20000, 30000, 20000); break;
			case 3: scale_interactive_tunables(20000, 40000, 40000); break;
			case 4: scale_interactive_tunables(10000, 40000, 60000); break;
		}
	}
	
	/*
	cpu = 0;
	pr_info("----HOTPLUG DEBUG INFO----\n");
	pr_info("Cores on:\t%d", num_online_cpus());
	pr_info("Core0:\t%d", load_array[0]);
	pr_info("Core1:\t%d", load_array[1]);
	pr_info("Core2:\t%d", load_array[2]);
	pr_info("Core3:\t%d", load_array[3]);
	pr_info("Av Load:\t%d", av_load);
	for_each_online_cpu(cpu)
	{
	pr_info("Cur_max%d:\t%d",cpu,get_cur_max(cpu));
	}
	pr_info("-------------------------");
	pr_info("Up count:\t%d\n",second_counter);
	pr_info("Dw count:\t%d\n",third_counter);
	*/
	
	queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void __cpuinit mako_hotplug_early_suspend(struct early_suspend *handler)
{	 
	int cpu;

	/* cancel the hotplug work when the screen is off and flush the WQ */
	cancel_delayed_work_sync(&decide_hotplug);
	flush_workqueue(wq);

	pr_info("Early Suspend stopping Hotplug work...\n");
	
	for_each_online_cpu(cpu) 
	{
		if (cpu) 
		{
			cpu_down(cpu);
		}
	}
	scale_interactive_tunables(10000, 20000, 40000);
	
	second_counter = 0;
	third_counter = 0;
	
	/* cap max frequency to 702MHz by default */
	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, 
			stats.suspend_frequency);
	pr_info("Cpulimit: Early suspend - limit cpu%d max frequency to: %dMHz\n",
			0, stats.suspend_frequency/1000);
}

static void __cpuinit mako_hotplug_late_resume(struct early_suspend *handler)
{  
	int cpu;

	/* online all cores when the screen goes online */
	for_each_possible_cpu(cpu) 
	{
		if (cpu) 
		{
			cpu_up(cpu);
		}
	}
	scale_interactive_tunables(0, 20000, 80000);
	
	second_counter = 0;
	third_counter = 0;
	
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
	stats.default_first_level = level;
}

void update_second_level(unsigned int level)
{
	stats.default_second_level = level;
}

void update_third_level(unsigned int level)
{
	stats.default_third_level = level;
}

void update_fourth_level(unsigned int level)
{
	stats.default_fourth_level = level;
}

void update_suspend_frequency(unsigned int freq)
{
	stats.suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
	stats.cores_on_touch = num;
}

unsigned int get_first_level()
{
	return stats.default_first_level;
}

unsigned int get_second_level()
{
	return stats.default_second_level;
}

unsigned int get_third_level()
{
	return stats.default_third_level;
}

unsigned int get_fourth_level()
{
	return stats.default_fourth_level;
}

unsigned int get_suspend_frequency()
{
	return stats.suspend_frequency;
}

unsigned int get_cores_on_touch()
{
	return stats.cores_on_touch;
}

bool get_heatwave()
{
	return stats.heatwave;
}

/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
	pr_info("Mako Hotplug driver started.\n");
	
	/* init everything here */
	stats.time_stamp[0] = 0;
	stats.time_stamp[1] = 0;
	stats.online_cpus = num_online_cpus();
	stats.default_first_level = DEFAULT_FIRST_LEVEL;
	stats.default_second_level = DEFAULT_SECOND_LEVEL;
	stats.default_third_level = DEFAULT_THIRD_LEVEL;
	stats.default_fourth_level = DEFAULT_FOURTH_LEVEL;
	stats.suspend_frequency = DEFAULT_SUSPEND_FREQ;
	stats.cores_on_touch = DEFAULT_CORES_ON_TOUCH;
	stats.now = 0;
	stats.heatwave = false;

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

