'use strict';

/**
 * Zigbee2MQTT external converter for the DIY Water Softener Salt Level Sensor.
 *
 * Device reports:
 *   - distance (cm) via genAnalogInput cluster, presentValue attribute
 *   - battery (%) via genPowerCfg cluster, batteryPercentageRemaining attribute
 *
 * Install: place this file in your Zigbee2MQTT external_converters directory
 * and reference it in configuration.yaml:
 *
 *   external_converters:
 *     - water_softener_converter.js
 */

const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const ea = exposes.access;

const fzDistance = {
    cluster: 'genAnalogInput',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data.presentValue !== undefined) {
            return { distance: parseFloat(msg.data.presentValue.toFixed(1)) };
        }
    },
};

module.exports = {
    zigbeeModel: ['talusan.softener.salt-level'],
    model: 'Salt Level Distance Sensor',
    vendor: 'talusan',
    description: 'Water softener salt level sensor',
    fromZigbee: [fzDistance, fz.battery],
    toZigbee: [],
    exposes: [
        exposes.numeric('distance', ea.STATE)
            .withUnit('cm')
            .withValueMin(0)
            .withValueMax(200)
            .withDescription('Distance from sensor to salt surface'),
        exposes.numeric('battery', ea.STATE)
            .withUnit('%')
            .withValueMin(0)
            .withValueMax(100)
            .withDescription('Remaining battery in %'),
    ],
    configure: async (device, coordinatorEndpoint, definition) => {
        const endpoint = device.getEndpoint(1);

        // Bind both clusters so the coordinator receives the device's explicit
        // attribute reports (sent via reportAnalogInput() / reportBatteryPercentage()
        // on each wake). configureReporting is intentionally omitted: z2m re-runs
        // configure on every end-device rejoin, and the ESP32 Zigbee stack
        // responds to each configureReporting command with an immediate attribute
        // report — before the fresh measurement has been loaded — producing stale
        // duplicate publishes. The device reports on its own schedule; no
        // automatic/periodic reporting configuration is needed.
        await endpoint.bind('genPowerCfg', coordinatorEndpoint);
        await endpoint.bind('genAnalogInput', coordinatorEndpoint);
    },
};
