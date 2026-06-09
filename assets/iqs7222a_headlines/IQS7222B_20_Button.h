/*
* This file contains all the necessary settings for the IQS7222B and this file can
* be changed from the GUI or edited here
* File:   IQS7222B_init.h
* Author: Azoteq
*/ 

#ifndef IQS7222B_INIT_H
#define IQS7222B_INIT_H

/* Change the Cycle Setup 0 - 4 */
/* Memory Map Position 0x8000 - 0x8401 */
#define CYCLE_0_CONV_FREQ_FRAC                   0x7F
#define CYCLE_0_CONV_FREQ_PERIOD                 0x02
#define CYCLE_0_SETTINGS                         0x62
#define CYCLE_0_CTX_SELECT                       0x02
#define CYCLE_1_CONV_FREQ_FRAC                   0x7F
#define CYCLE_1_CONV_FREQ_PERIOD                 0x02
#define CYCLE_1_SETTINGS                         0x62
#define CYCLE_1_CTX_SELECT                       0x20
#define CYCLE_2_CONV_FREQ_FRAC                   0x7F
#define CYCLE_2_CONV_FREQ_PERIOD                 0x02
#define CYCLE_2_SETTINGS                         0x62
#define CYCLE_2_CTX_SELECT                       0x80
#define CYCLE_3_CONV_FREQ_FRAC                   0x7F
#define CYCLE_3_CONV_FREQ_PERIOD                 0x02
#define CYCLE_3_SETTINGS                         0x62
#define CYCLE_3_CTX_SELECT                       0x04
#define CYCLE_4_CONV_FREQ_FRAC                   0x7F
#define CYCLE_4_CONV_FREQ_PERIOD                 0x02
#define CYCLE_4_SETTINGS                         0x62
#define CYCLE_4_CTX_SELECT                       0x40

/* Change the Cycle Setup 5 - 9 */
/* Memory Map Position 0x8500 - 0x8A01 */
#define CYCLE_5_CONV_FREQ_FRAC                   0x7F
#define CYCLE_5_CONV_FREQ_PERIOD                 0x02
#define CYCLE_5_SETTINGS                         0x62
#define CYCLE_5_CTX_SELECT                       0x02
#define CYCLE_6_CONV_FREQ_FRAC                   0x7F
#define CYCLE_6_CONV_FREQ_PERIOD                 0x02
#define CYCLE_6_SETTINGS                         0x62
#define CYCLE_6_CTX_SELECT                       0x20
#define CYCLE_7_CONV_FREQ_FRAC                   0x7F
#define CYCLE_7_CONV_FREQ_PERIOD                 0x02
#define CYCLE_7_SETTINGS                         0x62
#define CYCLE_7_CTX_SELECT                       0x80
#define CYCLE_8_CONV_FREQ_FRAC                   0x7F
#define CYCLE_8_CONV_FREQ_PERIOD                 0x02
#define CYCLE_8_SETTINGS                         0x62
#define CYCLE_8_CTX_SELECT                       0x04
#define CYCLE_9_CONV_FREQ_FRAC                   0x7F
#define CYCLE_9_CONV_FREQ_PERIOD                 0x02
#define CYCLE_9_SETTINGS                         0x62
#define CYCLE_9_CTX_SELECT                       0x40
#define GLOBAL_CYCLE_SETUP_0                     0x8B
#define GLOBAL_CYCLE_SETUP_1                     0x2B
#define COARSE_DIVIDER_PRELOAD                   0x10
#define FINE_DIVIDER_PRELOAD                     0x30
#define COMPENSATION_PRELOAD_0                   0x00
#define COMPENSATION_PRELOAD_1                   0x02

