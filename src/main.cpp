#include <ESP8266WiFi.h>
#include <ESPDomotic.h>
#include <time.h>
#include <string>
#include <sstream>

/*
HEAP Improvement https://learn.adafruit.com/memories-of-an-arduino/optimizing-sram
Avoid String https://hackingmajenkoblog.wordpress.com/2016/02/04/the-evils-of-arduino-strings/
*/

void setup();
void loop();
void initializeScheduling();
bool saveCronConf(uint8_t cronNo);
void loadCronConf(uint8_t cronNo);
void updateCronField(uint8_t cronNo, uint8_t index, const char* value);
void checkIrrigation();
bool isTimeToCheckSchedule ();
bool isTimeToIrrigate ();
void receiveMqttMessage(char* topic, uint8_t* payload, unsigned int length);
void updateCronCommand(char* topic, unsigned char* payload, unsigned int length);
bool changeStateCommand(char* topic, unsigned char* payload, unsigned int length);

/* Constants */
const char* DAYS_OF_WEEK[]    = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char* MONTHS_OF_YEAR[]  = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DIC"};

typedef char* tCron[6];

/* Crons management */
const char*   CRONS_CONF_FILES[]      = {"CRON_0.conf", "CRON_1.conf", "CRON_2.conf"};
const size_t  CRON_FIELD_STRING_SIZE  = CRON_FIELD_SIZE + 1;
const size_t  CRON_FIELDS             = 6;
const char*   CRON_FIELDS_NAMES[]     = {"SEC", "MIN", "HOU", "DOM", "MON", "DOW"};

const char* MODULE_TYPE               = "irrigation";

/* Irrigation control */
tCron           _irrCronExpressions[MAX_CRONS] = {
  {new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE]}, // 3 + 1 (max field length + '\0')
  {new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE]},
  {new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE], new char[CRON_FIELD_STRING_SIZE]}
};
bool            _irrigating           = false;
uint8_t         _currChannel          = 0;
unsigned long   _irrLastScheduleCheck = -TIMER_CHECK_THRESHOLD_SECONDS * 1000;

/* Channels control */
#ifdef NODEMCUV2
  // Id, Name, pin, state, timer 
Channel _channelA ("A", "channel_A", D1, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelB ("B", "channel_B", D2, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelC ("C", "channel_C", D4, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
Channel _channelD ("D", "channel_D", D5, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);

const uint8_t LED_PIN         = D7;
// TODO Define pin consts in configuration file (ini file)
#elif ESP12
  Channel _channelA ("A", "channel_A", 2, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelB ("B", "channel_B", 4, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelC ("C", "channel_C", 5, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);
  Channel _channelD ("D", "channel_D", 16, OUTPUT, HIGH, CHANNEL_DEFAULT_TIMER * 1000);

const uint8_t LED_PIN         = 13;
#endif

ESPDomotic _domoticModule;

template <typename T> void debug(T t) {
  Serial.println(t);
}

 /* recursive variadic function */
template<typename T, typename... Args> void debug(T t, Args... args) {
  Serial.print(t);
  Serial.print(" ");
  debug(args...) ;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  debug(F("Starting module"));
  std::ostringstream s;
  s << "Proeza irrigation " << ESP.getChipId();
  std::string chipname(s.str());
  _domoticModule.setPortalSSID(chipname.c_str());
  _domoticModule.setFeedbackPin(LED_PIN);
  _domoticModule.setMqttMessageCallback(receiveMqttMessage);
  _domoticModule.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
  _domoticModule.setWifiConnectTimeout(WIFI_CONNECT_TIMEOUT);
  _domoticModule.setConfigFileSize(CONFIG_FILE_SIZE);
  _domoticModule.setModuleType(MODULE_TYPE);
  _domoticModule.addChannel(&_channelA);
  _domoticModule.addChannel(&_channelB);
  _domoticModule.addChannel(&_channelC);
  _domoticModule.addChannel(&_channelD);
  _domoticModule.init();
  debug(F("Connected to wifi network. Local IP"), WiFi.localIP());
  configTime(TIMEZONE * 3600, 0, "ar.pool.ntp.org", "br.pool.ntp.org", "cl.pool.ntp.org"); // argentina, brazil, chile
  debug(F("Waiting for time..."));
  time_t now;
  while (!(now = time(nullptr))) {
    delay(500);
  }
  Serial.printf("Current time: %s", ctime(&now));
  initializeScheduling();
}

void initializeScheduling () {
  debug(F("Initializing scheduling"));
  for (uint8_t cronNo = 0; cronNo < MAX_CRONS; ++cronNo) {
    loadCronConf(cronNo);
  }
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
        debug(F("Irrigation sequence started"));
        _irrigating = true;
        _currChannel = 0;
        _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), "1");
      }
    }
  } else {
    if (_currChannel >= _domoticModule.getChannelsCount()) {
      debug(F("No more channels to process. Stoping irrigation sequence."));
      _irrigating = false;
      _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), "0");
    } else {
      Channel *channel = _domoticModule.getChannel(_currChannel);
      if (!channel->isEnabled()) {
        debug(F("Channel is disabled, going to next one."), channel->name);
        ++_currChannel;
      } else {
        if (channel->state == HIGH) {
          debug(F("Openning channel"), channel->name);
          _domoticModule.updateChannelState(channel, LOW);
        } else {
           if (channel->timeIsUp()) {
            debug(F("Closing channel"), channel->name);
            _domoticModule.updateChannelState(channel, HIGH);
            _currChannel++;
            delay(CLOSE_VALVE_DELAY);
          }
        }
      }
    }
  }
}

