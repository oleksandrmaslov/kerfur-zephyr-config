/******************************************************************************
 *                                                                            *
 *                                                                            *
 *                                Copyright by                                *
 *                                                                            *
 *                              Azoteq (Pty) Ltd                              *
 *                          Republic of South Africa                          *
 *                                                                            *
 *                           Tel: +27(0)21 863 0033                           *
 *                           E-mail: info@azoteq.com                          *
 *                                                                            *
 * ========================================================================== *
 * @file        IQS7222A.h                                                    *
 * @brief       This file contains the header information for an IQS7222A     *
 *              Arduino library. The goal of the library is to provide easy   *
 *              functionality for initializing and using the Azoteq IQS7222A  *
 *              capacitive touch device.                                      *
 * @author      JN. Lochner - Azoteq PTY Ltd                                  *
 * @version     v1.0.0                                                        *
 * @date        2023                                                          *
 * ========================================================================== *
 * @attention  Makes use of the following standard Arduino libraries:         *
 * - Arduino.h   -> included in IQS7222A.h, comes standard with Arduino       *
 * - Wire.h      -> Included in IQS7222A.h, comes standard with Arduino       *                                                                         *
*******************************************************************************/

#ifndef IQS7222A_h
#define IQS7222A_h

// Include Files
#include "Arduino.h"
#include "Wire.h"
#include "./inc/iqs7222a_addresses.h"

// Public Global Definitions
/* For use with Wire.h library. True argument with some functions closes the
I2C communication window. */
#define STOP    true
/* For use with Wire.h library. False argument with some functions keeps the
 I2C communication window open. */
#define RESTART false

//Device Info
#define IQS7222A_PRODUCT_NUM                    840

// Info Flags Byte Bits
#define IQS7222A_ATI_ACTIVE_BIT		        0
#define IQS7222A_ATI_ERROR_BIT	                1
#define IQS7222A_DEVICE_RESET_BIT	        3
#define IQS7222A_POWER_EVENT_BIT_0              4
#define IQS7222A_POWER_EVENT_BIT_1              5
#define IQS7222A_NORMAL_POWER_BIT	        0b00
#define IQS7222A_LOW_POWER_BIT		        0b01
#define IQS7222A_ULP_BIT		        0b10
#define IQS7222A_NORMAL_POWER_UPDATE	        6
#define IQS7222A_GLOBAL_HALT    	        7

// Events Bits
#define IQS7222A_PROX_EVENT_BIT                 0
#define IQS7222A_TOUCH_EVENT_BIT                1
#define IQS7222A_SLIDER_0_EVENT_BIT             2
#define IQS7222A_SLIDER_1_EVENT_BIT             3
#define IQS7222A_ATI_EVENT_BIT                  4
#define IQS7222A_POWER_EVENT_BIT                5

// Slider Event Bits
#define IQS7222A_SLIDER_TAP_BIT                 0
#define IQS7222A_SLIDER_SWIPE_BIT               1
#define IQS7222A_SLIDER_FLICK_BIT               2
#define IQS7222A_SLIDER_NEGATIVE_BIT            5
#define IQS7222A_SLIDER_GESTURE_BIT             6
#define IQS7222A_SLIDER_BUSY_BIT                7

// Channel Prox bits
#define IQS7222A_CH0_PROX_BIT                   0
#define IQS7222A_CH1_PROX_BIT                   1
#define IQS7222A_CH2_PROX_BIT                   2
#define IQS7222A_CH3_PROX_BIT                   3
#define IQS7222A_CH4_PROX_BIT                   4
#define IQS7222A_CH5_PROX_BIT                   5
#define IQS7222A_CH6_PROX_BIT                   6
#define IQS7222A_CH7_PROX_BIT                   7
#define IQS7222A_CH8_PROX_BIT                   0
#define IQS7222A_CH9_PROX_BIT                   1
#define IQS7222A_HALL_PROX_BIT                  2

// Channel Touch Bits
#define IQS7222A_CH0_TOUCH_BIT                  0
#define IQS7222A_CH1_TOUCH_BIT                  1
#define IQS7222A_CH2_TOUCH_BIT                  2
#define IQS7222A_CH3_TOUCH_BIT                  3
#define IQS7222A_CH4_TOUCH_BIT                  4
#define IQS7222A_CH5_TOUCH_BIT                  5
#define IQS7222A_CH6_TOUCH_BIT                  6
#define IQS7222A_CH7_TOUCH_BIT                  7
#define IQS7222A_CH8_TOUCH_BIT                  0
#define IQS7222A_CH9_TOUCH_BIT                  1
#define IQS7222A_HALL_TOUCH_BIT                 2

