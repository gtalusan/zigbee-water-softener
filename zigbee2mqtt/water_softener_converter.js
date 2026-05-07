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

const fzCustomV2 = {
    cluster: '64512',  // 0xFC00 — Z2M has custom clusters as decimal strings
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const result = {};
        if (msg.data[0x0000] !== undefined) result.distance = parseFloat(msg.data[0x0000].toFixed(1));
        if (msg.data[0x0001] !== undefined) result.wake_count = msg.data[0x0001];
        if (msg.data[0x0002] !== undefined) result.last_runtime_ms = msg.data[0x0002];
        if (msg.data[0x0003] !== undefined) result.vl53_error_count = msg.data[0x0003];
        return result;
    },
};

const v1Definition = {
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
        await endpoint.bind('genPowerCfg', coordinatorEndpoint);
        await endpoint.bind('genAnalogInput', coordinatorEndpoint);
    },
};

const v2Definition = {
    zigbeeModel: ['talusan.softener.salt-level-v2'],
    model: 'Salt Level Distance Sensor V2',
    vendor: 'talusan',
    description: 'Water softener salt level sensor (v2, custom cluster)',
    fromZigbee: [fzCustomV2, fz.battery],
    toZigbee: [],
    ota: true,
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
        exposes.numeric('wake_count', ea.STATE)
            .withValueMin(0)
            .withDescription('Number of deep sleep wake cycles'),
        exposes.numeric('last_runtime_ms', ea.STATE)
            .withUnit('ms')
            .withValueMin(0)
            .withDescription('Duration of last wake period before deep sleep'),
        exposes.numeric('vl53_error_count', ea.STATE)
            .withValueMin(0)
            .withDescription('Count of VL53L0X sensor read errors (distance=0)'),
    ],
    configure: async (device, coordinatorEndpoint, definition) => {
        const endpoint = device.getEndpoint(1);
        await endpoint.bind('genPowerCfg', coordinatorEndpoint);
        await endpoint.bind(0xFC00, coordinatorEndpoint);
        await endpoint.bind('genOta', coordinatorEndpoint);
    },
};

module.exports = [v1Definition, v2Definition];
