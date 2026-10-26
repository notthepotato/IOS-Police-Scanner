# Road Scanner for iPhone

Spoken alerts for mapped speed and license-plate (ALPR / Flock) cameras, plus police-gear alerts from a small ESP32 sensor. Alerts play through CarPlay or your car's Bluetooth the same way navigation directions do, so you never have to look at the phone.

**What's in this folder**

| Path | What it is |
|---|---|
| `RoadScanner/` | The iPhone app (SwiftUI) |
| `project.yml` | Project recipe the cloud build turns into an Xcode project |
| `.github/workflows/build-ipa.yml` | Builds the app on a GitHub cloud Mac, so you don't need one |
| `esp32/AxonSensor_CYD/` | Firmware for the ESP32-2432S028R "Cheap Yellow Display": detects Axon police gear, shows it on its screen, and sends alerts to the phone |

---

## 1. Build the app on GitHub (no Mac needed)

1. Create a new repository on GitHub. Public is easiest: GitHub's Mac build machines are free for public repos. Private works too, but uses your monthly free build minutes faster.
2. Upload **everything in this folder**, including the hidden `.github` folder. The simplest way is [GitHub Desktop](https://desktop.github.com): clone your empty repo, copy these files into it, then commit and push.
3. Open the repo's **Actions** tab. The **Build iPhone app** run starts automatically on every push and takes about 5–10 minutes. You can also start it by hand with **Run workflow**.
4. When it finishes with a green check, open the run, scroll to **Artifacts**, and download **RoadScanner-ipa**. Unzip it to get `RoadScanner.ipa`.

If the build fails, open the failed step, copy the red error lines, and send them to Claude to fix.

## 2. Install it on your iPhone from Windows (free Apple ID)

1. Install **iTunes from Apple's website** (not the Microsoft Store version) and **[Sideloadly](https://sideloadly.io)**.
2. Plug your iPhone into the PC with a cable and tap **Trust** on the phone if it asks.
3. Open Sideloadly, drag in `RoadScanner.ipa`, enter your Apple ID, and click **Start**. You can use a second Apple ID if you'd rather not use your main one; it must have been signed in on an iPhone at least once.
4. On the iPhone:
   - **Settings > General > VPN & Device Management**: tap your Apple ID and tap **Trust**.
   - If iOS says Developer Mode is required: **Settings > Privacy & Security > Developer Mode**, turn it on, and restart the phone.

**Every 7 days** the free signature expires and the app stops opening. Run Sideloadly again with the same `.ipa` **over the existing app**. Don't delete the app first, or you'll lose your saved cameras and settings.

## 3. Use it

1. Open Road Scanner and tap **Start scanning**. Allow location (**While Using** is enough) and Bluetooth.
2. The first GPS fix downloads every mapped camera within 20 km (about 12 miles) and saves it on the phone. Do this once at home on Wi-Fi. After that it re-downloads automatically when you've driven about 10 km away, and keeps working with no signal.
3. Connect to CarPlay and tap **Test voice**. You should hear it through the car speakers.
4. Switch to Maps or music. Road Scanner keeps running in the background (you'll see the blue location indicator). **Don't swipe it away** in the app switcher, or it stops.

What you'll hear:
- **Cameras:** "Flock camera ahead, 600 feet." Cameras that are already behind you stay quiet. Each camera alerts once per pass.
- **Police gear:** "Police equipment close." Requires the ESP32 sensor below.

**Settings:** the alert distance (0.1, ¼ or ½ mile), voice on/off, and ESP32 sensor on/off.

## 4. Set up the police sensor (ESP32 "Cheap Yellow Display")

iPhones hide other devices' Bluetooth addresses from apps, so the phone can't spot Axon gear by itself. The sensor is an **ESP32-2432S028R**, the 2.8" touchscreen board often called the "Cheap Yellow Display" (CYD). It sits in the car, does the scanning, shows what it finds on its own screen, and sends alerts to the app.

**You need** the board, the micro-USB cable it comes with, and a USB car charger.

### Flash it (Arduino IDE on Windows)

1. Install [Arduino IDE 2](https://www.arduino.cc/en/software).
2. **Boards Manager:** search **esp32**, then install **esp32 by Espressif Systems** (version 3.x).
3. **Library Manager:** install these three:
   - **NimBLE-Arduino** by h2zero (version 2.x)
   - **Adafruit ILI9341** (click **Install all** when it asks about Adafruit GFX and Adafruit BusIO)
   - **XPT2046_Touchscreen** by Paul Stoffregen
4. Open `esp32/AxonSensor_CYD/AxonSensor_CYD.ino`. Keep `RScanTypes.h` in the same folder; it opens as a second tab.
5. **Tools > Board > esp32 > ESP32 Dev Module**, and pick the board's COM port.
6. Click **Upload**. If it stalls at "Connecting…", hold the **BOOT** button on the back of the board until the upload starts.

**If no COM port shows up**, Windows needs the board's USB driver (the CH340 or CP210x chip next to the USB port). Install the driver for whichever chip yours has, then replug the board.

### Using it

| Screen | Meaning |
|---|---|
| Green **ALL CLEAR** | Scanning, no police gear in range. Shows when gear was last seen. |
| Red **POLICE GEAR** | Axon gear in range. Live signal bars, how close it is, and how many devices. The red LED on the back lights too. |
| **Phone** dot (top right) | Green when the iPhone app is connected. |

| Control | Action |
|---|---|
| Tap the screen | Mute or unmute the beeps |
| Hold the screen ~1 s | Cycle brightness 100% → 50% → 20%, handy at night |
| **BOOT** button | Fix the colors if the screen looks inverted (some board revisions, usually the ones with both micro-USB and USB-C) |

All three settings are remembered after power-off.

**Optional speaker:** the board has a 2-pin **SPEAK** connector (1.25 mm). Plug in a small 8 Ω speaker and it beeps three times on each new detection. Without one, the screen and phone still alert.

**Optional settings** at the top of the sketch: `SCREEN_ROTATION` (set `3` to flip the screen upside down), how long a device counts as "still here", and the beep pitch.

The Serial Monitor (115200 baud) prints every detection, which is handy for testing at home.

---

## Limits worth knowing

- **Coverage:** camera locations come from OpenStreetMap volunteers (the DeFlock project), so coverage is only as good as local mapping.
- **Nevada:** state law bans red-light and speed cameras, so around Las Vegas you'll mostly get plate-reader alerts. Speed cameras matter when you drive into other states.
- **Police gear range:** Bluetooth reaches roughly tens of meters. "Police equipment" means an officer's gear is close by, not that a speed trap is ahead. Not every officer's gear broadcasts.
- **Mounting the sensor:** put it where it doesn't block your view of the road, and check the windshield-mount rules for any state you drive through. The phone voice and beeps are enough while driving, so you don't need to watch the screen.
- **CarPlay screen:** the app plays audio only and doesn't appear on the CarPlay screen. Putting it there would need a paid developer account and Apple's approval of a CarPlay entitlement.
- **Driving:** set things up before you drive. The voice is there so you don't have to touch the phone.
