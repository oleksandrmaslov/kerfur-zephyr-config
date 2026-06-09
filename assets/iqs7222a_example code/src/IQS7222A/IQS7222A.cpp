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
 * @file        IQS7222A.cpp                                                  *
 * @brief       This file contains the constructors and methods which allow   *
 *              ease of use of an IQS7222A capacitive touch controller. The    *
 *              IQS7222A is a capacitive touch Integrated Circuit (IC) which  *
 *              provides multiple channel functionality. This class provides  *
 *              an easy means of initializing and interacting with the        *
 *              IQS7222A device from an Arduino.                              *
 * @author      JN. Lochner - Azoteq PTY Ltd                                  *
 * @version     v1.1                                                          *
 * @date        2023                                                          *
 * ========================================================================== *
 * @attention  Makes use of the following standard Arduino libraries:         *
 * - Arduino.h -> Included in IQS7222A.h, comes standard with Arduino         *
 * - Wire.h    -> Included in IQS7222A.h, comes standard with Arduino         *                                                                           *
 ******************************************************************************/

/* Include Files */
#include "IQS7222A.h"
#include "IQS7222A_Inductive_Coil_and_Slider.h"

/* Private Global Variables */
bool iqs7222a_deviceRDY = false;
uint8_t iqs7222a_ready_pin;

/* Private Functions */
void iqs7222a_ready_interrupt(void);

/*****************************************************************************/
/*                               CONSTRUCTORS                                */
/*****************************************************************************/
IQS7222A::IQS7222A(){
}

/*****************************************************************************/
/*                            PUBLIC METHODS                                 */
/*****************************************************************************/

/**
  * @name   begin
  * @brief  A method to initialize the IQS7222A device with the device address
  *         and ready pin specified by the user.
  * @param  deviceAddress -> The address of the IQS7222A device.
  * @param  readyPin      -> The Arduino pin which is connected to the ready
  *                          pin of the IQS7222A device.
  * @retval None.
  * @note   - Receiving a true return value does not mean that initialization 
  *           was successful.
  *         - Receiving a true return value only means that the IQS device 
  *           responded to the request for communication.
  *         - Receiving a false return value means that initialization did not 
  *           take place at all.
  *         - If communication is successfully established then it is unlikely 
  *           that initialization will fail.
  */
void IQS7222A::begin(uint8_t deviceAddressIn, uint8_t readyPinIn)
{
  // Initialize I2C communication here, since this library can't function without it.
  Wire.begin();
  Wire.setClock(400000);

  _deviceAddress = deviceAddressIn;
  iqs7222a_ready_pin = readyPinIn;
  attachInterrupt(digitalPinToInterrupt(iqs7222a_ready_pin), iqs7222a_ready_interrupt, CHANGE);

  /* Initialize "running" and "init" state machine variables. */
  iqs7222a_state.state      = IQS7222A_STATE_START;
  iqs7222a_state.init_state = IQS7222A_INIT_VERIFY_PRODUCT;
}

/**
  * @name   init
  * @brief  A method that runs through a normal start-up routine to set up the
  *         IQS7222A with the desired settings from the IQS7222A_init.h file.
  * @retval Returns true if the full start-up routine has been completed,
  *         returns false if not.
  * @param  None.
  * @note   - No false return will be given, the program will thus be stuck when 
  *           one of the cases is not able to finish.
  *         - See serial communication to find the ERROR case
  */
bool IQS7222A::init(void)
{
  uint16_t prod_num;
  uint8_t ver_maj, ver_min;

  switch (iqs7222a_state.init_state)
  {
    /* Verifies product number to determine if the correct device is connected
       for this example */
    case IQS7222A_INIT_VERIFY_PRODUCT:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_VERIFY_PRODUCT");
        prod_num = getProductNum(RESTART);
        ver_maj = getmajorVersion(RESTART);
        ver_min = getminorVersion(STOP);
        Serial.print("\t\tProduct number is: ");
        Serial.print(prod_num);
        Serial.print(" v");
        Serial.print(ver_maj);
        Serial.print(".");
        Serial.println(ver_min);
        if(prod_num == IQS7222A_PRODUCT_NUM)
        {
          Serial.println("\t\tIQS7222A Release UI Confirmed!");
          iqs7222a_state.init_state = IQS7222A_INIT_READ_RESET;
        }
        else
        {
          Serial.println("\t\tDevice is not a IQS7222A!");
          iqs7222a_state.init_state = IQS7222A_INIT_NONE;
        }
      }
    break;

    /* Verify if a reset has occurred */
    case IQS7222A_INIT_READ_RESET:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_READ_RESET");
        updateInfoFlags(RESTART);
        if (checkReset())
        {
          Serial.println("\t\tReset event occurred.");
          iqs7222a_state.init_state = IQS7222A_INIT_UPDATE_SETTINGS;
        }
        else
        {
          Serial.println("\t\t No Reset Event Detected - Request SW Reset");
          iqs7222a_state.init_state = IQS7222A_INIT_CHIP_RESET;
        }
      }
    break;

    /* Perform SW Reset */
    case IQS7222A_INIT_CHIP_RESET:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_CHIP_RESET");

        //Perform SW Reset
        SW_Reset(STOP);
        Serial.println("\t\tSoftware Reset Bit Set.");
        delay(100);
        iqs7222a_state.init_state = IQS7222A_INIT_READ_RESET;
      }
    break;

    /* Write all settings to IQS7222A from .h file */
    case IQS7222A_INIT_UPDATE_SETTINGS:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_UPDATE_SETTINGS");
        writeMM(RESTART);
        iqs7222a_state.init_state = IQS7222A_INIT_ACK_RESET;
      }
    break;

    /* Acknowledge that the device went through a reset */
    case IQS7222A_INIT_ACK_RESET:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_ACK_RESET");
        acknowledgeReset(STOP);
        iqs7222a_state.init_state = IQS7222A_INIT_ATI;
      }
      break;

    /* Run the ATI algorithm to recalibrate the device with newly added settings */
    case IQS7222A_INIT_ATI:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_ATI");
        ReATI(STOP);
        iqs7222a_state.init_state = IQS7222A_INIT_WAIT_FOR_ATI;
        Serial.println("\tIQS7222A_INIT_WAIT_FOR_ATI");
      }
    break;

    /* Read the ATI Active bit to see if the rest of the program can continue */
    case IQS7222A_INIT_WAIT_FOR_ATI:
      if(iqs7222a_deviceRDY)
      {
        if(!readATIactive())
        {
          Serial.println("\t\tDONE");
          iqs7222a_state.init_state = IQS7222A_INIT_READ_DATA;
        }
      }
    break;

    /* Read the latest data from the iqs7222C */
    case IQS7222A_INIT_READ_DATA:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_READ_DATA");
        queueValueUpdates();
        iqs7222a_state.init_state = IQS7222A_INIT_ACTIVATE_EVENT_MODE;
      }
    break;

    /* Turn on I2C Stream-In-Touch mode */
    case IQS7222A_INIT_ACTIVATE_STREAM_IN_TOUCH_MODE:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_ACTIVATE_STREAM_IN_TOUCH_MODE");
        setStreamInTouchMode(STOP);
        iqs7222a_state.init_state = IQS7222A_INIT_DONE;
      }
    break;

    /* Turn on I2C Event mode */
    case IQS7222A_INIT_ACTIVATE_EVENT_MODE:
      if(iqs7222a_deviceRDY)
      {
        Serial.println("\tIQS7222A_INIT_ACTIVATE_EVENT_MODE");
        setEventMode(STOP);
        iqs7222a_state.init_state = IQS7222A_INIT_DONE;
      }
    break;

    /* If all operations has been completed correctly, the RDY pin can be set
     * up as an interrupt to indicate when new data is available */
    case IQS7222A_INIT_DONE:
      Serial.println("\tIQS7222A_INIT_DONE");
      new_data_available = true;
      return true;
    break;

    default:
      break;
  }
  return false;
}


/**
  * @name   run
  * @brief  This method is called continuously during runtime, and serves as the
  *         main state machine
  * @param  None.
  * @retval None.
  * @note   The state machine continuously checks for specific events and
  *         updates the state machine accordingly. A reset event will cause
  *         the state machine to re-initialize the device.
  *
  *         queueValueUpdates can be edited by the user if other data should be
  *         read every time a RDY window is received.
  */
