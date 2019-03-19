/*
* Copyright 2019 Sipeed Co.,Ltd.

* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "sipeed_yolo2.h"
#include "sipeed_conf.h"

#include "w25qxx.h"

#include <mp.h>
#include "mpconfigboard.h"

#include "imlib.h"
#include "py_assert.h"
#include "py_helper.h"
#include "extmod/vfs.h"
#include "vfs_wrapper.h"
///////////////////////////////////////////////////////////////////////////////

#define _D  do{printf("%s --> %d\r\n",__func__,__LINE__);}while(0)

typedef struct py_kpu_net_obj
{
    mp_obj_base_t base;
    
    mp_obj_t        model_data;
    mp_obj_t        model_addr;
    mp_obj_t        model_size;
    mp_obj_t        model_path;
    mp_obj_t        kpu_task;
    mp_obj_t        net_args;
    mp_obj_t        net_deinit;
} __attribute__((aligned(8))) py_kpu_net_obj_t;

static int py_kpu_class_yolo2_print_to_buf(mp_obj_t self_in, char *buf);

static void py_kpu_net_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_kpu_net_obj_t *self = self_in;

    const char *path = NULL;
    if(MP_OBJ_IS_STR(self->model_path))
    {
        path = mp_obj_str_get_str(self->model_path);
    }

    uint32_t addr = 0;
    if(MP_OBJ_IS_INT(self->model_addr))
    {
        addr = mp_obj_get_int(self->model_addr);
    }

    const char net_args[512];

    if(py_kpu_class_yolo2_print_to_buf(self->net_args, net_args) != 0)
    {
        sprintf(net_args,"\"(null)\"");
    }
    
    mp_printf(print,
              "{\"model_data\": %d, \"model_addr\": %d, \"model_size\": %d, \"model_path\": \"%s\", \"net_args\": %s}",
                mp_obj_new_int(MP_OBJ_TO_PTR(((py_kpu_net_obj_t *)self_in)->model_data)),
                addr,
                mp_obj_get_int(self->model_size),
                path,
                net_args
                );
}

mp_obj_t py_kpu_net_model_data(mp_obj_t self_in) { return mp_obj_new_int(MP_OBJ_TO_PTR(((py_kpu_net_obj_t *)self_in)->model_data)); }
mp_obj_t py_kpu_net_model_addr(mp_obj_t self_in) { return ((py_kpu_net_obj_t *)self_in)->model_addr; }
mp_obj_t py_kpu_net_model_size(mp_obj_t self_in) { return ((py_kpu_net_obj_t *)self_in)->model_size; }
mp_obj_t py_kpu_net_model_path(mp_obj_t self_in) { return ((py_kpu_net_obj_t *)self_in)->model_path; }
mp_obj_t py_kpu_net_arg(mp_obj_t self_in)        { return ((py_kpu_net_obj_t *)self_in)->net_args; }

static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_net_model_data_obj,     py_kpu_net_model_data);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_net_model_addr_obj,     py_kpu_net_model_addr);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_net_model_size_obj,     py_kpu_net_model_size);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_net_model_path_obj,     py_kpu_net_model_path);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_net_arg_obj,            py_kpu_net_arg);

static const mp_rom_map_elem_t py_kpu_net_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_model_data),      MP_ROM_PTR(&py_kpu_net_model_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_model_addr),      MP_ROM_PTR(&py_kpu_net_model_addr_obj) },
    { MP_ROM_QSTR(MP_QSTR_model_size),      MP_ROM_PTR(&py_kpu_net_model_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_model_path),      MP_ROM_PTR(&py_kpu_net_model_path_obj) },
    { MP_ROM_QSTR(MP_QSTR_net_arg),         MP_ROM_PTR(&py_kpu_net_arg_obj) }
};

static MP_DEFINE_CONST_DICT(py_kpu_net_dict, py_kpu_net_dict_table);

static const mp_obj_type_t py_kpu_net_obj_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kpu_net,
    .print = py_kpu_net_obj_print,
    .locals_dict = (mp_obj_t) &py_kpu_net_dict
};

typedef struct {
                uint32_t magic_number;
                uint32_t layer_number;
                uint32_t layer_cfg_addr_offset;
                uint32_t eight_bit_mode;
                float scale;
                float bias;
            } model_config_t;


typedef struct {
                uint32_t reg_addr_offset;
                uint32_t act_addr_offset;
                uint32_t bn_addr_offset;
                uint32_t bn_len;
                uint32_t weights_addr_offset;
                uint32_t weights_len;
            } layer_config_t;


int model_init(kpu_task_t *task, const char *path)
{
    uint32_t layer_cfg_addr;
    model_config_t model_cfg;
    layer_config_t layer_cfg;
    kpu_layer_argument_t *layer_arg_ptr;
    void *ptr;
    mp_obj_t file;
    int ferr;

    file = vfs_internal_open(path,"rb", &ferr);
    if(file == MP_OBJ_NULL || ferr != 0)
        mp_raise_OSError(ferr);

    memset(task, 0, sizeof(kpu_task_t));
    //model_read(addr, (uint8_t *)&model_cfg, sizeof(model_config_t));
    vfs_internal_read(file, (uint8_t *)&model_cfg, sizeof(model_config_t), &ferr);
    if( ferr != 0)
        mp_raise_OSError(ferr);

    if (model_cfg.magic_number != 0x12345678)
        return -1;
    layer_arg_ptr = (kpu_layer_argument_t *)malloc(12 * 8 * model_cfg.layer_number);
    if (layer_arg_ptr == NULL)
        return -2;

    memset(layer_arg_ptr, 0, 12 * 8 * model_cfg.layer_number);
    task->layers = layer_arg_ptr;
    task->layers_length = model_cfg.layer_number;
    task->eight_bit_mode = model_cfg.eight_bit_mode;
    task->output_scale = model_cfg.scale;
    task->output_bias = model_cfg.bias;

    layer_cfg_addr =  model_cfg.layer_cfg_addr_offset;
    for (uint32_t i = 0; i < model_cfg.layer_number; i++)
    {
        // read layer config
        //model_read(layer_cfg_addr, (uint8_t *)&layer_cfg, sizeof(layer_config_t));
        vfs_internal_seek(file, layer_cfg_addr, VFS_SEEK_SET, &ferr);
        if(ferr != 0)
            mp_raise_OSError(ferr);

        vfs_internal_read(file, (uint8_t *)&layer_cfg, sizeof(layer_config_t), &ferr);
        if( ferr != 0)
            mp_raise_OSError(ferr);

        // read reg arg
        //model_read(addr + layer_cfg.reg_addr_offset, (uint8_t *)layer_arg_ptr, sizeof(kpu_layer_argument_t));
        vfs_internal_seek(file, layer_cfg.reg_addr_offset, VFS_SEEK_SET, &ferr);
        if(ferr != 0)
            mp_raise_OSError(ferr);

        vfs_internal_read(file, (uint8_t *)layer_arg_ptr, sizeof(kpu_layer_argument_t), &ferr);
        if( ferr != 0)
            mp_raise_OSError(ferr);



        // read act arg
        ptr = malloc(sizeof(kpu_activate_table_t));
        if (ptr == NULL)
            return -2;
        //model_read(addr + layer_cfg.act_addr_offset, (uint8_t *)ptr, sizeof(kpu_activate_table_t));
        
        vfs_internal_seek(file, layer_cfg.act_addr_offset, VFS_SEEK_SET, &ferr);
        if(ferr != 0)
            mp_raise_OSError(ferr);

        vfs_internal_read(file,  (uint8_t *)ptr, sizeof(kpu_activate_table_t), &ferr);
        if( ferr != 0)
            mp_raise_OSError(ferr);


        layer_arg_ptr->kernel_calc_type_cfg.data.active_addr = (uint32_t)ptr;
        // read bn arg
        ptr = malloc(layer_cfg.bn_len);
        if (ptr == NULL)
            return -2;
        //model_read(addr + layer_cfg.bn_addr_offset, (uint8_t *)ptr, layer_cfg.bn_len);
        vfs_internal_seek(file, layer_cfg.bn_addr_offset, VFS_SEEK_SET, &ferr);
        if(ferr != 0)
            mp_raise_OSError(ferr);

        vfs_internal_read(file,  (uint8_t *)ptr, layer_cfg.bn_len, &ferr);
        if( ferr != 0)
            mp_raise_OSError(ferr);

        layer_arg_ptr->kernel_pool_type_cfg.data.bwsx_base_addr = (uint32_t)ptr;
        // read weights arg
        ptr = malloc(layer_cfg.weights_len);
        if (ptr == NULL)
            return -2;
        //model_read(addr + layer_cfg.weights_addr_offset, (uint8_t *)ptr, layer_cfg.weights_len);
        vfs_internal_seek(file, layer_cfg.weights_addr_offset, VFS_SEEK_SET, &ferr);
        if(ferr != 0)
            mp_raise_OSError(ferr);

        vfs_internal_read(file, (uint8_t *)ptr, layer_cfg.weights_len, &ferr);
        if( ferr != 0)
            mp_raise_OSError(ferr);

        layer_arg_ptr->kernel_load_cfg.data.para_start_addr = (uint32_t)ptr;
        // next layer
        layer_cfg_addr += sizeof(layer_config_t);
        layer_arg_ptr++;
    }
    vfs_internal_close(file, &ferr);
    if( ferr != 0)
        mp_raise_OSError(ferr);
    return 0;
}

int model_deinit(kpu_task_t *task)
{
    for (uint32_t i = 0; i < task->layers_length; i++)
    {
        free(task->layers[i].kernel_calc_type_cfg.data.active_addr);
        free(task->layers[i].kernel_pool_type_cfg.data.bwsx_base_addr);
        free(task->layers[i].kernel_load_cfg.data.para_start_addr);
    }
    free(task->layers);
    return 0;
}

STATIC mp_obj_t py_kpu_class_load(uint n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    int err = 0;
    py_kpu_net_obj_t  *o = m_new_obj(py_kpu_net_obj_t);

    uint8_t *model_data = NULL;
    uint32_t model_size = 0;

    kpu_task_t *kpu_task = (kpu_task_t*)malloc(sizeof(kpu_task_t));
    if(kpu_task == NULL)
    {
        err = -1;
        goto error;
    }

    if(mp_obj_get_type(pos_args[0]) == &mp_type_int)
    {
        mp_int_t model_addr = mp_obj_get_int(pos_args[0]);

        if(model_addr < 0)
        {
            if(kpu_task)
                free(kpu_task);
            m_del(py_kpu_net_obj_t, o,sizeof(py_kpu_net_obj_t));
            mp_raise_ValueError("[MAIXPY]kpu: model_addr must > 0 ");
            return mp_const_false;
        }

        o->model_addr = mp_obj_new_int(model_addr);
        o->model_path = mp_const_none;

        uint8_t model_header[sizeof(kpu_model_header_t) + 1];

        w25qxx_status_t status = w25qxx_read_data_dma(model_addr, model_header, sizeof(kpu_model_header_t), W25QXX_QUAD_FAST);
        if(status != W25QXX_OK)
        {
            err = -2;//read error
            goto error;
        }

        model_size = kpu_model_get_size(model_header);

        if(model_size == -1)
        {
            err = -2;//read error
            goto error;
        }

        model_data = (uint8_t *)malloc(model_size * sizeof(uint8_t));
        if(model_data == NULL)
        {
            err = -1;//malloc error
            goto error;
        }

        status = w25qxx_read_data_dma(model_addr, model_data, model_size, W25QXX_QUAD_FAST);
        if(status != W25QXX_OK)
        {
            err = -2;//read error
            goto error;
        }
        int ret = kpu_model_load_from_buffer(kpu_task, model_data, NULL);
        if(ret != 0)
        {
            err = -3; //load error
            goto error;
        }
    }
    else if(mp_obj_get_type(pos_args[0]) == &mp_type_str)
    {
        const char *path = mp_obj_str_get_str(pos_args[0]);

        o->model_path = mp_obj_new_str(path,strlen(path));
        o->model_addr = mp_const_none;

        if(NULL != strstr(path,".bin"))
        {
            err=model_init(kpu_task,path);
            if( err != 0 )
            {
                model_deinit(kpu_task);
                goto error;
            }

        }
        else
        if(NULL != strstr(path,".kmodel"))
        {
     

            int ferr;
            mp_obj_t file;
            uint16_t tmp;
            uint8_t model_header[sizeof(kpu_model_header_t) + 1];

            file = vfs_internal_open(path,"rb", &ferr);
            if(file == MP_OBJ_NULL || ferr != 0)
                mp_raise_OSError(ferr);

            vfs_internal_read(file, model_header, sizeof(kpu_model_header_t), &ferr);
            if( ferr != 0)
                mp_raise_OSError(ferr);

            model_size = kpu_model_get_size(model_header);

            if(model_size == -1)
            {
                err = -2;//read error
                vfs_internal_close(file, &ferr);
                goto error;
            }

            model_data = (uint8_t *)malloc(model_size * sizeof(uint8_t));
            if(model_data == NULL)
            {
                err = -1;//malloc error
                goto error;
            }

            vfs_internal_seek(file, 0, VFS_SEEK_SET, &ferr);
            if(ferr != 0)
                mp_raise_OSError(ferr);

            vfs_internal_read(file, model_data, model_size, &ferr);
            if( ferr != 0)
                mp_raise_OSError(ferr);

            vfs_internal_close(file, &ferr);
            if( ferr != 0)
                mp_raise_OSError(ferr);

            int ret = kpu_model_load_from_buffer(kpu_task, model_data, NULL);
            if(ret != 0)
            {
                err = -3; //load error
                goto error;
            }

        }
        else
        {   
            m_del(py_kpu_net_obj_t, o,sizeof(py_kpu_net_obj_t));
            mp_raise_ValueError("[MAIXPY]kpu: model format don't match, only supply .bin or .kmodel ");
            return mp_const_false;
        }
        
    }
    else
    {
        m_del(py_kpu_net_obj_t, o,sizeof(py_kpu_net_obj_t));
        mp_raise_TypeError("[MAIXPY]kpu: only accept int and string");
        return mp_const_false;
    }

    

    o->base.type = &py_kpu_net_obj_type;
    o->net_args = mp_const_none;
    o->net_deinit = mp_const_none;

    o->kpu_task = MP_OBJ_FROM_PTR(kpu_task);
    o->model_data = MP_OBJ_FROM_PTR(model_data);
    o->model_size = mp_obj_new_int(model_size);

    return MP_OBJ_FROM_PTR(o);

error:
    if(kpu_task)
        free(kpu_task);

    if(model_data)
        free(model_data);

    m_del(py_kpu_net_obj_t, o,sizeof(py_kpu_net_obj_t));

    char msg[50];
    sprintf(msg,"[MAIXPY]kpu: load error %d", err);
    mp_raise_ValueError(msg);
    return mp_const_false;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_kpu_class_load_obj, 1, py_kpu_class_load);

///////////////////////////////////////////////////////////////////////////////

typedef struct py_kpu_class_yolo_args_obj {
    mp_obj_base_t base;

    mp_obj_t threshold, nms_value, anchor_number, anchor, rl_args;
} __attribute__((aligned(8))) py_kpu_class_yolo_args_obj_t;

typedef struct py_kpu_class_region_layer_arg
{
    float threshold;
    float nms_value;
    int anchor_number;
    float *anchor;
}__attribute__((aligned(8))) py_kpu_class_yolo_region_layer_arg_t;

static void py_kpu_class_yolo2_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_kpu_class_yolo_args_obj_t *yolo_args = self_in;

    py_kpu_class_yolo_region_layer_arg_t *rl_arg = yolo_args->rl_args;

    char msg[300];

    uint8_t num = rl_arg->anchor_number;

    if(num>0)
    {
        sprintf(msg,"%f",rl_arg->anchor[0]);
        for(uint16_t i = 1; i < num * 2; i++)
            sprintf(msg,"%s, %f",msg, rl_arg->anchor[i]);
    }

    mp_printf(print,
              "{\"threshold\":%f, \"nms_value\":%f, \"anchor_number\":%d, \"anchor\":\"(%s)\"}",
                rl_arg->threshold,
                rl_arg->nms_value,
                rl_arg->anchor_number,
                msg);
}

mp_obj_t py_kpu_calss_yolo2_anchor(mp_obj_t self_in);

mp_obj_t py_kpu_calss_yolo2_threshold(mp_obj_t self_in) { return ((py_kpu_class_yolo_args_obj_t *)self_in)->threshold; }
mp_obj_t py_kpu_calss_yolo2_nms_value(mp_obj_t self_in) { return ((py_kpu_class_yolo_args_obj_t *)self_in)->nms_value; }
mp_obj_t py_kpu_calss_yolo2_anchor_number(mp_obj_t self_in) { return ((py_kpu_class_yolo_args_obj_t *)self_in)->anchor_number; }

static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_calss_yolo2_anchor_obj,          py_kpu_calss_yolo2_anchor);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_calss_yolo2_threshold_obj,       py_kpu_calss_yolo2_threshold);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_calss_yolo2_nms_value_obj,       py_kpu_calss_yolo2_nms_value);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_calss_yolo2_anchor_number_obj,   py_kpu_calss_yolo2_anchor_number);

static const mp_rom_map_elem_t py_kpu_class_yolo2_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_anchor),              MP_ROM_PTR(&py_kpu_calss_yolo2_anchor_obj) },
    { MP_ROM_QSTR(MP_QSTR_threshold),           MP_ROM_PTR(&py_kpu_calss_yolo2_threshold_obj) },
    { MP_ROM_QSTR(MP_QSTR_nms_value),           MP_ROM_PTR(&py_kpu_calss_yolo2_nms_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_anchor_number),       MP_ROM_PTR(&py_kpu_calss_yolo2_anchor_number_obj) }
};

static MP_DEFINE_CONST_DICT(py_kpu_class_yolo2_dict, py_kpu_class_yolo2_dict_table);

static const mp_obj_type_t py_kpu_class_yolo_args_obj_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kpu_yolo2,
    .print = py_kpu_class_yolo2_print,
    // .subscr = py_kpu_calss_yolo2_subscr,
    .locals_dict = (mp_obj_t) &py_kpu_class_yolo2_dict
};

static int py_kpu_class_yolo2_print_to_buf(mp_obj_t self_in, char *buf)
{
    if(buf == NULL)
        return -1;

    if(mp_obj_get_type(self_in) == &py_kpu_class_yolo_args_obj_type)
    {
        py_kpu_class_yolo_args_obj_t *yolo_args = self_in;

        py_kpu_class_yolo_region_layer_arg_t *rl_arg = yolo_args->rl_args;

        char msg[300];

        uint8_t num = rl_arg->anchor_number;

        if(num>0)
        {
            sprintf(msg,"%f",rl_arg->anchor[0]);
            for(uint16_t i = 1; i < num * 2; i++)
                sprintf(msg,"%s, %f",msg, rl_arg->anchor[i]);
        }

        sprintf(buf,
                "{\"threshold\":%f, \"nms_value\":%f, \"anchor_number\":%d, \"anchor\":\"(%s)\"}",
                    rl_arg->threshold,
                    rl_arg->nms_value,
                    rl_arg->anchor_number,
                    msg);
        return 0;
    }
    return -1;
}

mp_obj_t py_kpu_calss_yolo2_anchor(mp_obj_t self_in)
{
    if(mp_obj_get_type(self_in) == &py_kpu_class_yolo_args_obj_type)
    {
        py_kpu_class_yolo_args_obj_t *yolo_args = MP_OBJ_TO_PTR(self_in);

        py_kpu_class_yolo_region_layer_arg_t *rl_arg = yolo_args->rl_args;

        mp_obj_t *tuple, *tmp;

        tmp = (mp_obj_t *)malloc(rl_arg->anchor_number * 2 * sizeof(mp_obj_t));

        for (uint8_t index = 0; index < rl_arg->anchor_number * 2; index++)
            tmp[index] = mp_obj_new_float(rl_arg->anchor[index]);

        tuple = mp_obj_new_tuple(rl_arg->anchor_number * 2, tmp);

        free(tmp);
        return tuple;
    }
    else
    {
        mp_raise_TypeError("[MAIXPY]kpu: object type error");
        return mp_const_false;
    }
}

mp_obj_t py_kpu_calss_yolo2_deinit(mp_obj_t self_in)
{
    if(mp_obj_get_type(self_in) == &py_kpu_class_yolo_args_obj_type)
    {
        py_kpu_class_yolo_args_obj_t *yolo_args = MP_OBJ_TO_PTR(self_in);

        py_kpu_class_yolo_region_layer_arg_t *rl_arg = yolo_args->rl_args;

        if(rl_arg->anchor)
            free(rl_arg->anchor);

        if(rl_arg)
            free(rl_arg);
        return mp_const_true;
    }
    else
    {
        mp_raise_TypeError("[MAIXPY]kpu: object type error");
        return mp_const_false;
    }
}

STATIC mp_obj_t py_kpu_class_init_yolo2(uint n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_kpu_net, ARG_threshold, ARG_nms_value, ARG_anchor_number, ARG_anchor};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kpu_net,              MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_threshold,            MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_nms_value,            MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_anchor_number,        MP_ARG_INT, {.u_int = 0x0}           },
        { MP_QSTR_anchor,               MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if(mp_obj_get_type(args[ARG_kpu_net].u_obj) == &py_kpu_net_obj_type)
    {
        float threshold, nms_value, *anchor = NULL;
        int anchor_number;

        threshold = mp_obj_get_float(args[ARG_threshold].u_obj);
        if(!(threshold >= 0.0 && threshold <= 1.0))
        {
            mp_raise_ValueError("[MAIXPY]kpu: threshold only support 0 to 1");
            return mp_const_false;
        }

        nms_value = mp_obj_get_float(args[ARG_nms_value].u_obj);
        if(!(nms_value >= 0.0 && nms_value <= 1.0))
        {
            mp_raise_ValueError("[MAIXPY]kpu: nms_value only support 0 to 1");
            return mp_const_false;
        }

        anchor_number = args[ARG_anchor_number].u_int;

        if(anchor_number > 0)
        {
            //need free
            anchor = (float*)malloc(anchor_number * 2 * sizeof(float));

            mp_obj_t *items;
            mp_obj_get_array_fixed_n(args[ARG_anchor].u_obj, args[ARG_anchor_number].u_int*2, &items);
        
            for(uint8_t index; index < args[ARG_anchor_number].u_int * 2; index++)
                anchor[index] = mp_obj_get_float(items[index]);
        }
        else
        {
            mp_raise_ValueError("[MAIXPY]kpu: anchor_number should > 0");
            return mp_const_false;
        }

        py_kpu_class_yolo_args_obj_t *yolo_args = m_new_obj(py_kpu_class_yolo_args_obj_t);

        yolo_args->base.type = &py_kpu_class_yolo_args_obj_type;

        yolo_args->threshold = mp_obj_new_float(threshold);
        yolo_args->nms_value = mp_obj_new_float(nms_value);
        yolo_args->anchor_number = mp_obj_new_int(anchor_number);

        mp_obj_t *tuple, *tmp;

        tmp = (mp_obj_t *)malloc(anchor_number * 2 * sizeof(mp_obj_t));

        for (uint8_t index = 0; index < anchor_number * 2; index++)
            tmp[index] = mp_obj_new_float(anchor[index]);

        tuple = mp_obj_new_tuple(anchor_number * 2, tmp);

        free(tmp);

        yolo_args->anchor = tuple;

        //need free
        py_kpu_class_yolo_region_layer_arg_t *rl_arg = malloc(sizeof(py_kpu_class_yolo_region_layer_arg_t));

        rl_arg->threshold = threshold;
        rl_arg->nms_value = nms_value;
        rl_arg->anchor_number = anchor_number;
        rl_arg->anchor = anchor;

        yolo_args->rl_args = MP_OBJ_FROM_PTR(rl_arg);

        py_kpu_net_obj_t *kpu_net = MP_OBJ_TO_PTR(args[ARG_kpu_net].u_obj);

        kpu_net->net_args = MP_OBJ_FROM_PTR(yolo_args);

        kpu_net->net_deinit = MP_OBJ_FROM_PTR(py_kpu_calss_yolo2_deinit);

        return mp_const_true;
    }
    else
    {
        mp_raise_TypeError("[MAIXPY]kpu: kpu_net type error");
        return mp_const_false;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_kpu_class_init_yolo2_obj, 5, py_kpu_class_init_yolo2);

///////////////////////////////////////////////////////////////////////////////

typedef struct py_kpu_class_yolo2_find_obj {
    mp_obj_base_t base;

    mp_obj_t x, y, w, h, classid, index, value, objnum;
} __attribute__((aligned(8))) py_kpu_class_yolo2_find_obj_t;

typedef struct py_kpu_class_list_link_data {
    rectangle_t rect;
    int classid;
    float value;
    int index;
    int objnum;
} __attribute__((aligned(8))) py_kpu_class_yolo2__list_link_data_t;

static void py_kpu_class_yolo2_find_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_kpu_class_yolo2_find_obj_t *self = self_in;
    mp_printf(print,
              "{\"x\":%d, \"y\":%d, \"w\":%d, \"h\":%d, \"value\":%f, \"classid\":%d, \"index\":%d, \"objnum\":%d}",
              mp_obj_get_int(self->x),
              mp_obj_get_int(self->y),
              mp_obj_get_int(self->w),
              mp_obj_get_int(self->h),
              (double)mp_obj_get_float(self->value),
              mp_obj_get_int(self->classid),
              mp_obj_get_int(self->index),
              mp_obj_get_int(self->objnum));
}

mp_obj_t py_kpu_class_yolo2_find_rect(mp_obj_t self_in)
{
    return mp_obj_new_tuple(4, (mp_obj_t[]){((py_kpu_class_yolo2_find_obj_t *)self_in)->x,
                                            ((py_kpu_class_yolo2_find_obj_t *)self_in)->y,
                                            ((py_kpu_class_yolo2_find_obj_t *)self_in)->w,
                                            ((py_kpu_class_yolo2_find_obj_t *)self_in)->h});
}

mp_obj_t py_kpu_class_yolo2_find_x(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->x; }
mp_obj_t py_kpu_class_yolo2_find_y(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->y; }
mp_obj_t py_kpu_class_yolo2_find_w(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->w; }
mp_obj_t py_kpu_class_yolo2_find_h(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->h; }
mp_obj_t py_kpu_class_yolo2_find_classid(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->classid; }
mp_obj_t py_kpu_class_yolo2_find_index(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->index; }
mp_obj_t py_kpu_class_yolo2_find_value(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->value; }
mp_obj_t py_kpu_class_yolo2_find_objnum(mp_obj_t self_in) { return ((py_kpu_class_yolo2_find_obj_t *)self_in)->objnum; }

static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_rect_obj, py_kpu_class_yolo2_find_rect);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_x_obj, py_kpu_class_yolo2_find_x);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_y_obj, py_kpu_class_yolo2_find_y);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_w_obj, py_kpu_class_yolo2_find_w);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_h_obj, py_kpu_class_yolo2_find_h);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_classid_obj, py_kpu_class_yolo2_find_classid);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_index_obj, py_kpu_class_yolo2_find_index);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_value_obj, py_kpu_class_yolo2_find_value);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_yolo2_find_objnum_obj, py_kpu_class_yolo2_find_objnum);

static const mp_rom_map_elem_t py_kpu_class_yolo2_find_type_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_rect),        MP_ROM_PTR(&py_kpu_class_yolo2_find_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_x),          MP_ROM_PTR(&py_kpu_class_yolo2_find_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_y),          MP_ROM_PTR(&py_kpu_class_yolo2_find_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_w),          MP_ROM_PTR(&py_kpu_class_yolo2_find_w_obj) },
    { MP_ROM_QSTR(MP_QSTR_h),          MP_ROM_PTR(&py_kpu_class_yolo2_find_h_obj) },
    { MP_ROM_QSTR(MP_QSTR_classid),     MP_ROM_PTR(&py_kpu_class_yolo2_find_classid_obj) },
    { MP_ROM_QSTR(MP_QSTR_index),       MP_ROM_PTR(&py_kpu_class_yolo2_find_index_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),       MP_ROM_PTR(&py_kpu_class_yolo2_find_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_objnum),      MP_ROM_PTR(&py_kpu_class_yolo2_find_objnum_obj) }
};

static MP_DEFINE_CONST_DICT(py_kpu_class_yolo2_find_type_locals_dict, py_kpu_class_yolo2_find_type_locals_dict_table);

static const mp_obj_type_t py_kpu_class_yolo2_find_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kpu_yolo2_find,
    .print = py_kpu_class_yolo2_find_print,
    // .subscr = py_kpu_class_subscr,
    .locals_dict = (mp_obj_t) &py_kpu_class_yolo2_find_type_locals_dict
};

volatile static uint8_t g_ai_done_flag = 0;

static void ai_done(void *ctx)
{
    g_ai_done_flag = 1;
}

STATIC mp_obj_t py_kpu_class_run_yolo2(uint n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    if(mp_obj_get_type(pos_args[0]) == &py_kpu_net_obj_type)
    {
        image_t *arg_img = py_image_cobj(pos_args[1]);
        PY_ASSERT_TRUE_MSG(IM_IS_MUTABLE(arg_img), "Image format is not supported.");

        if (arg_img->pix_ai == NULL)
        {
            mp_raise_ValueError("[MAIXPY]kpu: pix_ai or pixels is NULL!\n");
            return mp_const_false;
        }
        if(arg_img->w != 320 || arg_img->h != 240)
        {
            mp_raise_ValueError("[MAIXPY]kpu: img width or height error");
            return mp_const_false;
        }
        py_kpu_net_obj_t *kpu_net = MP_OBJ_TO_PTR(pos_args[0]);

        kpu_task_t *kpu_task = MP_OBJ_TO_PTR(kpu_net->kpu_task);
        
        g_ai_done_flag = 0;

        kpu_task->src = arg_img->pix_ai;
        kpu_task->dma_ch = K210_DMA_CH_KPU;
        kpu_task->callback = ai_done;
        kpu_single_task_init(kpu_task);

        py_kpu_class_yolo_args_obj_t *net_args = MP_OBJ_TO_PTR(kpu_net->net_args);
        py_kpu_class_yolo_region_layer_arg_t *rl_arg = net_args->rl_args;

        region_layer_t kpu_detect_rl;

        kpu_detect_rl.anchor_number = rl_arg->anchor_number;
        kpu_detect_rl.anchor = rl_arg->anchor;
        kpu_detect_rl.threshold = rl_arg->threshold;
        kpu_detect_rl.nms_value = rl_arg->nms_value;
        region_layer_init(&kpu_detect_rl, kpu_task);

        static obj_info_t mpy_kpu_detect_info;

        /* starat to calculate */
        kpu_start(kpu_task);
        while (!g_ai_done_flag)
            ;
        g_ai_done_flag = 0;
        /* start region layer */
        region_layer_run(&kpu_detect_rl, &mpy_kpu_detect_info);

        uint8_t obj_num = 0;
        obj_num = mpy_kpu_detect_info.obj_number;

        if (obj_num > 0)
        {
            list_t out;
            list_init(&out, sizeof(py_kpu_class_yolo2__list_link_data_t));

            for (uint8_t index = 0; index < obj_num; index++)
            {
                py_kpu_class_yolo2__list_link_data_t lnk_data;
                lnk_data.rect.x = mpy_kpu_detect_info.obj[index].x1;
                lnk_data.rect.y = mpy_kpu_detect_info.obj[index].y1;
                lnk_data.rect.w = mpy_kpu_detect_info.obj[index].x2 - mpy_kpu_detect_info.obj[index].x1;
                lnk_data.rect.h = mpy_kpu_detect_info.obj[index].y2 - mpy_kpu_detect_info.obj[index].y1;
                lnk_data.classid = mpy_kpu_detect_info.obj[index].classid;
                lnk_data.value = mpy_kpu_detect_info.obj[index].prob;

                lnk_data.index = index;
                lnk_data.objnum = obj_num;
                list_push_back(&out, &lnk_data);
            }

            mp_obj_list_t *objects_list = mp_obj_new_list(list_size(&out), NULL);

            for (size_t i = 0; list_size(&out); i++)
            {
                py_kpu_class_yolo2__list_link_data_t lnk_data;
                list_pop_front(&out, &lnk_data);

                py_kpu_class_yolo2_find_obj_t *o = m_new_obj(py_kpu_class_yolo2_find_obj_t);

                o->base.type = &py_kpu_class_yolo2_find_type;

                o->x = mp_obj_new_int(lnk_data.rect.x);
                o->y = mp_obj_new_int(lnk_data.rect.y);
                o->w = mp_obj_new_int(lnk_data.rect.w);
                o->h = mp_obj_new_int(lnk_data.rect.h);
                o->classid = mp_obj_new_int(lnk_data.classid);
                o->index = mp_obj_new_int(lnk_data.index);
                o->value = mp_obj_new_float(lnk_data.value);
                o->objnum = mp_obj_new_int(lnk_data.objnum);

                objects_list->items[i] = o;
            }
            kpu_single_task_deinit(kpu_task);
            region_layer_deinit(&kpu_detect_rl);

            return objects_list;
        }
        else
        {
            kpu_single_task_deinit(kpu_task);
            region_layer_deinit(&kpu_detect_rl);

            return mp_const_none;
        }
    }
    else
    {
        mp_raise_TypeError("[MAIXPY]kpu: kpu_net type error");
        return mp_const_false;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_kpu_class_run_yolo2_obj, 2, py_kpu_class_run_yolo2);

