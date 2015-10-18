#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#include "rcio.h"
#include "protocol.h"

struct rcio_state *rcio;
struct rcio_pwm *pwm;

static int rcio_pwm_safety_off(struct rcio_state *state);

static int rcio_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm);
static void rcio_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm);
static int rcio_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm, int duty_ns, int period_ns);
static int rcio_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm);
static void rcio_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm);

static int rcio_pwm_create_sysfs_handle(void);

struct rcio_pwm {
    struct pwm_chip chip;
    struct rcio_state *rcio;
};

static const struct pwm_ops rcio_pwm_ops = {
    .enable = rcio_pwm_enable,
    .disable = rcio_pwm_disable,
    .config = rcio_pwm_config,
    .request = rcio_pwm_request,
    .free = rcio_pwm_free,
    .owner = THIS_MODULE,
};

#define RCIO_PWM_MAX_CHANNELS 8
static u16 values[RCIO_PWM_MAX_CHANNELS] = {0};

static u16 frequency = 50;
static bool frequency_updated = false;

static bool armed = false;

static ssize_t channel_store(struct kobject *kobj, struct kobj_attribute *attr,
            const char *buf, size_t count)
{
    int value;
    int ret;

    ret = kstrtoint(buf, 10, &value);
    if (ret < 0) {
        return ret;
    }

    if (!strcmp(attr->attr.name, "ch0")) {
        values[0] = value;
    } else if (!strcmp(attr->attr.name, "ch1")) {
        values[1] = value;
    } else if (!strcmp(attr->attr.name, "ch2")) {
        values[2] = value;
    } else if (!strcmp(attr->attr.name, "ch3")) {
        values[3] = value;
    } else if (!strcmp(attr->attr.name, "ch4")) {
        values[4] = value;
    } else if (!strcmp(attr->attr.name, "ch5")) {
        values[5] = value;
    } else if (!strcmp(attr->attr.name, "ch6")) {
        values[6] = value;
    } else if (!strcmp(attr->attr.name, "ch7")) {
        values[7] = value;
    }



    if (ret < 0) {
        return -EBUSY;
    }

    return count;
}

static ssize_t frequency_store(struct kobject *kobj, struct kobj_attribute *attr,
            const char *buf, size_t count)
{
    int value;
    int ret;

    ret = kstrtoint(buf, 10, &value);
    if (ret < 0) {
        return ret;
    }

    frequency = value;
    frequency_updated = true;

    if (frequency < 0 || frequency > 1000) {
        return -EINVAL;
    }

    return count;
}

static ssize_t armed_store(struct kobject *kobj, struct kobj_attribute *attr,
            const char *buf, size_t count)
{
    int value;
    int ret;

    ret = kstrtoint(buf, 10, &value);
    if (ret < 0) {
        return ret;
    }

    if (value > 0) {
        armed = true;
    } else {
        armed = false;
    }

    return count;
}

static ssize_t frequency_show(struct kobject *kobj, struct kobj_attribute *attr,
            char *buf)
{
    return sprintf(buf, "%d\n", frequency);
}

static ssize_t armed_show(struct kobject *kobj, struct kobj_attribute *attr,
            char *buf)
{
    return sprintf(buf, "%d\n", armed ? 1: 0);
}


static struct kobj_attribute ch0_attribute = __ATTR(ch0, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch1_attribute = __ATTR(ch1, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch2_attribute = __ATTR(ch2, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch3_attribute = __ATTR(ch3, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch4_attribute = __ATTR(ch4, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch5_attribute = __ATTR(ch5, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch6_attribute = __ATTR(ch6, S_IWUSR, NULL, channel_store);
static struct kobj_attribute ch7_attribute = __ATTR(ch7, S_IWUSR, NULL, channel_store);
static struct kobj_attribute frequency_attribute = __ATTR_RW(frequency);
static struct kobj_attribute armed_attribute = __ATTR_RW(armed);

static struct attribute *attrs[] = {
    &ch0_attribute.attr,
    &ch1_attribute.attr,
    &ch2_attribute.attr,
    &ch3_attribute.attr,
    &ch4_attribute.attr,
    &ch5_attribute.attr,
    &ch6_attribute.attr,
    &ch7_attribute.attr,
    &frequency_attribute.attr,
    &armed_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .name = "pwm",
    .attrs = attrs,
};

int rcio_pwm_update(struct rcio_state *state)
{
    if (frequency_updated) {
        if (state->register_set_byte(state, PX4IO_PAGE_SETUP, PX4IO_P_SETUP_PWM_DEFAULTRATE, frequency) < 0) {
            printk(KERN_INFO "Frequency not set\n");
        }
        frequency_updated = false;
    }

    if (armed) {
        return state->register_set(state, PX4IO_PAGE_DIRECT_PWM, 0, values, RCIO_PWM_MAX_CHANNELS);
    }

    return true;
}

static int rcio_pwm_safety_off(struct rcio_state *state)
{
    return state->register_set_byte(state, PX4IO_PAGE_SETUP, PX4IO_P_SETUP_FORCE_SAFETY_OFF, PX4IO_FORCE_SAFETY_MAGIC);
}

int rcio_pwm_probe(struct rcio_state *state)
{
    int ret;

    rcio = state;

    ret = sysfs_create_group(rcio->object, &attr_group);

    if (ret < 0) {
        pr_err("PWM node not created");
        return ret;
    }

    if (rcio_pwm_safety_off(state) < 0) {
        pr_err("SAFETY ON");
        return -ENOTCONN;
    }

    if (state->register_set_byte(state, PX4IO_PAGE_SETUP, PX4IO_P_SETUP_ARMING, 
                PX4IO_P_SETUP_ARMING_IO_ARM_OK | 
                PX4IO_P_SETUP_ARMING_FMU_ARMED |
                PX4IO_P_SETUP_ARMING_ALWAYS_PWM_ENABLE) < 0) {
        pr_err("ARMING OFF");
        return -ENOTCONN;
    }
    
    if (state->register_set_byte(state, PX4IO_PAGE_SETUP, PX4IO_P_SETUP_PWM_DEFAULTRATE, frequency) < 0) {
        pr_err("Frequency not set");
        return -ENOTCONN;
    }

    if (rcio_pwm_create_sysfs_handle() < 0) {
        pr_warn("Generic PWM interface for RCIO not created");
    }

    return 0;
}

static int rcio_pwm_create_sysfs_handle(void)
{
    pwm = kzalloc(sizeof(struct rcio_pwm), GFP_KERNEL);
    
    if (!pwm)
        return -ENOMEM;

    pwm->chip.ops = &rcio_pwm_ops;
    pwm->chip.npwm = RCIO_PWM_MAX_CHANNELS;
    pwm->chip.can_sleep = false;

    return pwmchip_add(&pwm->chip);
}

static int rcio_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
    return 0;
}

static void rcio_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{

}

static int rcio_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm, int duty_ns, int period_ns)
{
    return 0;
}

static int rcio_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
    return 0;
}

static void rcio_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{

}


EXPORT_SYMBOL_GPL(rcio_pwm_probe);
EXPORT_SYMBOL_GPL(rcio_pwm_update);
MODULE_AUTHOR("Gerogii Staroselskii <georgii.staroselskii@emlid.com>");
MODULE_DESCRIPTION("RCIO PWM driver");
MODULE_LICENSE("GPL v2");