void IQS7222A::run(void)
{
  switch (iqs7222a_state.state)
  {
    /* After a hardware reset, this is the starting position of the main state
    machine */
    case IQS7222A_STATE_START:
      Serial.println("IQS7222A Initialization:");
      iqs7222a_state.state = IQS7222A_STATE_INIT;
    break;

    /* Perform the initialization routine on the IQS7222A */
    case IQS7222A_STATE_INIT:
      if(init())
      {
        Serial.println("IQS7222A Initialization complete!\n");
        iqs7222a_state.state = IQS7222A_STATE_RUN;
      }
    break;

    /* Send an 12C software reset in the next rdy window */
    case IQS7222A_STATE_SW_RESET:
      if(iqs7222a_deviceRDY)
      {
        SW_Reset(STOP);
        iqs7222a_state.state = IQS7222A_STATE_RUN;
      }
    break;

    /* Continuous reset monitoring state, ensure no reset event has occurred
      for data to be valid */
    case IQS7222A_STATE_CHECK_RESET:
      if(checkReset())
      {
        Serial.println("Reset Occurred!\n");
        new_data_available = false;
        iqs7222a_state.state = IQS7222A_STATE_START;
        iqs7222a_state.init_state = IQS7222A_INIT_VERIFY_PRODUCT;
      }
      /* A reset did not occur, move to the run state and wait for a new ready
      window */
      else
      {
        new_data_available = true; /* No reset, thus data is valid */
        iqs7222a_state.state = IQS7222A_STATE_RUN;
      }
    break;

    /* If a RDY Window is open, read the latest values from the IQS7222A */
    case IQS7222A_STATE_RUN:
      if(iqs7222a_deviceRDY)
      {
        queueValueUpdates();
        iqs7222a_deviceRDY = false;
        new_data_available = false;
        iqs7222a_state.state = IQS7222A_STATE_CHECK_RESET;
      }
    break;
  }
}

/**
  * @name   iqs7222a_ready_interrupt
  * @brief  A method used as an interrupt function. Only activated when a HL
  *         interrupt is seen on the correct Arduino interrupt pin.
  * @param  None.
  * @retval None.
  * @note   Keep this function as simple as possible to prevent stuck states
  *         and slow operations.
  */
void iqs7222a_ready_interrupt(void)
{
  if(digitalRead(iqs7222a_ready_pin))
  {
    iqs7222a_deviceRDY = false;
  }
  else
  {
    iqs7222a_deviceRDY = true;
  }
}

/**
  * @name   clearRDY
  * @brief  A method used to clear the ready interrupt bit.
  * @param  None.
  * @retval None.
  * @note
  */
void IQS7222A::clearRDY(void)
{
  iqs7222a_deviceRDY = false;
}

/**
  * @name   getRDYStatus
  * @brief  A method used to retrieve the device RDY status.
  * @param  None.
  * @retval Returns the boolean IQS7222A RDY state.
  *         - True when RDY line is LOW
  *         - False when RDY line is HIGH
  * @note
  */
bool IQS7222A::getRDYStatus(void)
{
  return iqs7222a_deviceRDY;
}

/**
  * @name   queueValueUpdates
  * @brief  All I2C read operations in the queueValueUpdates method will be
  *         performed each time the IQS7222A opens a RDY window.
  * @param  None.
  * @retval None.
  * @note   Any Address in the memory map can be read from here.
  */
void IQS7222A::queueValueUpdates(void)
{
  uint8_t transferBytes[16];	// The array which will hold the bytes to be transferred.

	/* Read the info flags, event flags, prox event states, touch event states,
  slider coordinates, and slider events. */
	readRandomBytes(IQS7222A_MM_INFOFLAGS, 16, transferBytes, STOP);

  IQSMemoryMap.SYSTEM_STATUS[0]       = transferBytes[0];
  IQSMemoryMap.SYSTEM_STATUS[1]       = transferBytes[1];

  IQSMemoryMap.EVENTS[0]              = transferBytes[2];
  IQSMemoryMap.EVENTS[1]              = transferBytes[3];

  IQSMemoryMap.PROX_EVENT_STATES[0]   = transferBytes[4];
  IQSMemoryMap.PROX_EVENT_STATES[1]   = transferBytes[5];

  IQSMemoryMap.TOUCH_EVENT_STATES[0]  = transferBytes[6];
  IQSMemoryMap.TOUCH_EVENT_STATES[1]  = transferBytes[7];

  IQSMemoryMap.SLIDER_0_OUTPUT[0]     = transferBytes[8];
  IQSMemoryMap.SLIDER_0_OUTPUT[1]     = transferBytes[9];

  IQSMemoryMap.SLIDER_1_OUTPUT[0]     = transferBytes[10];
  IQSMemoryMap.SLIDER_1_OUTPUT[1]     = transferBytes[11];

  IQSMemoryMap.SLIDER_0_STATUS[0]     = transferBytes[12];
  IQSMemoryMap.SLIDER_0_STATUS[1]     = transferBytes[13];

  IQSMemoryMap.SLIDER_1_STATUS[0]     = transferBytes[14];
  IQSMemoryMap.SLIDER_1_STATUS[1]     = transferBytes[15];
}

/**
  * @name	  readATIactive
  * @brief  A method that checks if the ATI routine is still active
  * @param  None.
  * @retval Returns true if a the ATI_ACTIVE_BIT is cleared, false if the
  *         ATI_ACTIVE_BIT is set.
  * @note   If the ATI routine is active the channel states (NONE, PROX, TOUCH) 
  *         might exhibit unwanted behaviour. Thus it is advised to wait for 
  *         the routine to complete before continuing.
  */
bool IQS7222A::readATIactive(void)
{
  /* Read the Info flags */
  updateInfoFlags(STOP);

  /* Return the ATI Active status */
  return getBit(IQSMemoryMap.SYSTEM_STATUS[0], IQS7222A_ATI_ACTIVE_BIT);
}

/**
  * @name	  checkReset
  * @brief  A method which checks if the device has reset and returns the reset 
  *         status.
  * @param  None.
  * @retval Returns true if a reset has occurred, false if no reset has occurred.
  * @note   If a reset has occurred the device settings should be reloaded using
  *         the begin function. After new device settings have been reloaded the
  *         acknowledge reset function can be used to clear the reset flag.
  */
bool IQS7222A::checkReset(void)
{
 	/* Perform a bitwise AND operation inside getBIT with the DEVICE_RESET_BIT
  to return the reset status */
  return getBit(IQSMemoryMap.SYSTEM_STATUS[0], IQS7222A_DEVICE_RESET_BIT);
}

/**
  * @name	getProductNum
  * @brief  A method that checks the device product number and compares the
  *         result to the defined value to return a boolean result.
  * @param  stopOrRestart -> Specifies whether the communications window must
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval Returns the product number as a 16-bit unsigned integer value.
  * @note   If the product is not correctly identified an appropriate messages 
  *         should be displayed.
  */
uint16_t IQS7222A::getProductNum(bool stopOrRestart)
{
	uint8_t transferBytes[2];	  // A temporary array to hold the byte to be transferred.
  uint8_t prodNumLow = 0;     // Temporary storage for the counts low byte.
  uint8_t prodNumHigh = 0;    // Temporary storage for the counts high byte.
  uint16_t prodNumReturn = 0; // The 16bit return value.
								// Use an array to be consistent with other methods of the library.
	// Read the Device info from the IQS7222A.
	readRandomBytes(IQS7222A_MM_PROD_NUM, 2, transferBytes, stopOrRestart);

  // Construct the 16bit return value.
  prodNumLow = transferBytes[0];
  prodNumHigh = transferBytes[1];
  prodNumReturn = (uint16_t)(prodNumLow);
  prodNumReturn |= (uint16_t)(prodNumHigh<<8);
  // Return the counts value.
  return prodNumReturn;
}

/**
  * @name	  getmajorVersion
  * @brief  A method which checks the device firmware version number, major value.
  * @param  stopOrRestart -> Specifies whether the communications window must 
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval Returns major version number as an 8-bit unsigned integer value.
  */
uint8_t IQS7222A::getmajorVersion(bool stopOrRestart)
{
	uint8_t transferBytes[2];	// A temporary array to hold the byte to be transferred.
  uint8_t ver_maj = 0;      // Temporary storage for the firmware version major number.

	/* Read the Device info from the IQS7222A. */
	readRandomBytes(IQS7222A_MM_MAJOR_VERSION_NUM, 2, transferBytes, stopOrRestart);

  /* Get major value from correct byte */
  ver_maj = transferBytes[0];
  /* Return the major firmware version number value. */
  return ver_maj;
}

