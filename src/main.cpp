#include <ESP8266WiFi.h>
#include <ESPDomotic.h>
#include <time.h>

void checkIrrigation();
void mqttConnectionCallback();
void updateCronExpression (const char* sec, const char* min, const char* hour, const char* dom, const char* mon, const char* dow);
void updateCronExpressionChunk(const char* a, uint8_t index);
bool isTimeToCheckSchedule ();
bool isTimeToIrrigate ();
void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length);
void updateCron(unsigned char* payload, unsigned int length);
bool changeState(unsigned char* payload, unsigned int length);

/* Constants */
const char*         DAYS_OF_WEEK[]    = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char*         MONTHS_OF_YEAR[]  = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DIC"};

/* Irrigation control */
char*           _irrCronExpression[]  = {new char[4], new char[4], new char[4], new char[4], new char[4], new char[4]};
bool            _irrigating           = false;
uint8_t         _currChannel          = 0;
unsigned long   _irrLastScheduleCheck = -TIMER_CHECK_THRESHOLD * 1000; // TIMER_CHECK_THRESHOLD is in seconds

/* Channels control */
#ifdef NODEMCUV2
  // Id, Name, pin, state, timer 
Channel _channelA ("A", "channel_A", D1, OUTPUT, HIGH, 60 * 1000);
Channel _channelB ("B", "channel_B", D2, OUTPUT, HIGH, 60 * 1000);
Channel _channelC ("C", "channel_C", D4, OUTPUT, HIGH, 60 * 1000);
Channel _channelD ("D", "channel_D", D5, OUTPUT, HIGH, 60 * 1000);

const uint8_t LED_PIN         = D7;
// TODO Define pin consts un configuration file (ini file)
#elif ESP12
  Channel _channelA ("A", "channel_A", 2, OUTPUT, HIGH, 60 * 1000);
  Channel _channelB ("B", "channel_B", 4, OUTPUT, HIGH, 60 * 1000);
  Channel _channelC ("C", "channel_C", 5, OUTPUT, HIGH, 60 * 1000);
  Channel _channelD ("D", "channel_D", 16, OUTPUT, HIGH, 60 * 1000);

const uint8_t LED_PIN         = 13;
#endif

ESPDomotic _domoticModule;

template <class T> void log (T text) {
  #ifdef LOGGING
  Serial.print("*IRR: ");
  Serial.println(text);
  #endif
}

template <class T, class U> void log (T key, U value) {
  #ifdef LOGGING
  Serial.print("*IRR: ");
  Serial.print(key);
  Serial.print(": ");
  Serial.println(value);
  #endif
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  log("Starting module");
  String ssid = "Proeza irrigation " + String(ESP.getChipId());
  _domoticModule.setPortalSSID(ssid.c_str());
  _domoticModule.setFeedbackPin(LED_PIN);
  _domoticModule.setMqttConnectionCallback(mqttConnectionCallback);
  _domoticModule.setMqttMessageCallback(receiveMqttMessage);
  _domoticModule.setModuleType("irrigation");
  _domoticModule.addChannel(&_channelA);
  _domoticModule.addChannel(&_channelB);
  _domoticModule.addChannel(&_channelC);
  _domoticModule.addChannel(&_channelD);
  _domoticModule.init();
  log(F("Connected to wifi network. Local IP"), WiFi.localIP());
  configTime(TIMEZONE * 3600, 0, "br.pool.ntp.org", "cl.pool.ntp.org", "co.pool.ntp.org"); // brazil, chile, colombia
  log("Waiting for time...");
  while (!time(nullptr)) {
    delay(500);
  }
  // Default cron every day at 4:00 AM
  updateCronExpression("0","0","4","*","*","?");
}

void loop() {
  _domoticModule.loop();
  checkIrrigation();
  delay(LOOP_DELAY);
}

void checkIrrigation() {
  if (!_irrigating) {
    if (isTimeToCheckSchedule()) {
      if (isTimeToIrrigate()) {
        log(F("Irrigation sequence started"));
        _irrigating = true;
        _currChannel = 0;
        _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), "1");
      }
    }
  } else {
    if (_currChannel >= _domoticModule.getChannelsCount()) {
      log(F("No more channels to process. Stoping irrigation sequence."));
      _irrigating = false;
      _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), "0");
    } else {
      Channel *channel = _domoticModule.getChannel(_currChannel);
      if (!channel->isEnabled()) {
        log(F("Channel is disabled, going to next one."), channel->name);
        ++_currChannel;
      } else {
        if (channel->state == HIGH) {
          log(F("Starting channel"), channel->name);
          log(F("Channel timer (seconds)"), channel->timer / 1000);
          _domoticModule.openChannel(channel);
          channel->updateTimerControl();
        } else { //LOW
           if (millis() > channel->timerControl) {
            log(F("Stoping channel"), channel->name);
            _domoticModule.closeChannel(_domoticModule.getChannel(_currChannel++));
            delay(CLOSE_VALVE_DELAY);
          }
        }
      }
    }
  }
}

