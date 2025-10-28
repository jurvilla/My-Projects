// general libraries
// Things I might need to add: 
// -- Sleep for INA226, BME680, HM 3300/3600
// -- Add these specs at end of file, this is where the sleep mode is activated for the esp32
// -- Add the power up calls for the sensors in the beginning of the code once the esp32 has woken up
#include <math.h>
#include <Wire.h>
// System Configuration and Power Management
#define POWER_MONITOR_MODE   0  // SET TO 1 TO MOVE INA226 TO BATTERY_LIFE_TEST ESP32, 0 TO KEEP ON SENSOR ESP32
#define SLEEP_MODE           2  // 0 = NO SLEEP (delay), 1 = LIGHT SLEEP, 2 = DEEP SLEEP
#define ENABLE_DEBUG_SERIAL  1  // SET TO 1 TO ENABLE DEBUG SERIAL (TESTING ONLY, WASTES POWER), 0 TO DISABLE
#define IAQ_ALT_LOCAL_CALC   0  // SET TO 1 TO CALCULATE IAQ AND ALTITUDE LOCALLY, 0 TO SEND NO VALUE FOR IAQ OR ALTITUDE (SAVES POWER)
#define EXTRA_POWER_DATA     0  // SET TO 1 TO READ ALL POWER DATA (SHUNT, LOAD, POWER), 0 TO READ ONLY BUS VOLTAGE AND CURRENT (SAVES POWER)
#define PACKET_FREQ_MS       60000 // Frequency to send packets, in milliseconds (60000 = 1 minute)
#define SLEEP_TIME_US        (PACKET_FREQ_MS * 1000)  // Convert ms to microseconds

// sensor libraries
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Tomoto_HM330X.h>

#if !POWER_MONITOR_MODE
#include <INA226_WE.h>
#endif

// defs
#define SDA_PIN              21
#define SCL_PIN              22
#define SERIAL2_RX_PIN       16
#define SERIAL2_TX_PIN       17
#define BME_I2C_ADDRESS      0x77
#define PARTICLE_I2C_ADDRESS 0x40

#if !POWER_MONITOR_MODE
#define POWER_I2C_ADDRESS    0x41
#endif

// consts for algorithm for IAQ and Altitude, from https://github.com/G6EJD/BME680-Example 
#if IAQ_ALT_LOCAL_CALC
#define SEALEVELPRESSURE_HPA 1014.0F
const float hum_w = 0.25f, gas_w = 0.75f, hum_ref = 40.0f;
float gas_ref = 0.0f;
int   gas_ref_count = 0;
#endif

// sensor object instantiations
Adafruit_BME680 bme;
Tomoto_HM330X pm;
#if !POWER_MONITOR_MODE
INA226_WE ina226 = INA226_WE(POWER_I2C_ADDRESS);
#endif