/**
  * @name	  getminorVersion
  * @brief  A method which checks the device firmware version number, minor value.
  * @param  stopOrRestart -> Specifies whether the communications window must 
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval Returns minor version number as an 8-bit unsigned integer value.
  */
uint8_t IQS7222A::getminorVersion(bool stopOrRestart)
{
	uint8_t transferBytes[2];	// A temporary array to hold the byte to be transferred.
  uint8_t ver_min = 0;      // Temporary storage for the firmware version minor number.

	/* Read the Device info from the IQ7222A. */
	readRandomBytes(IQS7222A_MM_MINOR_VERSION_NUM, 2, transferBytes, stopOrRestart);
  /* get major value from correct byte */
  ver_min = transferBytes[0];
  /* Return the minor firmware version number value. */
  return ver_min;
}

/**
  * @name	  acknowledgeReset
  * @brief  A method which clears the Show Reset bit by writing a 1 to the 
  *         ACK_RESET bit.
  * @param  stopOrRestart -> Specifies whether the communications window must 
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval None.
  * @note   If a reset has occurred the device settings should be reloaded using
  *         the begin function.After new device settings have been reloaded this
  *         method should be used to clear the reset bit.
  */
void IQS7222A::acknowledgeReset(bool stopOrRestart)
{
	uint8_t transferByte[2];	// A temporary array to hold the bytes to be transferred.
	/* Read the System Flags from the IQS7222A, these must be read first in order
    not to change any settings. */
	readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, RESTART);
	/* Set the ACK_RESET_BIT in the CONTROL_SETTINGS register to clear the
  reset flag */
	transferByte[0] = setBit(transferByte[0], IQS7222A_ACK_RESET_BIT);
	/* Write the new byte to the System Flags address. */
	writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, stopOrRestart);
}

/**
  * @name   ReATI
  * @brief  A method which sets the REDO_ATI_BIT in order to force the IQS7222A
  *         device to run the Automatic Tuning Implementation (ATI) routine.
  * @param  stopOrRestart -> Specifies whether the communications window must
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval None.
  * @note   To force ATI, RE_ATI_BIT in CONTROL_SETTINGS is set.
  */
void IQS7222A::ReATI(bool stopOrRestart)
{
  uint8_t transferByte[2]; // Array to store the bytes transferred.

  readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, RESTART);
  /* Set the RE_ATI_BIT in CONTROL_SETTINGS. */
  transferByte[0] = setBit(transferByte[0], IQS7222A_RE_ATI_BIT);
  /* Write the new byte to the required device. */
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, stopOrRestart);
}

/**
  * @name   SW_Reset
  * @brief  A method that sets the SW RESET bit to force the IQS7222A
  *         device to do a SW reset.
  * @param  stopOrRestart -> Specifies whether the communications window must be
  *                          kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval None.
  * @note   To perform a SW Reset, SW_RESET_BIT in SYSTEM_CONTROL is set.
  */
void IQS7222A::SW_Reset(bool stopOrRestart)
{
  uint8_t transferByte[2]; // Array to store the bytes transferred.

  readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, RESTART);
  /* Set the SW_RESET_BIT in CONTROL_SETTINGS. */
  transferByte[0] = setBit(transferByte[0], IQS7222A_SW_RESET_BIT);
  /* Write the new byte to the required device. */
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, stopOrRestart);
}

/**
  * @name   setStreamMode
  * @brief  A method to set the IQS7222A device into streaming mode.
  * @param  stopOrRestart -> Specifies whether the communications window must
  *                          be kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval None.
  * @note   All other bits at the register address are preserved.
  */
void IQS7222A::setStreamMode(bool stopOrRestart)
{
  uint8_t transferBytes[2]; // The array which will hold the bytes which are transferred.

  // First read the bytes at the memory address so that they can be preserved.
  readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferBytes, RESTART);
  // Set/Clear the INTERFACE_SELECTION_BITS in CONTROL_SETTINGS
  transferBytes[0] = clearBit(transferBytes[0], IQS7222A_INTERFACE_SELECT_BIT_0);
  transferBytes[0] = clearBit(transferBytes[0], IQS7222A_INTERFACE_SELECT_BIT_1);
  // Write the bytes back to the device
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferBytes, stopOrRestart);
}

/**
  * @name   setEventMode
  * @brief  A method to set the IQS7222A device into event mode.
  * @param  stopOrRestart ->  Specifies whether the communications window must
  *                           be kept open or must be closed after this action.
  *                           Use the STOP and RESTART definitions.
  * @retval None.
  * @note   All other bits at the register address are preserved.
  */
void IQS7222A::setEventMode(bool stopOrRestart)
{
  uint8_t transferByte[2]; // The array which will hold the bytes which are transferred.

  /* First read the bytes at the memory address so that they can be preserved */
  readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, RESTART);
  /* Set/Clear the INTERFACE_SELECTION_BITS in CONTROL_SETTINGS */
  transferByte[0] = setBit(transferByte[0], IQS7222A_INTERFACE_SELECT_BIT_0);
  transferByte[0] = clearBit(transferByte[0], IQS7222A_INTERFACE_SELECT_BIT_1);
  /* Write the bytes back to the device */
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferByte, stopOrRestart);
}

/**
  * @name   setStreamInTouchMode
  * @brief  A method to set the IQS7222A device into streaming when in touch mode.
  * @param  stopOrRestart -> Specifies whether the communications window must be
  *                          kept open or must be closed after this action.
  *                          Use the STOP and RESTART definitions.
  * @retval None.
  * @note  All other bits at the register address are preserved.
  */
void IQS7222A::setStreamInTouchMode(bool stopOrRestart)
{
  uint8_t transferBytes[2]; // The array which will hold the bytes which are transferred.

  // First read the bytes at the memory address so that they can be preserved.
  readRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferBytes, RESTART);
  // Set/Clear the INTERFACE_SELECTION_BITS in CONTROL_SETTINGS
  transferBytes[0] = clearBit(transferBytes[0], IQS7222A_INTERFACE_SELECT_BIT_0);
  transferBytes[0] = setBit(transferBytes[0], IQS7222A_INTERFACE_SELECT_BIT_1);
  // Write the bytes back to the device
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 2, transferBytes, stopOrRestart);
}

/**
  * @name   updateInfoFlags
  * @brief  A method that reads the IQS7222A info flags and assigns them to the
  *         SYSTEM_STATUS memory map local variable.
  * @param  stopOrRestart ->  Specifies whether the communications window must
  *                           be kept open or must be closed after this action.
  *              			        Use the STOP and RESTART definitions.
  * @retval None.
  * @note   The SYSTEM_STATUS memory map local variable is altered with the
  *         new value of the info flags register retrieved from the IQS7222A.
  */
void IQS7222A::updateInfoFlags(bool stopOrRestart)
{
	uint8_t transferBytes[2];	// The array which will hold the bytes to be transferred.
	// Read the info flags.
	readRandomBytes(IQS7222A_MM_INFOFLAGS, 2, transferBytes, stopOrRestart);

	// Assign the info flags to the SYSTEM_STATUS memory map local variables.
  IQSMemoryMap.SYSTEM_STATUS[0] =  transferBytes[0];
  IQSMemoryMap.SYSTEM_STATUS[1] =  transferBytes[1];
}

/**
  * @name   get_PowerMode
  * @brief  A method that reads the SYSTEM_STATUS memory map local variable
  *         and returns the current power mode.
  * @param  void
  * @retval Returns the current iqs7222a_power_modes state the device is in.
  * @note   See Datasheet on power mode options and timeouts. 
  *         Normal Power, Low Power and Ultra Low Power (ULP).
  */
iqs7222a_power_modes IQS7222A::get_PowerMode(void)
{
  uint8_t buffer = getBit(IQSMemoryMap.SYSTEM_STATUS[0], IQS7222A_POWER_EVENT_BIT_0);
  buffer += getBit(IQSMemoryMap.SYSTEM_STATUS[0], IQS7222A_POWER_EVENT_BIT_1) << 1;

  if(buffer == IQS7222A_NORMAL_POWER_BIT)
  {
    return IQS7222A_NORMAL_POWER;
  }
  else if(buffer == IQS7222A_LOW_POWER_BIT)
  {
    return IQS7222A_LOW_POWER;
  }
  else if(buffer == IQS7222A_ULP_BIT)
  {
    return IQS7222A_ULP;
  }
}

