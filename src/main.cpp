#include <ESP8266WiFi.h>
#include <ESPDomotic.h>
#include <time.h>

void setup();
void loop();
void initializeScheduling();
void updateCron(uint8_t cronNo, unsigned char* payload, unsigned int length);
void loadCronConf(uint8_t cronNo);
void setCronExpressionChunk(uint8_t cronNo, uint8_t index, const char* value);
void setCron(uint8_t cronNo, char* payload);
void checkIrrigation();
bool isTimeToCheckSchedule ();
bool isTimeToIrrigate ();
void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length);
void mqttConnectionCallback();
bool changeStateCommand(unsigned char* payload, unsigned int length);
char* mqttPayloadToString (uint8_t* payload, unsigned int length);

/* Constants */
const char*         DAYS_OF_WEEK[]    = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char*         MONTHS_OF_YEAR[]  = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DIC"};

const char*         CRONS_CONF[] = {"CRON_0.conf", "CRON_1.conf"};

typedef char* tCron[6];

/* Irrigation control */
tCron           _irrCronExpression[2] = {
  {new char[4], new char[4], new char[4], new char[4], new char[4], new char[4]},
  {new char[4], new char[4], new char[4], new char[4], new char[4], new char[4]}
};

bool            _irrigating           = false;
uint8_t         _currChannel          = 0;
unsigned long   _irrLastScheduleCheck = -TIMER_CHECK_THRESHOLD * 1000; // TIMER_CHECK_THRESHOLD is in seconds

/* Channels control */
#ifdef NODEMCUV2
  // Id, Name, pin, state, timer 
