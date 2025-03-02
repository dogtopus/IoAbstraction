/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include <inttypes.h>
#include "SwitchInput.h"

#define ONE_TURN_OF_ENCODER 32

SwitchInput switches;

void registerInterrupt(pinid_t pin);

KeyboardItem::KeyboardItem() {
	this->repeatInterval = NO_REPEAT;
	this->pin = -1;
	this->notify.callback = nullptr;
	this->counter = 0;
	this->stateFlags = NOT_PRESSED;
	this->previousState = NOT_PRESSED;
	this->callbackOnRelease = nullptr;
	this->acceleration = 0;
}

KeyboardItem::KeyboardItem(pinid_t pin, KeyCallbackFn callback, uint8_t repeatInterval, bool keyLogicIsInverted) {
    this->repeatInterval = repeatInterval;
    this->pin = pin;
	this->notify.callback = callback;
	this->counter = 0;
	previousState = NOT_PRESSED;
	stateFlags = NOT_PRESSED;
	callbackOnRelease = nullptr;
	acceleration = 0;
	bitWrite(stateFlags, KEY_LISTENER_MODE_BIT, 0);
	bitWrite(stateFlags, KEY_LOGIC_IS_INVERTED, keyLogicIsInverted);
}

KeyboardItem::KeyboardItem(pinid_t pin, SwitchListener* switchListener, uint8_t repeatInterval, bool keyLogicIsInverted) {
	this->pin = pin;
	this->repeatInterval = repeatInterval;
    this->notify.listener = switchListener;
    this->counter = 0;
    previousState = NOT_PRESSED;
	stateFlags = NOT_PRESSED;
	callbackOnRelease = nullptr;
	acceleration = 0;
	bitWrite(stateFlags, KEY_LISTENER_MODE_BIT, 1);
	bitWrite(stateFlags, KEY_LOGIC_IS_INVERTED, keyLogicIsInverted);
}

KeyboardItem::KeyboardItem(const KeyboardItem& other) {
    this->pin = other.pin;
    this->repeatInterval = other.repeatInterval;
    this->counter = other.counter;
    this->notify.listener = other.notify.listener;
    this->previousState = other.previousState;
    this->stateFlags = other.stateFlags;
    this->callbackOnRelease = other.callbackOnRelease;
    this->acceleration = other.acceleration;
}

KeyboardItem& KeyboardItem::operator=(const KeyboardItem& other) {
    if(this == &other) return *this;
    this->pin = other.pin;
    this->repeatInterval = other.repeatInterval;
    this->counter = other.counter;
    this->notify.listener = other.notify.listener;
    this->previousState = other.previousState;
    this->stateFlags = other.stateFlags;
    this->callbackOnRelease = other.callbackOnRelease;
    this->acceleration = other.acceleration;
    return *this;
}

void KeyboardItem::onRelease(KeyCallbackFn callbackOnRelease) {
	this->callbackOnRelease = callbackOnRelease;
}

void KeyboardItem::trigger(bool held) {
	if (!notify.callback) return;

	if (isUsingListener()) notify.listener->onPressed(pin, held);
	else notify.callback(pin, held);
}

void KeyboardItem::triggerRelease(bool held) {
	if (isUsingListener()) notify.listener->onReleased(pin, held);
	else if(callbackOnRelease) callbackOnRelease(pin, held);
}

void KeyboardItem::checkAndTrigger(uint8_t buttonState){
	if (notify.callback == nullptr && callbackOnRelease == nullptr) return;

	if (buttonState == HIGH) {
		if (getState() == NOT_PRESSED) {
			setState(DEBOUNCING1);
		}
		else if (isDebouncing()) {
			setState(PRESSED);
			previousState = PRESSED;
			counter = 0; 
			acceleration = 1;
			trigger(false);
				
		}
		else if (getState() == PRESSED) {
			counter++;
			if (counter > HOLD_THRESHOLD) {
				setState(BUTTON_HELD);
				previousState = BUTTON_HELD;
				trigger(true);
				counter = 0;
				acceleration = 1;
			}
		}
		else if (getState() == BUTTON_HELD && repeatInterval != NO_REPEAT && notify.callback != NULL) {
			counter = counter + (acceleration >> 2) + 1;
			if (counter > repeatInterval) {
				acceleration = min(255, acceleration + 1);
				trigger(true);
				counter = 0;
			}
		}
	}
	else if(getState() == DEBOUNCING1) {
		setState(DEBOUNCING2);
	}
	else {
		setState(NOT_PRESSED);
		if (previousState == PRESSED) {
			previousState = NOT_PRESSED;
			triggerRelease(false);
		} else if (previousState == BUTTON_HELD){
			previousState = NOT_PRESSED;
			triggerRelease(true);
		}
	}
}