/**
  * @name   channel_touchState
  * @brief  A method that reads the TOUCH_EVENT_STATES for a given channel and
  *         determines if the channel is in a touch state.
  * @param  channel -> The channel name on the IQS7222A (CH0-CH9, and HALL_TOUCH)
  *                    for which a touch state needs to be determined.
  * @retval Returns true if a touch is active and false if there is no touch.
  * @note   See the IQS7222A_Channel_e typedef for all possible channel names.
  */
bool IQS7222A::channel_touchState(iqs7222a_channel_e channel)
{
  switch(channel)
	{
		case IQS7222A_CH0:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH0_TOUCH_BIT);
		break;

    case IQS7222A_CH1:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH1_TOUCH_BIT);
		break;

    case IQS7222A_CH2:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH2_TOUCH_BIT);
		break;

    case IQS7222A_CH3:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH3_TOUCH_BIT);
		break;

    case IQS7222A_CH4:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH4_TOUCH_BIT);
		break;

    case IQS7222A_CH5:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH5_TOUCH_BIT);
		break;

    case IQS7222A_CH6:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH6_TOUCH_BIT);
		break;

    case IQS7222A_CH7:
      return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[0], IQS7222A_CH7_TOUCH_BIT);
		break;

    case IQS7222A_CH8:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[1], IQS7222A_CH8_TOUCH_BIT);
		break;

    case IQS7222A_CH9:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[1], IQS7222A_CH9_TOUCH_BIT);
		break;

     case IQS7222A_HALL:
			return getBit(IQSMemoryMap.TOUCH_EVENT_STATES[1], IQS7222A_HALL_TOUCH_BIT);
		break;

		default:
			return false;
		break;
	}
}

/**
  * @name   channel_proxState
  * @brief  A method which reads the PROX_EVENT_STATES for a given channel and
  *         determines if the channel is in a proximity state.
  * @param  channel ->  The channel name on the IQS7222A (CH0-CH9 and HALL_TOUCH)
  *                     for which a proximity state needs to be determined.
  * @retval Returns true is proximity is active and false if there is no proximity.
  * @note   See the IQS7222A_Channel_e typedef for all possible channel names.
  */
bool IQS7222A::channel_proxState(iqs7222a_channel_e channel)
{
   switch(channel)
	{
		case IQS7222A_CH0:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH0_PROX_BIT);
		break;

    case IQS7222A_CH1:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH1_PROX_BIT);
		break;

    case IQS7222A_CH2:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH2_PROX_BIT);
		break;

    case IQS7222A_CH3:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH3_PROX_BIT);
		break;

    case IQS7222A_CH4:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH4_PROX_BIT);
		break;

    case IQS7222A_CH5:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH5_PROX_BIT);
		break;

    case IQS7222A_CH6:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH6_PROX_BIT);
		break;

    case IQS7222A_CH7:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[0], IQS7222A_CH7_PROX_BIT);
		break;

    case IQS7222A_CH8:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[1], IQS7222A_CH8_PROX_BIT);
		break;

    case IQS7222A_CH9:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[1], IQS7222A_CH9_PROX_BIT);
		break;

    case IQS7222A_HALL:
			return getBit(IQSMemoryMap.PROX_EVENT_STATES[1], IQS7222A_HALL_PROX_BIT);
		break;

		default:
			return false;
		break;

	}
}

/**
  * @name   sliderCoordinate
  * @brief  A method that reads the coordinates from the SLIDER_0_OUTPUT
  *         memory map local variable and returns calculated slider position.
  * @param  slider -> The slider name on the IQS7222A (Slider 0 or Slider 1).
  * @retval Returns a 16-bit value which contains the slider coordinates from 
  *         0 to the resolution maximum.
  * @note   See the iqs7222a_slider_e typedef for all possible slider names.
  */
uint16_t IQS7222A::sliderCoordinate(iqs7222a_slider_e slider)
{
  uint16_t buffer;
  if(slider == IQS7222A_SLIDER0)
  {
    buffer = IQSMemoryMap.SLIDER_0_OUTPUT[0];
    buffer += IQSMemoryMap.SLIDER_0_OUTPUT[1]<<8;
    return buffer;
  }
  else
  {
    buffer = IQSMemoryMap.SLIDER_1_OUTPUT[0];
    buffer += IQSMemoryMap.SLIDER_1_OUTPUT[1]<<8;
    return buffer;
  }
}

/**
  * @name   slider_event_occurred
  * @brief  A method that tells the user if an event occurred on the given slider.
  * @param  slider -> The slider name on the IQS7222A (Slider 0 or Slider 1).
  * @retval Returns true if the slider event occurred bit has been set by the IQS7222A.
  * @note   See the IQS7222A_slider_e typedef for all possible slider names.
  */
bool IQS7222A::slider_event_occurred(iqs7222a_slider_e slider)
{
  if(slider == IQS7222A_SLIDER0)
  {
    if(getBit(IQSMemoryMap.EVENTS[1], IQS7222A_SLIDER_0_EVENT_BIT))
    {
      return true;
    }
    return false;
  }
  else
  {
    if(getBit(IQSMemoryMap.EVENTS[1], IQS7222A_SLIDER_1_EVENT_BIT))
    {
      return true;
    }
    return false;
  }
}

/**
  * @name   slider_event
  * @brief  A method that gives the type of event that occurred on the given
  *         slider.
  * @param  slider -> The slider name on the IQS7222A (Slider 0 or Slider 1).
  * @retval Returns the slider event that has occurred defined as
  *         iqs7222a_slider_events_e.
  * @note   See the iqs7222a_slider_events_e typedef for all possible slider
  *         events that can occur.
  */
iqs7222a_slider_events_e IQS7222A::slider_event(iqs7222a_slider_e slider)
{
  uint8_t buffer;
  /* Get Slider Event buffer for the selected slider */
  if(slider == IQS7222A_SLIDER0)
  {
    buffer = IQSMemoryMap.SLIDER_0_STATUS[0];
  }
  else
  {
    buffer = IQSMemoryMap.SLIDER_1_STATUS[0];
  }

  /* Find the slider event that occurred */
  if(getBit(buffer, IQS7222A_SLIDER_TAP_BIT))
  {
    return IQS7222A_SLIDER_TAP;
  }
  else if(getBit(buffer, IQS7222A_SLIDER_FLICK_BIT) && getBit(buffer, IQS7222A_SLIDER_NEGATIVE_BIT))
  {
    return IQS7222A_SLIDER_FLICK_LEFT;
  }
  else if(getBit(buffer, IQS7222A_SLIDER_FLICK_BIT) && !(getBit(buffer, IQS7222A_SLIDER_NEGATIVE_BIT)))
  {
    return IQS7222A_SLIDER_FLICK_RIGHT;
  }
  else if(getBit(buffer, IQS7222A_SLIDER_SWIPE_BIT) && getBit(buffer, IQS7222A_SLIDER_NEGATIVE_BIT))
  {
    return IQS7222A_SLIDER_SWIPE_LEFT;
  }
  else if(getBit(buffer, IQS7222A_SLIDER_SWIPE_BIT) && !(getBit(buffer, IQS7222A_SLIDER_NEGATIVE_BIT)))
  {
    return IQS7222A_SLIDER_SWIPE_RIGHT;
  }
  else
  {
    return IQS7222A_SLIDER_NONE;
  }
}
/*****************************************************************************/
/*										     	ADVANCED PUBLIC METHODS				    				  		 */
/*****************************************************************************/

/**
  * @name   writeMM
  * @brief  Function to write the whole memory map to the device (writable) registers
  * @param  stopOrRestart ->  Specifies whether the communications window must
  *                           be kept open or must be closed after this action.
  *                           Use the STOP and RESTART definitions.
  * @note   IQS7222A_init.h -> exported GUI init.h file
  * @retval None.
  */
