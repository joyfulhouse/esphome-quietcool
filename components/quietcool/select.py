import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import QuietCoolComponent, quietcool_ns


CONF_CONTROLLER_ID = "controller_id"

QuietCoolTimerSelect = quietcool_ns.class_(
    "QuietCoolTimerSelect", cg.Component, select.Select
)

# Order matters: it is the order Home Assistant renders, and it must match
# kTimerOptions in components/quietcool/esphome/timer_command.h, whose index is
# the TimerSelection ordinal. "None" means no TIMER (the fan runs until
# stopped), not a stopped fan.
TIMER_OPTIONS = ["None", "1 hour", "2 hours", "4 hours", "8 hours", "12 hours"]


CONFIG_SCHEMA = (
    select.select_schema(QuietCoolTimerSelect, icon="mdi:timer-outline")
    .extend(
        {
            cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # Subdirectory headers are not auto-included; declare the entity class.
    cg.add_global(
        cg.RawStatement('#include "quietcool/esphome/quietcool_timer_select.h"')
    )
    var = await select.new_select(config, options=TIMER_OPTIONS)
    await cg.register_component(var, config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(var.set_controller(controller))