/* Change the Button Setup 0 - 6 */
/* Memory Map Position 0x9000 - 0x9601 */
#define BUTTON_0_PROX_THRESHOLD                  0x0A
#define BUTTON_0_ENTER_EXIT                      0x12
#define BUTTON_0_TOUCH_THRESHOLD                 0x19
#define BUTTON_0_TOUCH_HYSTERESIS                0x00
#define BUTTON_1_PROX_THRESHOLD                  0x0A
#define BUTTON_1_ENTER_EXIT                      0x12
#define BUTTON_1_TOUCH_THRESHOLD                 0x19
#define BUTTON_1_TOUCH_HYSTERESIS                0x00
#define BUTTON_2_PROX_THRESHOLD                  0x0A
#define BUTTON_2_ENTER_EXIT                      0x12
#define BUTTON_2_TOUCH_THRESHOLD                 0x19
#define BUTTON_2_TOUCH_HYSTERESIS                0x00
#define BUTTON_3_PROX_THRESHOLD                  0x0A
#define BUTTON_3_ENTER_EXIT                      0x12
#define BUTTON_3_TOUCH_THRESHOLD                 0x19
#define BUTTON_3_TOUCH_HYSTERESIS                0x00
#define BUTTON_4_PROX_THRESHOLD                  0x0A
#define BUTTON_4_ENTER_EXIT                      0x12
#define BUTTON_4_TOUCH_THRESHOLD                 0x19
#define BUTTON_4_TOUCH_HYSTERESIS                0x00
#define BUTTON_5_PROX_THRESHOLD                  0x0A
#define BUTTON_5_ENTER_EXIT                      0x12
#define BUTTON_5_TOUCH_THRESHOLD                 0x19
#define BUTTON_5_TOUCH_HYSTERESIS                0x00
#define BUTTON_6_PROX_THRESHOLD                  0x0A
#define BUTTON_6_ENTER_EXIT                      0x12
#define BUTTON_6_TOUCH_THRESHOLD                 0x19
#define BUTTON_6_TOUCH_HYSTERESIS                0x00

/* Change the Button Setup 7 - 13 */
/* Memory Map Position 0x9700 - 0x9D01 */
#define BUTTON_7_PROX_THRESHOLD                  0x0A
#define BUTTON_7_ENTER_EXIT                      0x12
#define BUTTON_7_TOUCH_THRESHOLD                 0x19
#define BUTTON_7_TOUCH_HYSTERESIS                0x00
#define BUTTON_8_PROX_THRESHOLD                  0x0A
#define BUTTON_8_ENTER_EXIT                      0x12
#define BUTTON_8_TOUCH_THRESHOLD                 0x19
#define BUTTON_8_TOUCH_HYSTERESIS                0x00
#define BUTTON_9_PROX_THRESHOLD                  0x0A
#define BUTTON_9_ENTER_EXIT                      0x12
#define BUTTON_9_TOUCH_THRESHOLD                 0x19
#define BUTTON_9_TOUCH_HYSTERESIS                0x00
#define BUTTON_10_PROX_THRESHOLD                 0x0A
#define BUTTON_10_ENTER_EXIT                     0x12
#define BUTTON_10_TOUCH_THRESHOLD                0x19
#define BUTTON_10_TOUCH_HYSTERESIS               0x00
#define BUTTON_11_PROX_THRESHOLD                 0x0A
#define BUTTON_11_ENTER_EXIT                     0x12
#define BUTTON_11_TOUCH_THRESHOLD                0x19
#define BUTTON_11_TOUCH_HYSTERESIS               0x00
#define BUTTON_12_PROX_THRESHOLD                 0x0A
#define BUTTON_12_ENTER_EXIT                     0x12
#define BUTTON_12_TOUCH_THRESHOLD                0x19
#define BUTTON_12_TOUCH_HYSTERESIS               0x00
#define BUTTON_13_PROX_THRESHOLD                 0x0A
#define BUTTON_13_ENTER_EXIT                     0x12
#define BUTTON_13_TOUCH_THRESHOLD                0x19
#define BUTTON_13_TOUCH_HYSTERESIS               0x00

