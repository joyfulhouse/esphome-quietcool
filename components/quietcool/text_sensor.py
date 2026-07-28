import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import QuietCoolComponent


CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"

TEXT_SENSOR_SETTERS = {
    "command_status": "set_command_status_sensor",
    "evidence_source": "set_evidence_source_sensor",
    "last_tx_command": "set_last_tx_command_sensor",
    "last_rx_frame": "set_last_rx_frame_sensor",
    "last_confirmed_state": "set_last_confirmed_state_sensor",
    "speed_capability": "set_speed_capability_sensor",
    "remote_sender_id": "set_remote_sender_id_sensor",
}


CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC
).extend(
    {
        cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        cv.Required(CONF_KIND): cv.one_of(*TEXT_SENSOR_SETTERS, lower=True),
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    setter = TEXT_SENSOR_SETTERS[config[CONF_KIND]]
    cg.add(getattr(controller, setter)(var))
