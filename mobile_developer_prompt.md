````markdown
To: Senior Mobile App Developer (BLE Specialist)
From: Embedded Systems Engineering
Subject: BLE Protocol for ESP32 OTA Firmware Update

Hello,

This document outlines the BLE protocol for the new Over-the-Air (OTA) firmware update feature for our ESP32-based device. Please use this guide to implement the corresponding functionality in the mobile application.

The user's experience is our top priority. The app's UI should clearly and accurately reflect the device's status at every stage of the process to ensure the user feels informed and confident.

## BLE Service and Characteristic UUIDs

You will need to interact with a dedicated OTA service. All communication is managed through the characteristics listed below.

**OTA Service UUID:** `1a89b148-b4e8-43d7-952b-a0b4b01e43b3`

| Characteristic Name       | UUID                                     | Properties       | Description                                                                                                                              |
| ------------------------- | ---------------------------------------- | ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| **Current Version**       | `8A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C65`    | Read             | Read this characteristic to get the device's current firmware version as a UTF-8 string (e.g., "1.0.0"). This is in the main device service. |
| **Update Status**         | `2a89b148-b4e8-43d7-952b-a0b4b01e43b3`    | Read, Notify     | Subscribe to notifications on this characteristic to receive status updates. The status is a single-byte integer. See the status codes below. |
| **Update Control**        | `3a89b148-b4e8-43d7-952b-a0b4b01e43b3`    | Write            | Write a single-byte integer to this characteristic to send commands to the device. See the command codes below.                          |
| **Release Metadata**      | `4a89b148-b4e8-43d7-952b-a0b4b01e43b3`    | Read             | After an update is found (Status 2), read this characteristic to get a JSON string with the new version and release notes.                |
| **Progress**              | `5a89b148-b4e8-43d7-952b-a0b4b01e43b3`    | Read, Notify     | During the update (Status 4), subscribe to notifications to receive a single-byte integer representing the percentage complete (0-100).     |

## Interaction Logic & State Flow

The following is a step-by-step guide to the entire OTA process. The app should follow this logic precisely.

### Step 1: Initial State & Version Check

1.  **Connect** to the device.
2.  **Read** the `Current Version` characteristic to display the device's firmware version in your UI.
3.  **Subscribe** to notifications for the `Update Status` characteristic. The initial status will be `0` (Idle).

### Step 2: Checking for an Update

1.  When the user requests to check for an update, **write** the command code `1` to the `Update Control` characteristic.
2.  The device will notify you with the following statuses on the `Update Status` characteristic:
    *   `1` (Checking for update): Display a "Checking..." message to the user.
    *   `2` (Update available): An update has been found. Proceed to Step 3.
    *   `3` (No update available): Inform the user they are on the latest version. The process ends here.
    *   `5` (Update failed): This may happen if the device cannot connect to WiFi or the update server. Inform the user of the failure.

### Step 3: Displaying Update Information

1.  Upon receiving status `2`, **read** the `Release Metadata` characteristic.
2.  The value will be a JSON string, for example:
    ```json
    {
      "version": "1.1.0",
      "notes": "- Added new feature X.\n- Fixed bug Y."
    }
    ```
3.  Parse this JSON and display the new version number and release notes to the user.
4.  Provide the user with a button to "Start Update".

### Step 4: Performing the Update

1.  When the user clicks "Start Update", **write** the command code `2` to the `Update Control` characteristic.
2.  The device will notify with `Update Status` `4` (Update in progress). At this point, you should **subscribe** to notifications on the `Progress` characteristic.
3.  As the firmware is downloaded and flashed, the device will send frequent notifications on the `Progress` characteristic with a value from 0 to 100. Use this to display a progress bar.
4.  The `Update Status` may change to `5` (Update failed) if there is an error during the download or flashing. Inform the user and abort.

### Step 5: Finalization and Reboot

1.  When the update is complete, the device will send `Update Status` `6` (Update successful, rebooting).
2.  Inform the user that the device is rebooting and will reconnect shortly.
3.  The device will disconnect.

### Step 6: Post-Reboot Confirmation

1.  After the device reboots, it will start advertising again. The app should automatically reconnect.
2.  Upon reconnection, the device will **immediately** send a final `Update Status` notification with the value `7` (Post-reboot success confirmation).
3.  When you receive this status, you can confidently inform the user that the update was successful and the device is ready to use. This is the final step in the process.

## Status and Command Codes Summary

**Update Status Codes (Read/Notify):**

*   `0`: Idle
*   `1`: Checking for update
*   `2`: Update available
*   `3`: No update available
*   `4`: Update in progress
*   `5`: Update failed
*   `6`: Update successful, rebooting
*   `7`: Post-reboot success confirmation

**Update Control Codes (Write):**

*   `1`: Check for update
*   `2`: Start the update process

Please feel free to reach out if you have any questions.

Best regards,
Embedded Team
````