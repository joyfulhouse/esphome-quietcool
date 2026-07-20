import esphome.codegen as cg
from esphome.components import fan, script
import esphome.config_validation as cv


AUTO_LOAD = ["fan", "script"]

CONF_OFF_SCRIPT = "off_script"
CONF_LOW_SCRIPT = "low_script"
CONF_MEDIUM_SCRIPT = "medium_script"
CONF_HIGH_SCRIPT = "high_script"

quietcool_confirmed_fan_ns = cg.esphome_ns.namespace("quietcool_confirmed_fan")
QuietCoolFan = quietcool_confirmed_fan_ns.class_(
    "QuietCoolFan", cg.Component, fan.Fan
)


CONFIG_SCHEMA = (
    fan.fan_schema(QuietCoolFan, default_restore_mode="NO_RESTORE")
    .extend(
        {
            cv.Required(CONF_OFF_SCRIPT): cv.use_id(script.Script),
            cv.Required(CONF_LOW_SCRIPT): cv.use_id(script.Script),
            cv.Required(CONF_MEDIUM_SCRIPT): cv.use_id(script.Script),
            cv.Required(CONF_HIGH_SCRIPT): cv.use_id(script.Script),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await fan.new_fan(config)
    await cg.register_component(var, config)

    for key, setter in (
        (CONF_OFF_SCRIPT, var.set_off_script),
        (CONF_LOW_SCRIPT, var.set_low_script),
        (CONF_MEDIUM_SCRIPT, var.set_medium_script),
        (CONF_HIGH_SCRIPT, var.set_high_script),
    ):
        command_script = await cg.get_variable(config[key])
        cg.add(setter(command_script))