SwitchInput::SwitchInput() {
	this->ioDevice = nullptr;
	this->swFlags = 0;
    this->lastSyncStatus = true;
	for (int i = 0; i < MAX_ROTARY_ENCODERS; ++i) {
		encoder[i] = nullptr;
	}
}

void SwitchInput::initialiseInterrupt(IoAbstractionRef ioDevice, bool usePullUpSwitching) {
	this->ioDevice = ioDevice;
	this->swFlags = 0;
	bitWrite(swFlags, SW_FLAG_PULLUP_LOGIC, usePullUpSwitching);
	bitSet(swFlags, SW_FLAG_INTERRUPT_DRIVEN);

	// do not start any tasks here, we need to register interrupt on the pins instead.
}

void SwitchInput::initialise(IoAbstractionRef ioDevice, bool usePullUpSwitching) {
	this->ioDevice = ioDevice;
	this->swFlags = 0;
    	bitWrite(swFlags, SW_FLAG_PULLUP_LOGIC, usePullUpSwitching);

	taskManager.scheduleFixedRate(SWITCH_POLL_INTERVAL, [] {
		switches.runLoop();
	});

}

bool SwitchInput::addSwitch(pinid_t pin, KeyCallbackFn callback,uint8_t repeat, bool invertLogic) {
	if(internalAddSwitch(pin, invertLogic)) {
        KeyboardItem item(pin, callback, repeat, invertLogic);
        return keys.add(item);
    }
    
    return false;
}

bool SwitchInput::addSwitchListener(pinid_t pin, SwitchListener* listener, uint8_t repeat, bool invertLogic) {
	if(internalAddSwitch(pin, invertLogic)) {
        KeyboardItem item(pin, listener, repeat, invertLogic);
        return keys.add(item);
    }
    
    return false;
}

bool SwitchInput::internalAddSwitch(pinid_t pin, bool invertLogic) {
	if (ioDevice == nullptr) initialise(internalDigitalIo(), true);

	ioDevicePinMode(ioDevice, pin, isPullupLogic(invertLogic) ? INPUT_PULLUP : INPUT);

    if (isInterruptDriven()) {
		registerInterrupt(pin);
	}

    return true;
}

void SwitchInput::onRelease(pinid_t pin, KeyCallbackFn callbackOnRelease) {
	if (ioDevice == nullptr) initialise(internalDigitalIo(), true);

	auto keyItem = keys.getByKey(pin);
	if(pin) {
	    // already initialised, just add the release callback
	    keyItem->onRelease(callbackOnRelease);
	}
	else if(internalAddSwitch(pin, false)) {
	    // not yet added, we will do a best efforts standard initialisation.
        KeyboardItem newItem(pin, (KeyCallbackFn) nullptr, NO_REPEAT, false);
        newItem.onRelease(callbackOnRelease);
        keys.add(newItem);
    }
}

bool SwitchInput::isSwitchPressed(pinid_t pin) {
    return keys.getByKey(pin)->isPressed();
}

void SwitchInput::pushSwitch(pinid_t pin, bool held) {
    keys.getByKey(pin)->trigger(held);
}

void SwitchInput::changeEncoderPrecision(uint8_t slot, uint16_t precision, uint16_t currentValue, bool rollover) {
	if (slot < MAX_ROTARY_ENCODERS && encoder[slot] != nullptr) {
		encoder[slot]->changePrecision(precision, currentValue, rollover);
	}
}

void SwitchInput::setEncoder(uint8_t slot, RotaryEncoder* encoder) {
	if (slot < MAX_ROTARY_ENCODERS) {
		this->encoder[slot] = encoder;
	}
}

