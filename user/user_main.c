/*
 *  Copyright 2016 HomeACcessoryKid - HacK - homeaccessorykid@gmail.com
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * ESPRSSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*****************************************************************************************
 * HomeACcessoryKid hkc based ESP8266 accessory.
 *
 * This accessory includes a garage door and a normal door services.
 *
 *****************************************************************************************/

#include "esp_common.h"
#include "hkc.h"
#include "gpio.h"
#include "uart.h"
#include "queue.h"

xQueueHandle mgmtQueue;

#define GARAGEDOOR_OPENED   0
#define GARAGEDOOR_CLOSED   1
#define DOOR_FULLY_CLOSED   0
#define DOOR_FULLY_OPENED   100

#define MS_2_TICKS(ms)  ((ms) / portTICK_RATE_MS)

typedef struct
{
    const int closed_val;
    const int opened_val;
    const int gpio;
    int curr_state;
    int new_state;
    int aid;
    int tgt_iid;
    int curr_iid;
    struct {
        os_timer_t  t;
        bool        active;
    }timer_newstate, timer_close;
} door_info_t;

door_info_t garage_info = {
    .gpio       = GPIO_Pin_4,
    .closed_val = GARAGEDOOR_CLOSED,
    .opened_val = GARAGEDOOR_OPENED
};

door_info_t door_info = {
    .gpio       = GPIO_Pin_12,
    .closed_val = DOOR_FULLY_CLOSED,
    .opened_val = DOOR_FULLY_OPENED
};


#define LOGHEADER       "@@@ "


static void newstate_callback(void *arg)
{
    cJSON *value;
    door_info_t *pdoor = (door_info_t *)arg;

    pdoor->curr_state = pdoor->new_state;

    value = cJSON_CreateNumber(pdoor->curr_state);

    change_value(pdoor->aid, pdoor->curr_iid, value);
    send_events(NULL, pdoor->aid, pdoor->curr_iid);

    pdoor->timer_newstate.active = false;

    // set the close back timer
    if (pdoor->new_state == pdoor->opened_val && !pdoor->timer_close.active)
    {
        pdoor->timer_close.active = true;
        os_timer_arm(&(pdoor->timer_close.t), 1000, false);
    }
}

static void close_callback(void *arg)
{
    cJSON *value;
    door_info_t *pdoor = (door_info_t *)arg;

    pdoor->new_state = pdoor->closed_val;

    value = cJSON_CreateNumber(pdoor->new_state);
    //change tgt to closed
    change_value(pdoor->aid, pdoor->tgt_iid, value);
    send_events(NULL, pdoor->aid, pdoor->tgt_iid);

    pdoor->timer_close.active = false;

#if 1
    if (!pdoor->timer_newstate.active)
    {
        pdoor->timer_newstate.active = true;
        os_timer_arm(&(pdoor->timer_newstate.t), 500, false);
    }
#endif
}


void read_cb(int aid, int iid, cJSON *value, int mode)
{

    if (!value) return;

    os_printf(LOGHEADER "%s aid=%d iid=%d mode=%d value->type=%u value->valueint=%u\n", __FUNCTION__, aid, iid, mode, value->type, value->valueint);

    switch (mode)
    {
        case 0:
            os_printf(LOGHEADER "initializing....\n");
            break;

        case 1:  //changed by gui
        {
            os_printf(LOGHEADER "request for write....\n");
            break;
        }

        case 2: //update
            os_printf(LOGHEADER "request for refresh/read....\n");
            //do nothing
            break;

        default:
            //print an error?
            break;
    }
}


