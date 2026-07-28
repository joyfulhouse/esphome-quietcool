import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MINUTE,
)

from . import QuietCoolComponent

CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"

_CONTROLLER_FIELDS = {cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent)}

# Counters take neither a unit nor a duration device class — only
# `timer_remaining` does. The schema this replaced hardcoded UNIT_MINUTE +
# DEVICE_CLASS_DURATION onto every kind, which is exactly what could not
# serve the counters too.
_COUNTER_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

SENSOR_KINDS = {
    "timer_remaining": (
        "set_timer_remaining_sensor",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_DURATION,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    "tx_count": ("set_tx_count_sensor", _COUNTER_SCHEMA),
    "rx_valid_count": ("set_rx_valid_count_sensor", _COUNTER_SCHEMA),
    "rx_rejected_count": ("set_rx_rejected_count_sensor", _COUNTER_SCHEMA),
}

# A `kind`-keyed schema, not the single hardcoded schema this replaced: each
# kind's metadata (see SENSOR_KINDS above) must reach the entity untouched,
# so `timer_remaining` keeps its minutes/duration fields and the counters get
# none of them. This is the same discriminated-schema idiom ESPHome's own
# uptime/sensor platform uses for its "seconds" vs "timestamp" kinds.
CONFIG_SCHEMA = cv.typed_schema(
    {
        kind: schema.extend(_CONTROLLER_FIELDS)
        for kind, (_setter, schema) in SENSOR_KINDS.items()
    },
    key=CONF_KIND,
    lower=True,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    setter = SENSOR_KINDS[config[CONF_KIND]][0]
    cg.add(getattr(controller, setter)(var))
