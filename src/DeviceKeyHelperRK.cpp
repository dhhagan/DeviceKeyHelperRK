/**
 * Particle library for saving and restoring device private and public keys
 *
 * Location: https://github.com/rickkas7/DeviceKeyHelperRK
 * License: MIT
 */

#include "DeviceKeyHelperRK.h"

static Logger _log("app.devicekeys");

DeviceKeyHelper *DeviceKeyHelper::instance;


DeviceKeyHelper::DeviceKeyHelper(std::function<bool(DeviceKeyHelperSavedData *savedData)> load, std::function<bool(const DeviceKeyHelperSavedData *savedData)> save) :
	load(load), save(save) {
	instance = this;
}

DeviceKeyHelper::~DeviceKeyHelper() {

}

void DeviceKeyHelper::startMonitor() {
	System.on(cloud_status, eventHandlerStatic);
}


bool DeviceKeyHelper::check(CheckMode checkMode) {
	// On TCP devices, this is 1600 bytes so it's kind of large to allocate on the stack safely, plus we need two of them.
	// We deallocate it before exiting this function.
	bool result = true;

	uint8_t *onDevice = new uint8_t[DEVICE_KEYS_HELPER_SIZE];
	if (onDevice) {
		DeviceKeyHelperSavedData *saved = new DeviceKeyHelperSavedData();
		if (saved) {
			//
			dct_read_app_data_copy(DEVICE_KEYS_HELPER_OFFSET, onDevice, DEVICE_KEYS_HELPER_SIZE);

			bool saveKeys = false;

			if (load(saved)) {
				// We were able to load some data, but make sure it's valid
				if (validateData(saved)) {
					// Looks valid
					if (checkMode == CHECKMODE_SAVE_CURRENT) {
						_log.trace("force save device keys");
						saveKeys = true;
					}
					else
					if (memcmp(onDevice, saved->keys, DEVICE_KEYS_HELPER_SIZE) != 0) {
						// Changed
						if (checkMode != CHECKMODE_CHECK_ONLY) {
							int res = dct_write_app_data(saved->keys, DEVICE_KEYS_HELPER_OFFSET, DEVICE_KEYS_HELPER_SIZE);

							_log.info("device keys changed! reverting offset=%u size=%u result=%d", DEVICE_KEYS_HELPER_OFFSET, DEVICE_KEYS_HELPER_SIZE, res);

							if (checkMode != CHECKMODE_AUTOMATIC_NO_RESTART) {
								System.reset();
							}
						}
						else {
							_log.info("device keys changed");
						}
						result = false;
					}
					else {
						// Same
						_log.info("device keys unchanged");
					}
				}
				else {
					// Not valid, just save the current keys
					_log.info("was able to load device keys, but data was not valid");
					saveKeys = true;
				}

			}
			else {
				// Did not successfully load, so save the current key instead
				_log.info("was unable to load existing key data");
				saveKeys = true;
			}

			if (saveKeys) {
				if (saved->magic == DATA_HEADER_MAGIC &&
					saved->size == DEVICE_KEYS_HELPER_SIZE &&
					saved->sum == calculateChecksum(saved) &&
					memcmp(saved->keys, onDevice, DEVICE_KEYS_HELPER_SIZE) == 0) {
					//
					_log.trace("keys unchanged, no need to save");
					saveKeys = false;
				}
			}

			if (saveKeys) {
				// Save a header (with magic bytes, length, and checksum)
				_log.info("saving keys");
				memcpy(saved->keys, onDevice, DEVICE_KEYS_HELPER_SIZE);

				saved->magic = DATA_HEADER_MAGIC;
				saved->size = DEVICE_KEYS_HELPER_SIZE;
				saved->sum = calculateChecksum(saved);

				save(saved);
			}

			delete saved;
		}

		delete[] onDevice;
	}
	return result;
}


