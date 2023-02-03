#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "painlessMesh.h"
#include <Arduino_JSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include <time.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <HTTPClient.h>

String serverName = "http://192.168.4.22:1880/update-sensor";

// NODE CONFIG
int AP_NODE = 5;
int MASTER_NODE = 1;

IPAddress local_IP(192,168,4,22);
IPAddress gateway(192,168,4,9);
IPAddress subnet(255,255,255,0);

// TODO:
// Wifi AP toevoegen aan script
// Selectie backup master node
// Fixen bug met uitlezen JSON data (recieve4)
// POST-request naar Rasperry Pi
//   - Alert
//   - Alle logs
// Deep Sleep functie toevoegen

// MESH Details
#define   MESH_PREFIX     "ESP_NET_WERK_MESH" // Naam van mesh
#define   MESH_PASSWORD   "OmdatHetNetWerkt" // Wachtwoord van mesh
#define   MESH_PORT       5555  

const char *ssid = "Config_Node_NetWerk";
const char *passphrase = "987654321";

// Identifier voor deze node
int nodeNumber = 1;

// Online nodes
int onlineNodes[] = {};

// Counters voor aantal logs
int aantal_logs = 0;
int aantal_logs_local = 0;

//BME object on the default I2C pins
Adafruit_BME280 bme;
// String to send to other nodes with sensor readings
String readings;
String send_logs;

Scheduler userScheduler; 
painlessMesh  mesh;

// PROTOTYPES
void sendSensorData();
void storeLocalSensorData();
void onDroppedConnection(unsigned int nodeId );
void checkStatus();
void sendAlive();
void sendMessage3();
void sendReply4(int nodeid);
void sendLogsToServer();
String getReadings();

/*
* TASKS
*/

//Create task: to send messages and get readings;
Task taskSendMessage(TASK_SECOND * 10 , TASK_FOREVER, &sendSensorData);

//Create task: to store local data readings (every second)
Task storeLocalSensorReadings(TASK_SECOND * 1 , TASK_FOREVER, &storeLocalSensorData);

//Create task: to check for temperature and humidity changes
Task checkStatusTask(TASK_SECOND * 1 , TASK_FOREVER, &checkStatus);

//Create task: to check status of masternode
Task sendAliveTask(TASK_SECOND * 5, TASK_FOREVER, &sendAlive);

//Create task: to send send a message to receive all logs from the masternode
Task sendMessage3Task(TASK_SECOND * 20, 2, &sendMessage3);

Task syncLogsWithRasperry(TASK_SECOND * 10, TASK_FOREVER, &sendLogsToServer);

/*
* STRUCTURES
*/

struct Log {
  int node;
  double temp;
  double hum;
  double pres;
  time_t logged_at;
};

struct LocalLog {
  double temp;
  double hum;
  time_t logged_at;
};

// Alloceer memory voor de logs
struct Log *logs = (struct Log *) malloc(sizeof(struct Log) * 500);

// Alloceer memory voor de logs (LOCAL CONTEXT)
struct LocalLog *localData = (struct LocalLog *) malloc(sizeof(struct LocalLog) * 10);


/*
* START OF CODE
*/

String getReadings () {
  JSONVar jsonReadings;
  jsonReadings["type"] = 1;
  jsonReadings["node"] = nodeNumber;
  jsonReadings["temp"] = bme.readTemperature();
  jsonReadings["hum"] = bme.readHumidity();
  jsonReadings["pres"] = bme.readPressure()/100.0F;
  time_t current_time = time(NULL);
  //printf("Current time: %s", ctime(&current_time));
  jsonReadings["logged_at"] = current_time;
  readings = JSON.stringify(jsonReadings);
  return readings;
}

void sendMessage3 () {
  // stuurt een bericht om de logs te krijgen
  JSONVar msg3;
  msg3["type"] = 3;
  msg3["nodeid"] = nodeNumber;
  mesh.sendBroadcast(JSON.stringify(msg3));
}

void sendReply4 (int nodeid) {
  // verstuurt het antwoord op een type 3 bericht
  for (int log_index = 1; log_index <= aantal_logs; log_index++) {
    JSONVar msg4;
    msg4["type"] = 4;
    msg4["nodeid"] = nodeid;
    msg4["node"] = logs[log_index].node;
    msg4["temp"] = logs[log_index].temp;
    msg4["hum"] = logs[log_index].hum;
    msg4["pres"] = logs[log_index].pres;
    msg4["logged_at"] = logs[log_index].logged_at;
    mesh.sendBroadcast(JSON.stringify(msg4));
  }
}