///////////////////////////////////////////////////////////////////////////////

typedef void (*call_net_arg_deinit)(mp_obj_t o);

void call_deinit(call_net_arg_deinit call_back, mp_obj_t o)
{
    call_back(o);
}

STATIC mp_obj_t py_kpu_deinit(uint n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    if(mp_obj_get_type(pos_args[0]) == &py_kpu_net_obj_type)
    {
        py_kpu_net_obj_t *kpu_net = MP_OBJ_TO_PTR(pos_args[0]);

        if(MP_OBJ_TO_PTR(kpu_net->model_data))
            free(MP_OBJ_TO_PTR(kpu_net->model_data));

        if(MP_OBJ_TO_PTR(kpu_net->kpu_task))
            free(MP_OBJ_TO_PTR(kpu_net->kpu_task));

        if(MP_OBJ_TO_PTR(kpu_net->net_deinit))
        {
            call_deinit(MP_OBJ_TO_PTR(kpu_net->net_deinit),kpu_net->net_args);
        }
        return mp_const_true;
    }
    else
    {
        mp_raise_TypeError("[MAIXPY]kpu: kpu_net type error");
        return mp_const_false;
    }

}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_kpu_deinit_obj, 1, py_kpu_deinit);
///////////////////////////////////////////////////////////////////////////////
/*forward
	单纯的网络前向运�?
	输入参数�?
		net结构体的obj（必须）
		是否使用softmax（可选）
		stride（可选，默认1�?
		计算到第几层（可选）：默认为计算完，可选计算到第n�?
			计算到第n层，即修改第n层的send_data_out�?，使能dma输出，方法为�?
			修改原自动生成的kpu_task_init，设置layers_length为n
		
	输出�?
		特征图obj
			即n个通道的m*n的图片，0~255灰度，可以使用color map映射为伪彩色
			m*n即为最后一个layer的输出尺�?
			last_layer->image_size.data.o_row_wid，o_col_high
			新建一个类型，直接存储特征图数组，
			对外提供转化某通道特征图到Image对象的方法，
			并且提供deinit方向释放特征图空间�?
*/



