/******************************************************************************
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
 * @file        iqs7222a-example-code.ino                                     *
 * @brief       IQS7222A Inductive Coil and Slider Example code This example  *
 *              demonstrates how to write the desired settings to the         *
 *              IQS7222A to use the Inductive coil and slider EV-Kit.         *
 *              All data is displayed over serial communication               *
 *              with the following outputs:                                   *
 *                - Proximity and touch detection on Channel 0 (Large Coil)   *
 *                - Proximity and touch detection on Channel 7 (Small Coil)   *
 *                - Slider Gestures                                           *
 *                    * Swipe and Flick Left  <-                              *
 *                    * Swipe and Flick Right ->                              *
 *                    * Tap                                                   *
 *                - Power Mode Switching (if enabled)                         *
 *                - Force Communication when 'f\n' is sent over Serial        *
 *                - Software Reset when 'r\n' is sent over Serial             *
 * @author      JN. Lochner - Azoteq PTY Ltd                                  *
 * @version     v1.1                                                          *
 * @date        2023-07-14                                                    *
 ******************************************************************************/
#include <Arduino.h>
#include "src\IQS7222A\IQS7222A.h"

/*** Defines ***/
#define DEMO_IQS7222A_ADDR                     0x44
#define DEMO_IQS7222A_POWER_PIN                4
#define DEMO_IQS7222A_RDY_PIN                  7

/*** Instances ***/
IQS7222A iqs7222a;
iqs7222a_ch_states ch1_state    = IQS7222A_CH_NONE;
iqs7222a_ch_states ch9_state    = IQS7222A_CH_NONE;
iqs7222a_ch_states hall_state   = IQS7222A_CH_NONE;
iqs7222a_power_modes power_mode = IQS7222A_NORMAL_POWER;

/*** Global Variables ***/
bool slider_event = false;
bool show_data    = false;

void setup()
{
  /* Start Serial Communication */
  Serial.begin(115200);
  while(!Serial);
  Serial.println("Start Serial communication");

  /* Power On IQS7222A */
  pinMode(DEMO_IQS7222A_POWER_PIN, OUTPUT);
  delay(200);
  digitalWrite(DEMO_IQS7222A_POWER_PIN, LOW);
  delay(200);
  digitalWrite(DEMO_IQS7222A_POWER_PIN, HIGH);

  /* Initialize the IQS7222A with input parameters, device address, and RDY pin */
  iqs7222a.begin(DEMO_IQS7222A_ADDR, DEMO_IQS7222A_RDY_PIN);
  Serial.println("Device Ready");
  delay(1);
}

void loop()
{
  iqs7222a.run(); // Runs the IQS7222A

  force_comms_and_reset(); // function to initialize a force communication window.

  /* Process data read from IQS7222A when new data is available (RDY Line Low) */
  if(iqs7222a.new_data_available)
  {
    check_power_mode();
    read_slider_event();
    check_channel_1_state();
    check_channel_9_state();
    check_hall_effect_state();
    show_iqs7222a_data();
    iqs7222a.new_data_available = false;
  }
}

/* Function to check when the Current power mode of the IQS7222A changed */
void check_power_mode(void)
{
  iqs7222a_power_modes buffer = iqs7222a.get_PowerMode(); // read current power mode
  if(buffer != power_mode)
  {
    switch(buffer)
    {
      case IQS7222A_NORMAL_POWER:
        Serial.println("NORMAL POWER Activated!");
        break;
      case IQS7222A_LOW_POWER:
        Serial.println("LOW POWER Activated!");
        break;
      case IQS7222A_ULP:
        Serial.println("ULP Activated!");
        break;
    }

    /* Update the running state */
    power_mode = buffer;
  }
}

/* Function to check the Proximity and Touch states of an inductive channel
(large coil) */
void check_channel_1_state(void)
{
  /* check if the touch state bit is set for channel 1 */
  if(iqs7222a.channel_touchState(IQS7222A_CH1))
  {
    if(ch1_state != IQS7222A_CH_TOUCH)
    {
      Serial.println("CH1: Touch");
      ch1_state = IQS7222A_CH_TOUCH;
    }
  }
  /* check if the proximity state bit is set for channel 1 */
  else if (iqs7222a.channel_proxState(IQS7222A_CH1))
  {
    if(ch1_state != IQS7222A_CH_PROX)
    {
      Serial.println("CH1: Prox");
      ch1_state = IQS7222A_CH_PROX;
    }
  }
  /* None state if neither the touch nor proximity bit was set to 1 */
  else
  {
    if(ch1_state != IQS7222A_CH_NONE)
    {
      Serial.println("CH1: None");
      ch1_state = IQS7222A_CH_NONE;
    }
  }
}

