const mqtt = require('mqtt');

/**
 * Twilio Serverless Function
 * Receives an inbound SMS and publishes the message text to WLED via MQTT.
 *
 * Required Environment Variables (set in Twilio Console > Functions > Environments):
 *   MQTT_HOST      - HiveMQ Cloud broker hostname (e.g. abc123.s1.eu.hivemq.cloud)
 *   MQTT_USERNAME  - HiveMQ Cloud username
 *   MQTT_PASSWORD  - HiveMQ Cloud password
 *   WLED_TOPIC     - WLED device MQTT topic (e.g. wled/matrix)
 */
exports.handler = function (context, event, callback) {
  const twiml = new Twilio.twiml.MessagingResponse();

  // Twilio sends the SMS body in event.Body
  const raw = (event.Body || '').trim();
  if (!raw) {
    return callback(null, twiml);
  }

  // WLED's scrolling text field (segment name) is capped at 32 characters
  const messageText = raw.substring(0, 32);

  const brokerUrl = `mqtts://${context.MQTT_HOST}:8883`;

  const client = mqtt.connect(brokerUrl, {
    username: context.MQTT_USERNAME,
    password: context.MQTT_PASSWORD,
    clientId: `twilio-wled-${Date.now()}`,
    connectTimeout: 6000,
    reconnectPeriod: 0, // no auto-reconnect inside a serverless function
  });

  // JSON API payload: enable Scrolling Text effect (fx 122) with the SMS body
  const topic = `${context.WLED_TOPIC}/api`;
  const payload = JSON.stringify({
    seg: [
      {
        fx: 122,       // Scrolling Text effect
        n: messageText,
        col: [[255, 255, 255]], // white text
        sx: 128,       // scroll speed  (0-255)
        ix: 128,       // intensity / font size (0-255)
      },
    ],
  });

  client.on('connect', () => {
    client.publish(topic, payload, { qos: 1 }, (err) => {
      client.end(false, () => {
        if (err) {
          console.error('MQTT publish error:', err);
          return callback(err);
        }
        console.log(`Published to ${topic}: ${payload}`);
        // Respond with an empty TwiML reply (no reply SMS sent back)
        callback(null, twiml);
      });
    });
  });

  client.on('error', (err) => {
    console.error('MQTT connection error:', err);
    client.end();
    callback(err);
  });
};
