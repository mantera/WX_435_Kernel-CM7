#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <mach/gpio.h>
#include <mach/vreg.h>
#include <linux/bi041p_ts.h>
#include <asm/uaccess.h>
#include "../../../arch/arm/mach-msm/smd_private.h"
#include "../../../arch/arm/mach-msm/proc_comm.h"

#ifdef CONFIG_FIH_TOUCHSCREEN_BU21018MWV
extern int bu21018mwv_active;
#endif

// flag of HW type
static int hw_ver = HW_UNKNOW;
// static int bi041p_debug = 0;
static bool bTouch2 = 0;
#ifdef CONFIG_FIH_FTM
static int file_path = 0;
#endif

bool bBackCapKeyPressed = 0;
bool bMenuCapKeyPressed = 0;
bool bHomeCapKeyPressed = 0;
bool bSearchCapKeyPressed = 0;

static int int_disable = 0;

static struct workqueue_struct *bi041p_wq;

struct bi041p_info {
    struct i2c_client *client;
    struct input_dev *input;
    struct work_struct wqueue;
#ifdef CONFIG_HAS_EARLYSUSPEND
    struct early_suspend es;
#endif
} bi041p;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bi041p_early_suspend(struct early_suspend *h);
static void bi041p_late_resume(struct early_suspend *h);
#endif

static int bi041p_i2c_recv(u8 *buf, u32 count)
{
	struct i2c_msg msgs[] = {
		[0] = {
			.addr = bi041p.client->addr,
			.flags = I2C_M_RD,
			.len = count,
			.buf = buf,
		},
	};
	
	return (i2c_transfer(bi041p.client->adapter, msgs, 1) < 0) ? -EIO : 0;
}

static int bi041p_i2c_send(u8 *buf, u32 count)
{
	struct i2c_msg msgs[]= {
		[0] = {
			.addr = bi041p.client->addr,
			.flags = 0,
			.len = count,
			.buf = buf,
		},
	};
	
	return (i2c_transfer(bi041p.client->adapter, msgs, 1) < 0) ? -EIO : 0;
}

#define FW_MAJOR(x) ((((int)((x)[1])) & 0xF) << 4) + ((((int)(x)[2]) & 0xF0) >> 4)
#define FW_MINOR(x) ((((int)((x)[2])) & 0xF) << 4) + ((((int)(x)[3]) & 0xF0) >> 4)

/*
 * Keep the the Interrupt Service Request (ISR) workqueue as efficient as
 * possible so that the driver doesn't slow down other parts of the system
 * when under heavy load (i.e. gaming).
 */
#define XCORD1(x) ((((int)((x)[1]) & 0xF0) << 4) + ((int)((x)[2])))
#define YCORD1(y) ((((int)((y)[1]) & 0x0F) << 8) + ((int)((y)[3])))
#define XCORD2(x) ((((int)((x)[4]) & 0xF0) << 4) + ((int)((x)[5])))
#define YCORD2(y) ((((int)((y)[4]) & 0x0F) << 8) + ((int)((y)[6])))

