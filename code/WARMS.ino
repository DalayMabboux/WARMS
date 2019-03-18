/*
WARMS (c) by Dalay Mabboux

WARMS is licensed under a
Creative Commons Attribution-ShareAlike 4.0 International License.

You should have received a copy of the license along with this
work. If not, see <http://creativecommons.org/licenses/by-sa/4.0/>.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <PubSubClient.h>
#include <Wire.h> // must be included here so that Arduino library object file references work
#include <RtcDS3231.h>
#include <Adafruit_ADS1015.h>
#include "DHT.h"

// GPIO
const short int DHTPIN = 0; // GPIO0
const short int PUMP0 = 14; //GPIO14
const short int PUMP1 = 12; //GPIO12

// DIV
short int LL = 2; // Log level: 2 = debug, 1 = info, 0 = failure
short int sleepDuration = 1800; // Measurement period, default = 30min = 1800s
bool callbackHandled = false;

// MQTT
const char MQTT_SERVER[] =    "1.1.1.1";
const int MQTT_SERVERPORT =   1883;
const char MQTT_USERNAME[] =  "warms";
const char MQTT_PASSWORD[] =  "xxxxxxxxxx";
const char* MQTT_USERID;

// Feeds
const char MQTT_FEED_LOG[] =  "warms/log";
const char MQTT_FEED_TEMP[] = "warms/temp";
const char MQTT_FEED_HUM[] =  "warms/hum";
const char MQTT_FEED_LUM[] =  "warms/lum";
const char MQTT_FEED_MOI0[] = "warms/moi0";
const char MQTT_FEED_MOI1[] = "warms/moi1";
// Subscriptions
const char MQTT_SUB_PUMPS[] = "warms/pumps";
const char MQTT_SUB_SLEEP[] = "warms/sleepDuration";

// WIFI
const char WIFI_SSID[] = "xxxxxxxxxx";
const char WIFI_PASSWORD[] = "xxxxxxxxxx";
ESP8266WiFiMulti WiFiMulti;
WiFiClient wclient;
PubSubClient mqttclient(wclient);

// AD-Converter
Adafruit_ADS1015 ads;

// RTC
RtcDS3231<TwoWire> rtc(Wire);
#define countof(a) (sizeof(a) / sizeof(a[0]))

// DHT
const uint8_t DHTTYPE = DHT22;
DHT dht(DHTPIN, DHTTYPE);

struct PUMP_DURATIONS {
  unsigned int p0;
  unsigned int p1;
};

PUMP_DURATIONS calcPumpDurations(byte* payload);


/* SETUP ************************************************************************************************/
void setup() {
  Wire.begin(1, 3);
  rtc.Begin();
  rtc.Enable32kHzPin(false);
  rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmOne);
  ads.begin();
  dht.begin();

  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
  RtcDateTime now = rtc.GetDateTime();
  rtc.SetDateTime(compiled);

  if (!rtc.GetIsRunning()) {
    rtc.SetIsRunning(true);
  }

  // WIFI
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  while (WiFiMulti.run() != WL_CONNECTED) { // Wait until WIFI has been connected
    delay(500);
  }

  // MQTT
  String clientId = "ESP8266Client-";
  clientId += String(random(0xffff), HEX);
  MQTT_USERID = clientId.c_str();
  mqttclient.setServer(MQTT_SERVER, 1883);
  mqttclient.setCallback(callback);

  // PINMODES
  pinMode(PUMP0, OUTPUT);
  digitalWrite(PUMP0, HIGH);
  pinMode(PUMP1, OUTPUT);
  digitalWrite(PUMP1, HIGH);
}