// Utility Bits
#define IQS7222A_ACK_RESET_BIT		        0
#define IQS7222A_SW_RESET_BIT		        1
#define IQS7222A_RE_ATI_BIT		        2
#define IQS7222A_RESEED_BIT		        3
#define IQS7222A_POWER_MODE_BIT_0               4
#define IQS7222A_POWER_MODE_BIT_1               5
#define IQS7222A_INTERFACE_SELECT_BIT_0         6
#define IQS7222A_INTERFACE_SELECT_BIT_1         7
#define IQS7222A_INTERFACE_I2C_STREAM_BIT	0b00
#define IQS7222A_INTERFACE_I2C_EVENT_BIT	0b01
#define IQS7222A_INTERFACE_I2C_STREAM_IN_TOUCH	0b10

/* Defines and structs for IQS7222A states */
/**
* @brief  iqs7222c Init Enumeration.
*/
typedef enum {
        IQS7222A_STATE_NONE = (uint8_t) 0x00,
        IQS7222A_STATE_START,
        IQS7222A_STATE_INIT,
        IQS7222A_STATE_SW_RESET,
        IQS7222A_STATE_CHECK_RESET,
	IQS7222A_STATE_RUN,
} iqs7222a_state_e;

typedef enum {
        IQS7222A_INIT_NONE = (uint8_t) 0x00,
        IQS7222A_INIT_VERIFY_PRODUCT,
        IQS7222A_INIT_READ_RESET,
	IQS7222A_INIT_CHIP_RESET,
	IQS7222A_INIT_UPDATE_SETTINGS,
	IQS7222A_INIT_CHECK_RESET,
	IQS7222A_INIT_ACK_RESET,
	IQS7222A_INIT_ATI,
        IQS7222A_INIT_WAIT_FOR_ATI,
        IQS7222A_INIT_READ_DATA,
	IQS7222A_INIT_ACTIVATE_EVENT_MODE,
        IQS7222A_INIT_ACTIVATE_STREAM_IN_TOUCH_MODE,
	IQS7222A_INIT_DONE
} iqs7222a_init_e;

typedef enum {
        IQS7222A_CH0 = (uint8_t) 0x00,
        IQS7222A_CH1,
        IQS7222A_CH2,
        IQS7222A_CH3,
        IQS7222A_CH4,
        IQS7222A_CH5,
        IQS7222A_CH6,
        IQS7222A_CH7,
        IQS7222A_CH8,
        IQS7222A_CH9,
        IQS7222A_HALL,
} iqs7222a_channel_e;

typedef enum {
        IQS7222A_SLIDER_TAP = (uint8_t) 0x00,
        IQS7222A_SLIDER_SWIPE_RIGHT,
        IQS7222A_SLIDER_SWIPE_LEFT,
        IQS7222A_SLIDER_FLICK_RIGHT,
        IQS7222A_SLIDER_FLICK_LEFT,
        IQS7222A_SLIDER_NONE,
} iqs7222a_slider_events_e;

typedef enum {
        IQS7222A_SLIDER0 = false,
        IQS7222A_SLIDER1 = true,
} iqs7222a_slider_e;

typedef enum
{
        IQS7222A_CH_NONE = (uint8_t) 0x00,
        IQS7222A_CH_PROX,
        IQS7222A_CH_TOUCH,
        IQS7222A_CH_UNKNOWN,
} iqs7222a_ch_states;
typedef enum
{
        IQS7222A_NORMAL_POWER = (uint8_t) 0x00,
        IQS7222A_LOW_POWER,
        IQS7222A_ULP,
        IQS7222A_HALT,
        IQS7222A_POWER_UNKNOWN
} iqs7222a_power_modes;