typedef struct fmap
{
	uint8_t* data;
	uint16_t index;
	uint16_t w;
	uint16_t h;
	uint16_t ch;
} __attribute__((aligned(8)))fmap_t;

typedef struct py_kpu_fmap_obj
{
    mp_obj_base_t base;
    fmap_t fmap;
} __attribute__((aligned(8))) py_kpu_fmap_obj_t;


static void py_kpu_fmap_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
	py_kpu_fmap_obj_t *fmap_obj = MP_OBJ_TO_PTR(self_in);
	fmap_t* fmap = &(fmap_obj->fmap);

    mp_printf(print,
        "{\"fmap\": data=%d, \"index\": %d, \"w\": %d, \"h\": %d, \"ch\": %d}",
        fmap->data, fmap->index, fmap->w, fmap->h, fmap->ch);

	return;
}

static const mp_obj_type_t py_kpu_fmap_obj_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kpu_fmap,
    .print = py_kpu_fmap_print,
    // .locals_dict = (mp_obj_t) &py_kpu_region_locals_dict
};



STATIC mp_obj_t py_kpu_forward(uint n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_kpu_net, ARG_img, ARG_len};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kpu_net,              MP_ARG_OBJ, {.u_obj = mp_const_none} },
		{ MP_QSTR_img,              	MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_len, 			       	MP_ARG_INT, {.u_int = 0x0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if(mp_obj_get_type(args[ARG_kpu_net].u_obj) == &py_kpu_net_obj_type)
    {
		py_kpu_net_obj_t *kpu_net = MP_OBJ_TO_PTR(args[ARG_kpu_net].u_obj);
		image_t *arg_img = py_image_cobj(args[ARG_img].u_obj);
		kpu_task_t *kpu_task = MP_OBJ_TO_PTR(kpu_net->kpu_task);  
		int layers_length = args[ARG_len].u_int;	//计算的层�?
		
        if (arg_img->pix_ai == NULL)
        {
            mp_raise_ValueError("[MAIXPY]kpu: pix_ai or pixels is NULL!\n");
            return mp_const_false;
        }
        if(arg_img->w != 320 || arg_img->h != 240)
        {
            mp_raise_ValueError("[MAIXPY]kpu: img width or height error");
            return mp_const_false;
        }

        g_ai_done_flag = 0;
        kpu_task->src = arg_img->pix_ai;    //need judge is not null
        kpu_task->dma_ch = K210_DMA_CH_KPU;
        kpu_task->callback = ai_done;
		if(layers_length > 0)
		{	//设置计算的层数，注意在kpu_single_task_init中会自动使能第len层的dma输出
			kpu_task->layers_length = layers_length;
		}
		kpu_layer_argument_t *last_layer = &kpu_task->layers[kpu_task->layers_length - 1];
        kpu_single_task_init(kpu_task);
        /* starat to calculate */
        kpu_start(kpu_task);
        while (!g_ai_done_flag)
            ;
        g_ai_done_flag = 0;

        py_kpu_fmap_obj_t  *o = m_new_obj(py_kpu_fmap_obj_t);
		o->base.type = &py_kpu_fmap_obj_type;
		fmap_t* fmap = &(o->fmap);
		fmap->data = kpu_task->dst;
		fmap->index = kpu_task->layers_length;
		fmap->w = last_layer->image_size.data.o_row_wid + 1;;
		fmap->h = last_layer->image_size.data.o_col_high + 1;
		fmap->ch = last_layer->image_channel_num.data.o_ch_num + 1;
		return MP_OBJ_FROM_PTR(o);
		//kpu_single_task_deinit	//最后需要释�?
	}

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_kpu_forward_obj, 1, py_kpu_forward);

//生成第ch通道的特征图Image_t格式
/*typedef struct image {
    int w;
    int h;
    int bpp;
    union {
        uint8_t *pixels;
        uint8_t *data;
    };
	uint8_t *pix_ai;	//for MAIX AI speed up
} __attribute__((aligned(8)))image_t; 
*/

STATIC mp_obj_t py_kpu_fmap(mp_obj_t fmap_obj, mp_obj_t ch_obj)
{
	int ch = mp_obj_get_int(ch_obj);
	fmap_t* fmap = &(((py_kpu_fmap_obj_t*)fmap_obj)->fmap);
	if(ch<0 || ch>= (fmap->ch)) 
	{
        char str_ret[30];

        sprintf(str_ret,"[MAIXPY]kpu: channel out of range! input 0~%d", fmap->ch);
        mp_raise_ValueError(str_ret);

		return mp_const_none;
	}
	
	mp_obj_t image = py_image(fmap->w, fmap->h, IMAGE_BPP_GRAYSCALE, fmap->data + ((fmap->w)*(fmap->h))*ch);
	return image;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_kpu_fmap_obj, py_kpu_fmap);

STATIC mp_obj_t py_kpu_fmap_free(mp_obj_t fmap_obj)
{
	fmap_t* fmap = &(((py_kpu_fmap_obj_t*)fmap_obj)->fmap);
	free(fmap->data);
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_fmap_free_obj, py_kpu_fmap_free);



typedef struct py_kpu_netinfo_list_data {
    uint16_t index;
    uint16_t wi;
    uint16_t hi;
    uint16_t wo;
    uint16_t ho;
    uint16_t chi;
	uint16_t cho;
	uint16_t dw;
	uint16_t kernel_type;
	uint16_t pool_type;
	uint32_t para_size;
} __attribute__((aligned(8))) py_kpu_netinfo_list_data_t;

typedef struct py_kpu_class_netinfo_find_obj {
    mp_obj_base_t base;

    mp_obj_t index,wi,hi,wo,ho,chi,cho,dw,kernel_type,pool_type,para_size;
} __attribute__((aligned(8))) py_kpu_class_netinfo_find_obj_t;

static void py_kpu_class_netinfo_find_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_kpu_class_netinfo_find_obj_t *self = self_in;
    mp_printf(print,
              "{\"index\":%d, \"wi\":%d, \"hi\":%d, \"wo\":%d, \"ho\":%d, \"chi\":%d, \"cho\":%d, \"dw\":%d, \"kernel_type\":%d, \"pool_type\":%d, \"para_size\":%d}",
              mp_obj_get_int(self->index),
              mp_obj_get_int(self->wi),
              mp_obj_get_int(self->hi),
              mp_obj_get_int(self->wo),
              mp_obj_get_int(self->ho),
              mp_obj_get_int(self->chi),
              mp_obj_get_int(self->cho),
              mp_obj_get_int(self->dw),
			  mp_obj_get_int(self->kernel_type),
			  mp_obj_get_int(self->pool_type),
			  mp_obj_get_int(self->para_size));
}