void sendSensorData () {
  // Lees sensorwaarde uit van functie en parse JSON
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());
  
  // Sla sensorwaarden op in logs
  logs[aantal_logs].node = messageObject["node"];
  logs[aantal_logs].temp = messageObject["temp"];
  logs[aantal_logs].hum = messageObject["hum"];
  logs[aantal_logs].pres = messageObject["pres"];
  logs[aantal_logs].logged_at = messageObject["logged_at"];
  // Serial.printf("%d\n", aantal_logs);
  // Serial.printf("%d\n", logs[1].node);
  // Serial.printf("%lf\n", logs[1].temp);
  // Serial.printf("%lf\n", logs[1].hum);
  // Serial.printf("%lf\n", logs[1].pres);
  aantal_logs += 1;
  
  // Broadcast = naar alle andere nodes
  mesh.sendBroadcast(msg);
}

void sendAlive() {
  if (MASTER_NODE == nodeNumber) {
    JSONVar Alive;
    Alive["type"] = 5;
    mesh.sendBroadcast(JSON.stringify(Alive));
  }
}

void storeLocalSensorData() {
  // Lees sensorwaarde
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());

  // Checken hoe groot de huidige array is
  // alternatief: aantal_logs = _countof(localData);
  if (aantal_logs_local >= 9) {
    for (int log_index = aantal_logs_local - 1; log_index < 0; log_index--) {
      localData[log_index] = localData[log_index+1];
    }
    localData[0].temp = messageObject["temp"];
    localData[0].hum = messageObject["hum"];
    localData[0].logged_at = messageObject["logged_at"];
  }
  else {
    localData[9 - aantal_logs_local].temp = messageObject["temp"];
    localData[9 - aantal_logs_local].hum = messageObject["hum"];
    localData[9 - aantal_logs_local].logged_at = messageObject["logged_at"];
    aantal_logs_local += 1;
  }
}

void checkStatus() {
  int temp_sum = 0;
  int hum_sum = 0;
  for (int log_index = 1; log_index < aantal_logs_local; log_index++) {
    temp_sum += localData[log_index].temp;
  }
  for (int log_index = 1; log_index < aantal_logs_local; log_index++) {
    hum_sum += localData[log_index].hum;
  }
  int average_hum = hum_sum / (aantal_logs_local - 1);
  int current_hum = localData[0].hum;
  int average_temp = temp_sum / (aantal_logs_local - 1);
  int current_temp = localData[0].temp;
  if (((current_temp / average_temp) < 0.9) && ((current_hum / average_hum) < 0.8)) {
    //Alert toevoegen!!!
  }
}

void sendEmptyLogsMessage() {
  JSONVar message;
  message["type"] = 2;
  mesh.sendBroadcast(JSON.stringify(message));
}


// Initialiseren van sensor
void initBME(){
  if (!bme.begin(0x76)) {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
    while (1);
  }  
}

// Needed for painless library
void receivedCallback( uint32_t from, String &msg ) {
  Serial.printf("Received from %u msg=%s\n", from, msg.c_str());
  JSONVar messageObject = JSON.parse(msg.c_str());
  int type = messageObject["type"];
  // MESSAGE TYPES
  // 1 = Synchroniseer logs realtime (10 seconden)
  // 2 = Delete logs op alle nodes 
  // 3 = Als masternode stuur alle aanwezige logs naar nieuwe ESP
  // 4 = Ontvang alle logs van masternode 

  if (type == 1) {
    // SYNC SENSOR DATA
    int node = messageObject["node"];
    double temp = messageObject["temp"];
    double hum = messageObject["hum"];
    double pres = messageObject["pres"];
    time_t logged_at = messageObject["logged_at"];

    logs[aantal_logs].node = node;
    logs[aantal_logs].temp = temp;
    logs[aantal_logs].hum = hum;
    logs[aantal_logs].pres = pres;
    logs[aantal_logs].logged_at = logged_at;
    aantal_logs += 1;

  } else if (type == 2) {
    // FLUSH LOGS
    free(logs);
    struct Log *logs = (struct Log *) malloc(sizeof(struct Log) * 500);
  } else if ((type == 3) && (MASTER_NODE == nodeNumber)) {
    int nodeid = messageObject["nodeid"];
    sendReply4(nodeid);
    Serial.printf("Message 4 verzonden \n");
  } else if (type == 4) {
    // **WERKT MISSCHIEN?** ontvangt de volledige logs van de masternode
    int nodeid = messageObject["nodeid"];
    if (nodeid == nodeNumber) {
      int node = messageObject["node"];
      double temp = messageObject["temp"];
      double hum = messageObject["hum"];
      double pres = messageObject["pres"];
      time_t logged_at = messageObject["logged_at"];

      logs[aantal_logs].node = node;
      logs[aantal_logs].temp = temp;
      logs[aantal_logs].hum = hum;
      logs[aantal_logs].pres = pres;
      logs[aantal_logs].logged_at = logged_at;
      aantal_logs += 1;
    }
  }
}

