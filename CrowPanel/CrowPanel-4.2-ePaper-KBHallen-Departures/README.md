# KB Hallen St. Departure Board (CrowPanel ESP32-S3 5.79" e-Paper)

Live S-train departure board for **KB Hallen St.** (Rejseplanen stop id `8600642`),
built for the Elecrow CrowPanel ESP32-S3 5.79" e-Paper HMI Display (792x272).

(Note: this project folder is still named `CrowPanel-4.2-ePaper-...` from an earlier
iteration targeting the wrong panel size — safe to rename once it's not open in
Arduino IDE, since Arduino requires the `.ino` filename to match its folder name.)

## Why this doesn't use GxEPD2

GxEPD2 doesn't have a working driver for this panel. This sketch instead vendors
Elecrow's own bit-banged `EPD` / `EPD_Init` / `spi` driver (from their
[official example repo for this exact panel](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792)),
copied in alongside the sketch — it isn't published as an installable library.

This panel is physically driven by **two cascaded SSD1683 controller ICs**, addressed
internally as an 800px-wide buffer with an 8px seam gap between them — the driver
hides that automatically, so drawing code just uses the visible 792x272 area.

## Setup

1. **Get a free Rejseplanen API key.** The old open API has been shut down;
   API 2.0 requires an `accessId` for every request, including this station's
   departure board. Apply at
   [labs.rejseplanen.dk](https://labs.rejseplanen.dk/hc/da/articles/21553113674909-Adgang-til-data-fra-Labs)
   (non-commercial tier: 50,000 requests/month, free).
2. Copy `credentials_template.h` to `credentials.h` in this same folder and fill in
   your WiFi SSID/password and the `accessId` you received.
3. Install the **ArduinoJson** and **NTPClient** libraries via Arduino Library Manager.
4. Board setting: **ESP32S3 Dev Module** (enable USB CDC on boot if you want Serial
   output over the native USB port).
5. Wiring / pins are fixed to the CrowPanel's onboard panel connector in `spi.h`
   (CS 45, DC 46, RST 47, BUSY 48, SCK 12, MOSI 11) — no wiring needed if you're using
   the CrowPanel board as-is.

## Notes

- The board refreshes once a minute, using Elecrow's own tested clear/update
  sequence for this panel (`EPD_FastMode1Init` → `EPD_Display_Clear` → draw →
  `EPD_Display` → `EPD_PartUpdate` → `EPD_DeepSleep`).
- The panel's bitmap font is ASCII-only, so Danish characters (æ, ø, å) in station/
  destination names are transliterated (æ→ae, ø→o, å→aa) before drawing.
- A `!` prefix on the time means Rejseplanen has a real-time update that differs
  from the scheduled time (i.e. the train is running late); `X` means the
  departure is cancelled.
- `DISPLAY_ROTATION` at the top of the .ino is set to Elecrow's own tested default
  (180). If text ever comes out upside-down or mirrored on your unit, try 0, 90,
  or 270 instead — this panel's driver handles rotation differently per value
  (a genuine 180° rotation, not a mirror), unlike some other CrowPanel e-paper
  models.
- To point this at a different station, change `STATION_ID` / `STATION_NAME` at
  the top of the .ino — look up a stop's id via Rejseplanen's `location.name`
  endpoint (requires the same `accessId`).
