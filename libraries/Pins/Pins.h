/**
 * @file Pins.h
 * @author IR
 * @brief Update, set, and get predefined pin values
 * @version 0.1
 * @date 2020-11-11
 * 
 * @copyright Copyright (c) 2020
 * 
 * To define pin values refer to PinConfig.def
 * 
 */

#ifndef __ECU_PINS_H__
#define __ECU_PINS_H__

// IMPROVE: pin priority

#include "PinConfig.def"
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Get and set values to predefined pins.
 * 
 * Manage defined pins and ensure only defined pins are accessed
 * 
 * Refer to Pins.h for more info.
 */
namespace Pins {

/**
 * @brief A typedef for pin handler functions
 */
typedef void (*PinHandler)(uint8_t CAN_GPIO_Pin, int &value);

/**
 * @brief Get the pin value of a predefined canbus pin
 * 
 * @note Pins must first be defined in PinConfig.def
 * 
 * @param CAN_GPIO_Pin The canbus GPIO pin to get a value from
 * @return int Returns an int that represents either a digital or analog value
 */
int getCanPinValue(uint8_t CAN_GPIO_Pin);

/**
 * @brief Get the pin value of a predefined pin
 * 
 * @param GPIO_Pin The GPIO pin to get a value from
 * @return int Returns an int that represents either a digital or analog value
 */
int getPinValue(uint8_t GPIO_Pin);

/**
 * @brief Set the pin value of a predefined pin
 * 
 * @param GPIO_Pin The GPIO pin to set
 * @param value The value to set the analog/digital pin to
 */
void setPinValue(uint8_t GPIO_Pin, int value);

/**
 * @brief Set the value of an internal pin
 * @details These are fake pins that are usefully for sending gpio like values over canbus, See PinConfig.def for more info
 * 
 * @param Internal_Pin The Internal pin to set
 * @param value The value to set the analog/digital pin to
 */
void setInternalValue(uint8_t Internal_Pin, int value);

/**
 * @brief Resets physical pins to their inital state
 * @details This function sets pins as input/output and, if an output pin, sets them to their given init value. This is defined in PinConfig.def
 * 
 */
void resetPhysicalPins();

/**
 * @brief Poll analog pin values
 * @note WIP
 */
void update(void);

/**
 * @brief Initialize all predefined pins
 */
void initialize(void);

} // namespace Pins

#endif // __ECU_PINS_H__