static void bi041p_isr_workqueue(struct work_struct *work) {
    u8 buffer[9];
    int cnt, cap_button;
    int retry = 3;

    do {
        if (bi041p_i2c_recv(buffer, ARRAY_SIZE(buffer)) == 0) break;
        printk(KERN_INFO "[Touch] %s: retry = %d\n", __func__, 4-retry);
    } while (retry--);

/*
    if (bi041p_debug)
        printk(KERN_INFO "[Touch] %s: buffer[0]=0x%.2x,buffer[1]=0x%.2x,
                                      buffer[2]=0x%.2x,buffer[3]=0x%.2x,
                                      buffer[4]=0x%.2x,buffer[5]=0x%.2x,
                                      buffer[6]=0x%.2x,buffer[7]=0x%.2x,
                                      buffer[8]=0x%.2x\n", __func__,
                                      buffer[0],buffer[1],buffer[2],
                                      buffer[3],buffer[4],buffer[5],
                                      buffer[6],buffer[7],buffer[8]);
*/

    if (buffer[0] == 0x5A) {
        cnt = (buffer[8]) >> 1;
        cap_button = (buffer[8]) >> 4;

        if (cap_button == 0) {   // Touchscreen Pressed

#ifdef CONFIG_FIH_FTM
            if (cnt) {
                input_report_abs(bi041p.input, ABS_X, XCORD1(buffer));
                input_report_abs(bi041p.input, ABS_Y, abs(TS_MAX_Y - YCORD1(buffer)));
                input_report_abs(bi041p.input, ABS_PRESSURE, 255);
                input_report_key(bi041p.input, BTN_TOUCH, 1);
            } else {
                input_report_abs(bi041p.input, ABS_PRESSURE, 0);
                input_report_key(bi041p.input, BTN_TOUCH, 0);
            }
#else
            if (cnt) {      // First touch
                input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 255);
                input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD1(buffer));
                input_report_abs(bi041p.input, ABS_MT_POSITION_Y, TS_MAX_Y - YCORD1(buffer));
                input_mt_sync(bi041p.input);
            } else {        // First touch released
                input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
                input_mt_sync(bi041p.input);
            }
            if (cnt > 1) {  // Second touch
                input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 255);
                input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD2(buffer));
                input_report_abs(bi041p.input, ABS_MT_POSITION_Y, TS_MAX_Y - YCORD2(buffer));
                input_mt_sync(bi041p.input);
                bTouch2 = 1;
            } else if (bTouch2) {        // Second touch released
                input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
                input_mt_sync(bi041p.input);
                bTouch2 = 0;
            }
#endif

        } else if (cap_button == 1) {
            if (hw_ver == HW_FD1_PR3 || hw_ver == HW_FD1_PR4)
                input_report_key(bi041p.input, KEY_MENU, 1);
            else
                input_report_key(bi041p.input, KEY_BACK, 1);
            goto sync_out;

        } else if (cap_button == 2) {
            if (hw_ver == HW_FD1_PR3 || hw_ver == HW_FD1_PR4)
                input_report_key(bi041p.input, KEY_HOME, 1);
            else
                input_report_key(bi041p.input, KEY_MENU, 1);
            goto sync_out;

        } else if (cap_button == 4) {
            if (hw_ver == HW_FD1_PR3)
                input_report_key(bi041p.input, KEY_SEARCH, 1);
            else if (hw_ver == HW_FD1_PR4)
                input_report_key(bi041p.input, KEY_BACK, 1);
            else
                input_report_key(bi041p.input, KEY_HOME, 1);
            goto sync_out;

        } else if (cap_button == 8) {
            if (hw_ver == HW_FD1_PR3)
                input_report_key(bi041p.input, KEY_BACK, 1);
            else
                input_report_key(bi041p.input, KEY_SEARCH, 1);
            goto sync_out;
        }

        input_report_key(bi041p.input, KEY_BACK, 0);
        input_report_key(bi041p.input, KEY_MENU, 0);
        input_report_key(bi041p.input, KEY_HOME, 0);
        input_report_key(bi041p.input, KEY_SEARCH, 0);

sync_out:
	input_sync(bi041p.input);
    }
/*
 * Leave this out of production code
 *
    else if ((buffer[0] == 0x55) && (buffer[1] == 0x55) &&
             (buffer[2] == 0x55) && (buffer[3] == 0x55)) {
        printk(KERN_INFO "[Touch] %s: Receive the hello packet!\n", __func__);
    }
*/
}

static irqreturn_t bi041p_isr(int irq, void * handle)
{
    // Use dedicated bi041p workqueue for interrupt services to get
    // higher priority processing of normal touchscreen inputs.
    queue_work(bi041p_wq, &bi041p.wqueue);
    return IRQ_HANDLED;
}

static int bi041p_set_power_domain(void)
{
	struct vreg *vreg_ldo12;
	int rc;
	
	vreg_ldo12 = vreg_get(NULL, "gp9");
	
	if (IS_ERR(vreg_ldo12))
	{
		printk(KERN_ERR "%s: gp9 vreg get failed (%ld)\n", __func__, PTR_ERR(vreg_ldo12));
		return 0;
	}
	
	rc = vreg_set_level(vreg_ldo12, 3000);
	
	if (rc)
	{
		printk(KERN_ERR "%s: vreg LDO12 set level failed (%d)\n", __func__, rc);
		return 0;
	}
	
	rc = vreg_enable(vreg_ldo12);
	
	if (rc)
	{
		printk(KERN_ERR "%s: LDO12 vreg enable failed (%d)\n", __func__, rc);
		return 0;
	}
	
	return 1;
}

