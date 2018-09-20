#include <FS.h>              
#include <WiFiClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> 
#include <ESP8266mDNS.h>
#include <ESPConfig.h>
#include <time.h>

extern "C" {
  #include "user_interface.h"
}

/* Constants */
const char    STATE_OFF         = '0';
const char    STATE_ON          = '1';
const char*   CONFIG_FILE       = "/config.json";
const char*   SETTINGS_FILE     = "/settings.json";
const char*   DAYS_OF_WEEK[]    = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char*   MONTHS_OF_YEAR[]  = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DIC"};
const long    MILLIS_IN_MINUTE  = 60000;

struct Channel {
  const char*   id;
  char*         name;               // configurable over mqtt
  uint8_t       valvePin;
  char          state;
  unsigned long irrigationDuration; // millis configurable over mqtt
  unsigned long irrigationStopTime; // millis
  bool          enabled;            // configurable over mqtt

  Channel(const char* _id, const char* _name, uint8_t _vp, uint8_t _vs, uint8_t _irrDur) {
    id = _id;
    name = new char[CHANNEL_NAME_LENGTH + 1];
    updateName(_name);
    valvePin = _vp;
    state = _vs;
    irrigationDuration = _irrDur * MILLIS_IN_MINUTE;
    enabled = true;
  }

  void setDurationMinutes (uint8_t _irrDur) {
    irrigationDuration = _irrDur * MILLIS_IN_MINUTE;
  }
  
  int getDurationMinutes () {
    return irrigationDuration / MILLIS_IN_MINUTE;
  }

  void updateName (const char *v) {
    String s = String(v);
    s.toCharArray(name, CHANNEL_NAME_LENGTH);
  }
};

/* Module settings */
const String  MODULE_TYPE           = "irrigation";
char          _stationName[PARAM_LENGTH * 3 + 4];

WiFiClient                _wifiClient;
PubSubClient              _mqttClient(_wifiClient);
ESP8266WebServer          _httpServer(80);
ESP8266HTTPUpdateServer   _httpUpdater;

#ifdef WIFI_MIN_QUALITY
const uint8_t   _minimumQuality       = WIFI_MIN_QUALITY;
#else
const uint8_t   _minimumQuality       = -1;
#endif

#ifdef WIFI_CONN_TIMEOUT
const long      _connectionTimeout    = WIFI_CONN_TIMEOUT * 1000;
#else
const uint16_t  _connectionTimeout    = 0;
#endif

/* MQTT broker reconnection control */
unsigned long   _mqttNextConnAtte     = 0;

/* Irrigation control */
char*           _irrCronExpression[]  = {new char[4], new char[4], new char[4], new char[4], new char[4], new char[4]};
bool            _irrigating           = false;
uint8_t         _irrCurrChannel       = 0;
unsigned long   _irrLastScheduleCheck = -TIMER_CHECK_THRESHOLD * MILLIS_IN_MINUTE; // TIMER_CHECK_THRESHOLD is in minutes

/* Configuration control */
const uint8_t   PARAMS_COUNT          = 4;

/* Channels control */
const uint8_t CHANNELS_COUNT          = 4;

#ifdef NODEMCUV2
Channel _channels[] = {
  // Name, valve pin, state, irr time (minutes)
  {"A", "channel_A", D1, STATE_OFF, 1},
  {"B", "channel_B", D2, STATE_OFF, 1},
  {"C", "channel_C", D4, STATE_OFF, 1},
  {"D", "channel_D", D5, STATE_OFF, 1}
};
const uint8_t LED_PIN         = D7;

#elif ESP12
Channel _channels[] = {
  // Name, valve pin, state, irr time (minutes)
  {"A", "channel_A", 1, STATE_OFF, 1},
  {"B", "channel_B", 2, STATE_OFF, 1},
  {"C", "channel_C", 3, STATE_OFF, 1},
  {"D", "channel_D", 4, STATE_OFF, 1}
};

const uint8_t LED_PIN         = 5;
#endif

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

ESPConfig moduleConfig;