void IQS7222A::writeMM(bool stopOrRestart)
{
	uint8_t transferBytes[30];	// Temporary array which holds the bytes to be transferred.

  /* Change the Cycle Setup */
  /* Memory Map Position 0x8000 - 0x8403 */
  transferBytes[0] = CYCLE_0_CONV_FREQ_FRAC;
  transferBytes[1] = CYCLE_0_CONV_FREQ_PERIOD;
  transferBytes[2] = CYCLE_0_SETTINGS;
  transferBytes[3] = CYCLE_0_CTX_SELECT;
  transferBytes[4] = CYCLE_0_IREF_0;
  transferBytes[5] = CYCLE_0_IREF_1;
  transferBytes[6] = CYCLE_1_CONV_FREQ_FRAC;
  transferBytes[7] = CYCLE_1_CONV_FREQ_PERIOD;
  transferBytes[8] = CYCLE_1_SETTINGS;
  transferBytes[9] = CYCLE_1_CTX_SELECT;
  transferBytes[10] = CYCLE_1_IREF_0;
  transferBytes[11] = CYCLE_1_IREF_1;
  transferBytes[12] = CYCLE_2_CONV_FREQ_FRAC;
  transferBytes[13] = CYCLE_2_CONV_FREQ_PERIOD;
  transferBytes[14] = CYCLE_2_SETTINGS;
  transferBytes[15] = CYCLE_2_CTX_SELECT;
  transferBytes[16] = CYCLE_2_IREF_0;
  transferBytes[17] = CYCLE_2_IREF_1;
  transferBytes[18] = CYCLE_3_CONV_FREQ_FRAC;
  transferBytes[19] = CYCLE_3_CONV_FREQ_PERIOD;
  transferBytes[20] = CYCLE_3_SETTINGS;
  transferBytes[21] = CYCLE_3_CTX_SELECT;
  transferBytes[22] = CYCLE_3_IREF_0;
  transferBytes[23] = CYCLE_3_IREF_1;
  transferBytes[24] = CYCLE_4_CONV_FREQ_FRAC;
  transferBytes[25] = CYCLE_4_CONV_FREQ_PERIOD;
  transferBytes[26] = CYCLE_4_SETTINGS;
  transferBytes[27] = CYCLE_4_CTX_SELECT;
  transferBytes[28] = CYCLE_4_IREF_0;
  transferBytes[29] = CYCLE_4_IREF_1;
  writeRandomBytes16(IQS7222A_MM_CYCLE_SETUP_0, 30, transferBytes, RESTART);
  Serial.println("\t\t1. Write Cycle Settings");

  /* Change the Global Cycle Setup */
  /* Memory Map Position 0x8500 - 0x8502 */
  transferBytes[0] = GLOBAL_CYCLE_SETUP_0;
  transferBytes[1] = GLOBAL_CYCLE_SETUP_1;
  transferBytes[2] = COARSE_DIVIDER_PRELOAD;
  transferBytes[3] = FINE_DIVIDER_PRELOAD;
  transferBytes[4] = COMPENSATION_PRELOAD_0;
  transferBytes[5] = COMPENSATION_PRELOAD_1;
  writeRandomBytes16(IQS7222A_MM_GLOBAL_CYCLE_SETUP, 6, transferBytes, RESTART);
  Serial.println("\t\t2. Write Global Cycle Settings");

  /* Change the Button Setup 0 - 4 */
  /* Memory Map Position 0x9000 - 0x9502 */
  transferBytes[0] = BUTTON_0_PROX_THRESHOLD;
  transferBytes[1] = BUTTON_0_ENTER_EXIT;
  transferBytes[2] = BUTTON_0_TOUCH_THRESHOLD;
  transferBytes[3] = BUTTON_0_TOUCH_HYSTERESIS;
  transferBytes[4] = BUTTON_0_PROX_EVENT_TIMEOUT;
  transferBytes[5] = BUTTON_0_TOUCH_EVENT_TIMEOUT;
  transferBytes[6] = BUTTON_1_PROX_THRESHOLD;
  transferBytes[7] = BUTTON_1_ENTER_EXIT;
  transferBytes[8] = BUTTON_1_TOUCH_THRESHOLD;
  transferBytes[9] = BUTTON_1_TOUCH_HYSTERESIS;
  transferBytes[10] = BUTTON_1_PROX_EVENT_TIMEOUT;
  transferBytes[11] = BUTTON_1_TOUCH_EVENT_TIMEOUT;
  transferBytes[12] = BUTTON_2_PROX_THRESHOLD;
  transferBytes[13] = BUTTON_2_ENTER_EXIT;
  transferBytes[14] = BUTTON_2_TOUCH_THRESHOLD;
  transferBytes[15] = BUTTON_2_TOUCH_HYSTERESIS;
  transferBytes[16] = BUTTON_2_PROX_EVENT_TIMEOUT;
  transferBytes[17] = BUTTON_2_TOUCH_EVENT_TIMEOUT;
  transferBytes[18] = BUTTON_3_PROX_THRESHOLD;
  transferBytes[19] = BUTTON_3_ENTER_EXIT;
  transferBytes[20] = BUTTON_3_TOUCH_THRESHOLD;
  transferBytes[21] = BUTTON_3_TOUCH_HYSTERESIS;
  transferBytes[22] = BUTTON_3_PROX_EVENT_TIMEOUT;
  transferBytes[23] = BUTTON_3_TOUCH_EVENT_TIMEOUT;
  transferBytes[24] = BUTTON_4_PROX_THRESHOLD;
  transferBytes[25] = BUTTON_4_ENTER_EXIT;
  transferBytes[26] = BUTTON_4_TOUCH_THRESHOLD;
  transferBytes[27] = BUTTON_4_TOUCH_HYSTERESIS;
  transferBytes[28] = BUTTON_4_PROX_EVENT_TIMEOUT;
  transferBytes[29] = BUTTON_4_TOUCH_EVENT_TIMEOUT;
  writeRandomBytes16(IQS7222A_MM_BUTTON_SETUP_0, 30, transferBytes, RESTART);
  Serial.println("\t\t3. Write Button Settings 0 - 4");

  /* Change the Button Setup 5 - 9 */
  /* Memory Map Position 0x9500 - 0x9902 */
  transferBytes[0] = BUTTON_5_PROX_THRESHOLD;
  transferBytes[1] = BUTTON_5_ENTER_EXIT;
  transferBytes[2] = BUTTON_5_TOUCH_THRESHOLD;
  transferBytes[3] = BUTTON_5_TOUCH_HYSTERESIS;
  transferBytes[4] = BUTTON_5_PROX_EVENT_TIMEOUT;
  transferBytes[5] = BUTTON_5_TOUCH_EVENT_TIMEOUT;
  transferBytes[6] = BUTTON_6_PROX_THRESHOLD;
  transferBytes[7] = BUTTON_6_ENTER_EXIT;
  transferBytes[8] = BUTTON_6_TOUCH_THRESHOLD;
  transferBytes[9] = BUTTON_6_TOUCH_HYSTERESIS;
  transferBytes[10] = BUTTON_6_PROX_EVENT_TIMEOUT;
  transferBytes[11] = BUTTON_6_TOUCH_EVENT_TIMEOUT;
  transferBytes[12] = BUTTON_7_PROX_THRESHOLD;
  transferBytes[13] = BUTTON_7_ENTER_EXIT;
  transferBytes[14] = BUTTON_7_TOUCH_THRESHOLD;
  transferBytes[15] = BUTTON_7_TOUCH_HYSTERESIS;
  transferBytes[16] = BUTTON_7_PROX_EVENT_TIMEOUT;
  transferBytes[17] = BUTTON_7_TOUCH_EVENT_TIMEOUT;
  transferBytes[18] = BUTTON_8_PROX_THRESHOLD;
  transferBytes[19] = BUTTON_8_ENTER_EXIT;
  transferBytes[20] = BUTTON_8_TOUCH_THRESHOLD;
  transferBytes[21] = BUTTON_8_TOUCH_HYSTERESIS;
  transferBytes[22] = BUTTON_8_PROX_EVENT_TIMEOUT;
  transferBytes[23] = BUTTON_8_TOUCH_EVENT_TIMEOUT;
  transferBytes[24] = BUTTON_9_PROX_THRESHOLD;
  transferBytes[25] = BUTTON_9_ENTER_EXIT;
  transferBytes[26] = BUTTON_9_TOUCH_THRESHOLD;
  transferBytes[27] = BUTTON_9_TOUCH_HYSTERESIS;
  transferBytes[28] = BUTTON_9_PROX_EVENT_TIMEOUT;
  transferBytes[29] = BUTTON_9_TOUCH_EVENT_TIMEOUT;
  writeRandomBytes16(IQS7222A_MM_BUTTON_SETUP_5, 30, transferBytes, RESTART);
  Serial.println("\t\t4. Write Button Settings 5 - 9");

  /* Change the CH0 Setup */
  /* Memory Map Position 0xA000 - 0xA005 */
  transferBytes[0] = CH0_SETUP_0;
  transferBytes[1] = CH0_SETUP_1;
  transferBytes[2] = CH0_ATI_SETTINGS_0;
  transferBytes[3] = CH0_ATI_SETTINGS_1;
  transferBytes[4] = CH0_MULTIPLIERS_0;
  transferBytes[5] = CH0_MULTIPLIERS_1;
  transferBytes[6] = CH0_ATI_COMPENSATION_0;
  transferBytes[7] = CH0_ATI_COMPENSATION_1;
  transferBytes[8] = CH0_REF_PTR_0;
  transferBytes[9] = CH0_REF_PTR_1;
  transferBytes[10] = CH0_REFMASK_0;
  transferBytes[11] = CH0_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_0, 12, transferBytes, RESTART);
  Serial.println("\t\t5. Write Channel 0 Settings");

  /* Change the CH1 Setup */
  /* Memory Map Position 0xA100 - 0xA105 */
  transferBytes[0] = CH1_SETUP_0;
  transferBytes[1] = CH1_SETUP_1;
  transferBytes[2] = CH1_ATI_SETTINGS_0;
  transferBytes[3] = CH1_ATI_SETTINGS_1;
  transferBytes[4] = CH1_MULTIPLIERS_0;
  transferBytes[5] = CH1_MULTIPLIERS_1;
  transferBytes[6] = CH1_ATI_COMPENSATION_0;
  transferBytes[7] = CH1_ATI_COMPENSATION_1;
  transferBytes[8] = CH1_REF_PTR_0;
  transferBytes[9] = CH1_REF_PTR_1;
  transferBytes[10] = CH1_REFMASK_0;
  transferBytes[11] = CH1_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_1, 12, transferBytes, RESTART);
  Serial.println("\t\t6. Write Channel 1 Settings");

  /* Change the CH2 Setup */
  /* Memory Map Position 0xA200 - 0xA205 */
  transferBytes[0] = CH2_SETUP_0;
  transferBytes[1] = CH2_SETUP_1;
  transferBytes[2] = CH2_ATI_SETTINGS_0;
  transferBytes[3] = CH2_ATI_SETTINGS_1;
  transferBytes[4] = CH2_MULTIPLIERS_0;
  transferBytes[5] = CH2_MULTIPLIERS_1;
  transferBytes[6] = CH2_ATI_COMPENSATION_0;
  transferBytes[7] = CH2_ATI_COMPENSATION_1;
  transferBytes[8] = CH2_REF_PTR_0;
  transferBytes[9] = CH2_REF_PTR_1;
  transferBytes[10] = CH2_REFMASK_0;
  transferBytes[11] = CH2_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_2, 12, transferBytes, RESTART);
  Serial.println("\t\t7. Write Channel 2 Settings");

  /* Change the CH3 Setup */
  /* Memory Map Position 0xA300 - 0xA305 */
  transferBytes[0] = CH3_SETUP_0;
  transferBytes[1] = CH3_SETUP_1;
  transferBytes[2] = CH3_ATI_SETTINGS_0;
  transferBytes[3] = CH3_ATI_SETTINGS_1;
  transferBytes[4] = CH3_MULTIPLIERS_0;
  transferBytes[5] = CH3_MULTIPLIERS_1;
  transferBytes[6] = CH3_ATI_COMPENSATION_0;
  transferBytes[7] = CH3_ATI_COMPENSATION_1;
  transferBytes[8] = CH3_REF_PTR_0;
  transferBytes[9] = CH3_REF_PTR_1;
  transferBytes[10] = CH3_REFMASK_0;
  transferBytes[11] = CH3_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_3, 12, transferBytes, RESTART);
  Serial.println("\t\t8. Write Channel 3 Settings");

  /* Change the CH4 Setup */
  /* Memory Map Position 0xA400 - 0xA405 */
  transferBytes[0] = CH4_SETUP_0;
  transferBytes[1] = CH4_SETUP_1;
  transferBytes[2] = CH4_ATI_SETTINGS_0;
  transferBytes[3] = CH4_ATI_SETTINGS_1;
  transferBytes[4] = CH4_MULTIPLIERS_0;
  transferBytes[5] = CH4_MULTIPLIERS_1;
  transferBytes[6] = CH4_ATI_COMPENSATION_0;
  transferBytes[7] = CH4_ATI_COMPENSATION_1;
  transferBytes[8] = CH4_REF_PTR_0;
  transferBytes[9] = CH4_REF_PTR_1;
  transferBytes[10] = CH4_REFMASK_0;
  transferBytes[11] = CH4_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_4, 12, transferBytes, RESTART);
  Serial.println("\t\t9. Write Channel 4 Settings");

  /* Change the CH5 Setup */
  /* Memory Map Position 0xA500 - 0xA505 */
  transferBytes[0] = CH5_SETUP_0;
  transferBytes[1] = CH5_SETUP_1;
  transferBytes[2] = CH5_ATI_SETTINGS_0;
  transferBytes[3] = CH5_ATI_SETTINGS_1;
  transferBytes[4] = CH5_MULTIPLIERS_0;
  transferBytes[5] = CH5_MULTIPLIERS_1;
  transferBytes[6] = CH5_ATI_COMPENSATION_0;
  transferBytes[7] = CH5_ATI_COMPENSATION_1;
  transferBytes[8] = CH5_REF_PTR_0;
  transferBytes[9] = CH5_REF_PTR_1;
  transferBytes[10] = CH5_REFMASK_0;
  transferBytes[11] = CH5_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_5, 12, transferBytes, RESTART);
  Serial.println("\t\t10. Write Channel 5 Settings");

  /* Change the CH6 Setup */
  /* Memory Map Position 0xA600 - 0xA605 */
  transferBytes[0] = CH6_SETUP_0;
  transferBytes[1] = CH6_SETUP_1;
  transferBytes[2] = CH6_ATI_SETTINGS_0;
  transferBytes[3] = CH6_ATI_SETTINGS_1;
  transferBytes[4] = CH6_MULTIPLIERS_0;
  transferBytes[5] = CH6_MULTIPLIERS_1;
  transferBytes[6] = CH6_ATI_COMPENSATION_0;
  transferBytes[7] = CH6_ATI_COMPENSATION_1;
  transferBytes[8] = CH6_REF_PTR_0;
  transferBytes[9] = CH6_REF_PTR_1;
  transferBytes[10] = CH6_REFMASK_0;
  transferBytes[11] = CH6_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_6, 12, transferBytes, RESTART);
  Serial.println("\t\t11. Write Channel 6 Settings");

  /* Change the CH7 Setup */
  /* Memory Map Position 0xA700 - 0xA705 */
  transferBytes[0] = CH7_SETUP_0;
  transferBytes[1] = CH7_SETUP_1;
  transferBytes[2] = CH7_ATI_SETTINGS_0;
  transferBytes[3] = CH7_ATI_SETTINGS_1;
  transferBytes[4] = CH7_MULTIPLIERS_0;
  transferBytes[5] = CH7_MULTIPLIERS_1;
  transferBytes[6] = CH7_ATI_COMPENSATION_0;
  transferBytes[7] = CH7_ATI_COMPENSATION_1;
  transferBytes[8] = CH7_REF_PTR_0;
  transferBytes[9] = CH7_REF_PTR_1;
  transferBytes[10] = CH7_REFMASK_0;
  transferBytes[11] = CH7_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_7, 12, transferBytes, RESTART);
  Serial.println("\t\t12. Write Channel 7 Settings");

  /* Change the CH8 Setup */
  /* Memory Map Position 0xA800 - 0xA805 */
  transferBytes[0] = CH8_SETUP_0;
  transferBytes[1] = CH8_SETUP_1;
  transferBytes[2] = CH8_ATI_SETTINGS_0;
  transferBytes[3] = CH8_ATI_SETTINGS_1;
  transferBytes[4] = CH8_MULTIPLIERS_0;
  transferBytes[5] = CH8_MULTIPLIERS_1;
  transferBytes[6] = CH8_ATI_COMPENSATION_0;
  transferBytes[7] = CH8_ATI_COMPENSATION_1;
  transferBytes[8] = CH8_REF_PTR_0;
  transferBytes[9] = CH8_REF_PTR_1;
  transferBytes[10] = CH8_REFMASK_0;
  transferBytes[11] = CH8_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_8, 12, transferBytes, RESTART);
  Serial.println("\t\t13. Write Channel 8 Settings");

  /* Change the CH9 Setup */
  /* Memory Map Position 0xA900 - 0xA905 */
  transferBytes[0] = CH9_SETUP_0;
  transferBytes[1] = CH9_SETUP_1;
  transferBytes[2] = CH9_ATI_SETTINGS_0;
  transferBytes[3] = CH9_ATI_SETTINGS_1;
  transferBytes[4] = CH9_MULTIPLIERS_0;
  transferBytes[5] = CH9_MULTIPLIERS_1;
  transferBytes[6] = CH9_ATI_COMPENSATION_0;
  transferBytes[7] = CH9_ATI_COMPENSATION_1;
  transferBytes[8] = CH9_REF_PTR_0;
  transferBytes[9] = CH9_REF_PTR_1;
  transferBytes[10] = CH9_REFMASK_0;
  transferBytes[11] = CH9_REFMASK_1;
  writeRandomBytes16(IQS7222A_MM_CHANNEL_SETUP_9, 12, transferBytes, RESTART);
  Serial.println("\t\t14. Write Channel 9 Settings");

  /* Change the Filter Betas */
  /* Memory Map Position 0xAA00 - 0xAA01 */
  transferBytes[0] = COUNTS_BETA_FILTER;
  transferBytes[1] = LTA_BETA_FILTER;
  transferBytes[2] = LTA_FAST_BETA_FILTER;
  transferBytes[3] = RESERVED_FILTER_0;
  writeRandomBytes16(IQS7222A_MM_FILTER_BETAS, 4, transferBytes, RESTART);
  Serial.println("\t\t15. Write Filter Betas");

  /* Change the Slider/Wheel 0 Setup 0 & Delta Link */
  /* Memory Map Position 0xB000 - 0xB009 */
  transferBytes[0] = SLIDER0SETUP_GENERAL;
  transferBytes[1] = SLIDER0_LOWER_CAL;
  transferBytes[2] = SLIDER0_UPPER_CAL;
  transferBytes[3] = SLIDER0_BOTTOM_SPEED;
  transferBytes[4] = SLIDER0_TOPSPEED_0;
  transferBytes[5] = SLIDER0_RESOLUTION_0;
  transferBytes[6] = SLIDER0_ENABLE_MASK_0_7;
  transferBytes[7] = SLIDER0_ENABLE_MASK_8_11;
  transferBytes[8] = SLIDER0_ENABLESTATUSLINK_0;
  transferBytes[9] = SLIDER0_ENABLESTATUSLINK_1;
  transferBytes[10] = SLIDER0_DELTA0_0;
  transferBytes[11] = SLIDER0_DELTA0_1;
  transferBytes[12] = SLIDER0_DELTA1_0;
  transferBytes[13] = SLIDER0_DELTA1_1;
  transferBytes[14] = SLIDER0_DELTA2_0;
  transferBytes[15] = SLIDER0_DELTA2_1;
  transferBytes[16] = SLIDER0_DELTA3_0;
  transferBytes[17] = SLIDER0_DELTA3_1;
  transferBytes[18] = SLIDER0_GESTURE_ENABLE;
  transferBytes[19] = SLIDER0_MAX_TAP_TIME;
  transferBytes[20] = SLIDER0_MAX_SWIPE_TIME;
  transferBytes[21] = SLIDER0_MAX_SWIPE_DISTANCE;
  writeRandomBytes16(IQS7222A_MM_SLIDER_SETUP_0, 22, transferBytes, RESTART);
  Serial.println("\t\t16. Slider/Wheel 0 Settings");

  /* Change the Slider/Wheel 1 Setup 0 */
  /* Memory Map Position 0xB100 - 0xB105 */
  transferBytes[0] = SLIDER0SETUP_GENERAL;
  transferBytes[1] = SLIDER1_LOWER_CAL;
  transferBytes[2] = SLIDER1_UPPER_CAL;
  transferBytes[3] = SLIDER1_BOTTOM_SPEED;
  transferBytes[4] = SLIDER1_TOPSPEED_0;
  transferBytes[5] = SLIDER1_RESOLUTION_0;
  transferBytes[6] = SLIDER1_ENABLE_MASK_0_7;
  transferBytes[7] = SLIDER1_ENABLE_MASK_8_11;
  transferBytes[8] = SLIDER1_ENABLESTATUSLINK_0;
  transferBytes[9] = SLIDER1_ENABLESTATUSLINK_1;
  transferBytes[10] = SLIDER1_DELTA0_0;
  transferBytes[11] = SLIDER1_DELTA0_1;
  transferBytes[12] = SLIDER1_DELTA1_0;
  transferBytes[13] = SLIDER1_DELTA1_1;
  transferBytes[14] = SLIDER1_DELTA2_0;
  transferBytes[15] = SLIDER1_DELTA2_1;
  transferBytes[16] = SLIDER1_DELTA3_0;
  transferBytes[17] = SLIDER1_DELTA3_1;
  transferBytes[18] = SLIDER1_GESTURE_ENABLE;
  transferBytes[19] = SLIDER1_MAX_TAP_TIME;
  transferBytes[20] = SLIDER1_MAX_SWIPE_TIME;
  transferBytes[21] = SLIDER1_MAX_SWIPE_DISTANCE;
  writeRandomBytes16(IQS7222A_MM_SLIDER_SETUP_1, 22, transferBytes, RESTART);
  Serial.println("\t\t17. Slider/Wheel 1 Settings");

  /* Change the GPIO 0 Settings */
  /* Memory Map Position 0xC000 - 0xC002 */
  transferBytes[0] = GPIO0_SETUP_0;
  transferBytes[1] = GPIO0_SETUP_1;
  transferBytes[2] = ENABLE_MASK_0_7;
  transferBytes[3] = ENABLE_MASK_8_11;
  transferBytes[4] = ENABLESTATUSLINK_0;
  transferBytes[5] = ENABLESTATUSLINK_1;
  writeRandomBytes16(IQS7222A_MM_GPIO_SETTINGS, 6, transferBytes, RESTART);
  Serial.println("\t\t18. GPIO 0 Settings");

  /* Change the System Settings */
  /* Memory Map Position 0xD0 - 0xD9 */
  transferBytes[0] = SYSTEM_CONTROL_0;
  transferBytes[1] = SYSTEM_CONTROL_1;
  transferBytes[2] = ATI_ERROR_TIMEOUT_0;
  transferBytes[3] = ATI_ERROR_TIMEOUT_1;
  transferBytes[4] = ATI_REPORT_RATE_0;
  transferBytes[5] = ATI_REPORT_RATE_1;
  transferBytes[6] = NORMAL_MODE_TIMEOUT_0;
  transferBytes[7] = NORMAL_MODE_TIMEOUT_1;
  transferBytes[8] = NORMAL_MODE_REPORT_RATE_0;
  transferBytes[9] = NORMAL_MODE_REPORT_RATE_1;
  transferBytes[10] = LP_MODE_TIMEOUT_0;
  transferBytes[11] = LP_MODE_TIMEOUT_1;
  transferBytes[12] = LP_MODE_REPORT_RATE_0;
  transferBytes[13] = LP_MODE_REPORT_RATE_1;
  transferBytes[14] = ULP_MODE_TIMEOUT_0;
  transferBytes[15] = ULP_MODE_TIMEOUT_1;
  transferBytes[16] = ULP_MODE_REPORT_RATE_0;
  transferBytes[17] = ULP_MODE_REPORT_RATE_1;
  transferBytes[18] = TOUCH_PROX_EVENT_MASK;
  transferBytes[19] = POWER_ATI_SLIDER_EVENT_MASK;
  writeRandomBytes(IQS7222A_MM_CONTROL_SETTINGS, 20, transferBytes, stopOrRestart);
  Serial.println("\t\t19. System Settings");
}