mp_obj_t py_kpu_class_netinfo_find_index (mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->index; }
mp_obj_t py_kpu_class_netinfo_find_wi(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->wi; }
mp_obj_t py_kpu_class_netinfo_find_hi(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->hi; }
mp_obj_t py_kpu_class_netinfo_find_wo(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->wo; }
mp_obj_t py_kpu_class_netinfo_find_ho(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->ho; }
mp_obj_t py_kpu_class_netinfo_find_chi(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->chi; }
mp_obj_t py_kpu_class_netinfo_find_cho(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->cho; }
mp_obj_t py_kpu_class_netinfo_find_dw(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->dw; }
mp_obj_t py_kpu_class_netinfo_find_kernel_type(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->kernel_type; }
mp_obj_t py_kpu_class_netinfo_find_pool_type(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->pool_type; }
mp_obj_t py_kpu_class_netinfo_find_para_size(mp_obj_t self_in) { return ((py_kpu_class_netinfo_find_obj_t *)self_in)->para_size; }

static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_index_obj, py_kpu_class_netinfo_find_index);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_wi_obj, py_kpu_class_netinfo_find_wi);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_hi_obj, py_kpu_class_netinfo_find_hi);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_wo_obj, py_kpu_class_netinfo_find_wo);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_ho_obj, py_kpu_class_netinfo_find_ho);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_chi_obj, py_kpu_class_netinfo_find_chi);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_cho_obj, py_kpu_class_netinfo_find_cho);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_dw_obj, py_kpu_class_netinfo_find_dw);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_kernel_type_obj, py_kpu_class_netinfo_find_kernel_type);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_pool_type_obj, py_kpu_class_netinfo_find_pool_type);
static MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_class_netinfo_find_para_size_obj, py_kpu_class_netinfo_find_para_size);