static int bi041p_reset(void)
{
	int retry = 100,		/* retry count of device detecting */	    
		pkt;				/* packet buffer */
	
	if (gpio_tlmm_config(GPIO_CFG(56, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE))
	{
		printk(KERN_ERR "[Touch] %s: gpio_tlmm_config: 56 failed.\n", __func__);
		return 0;
	}
	else
	{
		gpio_set_value(56, 1);
		msleep(1);
		gpio_set_value(56, 0);
	}
	
	do
	{
	    if (gpio_get_value(42) == 0)
        {
			bi041p_i2c_recv((char *)&pkt, sizeof(int));
			pkt ^= 0x55555555;
        }
        
		msleep(10);
		
		if (--retry == 0)
		{
			printk(KERN_ERR "[Touch] %s: receive hello package timeout.\n", __func__);
			return 0;
		}
	}while (pkt);
	
	return 1;
}

void bi041p_disable(int en)
{
	if (en && !int_disable)
	{
		disable_irq_nosync(bi041p.client->irq);
		int_disable = 1;
	}
	else if (!en && int_disable)
	{
		enable_irq(bi041p.client->irq);
		int_disable = 0;
	}
}

#ifdef CONFIG_FIH_FTM
#define FW_FILE_1 "/ftm/emc_ISP_cds.cds"
#define FW_FILE_2 "/sdcard/emc_ISP_cds.cds"
#else
#define FW_FILE "/data/misc/emc_ISP_cds.cds"
#endif

static int bi041p_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	
#ifdef CONFIG_FIH_TOUCHSCREEN_BU21018MWV
	// check ROHM exist
	if (bu21018mwv_active)
	{
		printk(KERN_INFO "[Touch] %s: BU21018MWV already exists. bi041p_probe() abort.\n", __func__);
		return -ENODEV;
	}
#endif
		
	// read HWID
	if ((fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR3) ||
		(fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR231))
	{
		hw_ver = HW_FD1_PR3;
		printk(KERN_INFO "[Touch] Virtual key mapping for FD1 PR235.\n");
	}
	else if ((fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR4) ||
			(fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR5))
	{
	    hw_ver = HW_FD1_PR4;
	    printk(KERN_INFO "[Touch] Virtual key mapping for FD1 PR4 and PR5.\n");
	}
	else if (fih_get_product_id() == Product_FD1)
	{
	    hw_ver = HW_FD1_PR4;
	    printk(KERN_INFO "[Touch] Virtual key mapping for FD1.\n");
	}
	else
	{
		hw_ver = HW_FB0;
		printk(KERN_INFO "[Touch] Virtual key mapping for FB0.\n");
	}
	
	// check I2C
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
	{
		printk(KERN_ERR "[Touch] %s: Check I2C functionality failed!\n", __func__);
		return -ENODEV;
	}
	
	bi041p.client = client;
	strlcpy(client->name, "bi041p", I2C_NAME_SIZE);
	i2c_set_clientdata(client, &bi041p);
    
	// set power
	if (!bi041p_set_power_domain())
	{
		printk(KERN_ERR "[Touch] %s: Set power fail!\n", __func__);
    	return -EIO;
	}
    
    // chip reset
    if (!bi041p_reset())
    {
    	printk(KERN_ERR "[Touch] %s: Chip reset fail!\n", __func__);
		goto err_chk;
    }
    
	//
	bi041p.input = input_allocate_device();
	
    if (bi041p.input == NULL)
	{
		printk(KERN_ERR "[Touch] %s: Can not allocate memory for touch input device!\n", __func__);
		goto err1;
	}
	
        bi041p.input->name  = "bi041p";
        bi041p.input->phys  = "bi041p/input0";
        set_bit(EV_KEY, bi041p.input->evbit);
        set_bit(EV_ABS, bi041p.input->evbit);
        set_bit(EV_SYN, bi041p.input->evbit);
        clear_bit(EV_REP, bi041p.input->keybit);
        clear_bit(EV_SW, bi041p.input->keybit);
        clear_bit(EV_LED, bi041p.input->keybit);
        clear_bit(EV_SND, bi041p.input->keybit);
        clear_bit(EV_FF, bi041p.input->keybit);
        clear_bit(EV_FF_STATUS, bi041p.input->keybit);
        clear_bit(EV_PWR, bi041p.input->keybit);
        set_bit(BTN_TOUCH, bi041p.input->keybit);
        set_bit(KEY_BACK, bi041p.input->keybit);
        set_bit(KEY_MENU, bi041p.input->keybit);
        set_bit(KEY_HOME, bi041p.input->keybit);
        set_bit(KEY_SEARCH, bi041p.input->keybit);

#ifdef CONFIG_FIH_FTM
	input_set_abs_params(bi041p.input, ABS_X, TS_MIN_X, TS_MAX_X, 0, 0);
	input_set_abs_params(bi041p.input, ABS_Y, TS_MIN_Y, TS_MAX_Y, 0, 0);
	input_set_abs_params(bi041p.input, ABS_PRESSURE, 0, 255, 0, 0);
#else
    input_set_abs_params(bi041p.input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(bi041p.input, ABS_MT_POSITION_X, TS_MIN_X, TS_MAX_X, 0, 0);
    input_set_abs_params(bi041p.input, ABS_MT_POSITION_Y, TS_MIN_Y, TS_MAX_Y, 0, 0);
#endif
	
	if (input_register_device(bi041p.input))
	{
		printk(KERN_ERR "[Touch] %s: Can not register touch input device.\n", __func__);
		goto err2;
	}
    
	// initial IRQ
    gpio_tlmm_config(GPIO_CFG(42, 0, GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
    
	bi041p_wq = create_singlethread_workqueue("bi041p_wq");
	INIT_WORK(&bi041p.wqueue, bi041p_isr_workqueue);
    
    if (request_irq(bi041p.client->irq, bi041p_isr, IRQF_TRIGGER_FALLING, "bi041p", &bi041p))
    {
        printk(KERN_ERR "[Touch] %s: Request IRQ failed.\n", __func__);
        goto err3;
    }
    
#ifdef CONFIG_HAS_EARLYSUSPEND
	bi041p.es.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	bi041p.es.suspend = bi041p_early_suspend;
	bi041p.es.resume = bi041p_late_resume;
	register_early_suspend(&bi041p.es);
#endif

err_chk:

    return 0;
	
err3:
	free_irq(bi041p.client->irq, &bi041p);
	input_unregister_device(bi041p.input);
err2:
    input_free_device(bi041p.input);
err1:
	dev_set_drvdata(&client->dev, NULL);
	
    printk(KERN_ERR "[Touch] %s: Failed.\n", __func__);
    return -1;
}

static int bi041p_remove(struct i2c_client * client)
{
	printk(KERN_INFO "[Touch] %s\n", __func__);
	destroy_workqueue(bi041p_wq);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&bi041p.es);
#endif

	input_unregister_device(bi041p.input);
    return 0;
}

static int bi041p_suspend(struct i2c_client *client, pm_message_t state)
{
    return 0;
}

static int bi041p_resume(struct i2c_client *client)
{
    return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bi041p_early_suspend(struct early_suspend *h)
{
	char cmd[4] = {0x54, 0x50, 0x00, 0x01};

    printk(KERN_INFO "[Touch] %s\n", __func__);
    
	bi041p_disable(1);
	bi041p_i2c_send(cmd, ARRAY_SIZE(cmd));
}

static void bi041p_late_resume(struct early_suspend *h)
{
	if (bi041p_reset())
        bi041p_disable(0);
}
#endif

static const struct i2c_device_id bi041p_id[] = {
	{ "bi041p", 0 },
	{ }
};

static struct i2c_driver bi041p_driver = {
	.id_table	= bi041p_id,
	.probe		= bi041p_probe,
	.remove		= bi041p_remove,
	.suspend	= bi041p_suspend,
	.resume		= bi041p_resume,
	.driver = {
		.name	= "bi041p",
	},
};

static int __init bi041p_init(void)
{
	return i2c_add_driver(&bi041p_driver);
}

static void __exit bi041p_exit(void)
{
	i2c_del_driver(&bi041p_driver);
}

module_init(bi041p_init);
module_exit(bi041p_exit);

MODULE_DESCRIPTION("ELAN BI041P-T02XB01U driver Heavily modified");
MODULE_AUTHOR("FIH Div2 SW2 BSP");
MODULE_LICENSE("GPL");