int getRandomOnlineNode() {
  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  int randomNumber = rand() % currentNumberOfNodes; 
  return onlineNodes[randomNumber];
}

void newConnectionCallback(uint32_t nodeId) {
  // Lengte van de array waarin alle 'online' nodes zitten
  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  
  // Haalt eerst de eventuele node uit de online nodes
  delete_node(nodeId, currentNumberOfNodes);

  // Voegt node toe aan online nodes
  onlineNodes[currentNumberOfNodes + 1] = nodeId;

  Serial.printf("New Connection, nodeId = %u\n", nodeId);
}

void delete_node(int nodeId, int arraySize) {
    int i = 0;
    int index = -1;

    for(i = 0; i < arraySize; i++)
    {
        if(onlineNodes[i] == nodeId)
        {
            index = i;
            break;
        }
    }

    if(index != -1) {
        for(i = index; i < arraySize - 1; i++) {
          onlineNodes[i] = onlineNodes[i+1];
        }

        printf("New Array : ");
        for(i = 0; i < arraySize - 1; i++) {
          printf("%d ",onlineNodes[i]);
        }
    }
    else
        printf("Element Not Found\n");
}

void onDroppedConnection(unsigned int nodeId) {
  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  delete_node(nodeId, currentNumberOfNodes);
  Serial.printf("Node disconnected\n");
}

void sendLogsToServer() {
  if (nodeNumber == MASTER_NODE) {
    JSONVar sendLogs;
    for (int index_log = 0; index_log < aantal_logs; index_log++) {
      //JSONVar sendLogs;
      sendLogs[index_log]["nodeId"] = logs[index_log].node;
      sendLogs[index_log]["humidity"] = logs[index_log].hum;
      sendLogs[index_log]["pressure"] = logs[index_log].pres;
      sendLogs[index_log]["temperature"] = logs[index_log].temp;
      //Serial.printf("%d\n", index_log);
    }
    WiFiClient client;
    HTTPClient http;
    send_logs = JSON.stringify(sendLogs);
    //Serial.printf(send_logs);
    http.begin(client, "http://192.168.4.23:1880/sync");
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST(send_logs);
    //"{\"api_key\":\"tPmAT5Ab3j7F9\",\"sensor\":\"BME280\",\"value1\":\"24.25\",\"value2\":\"49.54\",\"value3\":\"1005.14\"}"
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    // End connection
    http.end();
    if (httpResponseCode != -1) {
      sendEmptyLogsMessage();
      free(logs);
      struct Log *logs = (struct Log *) malloc(sizeof(struct Log) * 500);
    }
  }
  }


void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(),offset);
}

void setup() {
  Serial.begin(115200);

  // Note: als sensor niet (correct) is aangesloten, geeft hij een error en stopt hij met uitvoeren
  initBME();

  // Mogelijke flags voor debugging ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE
  mesh.setDebugMsgTypes( ERROR | STARTUP | DEBUG);  

  // Initieer wifi-mesh
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );

  // voert de receivedCallback functie uit bij een binnenkomend bericht
  mesh.onReceive(&receivedCallback);

  // voert newConnectionCallback uit wanneer nieuwe node de mesh joined
  mesh.onNewConnection(&newConnectionCallback);
  
  // Wanneer een node offline gaat
  mesh.onDroppedConnection(&onDroppedConnection);

  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  // Stuur bij opstarten een type 3 message om alle logs van masternode te krijgen
  userScheduler.addTask(sendMessage3Task);
  sendMessage3Task.enable();

  // Serial.printf("Message type 3 werkt. \n");
  // for (int x = 0; x < 10; x++) {
  //   if (logs != NULL) {
  //     break;
  //   } 
  //   delay(1000);
  // }
  
  // Wordt elke 10 seconden uitgevoerd
  userScheduler.addTask(taskSendMessage);

  userScheduler.addTask(storeLocalSensorReadings);
  
  userScheduler.addTask(checkStatusTask);
  
  userScheduler.addTask(sendAliveTask);

  userScheduler.addTask(syncLogsWithRasperry);

  taskSendMessage.enable();
  storeLocalSensorReadings.enable();
  checkStatusTask.enable();
  sendAliveTask.enable();
  syncLogsWithRasperry.enable();

  if (nodeNumber == AP_NODE) {
    Serial.print("Setting soft-AP configuration ... ");
    Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ? "Ready" : "Failed!");

    Serial.print("Setting soft-AP ... ");
    Serial.println(WiFi.softAP(ssid, passphrase) ? "Ready" : "Failed!");

    Serial.print("Soft-AP IP address = ");
    Serial.println(WiFi.softAPIP());
  } else if (nodeNumber == MASTER_NODE) {
    // Master node connect met 
    WiFi.mode(WIFI_AP_STA); //Optional
    WiFi.begin(ssid, passphrase);
  }
}

void loop() {
  mesh.update();
}
