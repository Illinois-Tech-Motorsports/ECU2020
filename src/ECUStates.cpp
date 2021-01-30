#include "ECUStates.hpp"
#include "ECUGlobalConfig.h"
#include "Faults.h"
#include "Log.h"

State::State_t *ECUStates::Initialize::run(void) {
#if CONF_ECU_POSITION == BACK_ECU
    Log.i(ID, "Teensy 3.6 SAE BACK ECU Initalizing");
#else
    Log.i(ID, "Teensy 3.6 SAE FRONT ECU Initalizing");
#endif
    Canbus::setup();    // allocate and organize addresses
    Pins::initialize(); // setup predefined pins
    Fault::setup();     // load all buffers
#if CONF_ECU_POSITION == FRONT_ECU
    Logging::enableCanbusRelay(); // Allow logging though canbus
#endif

    Pins::setPinValue(PINS_BOTH_OPT_CAN_TRAN, LOW); // optional CAN transceiver enable pin

    if (Fault::anyFault()) {
        return &ECUStates::FaultState;
    }

    Log.i(ID, "Waiting for TSV Signal");
    while (!Pins::getPinValue(PINS_BACK_TSV_SIGNAL)) {
    }
    Log.i(ID, "TSV On");

    Log.d(ID, "Finshed Setup");
    return &ECUStates::PreCharge_State;
};

State::State_t *ECUStates::PreCharge_State::PreCharFault(void) {
    Log.w(ID, "Closing Air1 and Precharge Relay");
    Pins::setPinValue(PINS_BACK_AIR1, LOW);
    Pins::setPinValue(PINS_BACK_PRECHARGE_RELAY, LOW);
    return &ECUStates::FaultState;
}

bool ECUStates::PreCharge_State::voltageCheck() {
    Canbus::setSemaphore(ADD_BMS_VOLT);
    uint BMSVolt = *((uint *)(BMS_Voltage_Buffer + 1)); // TODO: interpret buffer
    Canbus::setSemaphore(0x0000);
    uint MCVolt = *((uint *)(ADD_MC0_VOLT + 1)); // TODO: interpret buffer
    Canbus::clearSemaphore();
    return 0.9 * BMSVolt <= MCVolt;
}

void ECUStates::PreCharge_State::getBuffers() {
    BMS_Voltage_Buffer = Canbus::getBuffer(ADD_BMS_VOLT);
    MC0_Voltage_Buffer = Canbus::getBuffer(ADD_MC0_VOLT);
    MC1_Voltage_Buffer = Canbus::getBuffer(ADD_MC1_VOLT);
};

State::State_t *ECUStates::PreCharge_State::run(void) { // FIXME: set pins to LOW or HIGH?
    Log.i(ID, "Loading Buffers");
    getBuffers();
    Log.i(ID, "Precharge running");

    if (Fault::hardFault()) {
        Log.e(ID, "Precharge Faulted before charge");
        return PreCharFault();
    }

    Log.w(ID, "Opening Air1 and Precharge Relay");
    Pins::setPinValue(PINS_BACK_AIR1, HIGH);
    Pins::setPinValue(PINS_BACK_PRECHARGE_RELAY, HIGH);

    if (Fault::hardFault()) {
        Log.e(ID, "Precharge Faulted after charge");
        return PreCharFault();
    }

    elapsedMillis timeElapsed;

    while (timeElapsed < 5000) {
        if (voltageCheck()) {
            Pins::setPinValue(PINS_BACK_PRECHARGE_RELAY, LOW);
            Log.i(ID, "Precharge Finished");
            Pins::setPinValue(PINS_BACK_AIR2, HIGH);
            return &ECUStates::Idle_State;
        }
    }

    Log.e(ID, "Precharge timed out");
    return PreCharFault();
};

