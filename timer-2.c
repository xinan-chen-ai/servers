#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/timer.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/fs.h>        // 必须包含：用于 filp_open, kernel_read
#include <linux/uaccess.h>   // 必须包含：用于 kernel_read

#define DRIVER_NAME "gpio_rising_timer_key_simple"
#define KEY_EVENT_CODE 116                         // 上报的按键码 
#define TIMER_EXPIRE_TIME_MS 30000                // 主定时器超时时间：30秒
#define POLLING_INTERVAL_MS 1000                  // 轮询定时器间隔：1秒
#define POLLING_TIMEOUT_SEC 15                    // 轮询超时时间

#define GPIO_NUMBER 101                            // 实际 GPIO 编号

#define BRIGHTNESS_SYSFS_PATH "/sys/class/backlight/backlight/brightness"
#define MAX_BRIGHTNESS_LEN 16 

struct gpio_timer_data {
    struct input_dev *input_dev;
    struct timer_list screen_timer;     // 主屏幕超时定时器 (用于亮屏后计时熄屏)
    struct timer_list polling_timer;    // 轮询定时器 (用于检测无活动后熄屏)
    int irq_num;
    bool timer_active;                  // 主定时器活动标志
    bool screen_on;                     // 屏幕状态：true=亮, false=灭
    spinlock_t lock;                    // 自旋锁
    int no_activity_count;              // 无活动计数器 (秒)
    bool polling_timer_active;          // 轮询定时器活动标志
};

static struct gpio_timer_data *g_data;

/**
 * read_brightness_and_update_state - 读取背光节点并更新 g_data->screen_on 状态
 * @return: 0 成功, 负数表示错误码 (读取失败时，内部会将 screen_on 设为 false)
 */
static int read_brightness_and_update_state(void)
{
    struct file *file = NULL;
    ssize_t ret;
    char buf[MAX_BRIGHTNESS_LEN];
    long brightness_val = 0;
    int rc = 0;
    loff_t pos = 0;
    unsigned long flags;

    // 使用锁保护 g_data->screen_on 的更新
    spin_lock_irqsave(&g_data->lock, flags);

    file = filp_open(BRIGHTNESS_SYSFS_PATH, O_RDONLY, 0);
    if (IS_ERR(file)) {
        rc = PTR_ERR(file);
        pr_debug("%s: Open %s failed, err=%d. Assume OFF.\n", DRIVER_NAME, BRIGHTNESS_SYSFS_PATH, rc);
        g_data->screen_on = false;
        spin_unlock_irqrestore(&g_data->lock, flags);
        return rc;
    }

    ret = kernel_read(file, buf, sizeof(buf) - 1, &pos);
    filp_close(file, NULL); // 立即关闭文件

    if (ret < 0) {
        pr_debug("%s: Read %s failed, err=%zd. Assume OFF.\n", DRIVER_NAME, BRIGHTNESS_SYSFS_PATH, ret);
        g_data->screen_on = false;
        spin_unlock_irqrestore(&g_data->lock, flags);
        return (int)ret;
    }

    if (ret == 0) {
        pr_debug("%s: Read 0 bytes from %s. Assume OFF.\n", DRIVER_NAME, BRIGHTNESS_SYSFS_PATH);
        g_data->screen_on = false;
        spin_unlock_irqrestore(&g_data->lock, flags);
        return -EIO;
    }

    buf[ret] = '\0';
    strim(buf);

    if (kstrtol(buf, 10, &brightness_val) < 0) {
        pr_debug("%s: Parse '%s' failed. Assume OFF.\n", DRIVER_NAME, buf);
        g_data->screen_on = false;
        spin_unlock_irqrestore(&g_data->lock, flags);
        return -EINVAL;
    }

    g_data->screen_on = (brightness_val > 0); // 0=灭, >0=亮
    pr_debug("%s: Brightness=%ld, screen_on=%s\n", DRIVER_NAME, brightness_val, g_data->screen_on ? "ON" : "OFF");

    spin_unlock_irqrestore(&g_data->lock, flags);
    return 0;
}

/**
 * polling_timer_callback - 轮询定时器回调函数
 * 每隔 POLLING_INTERVAL_MS 检查一次 GPIO 状态
 * 如果持续 POLLING_TIMEOUT_SEC 秒无活动，则熄屏
 */