/* Change the Button Setup 14 - 17 */
/* Memory Map Position 0x9E00 - 0xA301 */
#define BUTTON_14_PROX_THRESHOLD                 0x0A
#define BUTTON_14_ENTER_EXIT                     0x12
#define BUTTON_14_TOUCH_THRESHOLD                0x19
#define BUTTON_14_TOUCH_HYSTERESIS               0x00
#define BUTTON_15_PROX_THRESHOLD                 0x0A
#define BUTTON_15_ENTER_EXIT                     0x12
#define BUTTON_15_TOUCH_THRESHOLD                0x19
#define BUTTON_15_TOUCH_HYSTERESIS               0x00
#define BUTTON_16_PROX_THRESHOLD                 0x0A
#define BUTTON_16_ENTER_EXIT                     0x12
#define BUTTON_16_TOUCH_THRESHOLD                0x19
#define BUTTON_16_TOUCH_HYSTERESIS               0x00
#define BUTTON_17_PROX_THRESHOLD                 0x0A
#define BUTTON_17_ENTER_EXIT                     0x12
#define BUTTON_17_TOUCH_THRESHOLD                0x19
#define BUTTON_17_TOUCH_HYSTERESIS               0x00
#define BUTTON_18_PROX_THRESHOLD                 0x0A
#define BUTTON_18_ENTER_EXIT                     0x12
#define BUTTON_18_TOUCH_THRESHOLD                0x19
#define BUTTON_18_TOUCH_HYSTERESIS               0x00
#define BUTTON_19_PROX_THRESHOLD                 0x0A
#define BUTTON_19_ENTER_EXIT                     0x12
#define BUTTON_19_TOUCH_THRESHOLD                0x19
#define BUTTON_19_TOUCH_HYSTERESIS               0x00

/* Change the CH0 - CH2 Setup */
/* Memory Map Position 0xB000 - 0xB203 */
#define CH0_SETUP_0                              0x13
#define CH0_SETUP_1                              0x15
#define CH0_ATI_SETTINGS_0                       0x39
#define CH0_ATI_SETTINGS_1                       0x32
#define CH0_MULTIPLIERS_0                        0xE2
#define CH0_MULTIPLIERS_1                        0x2B
#define CH0_ATI_COMPENSATION_0                   0xDE
#define CH0_ATI_COMPENSATION_1                   0x61
#define CH1_SETUP_0                              0x13
#define CH1_SETUP_1                              0x15
#define CH1_ATI_SETTINGS_0                       0x39
#define CH1_ATI_SETTINGS_1                       0x32
#define CH1_MULTIPLIERS_0                        0xE2
#define CH1_MULTIPLIERS_1                        0x2D
#define CH1_ATI_COMPENSATION_0                   0xD6
#define CH1_ATI_COMPENSATION_1                   0x59
#define CH2_SETUP_0                              0x13
#define CH2_SETUP_1                              0x15
#define CH2_ATI_SETTINGS_0                       0x39
#define CH2_ATI_SETTINGS_1                       0x32
#define CH2_MULTIPLIERS_0                        0xE1
#define CH2_MULTIPLIERS_1                        0x31
#define CH2_ATI_COMPENSATION_0                   0xF2
#define CH2_ATI_COMPENSATION_1                   0x61

/* Change the CH3 - CH5 Setup */
/* Memory Map Position 0xB300 - 0xB503 */
#define CH3_SETUP_0                              0x13
#define CH3_SETUP_1                              0x15
#define CH3_ATI_SETTINGS_0                       0x39
#define CH3_ATI_SETTINGS_1                       0x32
#define CH3_MULTIPLIERS_0                        0xE2
#define CH3_MULTIPLIERS_1                        0x2B
#define CH3_ATI_COMPENSATION_0                   0xE0
#define CH3_ATI_COMPENSATION_1                   0x61
#define CH4_SETUP_0                              0x13
#define CH4_SETUP_1                              0x15
#define CH4_ATI_SETTINGS_0                       0x39
#define CH4_ATI_SETTINGS_1                       0x32
#define CH4_MULTIPLIERS_0                        0xE1
#define CH4_MULTIPLIERS_1                        0x39
#define CH4_ATI_COMPENSATION_0                   0xF0
#define CH4_ATI_COMPENSATION_1                   0x61
#define CH5_SETUP_0                              0x23
#define CH5_SETUP_1                              0x15
#define CH5_ATI_SETTINGS_0                       0x39
#define CH5_ATI_SETTINGS_1                       0x32
#define CH5_MULTIPLIERS_0                        0xE5
#define CH5_MULTIPLIERS_1                        0x27
#define CH5_ATI_COMPENSATION_0                   0xB0
#define CH5_ATI_COMPENSATION_1                   0x63