State::State_t *ECUStates::Idle_State::run(void) {
    Log.i(ID, "Waiting for Button or Charging Press");

    while (true) {
        if (Pins::getPinValue(PINS_FRONT_BUTTON_INPUT)) { // FIXME: Change to CanPin get
            Log.i(ID, "Button Pressed");
            return &ECUStates::Button_State;
        } else if (Pins::getPinValue(PINS_BACK_CHARGING_INPUT)) {
            Log.i(ID, "Charging Pressed");
            return &ECUStates::Charging_State;
        } else if (Fault::hardFault()) {
            break;
        }

        Log.d(ID, "Waiting");
    }

    return &ECUStates::FaultState;
}

State::State_t *ECUStates::Charging_State::run(void) {
    Pins::setPinValue(PINS_BACK_CHARGING_RELAY, HIGH);
    Log.i(ID, "Charging on");

    elapsedMillis voltLogNotify;

    while (Pins::getPinValue(PINS_BACK_CHARGING_SIGNAL)) {
        // IMPROVE: Don't use fault to stop charging
        if (Fault::hardFault()) {
            Pins::setPinValue(PINS_BACK_CHARGING_RELAY, LOW);
            Log.e(ID, "Charging faulted, turning off");
            return &ECUStates::FaultState;
        }
        if (voltLogNotify >= 1000) { // Notify every secondish
            voltLogNotify = 0;
            Log.i(ID, "Voltage", Pins::getPinValue(PINS_BACK_CHARGING_VOLTAGE));
        }
    }

    Pins::setPinValue(PINS_BACK_CHARGING_RELAY, LOW);
    Log.i(ID, "Charging turning off");

    return &ECUStates::Idle_State;
}

State::State_t *ECUStates::Button_State::run(void) {
    Log.i(ID, "Playing sound");

    // motor controller enable bit off
    Pins::setPinValue(PINS_BACK_SOUND_DRIVER, HIGH);

    elapsedMillis soundTimer;

    while (soundTimer <= 2000) {
        if (Fault::anyFault()) {
            Log.e(ID, "Failed to play sound");
            Pins::setPinValue(PINS_BACK_SOUND_DRIVER, LOW);
            return &ECUStates::FaultState;
        }
    }

    Pins::setPinValue(PINS_BACK_SOUND_DRIVER, LOW);
    Log.i(ID, "Playing sound finished");

    return &ECUStates::Driving_Mode_State;
}

void ECUStates::Driving_Mode_State::sendMCCommand(uint32_t MC_ADD, int torque, bool direction, bool enableBit) {
    int percentTorque = constrain(map(torque, 0, 1024, 0, 400), 0, 400); // separate func for negative vals (regen)
    Log.d(ID, "Torque percent: ", percentTorque);
    // Calculations value = (high_byte x 256) + low_byte
    uint8_t low_byte = percentTorque % 256;
    uint8_t high_byte = percentTorque / 256;
    Canbus::sendData(MC_ADD - 0x0A0, low_byte, high_byte, 0, 0, direction, enableBit); // TODO: is the const offset 0x0A0 okay?
}

void ECUStates::Driving_Mode_State::torqueVector(int torques[2]) {
    int pedal0 = Pins::getCanPinValue(PINS_FRONT_PEDAL0);
    int pedal1 = Pins::getCanPinValue(PINS_FRONT_PEDAL1);
    torques[0] = pedal0; // TODO: Add Torque vectoring algorithms
    torques[1] = pedal1;
}

uint32_t ECUStates::Driving_Mode_State::BMSSOC() {
    Canbus::setSemaphore(ADD_BMS_SOC);
    return *(uint *)(BMS_SOC_Buffer + 2); // TODO: interpret buffer
    Canbus::clearSemaphore();
}

uint32_t ECUStates::Driving_Mode_State::powerValue() {
    Canbus::setSemaphore(ADD_MC0_PWR);
    uint MC0_PWR = *(uint *)(MC0_PWR_Buffer + 4); // TODO: interpret buffer
    Canbus::setSemaphore(ADD_MC1_PWR);
    uint MC1_PWR = *(uint *)(MC1_PWR_Buffer + 4); // TODO: interpret buffer
    Canbus::clearSemaphore();
    return MC0_PWR + MC1_PWR;
}

