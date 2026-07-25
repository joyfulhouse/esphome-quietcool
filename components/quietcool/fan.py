import esphome.codegen as cg
from esphome.components import fan
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import QuietCoolComponent, quietcool_ns


CONF_CONTROLLER_ID = "controller_id"

QuietCoolFan = quietcool_ns.class_("QuietCoolFan", cg.Component, fan.Fan)


CONFIG_SCHEMA = (
    fan.fan_schema(QuietCoolFan, default_restore_mode="NO_RESTORE")
    .extend(
        {
            cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # Subdirectory headers are not auto-included; declare the entity class.
    cg.add_global(cg.RawStatement('#include "quietcool/esphome/quietcool_fan.h"'))
    var = await fan.new_fan(config)
    await cg.register_component(var, config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(var.set_controller(controller))