/*****************************************************************************/
/*                           PRIVATE METHODS                                 */
/*****************************************************************************/

/**
 * @name    readRandomBytes
 * @brief   A method that reads a specified number of bytes from a specified
 *          address and saves it into a user-supplied array. This method is used
 *          by all other methods in this class which read data from the IQS7222A
 *          device.
 * @param   memoryAddress -> The memory address at which to start reading bytes
 *                           from.  See the "iqs7222a_addresses.h" file.
 * @param   numBytes      -> The number of bytes that must be read.
 * @param   bytesArray    -> The array which will store the bytes to be read,
 *                           this array will be overwritten.
 * @param   stopOrRestart -> A boolean that specifies whether the communication
 *                           window should remain open or be closed after transfer.
 *                           False keeps it open, true closes it. Use the STOP and
 *                           RESTART definitions.
 * @retval  No value is returned, however, the user-supplied array is overwritten.
 * @note    Uses standard Arduino "Wire" library which is for I2C communication.
 *          Take note that C++ cannot return an array, therefore, the array which
 *          is passed as an argument is overwritten with the required values.
 *          Pass an array to the method by using only its name, e.g. "bytesArray",
 *          without the brackets, this passes a pointer to the array.
 */
void IQS7222A::readRandomBytes(uint8_t memoryAddress, uint8_t numBytes,
                               uint8_t bytesArray[], bool stopOrRestart)
{
  // A simple counter to assist with loading bytes into the user-supplied array.
  uint8_t i = 0;
  // Select the device with the address of "_deviceAddress" and start communication.
  Wire.beginTransmission(_deviceAddress);
  // Send a bit asking for the "memoryAddress" register.
  Wire.write(memoryAddress);
  // Complete the selection and communication initialization.
  Wire.endTransmission(RESTART);  // Restart transmission for reading that follows.
  /* The required device has now been selected and it has been told which register
  to send information from. */

  // Request "numBytes" bytes from the device which has the address "_deviceAddress"
  do
  {
    Wire.requestFrom((int)_deviceAddress, (int)numBytes, (int)stopOrRestart);
  }while(Wire.available() == 0);  // Wait for response, this sometimes takes a few attempts

  // Load the received bytes into the array until there are no more
  while(Wire.available())
  {
    // Load the received bytes into the user-supplied array
    bytesArray[i] = Wire.read();
    i++;
  }

  /* Always manually close the RDY window after a STOP is sent to prevent
  writing while ready closes */
  if(stopOrRestart == STOP)
  {
    iqs7222a_deviceRDY = false;
  }

}

