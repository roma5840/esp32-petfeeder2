# ASPetFeeder - ESP32 Firmware

This repository contains the official C++/Arduino firmware for the ESP32 microcontroller that powers the ASPetFeeder smart pet feeder. This code is designed to work in tandem with the [ASPetFeeder Mobile App](https://github.com/roma5840/PetFeeder).

The firmware enables the physical feeder to connect to Wi-Fi and communicate with a Firebase Realtime Database, allowing it to receive commands and report its status in real-time.

## Features

-   **Setup Mode:** On first boot, the ESP32 creates a Wi-Fi Access Point (`PetFeeder-Setup`) allowing the mobile app to securely send configuration credentials.
-   **Firebase Integration:** Securely authenticates with Firebase and listens for real-time changes in the database.
-   **Real-time Command Handling:** Instantly responds to "Feed Now" commands sent from the mobile app.
-   **Scheduled Feeding:** Fetches feeding schedules from Firebase and uses its internal clock (synced via NTP) to trigger feedings at the correct times.
-   **Status Reporting:** Updates its online status, last feed time, and last feed amount back to Firebase for the mobile app to display.
-   **Servo Motor Control:** Precisely controls a servo motor to dispense the specified amount of food.

## Getting Started

To use this firmware, you will need the necessary hardware and software, including the Arduino IDE and specific libraries.

### Prerequisites

-   An ESP32 Development Board
-   A Servo Motor
-   A stable power supply for the ESP32 and servo
-   Arduino IDE or PlatformIO installed

### Required Libraries

You must install the following libraries in your Arduino IDE before compiling the code. You can install them via the Library Manager or by downloading the ZIP files from the links below.

-   **Firebase ESP Client (v4.4.1):** [Download Link](https://github.com/mobizt/Firebase-ESP-Client/releases/tag/v4.4.1)
-   **ArduinoJson (v6.21.3):** [Download Link](https://github.com/bblanchon/ArduinoJson/releases/tag/v6.21.3)
-   **ESP32Time (v2.0.4):** [Download Link](https://github.com/fbiego/ESP32Time/releases/tag/2.0.4)
-   **LiquidCrystal I2C (v1.1.2):** [Download Link](https://www.arduinolibraries.info/libraries/liquid-crystal-i2-c)

### Flashing to the ESP32

1.  Open the `.ino` file in the Arduino IDE.
2.  Go to **Tools > Board** and select your ESP32 board model.
3.  Go to **Tools > Port** and select the correct COM port for your connected ESP32.
4.  **Set the Partition Scheme.** Due to the large size of the libraries, the default memory layout of the ESP32 is insufficient and will cause a compilation error (`text section exceeds available space in board`).
    -   Go to **Tools > Partition Scheme**.
    -   Select **`Minimal SPIFFS (1.9MB APP with OTA / 190KB SPIFFS)`**. This provides ample space for the application.
5.  Click the **Upload** button.
6.  Once the upload is complete, open the **Serial Monitor** at a baud rate of `115200` to see the device's status and logs.

## Versioning

-   **Current Firmware Version:** `v11.5.1`
-   **Compatible App Version:** `v1.16.2 or later`
-   **Base Init Commit:** `9esp32`