/*
 *                                                                                                  geany_encoding=koi8-r
 * hardware.h
 *
 * Copyright 2018 Edward V. Emelianov <eddy@sao.ru, edward.emelianoff@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */
#pragma once
#ifndef __HARDWARE_H__
#define __HARDWARE_H__

#include "stm32f1.h"
#include "time.h"

// onboard LEDs - PB8/PB9
#define LED0_port   GPIOB
#define LED0_pin    (1<<8)
#define LED1_port   GPIOB
#define LED1_pin    (1<<9)

// PPS pin - PA1
#define PPS_port    GPIOA
#define PPS_pin     (1<<1)

// PPS and triggers state
// amount of triggers, should be less than 9
#define TRIGGERS_AMOUNT  (3)
extern GPIO_TypeDef *trigport[TRIGGERS_AMOUNT];
extern uint16_t trigpin[TRIGGERS_AMOUNT];
extern uint8_t trigstate[TRIGGERS_AMOUNT];
uint8_t gettrig(uint8_t N);
#define GET_PPS()       ((GPIOA->IDR & (1<<1)) ? 1 : 0)

// USB pullup - PA15
#define USBPU_port  GPIOA
#define USBPU_pin   (1<<15)
#define USBPU_ON()  pin_clear(USBPU_port, USBPU_pin)
#define USBPU_OFF() pin_set(USBPU_port, USBPU_pin)

#define LED_blink()    pin_toggle(LED0_port, LED0_pin)
#define LED_on()       pin_clear(LED0_port, LED0_pin)
#define LED_off()      pin_set(LED0_port, LED0_pin)
#define LED1_blink()   pin_toggle(LED1_port, LED1_pin)
#define LED1_on()      pin_clear(LED1_port, LED1_pin)
#define LED1_off()     pin_set(LED1_port, LED1_pin)

// GPS USART == USART2, LIDAR USART == USART3
#define GPS_USART   (2)
#define LIDAR_USART (3)

typedef struct{
    uint32_t millis;
    curtime Time;
} trigtime;

// time of triggers shot
extern trigtime shottime[TRIGGERS_AMOUNT];
// if trigger[N] shots, the bit N will be 1
extern uint8_t trigger_shot;

void hw_setup();

#endif // __HARDWARE_H__