/**
  * @name   writeRandomBytes
  * @brief  A method that writes a specified number of bytes to a specified
  *         address, the bytes to write are supplied using an array pointer.
  *         This method is used by all other methods of this class which
  *         write data to the IQS7222A device.
  * @param  memoryAddress -> The memory address at which to start writing the
  *                          bytes to. See the "iqs7222a_addresses.h" file.
  * @param   numBytes      -> The number of bytes that must be written.
  * @param   bytesArray    -> The array which stores the bytes which will be
  *                           written to the memory location.
  * @param   stopOrRestart -> A boolean that specifies whether the communication
  *                           window should remain open or be closed of transfer.
  *                           False keeps it open, true closes it. Use the STOP
  *                           and RESTART definitions.
  * @retval No value is returned, only the IQS device registers are altered.
  * @note   Uses standard Arduino "Wire" library which is for I2C communication.
  *         Take note that a full array cannot be passed to a function in C++.
  *         Pass an array to the function by using only its name, e.g. "bytesArray",
  *         without the square brackets, this passes a pointer to the
  *         array. The values to be written must be loaded into the array prior
  *         to passing it to the function.
  */
void IQS7222A::writeRandomBytes(uint8_t memoryAddress, uint8_t numBytes, uint8_t bytesArray[], bool stopOrRestart)
{
  // Select the device with the address of "_deviceAddress" and start communication.
  Wire.beginTransmission(_deviceAddress);
  /* Specify the memory address where the IQS7222A must start saving the data,
  as designated by the "memoryAddress" variable. */
  Wire.write(memoryAddress);
  /* Write the bytes as specified in the array which the "arrayAddress" pointer
  points to */
  for(int i=0; i<numBytes; i++)
  {
    Wire.write(bytesArray[i]);
  }
  // End the transmission, user decides to STOP or RESTART.
  Wire.endTransmission(stopOrRestart);

    /* Always manually close the RDY window after a STOP is sent to prevent
    writing while ready closes */
  if(stopOrRestart == STOP)
  {
    iqs7222a_deviceRDY = false;
  }
}