static const mp_rom_map_elem_t py_kpu_class_netinfo_find_type_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_index),       MP_ROM_PTR(&py_kpu_class_netinfo_find_index_obj) },
    { MP_ROM_QSTR(MP_QSTR_wi),          MP_ROM_PTR(&py_kpu_class_netinfo_find_wi_obj) },
    { MP_ROM_QSTR(MP_QSTR_hi),          MP_ROM_PTR(&py_kpu_class_netinfo_find_hi_obj) },
    { MP_ROM_QSTR(MP_QSTR_wo),          MP_ROM_PTR(&py_kpu_class_netinfo_find_wo_obj) },
    { MP_ROM_QSTR(MP_QSTR_ho),          MP_ROM_PTR(&py_kpu_class_netinfo_find_ho_obj) },
    { MP_ROM_QSTR(MP_QSTR_chi),     	MP_ROM_PTR(&py_kpu_class_netinfo_find_chi_obj) },
    { MP_ROM_QSTR(MP_QSTR_cho),       	MP_ROM_PTR(&py_kpu_class_netinfo_find_cho_obj) },
    { MP_ROM_QSTR(MP_QSTR_dw),       	MP_ROM_PTR(&py_kpu_class_netinfo_find_dw_obj) },
    { MP_ROM_QSTR(MP_QSTR_kernel_type),	MP_ROM_PTR(&py_kpu_class_netinfo_find_kernel_type_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pool_type),   MP_ROM_PTR(&py_kpu_class_netinfo_find_pool_type_obj) },
	{ MP_ROM_QSTR(MP_QSTR_para_size),   MP_ROM_PTR(&py_kpu_class_netinfo_find_para_size_obj) },
};

