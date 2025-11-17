#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------
//  Generic structures (empty placeholders)
//  Quick swap over to C. Not guaranteed to be perfect
// 
// -------------------------------------------

// Represents data that is being sent and passed around
typedef struct { uint8_t *buffer; size_t length; } ByteBuffer;

// ID number for a message.
typedef struct { int id; } MessageID;

// Current status
typedef struct { int code; } Status;

/*Represents data sent by events within the system.
May be used to help with keeping asynchronus processes synchronus
*/
typedef struct { int dummy; } EventData;

// Data about the current state of the system
typedef struct { int dummy; } StateData;

// Data about a fault occuring in the system
typedef struct { int dummy; } FaultData;

// Used to set the desired threshold levels
typedef struct { int dummy; } SensorThresholdData;

// Used to set the configurations for the monitors
typedef struct { int dummy; } MonitoringConfig;

// Represents data captured by a sensor
typedef struct { int dummy; } SensorData;

// Represents a collection of SensorData structs
typedef struct { int dummy; } BulkSensorData;

// ID of a given sensor (used for tracking)
typedef struct { int dummy; } SensorID;

// Current system status, used for monitoring and tracking
typedef struct { int dummy; } SystemStatus;

// ID of a given Actuator
typedef struct { int dummy; } ActuatorID;

// Represents the value of an actuator
typedef struct { int dummy; } ActuatorValue;

// Represents a GPIO Pin
typedef struct { int dummy; } GPIOPin;

// ID of a given powerrail
typedef struct { int dummy; } PowerRailID;

// Powerrail state
typedef enum { POWER_ON, POWER_OFF } PowerState;

// Used to send commands directly from the top level down into the controller
typedef struct { int dummy; } ClientCommand;

// Information within the Logs
typedef struct { int dummy; } Log;

// Used to define changes to the hardware or configurations
typedef struct { int dummy; } HardwareConfigChange;

//Represents 
typedef struct { int dummy; } ParsedMessage;
typedef struct { int dummy; } PLDMRequest;
typedef struct { int dummy; } PLDMResponse;
typedef struct { int dummy; } PLDMCommand;


//  Enum placeholders
typedef enum { MESSAGE_TYPE_UNKNOWN } MessageType;
typedef enum { PLDM_TYPE_UNKNOWN } PLDMType;


/* IExternalCommunicator
    PLDM server needs to be able to recieve communication from outside sources.
    Server needs to be able to access the control and monitoring system, and parse
    through information sent to (and from) those systems.
*/ 
typedef struct IExternalCommunicator {
    // Recieve incoming data from an external source
    void (*receiveMessage)(struct IExternalCommunicator *self, ByteBuffer *data);
    
    // Check the data to ensure that it is valid
    bool (*validateMessage)(struct IExternalCommunicator *self, ByteBuffer *data);
    
    // Send messages to other internal handlers
    void (*dispatchMessage)(struct IExternalCommunicator *self, ByteBuffer *data);
    
    // Send an ACK back
    void (*sendAcknowledge)(struct IExternalCommunicator *self, MessageID *id, Status *status);
} IExternalCommunicator;

/* EventCommunicationAgent
    Needs to have methods for handling event notifications, state changes
    fault conditions, and sensor threshold crossings. Needs to have a queue for 
    handling asynchronus information. Needs to be able to update information in 
    real-time, and monitor internal systems to match given specifications. Needs 
    capabilities for retries, and data transfer in lossy or noisy enviroments.
*/
typedef struct EventCommunicationAgent {
    // Add an event to the queue to be processed
    void (*enqueueEvent)(struct EventCommunicationAgent *self, EventData *event);
    
    // Process an event that exists on the queue
    void (*processEventQueue)(struct EventCommunicationAgent *self);
    
    // Retries any transmissions that are unsuccessful
    void (*retryFailedTransmissions)(struct EventCommunicationAgent *self);
    
    // Send out a state change notification to other systems (or self for asynchronus processes)
    void (*notifyStateChange)(struct EventCommunicationAgent *self, StateData *state);
    
    // Send out a fault report 
    void (*reportFault)(struct EventCommunicationAgent *self, FaultData *fault);
    
    // Throw out an alert if a threshold is crossed.
    void (*reportThresholdCrossing)(struct EventCommunicationAgent *self, SensorThresholdData *threshold);
    
    // Changes configuration monitoring to set new timings, thresholds, and monitored behaviors
    void (*configureMonitoring)(struct EventCommunicationAgent *self, MonitoringConfig *config);
    
    // Provides reliable sending in noisy or lossy enviroments.
    bool (*reliableSend)(struct EventCommunicationAgent *self, ByteBuffer *payload);
} EventCommunicationAgent;

