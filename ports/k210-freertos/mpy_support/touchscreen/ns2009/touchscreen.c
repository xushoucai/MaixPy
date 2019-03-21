


#include "touchscreen.h"
#include "machine_i2c.h"
#include "errno.h"
#include "ns2009.h"

static struct ts_ns2009_pdata_t *ts_ns2009_pdata;
i2c_device_number_t m_i2c_num = 0;
static bool is_init = false;

int ns2009_hal_i2c_init_default();

int touchscreen_init( void* arg)
{
    int err;
    touchscreen_config_t* config = (touchscreen_config_t*)arg;
    if( config->i2c != NULL)
    {
        m_i2c_num = config->i2c->i2c;
    }
    else
    {
        ns2009_hal_i2c_init_default();
    }
    ts_ns2009_pdata = ts_ns2009_probe(config->calibration, &err);
    if (ts_ns2009_pdata == NULL || err!=0)
        return err;
    is_init = true;
    return 0;
}
bool touchscreen_is_init()
{
    return is_init;
}

int touchscreen_read( int* type, int* x, int* y)
{
    ts_ns2009_poll(ts_ns2009_pdata);
    *x = 0;
    *y = 0;
    switch (ts_ns2009_pdata->event->type)
    {
        case TOUCH_BEGIN:
            *x = ts_ns2009_pdata->event->x;
            *y = ts_ns2009_pdata->event->y;
            *type = TOUCHSCREEN_STATUS_PRESS;
            break;

        case TOUCH_MOVE:
            *x = ts_ns2009_pdata->event->x;
            *y = ts_ns2009_pdata->event->y;
            *type = TOUCHSCREEN_STATUS_MOVE;
            break;

        case TOUCH_END:
            *x= ts_ns2009_pdata->event->x;
            *y= ts_ns2009_pdata->event->y;
            *type = TOUCHSCREEN_STATUS_RELEASE;
            break;
        default:
            break;
    }
    return 0;
}

int touchscreen_deinit()
{
    ts_ns2009_remove(ts_ns2009_pdata);
    is_init = false;
    return 0;
}

int touchscreen_calibrate(int w, int h, int* cal)
{
    return do_tscal(ts_ns2009_pdata, w, h, cal);
}

//////////// HAL ////////////

int ns2009_hal_i2c_init_default()
{
    m_i2c_num = 0; // default i2c 0
    fpioa_set_function(30, FUNC_I2C0_SCLK +  m_i2c_num* 2);
    fpioa_set_function(31, FUNC_I2C0_SDA + m_i2c_num * 2);
    maix_i2c_init(m_i2c_num, 7, 400000);
}

int ns2009_hal_i2c_recv(const uint8_t *send_buf, size_t send_buf_len, uint8_t *receive_buf,
                  size_t receive_buf_len)
{
    return maix_i2c_recv_data(m_i2c_num, NS2009_SLV_ADDR, send_buf, send_buf_len, receive_buf, receive_buf_len);
}



