#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>

#define TIME_MS     1000

static struct timer_list my_timer;
static int timer_cnt;


// 定时器回调函数
static void timer_callback(struct timer_list *t)
{
    printk("timer_cnt = %d\r\n",timer_cnt);

    timer_cnt++;
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(TIME_MS));
}

// 模块初始化函数
static int __init timer_init(void)
{
    printk("timer_init\r\n");

    timer_setup(&my_timer, timer_callback, 0);
    timer_cnt = 0;
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(TIME_MS));

    return 0;
}

// 模块退出函数
static void __exit timer_exit(void)
{
    del_timer_sync(&my_timer);
    printk("timer_exit\r\n");
}

// 注册模块初始化和退出函数
module_init(timer_init);
module_exit(timer_exit);

// 模块信息
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CXN");
MODULE_DESCRIPTION("A simple Linux kernel timer driver that prints log every second");
MODULE_VERSION("1.0");