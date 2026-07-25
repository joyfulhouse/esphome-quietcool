import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import QuietCoolComponent


CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"

BINARY_SENSOR_SETTERS = {
    "state_known": "set_state_known_sensor",
    "timer_program_known": "set_timer_program_known_sensor",
    "timer_remaining_known": "set_timer_remaining_known_sensor",
    "confirmed_off": "set_confirmed_off_sensor",
    "controller_fault": "set_controller_fault_sensor",
}


CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC
).extend(
    {
        cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        cv.Required(CONF_KIND): cv.one_of(
            *BINARY_SENSOR_SETTERS, lower=True
        ),
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    setter = BINARY_SENSOR_SETTERS[config[CONF_KIND]]
    cg.add(getattr(controller, setter)(var))
