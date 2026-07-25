import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import QuietCoolComponent, quietcool_ns


CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"

QuietCoolButton = quietcool_ns.class_(
    "QuietCoolButton", cg.Component, button.Button
)
QuietCoolButtonKind = quietcool_ns.enum("QuietCoolButtonKind", is_class=True)

BUTTON_KINDS = {
    "refresh": QuietCoolButtonKind.Refresh,
    "learn": QuietCoolButtonKind.Learn,
    "forget": QuietCoolButtonKind.Forget,
}


CONFIG_SCHEMA = (
    button.button_schema(QuietCoolButton)
    .extend(
        {
            cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
            cv.Required(CONF_KIND): cv.enum(BUTTON_KINDS, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # Subdirectory headers are not auto-included; declare the entity class.
    cg.add_global(
        cg.RawStatement('#include "quietcool/esphome/quietcool_button.h"'))
    var = await button.new_button(config)
    await cg.register_component(var, config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(var.set_controller(controller))
    cg.add(var.set_kind(config[CONF_KIND]))
