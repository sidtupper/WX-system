"""WeeWX driver for WX-system gateway_latest.json (WeeWX 5.x).

Reads observations published atomically by gateway.py. Does not own the UART.
Rain is intentionally omitted until its semantics (increment vs cumulative)
are established. Custom observations need database schema additions to archive.
"""

import json
import logging
import time
from pathlib import Path

import weewx
import weewx.drivers

LOG = logging.getLogger(__name__)
DRIVER_VERSION = "0.1"


def loader(config_dict, engine):
    return GatewayJsonDriver(**config_dict["GatewayJson"])


class GatewayJsonDriver(weewx.drivers.AbstractDevice):
    def __init__(self, json_file="/var/tmp/gateway_latest.json",
                 poll_interval=2, max_age=300, **_kwargs):
        self.path = Path(json_file)
        self.poll_interval = max(0.2, float(poll_interval))
        self.max_age = float(max_age)
        self.last_received_at = None
        LOG.info("GatewayJson driver %s: reading %s", DRIVER_VERSION, self.path)

    @property
    def hardware_name(self):
        return "WX-system Gateway JSON"

    def genLoopPackets(self):
        fields = {
            "temperature": "outTemp",
            "humidity": "outHumidity",
            "barometric_pressure": "barometer",
            "wind_speed": "windSpeed",
            "wind_direction": "windDir",
            "wind_gust": "windGust",
        }
        while True:
            time.sleep(self.poll_interval)
            try:
                observation = json.loads(self.path.read_text(encoding="utf-8"))
                received_at = float(observation["received_at"])
                epoch = int(observation["epoch"])
                now = time.time()
                if received_at == self.last_received_at:
                    continue
                if received_at > now + 30 or now - received_at > self.max_age:
                    LOG.warning("Ignoring stale/future gateway JSON (received_at=%s)", received_at)
                    self.last_received_at = received_at
                    continue
                if abs(epoch - received_at) > self.max_age:
                    LOG.warning("Station epoch differs from gateway receive time; using gateway time")
                    epoch = int(received_at)
                packet = {"dateTime": epoch, "usUnits": weewx.METRICWX}
                for source, target in fields.items():
                    v = observation.get(source)
                    if v is not None:
                        packet[target] = float(v)
                # Do not publish rain until its meaning is verified.
                self.last_received_at = received_at
                yield packet
            except FileNotFoundError:
                continue
            except (ValueError, TypeError, KeyError, OSError) as exc:
                LOG.warning("Cannot read gateway JSON: %s", exc)

    def closePort(self):
        pass
