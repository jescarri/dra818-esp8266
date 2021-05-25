#include <DRA818.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ElegantOTA.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#define TXT_POW 14
#define TX_PIN 15
#define RX_PIN 13
#define PTT 4
#define SQL 5
#define PD_PIN 12
#define STRING_LEN 128
#define NUMBER_LEN 32
#define CONFIG_VERSION "v1.0.2"
#define STATUS_PIN 2
#define IOTWEBCONF_CONFIG_USE_MDNS 80
#define IOTWEBCONF_STATUS_ENABLED 2
#define MQTT_MAX_RECONNECT 3

const char thingName[] = "dra818v";
const char wifiInitialApPassword[] = "VA7RCVdra";
bool ipPrinted = false;
bool needMqttConnect = false;
unsigned long lastReportPTT = 0;
unsigned long lastReportSQL = 0;
unsigned long lastMqttConnectionAttempt = 0;
bool pttState = HIGH;
bool sqlState = HIGH;
bool retained = false;
int qos = 1;
bool dup = false;
bool validConfig = false;

void handleRoot();
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper);
void wifiConnected();
void reportPTT();
void reportSQL();

char tx_rx_freqVal[NUMBER_LEN];
char otaPasswordVal[STRING_LEN];
char squelchLevelVal[2];
char volumeVal[2];
char rxCTCSSVal[3];
char txCTCSSVal[3];
static char chooserValues[][STRING_LEN] = { "0", "1", "12", "13", "14" };
static char chooserNames[][STRING_LEN] = { "0", "67.0", "100.0", "103.5", "107.2" };

static char sqvolValues[][STRING_LEN] = { "1", "2", "3", "4", "5", "6", "7", "8" };
char mqttServerVal[STRING_LEN];
char mqttUserVal[STRING_LEN];
char mqttPasswordVal[STRING_LEN];
int mqttReconnectAttempts = 0;

DNSServer dnsServer;
WebServer server(80);
WiFiClient net;
PubSubClient mqttclient(net);
SoftwareSerial dra_serial(TX_PIN, RX_PIN);
DRA818 dra(&dra_serial, PTT);

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameterGroup draConfig = IotWebConfParameterGroup("DRA818", "Radio Module Configuration");
IotWebConfParameterGroup otaConfig = IotWebConfParameterGroup("OTA Settings", "OTA PASSWORD");
IotWebConfParameterGroup mqttConfig = IotWebConfParameterGroup("MQTT Settings", "MQTT Settings");

// DRA Config Parameters
IotWebConfNumberParameter tx_rx_freq = IotWebConfNumberParameter("TX/RX Frequency", "tx_rx_freq", tx_rx_freqVal, NUMBER_LEN, nullptr, "144.0000", "step='0.1'");
IotWebConfSelectParameter volume = IotWebConfSelectParameter("Volume Level", "volume", volumeVal, STRING_LEN, (char*)sqvolValues, (char*)sqvolValues, sizeof(sqvolValues) / STRING_LEN, STRING_LEN);
IotWebConfSelectParameter squelchLevel = IotWebConfSelectParameter("Squelch Level", "squelch", squelchLevelVal, STRING_LEN, (char*)sqvolValues, (char*)sqvolValues, sizeof(sqvolValues) / STRING_LEN, STRING_LEN);
IotWebConfSelectParameter txXTCSS = IotWebConfSelectParameter("TX CTCSS", "tx CTCSS", txCTCSSVal, STRING_LEN, (char*)chooserValues, (char*)chooserNames, sizeof(chooserValues) / STRING_LEN, STRING_LEN);
IotWebConfSelectParameter rxXTCSS = IotWebConfSelectParameter("RX CTCSS", "rx CTCSS", rxCTCSSVal, STRING_LEN, (char*)chooserValues, (char*)chooserNames, sizeof(chooserValues) / STRING_LEN, STRING_LEN);

//OTA
IotWebConfPasswordParameter otaPassword = IotWebConfPasswordParameter("OTA Password", "otaPassword", otaPasswordVal, STRING_LEN);