ESPConfigParam _moduleNameCfg (Text, "moduleName", "Module name", "", PARAM_LENGTH, "required");
ESPConfigParam _moduleLocationCfg (Text, "moduleLocation", "Module location", "", PARAM_LENGTH, "required");
ESPConfigParam _mqttPortCfg (Text, "mqttPort", "MQTT port", "", PARAM_LENGTH, "required");
ESPConfigParam _mqttHostCfg (Text, "mqttHost", "MQTT host", "", PARAM_LENGTH, "required");

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  Serial.println();
  log("Starting module");
  moduleConfig.addParameter(&_moduleLocationCfg);
  moduleConfig.addParameter(&_moduleNameCfg);
  moduleConfig.addParameter(&_mqttHostCfg);
  moduleConfig.addParameter(&_mqttPortCfg);
  moduleConfig.setTimeout(_connectionTimeout);
  moduleConfig.setPortalSSID("ESP-Irrigation");
  moduleConfig.setMinimumSignalQuality(_minimumQuality);
  moduleConfig.setSaveConfigCallback(saveConfig);
  moduleConfig.setStationNameCallback(getStationName);
  moduleConfig.connectWifiNetwork(loadConfig());
  moduleConfig.blockingFeedback(LED_PIN, 100, 8);

  // pins settings
  for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
    pinMode(_channels[i].valvePin, OUTPUT);
    digitalWrite(_channels[i].valvePin, HIGH);
  }
  loadChannelsSettings();
  log(F("Connected to wifi network. Local IP"), WiFi.localIP());
  configTime(TIMEZONE * 3600, 0, "br.pool.ntp.org", "cl.pool.ntp.org", "co.pool.ntp.org"); // brazil, chile, colombia
  log("Waiting for time...");
  while (!time(nullptr)) {
    delay(500);
  }
  // Default cron every day at 4:00 AM
  updateCronExpression("0","0","4","*","*","?");
  log(F("Configuring MQTT broker"));
  String port = String(_mqttPortCfg.getValue());
  log(F("Port"), port);
  log(F("Server"), _mqttHostCfg.getValue());
  _mqttClient.setServer(_mqttHostCfg.getValue(), (uint16_t) port.toInt());
  _mqttClient.setCallback(receiveMqttMessage);

  log(F("Setting OTA update"));
  MDNS.begin(getStationName());
  MDNS.addService("http", "tcp", 80);
  _httpUpdater.setup(&_httpServer);
  _httpServer.begin();
  log(F("HTTPUpdateServer ready.")); 
  log("Open http://" + String(getStationName()) + ".local/update");
  log("Open http://" + WiFi.localIP().toString() + "/update");
}

void loop() {
  _httpServer.handleClient();
  if (!_mqttClient.connected()) {
    connectBroker();
  }
  checkIrrigation();
  _mqttClient.loop();
  delay(LOOP_DELAY);
}

void checkIrrigation() {
  if (!_irrigating) {
    if (isTimeToCheckSchedule()) {
      if (isTimeToIrrigate()) {
        log(F("Irrigation sequence started"));
        _irrigating = true;
        _irrCurrChannel = 0;
        _mqttClient.publish(getStationTopic("state").c_str(), _irrigating ? "1" : "0");
      }
    }
  } else {
    if (_irrCurrChannel >= CHANNELS_COUNT) {
      log(F("No more channels to process. Stoping irrigation sequence."));
      _irrigating = false;
      _mqttClient.publish(getStationTopic("state").c_str(), _irrigating ? "1" : "0");
    } else {
      if (!isChannelEnabled(&_channels[_irrCurrChannel])) {
        log(F("Channel is disabled, going to next one."));
        ++_irrCurrChannel;
      } else {
        if (_channels[_irrCurrChannel].state == STATE_OFF) {
          log(F("Starting channel"), _channels[_irrCurrChannel].name);
          log(F("Channel irrigation duration (minutes)"), _channels[_irrCurrChannel].irrigationDuration / MILLIS_IN_MINUTE);
          openValve(&_channels[_irrCurrChannel]);
          _channels[_irrCurrChannel].irrigationStopTime = millis() + _channels[_irrCurrChannel].irrigationDuration;
        } else {
           if (millis() > _channels[_irrCurrChannel].irrigationStopTime) {
            log(F("Stoping channel"), _channels[_irrCurrChannel].name);
            closeValve(&_channels[_irrCurrChannel++]);
            delay(CLOSE_VALVE_DELAY);
          }
        }
      }
    }
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
  bool isTime = _irrLastScheduleCheck + TIMER_CHECK_THRESHOLD * MILLIS_IN_MINUTE < millis();
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
    digitalWrite(c->valvePin, LOW);
    c->state = STATE_ON;
  }
}