/* LOOP ************************************************************************************************/
void loop() {
  if (!mqttclient.connected()) {
    if (mqttclient.connect(MQTT_USERID, MQTT_USERNAME, MQTT_PASSWORD)) {
      mqttclient.subscribe(MQTT_SUB_PUMPS);
      mqttclient.subscribe(MQTT_SUB_SLEEP);
    }
  }
  
  if (!callbackHandled) {
    if (mqttclient.connected()) {
      // Check RTC
      if (!rtc.IsDateTimeValid()) {
        log2mqtt(0, "RTC date/time is not valid (IsDateTimeValid() == false");
      }
      if (!rtc.GetIsRunning()) {
        log2mqtt(0, "RTC was not actively running, starting now");
        rtc.SetIsRunning(true);
      } 
  
      // Handle moisture 1 sensor
      int16_t adc0 = ads.readADC_SingleEnded(0);
      publishFloat(MQTT_FEED_LUM, (float)adc0);
  
      // Handle moisture 2 sensor
      int16_t adc1 = ads.readADC_SingleEnded(1);
      publishFloat(MQTT_FEED_MOI0, (float)adc1);
  
      // Handle light sensor
      int16_t adc2 = ads.readADC_SingleEnded(2);
      publishFloat(MQTT_FEED_MOI1, (float)adc2);
  
      // DHT
      float dhtTemp = dht.readTemperature();
      float dhtHum = dht.readHumidity();
      publishFloat(MQTT_FEED_HUM, dhtHum);
      publishFloat(MQTT_FEED_TEMP, dhtTemp);
  
      // Wait one minute in total for a callback
      int mqttLoops = 12;
      while(mqttLoops >= 0) {
        mqttclient.loop();
        delay(5000);
        --mqttLoops;
      }
    }
  }
  
  // Set alarm
  RtcDateTime now = rtc.GetDateTime();
  RtcDateTime alarmTime = now + sleepDuration;
  DS3231AlarmOne alarm1(alarmTime.Day(), alarmTime.Hour(), alarmTime.Minute(), alarmTime.Second(), DS3231AlarmOneControl_HoursMinutesSecondsMatch);
  char tmpString[150];
  sprintf(tmpString, "Alarm set: %u-%u-%u %u:%u:%u", alarmTime.Year(), alarmTime.Month(), alarmTime.Day(), alarmTime.Hour(), alarmTime.Minute(), alarmTime.Second());
  log2mqtt(2, tmpString);
  // Wait a couple of seconds to send the log entry
  delay(5000);
  rtc.SetAlarmOne(alarm1);
  // Next command will power off the board
  rtc.LatchAlarmsTriggeredFlags();
}

/*
  Publishes the given string to the log feed if the specified loglevel is lower or equal to the log level (LL).
  Max. string size = 80 characters.
  ------------------------------------------------------------------------------------------------------------
    l:    the log level
    m:    the string to send to the log feed

    returns: void
*/
void log2mqtt(short int l, char m[]) {
  if (l <= LL) {
    mqttclient.publish(MQTT_FEED_LOG, m);
  }
}

/* Publishes the float val to the given topic. Format: YYY.YY */
void publishFloat(char const topic[], float val) {
  char tmp[7];
  dtostrf(val, 5, 2, tmp);
  mqttclient.publish(topic, tmp);
}

/* Called when receiving a message from MQTT broker */
void callback(char* topic, byte* payload, unsigned int length) {
  // In order to republish this payload, a copy must be made
  // as the orignal payload buffer will be overwritten whilst
  // constructing the PUBLISH packet.
  byte* p = (byte*) malloc(length);
  memcpy(p, payload, length);

  char tmpString[150];
  sprintf(tmpString, "Received message for the topic '%s' with the payload '%s'", topic, p);
  log2mqtt(2, tmpString);
  
  if (strcmp(topic, MQTT_SUB_SLEEP) == 0) {
    sleepDuration = atoi((char*)p);
    sprintf(tmpString, "Sleep duration set to '%u'", sleepDuration);
    log2mqtt(1, tmpString);
  } else if (strcmp(topic, MQTT_SUB_PUMPS) == 0) {
    PUMP_DURATIONS pumpWateringDurations = calcPumpDurations(p);
    sprintf(tmpString,  "Watering: pump 0 for '%u' [ms] and pump 1 for '%u' [ms]", pumpWateringDurations.p0, pumpWateringDurations.p1);
    log2mqtt(1, tmpString);
    if (pumpWateringDurations.p0 > 0) {
      pump(PUMP0, pumpWateringDurations.p0);
    }
    if (pumpWateringDurations.p1 > 0) {
      pump(PUMP1, pumpWateringDurations.p1);
    }
  } else {
    sprintf(tmpString, "Unknown topic '%s'", topic);
    log2mqtt(1, tmpString);
  }
  free(p);
  callbackHandled = true;
}

/*
   Extracts from the payload the first 8 characters, splits it in two strings, converts them into 'unsignged int' and returns them as a pointer.
*/
PUMP_DURATIONS calcPumpDurations(byte* payload) {
  char mot0[5];
  char mot1[5];
  // Get first 4 characters and convert them
  strncpy(mot0, (char*)payload, 4);
  mot0[4] = '\0';
  // Get the next 4 characters and convert them
  strncpy(mot1, (char*)payload + 4, 4);
  mot1[4] = '\0';
  //unsigned int t[] = {atoi(mot0), atoi(mot1)};
  return (PUMP_DURATIONS) {
    atoi(mot0), atoi(mot1)
  };
}

/*
   Activates the given pump for the specified duration.
   ----------------------------------------------------
    pump: the pump to activate (0 or 1)

*/
void pump(unsigned int pump, unsigned long duration) {
  digitalWrite(pump, LOW);
  delay(duration);
  digitalWrite(pump, HIGH);
}