/* ControlAndMonitoring
    Needs to have a function to collect sensor data, monitor system statuses,
    send information to GPIO's, actuators, and power rails. Needs to have functionality
    to recieve information from the client (via the server) to work with it. Needs to have
    functionality to modify the hardware.
*/
typedef struct ControlAndMonitoring {
    // Returns a specific set of sensor data to us 
    SensorData (*readSensor)(struct ControlAndMonitoring *self, SensorID *id);
    
    // Returns all sensor data back to us
    BulkSensorData (*readAllSensors)(struct ControlAndMonitoring *self);
    
    // Returns the system-level monitoring
    SystemStatus (*getSystemStatus)(struct ControlAndMonitoring *self);
    
    // Handles setting the actuator value 
    void (*setActuatorValue)(struct ControlAndMonitoring *self, ActuatorID *id, ActuatorValue *value);
StructureParser    
    // Handles GPIO control
    void (*toggleGPIO)(struct ControlAndMonitoring *self, GPIOPin *pin, bool enabled);
    
    // Handles hardware control for a power rail
    void (*setPowerRail)(struct ControlAndMonitoring *self, PowerRailID *id, PowerState state);
    
    // Handles commands that come in from the client
    void (*handleClientCommand)(struct ControlAndMonitoring *self, ClientCommand *command);
    
    // Allows for control for configurations to the hardware
    void (*applyHardwareConfiguration)(struct ControlAndMonitoring *self, HardwareConfigChange *change);
    
    // Clear out all the logs within the system
    void (*clearSystemLogs)(struct ControlAndMonitoring *self);
    
    // Get the logs within the system
    Log (*getSystemLogs)(struct ControlAndMonitoring *self, Log *inputLog);
} ControlAndMonitoring;

/* StructureHandler
    System needs to be able to handle different structure types and break down
    raw messages.
*/
typedef struct StructureHandler {
    ParsedMessage (*handle)(struct StructureHandler *self, ByteBuffer *rawMessage);
} StructureHandler;

/* StructureParser
    System needs to be able to handle different structure types and break down and
    reform the messages into understandable formats.
*/
typedef struct StructureParser {
    // Convert raw input into structured internal representation
    ParsedMessage (*parseMessage)(struct StructureParser *self, ByteBuffer *rawData);
    
    // Convert structured messages back to raw form
    ByteBuffer (*serializeMessage)(struct StructureParser *self, ParsedMessage *message);
    
    // Register handler for specific message types
    void (*registerStructureHandler)(struct StructureParser *self, MessageType type, StructureHandler *handler);
    
    // Capability query for message types
    bool (*supportsMessageType)(struct StructureParser *self, MessageType type);
} StructureParser;

// PLDMCapabilityHandler
typedef struct PLDMCapabilityHandler {
    // Gets the type of the PLDM command for handling support
    PLDMType (*getCapabilityType)(struct PLDMCapabilityHandler *self);
   
    // Executes a PLDM request and returns the appropriate response
    PLDMResponse (*handleCommand)(struct PLDMCapabilityHandler *self, PLDMRequest *request);
    
    //  Get a list of supported command identifiers
    PLDMCommand *(*getSupportedCommands)(struct PLDMCapabilityHandler *self, size_t *count);
} PLDMCapabilityHandler;

