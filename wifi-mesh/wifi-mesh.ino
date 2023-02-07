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
#include "ESP32Time.h"

// NODE CONFIG
int AP_NODE = 5;
int MASTER_NODE = 1;
int nodeNumber = 1;

IPAddress local_IP(192,168,4,22);
IPAddress gateway(192,168,4,9);
IPAddress subnet(255,255,255,0);

// MESH Details
#define   MESH_PREFIX     "ESP_NET_WERK_MESH" // Name of the mesh
#define   MESH_PASSWORD   "OmdatHetNetWerkt" // password of the mesh
#define   MESH_PORT       5555  

const char *ssid = "Config_Node_NetWerk";
const char *passphrase = "987654321";

//ESP32Time rtc;
ESP32Time rtc(3600);

// Countdown until a new masternode is chosen
int Master_countdown = 15;

// Used for chosing the nodeid of the new masternode
int Onlinenode = NULL;

// Online nodes
int onlineNodes[] = {};

// Counters for amount of logs
int aantal_logs = 0;
int aantal_logs_local = 0;

//BME object on the default I2C pins
Adafruit_BME280 bme;
// String to send to other nodes with sensor readings
String readings;
String send_logs;
String send_alert;

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
void sendAlertToServer(int node, double hum, double temp, double pres, time_t logged_at);
String getReadings();
void Mastercount();

/*
* TASKS
*/

//Create task to send messages and get readings;
Task taskSendMessage(TASK_SECOND * 10 , TASK_FOREVER, &sendSensorData);

//Create task to store local data readings (every second)
Task storeLocalSensorReadings(TASK_SECOND * 1 , TASK_FOREVER, &storeLocalSensorData);

//Create task to check for temperature and humidity changes
Task checkStatusTask(TASK_SECOND * 1 , TASK_FOREVER, &checkStatus);

//Create task to check status of masternode
Task sendAliveTask(TASK_SECOND * 5, TASK_FOREVER, &sendAlive);

//Create task to send send a message to receive all logs from the masternode
Task sendMessage3Task(TASK_SECOND * 20, 2, &sendMessage3);

//Creates task to send current logs to the server
Task syncLogsWithRasperry(TASK_SECOND * 20, TASK_FOREVER, &sendLogsToServer);

//Create task to count down from 15s since the last type 5 message
Task MastercountTask(TASK_SECOND * 5, TASK_FOREVER, &Mastercount);

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

// Allocate memory for the logs
struct Log *logs = (struct Log *) malloc(sizeof(struct Log) * 500);

// Allocate memory for the locally stored logs
struct LocalLog *localData = (struct LocalLog *) malloc(sizeof(struct LocalLog) * 10);


/*
* START OF CODE
*/

String getReadings () {
  // Measure the sensor readings and compile them in JSON format
  JSONVar jsonReadings;
  jsonReadings["type"] = 1;
  jsonReadings["node"] = nodeNumber;
  jsonReadings["temp"] = bme.readTemperature();
  jsonReadings["hum"] = bme.readHumidity();
  jsonReadings["pres"] = bme.readPressure()/100.0F;
  time_t current_time = time(NULL);
  jsonReadings["logged_at"] = current_time;
  readings = JSON.stringify(jsonReadings);
  return readings;
}

void sendMessage3 () {
  // Send a message asking for all logs (only occurs when node enters the Wi-Fi mesh)
  JSONVar msg3;
  msg3["type"] = 3;
  msg3["nodeid"] = nodeNumber;
  mesh.sendBroadcast(JSON.stringify(msg3));
}

void sendReply4 (int nodeid) {
  // Broadcast all current logs available on the device. This is as a reply on the type 3 message.
  for (int log_index = 0; log_index < aantal_logs; log_index++) {
    JSONVar msg4;
    msg4["type"] = 4;
    msg4["nodeid"] = nodeid;
    msg4["node"] = logs[log_index].node;
    msg4["temp"] = logs[log_index].temp;
    msg4["hum"] = logs[log_index].hum;
    msg4["pres"] = logs[log_index].pres;
    msg4["logged_at"] = logs[log_index].logged_at;
    msg4["masternode"] = MASTER_NODE;
    mesh.sendBroadcast(JSON.stringify(msg4));
  }
}

