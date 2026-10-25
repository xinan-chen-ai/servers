#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>

// 定义定时器结构体
static struct timer_list my_timer;

// 定时器回调函数
static void timer_callback(struct timer_list *t)
{
    int pin_value;
    // 打印日志
    printk(KERN_INFO "Timer fired! Jiffies: %lu\n", jiffies);
        
    pin_value = gpio_get_value(101); //GPIO3_A5
    printk(KERN_INFO "gpio_get_value: %d\n", pin_value);

    // 重新设置定时器，实现周期性
    // HZ 是内核每秒的节拍数，HZ 是 1 秒
    mod_timer(&my_timer, jiffies + (HZ / 2));
}

// 模块初始化函数
static int __init timer_init(void)
{
    printk(KERN_INFO "Timer module loaded\n");

    // 初始化定时器
    timer_setup(&my_timer, timer_callback, 0);

    // 设置定时器到期时间
    my_timer.expires = jiffies + (HZ / 2);

    // 注册定时器
    add_timer(&my_timer);

    return 0;
}

// 模块退出函数
static void __exit timer_exit(void)
{
    // 删除定时器（如果还在运行）
    del_timer(&my_timer);
    printk(KERN_INFO "Timer module unloaded\n");
}

// 注册模块初始化和退出函数
module_init(timer_init);
module_exit(timer_exit);

// 模块信息
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple Linux kernel timer driver that prints log every second");
MODULE_VERSION("1.0");