void mqttConnectionCallback() {
  // no additional action needed on mqtt client
}

void updateCronExpression (const char* sec, const char* min, const char* hour, const char* dom, const char* mon, const char* dow) {
  updateCronExpressionChunk(sec, 0);
  updateCronExpressionChunk(min, 1);
  updateCronExpressionChunk(hour, 2);
  updateCronExpressionChunk(dom, 3);
  updateCronExpressionChunk(mon, 4);
  updateCronExpressionChunk(dow, 5);
}

void updateCronExpressionChunk(const char* a, uint8_t index) {
  String(a).toCharArray(_irrCronExpression[index], 4);
}

bool isTimeToCheckSchedule () {
  bool isTime = _irrLastScheduleCheck + TIMER_CHECK_THRESHOLD * 1000 < millis();
  _irrLastScheduleCheck = isTime ? millis() : _irrLastScheduleCheck;
  return isTime;
}

bool isTimeToIrrigate () {
  log(F("Checking if is time to start irrigation"));
  time_t now = time(nullptr);
  Serial.printf("Current time: %s", ctime(&now));
  struct tm * ptm = gmtime(&now);
  String dow = ptm->tm_wday < 7 ? String(DAYS_OF_WEEK[ptm->tm_wday]): "";
  String mon = ptm->tm_mon < 12 ? String(MONTHS_OF_YEAR[ptm->tm_mon]): "";
  // Evaluates cron at minute level. Seconds granularity is not needed for irrigarion scheduling.
  bool timeToIrrigate = true;
  timeToIrrigate &= String(_irrCronExpression[5]).equals("?") || String(_irrCronExpression[5]).equalsIgnoreCase(dow) || String(_irrCronExpression[5]).equals(String(ptm->tm_wday + 1)); // Day of week
  timeToIrrigate &= String(_irrCronExpression[4]).equals("*") || String(_irrCronExpression[4]).equalsIgnoreCase(mon) || String(_irrCronExpression[4]).equals(String(ptm->tm_mon + 1)); // Month
  timeToIrrigate &= String(_irrCronExpression[3]).equals("*") || String(_irrCronExpression[3]).equals(String(ptm->tm_mday)); // Day of month
  timeToIrrigate &= String(_irrCronExpression[2]).equals("*") || String(_irrCronExpression[2]).equals(String(ptm->tm_hour)); // Hour
  timeToIrrigate &= String(_irrCronExpression[1]).equals("*") || String(_irrCronExpression[1]).toInt() == ptm->tm_min || (ptm->tm_min > String(_irrCronExpression[1]).toInt() && ptm->tm_min - (TIMER_CHECK_THRESHOLD / 60) <= String(_irrCronExpression[1]).toInt()); // Minute
  return timeToIrrigate;
}

void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  log(F("MQTT message on topic"), topic);
  // Station topics
  if (String(topic).equals(_domoticModule.getStationTopic("command/cron"))) {
    updateCron(payload, length);
  } else if (String(topic).equals(_domoticModule.getStationTopic("command/state"))) {
    changeState(payload, length);
    _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), _irrigating ? "1" : "0");
  }
}

void updateCron(unsigned char* payload, unsigned int length) {
  log(F("Updating cron"));
  if (length < 11) {
    log(F("Invalid payload"));
  } else {
    unsigned int i = 0;
    size_t k = 0;
    while (i < length && k < 6) { // cron consist of 6 chunks
      if (payload[i] == ' ') {
        ++i;
      } else {
        char aux[] = {'\0','\0','\0','\0'}; // cron chunks cant have more than 3 chars
        int j = 0;
        while (i < length && payload[i] != ' ' && j < 3) {
          aux[j++] = payload[i++];
        }
        updateCronExpressionChunk(aux, k++);
      }
    }
  }
  #ifdef LOGGING
  Serial.print("New cron expression: ");
  for (int i = 0; i < 6; ++i) {
    Serial.print(_irrCronExpression[i]);
    Serial.print(" ");
  }
  Serial.println();
  #endif
}

bool changeState(unsigned char* payload, unsigned int length) {
  if (length != 1) {
    log(F("Invalid payload"));
  } else {
    switch (payload[0]) {
      case '0':
        if (_irrigating) {
          // Set irrigation end to 0 to simulate it should have ended 
          Channel *channel = _domoticModule.getChannel(_currChannel);
          channel->timerControl = 0;
          _domoticModule.closeChannel(channel);
          _irrigating = false;
          log(F("Irrigation stopped"));
        }
        return true;
      case '1':
        if (!_irrigating) {
          _irrigating = true;
          _currChannel = 0;
          log(F("Irrigation started"));
        }
        return true;
      default:
        log(F("Invalid state"), payload[0]);
    }
  }
  return false;
}