void sendSensorData () {
  // Retrieve sensordata and parse JSON, then send it to all nodes
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());
  
  // Save the data on the current device. 
  logs[aantal_logs].node = messageObject["node"];
  logs[aantal_logs].temp = messageObject["temp"];
  logs[aantal_logs].hum = messageObject["hum"];
  logs[aantal_logs].pres = messageObject["pres"];
  logs[aantal_logs].logged_at = messageObject["logged_at"];
  aantal_logs += 1;
  
  mesh.sendBroadcast(msg);
}

void sendAlive() {
  // Sends a message if current node is the masternode. This lets the other nodes the masternode is still running
  if (MASTER_NODE == nodeNumber) {
    JSONVar Alive;
    Alive["type"] = 5;
    Alive["nodeid"] = nodeNumber;
    mesh.sendBroadcast(JSON.stringify(Alive));
  }
}

void storeLocalSensorData() {
  // Get the current sensor data. This is than stored on the current device.
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());

  // If there are 10 local_logs, remove the last log in the array and shift the entire array right by one place.
  // If the there are less than 10 local_logs a new log is added to the last empty location in the array and add 1 to aantal_local_logs.
  if (aantal_logs_local >= 10) {
    for (int log_index = (aantal_logs_local - 2); log_index >= 0; log_index--) {
      localData[log_index+1].temp = localData[log_index].temp;
      localData[log_index+1].hum = localData[log_index].hum;
      localData[log_index+1].logged_at = localData[log_index].logged_at;
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
  // Checks the local logs to see if and rapid changes occured. If a rapid increase in humidity and decrease in temperature is detected an alert is send.
  if (aantal_logs_local == 10) {
    int temp_sum = 0;
    int hum_sum = 0;
    int average_hum = 0;
    int average_temp = 0;
    for (int log_index = 1; log_index < aantal_logs_local; log_index++) {
      temp_sum += localData[log_index].temp;
    }
    for (int log_index = 1; log_index < aantal_logs_local; log_index++) {
      hum_sum += localData[log_index].hum;
    }
    average_hum = hum_sum / (aantal_logs_local - 1);
    float current_hum = localData[0].hum;
    
    average_temp = temp_sum / (aantal_logs_local - 1);
    float current_temp = localData[0].temp;
    if ((current_hum / average_hum) > 1.6) {
      JSONVar alert;
      alert["type"] = 99;
      alert["node"] = nodeNumber;
      alert["temp"] = bme.readTemperature();
      alert["hum"] = bme.readHumidity();
      alert["pres"] = bme.readPressure()/100.0F;
      time_t current_time = time(NULL);
      alert["logged_at"] = current_time;
      mesh.sendBroadcast(JSON.stringify(alert));
    }
  }
}

void sendEmptyLogsMessage() {
  // Broadcast a message indicating the removal of all logs on the devices.
  JSONVar message;
  message["type"] = 2;
  mesh.sendBroadcast(JSON.stringify(message));
}

void Mastercount() {
  /*
  if the current node is the accesspoint the function will check if the masternode is still running. if the masternode isn't responding for 15 seconds the AP will send 
  a message asking for the nodenumbers of the available nodes.
  */ 
  if (nodeNumber == AP_NODE) {
    if ((Onlinenode != NULL) && (Onlinenode != 2)) {
      MASTER_NODE = Onlinenode;
      Master_countdown = 15;
      Onlinenode = NULL;

      JSONVar masterchange;
      masterchange["type"] = 8;
      masterchange["newmaster"] = MASTER_NODE;
      mesh.sendBroadcast(JSON.stringify(masterchange));
    } else if (Master_countdown > 0) {
      Master_countdown = Master_countdown - 5;

    } else {
      JSONVar requestNodeNumber;
      requestNodeNumber["type"] = 6;
      mesh.sendBroadcast(JSON.stringify(requestNodeNumber));
    }
  }
}

void sendReply7 () {
  // Send a reply to a received type 6 message.
  JSONVar msg7;
  msg7["type"] = 7;
  msg7["onlinenode"] = nodeNumber;

  mesh.sendBroadcast(JSON.stringify(msg7));
}

// Initialize the sensor. If there is not a valid sensor, print a warning.
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
  // 1 = Broadcast the logs to all nodes every 10 seconds.
  // 2 = Delete all logs on all nodes.
  // 3 = If the receiver is a masternode, send all present logs to the newly entered node.
  // 4 = Receive all logs from the masternode if it made the request. 
  // 5 = If the message is send from the current masternode the master_countdown will be reset back to 15 seconds.
  // 6 = When recieved the node will respond with a type 7 message which contains the current node's nodenumber
  // 7 = When recieved as the accesspoint, the contents of the message will be set in the Onlinenode var.
  // 8 = When the accesspoint chose a new masternode it will send a type 8 message. when recieved the node will change the MASTER_NODE variable to match the AP
  // 99 = This message means the conditions have been met for an alert. Then the sendAlertToServer function will be invoked.

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
    memset(logs, 0, 500 * sizeof(logs));
    aantal_logs = 0;
    
  } else if ((type == 3) && (MASTER_NODE == nodeNumber)) {
    // SEND ALL LOGS
    int nodeid = messageObject["nodeid"];
    sendReply4(nodeid);
    
  } else if (type == 4) {
    // RECEIVE ALL LOGS FROM MASTERNODE
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
      MASTER_NODE = messageObject["masternode"];
    }

  } else if ((type == 5) && (AP_NODE == nodeNumber)) {
    // RESET COUNTDOWN FOR MASTERNODE
    int nodeid = messageObject["nodeid"];
    if (nodeid == MASTER_NODE) {
      Master_countdown = 15;
    }
    
  } else if (type == 6) {
    // REPLY WITH OWN NODENUMBER
    sendReply7();

  } else if ((type == 7) && (AP_NODE == nodeNumber)) {
    // PUSH RECEIVED NODENUMBER TO ONLINENODE
    int newnode = messageObject["onlinenode"];
    Onlinenode = newnode;
    
    
  } else if (type == 8) {
    // SET NEW MASTERNODE
    int master = messageObject["newmaster"];
    MASTER_NODE = master;

    if (nodeNumber == MASTER_NODE) {
      // IF MASTERNODE, CONNECT TO WIFI
      WiFi.mode(WIFI_AP_STA); 
      WiFi.begin(ssid, passphrase);
    }

  } else if ((type == 99) && (MASTER_NODE == nodeNumber)){
    // SAVE LOGS AND SEND DATA TO ALERT-URL
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
    sendAlertToServer(node, hum, temp, pres, logged_at);
  }
}