void write_cb(int aid, int iid, cJSON *value, int mode)
{
    door_info_t *pdoor;

    if (!value) return;

    pdoor = (iid == garage_info.tgt_iid) ? &garage_info : &door_info;

    os_printf(LOGHEADER "%s aid=%d iid=%d mode=%d value->type=%u value->valueint=%u\n", __FUNCTION__, aid, iid, mode, value->type, value->valueint);

    switch (mode)
    {
        case 0:
        {
            GPIO_ConfigTypeDef gpio_cfg;
            os_printf(LOGHEADER "initializing GPIO....\n");
            gpio_cfg.GPIO_IntrType = GPIO_PIN_INTR_DISABLE;         //no interrupt
            gpio_cfg.GPIO_Mode     = GPIO_Mode_Output;              //Output mode
            gpio_cfg.GPIO_Pullup   = GPIO_PullUp_DIS;                //improves transitions
            gpio_cfg.GPIO_Pin      = pdoor->gpio;
            gpio_config(&gpio_cfg);                                 //Initialization function

            pdoor->curr_state = pdoor->closed_val;
            pdoor->new_state  = pdoor->closed_val;

            GPIO_OUTPUT(pdoor->gpio, 1);
            break;
        }

        case 1:  //changed by gui
        {
            char *out; out=cJSON_Print(value);  os_printf(LOGHEADER "%s rcvd value:%s\n", __FUNCTION__, out);  free(out);  // Print to text, print it, release the string.
            if (!pdoor->timer_newstate.active)
            {
                pdoor->new_state = value->valueint;
                xQueueSend(mgmtQueue, &pdoor, 0);
            }
            break;
        }

        case 2: //update
            os_printf(LOGHEADER "request for refresh/read....\n");
            break;

        default:
            //print an error?
            break;
    }
}


void mgmt_task(void *arg)
{
    os_printf(LOGHEADER "mgmt task started\n");

    while(1)
    {
        void *qItem[1] = {NULL};

        while(!xQueueReceive(mgmtQueue, qItem, portMAX_DELAY));

        if (*qItem == NULL)
        {
            //identify
            int i;

            os_printf("Identifying...\n");

            for (i = 0; i < 8; i++)
            {
                GPIO_OUTPUT(GPIO_Pin_2,  GPIO_INPUT(GPIO_Pin_2) ^ 1);
                vTaskDelay(MS_2_TICKS(50));
            }
        }
        else
        {
            door_info_t *pdoor = (door_info_t *)*qItem;

            pdoor->timer_newstate.active = true;

            // generate a pulse
            if (pdoor->new_state == pdoor->opened_val)
            {
                GPIO_OUTPUT(pdoor->gpio, 0);
                vTaskDelay(MS_2_TICKS(500));
            }

            GPIO_OUTPUT(pdoor->gpio, 1);

            os_timer_arm(&(pdoor->timer_newstate.t), 500, false);
        }
    }
}


void identify(int aid, int iid, cJSON *value, int mode)
{
    switch (mode) {
        case 1:
        { //changed by gui
            xQueueSend(mgmtQueue, NULL, 0);
        }break;

        case 0:
        { //init
            mgmtQueue = xQueueCreate(4, sizeof(door_info_t *));
            xTaskCreate(mgmt_task, "mgmt", 256, NULL, 2, NULL);
        }break;

        case 2: { //update
            //do nothing
        }break;

        default: {
            //print an error?
        }break;
    }
}

