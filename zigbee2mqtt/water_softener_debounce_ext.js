// Z2M external extension: debounce all publishes for the water softener device.
//
// Problem: when the device reconnects after deep sleep, Z2M's publishLastSeen()
// fires on each signaling message and republishes the full cached state (stale
// sensor data). The built-in debounce only applies to the receive path, not to
// publishLastSeen. This extension intercepts ALL publishes via
// adjustMessageBeforePublish and debounces them — only the last message (which
// contains fresh data) gets published.

class WaterSoftenerDebounce {
    constructor(zigbee, mqtt, state, publishEntityState, eventBus,
                enableDisableExtension, restartCallback, addExtension,
                settings, logger) {
        this.mqtt = mqtt;
        this.logger = logger;
        this.pending = {};
        this.timers = {};
        this.MODEL_ID = 'Salt Level Distance Sensor';
        this.DELAY_MS = 1500;
    }

    adjustMessageBeforePublish(entity, message) {
        if (entity.definition?.model !== this.MODEL_ID) return;
        const addr = entity.ieeeAddr;

        // Save the latest full message per device
        this.pending[addr] = { payload: JSON.stringify(message), name: entity.name };

        // Suppress this publish by emptying the message object
        for (const key of Object.keys(message)) {
            delete message[key];
        }

        // Reset the debounce timer — only the last message wins
        clearTimeout(this.timers[addr]);
        this.timers[addr] = setTimeout(() => {
            const entry = this.pending[addr];
            if (entry) {
                this.mqtt.publish(entry.name, entry.payload, {});
                delete this.pending[addr];
            }
        }, this.DELAY_MS);
    }

    async start() {
        this.logger.info('WaterSoftenerDebounce extension started');
    }

    async stop() {
        for (const timer of Object.values(this.timers)) {
            clearTimeout(timer);
        }
    }
}

module.exports = WaterSoftenerDebounce;