int getRandomOnlineNode() {
  // Chose a random online node from the array.
  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  int randomNumber = rand() % currentNumberOfNodes; 
  return onlineNodes[randomNumber];
}

void newConnectionCallback(uint32_t nodeId) {
  // For a new connection add the new node to OnlineNodes[]. This new connection is first removed from the OnlineNodes[].
  // This new connection is printed.

  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  
  delete_node(nodeId, currentNumberOfNodes);

  onlineNodes[currentNumberOfNodes + 1] = nodeId;

  Serial.printf("New Connection, nodeId = %u\n", nodeId);
}

void delete_node(int nodeId, int arraySize) {
  // if a node disconnects form the mesh this functions will delete the nodeID from the Onlinenodes array.
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
  // If a connection has dropped, remove the node from OnlineNodes[].
  size_t currentNumberOfNodes = sizeof(onlineNodes)/sizeof(onlineNodes[0]);
  delete_node(nodeId, currentNumberOfNodes);
  Serial.printf("Node disconnected\n");
}

void sendLogsToServer() {
  // send logs to the server via a POST-request to the server API.
  if (nodeNumber == MASTER_NODE) {
    JSONVar sendLogs;
    for (int index_log = 0; index_log < aantal_logs; index_log++) {
      sendLogs[index_log]["nodeId"] = logs[index_log].node;
      sendLogs[index_log]["humidity"] = logs[index_log].hum;
      sendLogs[index_log]["pressure"] = logs[index_log].pres;
      sendLogs[index_log]["temperature"] = logs[index_log].temp;
      sendLogs[index_log]["logged_at"] = logs[index_log].logged_at;
    }
    WiFiClient client;
    HTTPClient http;
    send_logs = JSON.stringify(sendLogs);
    http.begin(client, "http://192.168.4.25:3000/api/logs");
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST(send_logs);
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    http.end();
    if (httpResponseCode != -1) {
      sendEmptyLogsMessage();
      memset(logs, 0, 500 * sizeof(logs));
      aantal_logs = 0;
    }
  }
}

