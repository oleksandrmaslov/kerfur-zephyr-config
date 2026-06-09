/******************************************************************************
*                                                                             *
*                                                                             *
*                                                                             *
*                                 Copyright by                                *
*                                                                             *
*                               Azoteq (Pty) Ltd                              *
*                           Republic of South Africa                          *
*                                                                             *
*                            Tel: +27(0)21 863 0033                           *
*                                 www.azoteq.com                              *
*                                                                             *
 * ========================================================================== *
* Refer to IQS7222A datasheet for more information, available here:           *
* https://www.azoteq.com/design/datasheets/                                   *
 * ========================================================================== *
*                       IQS7222A - Registers & Memory Map                     *
******************************************************************************/
#ifndef __IQS7222A_ADDRESSES_H
#define __IQS7222A_ADDRESSES_H

/* Device Information - Read Only */
#define IQS7222A_MM_PROD_NUM                0x00
#define IQS7222A_MM_MAJOR_VERSION_NUM       0x01
#define IQS7222A_MM_MINOR_VERSION_NUM       0x02

/* DATA: 0x10 - 0x17 */
#define IQS7222A_MM_INFOFLAGS		            0x10
#define IQS7222A_MM_EVENTS                  0x11
#define IQS7222A_MM_PROX_EVENT_STATES       0x12
#define IQS7222A_MM_TOUCH_EVENT_STATES      0x13
#define IQS7222A_MM_SLIDER_0_OUTPUT         0x14
#define IQS7222A_MM_SLIDER_1_OUTPUT         0x15
#define IQS7222A_MM_SLIDER_0_STATUS         0x16
#define IQS7222A_MM_SLIDER_1_STATUS         0x17

/* CHANNEL COUNTS: 0x20 - 0x2B */
#define IQS7222A_MM_CHANNEL_0_COUNTS        0x20
#define IQS7222A_MM_CHANNEL_1_COUNTS        0x21
#define IQS7222A_MM_CHANNEL_2_COUNTS        0x22
#define IQS7222A_MM_CHANNEL_3_COUNTS        0x23
#define IQS7222A_MM_CHANNEL_4_COUNTS        0x24
#define IQS7222A_MM_CHANNEL_5_COUNTS        0x25
#define IQS7222A_MM_CHANNEL_6_COUNTS        0x26
#define IQS7222A_MM_CHANNEL_7_COUNTS        0x27
#define IQS7222A_MM_CHANNEL_8_COUNTS        0x28
#define IQS7222A_MM_CHANNEL_9_COUNTS        0x29
#define IQS7222A_MM_CHANNEL_10_COUNTS       0x2A
#define IQS7222A_MM_CHANNEL_11_COUNTS       0x2B

/* CHANNEL LTA: 0x30 - 0x3B */
#define IQS7222A_MM_CHANNEL_0_LTA         	0x30
#define IQS7222A_MM_CHANNEL_1_LTA         	0x31
#define IQS7222A_MM_CHANNEL_2_LTA         	0x32
#define IQS7222A_MM_CHANNEL_3_LTA         	0x33
#define IQS7222A_MM_CHANNEL_4_LTA         	0x34
#define IQS7222A_MM_CHANNEL_5_LTA         	0x35
#define IQS7222A_MM_CHANNEL_6_LTA         	0x36
#define IQS7222A_MM_CHANNEL_7_LTA         	0x37
#define IQS7222A_MM_CHANNEL_8_LTA         	0x38
#define IQS7222A_MM_CHANNEL_9_LTA         	0x39
#define IQS7222A_MM_CHANNEL_10_LTA         	0x3A
#define IQS7222A_MM_CHANNEL_11_LTA         	0x3B