static void polling_timer_callback(struct timer_list *t)
{ 
    struct gpio_timer_data *data = from_timer(data, t, polling_timer);
    unsigned long flags;
    int current_gpio_val;
    bool gpio_active = false;

    // 在软中断上下文，可以安全地读取GPIO
    current_gpio_val = gpio_get_value(GPIO_NUMBER);
    // 假设高电平表示活动 (active high)
    if (current_gpio_val == 1) {
        gpio_active = true;
    }

    spin_lock_irqsave(&data->lock, flags);

    if (gpio_active) {
        data->no_activity_count = 0;
        pr_debug("%s: Polling - GPIO active, reset counter\n", DRIVER_NAME);
    } else {
        data->no_activity_count++;
        pr_debug("%s: Polling - No activity, count=%d\n", DRIVER_NAME, data->no_activity_count);

        if (data->no_activity_count >= POLLING_TIMEOUT_SEC) {
            if (data->screen_on) {
                input_report_key(data->input_dev, KEY_EVENT_CODE, 1);
                input_sync(data->input_dev);
                input_report_key(data->input_dev, KEY_EVENT_CODE, 0);
                input_sync(data->input_dev);
                pr_info("%s: POLLING: No activity for %d seconds, sent KEY_EVENT_CODE(%d) to turn OFF screen\n",
                        DRIVER_NAME, POLLING_TIMEOUT_SEC, KEY_EVENT_CODE);
                data->screen_on = false;
            }
            data->no_activity_count = 0;
        }
    }

    spin_unlock_irqrestore(&data->lock, flags);

    // 重新启动轮询定时器
    mod_timer(&data->polling_timer, jiffies + msecs_to_jiffies(POLLING_INTERVAL_MS));
}

/**
 * gpio_irq_handler - GPIO中断处理函数 (Top Half)
 * 实现核心逻辑：
 *   - 屏幕亮：只重置 screen_timer (延长亮屏)
 *   - 屏幕灭：上报 KEY_EVENT_CODE (唤醒屏幕) 并启动 screen_timer
 */
static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
    struct gpio_timer_data *data = (struct gpio_timer_data *)dev_id;
    unsigned long flags;
    int rc;

    // --- 1. 读取当前屏幕状态 ---
    rc = read_brightness_and_update_state();
    // 即使读取失败，g_data->screen_on 已被设为 false，逻辑仍然安全

    spin_lock_irqsave(&data->lock, flags);

    // --- 2. 根据屏幕状态执行不同操作 ---
    if (data->screen_on) {
        // 屏幕是亮的
        // 只重置主定时器，延长熄屏时间
        if (data->timer_active) {
            del_timer(&data->screen_timer);
        }
        mod_timer(&data->screen_timer, jiffies + msecs_to_jiffies(TIMER_EXPIRE_TIME_MS));
        data->timer_active = true;
        data->no_activity_count = 0; // 重置轮询计数器
        pr_info("%s: IRQ: Screen ON. Timer reset to %dms.\n", DRIVER_NAME, TIMER_EXPIRE_TIME_MS);
    } else {
        // 屏幕是灭的
        // 上报按键事件，触发系统亮屏
        input_report_key(data->input_dev, KEY_EVENT_CODE, 1);
        input_sync(data->input_dev);
        input_report_key(data->input_dev, KEY_EVENT_CODE, 0);
        input_sync(data->input_dev);
        pr_info("%s: IRQ: Screen OFF -> turning ON, sent KEY_EVENT_CODE(%d)\n",
                DRIVER_NAME, KEY_EVENT_CODE);
        data->screen_on = true; // 本地状态标记为亮

        // 启动主定时器，15秒后自动熄屏（如果无后续操作）
        mod_timer(&data->screen_timer, jiffies + msecs_to_jiffies(TIMER_EXPIRE_TIME_MS));
        data->timer_active = true;
        data->no_activity_count = 0; // 重置轮询计数器
    }

    spin_unlock_irqrestore(&data->lock, flags);
    return IRQ_HANDLED;
}

/**
 * screen_timer_callback - 主定时器超时回调
 * 用于在屏幕点亮后，经过 TIMER_EXPIRE_TIME_MS 时间后熄屏
 */
static void screen_timer_callback(struct timer_list *t)
{
    struct gpio_timer_data *data = from_timer(data, t, screen_timer);
    unsigned long flags;

    spin_lock_irqsave(&data->lock, flags);

    if (data->timer_active) {
        if (data->screen_on) {
            input_report_key(data->input_dev, KEY_EVENT_CODE, 1);
            input_sync(data->input_dev);
            input_report_key(data->input_dev, KEY_EVENT_CODE, 0);
            input_sync(data->input_dev);
            pr_info("%s: MAIN TIMER: Expired, sent KEY_EVENT_CODE(%d) to turn OFF screen\n",
                    DRIVER_NAME, KEY_EVENT_CODE);
            data->screen_on = false;
        }
        data->timer_active = false;
    }

    spin_unlock_irqrestore(&data->lock, flags);
}