void closeValve(Channel* c) {
  log(F("Closing valve of channel"), c->name);
  if (c->state == STATE_OFF) {
    log(F("Valve already closed, skipping"));
  } else {
    digitalWrite(c->valvePin, HIGH);
    c->state = STATE_OFF;
  }
}

bool loadConfig () {
  size_t size = getFileSize(CONFIG_FILE);
  if (size > 0) {
    char buff[size];
    loadFile(CONFIG_FILE, buff, size);
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.parseObject(buff);
    if (json.success()) {
      #ifdef LOGGING
      json.printTo(Serial);
      Serial.println();
      #endif
      for (uint8_t i = 0; i < PARAMS_COUNT; ++i) {
        moduleConfig.getParameter(i)->updateValue(json[moduleConfig.getParameter(i)->getName()]);
        log(moduleConfig.getParameter(i)->getName(), moduleConfig.getParameter(i)->getValue());
      }
      return true;
    } else {
      log(F("Failed to load json config"));
    }
  }
  return false;
}

bool loadChannelsSettings () {
  size_t size = getFileSize(SETTINGS_FILE);
  if (size > 0) {
    char buff[size];
    loadFile(SETTINGS_FILE, buff, size);
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.parseObject(buff);
    #ifdef LOGGING
    json.printTo(Serial);
    Serial.println();
    #endif
    if (json.success()) {
      for (uint8_t i = 0; i < CHANNELS_COUNT; ++i) {
        _channels[i].updateName(json[String(_channels[i].id) + "_name"]);
        _channels[i].irrigationDuration = json[String(_channels[i].id) + "_irrdur"];
        _channels[i].enabled = json[String(_channels[i].id) + "_enabled"];
        #ifdef LOGGING
        log(F("Channel id"), _channels[i].id);
        log(F("Channel name"), _channels[i].name);
        log(F("Channel enabled"), _channels[i].enabled);
        #endif
      }
      return true;
    } else {
      log(F("Failed to load json"));
    }
  }
  return false;
}

/*
  Returns the size of a file. 
  If 
    > the file does not exist
    > the FS cannot be mounted
    > the file cannot be opened for writing
    > the file is empty
  the value returned is 0.
  Otherwise the size of the file is returned.
*/
size_t getFileSize (const char* fileName) {
  if (SPIFFS.begin()) {
    if (SPIFFS.exists(fileName)) {
      File file = SPIFFS.open(fileName, "r");
      if (file) {
        size_t s = file.size();
        file.close();
        return s;
      } else {
        file.close();
        log(F("Cant open file"), fileName);
      }
    } else {
      log(F("File not found"), fileName);
    }
  } else {
    log(F("Failed to mount FS"));
  }
  return 0;
}

void loadFile (const char* fileName, char buff[], size_t size) {
  File file = SPIFFS.open(fileName, "r");
  file.readBytes(buff, size);
  file.close();
}

/** callback notifying the need to save config */
void saveConfig () {
  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (file) {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    //TODO Trim param values
    for (uint8_t i = 0; i < PARAMS_COUNT; ++i) {
      json[moduleConfig.getParameter(i)->getName()] = moduleConfig.getParameter(i)->getValue();
    }
    json.printTo(file);
    log(F("Configuration file saved"));
    json.printTo(Serial);
    Serial.println();
    file.close();
  } else {
    log(F("Failed to open config file for writing"));
  }
}

