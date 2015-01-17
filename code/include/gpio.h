/**
 * @file gpio.h
 * @brief gpio class defines
 */

#ifndef _PICOPTERX_GPIO_H
#define _PICOPTERX_GPIO_H

namespace picopter {
    /**
     * Controls the GPIO pins on the RPi, including PWM functionality.
     */
    namespace gpio {
        /** The GPIO pin of the switch for auto mode (wiringPi numbering) **/
        const int MODE_PIN = 5;
        /** The GPIO pin of the buzzer (wiringPi numbering) **/
        const int BUZZER_PIN = 2;

        void init();
        bool isAutoMode();
        void setBuzzer(bool value);
    }
}

#endif // _PICOPTERX_GPIO_H