static MP_DEFINE_CONST_DICT(py_kpu_class_netinfo_find_type_locals_dict, py_kpu_class_netinfo_find_type_locals_dict_table);

static const mp_obj_type_t py_kpu_class_netinfo_find_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kpu_netinfo_find,
    .print = py_kpu_class_netinfo_find_print,
    // .subscr = py_kpu_class_subscr,
    .locals_dict = (mp_obj_t) &py_kpu_class_netinfo_find_type_locals_dict
};


STATIC mp_obj_t py_kpu_netinfo(mp_obj_t py_kpu_net_obj)
{
	py_kpu_net_obj_t *kpu_net = MP_OBJ_TO_PTR(py_kpu_net_obj);
	kpu_task_t *kpu_task = MP_OBJ_TO_PTR(kpu_net->kpu_task);  
	
	int len = kpu_task->layers_length;
	kpu_layer_argument_t *layer; 
	list_t out;
	list_init(&out, sizeof(py_kpu_netinfo_list_data_t));

	for (uint8_t index = 0; index < len; index++)
	{
		py_kpu_netinfo_list_data_t data;
		layer = &kpu_task->layers[index];
		data.index = index;
		data.wi = layer->image_size.data.i_row_wid+1;
		data.hi = layer->image_size.data.i_col_high+1;
		data.wo = layer->image_size.data.o_row_wid+1;
		data.ho = layer->image_size.data.o_col_high+1;
		data.chi = layer->image_channel_num.data.i_ch_num+1;
		data.cho = layer->image_channel_num.data.o_ch_num+1;
		data.dw = layer->interrupt_enabe.data.depth_wise_layer;
		data.kernel_type = layer->kernel_pool_type_cfg.data.kernel_type;
		data.pool_type = layer->kernel_pool_type_cfg.data.pool_type;
		data.para_size = layer->kernel_load_cfg.data.para_size;
		list_push_back(&out, &data);
	}

	mp_obj_list_t *objects_list = mp_obj_new_list(list_size(&out), NULL);

	for (size_t i = 0; list_size(&out); i++)
	{
		py_kpu_netinfo_list_data_t lnk_data;
		list_pop_front(&out, &lnk_data);

		py_kpu_class_netinfo_find_obj_t *o = m_new_obj(py_kpu_class_netinfo_find_obj_t);

		o->base.type = &py_kpu_class_netinfo_find_type;

		o->index = mp_obj_new_int(lnk_data.index);
		o->wi = mp_obj_new_int(lnk_data.wi);
		o->hi = mp_obj_new_int(lnk_data.hi);
		o->wo = mp_obj_new_int(lnk_data.wo);
		o->ho = mp_obj_new_int(lnk_data.ho);
		o->chi = mp_obj_new_int(lnk_data.chi);
		o->cho= mp_obj_new_int(lnk_data.cho);
		o->dw = mp_obj_new_int(lnk_data.dw);
		o->kernel_type = mp_obj_new_int(lnk_data.kernel_type);
		o->pool_type = mp_obj_new_int(lnk_data.pool_type);
		o->para_size = mp_obj_new_int(lnk_data.para_size);

		objects_list->items[i] = o;
	}

	return objects_list;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_kpu_netinfo_obj, py_kpu_netinfo);


///////////////////////////////////////////////////////////////////////////////

static const mp_map_elem_t globals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                    MP_OBJ_NEW_QSTR(MP_QSTR_kpu) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_load),                        (mp_obj_t)&py_kpu_class_load_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init_yolo2),                  (mp_obj_t)&py_kpu_class_init_yolo2_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_yolo2),                   (mp_obj_t)&py_kpu_class_run_yolo2_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),                      (mp_obj_t)&py_kpu_deinit_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_forward),                     (mp_obj_t)&py_kpu_forward_obj },
	{ MP_OBJ_NEW_QSTR(MP_QSTR_fmap),                      	(mp_obj_t)&py_kpu_fmap_obj },
	{ MP_OBJ_NEW_QSTR(MP_QSTR_fmap_free),                   (mp_obj_t)&py_kpu_fmap_free_obj },
	{ MP_OBJ_NEW_QSTR(MP_QSTR_netinfo),                   	(mp_obj_t)&py_kpu_netinfo_obj },
    { NULL, NULL },
};

STATIC MP_DEFINE_CONST_DICT(globals_dict, globals_dict_table);

const mp_obj_module_t kpu_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_t)&globals_dict,
};
