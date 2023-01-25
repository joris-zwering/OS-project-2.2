#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "painlessMesh.h"
#include <Arduino_JSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include <time.h>

// MESH Details
#define   MESH_PREFIX     "ESP_NET_WERK_MESH" // Naam van mesh
#define   MESH_PASSWORD   "OmdatHetNetWerkt" // Wachtwoord van mesh
#define   MESH_PORT       5555  

//BME object on the default I2C pins
Adafruit_BME280 bme;

// Identifier voor deze node
int nodeNumber = 1;

// Online nodes
int onlineNodes[] = {};

// String to send to other nodes with sensor readings
String readings;

Scheduler userScheduler; 
painlessMesh  mesh;

// PROTOTYPES
void sendSensorData();
void storeLocalSensorData();
void onDroppedConnection(unsigned int nodeId );
void checkStatus();
String getReadings(); 

//Create tasks: to send messages and get readings;
Task taskSendMessage(TASK_SECOND * 10 , TASK_FOREVER, &sendSensorData);

//Create tasks: to store local data readings (every second)
Task storeLocalSensorReadings(TASK_SECOND * 1 , TASK_FOREVER, &sendSensorData);

//Create tasks: to check for temperature and humidity changes
Task checkStatusTask(TASK_SECOND * 1 , TASK_FOREVER, &checkStatus);

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

String getReadings () {
  JSONVar jsonReadings;
  jsonReadings["type"] = 1;
  jsonReadings["node"] = nodeNumber;
  jsonReadings["temp"] = bme.readTemperature();
  jsonReadings["hum"] = bme.readHumidity();
  jsonReadings["pres"] = bme.readPressure()/100.0F;
  time_t current_time;
  time(&current_time);
  jsonReadings["logged_at"] = current_time;
  readings = JSON.stringify(jsonReadings);
  return readings;
}

void sendSensorData () {
  // Lees sensorwaarde
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());
  // Sla sensorwaarden op in logs
  int aantal_logs = sizeof(localData) / sizeof(LocalLog);
  logs[aantal_logs + 1].node = messageObject["node"];
  logs[aantal_logs + 1].temp = messageObject["temp"];
  logs[aantal_logs + 1].hum = messageObject["hum"];
  logs[aantal_logs + 1].pres = messageObject["pres"];
  logs[aantal_logs + 1].logged_at = messageObject["logged_at"];
  // Broadcast = naar alle andere nodes inclusief deze node
  mesh.sendBroadcast(msg);
}

void storeLocalData() {
  // Lees sensorwaarde
  String msg = getReadings();
  JSONVar messageObject = JSON.parse(msg.c_str());

  // Checken hoe groot de huidige array is
  // alternatief: aantal_logs = _countof(localData);
  int aantal_logs = sizeof(localData) / sizeof(LocalLog);
  if (aantal_logs >= 10) {
    for (int i = aantal_logs - 1; i < 0; i--) {
      localData[i] = localData[i+1];
    }
    localData[0].temp = messageObject['temp'];
    localData[0].hum = messageObject['hum'];
    localData[0].logged_at = messageObject['logged_at'];
  }
  else {
    localData[aantal_logs + 1].temp = messageObject['temp'];
    localData[aantal_logs + 1].hum = messageObject['hum'];
    localData[aantal_logs + 1].logged_at = messageObject['logged_at'];
  }
}

void checkStatus() {
  int aantal_logs = sizeof(localData) / sizeof(LocalLog);
  int temp_sum = 0;
  int hum_sum = 0;
  for (int i = 1; i < aantal_logs; i++) {
    temp_sum += localData[i].temp;
  }
  for (int i = 1; i < aantal_logs; i++) {
    hum_sum += localData[i].hum;
  }
  int average_hum = hum_sum / (aantal_logs - 1);
  int current_hum = localData[0].hum;
  int average_temp = temp_sum / (aantal_logs - 1);
  int current_temp = localData[0].temp;
  if (((current_temp / average_temp) < 0.8) && ((current_hum / average_hum) < 0.8)) {
    // ALERT!!!
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
  // 3 = Synchroniseert alle logs (elke 4 min) 

  if (type == 1) {
    // SYNC SENSOR DATA
    int node = messageObject["node"];
    double temp = messageObject["temp"];
    double hum = messageObject["hum"];
    double pres = messageObject["pres"];
    time_t logged_at = messageObject["logged_at"];

    size_t currentNumberOfLogs = sizeof(logs)/sizeof(logs[0]);
    logs[currentNumberOfLogs + 1].node = node;
    logs[currentNumberOfLogs + 1].temp = temp;
    logs[currentNumberOfLogs + 1].hum = hum;
    logs[currentNumberOfLogs + 1].pres = pres;
    logs[currentNumberOfLogs + 1].logged_at = logged_at;
  } else if (type == 2) {
    // FLUSH LOGS
    free(logs);
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
  if (nodeNumber == 1) {
    // Loop trough every item in array
      // Inside for loop: make POST request to raspberry pi with every log entry
      // Remove log out of logs
      sendEmptyLogsMessage();
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

  // Wordt elke 10 seconden uitgevoerd
  userScheduler.addTask(taskSendMessage);

  userScheduler.addTask(storeLocalSensorReadings);
  
  userScheduler.addTask(checkStatusTask);

  taskSendMessage.enable();
  storeLocalSensorReadings.enable();
  checkStatusTask.enable();
}

void loop() {
  mesh.update();
}
