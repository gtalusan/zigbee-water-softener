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
        // Binds needed for the Arduino version (reports via binding table).
        // The ESP-IDF version sends directly to coordinator 0x0000 and
        // doesn't need these, but they're harmless with the debounce extension.
        const endpoint = device.getEndpoint(1);
        await endpoint.bind('genPowerCfg', coordinatorEndpoint);
        await endpoint.bind('genAnalogInput', coordinatorEndpoint);
    },
};