/**
  * @name   writeRandomBytes
  * @brief  A method that writes a specified number of bytes to a specified
  *         address, the bytes to write are supplied using an array pointer.
  *         This method is used by all other methods of this class that write
  *         data to the IQS7222A device.
  * @param  memoryAddress -> The memory address at which to start writing the bytes
  *                          to. See the "iqs7222a_addresses.h" file.
  * @param  numBytes      -> The number of bytes that must be written.
  * @param  bytesArray    -> The array which stores the bytes which will be
  *                          written to the memory location.
  * @param  stopOrRestart -> A boolean that specifies whether the communication
  *                          window should remain open or be closed of transfer.
  *                          False keeps it open, true closes it. Use the STOP
  *                          and RESTART definitions.
  * @retval No value is returned, only the IQS device registers are altered.
  * @note   Uses standard Arduino "Wire" library which is for I2C communication.
  *         Take note that a full array cannot be passed to a function in C++.
  *         Pass an array to the function by using only its name, e.g. "bytesArray",
  *         without the square brackets, this passes a pointer to the
  *         array. The values to be written must be loaded into the array prior
  *         to passing it to the function.
  */
void IQS7222A::writeRandomBytes16(uint16_t memoryAddress, uint8_t numBytes, uint8_t bytesArray[], bool stopOrRestart)
{
  /* Select the device with the address of "_deviceAddress" and start
  communication. */
  Wire.beginTransmission(_deviceAddress);
  /* Specify the memory address where the IQS7222A must start saving the data,
  as designated by the "memoryAddress" variable. */
  uint8_t addr_h, addr_l;
  addr_h = memoryAddress >> 8;
  addr_l = memoryAddress;
  Wire.write(addr_h);
  Wire.write(addr_l);
  /* Write the bytes as specified in the array which the "arrayAddress" pointer
  points to. */
  for(int i=0; i<numBytes; i++)
  {
    Wire.write(bytesArray[i]);
  }
  // End the transmission, user decides to STOP or RESTART.
  Wire.endTransmission(stopOrRestart);

    /* Always manually close the RDY window after a STOP is sent to prevent
    writing while ready closes */
  if(stopOrRestart == STOP)
  {
    iqs7222a_deviceRDY = false;
  }
}

/**
  * @name   getBit
  * @brief  A method that returns the chosen bit value of the provided byte.
  * @param  data       -> byte of which a given bit value needs to be calculated.
  * @param  bit_number -> a number between 0 and 7 representing the bit in question.
  * @retval The boolean value of the specific bit requested. 
  */
bool IQS7222A::getBit(uint8_t data, uint8_t bit_number)
{
  return (data & ( 1 << bit_number )) >> bit_number;
}

/**
  * @name   setBit
  * @brief  A method that returns the chosen bit value of the provided byte.
  * @param  data       -> byte of which a given bit value needs to be calculated.
  * @param  bit_number -> a number between 0 and 7 representing the bit in question.
  * @retval Returns an 8-bit unsigned integer value of the given data byte with 
  *         the requested bit set.
  */
uint8_t IQS7222A::setBit(uint8_t data, uint8_t bit_number)
{
	return (data |= 1UL << bit_number);
}

/**
  * @name   clearBit
  * @brief  A method that returns the chosen bit value of the provided byte.
  * @param  data       -> byte of which a given bit value needs to be calculated.
  * @param  bit_number -> a number between 0 and 7 representing the bit in question.
  * @retval Returns an 8-bit unsigned integer value of the given data byte with 
  *         the requested bit cleared.
  */
uint8_t IQS7222A::clearBit(uint8_t data, uint8_t bit_number)
{
	return (data &= ~(1UL << bit_number));
}

/**
  * @name   force_I2C_communication
  * @brief  A method which writes to memory address 0xFF to open a 
  *         communication window on the IQS7222A.
  * @param  None
  * @retval None
  * @note   Uses standard Arduino "Wire" library which is for I2C communication.
  */
void IQS7222A::force_I2C_communication(void)
{
    /* Ensure RDY is HIGH at the moment */
  if (!iqs7222a_deviceRDY)
  {
    /* Select the device with the address of "DEMO_IQS7222A_ADDR" and start
    communication. */
    Wire.beginTransmission(_deviceAddress);

    /* Write to memory address 0xFF that will prompt the IQS7222A to open a
    communication window.*/
    Wire.write(0xFF);

    /* End the transmission, user decides to STOP or RESTART. */
    Wire.endTransmission(STOP);
  }
}