/* Function to check the Proximity and Touch states of an inductive channel
(small coil) */
void check_channel_9_state(void)
{
  /* check if the touch state bit is set for channel 9 */
  if(iqs7222a.channel_touchState(IQS7222A_CH9))
  {
    if(ch9_state != IQS7222A_CH_TOUCH)
    {
      Serial.println("CH9: Touch");
      ch9_state = IQS7222A_CH_TOUCH;
    }
  }
  /* check if the proximity state bit is set for channel 9 */
  else if (iqs7222a.channel_proxState(IQS7222A_CH9))
  {
    if(ch9_state != IQS7222A_CH_PROX)
    {
      Serial.println("CH9: Prox");
      ch9_state = IQS7222A_CH_PROX;
    }
  }
  /* None state if neither the touch nor proximity bit was set to 1 */
  else
  {
    if(ch9_state != IQS7222A_CH_NONE)
    {
      Serial.println("CH9: None");
      ch9_state = IQS7222A_CH_NONE;
    }
  }
}

/* Function to check the Proximity and Touch states of the hall effect on
channel 10 */
void check_hall_effect_state(void)
{
  /* check if the touch state bit is set for channel 10 */
  if(iqs7222a.channel_touchState(IQS7222A_HALL))
  {
    if(hall_state != IQS7222A_CH_TOUCH)
    {
      Serial.println("HALL: Touch");
      hall_state = IQS7222A_CH_TOUCH;
    }
  }
  /* check if the proximity state bit is set for channel 10 */
  else if (iqs7222a.channel_proxState(IQS7222A_HALL))
  {
    if(hall_state != IQS7222A_CH_PROX)
    {
      Serial.println("HALL: Prox");
      hall_state = IQS7222A_CH_PROX;
    }
  }
  /* None state if neither the touch nor proximity bit was set to 1 */
  else
  {
    if(hall_state != IQS7222A_CH_NONE)
    {
      Serial.println("HALL: None");
      hall_state = IQS7222A_CH_NONE;
    }
  }
}

/* Function to process Slider events */
void read_slider_event(void)
{
  /* Read slider bit to check if a slider event occurred */
  slider_event = iqs7222a.slider_event_occurred(IQS7222A_SLIDER0);

  if(slider_event)
  {
    iqs7222a_slider_events_e slider_event_type = iqs7222a.slider_event(IQS7222A_SLIDER0); // returns slider event that occurred (tap, swipe, or flick) by reading event bits from MM
    switch(slider_event_type)
    {
      case IQS7222A_SLIDER_TAP:
        Serial.println("SLIDER 0: Tap");
        break;
      case IQS7222A_SLIDER_SWIPE_LEFT:
        Serial.println("SLIDER 0: Swipe <-");
        break;
      case IQS7222A_SLIDER_SWIPE_RIGHT:
        Serial.println("SLIDER 0: Swipe ->");
        break;
      case IQS7222A_SLIDER_FLICK_LEFT:
        Serial.println("SLIDER 0: Flick <-");
        break;
      case IQS7222A_SLIDER_FLICK_RIGHT:
        Serial.println("SLIDER 0: Flick ->");
        break;
      case IQS7222A_SLIDER_NONE:
        Serial.println("SLIDER 0: None");
        break;
    }
    slider_event = false;
  }
}

/* Force the IQS7222A to open a RDY window and read the current state of the
  device */
void force_comms_and_reset(void)
{
  char message = read_message();

  /* If an 'f' was received over serial, open a forced communication window
  and print the new data received */
  if(message == 'f')
  {
    iqs7222a.force_I2C_communication(); // Prompt the IQS7222A
    show_data = true;
  }

  /* If an 'r' was received over serial, request a software(SW) reset */
  if(message == 'r')
  {
    Serial.println("Software Reset Requested!");
    iqs7222a.force_I2C_communication(); // Request a RDY window
    iqs7222a.iqs7222a_state.state = IQS7222A_STATE_SW_RESET;
    power_mode = IQS7222A_NORMAL_POWER;
  }
}

/* Read message sent over serial communication */
char read_message(void)
{
  while (Serial.available())
  {
    if(Serial.available() > 0)
    {
      return  Serial.read();
    }
  }

  return '\n';
}

/* shows the current IQS7222A data */
void show_iqs7222a_data()
{
  if(show_data)
  {
    Serial.println("********* IQS7222A DATA *********");
    Serial.println("1. Channel 1:");
    Serial.print("\t- ");
    ch1_state = IQS7222A_CH_UNKNOWN;
    check_channel_1_state();
    Serial.println("2. Channel 9:");
    Serial.print("\t- ");
    ch9_state = IQS7222A_CH_UNKNOWN;
    check_channel_9_state();
    Serial.println("3. SLIDER 0: ");
    Serial.print("\t- Coordinate: ");
    Serial.println(iqs7222a.sliderCoordinate(IQS7222A_SLIDER0));
    Serial.println("4. Hall: ");
    Serial.print("\t- ");
    hall_state = IQS7222A_CH_UNKNOWN;
    check_hall_effect_state();
    Serial.println("5. Power:");
    Serial.print("\t- ");
    power_mode = IQS7222A_POWER_UNKNOWN;
    check_power_mode();
    Serial.println("*********************************");
    show_data = false;
  }
}