/* IQS7222A Memory map data variables, only save the data that might be used
during program runtime */
#pragma pack(1)
typedef struct
{
	/* READ ONLY */			        //  I2C Addresses:
	uint8_t VERSION_DETAILS[20]; 	        // 	0x00 -> 0x09
	uint8_t SYSTEM_STATUS[2];               // 	0x10
	uint8_t EVENTS[2]; 		        // 	0x11
	uint8_t PROX_EVENT_STATES[2]; 	        // 	0x12
        uint8_t TOUCH_EVENT_STATES[2]; 	        // 	0x13
        uint8_t SLIDER_0_OUTPUT[2];             //      0x14
        uint8_t SLIDER_1_OUTPUT[2];             //      0x15
        uint8_t SLIDER_0_STATUS[2];             // 	0x16
        uint8_t SLIDER_1_STATUS[2];             // 	0x17
	uint8_t CH0_COUNTS[2]; 	                // 	0x20
	uint8_t CH1_COUNTS[2]; 	                // 	0x21
	uint8_t CH2_COUNTS[2]; 	                // 	0x22
        uint8_t CH3_COUNTS[2]; 	                // 	0x23
	uint8_t CH4_COUNTS[2]; 	                // 	0x24
	uint8_t CH5_COUNTS[2]; 	                // 	0x25
        uint8_t CH6_COUNTS[2]; 	                // 	0x26
	uint8_t CH7_COUNTS[2]; 	                // 	0x27
	uint8_t CH8_COUNTS[2]; 	                // 	0x28
        uint8_t CH9_COUNTS[2]; 	                // 	0x29
        uint8_t CH10_COUNTS[2]; 	        // 	0x2A
        uint8_t CH11_COUNTS[2]; 	        // 	0x2B

	/* READ WRITE */		        //  I2C Addresses:
	uint8_t SYSTEM_CONTROL[2]; 	        // 	0xD0
} IQS7222A_MEMORY_MAP;
#pragma pack(4)

#pragma pack(1)
typedef struct {
        iqs7222a_state_e        state;
        iqs7222a_init_e         init_state;
}iqs7222a_s;
#pragma pack(4)

// Class Prototype
class IQS7222A
{
public:
        // Public Constructors
        IQS7222A();

        // Public Device States
        iqs7222a_s iqs7222a_state;

        // Public Variables
        IQS7222A_MEMORY_MAP IQSMemoryMap;
        bool new_data_available;

        // Public Methods
        void begin(uint8_t deviceAddressIn, uint8_t readyPinIn);
        bool init(void);
        void run(void);
        void queueValueUpdates(void);
        bool readATIactive(void);
        uint16_t getProductNum(bool stopOrRestart);
        uint8_t getmajorVersion(bool stopOrRestart);
        uint8_t getminorVersion(bool stopOrRestart);
        void acknowledgeReset(bool stopOrRestart);
        void ReATI(bool stopOrRestart);
        void SW_Reset(bool stopOrRestart);
        void writeMM(bool stopOrRestart);
        void clearRDY(void);
        bool getRDYStatus(void);

        void setStreamMode(bool stopOrRestart);
        void setEventMode(bool stopOrRestart);
        void setStreamInTouchMode(bool stopOrRestart);

        void updateInfoFlags(bool stopOrRestart);
        iqs7222a_power_modes get_PowerMode(void);
        bool checkReset(void);

        bool channel_touchState(iqs7222a_channel_e channel);
        bool channel_proxState(iqs7222a_channel_e channel);
        uint16_t sliderCoordinate(iqs7222a_slider_e slider);
        bool slider_event_occurred(iqs7222a_slider_e slider);
        iqs7222a_slider_events_e slider_event(iqs7222a_slider_e slider);

        void force_I2C_communication(void);

private:
        // Private Variables
        uint8_t _deviceAddress;

        // Private Methods
        void readRandomBytes(uint8_t memoryAddress, uint8_t numBytes, uint8_t bytesArray[], bool stopOrRestart);
        void writeRandomBytes(uint8_t memoryAddress, uint8_t numBytes, uint8_t bytesArray[], bool stopOrRestart);
        void writeRandomBytes16(uint16_t memoryAddress, uint8_t numBytes, uint8_t bytesArray[], bool stopOrRestart);
        bool getBit(uint8_t data, uint8_t bit_number);
        uint8_t setBit(uint8_t data, uint8_t bit_number);
        uint8_t clearBit(uint8_t data, uint8_t bit_number);
};
#endif // IQS7222A_h
