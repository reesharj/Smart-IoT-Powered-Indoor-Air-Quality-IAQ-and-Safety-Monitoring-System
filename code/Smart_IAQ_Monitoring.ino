#define BLYNK_PRINT Serial

#define BLYNK_TEMPLATE_ID "TMPL6Q6Uu2cHD"
#define BLYNK_TEMPLATE_NAME "Smart Kitchen"
#define BLYNK_AUTH_TOKEN "YOUR_BLYNK_AUTH_TOKEN"

// --- TELEGRAM CONFIG ---
#define TELEGRAM_BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define TELEGRAM_CHAT_ID "YOUR_TELEGRAM_CHAT_ID"
#define TELEGRAM_CHAT_ID2 "YOUR_SECOND_TELEGRAM_CHAT_ID"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <BlynkSimpleEsp32.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// Create a secure WiFi client for HTTPS
WiFiClientSecure client;

//Create Telegram bot object
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, client);

// --- WiFi Credential ---
char ssid[] = "YOUR_WIFI_NAME";
char pass[] = "YOUR_WIFI_PASSWORD";

// --- Telegram Alert Message ---
bool dangerAlertSent = false;

// --- Create Array for all users ---
const char* TELEGRAM_USERS[] = {TELEGRAM_CHAT_ID, TELEGRAM_CHAT_ID2};   //Add went user increase
const int NUM_USERS = sizeof(TELEGRAM_USERS)/sizeof(TELEGRAM_USERS[0]);

// --- PIN DEFINITIONS ---
const int BUZZER_PIN = 18;
const int RELAY_PIN = 19;
const int MQ2_PIN = 34; 
const int PIR_PIN = 23;

// RGB Pins (Common Anode)
const int LED_RED_PIN = 25;  
const int LED_GREEN_PIN = 26; 
const int LED_BLUE_PIN = 27; 

// --- SENSOR CONFIGURATION ---
const float RL_VALUE = 5.0; 
const float ADC_REF = 3.3;
const float RO_VALUE = 10.5; // Calibrated Ro value (MUST use ratio for accuracy)

// MQ-2 curve constants (log-log scale)
float LPG_Curve[2]   = {-0.45, 2.30};
float Smoke_Curve[2] = {-0.42, 2.33};

// --- PPM THRESHOLDS ---
const int GAS_THRESHOLD_PPM = 800;
const int WARNING_THRESHOLD_PPM = 400;

// --- TIMERS ---
const unsigned long WARNING_DELAY_MS = 5000; // 10 seconds
const unsigned long LOOP_PERIOD_MS = 1000;
const unsigned long MONITORING_HEARTBEAT_MS = 500;

// --- RELAY LOGIC ---
const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

// --- GLOBAL VARIABLES ---
unsigned long loopTimer = 0;
unsigned long heartbeatTimer = 0;
unsigned long warningNoMotionTimestamp = 0;
bool warningTimerActive = false;
int blinkState = LOW; // Used for heartbeat/blinking LEDs

// --- FILTERING ---
float filtered_adc = 0;
const float alpha = 0.15; // EMA smoothing factor

// --- BLYNK ---
BlynkTimer timer;

// --- HELPER FUNCTION FOR COMMON ANODE LED ---
// Logic: LOW = ON, HIGH = OFF
void setRgbLed(int redOn, int greenOn, int blueOn) {
    digitalWrite(LED_RED_PIN, redOn ? LOW : HIGH);
    digitalWrite(LED_GREEN_PIN, greenOn ? LOW : HIGH);
    digitalWrite(LED_BLUE_PIN, blueOn ? LOW : HIGH);
}

// --- SEND DATA TO BLYNK ---
void sendToBlynk(float lpg_ppm, int relayState, int pirState) {

  //  LPG PPM
  Blynk.virtualWrite(V0, lpg_ppm);

  // Reset all status LEDs
  Blynk.virtualWrite(V2, 0); //Safe
  Blynk.virtualWrite(V1, 0); //Cooking  
  Blynk.virtualWrite(V5, 0); //Danger

  // Gas status
  if (lpg_ppm >= GAS_THRESHOLD_PPM) {
    Blynk.virtualWrite(V5, 255);   // Danger
  } else if (lpg_ppm >= WARNING_THRESHOLD_PPM) {
    Blynk.virtualWrite(V1, 255);   // Cooking
  } else {
    Blynk.virtualWrite(V2, 255);     // Safe
  }

  // Fan status
  Blynk.virtualWrite(V3, relayState == RELAY_ON ? 255 : 0);

  // Motion
  Blynk.virtualWrite(V4, pirState == HIGH ? 255 : 0);
}

void sendTelegramToAll(String message) {
    for (int i = 0; i < NUM_USERS; i++) {
        bot.sendMessage(TELEGRAM_USERS[i], message, ""); // send message
        Serial.println("Telegram alert sent to: " + String(TELEGRAM_USERS[i]));
    }
}

