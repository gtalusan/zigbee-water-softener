# Overview

Water Softener Salt Level detector.

ESP32-C6 SuperMini ESP32 development board connected to a VL53L0X time of flight distance sensor breakout board.

The ESP32-C6 is _always_ in deep sleep mode.  It will wake up twice a day (every 12 hours) to report battery level and a distance measurement.

Mounted to the underside of the lid of a water softener's brine tank and powered with a 4.2V lithium-ion battery.

The distance measurement will be from the device to the top of the salt in the brine tank.  An automation, handled elsewhere, will deal with the notification when salt is too low.

It is this sensor's job to work reliably, quickly, with the most effective battery life.

# Technical Environment

Uses an Arduino sketch with the ESP32 core with the appropriate board.  Create using arduino-cli.

A Zigbee2Mqtt external converter must be created so that the sensor can be recognized and is able to join the zigbee mesh in my house.

# Implementation

Simple sketch.  Create an implementation plan.  Make no assumptions.  Ask if any gaps in understanding.  Iterate until done.

Zigbee2mqtt external converter.  Utilize the zigbee2mqtt herdsman repository to find appropriate examples for measuring and reporting distance.