/**
 * __init gpio_timer_key_init - 模块初始化函数
 */
static int __init gpio_timer_key_init(void)
{
    int ret;

    pr_info("%s: Initializing...\n", DRIVER_NAME);

    g_data = kzalloc(sizeof(*g_data), GFP_KERNEL);
    if (!g_data) {
        pr_err("%s: Failed to allocate memory\n", DRIVER_NAME);
        return -ENOMEM;
    }

    g_data->timer_active = false;
    g_data->no_activity_count = 0;
    g_data->polling_timer_active = false;

    ret = gpio_request_one(GPIO_NUMBER, GPIOF_IN, DRIVER_NAME);
    if (ret) {
        pr_err("%s: Failed to request GPIO %d, error %d\n", DRIVER_NAME, GPIO_NUMBER, ret);
        goto free_data;
    }
    pr_info("%s: Requested GPIO %d\n", DRIVER_NAME, GPIO_NUMBER);

    g_data->irq_num = gpio_to_irq(GPIO_NUMBER);
    if (g_data->irq_num < 0) {
        pr_err("%s: Failed to get IRQ for GPIO %d\n", DRIVER_NAME, GPIO_NUMBER);
        ret = g_data->irq_num;
        goto free_gpio;
    }

    // --- 关键修改：使用普通 IRQ，不是线程化 IRQ ---
    ret = request_irq(g_data->irq_num,
                      gpio_irq_handler,           // 中断处理函数 (Top Half)
                      IRQF_TRIGGER_RISING,        // 上升沿触发
                      DRIVER_NAME,                // 设备名
                      g_data);                    // 私有数据
    if (ret) {
        pr_err("%s: Failed to request IRQ %d, error %d\n", DRIVER_NAME, g_data->irq_num, ret);
        goto free_gpio;
    }
    pr_info("%s: Requested IRQ %d\n", DRIVER_NAME, g_data->irq_num);

    g_data->input_dev = input_allocate_device();
    if (!g_data->input_dev) {
        pr_err("%s: Failed to allocate input device\n", DRIVER_NAME);
        ret = -ENOMEM;
        goto free_irq;
    }

    g_data->input_dev->name = DRIVER_NAME;
    g_data->input_dev->id.bustype = BUS_HOST;
    __set_bit(EV_KEY, g_data->input_dev->evbit);
    __set_bit(KEY_EVENT_CODE, g_data->input_dev->keybit);

    ret = input_register_device(g_data->input_dev);
    if (ret) {
        pr_err("%s: Failed to register input device, error %d\n", DRIVER_NAME, ret);
        goto free_input_dev;
    }
    pr_info("%s: Registered input device\n", DRIVER_NAME);

    // 初始化定时器
    timer_setup(&g_data->screen_timer, screen_timer_callback, 0);
    timer_setup(&g_data->polling_timer, polling_timer_callback, 0);

    spin_lock_init(&g_data->lock);

    // --- 初始化时读取一次屏幕状态 ---
    ret = read_brightness_and_update_state();
    if (ret < 0) {
        pr_warn("%s: Could not determine initial screen state, using default (OFF)\n", DRIVER_NAME);
        // 继续执行，不返回错误
    }

    // --- 启动轮询定时器 ---
    mod_timer(&g_data->polling_timer, jiffies + msecs_to_jiffies(POLLING_INTERVAL_MS));
    g_data->polling_timer_active = true;

    pr_info("%s: Initialization successful. Polling timer started. Initial screen state: %s\n",
            DRIVER_NAME, g_data->screen_on ? "ON" : "OFF");
    return 0;

free_input_dev:
    input_free_device(g_data->input_dev);
free_irq:
    free_irq(g_data->irq_num, g_data);
free_gpio:
    gpio_free(GPIO_NUMBER);
free_data:
    kfree(g_data);
    return ret;
}

/**
 * __exit gpio_timer_key_exit - 模块退出函数
 */
static void __exit gpio_timer_key_exit(void)
{
    pr_info("%s: Exiting...\n", DRIVER_NAME);

    if (g_data) {
        // 删除所有定时器
        del_timer_sync(&g_data->screen_timer);
        del_timer_sync(&g_data->polling_timer);

        input_unregister_device(g_data->input_dev);
        free_irq(g_data->irq_num, g_data);
        gpio_free(GPIO_NUMBER);
        kfree(g_data);
    }

    pr_info("%s: Exit complete\n", DRIVER_NAME);
}

module_init(gpio_timer_key_init);
module_exit(gpio_timer_key_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("GPIO Key Driver with Smart Screen Control: Read node in IRQ to decide action");
MODULE_VERSION("1.3");
