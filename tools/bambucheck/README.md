# Bambu monitor checks

From the project root, run the web tests with Node.js 18 or newer.

```sh
node --test tools/bambucheck/web.test.cjs
```

The C++ checks use the same parser, settings changes, screen mask, history and alert queue as the firmware. The host adapters provide a clock and an in-memory Preferences store. Point `ARDUINOJSON_SRC` at the `src` directory of ArduinoJson 7.1.0.

```sh
c++ -std=c++17 -I"$ARDUINOJSON_SRC" -IMeteoPlaneRadar -Itools/bambucheck/host \
  tools/bambucheck/check.cpp MeteoPlaneRadar/BambuParse.cpp MeteoPlaneRadar/BambuSettings.cpp \
  MeteoPlaneRadar/PrintParse.cpp MeteoPlaneRadar/PrintNotices.cpp -o /tmp/mpr-bambucheck
/tmp/mpr-bambucheck
```

The MQTT test feeds valid MQTT frames through the pinned PubSubClient library, including a 38 kB report, an oversized report, the wrong serial's topic and an interrupted frame. Set `PUBSUBCLIENT` to the root of PubSubClient 2.8.

```sh
shim="$PUBSUBCLIENT/tests/src/lib"
c++ -std=c++17 -I"$ARDUINOJSON_SRC" -I"$PUBSUBCLIENT/src" -I"$shim" -IMeteoPlaneRadar \
  tools/bambucheck/mqtt.cpp MeteoPlaneRadar/BambuParse.cpp MeteoPlaneRadar/PrintParse.cpp \
  "$PUBSUBCLIENT/src/PubSubClient.cpp" "$shim/Buffer.cpp" "$shim/IPAddress.cpp" "$shim/Stream.cpp" \
  -o /tmp/mpr-bambu-mqtt
/tmp/mpr-bambu-mqtt
```

For an ESP32-S3 build run `arduino-cli compile --profile default MeteoPlaneRadar`. The same binary targets both Waveshare panels. A live acceptance check requires a printer and the appropriate LAN code or cloud token. With both monitors enabled, check an idle screen, a running print, a WiFi reconnect, a manual stop, and a queued alert while Settings is open.