bool SwitchInput::runLoop() {
	bool needAnotherGo = false;

	lastSyncStatus = ioDeviceSync(ioDevice);

	for (bsize_t i = 0; i < keys.count(); ++i) {
		// get the pins current state
		auto key = keys.itemAtIndex(i);
		uint8_t pinState = ioDeviceDigitalRead(ioDevice, key->getPin());
		if(isPullupLogic(key->isLogicInverted())) {
			pinState = !pinState;
		}
		// and pass to the key handler.
		key->checkAndTrigger(pinState);

		// we need to call into here again if we are debouncing or anything is pressed.
		needAnotherGo |= (key->isDebouncing() || key->isPressed());
	}

	return needAnotherGo;
}


/******ROTARY ENCODERS *****/


RotaryEncoder::RotaryEncoder(EncoderCallbackFn callback) {
	this->callback = callback;
	this->currentReading = 0;
	this->maximumValue = 0;
    this->lastSyncStatus = true;
    this->rollover = false;
    this->intent = CHANGE_VALUE;
}

void RotaryEncoder::changePrecision(uint16_t maxValue, int currentValue, bool rolloverOnMax) {
	this->maximumValue = maxValue;
	this->currentReading = currentValue;
	this->rollover = rolloverOnMax;
	if(maxValue == 0 && currentValue == 0U) intent = DIRECTION_ONLY;
	callback(currentReading);
}

void RotaryEncoder::setUserIntention(EncoderUserIntention intention) {
    intent = intention;
    if(intention == DIRECTION_ONLY) {
        maximumValue = 0;
        currentReading = 0;
    }
}

// this abs accounts for some boards where abs is a double precision function
#define safeAbs(x) ((x) < 0 ? -(x) : (x))

void RotaryEncoder::increment(int8_t incVal) {
    if(maximumValue == 0) {
		// first check if we are in direction only mode (max = 0)
		 callback(incVal);
         return;
	}

    if(incVal >= 0) {
        if(rollover) {
			currentReading = (currentReading + incVal);
			if (currentReading > maximumValue) currentReading = currentReading - maximumValue - 1;
        }
		else {
			currentReading = min((uint16_t)(currentReading + incVal), maximumValue);
		}
	}
	else if(currentReading < abs(incVal)) {
		currentReading = rollover? maximumValue - safeAbs(incVal) + 1 : 0;
	}
	else if(currentReading != 0) {
		currentReading += incVal;
    }
	callback(currentReading);
}

HardwareRotaryEncoder::HardwareRotaryEncoder(pinid_t pinA, pinid_t pinB, EncoderCallbackFn callback, HWAccelerationMode accelerationMode, EncoderType encoderType) : RotaryEncoder(callback) {
	this->pinA = pinA;
	this->pinB = pinB;
	this->lastChange = micros();
    this->accelerationMode = accelerationMode;
	this->encoderType = encoderType;

	// set the pin directions to input with pull ups enabled
	ioDevicePinMode(switches.getIoAbstraction(), pinA, INPUT_PULLUP);
	ioDevicePinMode(switches.getIoAbstraction(), pinB, INPUT_PULLUP);

	// read back the initial values.
	lastSyncStatus = ioDeviceSync(switches.getIoAbstraction());	
	this->aLast = ioDeviceDigitalRead(switches.getIoAbstraction(), pinA);
	this->cleanFromB = ioDeviceDigitalRead(switches.getIoAbstraction(), pinB);

	registerInterrupt(pinA);
}

void checkRunLoopAndRepeat() {
	// turn off interrupts until deboucing / repeat logic is complete.
	switches.setInterruptDebouncing(true);

	// instead of running constantly, we only run when there's a need to, eg something
	// is still in a debouncing state. Otherwise we wait for an interrupt.
	// switches.runLoop returns true when it needs to run again.
	if(switches.runLoop()) {
		taskManager.scheduleOnce(20, [] { 
			checkRunLoopAndRepeat();
		});
	}
	else {
		// back to normal now - interrupt only
		switches.setInterruptDebouncing(false);
	}
}