/* CYCLE SETUP: 0x80 - 0x87 */
#define IQS7222A_MM_CYCLE_SETUP_0           0x8000
#define IQS7222A_MM_CYCLE_SETUP_1           0x8100
#define IQS7222A_MM_CYCLE_SETUP_2           0x8200
#define IQS7222A_MM_CYCLE_SETUP_3       	  0x8300
#define IQS7222A_MM_CYCLE_SETUP_4           0x8400
#define IQS7222A_MM_CYCLE_SETUP_5           0x8500
#define IQS7222A_MM_CYCLE_SETUP_6           0x8600
#define IQS7222A_MM_GLOBAL_CYCLE_SETUP      0x8700

/* BUTTON SETUP: 0x90 - 0x99 */
#define IQS7222A_MM_BUTTON_SETUP_0          0x9000
#define IQS7222A_MM_BUTTON_SETUP_1        	0x9100
#define IQS7222A_MM_BUTTON_SETUP_2          0x9200
#define IQS7222A_MM_BUTTON_SETUP_3          0x9300
#define IQS7222A_MM_BUTTON_SETUP_4          0x9400
#define IQS7222A_MM_BUTTON_SETUP_5       	  0x9500
#define IQS7222A_MM_BUTTON_SETUP_6   		    0x9600
#define IQS7222A_MM_BUTTON_SETUP_7         	0x9700
#define IQS7222A_MM_BUTTON_SETUP_8          0x9800
#define IQS7222A_MM_BUTTON_SETUP_9          0x9900
#define IQS7222A_MM_BUTTON_SETUP_10         0x9A00
#define IQS7222A_MM_BUTTON_SETUP_11         0x9B00

/* CHANNEL SETUP: 0xA0 - 0xAB */
#define IQS7222A_MM_CHANNEL_SETUP_0         0xA000
#define IQS7222A_MM_CHANNEL_SETUP_1         0xA100
#define IQS7222A_MM_CHANNEL_SETUP_2         0xA200
#define IQS7222A_MM_CHANNEL_SETUP_3         0xA300
#define IQS7222A_MM_CHANNEL_SETUP_4         0xA400
#define IQS7222A_MM_CHANNEL_SETUP_5         0xA500
#define IQS7222A_MM_CHANNEL_SETUP_6         0xA600
#define IQS7222A_MM_CHANNEL_SETUP_7         0xA700
#define IQS7222A_MM_CHANNEL_SETUP_8         0xA800
#define IQS7222A_MM_CHANNEL_SETUP_9         0xA900
#define IQS7222A_MM_HALL_SWITCH_CHANNEL     0xAA00
#define IQS7222A_MM_HALL_CONTROL_CHANNEL    0xAB00

/* FILTER BETAS: 0xAC */
#define IQS7222A_MM_FILTER_BETAS            0xAC00

/* SLIDER SETUP: 0xB0 - 0xB1 */
#define IQS7222A_MM_SLIDER_SETUP_0       	  0xB000
#define IQS7222A_MM_SLIDER_SETUP_1          0xB100

/* GPIO SETTINGS: 0xC0 */
#define IQS7222A_MM_GPIO_SETTINGS	          0xC000

/* PMU & SYSTEM SETTINGS: 0xD0 - 0xD8*/
#define IQS7222A_MM_CONTROL_SETTINGS        0xD0
#define IQS7222A_MM_ATI_ERROR_TIMEOUT       0xD1
#define IQS7222A_MM_ATI_REPORT_RATE         0xD2
#define IQS7222A_MM_NP_TIMEOUT         		  0xD3
#define IQS7222A_MM_NP_REPORT_RATE         	0xD4
#define IQS7222A_MM_LP_TIMEOUT         		  0xD5
#define IQS7222A_MM_LP_REPORT_RATE         	0xD6
#define IQS7222A_MM_ULP_NP_UPDATE_RATE      0xD7
#define IQS7222A_MM_ULP_REPORT_RATE         0xD8
#define IQS7222A_MM_ULP_ENTRY_MASK          0xD9
#define IQS7222A_MM_EVENT_MASK              0xDA
#define IQS7222A_MM_HALL_OFF_BIAS_CURRENT   0xDB
#define IQS7222A_MM_I2C_COMMUNICATION       0xDC

  
#endif /* __IQS7222A_ADDRESSES_H */