void ECUStates::Driving_Mode_State::getBuffers() {
    MC0_RPM_Buffer = Canbus::getBuffer(ADD_MC0_RPM);
    MC1_RPM_Buffer = Canbus::getBuffer(ADD_MC1_RPM);
    MC0_PWR_Buffer = Canbus::getBuffer(ADD_MC0_PWR);
    MC1_PWR_Buffer = Canbus::getBuffer(ADD_MC1_PWR);
    BMS_SOC_Buffer = Canbus::getBuffer(ADD_BMS_SOC);
}

State::State_t *ECUStates::Driving_Mode_State::run(void) {
    Log.i(ID, "Loading Buffers");
    getBuffers();
    Log.i(ID, "Driving mode on");

#if CONF_ECU_POSITION == FRONT_ECU
    elapsedMillis timeElapsed;
#endif

    while (true) {
        Canbus::update();
#if CONF_ECU_POSITION == FRONT_ECU
        if (timeElapsed >= 5) { // Update Tablet every 5ms
            timeElapsed = 0;
            // receive rpm of MCs, interpret, then send to from teensy for logging
            Canbus::setSemaphore(ADD_MC0_RPM);
            int MC_Rpm_Val_0 = *(int *)(MC0_RPM_Buffer + 4); // TODO: interpret buffer
            Canbus::setSemaphore(ADD_MC1_RPM);
            int MC_Rpm_Val_1 = *(int *)(MC1_RPM_Buffer + 4); // TODO: interpret buffer
            Canbus::clearSemaphore();
            int MC_Spd_Val_0 = MC_Rpm_Val_0; // TODO: convert RPM -> SPD
            int MC_Spd_Val_1 = MC_Rpm_Val_1;
            int speed = (MC_Spd_Val_0 + MC_Spd_Val_1) / 2;
            Log.i(ID, "Current Motor Speed:", speed);
            Log.i(ID, "Current Power Value:", powerValue());   // Canbus message from MCs
            Log.i(ID, "BMS State Of Charge Value:", BMSSOC()); // Canbus message
        }
#else
        // TODO: Send fault reset to MCs
        Log.d(ID, "Sending Fault reset to MCs complete");

        for (size_t i = 0; i < 4; i++) {          // IMPROVE: Send only once? Check MC heartbeat catches it
            sendMCCommand(ADD_MC0_CTRL, 0, 0, 0); // MC 1
            sendMCCommand(ADD_MC1_CTRL, 0, 1, 0); // MC 2
            delay(10);
        }

        int MotorTorques[2];
        torqueVector(MotorTorques);

        sendMCCommand(ADD_MC0_CTRL, MotorTorques[0], 0, 0); // MC 1
        sendMCCommand(ADD_MC1_CTRL, MotorTorques[1], 1, 0); // MC 2

        if (Pins::getCanPinValue(PINS_FRONT_BUTTON_INPUT)) {
            Log.w(ID, "Going back to Idle state");
            break;
        }

        if (Fault::hardFault()) {
            //TODO: anything extra when a fault happens while driving?
            return &ECUStates::FaultState;
        }
#endif
    }

    Log.i(ID, "Driving mode off");
    return &ECUStates::Idle_State;
}

State::State_t *ECUStates::FaultState::run(void) { // IMPROVE: Should fault state do anything else?
    Canbus::enableInterrupts(false);
    Log.f(ID, "FAULT STATE");
    Fault::logFault();
    delay(250);
    return &ECUStates::FaultState;
}

State::State_t *ECUStates::Logger_t::run(void) {

    static elapsedMillis timeElapsed;

    if (timeElapsed >= 2000) {
        timeElapsed = timeElapsed - 2000;
        Log(ID, "A7 Pin Value:", Pins::getPinValue(0));
        Log("FAKE ID", "A7 Pin Value:");
        Log(ID, "whaAAAT?");
        Log(ID, "", 0xDEADBEEF);
        Log(ID, "Notify code: ", getNotify());
    }

    return &ECUStates::Bounce;
};

State::State_t *ECUStates::Bounce_t::run(void) {
    delay(250);
    Log.i(ID, "Bounce!");
    State::notify(random(100));
    delay(250);
    return State::getLastState();
}