uint16_t DeviceKeyHelper::calculateChecksum(const DeviceKeyHelperSavedData *savedData) const {
	uint16_t sum = 0;

	for(size_t ii = 0; ii < DEVICE_KEYS_HELPER_SIZE; ii++) {
		sum += savedData->keys[ii];
	}
	return sum;
}

bool DeviceKeyHelper::validateData(const DeviceKeyHelperSavedData *savedData) const {

	if (savedData->magic != DATA_HEADER_MAGIC || savedData->size != DEVICE_KEYS_HELPER_SIZE) {
		_log.info("bad magic bytes or size magic=%08lx size=%u", savedData->magic, savedData->size);
		return false;
	}

	if (savedData->sum != calculateChecksum(savedData)) {
		_log.info("bad checksum");
		return false;
	}
	return true;
}

void DeviceKeyHelper::eventHandler(system_event_t event, int param) {
	if (event == cloud_status) {
		if (param == cloud_status_connecting) {
			_log.trace("cloud_status_connecting");
			connected = false;
		}
		else
		if (param == cloud_status_connected) {
			_log.trace("cloud_status_connected");

			connected = true;
			failureCount = 0;
			check(CheckMode::CHECKMODE_SAVE_CURRENT);
		}
		else
		if (param == cloud_status_disconnected) {
			_log.trace("cloud_status_disconnected");

			if (!connected) {
#if SYSTEM_VERSION >= 0x00080000
				int32_t value;
				if (getSystemDiagValue(DIAG_ID_CLOUD_CONNECTION_ERROR_CODE, value)) {
					_log.trace("DIAG_ID_CLOUD_CONNECTION_ERROR_CODE=%ld", value);

					if (value == 26 || value == 10) {
						// Keys error. It's 26 on TCP devices and 10 on UDP devices.
						_log.warn("keys error, resetting keys if possible");
						Particle.disconnect();
						check(CheckMode::CHECKMODE_AUTOMATIC);

						// Normally this line won't be reached because check does a restart if keys
						// are restored, but this is here in case the restore fails, so the device
						// won't be left in disconnected state.
						System.reset();
					}
				}
#else
				failureCount++;
				_log.info("failed to connect %d", failureCount);

				if (failureCount >= 3) {
					// If this happens more than 3 times, assume we have a keys error
					_log.warn("possible keys error, resetting keys if possible");
					Particle.disconnect();
					check(CheckMode::CHECKMODE_AUTOMATIC);

					// Normally this line won't be reached because check does a restart if keys
					// are restored, but this is here in case the restore fails, so the device
					// won't be left in disconnected state.
					System.reset();
				}
#endif

			}
		}

	}

}


// [static]
void DeviceKeyHelper::eventHandlerStatic(system_event_t event, int param) {
	getInstance()->eventHandler(event, param);
}

// [static]
bool DeviceKeyHelper::getSystemDiagValue(uint16_t id, int32_t &value) {
#if SYSTEM_VERSION >= 0x00080000
    // Only available in system firmware 0.8.0 and later!
    union Data {
        struct __attribute__((packed)) {
            uint16_t idSize;
            uint16_t valueSize;
            uint16_t id;
            int32_t     value;
            size_t offset;
        } d;
        uint8_t b[10];
    };
    Data data;
    data.d.offset = data.d.value = 0;

    struct {
        static bool appender(void* appender, const uint8_t* data, size_t size) {
            Data *d = (Data *)appender;
            if ((d->d.offset + size) <= sizeof(Data::b)) {
                memcpy(&d->b[d->d.offset], data, size);
                d->d.offset += size;
            }
            return true;
        }
    } Callback;

    system_format_diag_data(&id, 1, 1, Callback.appender, &data, nullptr);

    // _log.info("idSize=%u valueSize=%u id=%u value=%ld", data.d.idSize, data.d.valueSize, data.d.id, data.d.value);

    value = data.d.value;

    return (data.d.offset == sizeof(Data::b));
#else
    value = 0;
    return false;
#endif
}