void receiveMqttMessage(char* topic, unsigned char* payload, unsigned int length) {
  log(F("Received mqtt message topic"), topic);
  // Station topics
  if (String(topic).equals(getStationTopic("hrst"))) {
    hardReset();
  } else if (String(topic).equals(getStationTopic("cron"))) {
    updateCron(payload, length);
  } else if (String(topic).equals(getStationTopic("control"))) {
    changeState(payload, length);
    _mqttClient.publish(getStationTopic("state").c_str(), _irrigating ? "1" : "0");
  } else {
    // Channels topics
    for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
      if (getChannelTopic(&_channels[i], "enable").equals(topic)) {
        if (enableChannel(&_channels[i], payload, length)) {
          saveChannelsSettings();
        }
        _mqttClient.publish(getChannelTopic(&_channels[i], "state").c_str(), _channels[i].enabled ? "1" : "0");
      } else if (getChannelTopic(&_channels[i], "irrdur").equals(topic)) {
        if (updateChannelIrrigationDuration(&_channels[i], payload, length)) {
          saveChannelsSettings();
        }
      } else if (getChannelTopic(&_channels[i], "rename").equals(topic)) {
        if (renameChannel(&_channels[i], payload, length)) {
          saveChannelsSettings();
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

bool renameChannel(Channel* c, unsigned char* payload, unsigned int length) {
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
    _mqttClient.unsubscribe(getChannelTopic(c, "+").c_str());
    c->updateName(newName);
    _mqttClient.subscribe(getChannelTopic(c, "+").c_str());
  }
  return renamed;
}

bool updateChannelIrrigationDuration(Channel* c, unsigned char* payload, unsigned int length) {
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
  int newDur = String(buff).toInt();
  log(F("Duration"), newDur);
  bool durChanged = c->getDurationMinutes() != newDur;
  c->setDurationMinutes(newDur);
  return durChanged;
}

void saveChannelsSettings () {
  File file = SPIFFS.open(SETTINGS_FILE, "w");
  if (file) {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    //TODO Trim param values
    for (uint8_t i = 0; i < CHANNELS_COUNT; ++i) {
      json[String(_channels[i].id) + "_name"] = _channels[i].name;
      json[String(_channels[i].id) + "_irrdur"] = _channels[i].irrigationDuration;
      json[String(_channels[i].id) + "_enabled"] = _channels[i].enabled;
    }
    json.printTo(file);
    log(F("Configuration file saved"));
    json.printTo(Serial);
    Serial.println();
    file.close();
  } else {
    log(F("Failed to open config file for writing"));
  }
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
          _channels[_irrCurrChannel].irrigationStopTime = 0;
          closeValve(&_channels[_irrCurrChannel]);
          _irrigating = false;
          log(F("Irrigation stopped"));
        }
        return true;
      case STATE_ON:
        if (!_irrigating) {
          _irrigating = true;
          _irrCurrChannel = 0;
          log(F("Irrigation started"));
        }
        return true;
      default:
        log(F("Invalid state"), payload[0]);
    }
  }
  return false;
}

bool isChannelEnabled (Channel *c) {
  return c->enabled && c->name != NULL && strlen(c->name) > 0;
}

void connectBroker() {
  if (_mqttNextConnAtte <= millis()) {
    _mqttNextConnAtte = millis() + MQTT_BROKER_CONNECTION_RETRY;
    log(F("Connecting MQTT broker as"), getStationName());
    if (_mqttClient.connect(getStationName())) {
      log(F("MQTT broker Connected"));
      _mqttClient.subscribe(getStationTopic("+").c_str());
      for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
        _mqttClient.subscribe(getChannelTopic(&_channels[i], "+").c_str());
      }
    } else {
      log(F("Failed. RC:"), _mqttClient.state());
    }
  }
}

char* getStationName () {
  if (strlen(_stationName) <= 0) {
    size_t size = MODULE_TYPE.length() + _moduleLocationCfg.getValueLength() + _moduleNameCfg.getValueLength() + 4;
    String sn;
    sn.concat(MODULE_TYPE);
    sn.concat("_");
    sn.concat(_moduleLocationCfg.getValue()); 
    sn.concat("_");
    sn.concat(_moduleNameCfg.getValue());
    sn.toCharArray(_stationName, size);
  } 
  return _stationName;
}

String getChannelTopic (Channel *c, String cmd) {
  return MODULE_TYPE + F("/") + _moduleLocationCfg.getValue() + F("/") + _moduleNameCfg.getValue() + F("/") + c->name + F("/") + cmd;
}

String getStationTopic (String cmd) {
  return MODULE_TYPE + F("/") + _moduleLocationCfg.getValue() + F("/") + _moduleNameCfg.getValue() + F("/") + cmd;
}