void receiveMqttMessage(char* topic, unsigned char* payload, unsigned int length) {
  debug(F("MQTT message on topic"), topic);
  // Station topic
  std::string receivedTopic = std::string(topic);
  if (receivedTopic.find(_domoticModule.getStationTopic("command/cron/").c_str()) != std::string::npos) {
    updateCronCommand(topic, payload, length);
  } else if (receivedTopic.find(_domoticModule.getStationTopic("command/state").c_str()) != std::string::npos) {
    changeStateCommand(topic, payload, length);
    _domoticModule.getMqttClient()->publish(_domoticModule.getStationTopic("feedback/state").c_str(), _irrigating ? "1" : "0");
  } else {
    debug(F("Not a station topic"));
  }
}

void updateCronCommand (char* topic, unsigned char* payload, unsigned int length) {
  std::string tail = std::string(topic).substr(_domoticModule.getStationTopic("command/cron/").length());
  size_t cronID = atoi(tail.substr(0, tail.find("/")).c_str());
  size_t fieldID = atoi(tail.substr(tail.find("/") + 1).c_str());
  char value[length + 1];
  strcpy(value, (char*) payload);
  value[length] = '\0'; // to treat payload as string
  updateCronField(cronID, fieldID, value);
  saveCronConf(cronID);
  String feedbackTopic = _domoticModule.getStationTopic("feedback/cron/") + tail.c_str();
  _domoticModule.getMqttClient()->publish(feedbackTopic.c_str(), value);
}

void loadCronConf(uint8_t cronNo) {
  debug(F("Getting configuration for cron number"), cronNo);
  char* conf = _domoticModule.getConf(CRONS_CONF_FILES[cronNo]);
  if (conf) {
    debug(F("Cron conf loaded"), conf);
    size_t i = 0, chunkNo = 0, length = strlen(conf);
    while (i < length && chunkNo < CRON_FIELDS) {
      if (conf[i] == ' ') {
        ++i;
      } else {
        char value[CRON_FIELD_STRING_SIZE] = {'\0'}; 
        int j = 0;
        while (i < length && conf[i] != ' ' && j < CRON_FIELD_SIZE) 
          value[j++] = conf[i++];
        updateCronField(cronNo, chunkNo++, value);
      }
    }
    delete[] conf;
  } else { 
    debug(F("Conf not found for cron number"), cronNo, F(". Setting defaults"));
    updateCronField(cronNo, 0, "0");
    updateCronField(cronNo, 1, "0");
    updateCronField(cronNo, 2, "6");
    updateCronField(cronNo, 3, "*");
    updateCronField(cronNo, 4, "*");
    updateCronField(cronNo, 5, "?");
  }
}

void updateCronField(uint8_t cronNo, uint8_t fieldNo, const char* value) {
  debug(F("Updating cron ["), cronNo, F("] field ["), CRON_FIELDS_NAMES[fieldNo], F("] with value:"), value);
  strcpy(_irrCronExpressions[cronNo][fieldNo], value);
}