//
IotWebConfPasswordParameter mqttPassword = IotWebConfPasswordParameter("MQTT Password", "mqttPassword", mqttPasswordVal, STRING_LEN);
IotWebConfTextParameter mqttUser = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUserVal, STRING_LEN);
IotWebConfTextParameter mqttServer = IotWebConfTextParameter("MQTT Server", "mqttServer", mqttServerVal, STRING_LEN);

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(9600);
    Serial.println();
    Serial.println("Starting up...");
    draConfig.addItem(&tx_rx_freq);
    draConfig.addItem(&squelchLevel);
    draConfig.addItem(&volume);
    draConfig.addItem(&txXTCSS);
    draConfig.addItem(&rxXTCSS);
    otaConfig.addItem(&otaPassword);
    mqttConfig.addItem(&mqttPassword);
    mqttConfig.addItem(&mqttUser);
    mqttConfig.addItem(&mqttServer);

    iotWebConf.addParameterGroup(&draConfig);
    iotWebConf.addParameterGroup(&otaConfig);
    iotWebConf.addParameterGroup(&mqttConfig);
    iotWebConf.setConfigSavedCallback(&configSaved);
    iotWebConf.setFormValidator(&formValidator);
    iotWebConf.setWifiConnectionCallback(&wifiConnected);

    validConfig = iotWebConf.init();

    // -- Set up required URL handlers on the web server.
    server.on("/", handleRoot);
    server.on("/config", [] { iotWebConf.handleConfig(); });
    server.onNotFound([]() { iotWebConf.handleNotFound(); });
    ElegantOTA.begin(&server, "admin", otaPasswordVal);

    Serial.println("Ready.");
    dra_serial.begin(9600);
    pinMode(PD_PIN, OUTPUT);
    digitalWrite(PD_PIN, HIGH);
    pinMode(PTT, INPUT);
    pinMode(SQL, INPUT);
    pinMode(TXT_POW, OUTPUT);
    digitalWrite(TXT_POW, HIGH);
    dra.setFreq(atof(tx_rx_freqVal));
    dra.setTXCTCSS(0);
    dra.setSquelch(atof(squelchLevelVal)); // Squelch level 3.
    dra.setRXCTCSS(0);			   // No CTCSS on RX.
    dra.writeFreq();			   // Write out frequency settings to the DRA module.
    dra.setVolume(atof(volumeVal));	   // Set output volume to '4'.
    dra.setFilters(true, true, true);	   // Sets all filters (Pre/De-Emphasis, High-Pass, Low-Pass) on.
}

void loop()
{
    iotWebConf.doLoop();
    if (validConfig) {
	if (!mqttclient.loop()) {
	    setupMqtt();
	}
	reportPTT();
	reportSQL();
    }
}

void handleRoot()
{
    // -- Let IotWebConf test and handle captive portal requests.
    if (iotWebConf.handleCaptivePortal()) {
	// -- Captive portal request were already served.
	return;
    }
    String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
    s += "<title>IotWebConf 03 Custom Parameters</title></head><body>Hello world!";
    s += "<ul>";
    s += "<li>txFreq: ";
    s += String(tx_rx_freqVal);
    s += "<li>Volume: ";
    s += String(volumeVal);
    s += "<li>Squelch: ";
    s += String(squelchLevelVal);
    s += "</ul>";
    s += "Go to <a href='config'>configure page</a> to change values.";
    s += "</body></html>\n";

    server.send(200, "text/html", s);
}

void configSaved()
{
    Serial.println("Configuration was updated.");
    delay(1000);
    ESP.restart();
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper)
{
    Serial.println("Validating form.");
    bool valid = true;
    return valid;
}

void wifiConnected()
{
    Serial.println(WiFi.localIP());
    if (validConfig) {
	setupMqtt();
	Serial.println("Connected to MQTT");
    }
}

void setupMqtt()
{
    if (WiFi.status() == WL_CONNECTED) {
	while (!mqttclient.connected()) {
	    if (mqttReconnectAttempts > MQTT_MAX_RECONNECT) {
		return;
	    }
	    Serial.println("Connecting to MQTT...");
	    char clientId[20];
	    String a = WiFi.macAddress();
	    String wm = "{\"name\":\"tempcontroller\",\"status\":\"dead\"}";
	    char willTopic[50];
	    char willMessage[24];
	    a.toCharArray(clientId, 20);
	    String wt = "dra818/lastwill";
	    wt.toCharArray(willTopic, 30);
	    wm.toCharArray(willMessage, 40);
	    mqttclient.setServer(mqttServerVal, 1883);
	    if (mqttclient.connect(clientId, mqttUserVal, mqttPasswordVal, willTopic, 2, 0, willMessage)) {
		mqttReconnectAttempts = 0;
		Serial.println("connected");
	    } else {
		Serial.print("failed with state ");
		Serial.print(mqttclient.state());
		mqttReconnectAttempts++;
	    }
	}
    }
}

void reportPTT()
{
    unsigned long now = millis();
    if (mqttclient.connected()) {
	if ((500 < now - lastReportPTT) && (pttState != digitalRead(PTT))) {
	    pttState = 1 - pttState; // invert pin state as it is changed
	    lastReportPTT = now;
	    Serial.print("Sending on MQTT channel 'dra818/ptt' :");
	    Serial.println(pttState == LOW ? "ON" : "OFF");
	    switch (pttState) {
	    case LOW:
		mqttclient.publish("dra818/ptt", "1");
		break;
	    case HIGH:
		mqttclient.publish("dra818/ptt", "0");
		break;
	    }
	}
    }
}

void reportSQL()
{
    unsigned long now = millis();
    if (mqttclient.connected()) {
	if ((500 < now - lastReportSQL) && (sqlState != digitalRead(SQL))) {
	    Serial.println("w000t");
	    sqlState = 1 - sqlState; // invert pin state as it is changed
	    lastReportSQL = now;
	    Serial.print("Sending on MQTT channel 'dra818/sql' :");
	    Serial.println(sqlState == LOW ? "ON" : "OFF");
	    switch (sqlState) {
	    case LOW:
		mqttclient.publish("dra818/sql", "1");
		break;
	    case HIGH:
		mqttclient.publish("dra818/sql", "0");
		break;
	    }
	}
    }
}
