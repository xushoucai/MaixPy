#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include <stdio.h>
#include "modmachine.h"
#include "sysctl.h"

#if MICROPY_PY_MACHINE


STATIC mp_obj_t machine_reset()
{
    sipeed_sys_reset();
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(machine_reset_obj, machine_reset);

STATIC mp_obj_t machine_reset_cause()
{
    sysctl_reset_enum_status_t cause = sysctl_get_reset_status();
	return mp_obj_new_int((mp_int_t)cause);
}
MP_DEFINE_CONST_FUN_OBJ_0(machine_reset_cause_obj, machine_reset_cause);

STATIC const mp_map_elem_t machine_module_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_machine) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_ROM_PTR(&machine_uart_type) },
    { MP_ROM_QSTR(MP_QSTR_Timer), MP_ROM_PTR(&machine_timer_type) },
    { MP_ROM_QSTR(MP_QSTR_PWM),  MP_ROM_PTR(&machine_pwm_type) },
    { MP_ROM_QSTR(MP_QSTR_I2C),  MP_ROM_PTR(&machine_hard_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_SPI),  MP_ROM_PTR(&machine_hw_spi_type) },
    { MP_ROM_QSTR(MP_QSTR_reset),  MP_ROM_PTR(&machine_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_cause),  MP_ROM_PTR(&machine_reset_cause_obj) },
    { MP_ROM_QSTR(MP_QSTR_PWRON_RESET),  MP_ROM_INT(SYSCTL_RESET_STATUS_HARD) },
    { MP_ROM_QSTR(MP_QSTR_HARD_RESET),  MP_ROM_INT(SYSCTL_RESET_STATUS_HARD) },
    { MP_ROM_QSTR(MP_QSTR_WDT_RESET),  MP_ROM_INT(SYSCTL_RESET_STATUS_WDT0) },
    { MP_ROM_QSTR(MP_QSTR_WDT1_RESET),  MP_ROM_INT(SYSCTL_RESET_STATUS_WDT1) },
    { MP_ROM_QSTR(MP_QSTR_SOFT_RESET),  MP_ROM_INT(SYSCTL_RESET_STATUS_SOFT) },
};

STATIC MP_DEFINE_CONST_DICT (
    machine_module_globals,
    machine_module_globals_table
);

const mp_obj_module_t machine_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&machine_module_globals,
};

#endif // MICROPY_PY_MACHINE