bool saveCronConf (uint8_t cronNo) {
  char toSave[CRON_FIELDS * CRON_FIELD_SIZE + CRON_FIELDS] = {'\0'}; //fields size + spaces + end char
  size_t cursor = 0;
  for (size_t fieldNo = 0; fieldNo < CRON_FIELDS; ++fieldNo) {
    size_t i = 0;
    while (i < CRON_FIELD_SIZE && _irrCronExpressions[cronNo][fieldNo][i] != '\0') {
      toSave[cursor++] = _irrCronExpressions[cronNo][fieldNo][i++];
    }
    if (fieldNo != CRON_FIELDS - 1) 
      toSave[cursor++]= ' '; //no space after last field
  }
  debug(F("Saving cron"), cronNo, F(" conf: "), toSave);
  return _domoticModule.updateConf(CRONS_CONF_FILES[cronNo], toSave);
}

bool changeStateCommand(char* topic, unsigned char* payload, unsigned int length) {
  if (length != 1) {
    debug(F("Invalid payload"));
  } else {
    switch (payload[0]) {
      case '0':
        if (_irrigating) {
          // Set irrigation end to 0 to simulate it should have ended 
          Channel *channel = _domoticModule.getChannel(_currChannel);
          _domoticModule.updateChannelState(channel, HIGH);
          _irrigating = false;
          debug(F("Irrigation stopped"));
        }
        return true;
      case '1':
        if (!_irrigating) {
          _irrigating = true;
          _currChannel = 0;
          debug(F("Irrigation started"));
        }
        return true;
      default:
        debug(F("Invalid state"), payload[0]);
    }
  }
  return false;
}

bool isTimeToCheckSchedule () {
  bool isTime = _irrLastScheduleCheck + TIMER_CHECK_THRESHOLD_SECONDS * 1000 < millis();
  _irrLastScheduleCheck = isTime ? millis() : _irrLastScheduleCheck;
  return isTime;
}

bool isTimeToIrrigate () {
  debug(F("Checking if it is time to start irrigation"));
  time_t now = time(nullptr);
  Serial.printf("Current time: %s", ctime(&now));
  struct tm * ptm = localtime(&now);
  const char* mon = ptm->tm_mon < 12 ? MONTHS_OF_YEAR[ptm->tm_mon] : "";
  const char* dow = ptm->tm_wday < 7 ? DAYS_OF_WEEK[ptm->tm_wday] : "";
  // Evaluates cron at minute level. Seconds granularity is not needed for irrigarion scheduling.
  boolean tti;
  uint8_t cronNo = 0;
  do {
    tti = true;
    tti &= _irrCronExpressions[cronNo][1][0] == '*' 
                      || atoi(_irrCronExpressions[cronNo][1]) == ptm->tm_min 
                      || (  
                          ptm->tm_min > atoi(_irrCronExpressions[cronNo][1]) 
                          && 
                          ptm->tm_min - (TIMER_CHECK_THRESHOLD_SECONDS / 60) <= atoi(_irrCronExpressions[cronNo][1])
                        ); // Minute
    tti &= _irrCronExpressions[cronNo][2][0] == '*' 
                      || atoi(_irrCronExpressions[cronNo][2]) == ptm->tm_hour; // Hour
    tti &= _irrCronExpressions[cronNo][3][0] == '*' 
                      || atoi(_irrCronExpressions[cronNo][3]) == ptm->tm_mday; // Day of month
    tti &= _irrCronExpressions[cronNo][4][0] == '*' 
                      || strncasecmp(mon, _irrCronExpressions[cronNo][4], CRON_FIELD_SIZE) == 0 
                      || atoi(_irrCronExpressions[cronNo][4]) == ptm->tm_mon + 1; // Month
    tti &= _irrCronExpressions[cronNo][5][0] == '?'
                      || strncasecmp(dow, _irrCronExpressions[cronNo][5], CRON_FIELD_SIZE) == 0
                      || atoi(_irrCronExpressions[cronNo][5]) == ptm->tm_wday + 1; // Day of week
    debug(F("Checking cron"), cronNo);
    ++cronNo;
  } while (!tti && cronNo < MAX_CRONS);
  debug(F("IS TTI?"), tti ? "YES" : "NO");
  return tti;
}