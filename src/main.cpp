// ===========================================================================
// ⚡ DuoClock ESP32 Firmware — Serial Button & LED Controller
// ===========================================================================
//
// This firmware runs on an ESP32 DevKit v1 and acts as the DuoClock I/O
// controller:
//   - Reads two physical buttons (THEM and ME)
//   - Sends button press events over USB serial to the Pi
//   - Receives LED control commands from the Pi over serial
//   - Tracks LED state and responds to status queries
//   - Supports a reset command to turn off all LEDs
//
// Timer logic, logging, and intelligence lives on the Pi Zero W.
// The ESP32 owns hardware state and can report it on request — this lets
// the Pi recover cleanly after a reboot without orphaning lit LEDs.
//
// ┌────────────────────────────────────────────────────────────────┐
// │                     SERIAL PROTOCOL                            │
// │                                                                │
// │  ESP32 → Pi:                                                  │
// │    "T\n"        — THEM button pressed (falling edge only)     │
// │    "M\n"        — ME button pressed (falling edge only)       │
// │    "DUOCLOCK\n" — Device identification on boot               │
// │    "ST0\n"/"ST1\n" — THEM LED state (response to S query)    │
// │    "SM0\n"/"SM1\n" — ME LED state (response to S query)      │
// │    "ROK\n"      — Acknowledge reset completed                 │
// │                                                                │
// │  Pi → ESP32:                                                  │
// │    "T1\n" — Turn THEM (red) LED ON                            │
// │    "T0\n" — Turn THEM (red) LED OFF                           │
// │    "M1\n" — Turn ME (yellow) LED ON                           │
// │    "M0\n" — Turn ME (yellow) LED OFF                          │
// │    "S\n"  — Query current LED state (reply: ST#, SM#)         │
// │    "R\n"  — Reset: turn off all LEDs, reply ROK               │
// └────────────────────────────────────────────────────────────────┘
//
// 📌 Pin Map:
//   GPIO 13 ← BTN_THEM (input, internal pull-up, active LOW)
//   GPIO 15 ← BTN_ME   (input, internal pull-up, active LOW)
//   GPIO 17 → LED_THEM (output, red LED via resistor)
//   GPIO 16 → LED_ME   (output, yellow LED via resistor)
//
// 🔧 Debouncing:
//   20ms delay-based debounce on both buttons. Only the press (HIGH→LOW
//   transition) is reported — not the release. This prevents double-firing.
//
// ===========================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 📌 Pin assignments — match the physical wiring
// ---------------------------------------------------------------------------
const int BTN_THEM = 13;  // 🔴 THEM button input (to GND, internal pullup)
const int BTN_ME   = 15;  // 🟡 ME button input   (to GND, internal pullup)
const int LED_THEM = 17;  // 🔴 THEM LED output    (red, through resistor)
const int LED_ME   = 16;  // 🟡 ME LED output      (yellow, through resistor)

// ---------------------------------------------------------------------------
// 🔄 Button state tracking for edge detection
// ---------------------------------------------------------------------------
// Buttons use INPUT_PULLUP, so unpressed = HIGH (true), pressed = LOW (false).
// We only send an event on the HIGH→LOW transition (press), not release.
// ---------------------------------------------------------------------------
bool lastBtnThem = true;  // Previous state of THEM button
bool lastBtnMe   = true;  // Previous state of ME button

// ---------------------------------------------------------------------------
// 💡 LED state tracking
// ---------------------------------------------------------------------------
// Mirrors the physical LED state so we can report it back to the Pi on
// request. This is essential for Pi reboot recovery — the Pi sends "S\n"
// after reconnecting and we reply with the current state of each LED.
// ---------------------------------------------------------------------------
bool ledThem = false;
bool ledMe   = false;

// ===========================================================================
// 📡 Process incoming serial commands from the Pi
// ===========================================================================
// Reads complete lines (terminated by \n) and maps them to actions:
//   T1/T0, M1/M0 — LED control (state tracked in ledThem/ledMe)
//   S            — Status query: reply with current LED state
//   R            — Reset: turn off all LEDs and acknowledge
// Case-insensitive. Unknown commands are silently ignored.
// ===========================================================================
void processSerial() {
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    // --- LED control commands (track state for status queries) ---
    if (cmd == "T1" || cmd == "t1") {
      digitalWrite(LED_THEM, HIGH);
      ledThem = true;
    } else if (cmd == "T0" || cmd == "t0") {
      digitalWrite(LED_THEM, LOW);
      ledThem = false;
    } else if (cmd == "M1" || cmd == "m1") {
      digitalWrite(LED_ME, HIGH);
      ledMe = true;
    } else if (cmd == "M0" || cmd == "m0") {
      digitalWrite(LED_ME, LOW);
      ledMe = false;

    // --- Status query: report current LED state back to Pi ---
    } else if (cmd == "S" || cmd == "s") {
      Serial.println(String("ST") + (ledThem ? "1" : "0"));
      Serial.println(String("SM") + (ledMe ? "1" : "0"));

    // --- Reset: turn off all LEDs and acknowledge ---
    // Used by the Pi on reconnect or from the Settings menu reboot.
    // Ensures no LEDs are left orphaned after a Pi restart.
    } else if (cmd == "R" || cmd == "r") {
      digitalWrite(LED_THEM, LOW);
      digitalWrite(LED_ME, LOW);
      ledThem = false;
      ledMe = false;
      Serial.println("ROK");
    }
  }
}

// ===========================================================================
// 🚀 Setup — runs once on boot
// ===========================================================================
void setup() {
  Serial.begin(115200);  // Must match Pi's baud_rate config
  delay(300);            // Brief delay for serial to stabilise

  // Configure LED pins as outputs (default LOW = off)
  pinMode(LED_THEM, OUTPUT);
  pinMode(LED_ME,   OUTPUT);

  // Configure button pins with internal pull-up resistors
  // Buttons connect GPIO to GND — pressed = LOW, released = HIGH
  pinMode(BTN_THEM, INPUT_PULLUP);
  pinMode(BTN_ME,   INPUT_PULLUP);

  // Identification string — the Pi monitor looks for this to confirm
  // it's talking to the right device
  Serial.println("DUOCLOCK");
}

// ===========================================================================
// 🔄 Main loop — poll buttons + process serial commands
// ===========================================================================
// Runs continuously. Checks for:
//   1. Incoming serial commands from the Pi (LED control)
//   2. THEM button state change (with 20ms debounce)
//   3. ME button state change (with 20ms debounce)
//
// The 5ms delay at the end prevents busy-spinning while still being
// responsive enough for button presses (human reaction time ~150ms).
// ===========================================================================
void loop() {
  processSerial();

  // --- 🔴 THEM button polling with debounce ---
  bool btnThem = digitalRead(BTN_THEM);
  if (btnThem != lastBtnThem) {
    delay(20);  // 20ms debounce
    btnThem = digitalRead(BTN_THEM);
    if (btnThem != lastBtnThem) {
      lastBtnThem = btnThem;
      if (!btnThem) Serial.println("T");  // Only on press (HIGH→LOW)
    }
  }

  // --- 🟡 ME button polling with debounce ---
  bool btnMe = digitalRead(BTN_ME);
  if (btnMe != lastBtnMe) {
    delay(20);  // 20ms debounce
    btnMe = digitalRead(BTN_ME);
    if (btnMe != lastBtnMe) {
      lastBtnMe = btnMe;
      if (!btnMe) Serial.println("M");  // Only on press (HIGH→LOW)
    }
  }

  delay(5);  // Small yield to prevent busy-spinning
}
