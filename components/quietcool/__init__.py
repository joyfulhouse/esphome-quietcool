"""QuietCool confirmation controller external component."""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import sx126x, sx127x
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID, CONF_TYPE
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed


# "select" must be here even though only some configs declare a select: block:
# _copy_source_tree copies quietcool_timer_select.cpp into EVERY build, and
# its select/select.h include only resolves when the select component is
# loaded — without this, a config with no select: block (both CI example
# YAMLs) fails to compile (adversarial round 4, codex).
AUTO_LOAD = ["binary_sensor", "button", "fan", "select", "sensor", "text_sensor"]
CODEOWNERS = []

CONF_ADAPTER_ID = "adapter_id"
CONF_JITTER_SEED = "jitter_seed"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
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


def validate_heartbeat_interval(value):
    interval = cv.positive_time_period_seconds(value)
    seconds = interval.total_seconds
    if seconds != 0 and not 60 <= seconds <= 3600:
        raise cv.Invalid(
            "heartbeat_interval must be 0s or between 60s and 3600s"
        )
    return interval


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
        cv.Optional(CONF_HEARTBEAT_INTERVAL, default="300s"):
            validate_heartbeat_interval,
    }
).extend(cv.COMPONENT_SCHEMA)


# Worst-case measured target stack for the command path is ~6.3 KB (hardware
# peak 6,304 B; deepest chain loop -> reduce -> dispatch ->
# handle_command_request -> begin_transaction). ESPHome's default loop stack is
# 8,192 B, leaving ~1.9 KB — a margin that demonstrably overran into the
# FreeRTOS ready lists and crash-looped a production controller twice before
# the coordinator was made iterative. 16 KB is the validated floor.
MIN_LOOP_TASK_STACK_SIZE = 16384

_STACK_GUIDANCE = f"""
The quietcool coordinator requires an ESP-IDF build with a loop-task stack of
at least {MIN_LOOP_TASK_STACK_SIZE} bytes:

esp32:
  framework:
    type: esp-idf
    advanced:
      loop_task_stack_size: {MIN_LOOP_TASK_STACK_SIZE}

Worst-case target stack for the command path is ~6.3 KB. ESPHome's 8,192 B
default leaves under 2 KB of headroom, which overflowed into FreeRTOS kernel
structures on real hardware (Guru Meditation LoadProhibited in
prvSelectHighestPriorityTaskSMP). See
docs/claude/2026-07-24-post-cutover-audit.md.
""".rstrip()


def _validate_loop_task_stack(config):
    """Reject any build whose loop stack cannot hold the coordinator.

    Enforced here rather than documented, because the requirement is invisible
    at compile time and its violation manifests as intermittent kernel
    corruption rather than a clean failure.
    """
    full_config = fv.full_config.get()
    framework = full_config.get("esp32", {}).get("framework", {})

    # loop_task_stack_size is an ESP-IDF-only option; its absence means either
    # an Arduino build or a framework that cannot express the requirement.
    stack_size = framework.get("advanced", {}).get("loop_task_stack_size")
    if stack_size is None:
        raise cv.Invalid(
            "quietcool requires the ESP-IDF framework; this configuration does "
            f"not set a loop-task stack size.\n{_STACK_GUIDANCE}"
        )
    if stack_size < MIN_LOOP_TASK_STACK_SIZE:
        raise cv.Invalid(
            f"loop_task_stack_size is {stack_size} bytes, below the "
            f"{MIN_LOOP_TASK_STACK_SIZE}-byte minimum quietcool "
            f"requires.\n{_STACK_GUIDANCE}"
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_loop_task_stack


_COMPONENT_DIR = Path(__file__).resolve().parent
_SOURCE_TREE_DIRS = ("core", "ports", "radio", "esphome")
_SOURCE_SUFFIXES = frozenset({".h", ".hpp", ".c", ".cpp", ".tcc"})


def _source_tree_files():
    """(relative path, absolute path) of every C/C++ file in the source tree."""
    files = []
    for top in _SOURCE_TREE_DIRS:
        for path in sorted((_COMPONENT_DIR / top).rglob("*")):
            if path.is_file() and path.suffix in _SOURCE_SUFFIXES:
                files.append((path.relative_to(_COMPONENT_DIR), path))
    return files


def _copy_source_tree():
    """Copy core/ports/radio/esphome into the build at src/quietcool/.

    ESPHome copies only the top-level C++ files of an external component
    (recursion into subdirectories is reserved for ESPHome's own core), and
    every file of this component lives in a subdirectory. Without this step a
    github:// source -- the ESPHome Device Builder install -- compiles a
    main.cpp whose quietcool headers do not exist (issue #42). Doing the copy
    here makes the component self-contained however it is sourced, with no
    `esphome: includes:` bridge (which only accepts local paths).

    Files are copied only when changed, so incremental builds stay
    incremental, and source files no longer in the tree are pruned so a
    removed .cpp cannot linger in the build and be compiled.
    """
    dest_root = Path(CORE.relative_src_path("quietcool"))
    wanted = set()
    for rel, src in _source_tree_files():
        dst = dest_root / rel
        wanted.add(dst)
        copy_file_if_changed(src, dst)
    if dest_root.is_dir():
        for stale in dest_root.rglob("*"):
            if (stale.is_file() and stale.suffix in _SOURCE_SUFFIXES
                    and stale not in wanted):
                stale.unlink()


async def to_code(config):
    # Supply the C++ source tree at src/quietcool (see _copy_source_tree).
    # All cross-directory includes are src-rooted ("quietcool/core/...") so
    # they resolve from the build's src/ include root under both PlatformIO
    # and ESP-IDF without any -I flag (a relative -I breaks under ESP-IDF's
    # build directory layout).
    _copy_source_tree()
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
        config[CONF_HEARTBEAT_INTERVAL].total_milliseconds,
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
