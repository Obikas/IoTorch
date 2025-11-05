
//Generic classes that need information to fully function
class ByteBuffer {}
class MessageID {}
class Status {}
class EventData {}
class StateData {}
class FaultData {}
class SensorThresholdData {}
class MonitoringConfig {}
class SensorData {}
class BulkSensorData {}
class SensorID {}
class SystemStatus {}
class ActuatorID {}
class ActuatorValue {}
class GPIOPin {}
class PowerRailID {}
enum PowerState { ON, OFF }
class ClientCommand {}

/*Log is used to keep track of the recorded history of what has happened
Can include health history, fault events, system events, etc. */
class Log {};

//Needs to have a clean set of instructions for valid or invalid changes
class HardwareConfigChange {}

class ParsedMessage {}

enum MessageType {}

class PLDMRequest {}

class PLDMResponse {}

enum PLDMType {}

class PLDMCommand {}


//List of interfaces needed for Figure 5 (Microcontroller Data Representation Layer)
interface PLDMInterfaces {
    
    
    interface IExternalCommunicator {
        
        //Recieve incoming data from ByteBuffer
        void receiveMessage(ByteBuffer data);
        
        //Validate the incoming data against expected values
        boolean validateMessage(ByteBuffer data);
        
        //Send message to other internal handlers
        void dispatchMessage(ByteBuffer data);
        
        //Send an ACK back 
        void sendAcknowledge(MessageID id, Status status);
    };

    public interface EventCommunicationAgent {

    // Queue event notifications
    //If an event fails we need to log it and send it to the retry system
    void enqueueEvent(EventData event);

    // Main processing loop to empty the queue
    void processEventQueue();

    // Retry logic for failed transmissions
    void retryFailedTransmissions();

    // Notify real-time state updates
    void notifyStateChange(StateData state);

    // Fault and threshold-specific messaging
    void reportFault(FaultData fault);
    void reportThresholdCrossing(SensorThresholdData threshold);

    // Configuration of timing, thresholds, monitoring behaviors
    void configureMonitoring(MonitoringConfig config);

    // Provides reliable sending in lossy/noisy network environments
    boolean reliableSend(ByteBuffer payload);
}

public interface ControlAndMonitoring {

    // Sensor and telemetry input
    SensorData readSensor(SensorID id);
    
    //Returns all sensor data back to us (Essentially a return broadcast)
    BulkSensorData readAllSensors();

    // System-level monitoring
    SystemStatus getSystemStatus();

    // Hardware control and actuation
    void setActuatorValue(ActuatorID id, ActuatorValue value);
    void toggleGPIO(GPIOPin pin, boolean enabled);
    void setPowerRail(PowerRailID id, PowerState state);

    // Handle commands originating externally (via PLDM server)
    void handleClientCommand(ClientCommand command);

    // Modify hardware or configuration
    void applyHardwareConfiguration(HardwareConfigChange change);

    //Clear out the logs within the system
    void clearSystemLogs();

    //Retrives the information within a given log
    Log getSystemLogs(Log inputLog);

}

public interface StructureParser {

    // Convert raw input into structured internal representation
    ParsedMessage parseMessage(ByteBuffer rawData);

    // Convert structured messages back to raw form
    ByteBuffer serializeMessage(ParsedMessage message);

    // Register handler for specific message types
    void registerStructureHandler(MessageType type, StructureHandler handler);

    // Capability query for message types
    boolean supportsMessageType(MessageType type);
}

public interface StructureHandler {
    ParsedMessage handle(ByteBuffer rawMessage);
}

public interface PLDMCapabilityHandler {

    // Identify which PLDM command family this handler supports
    PLDMType getCapabilityType();

    // Execute PLDM request and return the appropriate response
    PLDMResponse handleCommand(PLDMRequest request);

    // Optional: list supported command identifiers
    java.util.List<PLDMCommand> getSupportedCommands();
}



}