void onSwitchesInterrupt(__attribute__((unused)) pinid_t pin) {
	if(switches.isInterruptDriven() && !switches.isInterruptDebouncing()) {
		checkRunLoopAndRepeat();
	}

  for(int i = 0; i < MAX_ROTARY_ENCODERS; ++i) {
		if(switches.encoder[i]) {
			switches.encoder[i]->encoderChanged();
		}
	}
}

int HardwareRotaryEncoder::amountFromChange(unsigned long change) {
	if(change > 250000 || maximumValue < ONE_TURN_OF_ENCODER) return 1;

    if(accelerationMode == HWACCEL_NONE) {
        return 1;
    }
    else if(accelerationMode == HWACCEL_REGULAR) {
        if(change > 120000) return 2;
        else if (change > 70000) return 4;
        else if (change > 30000) return 6;
        else return 10;
    }
    else { // slower, very slight acceleration..
        
        if(change > 100000) return 2;
        else if (change > 30000) return 3;
        else return 4;
    }

}

void HardwareRotaryEncoder::encoderChanged() {
	lastSyncStatus = ioDeviceSync(switches.getIoAbstraction());
	uint8_t a = ioDeviceDigitalRead(switches.getIoAbstraction(), pinA);
	uint8_t b = ioDeviceDigitalRead(switches.getIoAbstraction(), pinB);

	if(encoderType == QUARTER_CYCLE){
		if((a != aLast) || (b != cleanFromB)) {
			aLast = a;
			if((a != aLast) || (b != cleanFromB)) {
				cleanFromB = b;
				if((a || cleanFromB) || (a == 0 && b == 0)) {	
					unsigned long timeNow = micros();
					int amt = amountFromChange(timeNow - lastChange);
					increment(a != b ? -amt : amt);
					lastChange = timeNow;
				}
			}
		}		
	}
	else {
		if(a != aLast) {
			aLast = a;
			if(b != cleanFromB) {
				cleanFromB = b;
				if(a) {	
					unsigned long timeNow = micros();
					int amt = amountFromChange(timeNow - lastChange);
					increment(a != b ? -amt : amt);
					lastChange = timeNow;
				}
			}
		}	
	}

}



/******** UP DOWN BUTTON ENCODER *******/

void switchEncoderUp(__attribute((unused)) pinid_t key, __attribute((unused)) bool heldDown) {
    int dir = (switches.getEncoder()->getUserIntention() == SCROLL_THROUGH_ITEMS) ? -1 : 1;
	switches.getEncoder()->increment(dir);
}

void switchEncoderDown(__attribute((unused)) pinid_t key, __attribute((unused)) bool heldDown) {
    int dir = (switches.getEncoder()->getUserIntention() == SCROLL_THROUGH_ITEMS) ? 1 : -1;
	switches.getEncoder()->increment(dir);
}

EncoderUpDownButtons::EncoderUpDownButtons(pinid_t pinUp, pinid_t pinDown, EncoderCallbackFn callback, uint8_t speed) : RotaryEncoder(callback) {
	switches.addSwitch(pinUp, switchEncoderUp, speed);
	switches.addSwitch(pinDown, switchEncoderDown, speed);
}

/******** ENCODER SETUP METHODS ***********/

void setupUpDownButtonEncoder(pinid_t pinUp, pinid_t pinDown, EncoderCallbackFn callback) {
	if (switches.getIoAbstraction() == nullptr) switches.initialise(internalDigitalIo(), true);

	EncoderUpDownButtons* enc = new EncoderUpDownButtons(pinUp, pinDown, callback);
	switches.setEncoder(enc);
}

void registerInterrupt(pinid_t pin) {
	taskManager.setInterruptCallback(onSwitchesInterrupt);
	taskManager.addInterrupt(switches.getIoAbstraction(), pin, CHANGE);
}

void setupRotaryEncoderWithInterrupt(pinid_t pinA, pinid_t pinB, EncoderCallbackFn callback, HWAccelerationMode accelerationMode, EncoderType encoderType) {
	if (switches.getIoAbstraction() == nullptr) switches.initialise(internalDigitalIo(), true);

	switches.setEncoder(new HardwareRotaryEncoder(pinA, pinB, callback, accelerationMode, encoderType));
}
