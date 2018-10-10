#include <FS.h>
#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <ESPDomotic.h>
#include <time.h>

/* Constants */
const char          STATE_OFF         = '0';
const char          STATE_ON          = '1';
const char*         SETTINGS_FILE     = "/settings.json";
const char*         DAYS_OF_WEEK[]    = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char*         MONTHS_OF_YEAR[]  = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DIC"};

/* Module settings */
const char*     _moduleType           = "irrigation";

/* Irrigation control */
char*           _irrCronExpression[]  = {new char[4], new char[4], new char[4], new char[4], new char[4], new char[4]};
bool            _irrigating           = false;
uint8_t         _currChannel          = 0;
unsigned long   _irrLastScheduleCheck = -TIMER_CHECK_THRESHOLD * 1000; // TIMER_CHECK_THRESHOLD is in seconds

/* Channels control */
#ifdef NODEMCUV2
  // Id, Name, pin, state, timer 
Channel _channelA ("A", "channel_A", D1, STATE_OFF, 60 * 1000, OUTPUT);
Channel _channelB ("B", "channel_B", D2, STATE_OFF, 60 * 1000, OUTPUT);
Channel _channelC ("C", "channel_C", D4, STATE_OFF, 60 * 1000, OUTPUT);
Channel _channelD ("D", "channel_D", D5, STATE_OFF, 60 * 1000, OUTPUT);

const uint8_t LED_PIN         = D7;
// TODO Define pin consts un configuration file (ini file)
#elif ESP12
  Channel _channelA ("A", "channel_A", 1, STATE_OFF, 60 * 1000, OUTPUT);
  Channel _channelB ("B", "channel_B", 2, STATE_OFF, 60 * 1000, OUTPUT);
  Channel _channelC ("C", "channel_C", 3, STATE_OFF, 60 * 1000, OUTPUT);
  Channel _channelD ("D", "channel_D", 4, STATE_OFF, 60 * 1000, OUTPUT);

const uint8_t LED_PIN         = 5;
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
  pinMode(LED_PIN, OUTPUT);
  Serial.println();
  log("Starting module");
  String ssid = "Proeza irrigation " + String(ESP.getChipId());
  _domoticModule.setPortalSSID(ssid.c_str());
  _domoticModule.setFeedbackPin(LED_PIN);
  _domoticModule.setMqttConnectionCallback(mqttConnectionCallback);
  _domoticModule.setMqttMessageCallback(receiveMqttMessage);
  _domoticModule.setModuleType(_moduleType);
  _domoticModule.setDebugOutput(LOGGING);
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
        _domoticModule.getMqttClient()->publish(getStationTopic("state").c_str(), "1");
      }
    }
  } else {
    if (_currChannel >= _domoticModule.getChannelsCount()) {
      log(F("No more channels to process. Stoping irrigation sequence."));
      _irrigating = false;
      _domoticModule.getMqttClient()->publish(getStationTopic("state").c_str(), "0");
    } else {
      if (!_domoticModule.getChannel(_currChannel)->isEnabled()) {
        log(F("Channel is disabled, going to next one."));
        ++_currChannel;
      } else {
        if (_domoticModule.getChannel(_currChannel)->state == STATE_OFF) {
          log(F("Starting channel"), _domoticModule.getChannel(_currChannel)->name);
          log(F("Channel timer (seconds)"), _domoticModule.getChannel(_currChannel)->timer / 1000);
          openValve(_domoticModule.getChannel(_currChannel));
          _domoticModule.getChannel(_currChannel)->updateTimerControl();
        } else {
           if (millis() > _domoticModule.getChannel(_currChannel)->timerControl) {
            log(F("Stoping channel"), _domoticModule.getChannel(_currChannel)->name);
            closeValve(_domoticModule.getChannel(_currChannel++));
            delay(CLOSE_VALVE_DELAY);
          }
        }
      }
    }
  }
}

void mqttConnectionCallback() {
  _domoticModule.getMqttClient()->subscribe(getStationTopic("+").c_str());
  for (size_t i = 0; i < _domoticModule.getChannelsCount(); ++i) {
    _domoticModule.getMqttClient()->subscribe(getChannelTopic(_domoticModule.getChannel(i), "+").c_str());
  }
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
  struct tm * ptm;
  ptm = gmtime(&now);
  String dow = ptm->tm_wday < 7 ? String(DAYS_OF_WEEK[ptm->tm_wday]): "";
  String mon = ptm->tm_mon < 12 ? String(MONTHS_OF_YEAR[ptm->tm_mon]): "";
  // Evaluates cron at minute level. Seconds granularity is not needed for irrigarion scheduling.
  bool timeToIrrigate = true;
  timeToIrrigate &= String(_irrCronExpression[5]).equals("?") || String(_irrCronExpression[5]).equalsIgnoreCase(dow) || String(_irrCronExpression[5]).equals(String(ptm->tm_wday + 1)); // Day of week
  timeToIrrigate &= String(_irrCronExpression[4]).equals("*") || String(_irrCronExpression[4]).equalsIgnoreCase(mon) || String(_irrCronExpression[4]).equals(String(ptm->tm_mon + 1)); // Month
  timeToIrrigate &= String(_irrCronExpression[3]).equals("*") || String(_irrCronExpression[3]).equals(String(ptm->tm_mday)); // Day of month
  timeToIrrigate &= String(_irrCronExpression[2]).equals("*") || String(_irrCronExpression[2]).equals(String(ptm->tm_hour)); // Hour
  timeToIrrigate &= String(_irrCronExpression[1]).equals("*") || String(_irrCronExpression[1]).toInt() == ptm->tm_min || (String(_irrCronExpression[1]).toInt() > ptm->tm_min && String(_irrCronExpression[1]).toInt() - TIMER_CHECK_THRESHOLD < ptm->tm_min); // Minute
  return timeToIrrigate;
}

