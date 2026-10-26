# Road Scanner for iPhone

Spoken alerts for mapped speed and license-plate (ALPR / Flock) cameras, plus police-gear alerts from a small ESP32 sensor. Alerts play through CarPlay or your car's Bluetooth the same way navigation directions do, so you never have to look at the phone.

**What's in this folder**

| Path | What it is |
|---|---|
| `RoadScanner/` | The iPhone app (SwiftUI) |
| `project.yml` | Project recipe the cloud build turns into an Xcode project |
| `.github/workflows/build-ipa.yml` | Builds the app on a GitHub cloud Mac, so you don't need one |
| `esp32/AxonSensor/AxonSensor.ino` | ESP32 firmware that detects Axon police gear and sends alerts to the phone |

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

## 4. Build the ESP32 police sensor

iPhones hide other devices' Bluetooth addresses from apps, so the phone can't spot Axon gear by itself. A cheap ESP32 board in the car does the scanning and forwards alerts to the app.

**You need** any ESP32 with Bluetooth LE (ESP32, ESP32-S3, -C3 or -C6; **not the ESP32-S2**), a USB cable, and a USB car charger.

1. In Arduino IDE, install the **esp32** board package (Boards Manager), then install **NimBLE-Arduino by h2zero**, version 2.x (Library Manager).
2. Open `esp32/AxonSensor/AxonSensor.ino`, pick your board and port, and upload.
3. Optional settings at the top of the sketch:
   - `LED_PIN`: the onboard LED. GPIO 2 on most dev boards; set `-1` to disable.
   - `BUZZER_PIN`: an active buzzer, so the sensor also works without the phone.
4. Power it from the car's USB port. With the app scanning, it connects on its own, the LED stays lit while connected, and the app shows **ESP32 sensor: Connected**.

The Serial Monitor (115200 baud) prints every detection, which is handy for testing at home.

---

## Limits worth knowing

- **Coverage:** camera locations come from OpenStreetMap volunteers (the DeFlock project), so coverage is only as good as local mapping.
- **Nevada:** state law bans red-light and speed cameras, so around Las Vegas you'll mostly get plate-reader alerts. Speed cameras matter when you drive into other states.
- **Police gear range:** Bluetooth reaches roughly tens of meters. "Police equipment" means an officer's gear is close by, not that a speed trap is ahead. Not every officer's gear broadcasts.
- **CarPlay screen:** the app plays audio only and doesn't appear on the CarPlay screen. Putting it there would need a paid developer account and Apple's approval of a CarPlay entitlement.
- **Driving:** set things up before you drive. The voice is there so you don't have to touch the phone.