/* Change the CH6 - CH8 Setup */
/* Memory Map Position 0xB600 - 0xB803 */
#define CH6_SETUP_0                              0x23
#define CH6_SETUP_1                              0x15
#define CH6_ATI_SETTINGS_0                       0x39
#define CH6_ATI_SETTINGS_1                       0x32
#define CH6_MULTIPLIERS_0                        0xE2
#define CH6_MULTIPLIERS_1                        0x2D
#define CH6_ATI_COMPENSATION_0                   0xD9
#define CH6_ATI_COMPENSATION_1                   0x59
#define CH7_SETUP_0                              0x23
#define CH7_SETUP_1                              0x15
#define CH7_ATI_SETTINGS_0                       0x39
#define CH7_ATI_SETTINGS_1                       0x32
#define CH7_MULTIPLIERS_0                        0xE1
#define CH7_MULTIPLIERS_1                        0x31
#define CH7_ATI_COMPENSATION_0                   0xFE
#define CH7_ATI_COMPENSATION_1                   0x61
#define CH8_SETUP_0                              0x23
#define CH8_SETUP_1                              0x15
#define CH8_ATI_SETTINGS_0                       0x39
#define CH8_ATI_SETTINGS_1                       0x32
#define CH8_MULTIPLIERS_0                        0xE2
#define CH8_MULTIPLIERS_1                        0x2D
#define CH8_ATI_COMPENSATION_0                   0xD6
#define CH8_ATI_COMPENSATION_1                   0x59

/* Change the CH9 - CH11 Setup */
/* Memory Map Position 0xB900 - 0xBB03 */
#define CH9_SETUP_0                              0x23
#define CH9_SETUP_1                              0x15
#define CH9_ATI_SETTINGS_0                       0x39
#define CH9_ATI_SETTINGS_1                       0x32
#define CH9_MULTIPLIERS_0                        0xE1
#define CH9_MULTIPLIERS_1                        0x3D
#define CH9_ATI_COMPENSATION_0                   0xF3
#define CH9_ATI_COMPENSATION_1                   0x69
#define CH10_SETUP_0                             0x13
#define CH10_SETUP_1                             0x15
#define CH10_ATI_SETTINGS_0                      0x39
#define CH10_ATI_SETTINGS_1                      0x32
#define CH10_MULTIPLIERS_0                       0xE2
#define CH10_MULTIPLIERS_1                       0x29
#define CH10_ATI_COMPENSATION_0                  0xE8
#define CH10_ATI_COMPENSATION_1                  0x59
#define CH11_SETUP_0                             0x13
#define CH11_SETUP_1                             0x15
#define CH11_ATI_SETTINGS_0                      0x39
#define CH11_ATI_SETTINGS_1                      0x32
#define CH11_MULTIPLIERS_0                       0xE2
#define CH11_MULTIPLIERS_1                       0x29
#define CH11_ATI_COMPENSATION_0                  0xE4
#define CH11_ATI_COMPENSATION_1                  0x59

/* Change the CH12 - CH14 Setup */
/* Memory Map Position 0xBC00 - 0xBE03 */
#define CH12_SETUP_0                             0x13
#define CH12_SETUP_1                             0x15
#define CH12_ATI_SETTINGS_0                      0x39
#define CH12_ATI_SETTINGS_1                      0x32
#define CH12_MULTIPLIERS_0                       0xE1
#define CH12_MULTIPLIERS_1                       0x31
#define CH12_ATI_COMPENSATION_0                  0xD3
#define CH12_ATI_COMPENSATION_1                  0x59
#define CH13_SETUP_0                             0x13
#define CH13_SETUP_1                             0x15
#define CH13_ATI_SETTINGS_0                      0x39
#define CH13_ATI_SETTINGS_1                      0x32
#define CH13_MULTIPLIERS_0                       0xE6
#define CH13_MULTIPLIERS_1                       0x25
#define CH13_ATI_COMPENSATION_0                  0xDB
#define CH13_ATI_COMPENSATION_1                  0x63
#define CH14_SETUP_0                             0x13
#define CH14_SETUP_1                             0x15
#define CH14_ATI_SETTINGS_0                      0x39
#define CH14_ATI_SETTINGS_1                      0x32
#define CH14_MULTIPLIERS_0                       0xE1
#define CH14_MULTIPLIERS_1                       0x3D
#define CH14_ATI_COMPENSATION_0                  0xF0
#define CH14_ATI_COMPENSATION_1                  0x69