void setup() {
    Serial.begin(115200);
    
    analogReadResolution(12);

    Blynk.begin(BLYNK_AUTH_TOKEN,ssid,pass);
    client.setInsecure();
    Serial.println("--- IDP System - PPM Monitoring Started ---");

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);
    pinMode(PIR_PIN, INPUT);

    // Initial State
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RELAY_PIN, RELAY_OFF);
    setRgbLed(0, 0, 0); // All OFF

    // --- MQ-2 Warm-up ---
    // Note: In a final project, you can reduce this, but 3 mins is standard for stability.
    Serial.println("Warming up MQ-2 sensor...");
    delay(5000); // Shortened for testing, use longer for accuracy
    Serial.println("MQ-2 Ready.");
    
}

void loop() {
  Blynk.run();
  timer.run();
  
    unsigned long currentTime = millis();

    // --- HEARTBEAT BLINK TRIGGER ---
    if (currentTime - heartbeatTimer >= MONITORING_HEARTBEAT_MS) {
        heartbeatTimer = currentTime;
        blinkState = !blinkState;
    }

    // --- READ MQ-2 (EMA FILTER) ---
    int raw = analogRead(MQ2_PIN);
    filtered_adc = (alpha * raw) + ((1 - alpha) * filtered_adc);
    
    // Convert ADC to voltage
    float volt = (filtered_adc / 4095.0) * ADC_REF;
    if (volt < 0.05) volt = 0.05; // Prevent log errors

    // Calculate RS and Ratio
    float rs = ((ADC_REF * RL_VALUE) / volt) - RL_VALUE;
    float ratio = rs / RO_VALUE; 

    // Calculate PPM using Ratio
    float lpg_ppm   = pow(10, ((log10(ratio) - LPG_Curve[1]) / LPG_Curve[0]));
    float smoke_ppm = pow(10, ((log10(ratio) - Smoke_Curve[1]) / Smoke_Curve[0]));

    int pirState = digitalRead(PIR_PIN);

    // --- STEP 1: LED COLOR LOGIC (Common Anode) ---
    if (lpg_ppm >= GAS_THRESHOLD_PPM) {
        setRgbLed(blinkState, 0, 0); // Blink RED
    } else if (lpg_ppm >= WARNING_THRESHOLD_PPM) {
        setRgbLed(0, blinkState, 0); // Blink GREEN
    } else {
        setRgbLed(0, 0, blinkState); // Blink BLUE (Heartbeat)
    }

    // --- STEP 2: MAIN SAFETY LOGIC (Every 1 second) ---
    if (currentTime - loopTimer >= LOOP_PERIOD_MS) {
        loopTimer = currentTime;

        Serial.print("LPG: "); Serial.print(lpg_ppm);
        Serial.print(" ppm | Smoke: "); Serial.print(smoke_ppm);
        Serial.print(" ppm | Ratio: "); Serial.print(ratio);
        Serial.print(" | Motion: ");
        Serial.println(pirState == HIGH ? "DETECTED" : "NONE");

        int desiredFanState = RELAY_OFF;
        int desiredBuzzerState = LOW;

        // A: DANGER STATE
        if (lpg_ppm >= GAS_THRESHOLD_PPM) {
            desiredFanState = RELAY_ON;
            desiredBuzzerState = HIGH;
            warningTimerActive = false;
            Serial.println("!!! DANGER: Critical Gas! Fan & Buzzer ON.");

            // send telegram alert only once
            if (!dangerAlertSent) {
              for (int i = 0; i < NUM_USERS; i++){
                  bot.sendMessage(TELEGRAM_USERS[i], "🚨 DANGER: LPG gas level critical! PPM=" + String(lpg_ppm), "");
                  Serial.println("Telegram alert sent to: " + String(TELEGRAM_USERS[i]));
              }
              dangerAlertSent = true;
            }
            else {
            dangerAlertSent = false; // reset once gas drops
            }
        }

        // B: WARNING STATE
        else if (lpg_ppm >= WARNING_THRESHOLD_PPM) {
            desiredFanState = RELAY_ON;

            if (pirState == LOW) {
                if (!warningTimerActive) {
                    warningNoMotionTimestamp = currentTime;
                    warningTimerActive = true;
                }

                if (currentTime - warningNoMotionTimestamp >= WARNING_DELAY_MS) {
                    Serial.println("!!! WARNING: No Motion Timeout. Buzzer ON.");
                    desiredBuzzerState = HIGH;
                } else {
                    Serial.println("WARNING: Fan ON, Buzzer Waiting for Timeout...");
                }
            } else {
                warningTimerActive = false;
                Serial.println("WARNING: Motion Detected. Fan ON, Buzzer OFF.");
            }
        }

        // C: SAFE STATE
        else {
            desiredFanState = RELAY_OFF;
            desiredBuzzerState = LOW;
            warningTimerActive = false;
            Serial.println("SAFE: System Normal.");
        }

        digitalWrite(RELAY_PIN, desiredFanState);
        digitalWrite(BUZZER_PIN, desiredBuzzerState);

        //Send to Blynk
        sendToBlynk(lpg_ppm,desiredFanState,pirState);
    }
}

