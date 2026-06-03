// esp32_stack.cpp
// Overrides the Arduino-ESP32 weak symbol to set loop task stack to 32KB.
// This file must be in the same sketch folder as esp32_ereader.ino
// Works with Arduino-ESP32 core 3.x (avoids the broken SET_LOOP_TASK_STACK_SIZE macro).

#include <Arduino.h>

// getArduinoLoopTaskStackSize() is declared as a weak symbol in Arduino-ESP32 3.x.
// Defining it here overrides the default (8192).
size_t getArduinoLoopTaskStackSize() {
  return 32 * 1024;  // 32KB stack for the loop() task
}