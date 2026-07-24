import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_MINUTE,
)

from . import QuietCoolComponent


CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"


CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_MINUTE,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_DURATION,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        cv.Required(CONF_KIND): cv.one_of("timer_remaining", lower=True),
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(controller.set_timer_remaining_sensor(var))