void openValve(Channel* c) {
  log(F("Opening valve of channel"), c->name);
  if (c->state == STATE_ON) {
    log(F("Valve already opened, skipping"));
  } else {
    digitalWrite(c->pin, LOW);
    c->state = STATE_ON;
  }
}

void closeValve(Channel* c) {
  log(F("Closing valve of channel"), c->name);
  if (c->state == STATE_OFF) {
    log(F("Valve already closed, skipping"));
  } else {
    digitalWrite(c->pin, HIGH);
    c->state = STATE_OFF;
  }
}

void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  log(F("Received mqtt message topic"), topic);
  // Station topics
  if (String(topic).equals(getStationTopic("hrst"))) {
    hardReset();
  } else if (String(topic).equals(getStationTopic("cron"))) {
    updateCron(payload, length);
  } else if (String(topic).equals(getStationTopic("control"))) {
    changeState(payload, length);
    _domoticModule.getMqttClient()->publish(getStationTopic("state").c_str(), _irrigating ? "1" : "0");
  } else {
    // Channels topics
    for (size_t i = 0; i < _domoticModule.getChannelsCount(); ++i) {
      if (getChannelTopic(_domoticModule.getChannel(i), "enable").equals(topic)) {
        if (enableChannel(_domoticModule.getChannel(i), payload, length)) {
          _domoticModule.saveChannelsSettings();
        }
        _domoticModule.getMqttClient()->publish(getChannelTopic(_domoticModule.getChannel(i), "state").c_str(), _domoticModule.getChannel(i)->enabled ? "1" : "0");
      } else if (getChannelTopic(_domoticModule.getChannel(i), "timer").equals(topic)) {
        if (updateChannelTimer(_domoticModule.getChannel(i), payload, length)) {
          _domoticModule.saveChannelsSettings();
        }
      } else if (getChannelTopic(_domoticModule.getChannel(i), "rename").equals(topic)) {
        if (renameChannel(_domoticModule.getChannel(i), payload, length)) {
          _domoticModule.saveChannelsSettings();
        }
      }
    }
  }
}

bool enableChannel(Channel* c, unsigned char* payload, unsigned int length) {
  log(F("Changing channel state"), c->name);
  if (length != 1 || !payload) {
    log(F("Invalid payload. Ignoring."));
    return false;
  }
  bool stateChanged = false;
  switch (payload[0]) {
    case STATE_OFF:
      stateChanged = c->enabled;
      c->enabled = false;
      break;
    case STATE_ON:
      stateChanged = !c->enabled;
      c->enabled = true;
      break;
    default:
      log(F("Invalid state"), payload[0]);
      break;
  }
  return stateChanged;
}

bool renameChannel(Channel* c, uint8_t* payload, unsigned int length) {
  log(F("Updating channel name"), c->name);
  if (length < 1) {
    log(F("Invalid payload"));
    return false;
  }
  char newName[length + 1];
  for (uint16_t i = 0 ; i < length; ++ i) {
    newName[i] = payload[i];
  }
  newName[length] = '\0';
  bool renamed = !String(c->name).equals(String(newName));
  if (renamed) {
    log(F("Channel renamed"), newName);
    _domoticModule.getMqttClient()->unsubscribe(getChannelTopic(c, "+").c_str());
    c->updateName(newName);
    _domoticModule.getMqttClient()->subscribe(getChannelTopic(c, "+").c_str());
  }
  return renamed;
}

bool updateChannelTimer(Channel* c, uint8_t* payload, unsigned int length) {
  log(F("Updating irrigation duration for channel"), c->name);
  if (length < 1) {
    log(F("Invalid payload"));
    return false;
  }
  char buff[length + 1];
  for (uint16_t i = 0 ; i < length; ++ i) {
    buff[i] = payload[i];
  }
  buff[length] = '\0';
  log(F("New duration for channel"), c->name);
  long newTimer = String(buff).toInt();
  log(F("Duration"), newTimer);
  bool timerChanged = c->timer != (unsigned long) newTimer * 60 * 1000;
  c->timer = newTimer * 60 * 1000; // received in seconds set in millis
  return timerChanged;
}

void hardReset () {
  log(F("Doing a module hard reset"));
  SPIFFS.format();
  WiFi.disconnect();
  delay(200);
  ESP.restart();
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
      case STATE_OFF:
        if (_irrigating) {
          // Set irrigation end to 0 to simulate it should have ended 
          _domoticModule.getChannel(_currChannel)->timerControl = 0;
          closeValve(_domoticModule.getChannel(_currChannel));
          _irrigating = false;
          log(F("Irrigation stopped"));
        }
        return true;
      case STATE_ON:
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

String getChannelTopic (Channel *c, String cmd) {
  return String(_moduleType) + F("/") + _domoticModule.getModuleLocation() + F("/") + _domoticModule.getModuleName() + F("/") + c->name + F("/") + cmd;
}

String getStationTopic (String cmd) {
  return String(_moduleType) + F("/") + _domoticModule.getModuleLocation() + F("/") + _domoticModule.getModuleName() + F("/") + cmd;
}