void sendAlertToServer(int node, double hum, double temp, double pres, time_t logged_at) {
  // Send the alert-data with a POST-request to the server API.
  if (nodeNumber == MASTER_NODE) {
    JSONVar sendAlert;
    sendAlert["nodeId"] = node;
    sendAlert["humidity"] = hum;
    sendAlert["pressure"] = pres;
    sendAlert["temperature"] = temp;
    sendAlert["logged_at"] = logged_at;
    WiFiClient client;
    HTTPClient http;
    send_alert = JSON.stringify(sendAlert);
    http.begin(client, "http://192.168.4.25:3000/api/alert");
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST(send_alert);
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    http.end();
  }
}


void nodeTimeAdjustedCallback(int32_t offset) {
  // displays the time offset which synchronizes the nodes
  Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(),offset);
}

void setup() {
  Serial.begin(115200);
  // Set the time manually. This is done because of a lack of access to a NTP-server.
  rtc.setTime(0, 30, 11, 7, 2, 2023);
  // If the BME-sensor is not attached correctly, display an error and stop running.
  initBME();

  // Possible flags for debugging ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE
  mesh.setDebugMsgTypes( ERROR | STARTUP | DEBUG);  

  // Initialize wifi-mesh
  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );

  // Run receivedCallback with a new incoming message.
  mesh.onReceive(&receivedCallback);

  // Run newConnectionCallback when a new node joins the mesh.
  mesh.onNewConnection(&newConnectionCallback);
  
  // run onDroppedConnection when a node drops connection.
  mesh.onDroppedConnection(&onDroppedConnection);
  
  // run nodeTimeAdjustedCallback when the time needs synchronization. 
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  // Send a type 3 message after booting up to receive all logs from the masternode.
  userScheduler.addTask(sendMessage3Task);
  sendMessage3Task.enable();
  
  // Add tasks which are described al line 76.
  userScheduler.addTask(taskSendMessage);

  userScheduler.addTask(storeLocalSensorReadings);
  
  userScheduler.addTask(checkStatusTask);
  
  userScheduler.addTask(sendAliveTask);

  userScheduler.addTask(syncLogsWithRasperry);

  userScheduler.addTask(MastercountTask);


  // Enables the tasks that where created.
  taskSendMessage.enable();
  storeLocalSensorReadings.enable();
  checkStatusTask.enable();
  sendAliveTask.enable();
  syncLogsWithRasperry.enable();
  MastercountTask.enable();

  if (nodeNumber == AP_NODE) {
    // When the current node is chosen to be the accesspoint the node will start the access point.
    Serial.print("Setting soft-AP configuration ... ");
    Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ? "Ready" : "Failed!");

    Serial.print("Setting soft-AP ... ");
    Serial.println(WiFi.softAP(ssid, passphrase) ? "Ready" : "Failed!");

    Serial.print("Soft-AP IP address = ");
    Serial.println(WiFi.softAPIP());
  } else if (nodeNumber == MASTER_NODE) {
    // Masternode connects with de accesspoint.
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, passphrase);
  }
}

void loop() {
  // Continually update the mesh information.
  mesh.update();
}
