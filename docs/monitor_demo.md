# Monitor Box Demo (monitor_demo)

***Copyright © Quectel Wireless Solutions Co\., Ltd\. 2026\. All rights reserved\.***

---

## Revision History

| Version | Date | Author | Change Description |
| --- | --- | --- | --- |
| 0.1 | 2026-11-09 |  | Created the demo user guide based on the current `monitor_box` implementation |
| 0.2 | 2026-11-10 |  | Added numbered chapters and a table of contents |
| 0.3 | 2026-11-10 |  | Expanded the Web UI chapter with browser compatibility, mobile access, configuration backup, API reference and a typical demo workflow |

## Table of Contents

[1 Project Overview](#1-project-overview)  
[2 Feature Overview](#2-feature-overview)  
[3 Development Resources](#3-development-resources)  
　　[3.1 Hardware List](#31-hardware-list)  
　　[3.2 Software Resources](#32-software-resources)  
[4 Quick Start](#4-quick-start)  
　　[4.1 Hardware Connections](#41-hardware-connections)  
　　[4.2 Building the Programs](#42-building-the-programs)  
　　[4.3 Deploying to the Board](#43-deploying-to-the-board)  
　　[4.4 Checking Runtime Status](#44-checking-runtime-status)  
　　[4.5 Opening the Desktop App](#45-opening-the-desktop-app)  
[5 UI and Operation](#5-ui-and-operation)  
　　[5.1 Main Interface](#51-main-interface)  
　　[5.2 Detection Capability Settings](#52-detection-capability-settings)  
　　[5.3 Regions and Counting Lines](#53-regions-and-counting-lines)  
　　　　[5.3.1 Drawing Polygonal Regions](#531-drawing-polygonal-regions)  
　　　　[5.3.2 Drawing Crossing Lines](#532-drawing-crossing-lines)  
　　[5.4 Ground Positioning and Bird's-Eye View](#54-ground-positioning-and-birds-eye-view)  
[6 Feature Demonstrations](#6-feature-demonstrations)  
　　[6.1 Object Detection](#61-object-detection)  
　　[6.2 Intrusion Detection](#62-intrusion-detection)  
　　[6.3 Electronic Fence](#63-electronic-fence)  
　　[6.4 People Counting](#64-people-counting)  
　　[6.5 Loitering Detection](#65-loitering-detection)  
　　[6.6 Package Detection](#66-package-detection)  
　　[6.7 Fall Detection](#67-fall-detection)  
　　[6.8 Fire and Smoke Detection](#68-fire-and-smoke-detection)  
　　[6.9 Low-Power Standby](#69-low-power-standby)  
[7 Web UI Usage](#7-web-ui-usage)  
　　[7.1 Access and Connection](#71-access-and-connection)  
　　　　[7.1.1 Browser Compatibility](#711-browser-compatibility)  
　　　　[7.1.2 Mobile Access](#712-mobile-access)  
　　[7.2 Page Overview](#72-page-overview)  
　　[7.3 Live Monitoring](#73-live-monitoring)  
　　　　[7.3.1 Video Area](#731-video-area)  
　　　　[7.3.2 Information Cards](#732-information-cards)  
　　　　[7.3.3 Standby and Wake-Up](#733-standby-and-wake-up)  
　　[7.4 Alarm Event Management](#74-alarm-event-management)  
　　　　[7.4.1 Querying and Filtering](#741-querying-and-filtering)  
　　　　[7.4.2 Viewing Event Details](#742-viewing-event-details)  
　　　　[7.4.3 Deleting Events](#743-deleting-events)  
　　[7.5 Regions, Counting Lines and Activity Heatmap](#75-regions-counting-lines-and-activity-heatmap)  
　　　　[7.5.1 Page Layout and Common Operations](#751-page-layout-and-common-operations)  
　　　　[7.5.2 Drawing Regions](#752-drawing-regions)  
　　　　[7.5.3 Drawing Crossing Lines](#753-drawing-crossing-lines)  
　　　　[7.5.4 Drawing Ground Calibration and Ground Zones](#754-drawing-ground-calibration-and-ground-zones)  
　　　　[7.5.5 Activity Heatmap](#755-activity-heatmap)  
　　[7.6 Bird's-Eye View and Ground Positioning](#76-birds-eye-view-and-ground-positioning)  
　　　　[7.6.1 Bird's-Eye View Canvas](#761-birds-eye-view-canvas)  
　　　　[7.6.2 Calibration Quality](#762-calibration-quality)  
　　　　[7.6.3 Ground Zone Management](#763-ground-zone-management)  
　　　　[7.6.4 Target List and Device Location](#764-target-list-and-device-location)  
　　[7.7 Parameter Settings](#77-parameter-settings)  
　　　　[7.7.1 Settings Categories](#771-settings-categories)  
　　　　[7.7.2 Configuration Backup and Restore](#772-configuration-backup-and-restore)  
　　[7.8 Runtime Status](#78-runtime-status)  
　　[7.9 HTTP API](#79-http-api)  
　　[7.10 Configuration Hot Reload and Restart Scope](#710-configuration-hot-reload-and-restart-scope)  
　　[7.11 Web UI vs. Desktop App](#711-web-ui-vs-desktop-app)  
　　[7.12 Typical Demo Workflow](#712-typical-demo-workflow)  
　　[7.13 Usage Notes](#713-usage-notes)  
[8 Performance Reference](#8-performance-reference)  
[9 Troubleshooting](#9-troubleshooting)  
　　[9.1 Camera Not Detected](#91-camera-not-detected)  
　　[9.2 Camera Device Busy](#92-camera-device-busy)  
　　[9.3 No Video in the App](#93-no-video-in-the-app)  
　　[9.4 Detection Model Load Failure](#94-detection-model-load-failure)  
　　[9.5 Feature Enabled but No Alarm](#95-feature-enabled-but-no-alarm)  
　　[9.6 Settings Cannot Be Saved](#96-settings-cannot-be-saved)  
　　[9.7 Positioning Jitters or Has Large Errors](#97-positioning-jitters-or-has-large-errors)  
　　[9.8 Browser Cannot Access the Web UI](#98-browser-cannot-access-the-web-ui)  
[10 Usage Notes](#10-usage-notes)  

## 1 Project Overview

The Monitor Box Demo is built on the **Quectel Pi M2** smart single-board computer (Rockchip RK3576, Debian 13). The device captures a live feed from a USB UVC camera, runs object detection, pose detection and fire/smoke models on the on-board NPU, and uses a rules engine to evaluate regions, line crossings, dwell time and event-driven actions.

The demo provides a desktop app, browser preview, event snapshots and alarm clips. It can demonstrate:

- Detection of people, vehicles, pets and other common COCO objects;
- Intrusion detection, electronic fences, loitering detection and abandoned-package detection;
- Bidirectional people counting;
- Fall detection;
- Fire and smoke detection;
- 2D ground positioning, tracks, ground-zone analysis and a bird's-eye view;
- Alarm events, snapshots, pre/post-event clips, audio prompts and low-power standby.

<!-- Image placement: Monitor Box device and live detection view. -->

> Note: the source project directory is named `monitor_box`. This document uses the demo name `monitor_demo`, but all commands still use the actual path `projects/demo-monitor-box`.

## 2 Feature Overview

| Capability | Description | Default State | Additional Dependency |
| --- | --- | --- | --- |
| Object detection | Detects and displays the 80 COCO classes | On | Detector model included with the project |
| Intrusion detection | Alarms when a target enters a configured warning zone | Off | Draw a warning zone |
| Fall detection | Detects falls from body pose over consecutive frames | Off | RTMPose pose model |
| People counting | Accumulates entries and exits according to crossing-line direction | Off | Draw a crossing line |
| Fire and smoke | Detects fire and smoke with a dedicated model | Off | Fire/smoke model |
| Electronic fence | Alarms when a target crosses a configured fence region | Off | Draw a fence region |
| Loitering detection | Alarms when the same target stays in a region beyond the threshold | Off | Draw a warning zone |
| Package detection | Detects unattended backpacks, handbags and suitcases | Off | Detector model |
| Ground positioning | Converts people, vehicles and pets into ground coordinates and draws a bird's-eye view | On | Complete ground calibration |

Each capability can be enabled independently in the desktop app under **Settings → Detection Capabilities**. After a feature is enabled, it must also be bound to a region or crossing line under **Region Settings** before it takes effect in a specific location.

## 3 Development Resources

### 3.1 Hardware List

| Accessory | Quantity | Specification or Notes |
| --- | --- | --- |
| Quectel Pi M2 smart single-board computer | 1 | RK3576, Debian 13 factory system |
| USB UVC camera | 1 | 1080p MJPG recommended; connect through the M2 USB 3.0 port |
| HDMI display | 1 | For local desktop demonstrations; optional |
| USB keyboard and mouse | 1 each | For operating the desktop app; optional |
| Ethernet cable or Wi-Fi | 1 connection | For Web preview, device location and remote access |
| Power adapter | 1 | Select one that meets the Quectel Pi M2 power requirements |
| Powered speaker | 1 set | For demonstrating alarm audio; optional |

A standard USB UVC camera is recommended. The project defaults to 1920×1080, 30 fps and MJPG. Lower-resolution cameras can also be used, but the video parameters in the configuration file must be adjusted accordingly.

### 3.2 Software Resources

- Quectel Pi M2 Debian 13 factory image;
- SDK source code and cross-compilation toolchain;
- RKNN Runtime 2.3.2;
- The YOLOv8s INT8 detector model and COCO label file included with the project;
- GTK4, libadwaita, GStreamer and Python 3 runtimes in the desktop environment.

> The camera, detector model, RKNN Runtime, RGA, MPP and JPEG runtimes must use compatible versions. If the model and Runtime versions do not match, the program usually fails while loading the model.

## 4 Quick Start

### 4.1 Hardware Connections

Complete the connections in the following order:

1. **Camera**: connect the USB UVC camera to the Quectel Pi M2 USB 3.0 port.
2. **Display**: connect the Quectel Pi M2 to the monitor with an HDMI cable.
3. **Peripherals**: connect a USB keyboard and mouse.
4. **Network**: connect an Ethernet cable, or join the device to the same LAN as the demonstration terminal.
5. **Audio (optional)**: connect a powered speaker or USB speaker for alarm prompts.
6. **Power**: power the Quectel Pi M2 with a compatible adapter.

<!-- Image placement: Quectel Pi M2, USB camera, display and power connection diagram. -->

After the device boots, confirm the camera node in a terminal:

```bash
v4l2-ctl --list-devices
ls -l /dev/video*
```

The project uses `/dev/video73` by default. If the actual node differs, specify it with `--device` at startup or change `video.device` in the configuration file.

### 4.2 Building the Programs

Run the following from the SDK root on the **SDK host**. If this repository has not been cloned into the SDK checkout yet:

```bash
git clone https://github.com/Quectel-Pi/demo-monitor-box.git projects/demo-monitor-box
make -C projects/demo-monitor-box
```

The main build outputs are:

```text
projects/demo-monitor-box/build/mb-infer
projects/demo-monitor-box/build/mb-capture
```

- `mb-infer`: offline single-image inference tool for verifying the model and NPU;
- `mb-capture`: live monitoring program responsible for camera capture, inference, tracking, rule evaluation, streaming and alarms.

### 4.3 Deploying to the Board

First confirm that the board is connected through ADB and that `adb shell` has root access:

```bash
adb devices
adb shell id
```

Push the files required to run the demo to the board:

```bash
adb shell 'mkdir -p /userdata/monitor_box/deploy/build /userdata/monitor_box/deploy/config /userdata/monitor_box/deploy/models /userdata/monitor_box/deploy/tools /userdata/monitor_box/deploy/app'

adb push projects/demo-monitor-box/build/mb-infer projects/demo-monitor-box/build/mb-capture /userdata/monitor_box/deploy/build/
adb push projects/demo-monitor-box/config/monitor_box.json /userdata/monitor_box/deploy/config/
adb push projects/demo-monitor-box/models/yolov8s-640-640.rknn projects/demo-monitor-box/models/coco_80_labels_list.txt /userdata/monitor_box/deploy/models/
adb push projects/demo-monitor-box/install.sh projects/demo-monitor-box/setup_autostart.sh /userdata/monitor_box/deploy/
adb push projects/demo-monitor-box/app/99-monitor-box-video.conf /userdata/monitor_box/deploy/app/
adb push projects/demo-monitor-box/tools/migrate_config.py /userdata/monitor_box/deploy/tools/
```

Install the backend program and default configuration:

```bash
adb shell 'cd /userdata/monitor_box/deploy && bash install.sh'
```

Install and enable the boot service:

```bash
adb shell 'cd /userdata/monitor_box/deploy && bash setup_autostart.sh install'
```

Install the desktop app from the SDK host:

```bash
bash projects/demo-monitor-box/app/install_app.sh
```

The app installation script updates only the desktop app. To update the backend at the same time, run this from the SDK root:

```bash
bash projects/demo-monitor-box/tools/update_backend_adb.sh
```

### 4.4 Checking Runtime Status

```bash
adb shell 'systemctl status monitor-box --no-pager'
adb shell 'curl --fail http://127.0.0.1:8080/status'
```

A healthy system should show:

- `camera.connected` set to `true`;
- `h264.running` set to `true`;
- `capture_fps` close to the configured 30 fps;
- Continuously updating values for `capture_fps`, `ai_ms`, `loop_ms` and related metrics.

If the service status is abnormal, inspect the logs:

```bash
adb shell 'tail -n 100 /opt/monitor_box/log/service.log'
adb shell 'tail -n 100 /opt/monitor_box/log/service_error.log'
```

### 4.5 Opening the Desktop App

On the board desktop, press `Super`, search for **Monitor Box**, and open the app.

The app uses the **Auto** video source by default:

1. When the monitoring service is running, it prefers the H.264 stream from `mb-capture`;
2. If H.264 is unavailable, it switches to the Web MJPEG preview;
3. It attempts to open the local camera directly only when the monitoring service is not running.

> A `/dev/video*` device is normally exclusive to one process. While `mb-capture` is running, do not force the app to use **Local Camera**, or it will report that the device is busy.

<!-- Image placement: Monitor Box desktop app main interface. -->

## 5 UI and Operation

### 5.1 Main Interface

The main interface displays the live video, detection boxes, regions, counting lines and performance information. Common shortcuts are listed below:

| Operation | Shortcut | Description |
| --- | --- | --- |
| Open or close the camera | `F8` | Closing enters standby and stops capture, inference, encoding and alarms |
| Event sidebar | `F9` | View alarm events, snapshots and clips |
| Region Settings | `Ctrl+R` | Draw regions, crossing lines and ground calibration on the video |
| Settings | `Ctrl+,` | Enable detection capabilities and adjust video and power parameters |
| Bird's-Eye View / Positioning | `Ctrl+G` | View ground coordinates, tracks and ground-zone statistics |
| Fullscreen | `F11` | Enter or exit fullscreen |

### 5.2 Detection Capability Settings

Go to **Settings → Detection Capabilities** and enable the capabilities required for the demonstration. The feature master switch only determines whether a capability operates; the locations where it takes effect are determined by bindings in **Region Settings**.

For a first demonstration, use this order:

1. Enable **Object Detection** and confirm that people, vehicles and other targets are tracked with stable boxes;
2. Enable **Intrusion Detection** and bind a warning zone in the video;
3. Enable **People Counting** and draw a crossing line at the entrance;
4. Enable **Electronic Fence** or **Loitering Detection**, reusing the warning zone;
5. Install the pose model, then enable **Fall Detection**;
6. Prepare the fire/smoke model, then enable **Fire and Smoke**;
7. Complete ground calibration, then enable **Ground Positioning**.

After settings are saved, `mb-capture` hot-reloads them in about one second. Changes to the camera, model paths, streaming and Web port normally require a service restart.

### 5.3 Regions and Counting Lines

Open **Region Settings** and draw regions over the current live frame.

A maximum of 12 regions can be created; ground calibrations and ignore zones also count toward this limit. The app, Web UI and monitoring service reject the 13th region. Crossing lines do not count toward the region limit.

#### 5.3.1 Drawing Polygonal Regions

1. Click **Draw Region**;
2. Click the warning-zone boundary vertices in order;
3. Click the first vertex again, double-click, or press Enter to close the region;
4. Drag a vertex to reshape the region, or drag inside the region to move it as a whole;
5. Under **Enable Here**, select the features that should use it;
6. Click **Save**.

Region roles include:

| Role | Typical Use |
| --- | --- |
| Private Area | Yard, parking space or entrance; used for intrusion, fence and loitering |
| Public Area | Road or sidewalk; normally used for counting or positioning |
| Ignore Area | Trees, televisions, reflections and other false-detection areas; excluded from all rules |
| Ground Calibration | A rectangle with known real-world dimensions on the ground; used for ground positioning |

#### 5.3.2 Drawing Crossing Lines

1. Click **Draw Crossing Line**;
2. Select the start and end points across the entrance or passage;
3. The side indicated by the arrow is **In**;
4. If the direction is reversed, click **Reverse**;
5. Under **Enable Here**, select **People Counting**, then save.

<!-- Image placement: Region Settings page, warning zone and crossing line. -->

### 5.4 Ground Positioning and Bird's-Eye View

Ground positioning requires calibration on the video before ground-zone analysis can be performed.

Recommended sequence:

1. Open **Region Settings → Draw Ground Calibration**;
2. Select a rectangular area of known dimensions on the ground;
3. Click the four corners in the order near-left, near-right, far-right and far-left;
4. Enter the actual width and depth of the area in metres;
5. Save the calibration;
6. Open **Settings → Ground Positioning and Bird's-Eye View** and enable the ground-positioning master switch;
7. Draw the required ground zones in Region Settings or on the Bird's-Eye View page;
8. Open **Bird's-Eye View / Positioning** and inspect calibration quality, target positions and tracks.

Ground positioning uses the bottom-centre point of a target box as the foot point and projects it onto the ground plane. When feet are occluded, a person is sitting or lying down, or a target is on stairs or a second floor, the coordinates should be treated as a reference only.

<!-- Image placement: Ground calibration and bird's-eye view interface. -->

## 6 Feature Demonstrations

### 6.1 Object Detection

Keep the camera stable and move people, vehicles or common objects into the frame. Detection boxes, classes and confidence values appear on the live video. A single line below the app title shows the video source, playback and capture frame rates, AI time, link latency and tracked-target count.

### 6.2 Intrusion Detection

1. Draw a **Private Area** in the video;
2. Bind **Intrusion Detection** to the region;
3. Open **Settings → Detection Capabilities → Intrusion Detection**;
4. Let a person enter the warning zone and remain there for about 0.5 seconds;
5. Open the event sidebar and inspect the alarm level, target, snapshot and clip.

### 6.3 Electronic Fence

1. Draw the ground area inside the fence or passage;
2. Bind **Electronic Fence** to the region;
3. Turn on the feature master switch;
4. Let a target enter or cross the configured area;
5. Inspect the fence alarm in the event sidebar.

The fence region should contain only the ground inside the fence. Do not include the fence itself or the road outside it, as this reduces false alarms caused by occlusion while someone climbs over the fence.

### 6.4 People Counting

1. Draw a crossing line at the entrance;
2. Set the arrow direction to the entry direction;
3. Bind **People Counting** to the line;
4. Open **Settings → Detection and Audio → Feature Master Switches → People Counting**;
5. Let people walk across the line one after another;
6. Observe the `IN`, `OUT` and `NOW` values in the upper-right corner.

To restart counting, go to **Settings → Detection and Audio → Feature Master Switches → Current People Counting** and click **Reset Now**. This resets only the current `IN` / `OUT` values; history already written to `counts.jsonl` is not deleted.

Counting results can also be queried through the Web API:

```bash
curl http://<device-ip>:8080/counts
```

Reset the current counters:

```bash
curl -X DELETE http://<device-ip>:8080/counts
```

### 6.5 Loitering Detection

1. Bind **Loitering Detection** to a warning zone;
2. Enable the loitering feature;
3. Adjust the loitering alarm delay in Settings; the default is 60 seconds;
4. Keep the same target continuously inside the region;
5. After the threshold is reached, inspect the alarm event and clip;
6. After the escalation threshold is reached, the event level is raised to critical.

### 6.6 Package Detection

1. Enable **Package Detection**;
2. Set the package stationary-alarm delay in Settings; the default is 20 seconds;
3. Place a backpack, handbag or suitcase in the frame and leave;
4. When the package remains stationary with no person nearby for the configured time, an abandoned-object alarm is generated;
5. When the package is removed, a removal event can be generated.

This demo uses the COCO `backpack`, `handbag` and `suitcase` classes. It is suitable for demonstrating abandoned backpacks and suitcases, but it is not a dedicated model for arbitrary parcels or boxes.

### 6.7 Fall Detection

Fall detection requires a pose model. Before first use, run this on the SDK host:

```bash
cd projects/demo-monitor-box
PYTHON=~/.venvs/rknn232/bin/python tools/get_rtmpose.sh t
```

After the model is generated, run the deployment process again or copy it to the board manually:

```bash
adb push projects/demo-monitor-box/models/rtmpose_t-256x192-rk3576.rknn /opt/monitor_box/models/
```

Then open **Settings → Detection Capabilities → Fall Detection** and ensure that the full body of people in the frame is visible. After a person falls and the pose verdict remains satisfied, a fall alarm is generated.

### 6.8 Fire and Smoke Detection

Fire and smoke detection requires a separate model. Before first use, run this on the SDK host:

```bash
projects/demo-monitor-box/tools/fire_model/build_fire_model.sh
```

The generated files are:

```text
projects/demo-monitor-box/models/fire_yolov8n.rknn
projects/demo-monitor-box/models/fire_labels.txt
```

Run the deployment process again or copy them to the board manually:

```bash
adb push projects/demo-monitor-box/models/fire_yolov8n.rknn projects/demo-monitor-box/models/fire_labels.txt /opt/monitor_box/models/
```

After enabling **Settings → Detection Capabilities → Fire and Smoke**, keep fire or smoke visible in the frame. Fire is confirmed for about 1 second by default and smoke for about 3 seconds, reducing single-frame false alarms.

> This feature is intended for demo purposes and cannot replace certified fire-alarm equipment. Public datasets differ from real installation views, so thresholds and model performance must be re-evaluated with on-site footage before operational use.

### 6.9 Low-Power Standby

Click the camera button in the app title bar or press `F8` to put the device into standby:

- Close the camera and release the device node;
- Stop NPU inference and video encoding;
- Stop generating new alarms;
- Keep the Web UI and event queries available.

Click the camera button again or press `F8` to resume video. You can also select Normal, Eco or Standby mode under **Settings → Power and Power Saving**.

## 7 Web UI Usage

Monitor Box includes a built-in Web management page. No client installation is required. From a browser on the same LAN, you can view live video, handle alarm events, edit monitoring regions, inspect ground positioning, change runtime parameters and check device status.

The Web UI is served by `mb-capture`. Closing the desktop app does not stop the Web service. When the device enters standby, live video stops, but event queries, runtime status and configuration pages remain available.

### 7.1 Access and Connection

The Web service listens on `0.0.0.0:8080`. First confirm that the service is running on the board or through ADB:

```bash
adb shell 'systemctl is-active monitor-box'
adb shell 'curl --fail http://127.0.0.1:8080/status'
```

If the board has a LAN address, open this URL in a browser:

```text
http://<device-ip>:8080/
```

Do not rely only on `127.0.0.1` when checking the address. For example:

```bash
ip -br addr
```

Other computers can access the Web UI only when `eth0` or the Wi-Fi interface is `UP` and has a LAN address such as `192.168.x.x` or `10.x.x.x`. When no Ethernet cable is connected, `eth0` commonly reports `NO-CARRIER`; the Web service can still be running, but browsers on the LAN cannot connect.

If the board has no LAN address, access the UI through ADB. Run this on the SDK host:

```bash
make -C projects/demo-monitor-box web-adb
```

The script establishes an `8080 -> 8080` forward and prints two URLs:

```text
http://127.0.0.1:8080/          # Use when connecting through SSH to the same Linux host
http://<Ubuntu VM IP>:8080/     # Use from a browser in local VS Code Remote
```

If VS Code connects to an Ubuntu VM through SSH Remote and the browser runs on the local computer, `127.0.0.1` points to the local computer. Use the VM IP address printed by the script. You can also run `ip -4 -br addr` or forward port `8080` in the VS Code **Ports** panel.

You can also create the forward manually:

```bash
adb forward tcp:8080 tcp:8080
```

The lower-left corner of the page shows the connection status to the monitoring service. **Service Online** means the page is connected to `mb-capture`. If **Disconnected** is shown, check the backend service, network connection and Web port.

Default configuration:

| Parameter | Default Value | Configuration Field |
| --- | --- | --- |
| Web listen address | `0.0.0.0` | `web.listen` |
| Web port | `8080` | `web.port` |
| MJPEG preview width | 640 | `web.stream_width` |
| MJPEG preview height | 360 | `web.stream_height` |
| MJPEG preview frame rate | 10 fps | `web.stream_fps` |
| MJPEG image quality | 70 | `web.stream_jpeg_quality` |

> The Web service allows LAN access but has no login or permission control. Use it only on a trusted network, or change the listen address to `127.0.0.1`.

#### 7.1.1 Browser Compatibility

The Web UI uses standard HTML, CSS and JavaScript and does not require third-party plug-ins. Recommended browsers:

| Browser | Minimum Version | Notes |
| --- | --- | --- |
| Chrome / Chromium | 90+ | Recommended; most reliable live video and event playback |
| Edge | 90+ | Uses the same engine as Chromium |
| Firefox | 95+ | Supported, but MJPEG buffering behaves slightly differently |
| Safari | 15+ | Available on macOS and iOS |

Internet Explorer and browsers with old engines are not recommended. Some interactions, including live video loading, event detail dialogs and region drawing, may not work correctly.

#### 7.1.2 Mobile Access

Phone and tablet browsers can also access the Web UI:

1. Make sure the phone and the board are on the same Wi-Fi network;
2. Enter `http://<device-ip>:8080/` in the browser address bar;
3. Live video, event lists and status pages adapt to the screen width;
4. Region-drawing precision is limited on mobile devices. Use mobile access for viewing and simple adjustments; perform complex calibration in a desktop browser.

To add the Web UI to the phone home screen, select **Add to Home Screen** in the browser and open the shortcut in an approximately fullscreen layout.

<!-- Image placement: Web live-monitoring home page. -->

### 7.2 Page Overview

The Web navigation bar contains six pages:

| Page | Primary Purpose |
| --- | --- |
| Live | View live video, device summary, people counts and recent events |
| Events | Query alarms by level, inspect snapshots, replay clips and delete historical events |
| Regions | Draw regions, crossing lines, ground calibration and ground zones; inspect the activity heatmap |
| Bird's-Eye | View ground coordinates, target tracks, ground-zone statistics and calibration quality |
| Settings | Modify detection, video, display, positioning, event and advanced configuration |
| Status | View capture, inference, encoding, event, loitering and configuration information |

The page header provides these global controls:

- **Operating Mode**: select Normal, Eco or Standby, then click **Apply**;
- **Light/Dark Theme**: switch between light and dark interfaces;
- **Refresh**: reload data for the current page.

### 7.3 Live Monitoring

The **Live** page is the default Web home page. It contains the video area, information cards on the right and a toolbar at the top.

#### 7.3.1 Video Area

The video area displays the camera feed through MJPEG and overlays:

- Detection boxes and target class labels;
- Warning zones, fence regions, ignore regions and counting lines;
- The bird's-eye-view picture-in-picture, if enabled;
- The activity heatmap, if it is enabled for display;
- Video status notifications such as camera disconnection, device standby and view-position changes.

Four key metrics are displayed below the video:

| Metric | Meaning |
| --- | --- |
| Capture | Raw camera capture frame rate in fps |
| AI | Detection inference time in ms |
| Tracks | Number of currently stable tracked targets |
| End-to-End | Total latency from driver frame output through OSD completion to encoder input, in ms |

MJPEG preview quality and frame rate can be changed under **Settings → Video and Inference**. Higher quality and frame rate increase network and CPU load. Reduce these values when multiple clients are viewing the preview at the same time.

#### 7.3.2 Information Cards

The right side contains three information cards:

- **Device Summary**: camera node, resolution, inference status, H.264 encoding status and saved event count;
- **People Counting**: entries, exits and people currently in view for each counting line. Click **Reset** to clear the counters;
- **Recent Events**: the latest alarms; click one to open its details.

#### 7.3.3 Standby and Wake-Up

When the device is in standby, the video area displays **Camera Closed**, while other page functions remain available. To resume:

1. Click the **Operating Mode** selector in the page header;
2. Select **Normal** or **Eco**;
3. Click **Apply**;
4. Wait a few seconds for the video to return.

In standby, event queries, configuration changes and status views remain available; only new detections and alarms stop.

<!-- Image placement: Web live video, device summary and people counting. -->

### 7.4 Alarm Event Management

Open the **Events** page to view alarm records stored by the backend.

#### 7.4.1 Querying and Filtering

By default, the page loads 50 events per batch and provides these filtering and paging controls:

- Filter by **Critical**, **Warning** or **Info** level;
- Click **Refresh** to reload events;
- Click **Older Events** to load the next older batch;
- Click **Back to Latest** to return to the newest batch;
- The top of the page shows the total event count and the number currently loaded.

#### 7.4.2 Viewing Event Details

Click **Details** on an event row to view:

- Event time, event ID and level;
- Feature name and event type;
- Target class, tracking ID and region information;
- An event snapshot with detection boxes;
- The completed MP4 alarm clip.

If a clip is still being generated, the page displays its status. After generation completes, the clip can be played directly in the browser or opened separately.

#### 7.4.3 Deleting Events

The page supports three deletion operations:

- Click **Delete Clip** to delete only the clip while keeping the event record and snapshot;
- Click **Delete** to delete one event together with its snapshot and clip;
- Select multiple events and click **Delete Selected** for batch deletion.

Events with clips still being generated are not deleted. The system skips them and reports this in the result. Deletion cannot be undone, so confirm whether an event must be retained before proceeding.

### 7.5 Regions, Counting Lines and Activity Heatmap

The **Regions** page uses the current video without OSD as its editing background. Monitoring regions, crossing lines and ground calibration can be configured entirely in the browser. Before saving, all changes remain in the local editor and do not affect the running backend. After you click **Save Regions**, the configuration is submitted to `mb-capture` and hot-reloaded.

#### 7.5.1 Page Layout and Common Operations

The canvas is on the left, and the region list and property panel are on the right. Common operations:

- Click **Draw Region**, **Draw Crossing Line**, **Draw Ground Calibration** or **Draw Ground Zone** to start drawing;
- Click **Finish** to end the current drawing operation;
- Click **Undo Last Point** to remove the last vertex;
- Click **Cancel** to discard the current drawing operation;
- Select an existing item in the right-hand list to edit its name, role, bound features and parameters;
- With an item selected, press `Delete` or `Backspace` to remove it;
- Click **Save Regions** to submit the changes and trigger backend hot reload.

The backend validates the configuration when saving. If validation fails, the page reports the reason and does not overwrite the existing configuration.

#### 7.5.2 Drawing Regions

1. Click **Draw Region**;
2. Click the polygon vertices in order on the video;
3. Click **Finish** to close the region;
4. Select the region in the right-hand list and set its name, role and bound features;
5. Click **Save Regions**.

Region roles include Private Area, Public Area, Ignore Area, Ground Calibration and Unlabeled. Intrusion, fence and loitering features are normally bound to Private Areas; people counting is bound to crossing lines; ground positioning is bound to the ground-calibration region.

#### 7.5.3 Drawing Crossing Lines

1. Click **Draw Crossing Line**;
2. Select the start and end points across the entrance or passage;
3. Click **Finish**;
4. Set the line name on the right and bind **People Counting**;
5. Click **Save Regions**.

The arrow shows the **In** direction. If the direction is reversed, adjust it in the line properties.

#### 7.5.4 Drawing Ground Calibration and Ground Zones

- **Draw Ground Calibration**: select a rectangular area of known dimensions on the ground and enter its real width and depth. This provides the mapping from pixel coordinates to metric coordinates;
- **Draw Ground Zone**: draw the ground area used for crossing or dwell statistics;
- **Save Regions**: validate and write the configuration back so that `mb-capture` can hot-reload it.

Select flat ground for calibration and make sure all four corners lie on the same horizontal plane. Do not use one set of calibration parameters for stairs, slopes or multiple floor levels.

#### 7.5.5 Activity Heatmap

The region editor provides these heatmap functions:

- **Show Heatmap**: display the activity heatmap on the editor background and Live page;
- **Learning**: allow the detection model to keep running when other detection features are off so activity data can be collected;
- **Heatmap Suggestions**: recommend regions or crossing lines based on where people and vehicles appear and how they move;
- **Clear Heatmap**: delete existing statistics and start collecting again.

Heatmap suggestions are configuration guidance only. Review them against the actual site requirements before saving. Suggested regions and lines do not automatically enable detection features.

<!-- Image placement: Web region editor and activity heatmap. -->

### 7.6 Bird's-Eye View and Ground Positioning

The **Bird's-Eye** page is used to view ground-positioning results and manage ground zones. The bird's-eye canvas is on the left; calibration quality, ground zones, target list and device-location information are on the right.

#### 7.6.1 Bird's-Eye View Canvas

The bird's-eye view uses metric coordinates and displays:

- Calibration-area boundaries;
- Ground-zone outlines, which can be clicked for viewing and editing;
- Real-time target positions, colour-coded by person, vehicle, pet and similar groups;
- Target motion tracks: smoothed paths over the most recent seconds.

Click **Reset Statistics** to clear visit and dwell statistics for all ground zones and restart accumulation.

#### 7.6.2 Calibration Quality

The **Calibration Quality** card on the right displays:

- **Quality Level**: good / fair / poor / invalid;
- **Mean Residual**: fitting error of the calibration points, in pixels;
- **Coverage**: distribution of calibration points across the frame;
- **Messages**: whether calibration is acceptable and whether additional check points are needed.

When calibration quality is `poor` or `invalid`, positioning error is likely to be large. Return to the **Regions** page and adjust the calibration points.

#### 7.6.3 Ground Zone Management

The **Ground Zones** card allows you to add, inspect and delete ground zones. Each ground zone supports:

- A custom name;
- Selection of target groups such as people, vehicles and pets;
- Entry-crossing alarms;
- A dwell-timeout alarm threshold.

Ground-zone alarms behave like video-region alarms and produce records and snapshots through the event system.

#### 7.6.4 Target List and Device Location

- **Target List**: displays the class, position and movement direction of targets currently detected within the calibrated area;
- **Device Location**: displays the box's latitude, longitude, positioning method and accuracy. Click **Locate Now** to trigger another positioning attempt manually.

Device latitude/longitude and 2D ground positioning are different concepts: the former is the box's position on Earth, while the latter is the relative coordinate of a target on the ground plane in the video.

> Ground positioning applies only to targets on the ground plane. When a person sits or lies down, feet are occluded, or a target is on stairs or a second floor, positions and tracks should be treated as reference only.

<!-- Image placement: Web bird's-eye view, calibration quality and ground zones. -->

### 7.7 Parameter Settings

On the **Settings** page, categories are listed on the left and the corresponding parameter form is shown on the right. To save one category, click the save button at the bottom of that page. The page then reports success or the reason for failure.

#### 7.7.1 Settings Categories

| Settings Page | Main Content | Application Method |
| --- | --- | --- |
| General | Device name, log level, operating mode, interface language, feature switches and time parameters | Mostly hot-reloaded |
| Detection and Audio | Event audio, inference frame rate, confidence, NMS and tracking parameters | Hot-reloaded |
| Video and Inference | Camera node, resolution, frame rate, model paths and Web preview parameters | Camera, model and Web parameters normally require a restart |
| Display | Performance metrics, video status below the title, activity heatmap, bird's-eye picture-in-picture and region opacity | Hot-reloaded |
| Ground Positioning | Ground-positioning master switch, device latitude/longitude and positioning sources | Hot-reloaded |
| Events and Storage | Debounce, cooldown, snapshot quality, clip duration and retention policy | Hot-reloaded |
| Advanced JSON | Complete configuration file | Atomically written and hot-reloaded after validation |

Feature switches, region parameters, event policies and display parameters normally take effect within one second after saving. Changes to the camera node, capture resolution, model paths and Web port require restarting the `monitor-box` service.

#### 7.7.2 Configuration Backup and Restore

The recommended way to back up configuration through the Web UI is:

1. Open **Settings → Advanced JSON**;
2. Click **Reload** to read the current complete configuration;
3. Select all text, copy it and save it to a local file;
4. To restore, paste the saved JSON back into the text box;
5. Click **Validate and Save Full Configuration**.

The backend validates the configuration. If validation fails, the existing configuration is not overwritten and the page displays the reason.

> Advanced JSON replaces the complete configuration in one operation. Back it up before saving and verify the JSON syntax, field names and value ranges. Invalid configuration can disable features, prevent models from loading or invalidate camera parameters.

### 7.8 Runtime Status

The **Status** page displays device runtime information:

- Capture frame rate, AI time, per-frame processing time and link latency;
- Camera node, connection state and reconnect count;
- Inference models, tracked-target count and encoding state;
- Saved event count, triggered event count and clip state;
- View-position change detection and automatic region-follow state;
- Current loitering sessions and historical statistics;
- Configuration-file path, event storage directory and storage usage;
- Shortcuts to Events, Regions and Settings.

If live video is abnormal, first inspect camera, inference, encoding and error counters on the **Status** page.

### 7.9 HTTP API

The Web UI also provides HTTP APIs for script integration and secondary development. The default port is `8080`. All endpoints return JSON or image data and include CORS headers for cross-origin requests.

The APIs are grouped as follows:

- **Pages and resources**: home page, stylesheets, scripts and icons;
- **Video and snapshots**: MJPEG stream, annotated snapshot and raw snapshot;
- **Events and clips**: event list, details, snapshots, clips and deletion;
- **Configuration**: read and replace the complete configuration;
- **Status**: runtime status, counts, heatmap, positioning, loitering and related data;
- **Control**: power-mode switching, view alignment actions, counter reset and similar operations.

| Endpoint | Method | Description |
| --- | --- | --- |
| `/` | GET | Web management page |
| `/stream` | GET | MJPEG live video |
| `/snapshot.jpg` | GET | Current annotated frame |
| `/snapshot.jpg?raw=1` | GET | Raw frame without OSD |
| `/status` | GET | Runtime status and link metrics |
| `/config` | GET / PUT | Read or replace the complete configuration |
| `/events` | GET | Query alarm events |
| `/events/<id>` | GET / DELETE | Read or delete one event |
| `/events/<id>.jpg` | GET | Event snapshot |
| `/events/<id>.mp4` | GET / DELETE | Play or delete an event clip |
| `/events/delete` | POST | Batch-delete events |
| `/counts` | GET / DELETE | Query or reset line-crossing counts |
| `/heatmap` | GET / DELETE | Query or clear the activity heatmap |
| `/ground` | GET | Query ground positioning and bird's-eye data |
| `/ground/stats` | DELETE | Reset ground-zone statistics |
| `/geo` | GET | Query device latitude/longitude |
| `/geo/refresh` | POST | Refresh device location immediately |
| `/power` | GET / POST | Query or switch Normal, Eco and Standby modes |
| `/view` | GET / POST | Query or handle view-alignment notices |
| `/dwell` | GET | Query loitering sessions and statistics |

Common command-line examples:

```bash
# View runtime status
curl http://<device-ip>:8080/status

# Capture the current frame
curl -o snapshot.jpg http://<device-ip>:8080/snapshot.jpg

# View people counts
curl http://<device-ip>:8080/counts

# Reset people counts
curl -X DELETE http://<device-ip>:8080/counts

# Query the 20 most recent warnings
curl 'http://<device-ip>:8080/events?limit=20&level=warning'

# Switch to standby
curl -X POST -H "Content-Type: application/json" -d '{"mode":"standby"}' http://<device-ip>:8080/power

# Restore Eco mode
curl -X POST -H "Content-Type: application/json" -d '{"mode":"eco"}' http://<device-ip>:8080/power
```

### 7.10 Configuration Hot Reload and Restart Scope

Not every configuration value saved through the Web UI takes effect immediately. Parameters that can be hot-reloaded include:

- Detection feature switches and rule thresholds;
- Shapes of regions, counting lines and ground zones;
- Event debounce, cooldown and retention policies;
- Display parameters such as performance metrics, heatmap and bird's-eye picture-in-picture;
- Power mode and event audio;
- Positioning-related parameters.

The following changes normally require restarting the `monitor-box` service:

- Camera node, resolution, frame rate and pixel format;
- Detection, pose and fire/smoke model paths;
- Web listen address and port;
- Low-level H.264 encoder parameters such as bitrate and GOP.

If you are unsure whether a restart is required, save the change and observe its effect first. Restart the service if the feature does not behave as expected.

### 7.11 Web UI vs. Desktop App

Both the Web UI and desktop app can display video and manage configuration, but they suit different scenarios.

| Dimension | Web UI | Desktop App |
| --- | --- | --- |
| Deployment | Browser access; no installation required | Must be installed on the board and opened from the desktop |
| Video source | MJPEG preview | Prefers H.264 for better image quality and lower latency |
| Local audio | Depends on the browser and computer speakers | Uses the board audio output |
| Scope | Viewing, configuration and event management | Besides configuration, can directly control the local camera |
| Multiple users | Supports multiple browser clients | Single-user desktop interaction |
| Best suited for | Remote viewing, collaboration and mobile access | Local debugging, large-screen display and full-feature demonstrations |

In general, use the desktop app on the board for initial calibration and complex configuration. Use the Web UI for routine viewing, event handling and remote management.

### 7.12 Typical Demo Workflow

The following sequence demonstrates an intrusion alarm from opening the Web UI to reviewing the event. It can be used as an on-site demonstration guide.

1. Open a browser and visit `http://<device-ip>:8080/`;
2. On the **Live** page, confirm that the camera is online, the video is normal and detection boxes are visible;
3. Open the **Regions** page, draw a warning zone and bind **Intrusion Detection**;
4. Save the regions and wait for backend hot reload;
5. Open **Settings → General**, enable the **Intrusion Detection** master switch and save;
6. Return to the **Live** page and let a target walk into the warning zone;
7. Observe the detection box and region highlight, and check whether a new-event notice appears in the upper-right corner;
8. Open the **Events** page and inspect the new alarm, snapshot and clip;
9. Click **Details** to play the alarm clip and verify that the pre/post-event segments are complete;
10. Optional: open the **Status** page to inspect capture frame rate, AI time and event counts.

To demonstrate people counting or ground positioning instead, replace step 3 with drawing a crossing line or ground calibration, then enable the corresponding feature switch.

### 7.13 Usage Notes

- Web live video uses MJPEG. It is intended for browser preview and is not a high-real-time, low-latency video API.
- Opening several live views at the same time increases encoding, network and CPU load. Limit the number of concurrent clients.
- Events, ground calibration, heatmaps and configuration are written to device storage. Confirm available storage before performing frequent operations.
- `PUT /config` replaces the complete configuration file. Prefer saving through page settings or targeted per-item changes.
- Web APIs have no authentication and should not be exposed directly to the public Internet.
- Changes to the camera node, model paths, streaming parameters or Web port normally require restarting the `monitor-box` service.
- On first access after clearing the browser cache or in private mode, loading can take slightly longer. This is normal.
- Region-drawing precision is limited on mobile devices. Perform complex calibration in a desktop browser.

## 8 Performance Reference

The following values illustrate the hardware capability of the demo. Actual results depend on the camera, models, resolution and environment:

| Item | Reference Result |
| --- | --- |
| Video capture | 1080p MJPG at about 30 fps |
| Detection inference rate | 12 fps by default |
| YOLOv8s NPU time per frame | About 29.6 ms |
| RTMPose-t pose model | About 24 ms for three people |
| Fire/smoke model | About 14.1 ms per frame |
| Live video | Decoding, drawing and H.264 encoding run independently and do not wait for every inference result |

If power consumption or temperature is more sensitive at the site, switch to Eco mode under **Settings → Power and Power Saving**. Eco mode lowers capture and inference frame rates while alarm features remain active.

## 9 Troubleshooting

### 9.1 Camera Not Detected

Check whether the USB camera is connected, whether the system recognizes the UVC device and whether the configured node matches the actual node:

```bash
v4l2-ctl --list-devices
ls -l /dev/video*
```

If the device node has changed, modify `video.device`, or start `mb-capture` with `--device /dev/videoX`.

### 9.2 Camera Device Busy

`/dev/video*` is being used by another program. Confirm whether `mb-capture` is running and make the app use the **Auto** or **H.264 Stream** source. Do not let two processes open the camera directly at the same time.

### 9.3 No Video in the App

Check the following in order:

```bash
systemctl status monitor-box --no-pager
curl --fail http://127.0.0.1:8080/status
tail -n 100 /opt/monitor_box/log/service_error.log
```

Pay particular attention to `camera.connected`, `h264.running`, the capture node and the most recent error message.

### 9.4 Detection Model Load Failure

Check whether the model file exists and whether RKNN Runtime, headers and the model-conversion toolkit are compatible versions:

```bash
ls -l /opt/monitor_box/models/
ls -l /usr/lib/librknnrt.so
```

The default detector model is `yolov8s-640-640.rknn`, and the label file is `coco_80_labels_list.txt`. If the model and runtime versions do not match, model initialization cannot complete.

### 9.5 Feature Enabled but No Alarm

Both the feature master switch and the region binding must be satisfied. Open **Region Settings** and confirm that the target region is bound to the corresponding feature. Then open **Settings** and confirm that the feature master switch is on. Some capabilities also require additional models, such as fall detection and fire/smoke detection.

### 9.6 Settings Cannot Be Saved

While the monitoring service is running, the desktop app saves settings through the backend configuration API. If the backend is not running or the configuration file is not writable, the app may only be able to read configuration. Confirm the configuration path and permissions instead of directly relaxing file permissions:

```bash
curl http://127.0.0.1:8080/status
ls -l /opt/monitor_box/etc/monitor_box/monitor_box.json
```

### 9.7 Positioning Jitters or Has Large Errors

Check ground calibration first:

1. Are all four calibration points on the same level ground?
2. Were the correct real-world width and depth entered?
3. Is the target's foot point clearly visible?
4. Has the camera been moved or bumped?
5. Is the calibration area near the centre of the frame and away from edges with severe distortion?

Positioning error increases substantially with strong wide-angle lens distortion, when a target is sitting or lying down, or when targets are on multiple floor levels.

### 9.8 Browser Cannot Access the Web UI

First confirm the backend and port on the board:

```bash
systemctl is-active monitor-box
curl --fail http://127.0.0.1:8080/status
ss -ltnp | grep ':8080'
ip -br addr
```

`mb-capture` should listen on `0.0.0.0:8080`. Then test `http://<device-ip>:8080/` from another computer.

Respond according to the check result:

- `eth0` is `DOWN` / `NO-CARRIER`: connect the Ethernet cable, or run `make -C projects/demo-monitor-box web-adb` and use the URL printed by the script;
- Local VS Code Remote: browse to `http://<Ubuntu VM IP>:8080/`; do not use `127.0.0.1`;
- Only `127.0.0.1` is shown: the board has no usable LAN address, so other computers cannot connect directly;
- `systemctl` is not `active`: inspect `journalctl -u monitor-box -n 100 --no-pager` and `/opt/monitor_box/log/service_error.log`;
- The backend is healthy but the page still cannot be opened: check whether the browser and board are on the same subnet, whether router client isolation is enabled, and the `web.listen`, `web.port` and firewall settings.

## 10 Usage Notes

- A 1080p MJPG USB camera is recommended. Uncompressed YUYV can saturate USB bandwidth.
- Do not let the camera and the app connect directly to the same device node at the same time.
- Fire/smoke detection cannot replace certified fire equipment, and fall detection cannot replace human supervision.
- Alarm performance depends on mounting angle, lighting, occlusion, model thresholds and site background. Revalidate with on-site footage before operational deployment.
- The project assumes a trusted LAN by default; the Web APIs have no authentication.
- When replacing detection, pose or fire/smoke models, verify the model input size, class order, output format and RKNN version together.