Channel _channelA ("A", "channel_A", D1, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelB ("B", "channel_B", D2, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelC ("C", "channel_C", D4, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelD ("D", "channel_D", D5, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);

const uint8_t LED_PIN         = D7;
// TODO Define pin consts un configuration file (ini file)
#elif ESP12
  Channel _channelA ("A", "channel_A", 2, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelB ("B", "channel_B", 4, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelC ("C", "channel_C", 5, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelD ("D", "channel_D", 16, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);

const uint8_t LED_PIN         = 13;
#endif

ESPDomotic _domoticModule;

template <class T> void log (T text) {
  Serial.print("*IRR: ");
  Serial.println(text);
}

template <class T, class U> void log (T key, U value) {
  Serial.print("*IRR: ");
  Serial.print(key);
  Serial.print(": ");
  Serial.println(value);
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
  _domoticModule.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
  _domoticModule.setWifiConnectTimeout(WIFI_CONNECT_TIMEOUT);
  _domoticModule.setConfigFileSize(CONFIG_FILE_SIZE);
  _domoticModule.setModuleType("irrigation");
  _domoticModule.addChannel(&_channelA);
  _domoticModule.addChannel(&_channelB);
  _domoticModule.addChannel(&_channelC);
  _domoticModule.addChannel(&_channelD);
  _domoticModule.init();
  log(F("Connected to wifi network. Local IP"), WiFi.localIP());
  configTime(TIMEZONE * 3600, 0, "ar.pool.ntp.org", "br.pool.ntp.org", "cl.pool.ntp.org"); // argentina, brazil, chile
  log("Waiting for time...");
  time_t now;
  while (!(now = time(nullptr))) {
    delay(500);
  }
  Serial.printf("Current time: %s", ctime(&now));
  initializeScheduling();
}

void loop() {
  _domoticModule.loop();
  checkIrrigation();
  delay(LOOP_DELAY);
}

void initializeScheduling () {
  log("Initializing scheduling");
  for (uint8_t cronNo = 0; cronNo < MAX_CRONS; ++cronNo) {
    loadCronConf(cronNo);
  }
}

void loadCronConf(uint8_t cronNo) {
  log("Getting configuration for cron number", cronNo);
  char* conf = _domoticModule.getConf(CRONS_CONF[cronNo]);
  if (conf) {
    log("Cron conf loaded", conf);
    setCron(cronNo, conf);
    delete[] conf;
  } else { 
    log("Conf not found for cron number", cronNo);
    setCronExpressionChunk(cronNo, 0, "0");
    setCronExpressionChunk(cronNo, 1, "0");
    setCronExpressionChunk(cronNo, 2, "4");
    setCronExpressionChunk(cronNo, 3, "*");
    setCronExpressionChunk(cronNo, 4, "*");
    setCronExpressionChunk(cronNo, 5, "?");
  }
}

void setCron(uint8_t cronNo, char* payload) {
  unsigned int i = 0; 
  size_t k = 0;
  size_t length = strlen(payload);
  while (i < length && k < 6) { // cron consist of 6 chunks
    if (payload[i] == ' ') {
      ++i;
    } else {
      char aux[] = {'\0','\0','\0','\0'}; // cron chunks cant have more than 3 chars
      int j = 0;
      while (i < length && payload[i] != ' ' && j < 3) {
        aux[j++] = payload[i++];
      }
      setCronExpressionChunk(cronNo, k++, aux);
    }
  }
}

void setCronExpressionChunk(uint8_t cronNo, uint8_t index, const char* value) {
  String sChunk = String(value);
  log("Updating cron chunk", String(sChunk));
  sChunk.toCharArray(_irrCronExpression[cronNo][index], 4);
}

void updateCron(uint8_t cronNo, unsigned char* payload, unsigned int length) {
  log(F("Updating cron"));
  if (length < 11) {
    log(F("Invalid payload"));
  } else {
    char* cronConf = mqttPayloadToString(payload, length);
    setCron(cronNo, cronConf);
    if (_domoticModule.updateConf(CRONS_CONF[cronNo], cronConf)) {
      log("Cron conf persisted OK");
    } else {
      log("Cron conf not saved");
    }
    delete[] cronConf;
    #ifndef LOGGING
    Serial.print("New cron expression: ");
    for (int i = 0; i < 6; ++i) {
      Serial.print(_irrCronExpression[cronNo][i]);
      Serial.print(" ");
    }
    Serial.println();
    #endif
  }
}

char* mqttPayloadToString (uint8_t* payload, unsigned int length) {
  char* aux = new char[length + 1];
  log("Payload debug. Length", length);
  for (unsigned int i = 0; i < length; ++i) {
    aux[i] = payload[i];
    Serial.print(payload[i]);
  }
  Serial.println();
  aux[length] = '\0';
  log("Payload converted", aux);
  return aux;
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
          log(F("Openning channel"), channel->name);
          _domoticModule.updateChannelState(channel, LOW);
        } else {
           if (channel->timeIsUp()) {
            log(F("Closing channel"), channel->name);
            _domoticModule.updateChannelState(channel, HIGH);
            _currChannel++;
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

bool isTimeToCheckSchedule () {
  bool isTime = _irrLastScheduleCheck + TIMER_CHECK_THRESHOLD * 1000 < millis();
  _irrLastScheduleCheck = isTime ? millis() : _irrLastScheduleCheck;
  return isTime;
}

bool isTimeToIrrigate () {
  log(F("Checking if is time to start irrigation"));
  time_t now = time(nullptr);
  Serial.printf("Current time: %s", ctime(&now));
  struct tm * ptm = localtime(&now);
  String dow = ptm->tm_wday < 7 ? String(DAYS_OF_WEEK[ptm->tm_wday]): "";
  String mon = ptm->tm_mon < 12 ? String(MONTHS_OF_YEAR[ptm->tm_mon]): "";
  // Evaluates cron at minute level. Seconds granularity is not needed for irrigarion scheduling.
  boolean timeToIrrigate = true;
  uint8_t cronNo = 0;
  do {
    log("Checking cron", cronNo);

    timeToIrrigate = true;
    timeToIrrigate &= String(_irrCronExpression[cronNo][1]).equals("*") || String(_irrCronExpression[cronNo][1]).toInt() == ptm->tm_min || (ptm->tm_min > String(_irrCronExpression[cronNo][1]).toInt() && ptm->tm_min - (TIMER_CHECK_THRESHOLD / 60) <= String(_irrCronExpression[cronNo][1]).toInt()); // Minute
    log("chunk min", String(_irrCronExpression[cronNo][1]));
    log("time min", String(ptm->tm_min));
    log("coincide min", String(_irrCronExpression[cronNo][1]).equals(String(ptm->tm_min)));
    timeToIrrigate &= String(_irrCronExpression[cronNo][2]).equals("*") || String(_irrCronExpression[cronNo][2]).equals(String(ptm->tm_hour)); // Hour
    log("chunk hour", String(_irrCronExpression[cronNo][2]));
    log("time hour", String(ptm->tm_hour));
    log("coincide hour", String(_irrCronExpression[cronNo][2]).equals(String(ptm->tm_hour)));
    timeToIrrigate &= String(_irrCronExpression[cronNo][3]).equals("*") || String(_irrCronExpression[cronNo][3]).equals(String(ptm->tm_mday)); // Day of month
    log("chunk dom", String(_irrCronExpression[cronNo][3]));
    log("coincide dom", String(_irrCronExpression[cronNo][3]).equals("*"));
    timeToIrrigate &= String(_irrCronExpression[cronNo][4]).equalsIgnoreCase("*") || String(_irrCronExpression[cronNo][4]).equalsIgnoreCase(mon) || String(_irrCronExpression[cronNo][4]).equals(String(ptm->tm_mon + 1)); // Month
    log("chunk mon", String(_irrCronExpression[cronNo][4]));
    log("coincide mon", String(_irrCronExpression[cronNo][4]).equals("*"));
    timeToIrrigate &= String(_irrCronExpression[cronNo][5]).equalsIgnoreCase("?") || String(_irrCronExpression[cronNo][5]).equalsIgnoreCase(dow) || String(_irrCronExpression[cronNo][5]).equals(String(ptm->tm_wday + 1)); // Day of week
    log("chunk dow", String(_irrCronExpression[cronNo][5]));
    log("coincide dow", String(_irrCronExpression[cronNo][5]).equals("?"));
    ++cronNo;
  } while (!timeToIrrigate && cronNo < MAX_CRONS);
  log("TTI", timeToIrrigate);
  return timeToIrrigate;
}

void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  log(F("MQTT message on topic"), topic);
  // Station topics
  if (String(topic).equals(_domoticModule.getStationTopic("command/cron/0"))) {
    updateCron(0, payload, length);
  } else if (String(topic).equals(_domoticModule.getStationTopic("command/cron/1"))) {
    updateCron(1, payload, length);
  } else if (String(topic).equals(_domoticModule.getStationTopic("command/state"))) {
    changeStateCommand(payload, length);
    _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), _irrigating ? "1" : "0");
  } else {
    log("Unknown topic");
  }
}

bool changeStateCommand(unsigned char* payload, unsigned int length) {
  if (length != 1) {
    log(F("Invalid payload"));
  } else {
    switch (payload[0]) {
      case '0':
        if (_irrigating) {
          // Set irrigation end to 0 to simulate it should have ended 
          Channel *channel = _domoticModule.getChannel(_currChannel);
          _domoticModule.updateChannelState(channel, HIGH);
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