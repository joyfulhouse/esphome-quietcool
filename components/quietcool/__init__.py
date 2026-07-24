"""QuietCool confirmation controller external component."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import sx126x, sx127x
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE


AUTO_LOAD = ["binary_sensor", "button", "fan", "sensor", "text_sensor"]
CODEOWNERS = []

CONF_ADAPTER_ID = "adapter_id"
CONF_JITTER_SEED = "jitter_seed"
CONF_PACKET = "packet"
CONF_PREFERENCE_KEY = "preference_key"
CONF_RADIO = "radio"
CONF_SENDER_SEED = "sender_seed"

RADIO_SX127X = "sx127x"
RADIO_SX126X = "sx126x"

# Fully qualified on purpose: the platform-free core lives in ::quietcool,
# the adapters in esphome::quietcool. Generated main.cpp does
# `using namespace esphome;`, so a bare `quietcool::` reference would be
# ambiguous between the two. Qualifying with esphome:: resolves uniquely.
quietcool_ns = cg.global_ns.namespace("esphome").namespace("quietcool")
QuietCoolComponent = quietcool_ns.class_("QuietCoolComponent", cg.Component)
Sx127xRadioAdapter = quietcool_ns.class_("Sx127xRadioAdapter")
Sx126xRadioAdapter = quietcool_ns.class_("Sx126xRadioAdapter")
QuietCoolOnPacketAction = quietcool_ns.class_(
    "QuietCoolOnPacketAction", automation.Action
)


def validate_sender_seed(value):
    value = cv.hex_uint32_t(value)
    if value != 0 and (value >> 24) != 0xCB:
        raise cv.Invalid("sender_seed must be zero or begin with the OEM 0xCB prefix")
    return value


RADIO_SCHEMA = cv.typed_schema(
    {
        RADIO_SX127X: cv.Schema(
            {
                cv.Required(CONF_ID): cv.use_id(sx127x.SX127x),
                cv.GenerateID(CONF_ADAPTER_ID): cv.declare_id(
                    Sx127xRadioAdapter
                ),
            }
        ),
        RADIO_SX126X: cv.Schema(
            {
                cv.Required(CONF_ID): cv.use_id(sx126x.SX126x),
                cv.GenerateID(CONF_ADAPTER_ID): cv.declare_id(
                    Sx126xRadioAdapter
                ),
            }
        ),
    },
    key=CONF_TYPE,
    lower=True,
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(QuietCoolComponent),
        cv.Required(CONF_RADIO): RADIO_SCHEMA,
        cv.Optional(CONF_SENDER_SEED, default=0): validate_sender_seed,
        cv.Optional(CONF_PREFERENCE_KEY, default=0x51434332): cv.hex_uint32_t,
        cv.Optional(CONF_JITTER_SEED, default=0x51434332): cv.hex_uint32_t,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # ESPHome 2025.11 copies only top-level external-component C++ resources.
    # The contractual source tree is supplied by the example's esphome.includes
    # directory bridge, which lands at src/quietcool. All cross-directory
    # includes are src-rooted ("quietcool/core/...") so they resolve from the
    # build's src/ include root under both PlatformIO and ESP-IDF without any
    # -I flag (a relative -I breaks under ESP-IDF's build directory layout).
    # ESPHome auto-includes only top-level component headers into main.cpp;
    # this component's C++ lives in subdirectories, so the generated code's
    # class declarations need their headers added explicitly. The two radio
    # adapter headers self-guard on QUIETCOOL_USE_SX127X/SX126X, so including
    # both is safe regardless of the configured variant.
    cg.add_global(
        cg.RawStatement('#include "quietcool/esphome/quietcool_component.h"'))
    cg.add_global(cg.RawStatement('#include "quietcool/esphome/automation.h"'))
    cg.add_global(
        cg.RawStatement('#include "quietcool/radio/sx127x_radio_adapter.h"'))
    cg.add_global(
        cg.RawStatement('#include "quietcool/radio/sx126x_radio_adapter.h"'))

    radio_config = config[CONF_RADIO]
    radio = await cg.get_variable(radio_config[CONF_ID])
    if radio_config[CONF_TYPE] == RADIO_SX127X:
        cg.add_define("QUIETCOOL_USE_SX127X")
    else:
        cg.add_define("QUIETCOOL_USE_SX126X")

    adapter = cg.new_Pvariable(radio_config[CONF_ADAPTER_ID], radio)
    var = cg.new_Pvariable(
        config[CONF_ID],
        adapter,
        config[CONF_SENDER_SEED],
        config[CONF_PREFERENCE_KEY],
        config[CONF_JITTER_SEED],
    )
    await cg.register_component(var, config)


ON_PACKET_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(QuietCoolComponent),
        cv.Optional(CONF_PACKET, default="received_packet"): cv.one_of(
            "received_packet", lower=True
        ),
    }
)


@automation.register_action(
    "quietcool.on_packet", QuietCoolOnPacketAction, ON_PACKET_ACTION_SCHEMA
)
async def quietcool_on_packet_to_code(
    config, action_id, template_arg, args
):
    controller = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, controller)