/* Change the CH15 - CH17 Setup */
/* Memory Map Position 0xBF00 - 0xC103 */
#define CH15_SETUP_0                             0x23
#define CH15_SETUP_1                             0x15
#define CH15_ATI_SETTINGS_0                      0x39
#define CH15_ATI_SETTINGS_1                      0x32
#define CH15_MULTIPLIERS_0                       0xE1
#define CH15_MULTIPLIERS_1                       0x37
#define CH15_ATI_COMPENSATION_0                  0xE2
#define CH15_ATI_COMPENSATION_1                  0x61
#define CH16_SETUP_0                             0x23
#define CH16_SETUP_1                             0x15
#define CH16_ATI_SETTINGS_0                      0x39
#define CH16_ATI_SETTINGS_1                      0x32
#define CH16_MULTIPLIERS_0                       0xE2
#define CH16_MULTIPLIERS_1                       0x2B
#define CH16_ATI_COMPENSATION_0                  0xDC
#define CH16_ATI_COMPENSATION_1                  0x61
#define CH17_SETUP_0                             0x23
#define CH17_SETUP_1                             0x15
#define CH17_ATI_SETTINGS_0                      0x39
#define CH17_ATI_SETTINGS_1                      0x32
#define CH17_MULTIPLIERS_0                       0xE1
#define CH17_MULTIPLIERS_1                       0x39
#define CH17_ATI_COMPENSATION_0                  0xFA
#define CH17_ATI_COMPENSATION_1                  0x61

/* Change the CH18 - CH19 Setup */
/* Memory Map Position 0xC200 - 0xC303 */
#define CH18_SETUP_0                             0x23
#define CH18_SETUP_1                             0x15
#define CH18_ATI_SETTINGS_0                      0x39
#define CH18_ATI_SETTINGS_1                      0x32
#define CH18_MULTIPLIERS_0                       0xE2
#define CH18_MULTIPLIERS_1                       0x2D
#define CH18_ATI_COMPENSATION_0                  0xE3
#define CH18_ATI_COMPENSATION_1                  0x59
#define CH19_SETUP_0                             0x23
#define CH19_SETUP_1                             0x15
#define CH19_ATI_SETTINGS_0                      0x39
#define CH19_ATI_SETTINGS_1                      0x32
#define CH19_MULTIPLIERS_0                       0xE2
#define CH19_MULTIPLIERS_1                       0x31
#define CH19_ATI_COMPENSATION_0                  0xF2
#define CH19_ATI_COMPENSATION_1                  0x69

/* Change the Filter Betas */
/* Memory Map Position 0xC400 - 0xC401 */
#define COUNTS_BETA_FILTER                       0x12
#define LTA_BETA_FILTER                          0x78
#define LTA_FAST_BETA_FILTER                     0x34
#define RESERVED_FILTER_0                        0x08

/* Change the System Settings */
/* Memory Map Position 0xD0 - 0xD9 */
#define SYSTEM_CONTROL_0                         0x00
#define SYSTEM_CONTROL_1                         0x14
#define ATI_ERROR_TIMEOUT_0                      0x02
#define ATI_ERROR_TIMEOUT_1                      0x00
#define ATI_REPORT_RATE_0                        0x00
#define ATI_REPORT_RATE_1                        0x00
#define NORMAL_MODE_TIMEOUT_0                    0x88
#define NORMAL_MODE_TIMEOUT_1                    0x13
#define NORMAL_MODE_REPORT_RATE_0                0x10
#define NORMAL_MODE_REPORT_RATE_1                0x00
#define LP_MODE_TIMEOUT_0                        0x88
#define LP_MODE_TIMEOUT_1                        0x13
#define LP_MODE_REPORT_RATE_0                    0x3C
#define LP_MODE_REPORT_RATE_1                    0x00
#define ULP_MODE_TIMEOUT_0                       0x10
#define ULP_MODE_TIMEOUT_1                       0x27
#define ULP_MODE_REPORT_RATE_0                   0x96
#define ULP_MODE_REPORT_RATE_1                   0x00
#define PROX_EVENT_TIMEOUT_0                     0x08
#define TOUCH_EVENT_TIMEOUT_0                    0x28
#define TOUCH_PROX_EVENT_EN                      0xEE
#define POWER_ATI_EVENT_EN                       0xEE
#define I2CCOMMS_0                               0xEE

#endif	/* IQS7222B_INIT_H */