void setup() {

  //pinMode(gpio_num_t(34), OUTPUT); // wake up  hm 3300/3600
  //digitalWrite(gpio_num_t(34), HIGH);

#if ENABLE_DEBUG_SERIAL
  // setup usb serial for debugging
  Serial.begin(115200);
  delay(1000); // Give time for serial to initialize
  
  // Check what caused the ESP32 to wake up
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.print("Wake up reason: ");
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("External signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("External signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP: Serial.println("ULP program"); break;
    default: Serial.printf("Not a deep sleep reset: %d\n", wakeup_reason); break;
  }
  Serial.print("Time since boot: ");
  Serial.print(millis());
  Serial.println(" ms");
  Serial.println("=== SENSOR INITIALIZATION ===");
#endif
  // uart connection to wireless tracker
  Serial2.begin(9600, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
  // begin wire for i2c
  Wire.begin(SDA_PIN, SCL_PIN);
 
  // bme680 init;
  if(!bme.begin(BME_I2C_ADDRESS)){
#if ENABLE_DEBUG_SERIAL
    Serial.println("Could not find a valid BME680 sensor, check wiring!");
    while(1){}
#else
    while(1){}  // Still halt on error even without debug
#endif
  } else {
#if ENABLE_DEBUG_SERIAL
    Serial.println("BME680 started.");
#endif
  }
  // rest of bme680 settings - balanced power and accuracy
  bme.setTemperatureOversampling(BME680_OS_2X);  // Balanced accuracy vs power
  bme.setHumidityOversampling   (BME680_OS_2X);  // Library default - good balance
  bme.setPressureOversampling   (BME680_OS_4X);  // Library default - good balance  
  bme.setIIRFilterSize          (BME680_FILTER_SIZE_3);  // Library default - moderate filtering
  bme.setGasHeater(320, 120);  // Slightly reduced heating time from 150ms

#if IAQ_ALT_LOCAL_CALC
  // burn-in gas to initialize gas_ref as an average of 10 gas readings
  {
    float tot = 0;
    for(int i = 0; i < 10; i++) { 
      bme.performReading();
      tot += bme.gas_resistance;
      // No delay needed - performReading() already waits for measurement completion
    }
    gas_ref = tot / 10.0f;  // divide by 10 for average
  }
#endif

  // particle sensor init
  if(pm.begin(PARTICLE_I2C_ADDRESS)){
#if ENABLE_DEBUG_SERIAL
    Serial.println("HM3301 started.");
#endif
  }

  // power sensor init
#if !POWER_MONITOR_MODE
  if(!ina226.init()){
#if ENABLE_DEBUG_SERIAL
    Serial.println("Failed to init INA226. Check your wiring.");
    while(1){}
#else
    while(1){}  // Still halt on error even without debug
#endif
  } else {
    //ina226.powerUp();
#if ENABLE_DEBUG_SERIAL
    Serial.println("Power sensor started.");
#endif
  }
  ina226.waitUntilConversionCompleted(); //if you comment this line the first data might be zero
#else
#if ENABLE_DEBUG_SERIAL
  Serial.println("Power sensor disabled - moved to battery_life_test ESP32");
#endif
#endif
}

void loop() {
  String packet = "";

  // Note: Timestamp now added by battery_life_test.ino for accurate timing in deep sleep mode

  // --- BME680 ---
  float t = NAN, h = NAN, p = NAN, g = NAN, alt = NAN, iaq = NAN;
  if (bme.performReading()) {
    t   = bme.temperature;
    h   = bme.humidity;
    p   = bme.pressure  / 100.0f;
    g   = bme.gas_resistance;

#if IAQ_ALT_LOCAL_CALC
    alt = bme.readAltitude(SEALEVELPRESSURE_HPA);
    // humidity score (0–25%)
    float hs = (h >= 38 && h <= 42) ? hum_w * 100.0f : (h < 38 ? hum_w/hum_ref * h * 100.0f : (((-hum_w/(100.0f - hum_ref) * h) + (hum_w * 100.0f/(100.0f - hum_ref))) * 100.0f));
    // gas score (0–75%)
    const float gl = 5000.0f, gu = 50000.0f;
    float ref = constrain(gas_ref, gl, gu);
    float gs = ((gas_w/(gu - gl) * ref) - (gl * (gas_w/(gu - gl)))) * 100.0f;
    iaq = hs + gs;
    
    // every 10 cycles of the main loop, update gas_ref as an average of 10 gas readings
    if (++gas_ref_count >= 10) {
      float tot = 0;
      for(int i = 0; i < 10; i++) {
        bme.performReading();
        tot += bme.gas_resistance;
        // No delay needed - performReading() already waits for measurement completion
      }
      gas_ref = tot / 10.0f;
      gas_ref_count = 0;
    }
#endif

#if ENABLE_DEBUG_SERIAL
    Serial.print("temp_C_C: "); Serial.println(t);
    Serial.print("hum_pct_%: "); Serial.println(h);
    Serial.print("pres_hPa: "); Serial.println(p);
    Serial.print("gas_Ohm: "); Serial.println(g);
#if IAQ_ALT_LOCAL_CALC
    Serial.print("alt_m: "); Serial.println(alt);
    Serial.print("iaq_pct: "); Serial.println(iaq);
#else
    Serial.println("alt_m: DISABLED");
    Serial.println("iaq_pct: DISABLED");
#endif
#endif
    packet += String(t);
    packet += "," + String(h);
    packet += "," + String(p);
    packet += "," + String(g);
#if IAQ_ALT_LOCAL_CALC
    packet += "," + String(alt);
    packet += "," + String(iaq);
#endif
  } else {
#if ENABLE_DEBUG_SERIAL
    Serial.println("temp_C_C: ERR");
    Serial.println("hum_pct_%: ERR");
    Serial.println("pres_hPa: ERR");
    Serial.println("gas_Ohm: ERR");
#if IAQ_ALT_LOCAL_CALC
    Serial.println("alt_m: ERR");
    Serial.println("iaq_pct: ERR");
#else
    Serial.println("alt_m: DISABLED");
    Serial.println("iaq_pct: DISABLED");
#endif
#endif
#if IAQ_ALT_LOCAL_CALC
    packet += ",,,,,,";
#else
    packet += ",,,";
#endif
  }

  // --- Particle Sensor ---
  float pm1_std = NAN, pm2_5_std = NAN, pm10_std = NAN;
  // float pm1_atm = NAN, pm2_5_atm = NAN, pm10_atm = NAN;
  if (pm.readSensor()) {
    pm1_std   = pm.std.getPM1();
    pm2_5_std = pm.std.getPM2_5();
    pm10_std  = pm.std.getPM10();
    // pm1_atm   = pm.atm.getPM1();
    // pm2_5_atm = pm.atm.getPM2_5();
    // pm10_atm  = pm.atm.getPM10();

#if ENABLE_DEBUG_SERIAL
    Serial.print("pm1_std: "); Serial.println(pm1_std);
    Serial.print("pm2_5_std: "); Serial.println(pm2_5_std);
    Serial.print("pm10_std: "); Serial.println(pm10_std);
    // Serial.print("pm1_atm: "); Serial.println(pm1_atm);
    // Serial.print("pm2_5_atm: "); Serial.println(pm2_5_atm);
    // Serial.print("pm10_atm: "); Serial.println(pm10_atm);
#endif
    packet += "," + String(pm1_std);
    packet += "," + String(pm2_5_std);
    packet += "," + String(pm10_std);
    // packet += "," + String(pm1_atm);
    // packet += "," + String(pm2_5_atm);
    // packet += "," + String(pm10_atm);
  } else {
#if ENABLE_DEBUG_SERIAL
    Serial.println("pm1_std: ERR");
    Serial.println("pm2_5_std: ERR");
    Serial.println("pm10_std: ERR");
    // Serial.println("pm1_atm: ERR");
    // Serial.println("pm2_5_atm: ERR");
    // Serial.println("pm10_atm: ERR");
#endif
    packet += ",,,";
  }

  // --- Power Sensor ---
#if !POWER_MONITOR_MODE
#if EXTRA_POWER_DATA
  float shuntVoltage_mV = ina226.getShuntVoltage_mV();
  float busVoltage_V = ina226.getBusVoltage_V();
  float loadVoltage_V = busVoltage_V + (shuntVoltage_mV/1000);
  float current_mA = ina226.getCurrent_mA();
  float power_mW = ina226.getBusPower();
#else
  float busVoltage_V = ina226.getBusVoltage_V();
  float current_mA = ina226.getCurrent_mA();
#endif

#if ENABLE_DEBUG_SERIAL
#if EXTRA_POWER_DATA
  Serial.print("shuntVoltage_mV: "); Serial.println(shuntVoltage_mV);
  Serial.print("busVoltage_V: "); Serial.println(busVoltage_V);
  Serial.print("loadVoltage_V: "); Serial.println(loadVoltage_V);
  Serial.print("current_mA: "); Serial.println(current_mA);
  Serial.print("power_mW: "); Serial.println(power_mW);
#else
  Serial.print("busVoltage_V: "); Serial.println(busVoltage_V);
  Serial.print("current_mA: "); Serial.println(current_mA);
#endif
#endif
#if EXTRA_POWER_DATA
  packet += "," + String(shuntVoltage_mV);
  packet += "," + String(busVoltage_V);
  packet += "," + String(loadVoltage_V);
  packet += "," + String(current_mA);
  packet += "," + String(power_mW);
#else
  packet += "," + String(busVoltage_V);
  packet += "," + String(current_mA);
#endif
#else
  // Power monitoring disabled - handled by battery_life_test ESP32
#if ENABLE_DEBUG_SERIAL
  Serial.println("Power monitoring: DISABLED (handled by battery_life_test)");
#endif
#endif


  // --- Send packet over Serial2 ---
  Serial2.println(packet);

  // Esp32 goes to sleep, 3 different modes:
#if ENABLE_DEBUG_SERIAL
  Serial.print("PACKET: ");
  Serial.println(packet);
  Serial.print("About to sleep for ");
  Serial.print(SLEEP_TIME_US / 1000000);
  Serial.print(" seconds (");
  Serial.print(SLEEP_TIME_US);
  Serial.println(" microseconds)");
  Serial.print("Current millis(): ");
  Serial.println(millis());
#endif

  Serial2.flush();  // Ensure all data is sent before sleeping
#if POWER_MONITOR_MODE // puts ina226 into sleep when esp32 is sleeping
  //ina226.powerDown();
#endif
#if SLEEP_MODE == 2
  //Deep Sleep - Maximum power savings, restarts ESP32
  //digitalWrite(gpio_num_t(34), LOW);
  //gpio_hold_en(gpio_num_t(34));
#if ENABLE_DEBUG_SERIAL
  Serial.println("Entering DEEP SLEEP...");
  Serial.flush();
#endif
  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_deep_sleep_start();

#elif SLEEP_MODE == 1
  // Light Sleep - Good power savings, preserves memory/state
#if ENABLE_DEBUG_SERIAL
  Serial.println("Entering LIGHT SLEEP...");
  Serial.flush();
#endif
  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_light_sleep_start();
#if ENABLE_DEBUG_SERIAL
  Serial.println("Woke up from light sleep");
  Serial.println();
#endif

#else
  // No Sleep - Just delay, maximum power consumption but good for debugging
#if ENABLE_DEBUG_SERIAL
  Serial.println("Using DELAY (no sleep)...");
  Serial.println();
#endif
  delay(PACKET_FREQ_MS);
#endif
}