extern  cJSON       *root;
void    hkc_user_init(char *accname)
{
    //do your init thing beyond the bear minimum
    //avoid doing it in user_init else no heap left for pairing
    cJSON *accs,*sers,*chas,*value;
    int aid=0,iid=0;

    accs=initAccessories();

    //////////////////////////////////////////
    //
    sers=addAccessory(accs,++aid);
    garage_info.aid = aid;
    door_info.aid   = aid;

    //service 0 describes the accessory
    chas=addService(      sers,++iid,APPLE,ACCESSORY_INFORMATION_S);
    addCharacteristic(chas,aid,++iid,APPLE,NAME_C,accname,NULL);
    addCharacteristic(chas,aid,++iid,APPLE,MANUFACTURER_C,"HacK",NULL);
    addCharacteristic(chas,aid,++iid,APPLE,MODEL_C,"Rev-1",NULL);
    addCharacteristic(chas,aid,++iid,APPLE,SERIAL_NUMBER_C,"1",NULL);
    addCharacteristic(chas,aid,++iid,APPLE,IDENTIFY_C,NULL,identify);

    //service 1
    chas=addService(       sers, ++iid, APPLE, GARAGE_DOOR_OPENER_S);
    addCharacteristic(chas, aid, ++iid, APPLE, NAME_C,             "cancellone", NULL);
    addCharacteristic(chas, aid, ++iid, APPLE, TARGET_DOORSTATE_C, "1",     write_cb);
    garage_info.tgt_iid = iid;

    addCharacteristic(chas, aid, ++iid, APPLE, CURRENT_DOOR_STATE_C,   "1", read_cb);
    garage_info.curr_iid = iid;
    addCharacteristic(chas, aid, ++iid, APPLE, OBSTRUCTION_DETECTED_C, "0", NULL);
    /**/
    ///////////////////////////////////////////

    chas=addService(       sers, ++iid, APPLE, DOOR_S);
    addCharacteristic(chas, aid, ++iid, APPLE, NAME_C,             "cancellino", NULL);
    addCharacteristic(chas, aid, ++iid, APPLE, TARGET_POSITION_C,  "0",          write_cb);
    door_info.tgt_iid = iid;
    addCharacteristic(chas, aid, ++iid, APPLE, CURRENT_POSITION_C, "0", read_cb);
    door_info.curr_iid = iid;
    addCharacteristic(chas, aid, ++iid, APPLE, POSITION_STATE_C,   "2", NULL);
    addCharacteristic(chas, aid, ++iid, APPLE, OBSTRUCTION_DETECTED_C, "0", NULL);

    char *out;
    out=cJSON_Print(root);  os_printf("%s\n",out);  free(out);  // Print to text, print it, release the string.

//  for (iid=1;iid<MAXITM+1;iid++) {
//      out=cJSON_Print(acc_items[iid].json);
//      os_printf("1.%d=%s\n",iid,out); free(out);
//  }

    gpio_intr_handler_register(gpio_intr_handler,NULL);         //Register the interrupt function
    GPIO_INTERRUPT_ENABLE;
}

/******************************************************************************
 * FunctionName : user_init
 * Description  : entry of user application, init user function here
 * Parameters   : none
 * Returns      : none
*******************************************************************************/
void user_init(void)
{
    uart_init_new();
    os_printf(LOGHEADER "start of user_init @ %d\n",system_get_time()/1000);

//use this block only once to set your favorite access point or put your own selection routine
/*    wifi_set_opmode(STATION_MODE);
    struct station_config *sconfig = (struct station_config *)zalloc(sizeof(struct station_config));
    sprintf(sconfig->ssid, "secret-ssid"); //don't forget to set this if you use it
    sprintf(sconfig->password, "secret-passwd"); //don't forget to set this if you use it
    wifi_station_set_config(sconfig);
    free(sconfig);
    wifi_station_connect(); /**/

#if 0
    if (wifi_get_sleep_type() != NONE_SLEEP_T) wifi_set_sleep_type(NONE_SLEEP_T);
#endif

    //try to only do the bare minimum here and do the rest in hkc_user_init
    // if not you could easily run out of stack space during pairing-setup
    hkc_init("esp-cancel");

    garage_info.timer_newstate.active = false;
    door_info.timer_newstate.active   = false;
    garage_info.timer_close.active = false;
    door_info.timer_close.active   = false;

    os_timer_setfn(&(garage_info.timer_newstate.t), newstate_callback, &garage_info);
    os_timer_setfn(&(door_info.timer_newstate.t),   newstate_callback, &door_info);
    os_timer_setfn(&(garage_info.timer_close.t),    close_callback, &garage_info);
    os_timer_setfn(&(door_info.timer_close.t),      close_callback, &door_info);

    os_printf(LOGHEADER "end of user_init @ %d\n",system_get_time()/1000);
}

/***********************************************************************************
 * FunctionName : user_rf_cal_sector_set forced upon us by espressif since RTOS1.4.2
 * Description  : SDK just reversed 4 sectors, used for rf init data and paramters.
 *                We add this function to force users to set rf cal sector, since
 *                we don't know which sector is free in user's application.
 *                sector map for last several sectors : ABCCC
 *                A : rf cal    B : rf init data    C : sdk parameters
 * Parameters   : none
 * Returns      : rf cal sector
***********************************************************************************/
uint32 user_rf_cal_sector_set(void) {
    extern char flashchip;
    SpiFlashChip *flash = (SpiFlashChip*)(&flashchip + 4);
    // We know that sector size is 4096
    //uint32_t sec_num = flash->chip_size / flash->sector_size;
    uint32_t sec_num = flash->chip_size >> 12;
    return sec_num - 5;
}
