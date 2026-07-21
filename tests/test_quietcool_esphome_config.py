import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "quietcool-lora32.yaml"
V3_CONFIG = ROOT / "quietcool-lora-v3.yaml"
SECRETS = ROOT / "secrets.yaml"
README = ROOT / "README.md"
CONFIRMED_FAN_HEADER = (
    ROOT / "components" / "quietcool_confirmed_fan" / "quietcool_confirmed_fan.h"
)
CONFIRMED_FAN_PLATFORM = (
    ROOT / "components" / "quietcool_confirmed_fan" / "fan.py"
)

DISPLAY_TEMPERATURE_SENSORS = (
    ("display_indoor_entity", "sensor.quietcool_display_indoor", "temp_indoor"),
    ("display_outdoor_entity", "sensor.quietcool_display_outdoor", "temp_outdoor"),
    ("display_attic_entity", "sensor.quietcool_display_attic", "temp_attic"),
)

LEGACY_AIOSENSE_TEMPERATURE_SENSORS = (
    ("sensor.aiosense_dining_room_temperature", "temp_indoor_dining_room"),
    ("sensor.aiosense_guest_bathroom_temperature", "temp_indoor_guest_bathroom"),
    ("sensor.aiosense_kaelyns_bedroom_temperature", "temp_indoor_kaelyns_bedroom"),
    ("sensor.aiosense_kitchen_temperature", "temp_indoor_kitchen"),
    ("sensor.aiosense_living_room_temperature", "temp_indoor_living_room"),
    ("sensor.aiosense_living_room_tv_temperature", "temp_indoor_living_room_tv"),
    ("sensor.aiosense_office_desk_temperature", "temp_indoor_office_desk"),
    ("sensor.aiosense_office_tv_temperature", "temp_indoor_office_tv"),
    ("sensor.aiosense_office_bathroom_temperature", "temp_indoor_office_bathroom"),
    ("sensor.aiosense_starry_s_office_temperature", "temp_indoor_starry_s_office"),
    ("sensor.aiosense_sunroom_temperature", "temp_indoor_sunroom"),
)


def top_level_block(text: str, name: str) -> str:
    lines = text.splitlines()
    start = lines.index(f"{name}:")
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if line and not line.startswith((" ", "#")):
            end = index
            break
    return "\n".join(lines[start:end])


def script_blocks(text: str) -> dict[str, str]:
    scripts = top_level_block(text, "script")
    matches = re.finditer(
        r"(?ms)^  - id: (?P<id>[a-z0-9_]+)\n(?P<body>.*?)(?=^  - id: |\Z)",
        scripts,
    )
    return {match.group("id"): match.group("body") for match in matches}


def packet_bytes(script: str) -> list[int]:
    # Extracts bytes from every literal `data: [...]` array in the script,
    # not just the first one, in case a script contains more than one.
    payload: list[int] = []
    for match in re.finditer(r"(?ms)data:\s*\[(.*?)\]", script):
        payload.extend(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))
    return payload


def _section_item_containing(
    text: str, section_name: str, item_prefix: str, marker: str
) -> str:
    """Return the one `  - <item_prefix>:`-delimited item holding marker."""
    section = top_level_block(text, section_name)
    pattern = rf"(?ms)^  - {item_prefix}:.*?(?=^  - {item_prefix}:|\Z)"
    for match in re.finditer(pattern, section):
        item = match.group(0)
        if marker in item:
            return item
    raise ValueError(f"No {section_name} item contains {marker!r}")


def list_item_containing(text: str, section_name: str, marker: str) -> str:
    return _section_item_containing(text, section_name, "platform", marker)


def interval_item_containing(text: str, marker: str) -> str:
    """Return one top-level interval item instead of the aggregate block."""
    return _section_item_containing(text, "interval", "interval", marker)


def oem_state_matches(desired: int, reported: int) -> bool:
    """Model the comparison recovered from STM32 function 0x08005F90."""
    desired_state = desired & 0x3F
    reported_state = reported & 0x3F
    if (desired & 0x0F) == 0:
        return (reported_state & 0x0F) == 0
    return desired_state == reported_state


class QuietCoolESPHomeConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CONFIG.read_text()
        cls.v3_text = V3_CONFIG.read_text()
        cls.secrets_text = SECRETS.read_text() if SECRETS.exists() else ""
        cls.readme_text = README.read_text()

    def test_radio_profile_matches_recovered_firmware(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        expected_lines = (
            "  frequency: 433920000",
            "  modulation: FSK",
            "  bandwidth: 50_0kHz",
            "  packet_mode: true",
            "  bitsync: true",
            "  bitrate: 2400",
            "  deviation: 10000",
            "  sync_value: [0x2D, 0xD4]",
            "  payload_length: 0",
            "  crc_enable: false",
            "  preamble_size: 8",
            "  preamble_polarity: 0xAA",
            "  pa_pin: BOOST",
            "  pa_power: 17",
        )
        for line in expected_lines:
            with self.subTest(line=line):
                self.assertIn(line, radio)

    def test_only_tx_burst_transmits(self) -> None:
        # sx127x.send_packet appears exactly once in the whole config,
        # inside the single queued core script tx_burst. The five command
        # wrappers are thin: they route through begin_transaction (the one
        # shared join/arm site), and neither they nor begin_transaction
        # contain send_packet or delay:, so requests complete synchronously
        # and can never drop a rapid re-press.
        scripts = script_blocks(self.text)
        self.assertIn("tx_burst", scripts)
        tx_burst = scripts["tx_burst"]

        total_occurrences = self.text.count("sx127x.send_packet")
        self.assertEqual(total_occurrences, 1)
        self.assertEqual(tx_burst.count("sx127x.send_packet"), 1)

        self.assertRegex(tx_burst, r"(?m)^\s+mode: queued\s*$")
        self.assertRegex(tx_burst, r"(?m)^\s+max_runs: \d+\s*$")
        self.assertRegex(tx_burst, r"(?m)^\s+count: 3\s*$")
        self.assertRegex(tx_burst, r"(?m)^\s+- delay: 45ms\s*$")
        self.assertIn("id(learned_sender_id)", tx_burst)
        self.assertIn(
            "id(tx_burst_sender_id) = id(learned_sender_id);", tx_burst
        )
        self.assertIn("uint32_t sender_id = id(tx_burst_sender_id);", tx_burst)
        for shift in (24, 16, 8, 0):
            self.assertIn(f"(sender_id >> {shift}) & 0xFF", tx_burst)
        self.assertIn("cmd, cmd", tx_burst)

        begin = scripts["begin_transaction"]
        self.assertNotIn("sx127x.send_packet", begin)
        self.assertNotIn("delay:", begin)
        self.assertIn("id: tx_burst", begin)
        for wrapper_id in ("send_off", "send_low", "send_medium", "send_high", "send_timer"):
            with self.subTest(script_id=wrapper_id):
                wrapper = scripts[wrapper_id]
                self.assertNotIn("sx127x.send_packet", wrapper)
                self.assertNotIn("delay:", wrapper)
                self.assertIn("id: begin_transaction", wrapper)
                self.assertNotIn("id: tx_burst", wrapper)

    def test_bounded_tx_queue_rejection_cannot_erase_latest_desired_refire(self) -> None:
        # ESPHome rejects execute() beyond max_runs. begin_transaction
        # therefore persists the latest desired/refire state before its
        # enqueue. Once no burst is active, automatic query/refire clears
        # stale queued work so its execute cannot be rejected or delayed by
        # obsolete requests.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                tx = script_blocks(text)["tx_burst"]
                self.assertIn("mode: queued", tx)
                self.assertIn("max_runs: 5", tx)

                begin = script_blocks(text)["begin_transaction"]
                self.assertLess(begin.index("id(refire_cmd) = cmd;"), begin.index("id: tx_burst"))
                self.assertLess(begin.index("id(refire_left) = refires;"), begin.index("id: tx_burst"))

                refire = interval_item_containing(text, 'ESP_LOGD("REFIRE"')
                self.assertIn("!id(tx_burst).is_running()", refire)
                self.assertIn("id: tx_burst", refire)
                self.assertIn("cmd: !lambda 'return id(refire_cmd);'", refire)
                stop_index = refire.index("id(tx_burst).stop();")
                execute_index = refire.index("id: tx_burst", stop_index)
                decrement_index = refire.index(
                    "id(refire_left) = id(refire_left) - 1;", execute_index
                )
                self.assertTrue(stop_index < execute_index < decrement_index)

    def test_every_actual_state_burst_invalidates_known_state_before_airtime(self) -> None:
        # A mismatch response can make state known again before an automatic
        # re-fire. tx_burst must invalidate at actual execution too, not only
        # in user wrappers, or a successful re-fire with a lost reply leaves
        # HA claiming the old state is still authoritative.
        for config_name, text, send_action in (
            ("SX1278", self.text, "sx127x.send_packet"),
            ("SX1262", self.v3_text, "sx126x.send_packet"),
        ):
            with self.subTest(config=config_name):
                tx = script_blocks(text)["tx_burst"]
                invalidate_start = tx.index("if (cmd != 0x66)")
                fan_unknown = tx.index("id(fan_state_known) = false;", invalidate_start)
                timer_unknown = tx.index("id(timer_state_known) = false;", invalidate_start)
                fan_sensor_unknown = tx.index(
                    "id(fan_state_known_sensor).publish_state(false);", invalidate_start
                )
                timer_sensor_unknown = tx.index(
                    "id(timer_state_known_sensor).publish_state(false);", invalidate_start
                )
                send_index = tx.index(send_action)
                self.assertTrue(invalidate_start < fan_unknown < send_index)
                self.assertTrue(invalidate_start < timer_unknown < send_index)
                self.assertTrue(fan_unknown < fan_sensor_unknown < send_index)
                self.assertTrue(timer_unknown < timer_sensor_unknown < send_index)

    def test_actual_tx_refuses_stale_commands_and_honors_oem_query_holdoff(self) -> None:
        # Queue order must never override latest-desired semantics, and hearing
        # the physical remote's 66 query reserves its full query/command
        # exchange before any newly requested local state burst may take air.
        for config_name, text, send_action in (
            ("SX1278", self.text, "sx127x.send_packet"),
            ("SX1262", self.v3_text, "sx126x.send_packet"),
        ):
            with self.subTest(config=config_name):
                tx = script_blocks(text)["tx_burst"]
                guard = tx.index("bool stale_state_command")
                send = tx.index(send_action)
                self.assertIn("cmd != 0x66", tx[guard:send])
                self.assertIn(
                    "(!id(cl_active) || cmd != id(cl_desired_cmd))",
                    tx[guard:send],
                )
                self.assertIn("bool oem_exchange_holdoff", tx[guard:send])
                self.assertIn("(millis() - id(oem_query_seen_ms)) < 2000UL", tx[guard:send])
                self.assertLess(guard, send)

                refire = interval_item_containing(text, 'ESP_LOGD("REFIRE"')
                self.assertIn(
                    "(!id(oem_query_seen) ||\n                      (millis() - id(oem_query_seen_ms)) >= 2000UL)",
                    refire,
                )

                for wrapper_id in ("send_off", "send_low", "send_medium", "send_high", "send_timer"):
                    wrapper = script_blocks(text)[wrapper_id]
                    self.assertNotIn("id(oem_query_seen) = false;", wrapper)
    def test_tx_burst_payload_byte_order_is_msb_first(self) -> None:
        # FIX 3: test_only_tx_burst_transmits above only checks that each
        # `(sender_id >> N) & 0xFF` substring EXISTS somewhere in tx_burst,
        # never their ORDER inside the actual `return {...}` initializer -
        # swapping >>16 and >>8 there (garbling the on-air bytes to
        # CB 47 00 39) would still pass every one of those assertions.
        # Extract the initializer text itself and assert the shift
        # sequence is exactly MSB-first (24, 16, 8, 0) in positional
        # order, followed by the two repeated command bytes.
        tx_burst = script_blocks(self.text)["tx_burst"]
        match = re.search(r"(?s)return\s*\{(.*?)\};", tx_burst)
        self.assertIsNotNone(match)
        initializer = match.group(1)
        shifts = [int(value) for value in re.findall(r">>\s*(\d+)\)", initializer)]
        self.assertEqual(shifts, [24, 16, 8, 0])
        last_shift_index = initializer.rindex(">> 0)")
        cmd_index = initializer.index("cmd, cmd")
        self.assertLess(last_shift_index, cmd_index)

    def test_sender_id_seed_and_persisted_global(self) -> None:
        substitutions = top_level_block(self.text, "substitutions")
        self.assertIn('quietcool_sender_id: "0x00000000"', substitutions)

        globals_block = top_level_block(self.text, "globals")
        start = globals_block.index("- id: learned_sender_id")
        next_id = globals_block.find("\n  - id:", start + 1)
        item = globals_block[start:] if next_id == -1 else globals_block[start:next_id]
        self.assertIn("type: uint32_t", item)
        self.assertIn("restore_value: true", item)
        self.assertIn('initial_value: "0x00000000"', item)

    def test_boot_seeds_only_an_unprovisioned_sender_and_arms_auto_learn(self) -> None:
        boot = top_level_block(self.text, "esphome")
        self.assertIn("const uint32_t sender_seed = ${quietcool_sender_id};", boot)
        self.assertIn(
            "if (id(learned_sender_id) == 0 && sender_seed != 0)", boot
        )
        self.assertIn("id(learned_sender_id) = sender_seed;", boot)
        self.assertIn("learned_sender_id->update();", boot)
        self.assertIn("global_preferences->sync();", boot)
        self.assertIn("uint32_t sender_id = id(learned_sender_id);", boot)
        self.assertIn("if (sender_id == 0)", boot)
        self.assertIn("id(learn_active) = true;", boot)
        self.assertIn("id(learn_auto_mode) = true;", boot)
        self.assertIn("id(remote_sender_id_sensor).publish_state", boot)

    def test_tx_burst_refuses_zero_sender_before_count_or_send(self) -> None:
        tx_burst = script_blocks(self.text)["tx_burst"]
        self.assertIn("return id(learned_sender_id) == 0 || unsafe_manual_query ||", tx_burst)
        self.assertIn('ESP_LOGE("TX"', tx_burst)

        zero_guard = tx_burst.index("return id(learned_sender_id) == 0 || unsafe_manual_query ||")
        counter = tx_burst.index(
            "id(tx_count_sensor).publish_state(id(tx_count_sensor).state + 1);"
        )
        snapshot = tx_burst.index(
            "id(tx_burst_sender_id) = id(learned_sender_id);"
        )
        sender_read = tx_burst.index("uint32_t sender_id = id(tx_burst_sender_id);")
        send = tx_burst.index("sx127x.send_packet")
        # YAML names the send_packet action before its data lambda text;
        # runtime ordering is guard -> else/count -> action/data lambda.
        self.assertTrue(zero_guard < snapshot < counter < send < sender_read)

        # No wrapper may increment first and then discover in tx_burst that
        # the sender is unset: refusal means the TX counter stays unchanged.
        for script_id, body in script_blocks(self.text).items():
            if script_id != "tx_burst":
                with self.subTest(script_id=script_id):
                    self.assertNotIn(
                        "id(tx_count_sensor).state + 1", body
                    )

    def test_example_seed_composes_the_end_to_end_payloads(self) -> None:
        # The public template ships unprovisioned (0x00000000); this models a
        # provisioned example ID to pin the payload-composition math.
        # send_off is speed-matched at runtime (90/A0/B0) and asserted
        # separately in test_send_off_is_speed_matched.
        seed = 0xCB123456
        sender_bytes = [(seed >> shift) & 0xFF for shift in (24, 16, 8, 0)]
        self.assertEqual(sender_bytes, [0xCB, 0x12, 0x34, 0x56])
        expected = {
            "send_low": [0xCB, 0x12, 0x34, 0x56, 0x9F, 0x9F],
            "send_medium": [0xCB, 0x12, 0x34, 0x56, 0xAF, 0xAF],
            "send_high": [0xCB, 0x12, 0x34, 0x56, 0xBF, 0xBF],
        }
        scripts = script_blocks(self.text)
        for script_id, payload in expected.items():
            with self.subTest(script_id=script_id):
                command_match = re.search(
                    r"(?m)^\s+cmd:\s*0x([0-9A-Fa-f]{2})\s*$", scripts[script_id]
                )
                self.assertIsNotNone(command_match)
                command = int(command_match.group(1), 16)
                self.assertEqual(sender_bytes + [command, command], payload)

    def test_fixed_state_scripts_route_through_tx_burst_with_correct_command_byte(self) -> None:
        # The three ON command bytes are byte-identical to the previously
        # verified firmware and must never change; OFF and TIMER compute
        # their bytes at runtime. All route through begin_transaction.
        expected = {
            "send_low": 0x9F,
            "send_medium": 0xAF,
            "send_high": 0xBF,
        }
        scripts = script_blocks(self.text)
        for script_id, command_byte in expected.items():
            with self.subTest(script_id=script_id):
                wrapper = scripts[script_id]
                self.assertIn("id: begin_transaction", wrapper)
                self.assertRegex(wrapper, rf"(?m)^\s+cmd:\s*0x{command_byte:02X}\s*$")
                self.assertNotIn("sx127x.send_packet", wrapper)
                self.assertNotIn("delay:", wrapper)

    def test_send_off_is_speed_matched_and_reaimed_by_the_closed_loop(self) -> None:
        # Live evidence (2026-07-19): a fan running a High/1h timer ignored
        # six spaced 0x90 bursts and confirmed OFF first-try once in Low.
        # OFF must therefore be the speed-matched variant (90/A0/B0), like
        # the OEM remote's remembered speed, and the closed loop must
        # re-aim the re-fires at the fan's reported speed on a mismatch.
        off = script_blocks(self.text)["send_off"]
        self.assertIn("uint8_t off_speed_nibble = 0x90;", off)
        self.assertIn("if (id(quietcool_fan).speed == 2) off_speed_nibble = 0xA0;", off)
        self.assertIn("else if (id(quietcool_fan).speed >= 3) off_speed_nibble = 0xB0;", off)
        self.assertIn("id(off_tx_command) = off_speed_nibble;", off)
        self.assertIn("cmd: !lambda 'return id(off_tx_command);'", off)
        # The 0x80 neutral form must never be transmittable: the adaptation
        # is gated on reported speed 1-3.
        coord = interval_item_containing(self.text, "id(cl_report_ready)")
        self.assertIn("desired_is_off && actual_speed >= 1 && actual_speed <= 3", coord)
        self.assertIn("(uint8_t) (0x80 | (actual_speed << 4))", coord)
        # The re-aim retargets the live transaction (cl_desired_cmd +
        # refire_cmd); off_tx_command is deliberately untouched because
        # send_off recomputes it from entity speed on every fresh request.
        self.assertIn("id(cl_desired_cmd) = adapted_off;", coord)
        self.assertIn("id(refire_cmd) = adapted_off;", coord)
        self.assertNotIn("id(off_tx_command) = adapted_off;", coord)

    def test_yield_policy_requires_a_running_state_report(self) -> None:
        # A fan that missed our ON command reports its off state (90/A0/B0
        # - valid command encodings), so an off report on an ON transaction
        # must re-fire, not yield. A running report must also differ from the
        # state that was authoritative when the transaction began.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            coord = interval_item_containing(text, "id(cl_report_ready)")
            ambiguity_start = coord.index("bool report_ambiguous_for_authority")
            ambiguity = coord[ambiguity_start : coord.index(";", ambiguity_start)]
            yield_start = coord.index("bool should_yield", ambiguity_start)
            yield_expression = coord[yield_start : coord.index(";", yield_start)]
            with self.subTest(config=config_name):
                self.assertIn("report_for_transaction", ambiguity)
                self.assertIn("!state_matches", ambiguity)
                self.assertIn("report_could_be_command", ambiguity)
                self.assertIn("report_ambiguous_for_authority", yield_expression)
                self.assertIn("!desired_is_off", yield_expression)
                self.assertIn("actual_duration != 0", yield_expression)
                self.assertIn("actual != id(cl_prior_confirmed_state)", yield_expression)

    def test_speed_change_does_not_yield_to_prior_confirmed_state(self) -> None:
        # A speed->speed query can echo the exact canonical state that was
        # authoritative before the command. Preserve that state across the
        # transaction invalidation and exclude it from the OEM-yield policy.
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            globals_block = top_level_block(text, "globals")
            prior = globals_block[globals_block.index("- id: cl_prior_confirmed_state") :]
            begin = script_blocks(text)["begin_transaction"]
            coord = interval_item_containing(text, "id(cl_report_ready)")
            yield_start = coord.index("bool should_yield")
            yield_expression = coord[yield_start : coord.index(";", yield_start)]
            promotion = coord.index("id(cl_prior_confirmed_state) = actual;")
            authority = coord.rindex("if (state_authoritative) {", 0, promotion)
            radio = top_level_block(text, radio_key)
            with self.subTest(config=config_name):
                self.assertIn('initial_value: "0xFF"', prior[:500])
                self.assertNotIn("id(cl_prior_confirmed_state) = 0xFF;", begin)
                self.assertIn("actual != id(cl_prior_confirmed_state)", yield_expression)
                self.assertLess(authority, promotion)
                # All promoted states are cmd & 0x3F, with OFF canonicalized
                # further to zero, so the 0xFF unknown sentinel cannot collide.
                self.assertIn("uint8_t canonical_state = cmd & 0x3F;", radio)
                self.assertIn("if (duration_nibble == 0) canonical_state = 0;", radio)

    def test_prior_confirmed_state_invalidation_tracks_authority_loss_reason(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            scripts = script_blocks(text)
            radio = top_level_block(text, radio_key)
            coordinator = interval_item_containing(text, "id(cl_report_ready)")
            invalidate = "id(cl_prior_confirmed_state) = 0xFF;"

            # Our own arm and serialized command/re-fire invalidate current
            # authority but retain the pre-command belief across command bursts.
            begin = scripts["begin_transaction"]
            tx = scripts["tx_burst"]
            self_command_start = tx.index("if (cmd != 0x66) {")
            self_command_end = tx.index("id(tx_burst_sender_id)", self_command_start)
            with self.subTest(config=config_name, site="self-owned invalidation"):
                self.assertNotIn(invalidate, begin)
                self.assertNotIn(invalidate, tx[self_command_start:self_command_end])
                self.assertIn("Self-command invalidation preserves", begin)
                self.assertIn("Self-command execution preserves", tx)

            # Every external-traffic authority loss discards the belief.
            external_sites = (
                radio[
                    radio.index("if (exact_frame && cmd == 0x66 &&") :
                    radio.index(
                        "\n          return;",
                        radio.index("if (exact_frame && cmd == 0x66 &&"),
                    )
                ],
                radio[
                    radio.index("if (id(cl_query_response_complete) &&") :
                    radio.index("Passive local-response repeat", radio.index("if (id(cl_query_response_complete) &&"))
                ],
                radio[
                    radio.index("if (remote_command_ok) {") :
                    radio.index(
                        "\n          return;", radio.index("if (remote_command_ok) {")
                    )
                ],
                radio[radio.index("// Any other strictly validated passive state observation") :],
            )
            for site_index, site in enumerate(external_sites):
                with self.subTest(config=config_name, external_site=site_index):
                    self.assertIn(invalidate, site)

            # Explicit revalidation/reset/expiry paths also discard stale
            # beliefs rather than carrying them into a later transaction.
            explicit_sites = (
                tx[tx.index("if (!id(cl_query_epoch_confirmation))") :],
                scripts["arm_manual_learn"],
                interval_item_containing(text, "Estimated timer deadline reached"),
                list_item_containing(text, "button", 'name: "Refresh Fan State"'),
                list_item_containing(text, "button", 'name: "Query Fan State (probe)"'),
                list_item_containing(text, "button", 'name: "Forget Remote ID"'),
            )
            for site_index, site in enumerate(explicit_sites):
                with self.subTest(config=config_name, explicit_site=site_index):
                    self.assertIn(invalidate, site)

            # Manual timeout, suspected OEM yield, and terminal FAILED
            # outcomes must poison the belief before a future transaction.
            manual_timeout_start = coordinator.index(
                "if (id(cl_query_epoch) && !id(cl_query_epoch_confirmation)"
            )
            manual_timeout_end = coordinator.index(
                "if (id(cl_query_epoch) &&", manual_timeout_start + 1
            )
            yield_start = coordinator.index("if (should_yield) {")
            yield_end = coordinator.index("} else {", yield_start)
            terminal_mismatch = coordinator[
                coordinator.rindex("} else {", 0, coordinator.index("FAILED after bounded attempts")) :
                coordinator.index(
                    "id(command_confirmation_status_sensor).publish_state(status);",
                    coordinator.index("FAILED after bounded attempts"),
                )
            ]
            for site_name, site in (
                ("manual timeout", coordinator[manual_timeout_start:manual_timeout_end]),
                ("OEM yield", coordinator[yield_start:yield_end]),
                ("terminal mismatch", terminal_mismatch),
            ):
                with self.subTest(config=config_name, failure_site=site_name):
                    self.assertIn(invalidate, site)

            # A command-query timeout is asymmetric: silence while a bounded
            # re-fire remains preserves the prior-state discriminator, but the
            # final no-consensus outcome invalidates it. Slice at the inner
            # else so a common pre-branch wipe cannot satisfy this structure.
            timeout_start = coordinator.index("// Query timeout does not consume")
            timeout_end = coordinator.index("      - if:", timeout_start)
            query_timeout = coordinator[timeout_start:timeout_end]
            pending_start = query_timeout.index("if (id(refire_left) > 0) {")
            terminal_start = query_timeout.index("} else {", pending_start)
            refire_pending_timeout = query_timeout[:terminal_start]
            terminal_timeout = query_timeout[terminal_start:]
            with self.subTest(config=config_name, failure_site="pending timeout"):
                self.assertNotIn(invalidate, refire_pending_timeout)
            with self.subTest(config=config_name, failure_site="terminal timeout"):
                self.assertIn(invalidate, terminal_timeout)

    def test_no_script_uses_a_literal_data_array(self) -> None:
        # Post-refactor, the only payload construction lives in tx_burst's
        # templated `data: !lambda`; no script should embed a literal
        # `data: [...]` byte array anymore (that was the pre-FIX-1 shape).
        scripts = script_blocks(self.text)
        for script_id, body in scripts.items():
            with self.subTest(script_id=script_id):
                self.assertEqual(packet_bytes(body), [])

    def test_script_ids_are_exactly_the_expected_set(self) -> None:
        scripts = script_blocks(self.text)
        self.assertEqual(
            set(scripts),
            {"tx_burst", "begin_transaction", "arm_manual_learn", "send_off",
             "send_low", "send_medium", "send_high", "send_timer"},
        )

    def test_timer_script_is_speed_aware(self) -> None:
        scripts = script_blocks(self.text)
        self.assertIn("send_timer", scripts)
        timer = scripts["send_timer"]

        # Takes a duration nibble parameter instead of hardcoding one.
        self.assertIn("duration_nibble: uint8_t", timer)

        # Combines the fan entity's CURRENT speed with the duration nibble
        # rather than assuming Low, and defaults to Low (0x90) when off.
        self.assertIn("id(quietcool_fan).speed", timer)
        self.assertIn("0x90", timer)
        self.assertIn("0xA0", timer)
        self.assertIn("0xB0", timer)
        self.assertIn("speed_nibble | duration_nibble", timer)

        # Routes through the shared join/arm site, never transmitting or
        # publishing anything itself.
        self.assertIn("id: begin_transaction", timer)
        self.assertNotIn("sx127x.send_packet", timer)
        self.assertNotIn("delay:", timer)
        self.assertNotIn("id(quietcool_fan).state = true;", timer)
        self.assertNotIn("id(quietcool_fan).speed = observed_speed;", timer)
        self.assertNotIn("id(quietcool_fan).publish_state();", timer)

    def test_timer_select_maps_every_duration_and_refuses_unsafe_none(self) -> None:
        select_block = top_level_block(self.text, "select")
        self.assertIn('name: "Fan Timer"', select_block)
        self.assertIn("id: fan_timer_select", select_block)
        # All five OEM durations plus None, each option mapped to its exact
        # duration nibble inside set_action.
        for option in ("None", "1 hour", "2 hours", "4 hours", "8 hours", "12 hours"):
            self.assertIn(f'"{option}"', select_block)
        for option, nibble in (
            ("1 hour", "0x1"),
            ("2 hours", "0x2"),
            ("4 hours", "0x4"),
            ("8 hours", "0x8"),
            ("12 hours", "0xC"),
        ):
            with self.subTest(option=option):
                self.assertIn(f'if (x == "{option}") nibble = {nibble};', select_block.replace("else if", "if"))
        self.assertIn("id(send_timer)->execute(nibble);", select_block)
        # The protocol has no non-actuating clear. None must never resend a
        # speed, because that can restart a fan whose timer already expired.
        self.assertIn("Timer None refused safely", select_block)
        self.assertIn("id(timer_select_synced_hours) = 0xFF;", select_block)
        for script_id in ("send_medium", "send_high", "send_low"):
            self.assertNotIn(f"id({script_id})->execute();", select_block)
        self.assertNotIn("sx127x.send_packet", select_block)
        self.assertNotIn("tx_burst", select_block)
        # Not optimistic: UI state comes only from the sync mirror.
        self.assertIn("optimistic: false", select_block)

    def test_timer_select_sync_mirrors_all_arming_sites_without_tx(self) -> None:
        # The 1 s sync interval mirrors timer_active/timer_armed_hours into
        # the select via publish_state (which never fires set_action).
        sync = interval_item_containing(self.text, "id(fan_timer_select).publish_state(desired)")
        self.assertIn("id(timer_active)", sync)
        self.assertIn("id(timer_armed_hours)", sync)
        self.assertNotIn("make_call", sync)
        self.assertNotIn("tx_burst", sync)
        self.assertNotIn("script.execute", sync)
        # Only a matching locally initiated timer confirmation can arm it.
        self.assertEqual(self.text.count("id(timer_armed_hours) = (uint8_t)"), 1)

    def test_boot_never_transmits_any_packet(self) -> None:
        # Hard invariant: boot/OTA/reconnect may initialize telemetry but may
        # not execute tx_burst. Even a non-energizing query remains an explicit
        # user action instead of a boot side effect.
        for config_name, text, send_action in (
            ("SX1278", self.text, "sx127x.send_packet"),
            ("SX1262", self.v3_text, "sx126x.send_packet"),
        ):
            with self.subTest(config=config_name):
                boot = top_level_block(text, "esphome")
                self.assertNotIn(send_action, boot)
                self.assertNotIn("fan.turn_on", boot)
                self.assertNotIn("fan.turn_off", boot)
                for script_id in (
                    "send_off",
                    "send_low",
                    "send_medium",
                    "send_high",
                    "send_timer",
                ):
                    self.assertNotIn(script_id, boot)
                self.assertEqual(boot.count("script.execute"), 0)
                self.assertNotIn("Post-boot status query", boot)
                self.assertNotIn("- delay: 12s", boot)
                self.assertNotIn("id(quietcool_fan)", boot)

    def test_bare_turn_on_after_boot_defaults_to_low_not_high(self) -> None:
        # ESPHome's FanCall::validate_() maps "turn on with no explicit
        # speed while Fan::speed == 0" to full speed (High). With
        # restore_mode: NO_RESTORE, speed starts at its class default of 0
        # unless something else sets it first. The custom component pre-seeds
        # speed to 1 (Low) in setup() via a raw field write (no publish, no TX).
        fan_block = top_level_block(self.text, "fan")
        self.assertIn("restore_mode: NO_RESTORE", fan_block)
        header = CONFIRMED_FAN_HEADER.read_text()
        setup = header[header.index("void setup()") : header.index("void dump_config()")]
        self.assertIn("this->speed == 0", setup)
        self.assertIn("this->speed = 1", setup)
        self.assertNotIn("publish_state", setup)

    def test_fan_restore_mode_prevents_boot_time_publish(self) -> None:
        # Public state must never be restored as though it were a physical RF
        # observation. The custom component also deliberately never calls the
        # Fan restore/apply path from setup().
        fan_block = top_level_block(self.text, "fan")
        self.assertIn("restore_mode: NO_RESTORE", fan_block)

    def test_both_fan_entities_are_confirmation_driven_not_optimistic(self) -> None:
        # The stock TemplateFan publishes requested values synchronously from
        # control(), before RF confirmation. Both targets must instead route
        # calls to scripts while keeping public state untouched until RX.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                fan_block = top_level_block(text, "fan")
                self.assertIn("platform: quietcool_confirmed_fan", fan_block)
                self.assertNotIn("platform: template", fan_block)
                self.assertNotIn("on_state:", fan_block)
                for key, script_id in (
                    ("off_script", "send_off"),
                    ("low_script", "send_low"),
                    ("medium_script", "send_medium"),
                    ("high_script", "send_high"),
                ):
                    self.assertIn(f"{key}: {script_id}", fan_block)

        self.assertTrue(CONFIRMED_FAN_HEADER.exists())
        self.assertTrue(CONFIRMED_FAN_PLATFORM.exists())
        header = CONFIRMED_FAN_HEADER.read_text()
        control = header[header.index("void control(") :]
        for script in ("off", "low", "medium", "high"):
            self.assertIn(f"{script}_script_->execute()", control)
        self.assertIn("call.get_state()", control)
        self.assertIn("call.get_speed()", control)
        self.assertNotIn("this->state =", control)
        self.assertNotIn("this->speed =", control)
        self.assertNotIn("publish_state", control)

    def test_no_redundant_fan_or_timer_buttons_remain(self) -> None:
        # Off/Low/Medium/High live on the fan entity (HA's fan card), and
        # the timer moved to the Fan Timer select; the former one-tap
        # buttons are deliberately gone.
        for name in (
            '"Fan OFF"',
            '"Fan Low"',
            '"Fan Medium"',
            '"Fan High"',
            '"Timer 1H"',
            '"Timer 2H"',
            '"Timer 4H"',
        ):
            self.assertNotIn(f"name: {name}", self.text)

    def test_prg_long_press_off_is_unconditional(self) -> None:
        # The physical PRG long-press is now the one-tap guaranteed-off
        # path; fan.turn_off always reaches the custom control() and therefore
        # send_off even when the entity already reads as off. It must not be
        # gated by a condition.
        binary_block = top_level_block(self.text, "binary_sensor")
        index = binary_block.index("fan.turn_off: quietcool_fan")
        window = binary_block[max(0, index - 500) : index]
        self.assertIn("min_length: 1000ms", window)
        self.assertNotIn("if:", binary_block[binary_block.rindex("min_length: 1000ms", 0, index) : index])

    def test_confirmed_zero_duration_always_clears_timer_metadata(self) -> None:
        # Every confirmed report atomically replaces timer metadata. A timer
        # request is never pre-armed, and OFF/continuous always clears it.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            coord = interval_item_containing(text, "id(cl_report_ready)")
            reconcile_start = coord.index("uint32_t actual_timer_hours")
            timer_known = coord.index(
                "id(timer_state_known) = timer_authoritative;", reconcile_start
            )
            authority_guard = coord.index("if (timer_authoritative) {", timer_known)
            clear_index = coord.index("id(timer_active) = false;", authority_guard)
            reconcile = coord[reconcile_start:clear_index]
            with self.subTest(config=config_name):
                self.assertIn("if (actual_timer_hours > 0)", reconcile)
                self.assertIn("} else {", reconcile)
                self.assertNotIn("if (!state_matches)", reconcile)
                self.assertTrue(timer_known < authority_guard < clear_index)

    def test_mismatch_yield_policy_never_yields_off(self) -> None:
        coord = interval_item_containing(self.text, "id(cl_report_ready)")
        self.assertIn("report_could_be_command", coord)
        self.assertIn("desired_is_off", coord)
        ambiguity_start = coord.index("bool report_ambiguous_for_authority")
        ambiguity = coord[ambiguity_start : coord.index(";", ambiguity_start)]
        self.assertIn("report_could_be_command", ambiguity)
        self.assertNotIn("!desired_is_off", ambiguity)
        yield_start = coord.index("bool should_yield", ambiguity_start)
        yield_expression = coord[yield_start : coord.index(";", yield_start)]
        self.assertIn("!desired_is_off", yield_expression)
        self.assertIn("possible OEM override", coord)
        # Yield cancels the re-fires; the OFF path must keep them.
        yield_index = coord.index("possible OEM override")
        self.assertIn("id(refire_left) = 0;", coord[ambiguity_start:yield_index])
        self.assertIn("spaced re-fire pending", coord[yield_index:])

    def test_equivalent_requests_join_every_active_transaction(self) -> None:
        # A caller may retry any HA service while the non-optimistic entity
        # is awaiting RF confirmation. Equivalent requests must be
        # observations, not fresh transactions that reset the bounded
        # command/refire budget. All five wrappers share ONE join/arm site:
        # begin_transaction. The join lambda generalizes: duration-zero
        # requests are equivalent to any active duration-zero transaction
        # (so a re-aimed B0 absorbs a fresh 90), everything else must match
        # on the lower six bits.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                scripts = script_blocks(text)
                begin = scripts["begin_transaction"]
                self.assertIn("if (!id(cl_active)) return false;", begin)
                self.assertIn("bool both_off = ((cmd & 0x0F) == 0) &&", begin)
                self.assertIn("((id(cl_desired_cmd) & 0x0F) == 0);", begin)
                self.assertIn(
                    "(id(cl_desired_cmd) & 0x3F) == (cmd & 0x3F);", begin
                )
                self.assertIn("return both_off || same_state;", begin)
                # STRUCTURAL: the join branch (before else:) must contain the
                # observation log and NO arming/reset/TX tokens; every arm
                # token must live strictly inside the else branch. This
                # catches an accidental duplication of the arm block into
                # the join path (which would silently defeat the fix).
                else_index = begin.index("          else:")
                joined = begin[:else_index]
                fresh = begin[else_index:]
                self.assertIn("joined active transaction", joined)
                for forbidden in (
                    "id: tx_burst",
                    "id(refire_cmd) =",
                    "id(refire_left) =",
                    "id(cl_active) = true;",
                    "id(cl_desired_cmd) = cmd;",
                    "id(cl_command_attempts) =",
                    "id(cl_candidate_total_count) =",
                    "id(fan_state_known) =",
                    "id(timer_state_known) =",
                    "publish_state",
                ):
                    self.assertNotIn(forbidden, joined)
                for required in (
                    "id(refire_cmd) = cmd;",
                    "id(refire_left) = refires;",
                    "id(cl_active) = true;",
                    "id(cl_desired_cmd) = cmd;",
                ):
                    self.assertIn(required, fresh)

    def test_ambiguous_oem_yield_never_promotes_physical_or_timer_authority(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            coordinator = interval_item_containing(text, "id(cl_report_ready)")
            ambiguity = coordinator.index("bool report_ambiguous_for_authority")
            fan_authority = coordinator.index(
                "id(fan_state_known) = state_authoritative;", ambiguity
            )
            timer_authority = coordinator.index(
                "id(timer_state_known) = timer_authoritative;", fan_authority
            )
            timer_guard = coordinator.index("if (timer_authoritative) {", timer_authority)
            yield_branch = coordinator.index("if (should_yield) {", timer_guard)
            untrusted = coordinator[fan_authority:yield_branch]
            with self.subTest(config=config_name):
                self.assertTrue(
                    ambiguity < fan_authority < timer_authority < timer_guard < yield_branch
                )
                self.assertIn(
                    "bool report_authoritative = !report_ambiguous_for_authority;",
                    coordinator,
                )
                self.assertIn("id(fan_state_known_sensor).publish_state(false);", untrusted)
                self.assertIn("id(timer_state_known_sensor).publish_state(false);", untrusted)

    def test_pending_energizing_retry_cannot_publish_confirmed_off(self) -> None:
        # A non-command-shaped OFF report can be valid fan state, but while an
        # ON/timer mismatch still has a future re-fire it must remain unknown.
        # Otherwise Confirmed Off can briefly become true immediately before
        # the controller intentionally energizes the fan again.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            coordinator = interval_item_containing(text, "id(cl_report_ready)")
            future = coordinator.index("bool future_energizing_work")
            authority = coordinator.index("bool state_authoritative", future)
            confirmed_off = coordinator.index(
                "id(fan_confirmed_off_sensor).publish_state(actual_duration == 0);",
                authority,
            )
            expression = coordinator[future:authority]
            with self.subTest(config=config_name):
                self.assertIn("report_for_transaction && !state_matches", expression)
                self.assertIn("!desired_is_off", expression)
                self.assertIn("id(refire_left) > 0", expression)
                self.assertIn("!should_yield", expression)
                authority_expr = coordinator[
                    authority : coordinator.index(";", authority)
                ]
                self.assertIn("!future_energizing_work", authority_expr)
                self.assertLess(authority, confirmed_off)

    def test_consensus_dedup_floor_is_the_validated_60ms(self) -> None:
        # Anchored to the actual candidate-dedup comparison, not the whole
        # radio block, so an unrelated 60UL elsewhere can't satisfy it.
        radio = top_level_block(self.text, "sx127x")
        anchor = radio.index("id(cl_candidate_last_ms)) >= 60UL")
        self.assertNotIn(">= 95UL", radio)
        # The honest-timing comment (frame airtime included) must sit
        # directly above that comparison.
        window = radio[max(0, anchor - 1500) : anchor]
        self.assertIn("102 ms", window)

    def test_user_command_closes_manual_learn_window(self) -> None:
        tx_burst = script_blocks(self.text)["tx_burst"]
        self.assertIn("Learn window cancelled by user command", tx_burst)
        cancel_index = tx_burst.index("if (id(learn_active))")
        send_index = tx_burst.index("send_packet")
        self.assertLess(cancel_index, send_index)

    def test_timer_select_none_never_transmits_or_calls_a_state_script(self) -> None:
        select_block = top_level_block(self.text, "select")
        self.assertIn("Timer None refused safely", select_block)
        self.assertIn("id(timer_select_synced_hours) = 0xFF;", select_block)
        for forbidden in ("id(send_low)->execute", "id(send_medium)->execute", "id(send_high)->execute", "tx_burst"):
            self.assertNotIn(forbidden, select_block)

    def test_refresh_button_sends_only_the_status_query(self) -> None:
        button = list_item_containing(
            self.text, "button", 'name: "Refresh Fan State"'
        )
        self.assertIn("id: tx_burst", button)
        self.assertRegex(button, r"(?m)^\s+cmd:\s*0x66\s*$")
        self.assertIn("id(fan_state_known_sensor).publish_state(false);", button)
        self.assertIn("id(fan_confirmed_off_sensor).publish_state(false);", button)
        for forbidden in ("0x90", "0x9F", "0xAF", "0xBF", "send_timer"):
            self.assertNotIn(forbidden, button)

    def test_setup_entities_are_config_category_and_learn_forget_disabled(self) -> None:
        for marker in ('name: "Learn Remote ID"', 'name: "Forget Remote ID"'):
            index = self.text.index(marker)
            window = self.text[index : index + 400]
            self.assertIn("entity_category: config", window)
            self.assertIn("disabled_by_default: true", window)
        light_block = top_level_block(self.text, "light")
        self.assertIn("entity_category: config", light_block)

    def test_rx_validation_rejects_malformed_frames(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("x.size() != 6", radio)
        self.assertIn("x[4] != x[5]", radio)
        self.assertIn("sender_id != id(learned_sender_id)", radio)
        for nibble in ("0x90", "0xA0", "0xB0"):
            self.assertIn(nibble, radio)
        for nibble in ("0x0", "0x1", "0x2", "0x4", "0x8", "0xC", "0xF"):
            self.assertIn(f"duration_nibble == {nibble}", radio)

    def test_oem_lower_six_bit_state_comparison_model(self) -> None:
        # The firmware compares desired/reported state after shifting both
        # bytes left two bits, which discards capability metadata bits 7:6.
        # OFF is the one special case: remembered speed is a wildcard.
        self.assertTrue(oem_state_matches(0x9F, 0x1F))  # capability unknown
        self.assertTrue(oem_state_matches(0x9F, 0x9F))  # 2-speed metadata
        self.assertTrue(oem_state_matches(0xAF, 0x6F))  # 1-speed metadata
        self.assertTrue(oem_state_matches(0xAF, 0xEF))  # 3-speed metadata
        for off_report in (0x00, 0x10, 0x80, 0x90, 0xB0, 0xF0):
            with self.subTest(off_report=off_report):
                self.assertTrue(oem_state_matches(0x90, off_report))
        self.assertFalse(oem_state_matches(0x9F, 0xB0))
        self.assertFalse(oem_state_matches(0xBF, 0x1F))

    def test_correlated_response_decoder_uses_firmware_state_fields(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("uint8_t state_speed = (cmd >> 4) & 0x03;", radio)
        self.assertIn("uint8_t canonical_state = cmd & 0x3F;", radio)
        self.assertIn("uint8_t capability = cmd >> 6;", radio)
        self.assertIn("bool state_encoding_ok", radio)
        self.assertIn("duration_nibble == 0 || state_speed != 0", radio)

        # Direction comes from our bounded local response epoch, never from
        # bit 7: both command reports and direct-query replies, including live
        # 1F and B0/90 forms, must reach the same decoder/consensus branch.
        response_start = radio.index("bool local_response_epoch")
        response_end = radio.index("// Normal six-byte traffic", response_start)
        response = radio[response_start:response_end]
        self.assertNotIn("cmd & 0x80", response)
        self.assertNotIn("cmd < 0x80", response)
        self.assertIn("id(cl_query_window)", response)
        self.assertIn("response_min_ms", response)
        self.assertIn("response_window_ms", response)

    def test_correlated_recovery_never_weakens_normal_validation(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        # A one-bit known-header repair and an overlength first-six-byte
        # recovery are only candidates inside a locally-opened response
        # window. Normal traffic retains exact length, duplicate, and sender.
        self.assertIn("header_bit_errors <= 1", radio)
        self.assertIn("if (x.size() < 6)", radio)
        self.assertIn("bool recovered_candidate", radio)
        self.assertIn("cl_candidate_exact_count", radio)
        self.assertIn("cl_candidate_total_count", radio)
        self.assertIn("candidate_exact_count >= 1 || candidate_total_count >= 3", radio)

        normal_start = radio.index("// Normal six-byte traffic")
        normal = radio[normal_start:]
        self.assertIn("if (x.size() != 6)", normal)
        self.assertIn("if (x[4] != x[5])", normal)
        self.assertIn("sender_id != id(learned_sender_id)", normal)

    def test_rx_callback_never_executes_a_transmit_action(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertNotRegex(radio, r"(?m)^\s+- script\.execute:")
        self.assertNotRegex(radio, r"(?m)^\s+- sx127x\.send_packet:")
        self.assertNotIn("id(tx_burst).execute", radio)

    def test_rx_handles_wake_query_and_special_frame_without_publishing(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("cmd == 0x66", radio)
        self.assertRegex(
            radio,
            r"x\[0\] == 0xCE && \(sender_id & 0x00FFFFFFUL\) ==\s*"
            r"\(id\(learned_sender_id\) & 0x00FFFFFFUL\)",
        )

        # Neither branch may publish the fan entity or execute a script.
        # CE is log-only; an external query is an OEM-priority cancellation
        # event, but still cannot actuate or publish fan state directly.
        wake_start = radio.index("if (exact_frame && cmd == 0x66 &&")
        wake_end = radio.index("return;", wake_start) + len("return;")
        wake_branch = radio[wake_start:wake_end]
        self.assertNotIn("script.execute", wake_branch)
        self.assertNotIn("quietcool_fan", wake_branch)

        ce_start = radio.index("if (exact_frame && x[0] == 0xCE")
        ce_end = radio.index("return;", ce_start) + len("return;")
        ce_branch = radio[ce_start:ce_end]
        self.assertNotIn("publish_state", ce_branch)
        self.assertNotIn("script.execute", ce_branch)
        self.assertNotIn("quietcool_fan", ce_branch)

    def test_learn_mode_accepts_only_two_matching_valid_bursts(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        learn_start = radio.index("if (id(learn_active))")
        normal_sender = radio.index("sender_id != id(learned_sender_id)")
        learn_block = radio[learn_start:normal_sender]

        # Prefix and command structure are validated before a candidate can
        # become the persisted sender. FIX 1b: only a real state-command
        # frame qualifies as a learn candidate - the 66 66 wake/status
        # query is passive traffic and must never confirm a learn by
        # itself (see test_learn_command_ok_excludes_wake_query below).
        self.assertIn("x[0] != 0xCB || !learn_command_ok", learn_block)
        self.assertIn("speed_ok && duration_ok", radio)

        # A different sender restarts the count. A matching sender confirms
        # only across the explicit different-burst/time-window guard.
        self.assertIn(
            "candidate_sender_id != id(learn_candidate_id)", learn_block
        )
        self.assertIn("candidate_age > 600", learn_block)
        self.assertIn("candidate_age < 60000", learn_block)
        self.assertIn(
            "(now - id(learn_window_started)) >= 120000UL", learn_block
        )
        self.assertIn("id(learn_candidate_id) = candidate_sender_id;", learn_block)
        self.assertIn("id(learned_sender_id) = candidate_sender_id;", learn_block)
        self.assertIn("global_preferences->sync();", learn_block)
        value_write = learn_block.index("id(learned_sender_id) = candidate_sender_id;")
        component_update = learn_block.index("learned_sender_id->update();")
        preference_sync = learn_block.index("global_preferences->sync();")
        self.assertTrue(value_write < component_update < preference_sync)
        self.assertIn("id(learn_active) = false;", learn_block)
        self.assertIn("id(remote_sender_id_sensor).publish_state", learn_block)
        self.assertIn("id(learn_confirm_until)", learn_block)

        # Every path through the learn branch returns before normal RX state
        # publication and therefore cannot publish fan state.
        self.assertNotIn("quietcool_fan", learn_block)
        self.assertNotIn("script.execute", learn_block)
        self.assertNotIn("sx127x.send_packet", learn_block)

    def test_learn_window_timeout_and_auto_rearm_are_rx_storage_only(self) -> None:
        learn_interval = interval_item_containing(self.text, "uint32_t learn_elapsed")
        self.assertIn("120000UL", learn_interval)
        self.assertIn("id(learn_auto_mode)", learn_interval)
        self.assertIn("id(learn_candidate_id) = 0;", learn_interval)
        self.assertNotIn("script.execute", learn_interval)
        self.assertNotIn("sx127x.send_packet", learn_interval)

    def test_learn_command_ok_excludes_wake_query(self) -> None:
        # FIX 1b: only a real state-command frame (speed nibble 9/A/B with
        # a valid duration nibble) may start or confirm a learn candidate.
        # Before this fix, `cmd == 0x66` also qualified, so two passive
        # 66 66 wake/status frames overheard from a neighbor's unit could
        # complete a learn without any deliberate button press.
        radio = top_level_block(self.text, "sx127x")
        self.assertIn(
            "bool learn_command_ok = speed_ok && duration_ok;", radio
        )
        self.assertNotIn("(cmd == 0x66) || (speed_ok && duration_ok)", radio)
        self.assertNotIn("cmd == 0x66) || (speed_ok", radio)

    def test_auto_learn_rearm_is_bounded_to_fifteen_minutes_since_armed(self) -> None:
        # FIX 1a: unprovisioned auto-learn (learn_auto_mode true while
        # learned_sender_id == 0) must not re-arm its 120-second window
        # forever - otherwise a parked unprovisioned unit stays listening
        # indefinitely and can eventually learn a neighbor's ID from
        # cross-talk. Auto-learn may only keep re-arming for 15 minutes
        # after learn_auto_armed_at; past that ceiling it disarms fully,
        # exactly like an expired manual window.
        interval_block = top_level_block(self.text, "interval")
        self.assertIn("id(learn_auto_armed_at)", interval_block)
        self.assertIn("900000UL", interval_block)
        self.assertRegex(
            interval_block,
            r"\(millis\(\)\s*-\s*id\(learn_auto_armed_at\)\)\s*<\s*900000UL",
        )
        rearm_block = interval_block[interval_block.index("if (!id(learn_active))") :]
        within_ceiling = rearm_block.index("within_auto_ceiling")
        rearm_call = rearm_block.index("id(learn_window_started) = millis();")
        disarm_call = rearm_block.index("id(learn_active) = false;")
        self.assertTrue(within_ceiling < rearm_call < disarm_call)

        # Both entry points into unprovisioned auto-mode - cold boot and a
        # Forget - reset the ceiling anchor, so each deliberate
        # re-provisioning attempt gets its own fresh 15 minutes rather than
        # inheriting whatever remained since the original boot.
        boot = top_level_block(self.text, "esphome")
        self.assertIn("id(learn_auto_armed_at) = millis();", boot)
        forget_button = list_item_containing(
            self.text, "button", 'name: "Forget Remote ID"'
        )
        self.assertIn("id(learn_auto_armed_at) = millis();", forget_button)

        # The bounded manual windows (Learn Remote ID button, PRG
        # long-press) already self-expire at 120 seconds without re-arming
        # and must not reference the auto-learn ceiling at all.
        learn_button = list_item_containing(
            self.text, "button", 'name: "Learn Remote ID"'
        )
        self.assertNotIn("learn_auto_armed_at", learn_button)
        prg = list_item_containing(self.text, "binary_sensor", 'name: "PRG Button"')
        very_long = prg[prg.index("min_length: 5000ms") :]
        self.assertNotIn("learn_auto_armed_at", very_long)

    def test_forget_survives_reboot_via_seed_suppressed_flag(self) -> None:
        # FIX 2: a unit MAY compile with a nonzero seed (the public
        # template ships 0x00000000)
        # (quietcool_sender_id), and on_boot previously reseeded that
        # compiled default whenever learned_sender_id read 0 - which is
        # exactly what a fresh Forget leaves behind. Since ESPHome restores
        # persisted globals (priority 800) before on_boot runs (priority
        # -100), that reseed happened deterministically on every reboot,
        # silently reverting the Forget. seed_suppressed, persisted just
        # like learned_sender_id, closes that hole.
        globals_block = top_level_block(self.text, "globals")
        start = globals_block.index("- id: seed_suppressed")
        next_id = globals_block.find("\n  - id:", start + 1)
        item = globals_block[start:] if next_id == -1 else globals_block[start:next_id]
        self.assertIn("type: bool", item)
        self.assertIn("restore_value: true", item)
        self.assertIn('initial_value: "false"', item)

        boot = top_level_block(self.text, "esphome")
        self.assertIn(
            "if (id(learned_sender_id) == 0 && id(seed_suppressed))", boot
        )
        self.assertIn(
            "else if (id(learned_sender_id) == 0 && sender_seed != 0)", boot
        )
        # A skipped seed and a real reseed are logged distinctly, so a
        # compiled default can never be mistaken for a learned value.
        self.assertIn("suppressed by a prior Forget", boot)
        self.assertIn("Seeding compiled default sender ID", boot)

        forget_button = list_item_containing(
            self.text, "button", 'name: "Forget Remote ID"'
        )
        self.assertIn("id(seed_suppressed) = true;", forget_button)
        self.assertIn("seed_suppressed->update();", forget_button)

        # A successful learn clears the suppression flag again.
        radio = top_level_block(self.text, "sx127x")
        learn_start = radio.index("if (id(learn_active))")
        normal_sender = radio.index("sender_id != id(learned_sender_id)")
        learn_block = radio[learn_start:normal_sender]
        self.assertIn("id(seed_suppressed) = false;", learn_block)
        self.assertIn("seed_suppressed->update();", learn_block)
        clear_index = learn_block.index("id(seed_suppressed) = false;")
        accept_index = learn_block.index(
            "id(learned_sender_id) = candidate_sender_id;"
        )
        self.assertGreater(clear_index, accept_index)

    def test_manual_learn_and_forget_controls_present(self) -> None:
        arm = script_blocks(self.text)["arm_manual_learn"]
        self.assertIn("id(learn_active) = true;", arm)
        self.assertIn("id(learn_auto_mode) = false;", arm)
        self.assertIn("id(learn_window_started) = millis();", arm)
        self.assertIn("id(tx_burst).stop();", arm)
        self.assertIn("id(cl_query_epoch) = false;", arm)
        self.assertIn("id(cl_query_epoch_confirmation) = false;", arm)
        self.assertNotIn("sx127x.send_packet", arm)
        # A comment may NAME learn_auto_armed_at (to say it's untouched);
        # the assignment itself must not exist here.
        self.assertNotIn("id(learn_auto_armed_at) =", arm)

        learn_button = list_item_containing(
            self.text, "button", 'name: "Learn Remote ID"'
        )
        self.assertIn("script.execute: arm_manual_learn", learn_button)

        forget_button = list_item_containing(
            self.text, "button", 'name: "Forget Remote ID"'
        )
        self.assertIn("id(learned_sender_id) = 0;", forget_button)
        self.assertIn("learned_sender_id->update();", forget_button)
        self.assertIn("global_preferences->sync();", forget_button)
        self.assertIn("id(learn_active) = true;", forget_button)
        self.assertIn("id(learn_auto_mode) = true;", forget_button)
        self.assertIn('publish_state("unset")', forget_button)
        self.assertIn("id(tx_burst).stop();", forget_button)
        self.assertIn("id(cl_query_epoch) = false;", forget_button)
        self.assertIn("id(cl_query_epoch_confirmation) = false;", forget_button)
        for banned in ("sx127x.send_packet", "script.execute", "fan.turn_on", "fan.turn_off"):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, forget_button)

    def test_entering_learn_mode_invalidates_all_physical_state_knowledge(self) -> None:
        # Learn button and PRG very-long press share arm_manual_learn (one
        # teardown site); Forget keeps its own inline teardown because it
        # also clears the sender and re-arms auto-learn.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                arm = script_blocks(text)["arm_manual_learn"]
                for token in (
                    "id(fan_state_known) = false;",
                    "id(timer_state_known) = false;",
                    "id(fan_state_known_sensor).publish_state(false);",
                    "id(timer_state_known_sensor).publish_state(false);",
                    "id(fan_confirmed_off_sensor).publish_state(false);",
                ):
                    self.assertIn(token, arm)
                learn = list_item_containing(text, "button", 'name: "Learn Remote ID"')
                self.assertIn("script.execute: arm_manual_learn", learn)
                prg = list_item_containing(text, "binary_sensor", 'name: "PRG Button"')
                very_long = prg[prg.index("min_length: 5000ms"):]
                self.assertIn("script.execute: arm_manual_learn", very_long)
                forget = list_item_containing(text, "button", 'name: "Forget Remote ID"')
                self.assertIn("id(fan_state_known) = false;", forget)
                self.assertIn("id(timer_state_known) = false;", forget)

    def test_prg_very_long_press_enters_manual_learn_without_collision(self) -> None:
        prg = list_item_containing(self.text, "binary_sensor", 'name: "PRG Button"')
        self.assertIn("min_length: 1000ms", prg)
        self.assertIn("max_length: 4999ms", prg)
        self.assertIn("min_length: 5000ms", prg)
        self.assertIn("max_length: 10000ms", prg)
        very_long = prg[prg.index("min_length: 5000ms") :]
        self.assertIn("script.execute: arm_manual_learn", very_long)
        self.assertNotIn("fan.turn_off", very_long)

    def test_remote_sender_id_text_sensor_present(self) -> None:
        item = list_item_containing(
            self.text, "text_sensor", 'name: "Remote Sender ID"'
        )
        self.assertIn("platform: template", item)
        self.assertIn("id: remote_sender_id_sensor", item)
        self.assertIn("update_interval: never", item)

    def test_readme_documents_complete_learn_mode_flow(self) -> None:
        readme = self.readme_text
        self.assertIn("## Learn mode / porting to your own fan", readme)
        for required in (
            'quietcool_sender_id: "0x00000000"',
            "First-boot",
            "Learn Remote ID",
            "Forget Remote ID",
            "more than 600 ms",
            "less than 60",
            "NVS",
            "survives ordinary reboot, OTA",
            "full flash/NVS erase",
            "two-burst neighbor guard",
            "learn-active.png",
            "learn-confirmed.png",
        ):
            with self.subTest(required=required):
                self.assertIn(required, readme)

    def test_rx_deduplicates_oem_bursts(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("last_valid_rx_command", radio)
        self.assertIn("last_valid_rx_time", radio)
        self.assertIn("millis()", radio)

        # Anchor suppression to the first accepted frame for just longer than
        # one complete three-frame train (~204 ms at the observed callback
        # cadence). A sliding timestamp chained two distinct same-command
        # presses at ~400 ms into one indefinitely extended duplicate window.
        self.assertIn("(now - id(last_valid_rx_time)) < 300", radio)

        # Suppressed repeats must return before either tracker is refreshed.
        # The next same-command burst can therefore be accepted once the
        # fixed first-frame window expires.
        is_duplicate_decl = radio.index("bool is_duplicate")
        command_write = radio.index("id(last_valid_rx_command) = cmd;", is_duplicate_decl)
        time_write = radio.index("id(last_valid_rx_time) = now;", is_duplicate_decl)
        if_duplicate = radio.index("if (is_duplicate)", is_duplicate_decl)
        duplicate_return = radio.index("return;", if_duplicate)
        self.assertLess(is_duplicate_decl, if_duplicate)
        self.assertLess(duplicate_return, command_write)
        self.assertLess(duplicate_return, time_write)

    def test_rx_accepts_neutral_off_80_for_diagnostics_only(self) -> None:
        # Observed live from a second unit's OEM remote: its Off
        # button transmits 80 80 - speed nibble 8
        # ("no remembered speed"), duration 0 - which the original
        # downstairs-derived 9/A/B whitelist rejected, leaving the entity
        # stuck on the previous state. 0x80 must be decodable as OFF, but
        # only exactly 0x80: nibble-8 frames with a nonzero duration stay
        # rejected until physically observed, and 0x80 must never appear in
        # any TX payload.
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("bool off_neutral = (cmd == 0x80);", radio)
        self.assertIn("if (!state_encoding_ok)", radio)
        # It participates in OEM command classification, but passive RX never
        # mutates the safety fan entity.
        self.assertIn("((speed_ok && duration_ok) || off_neutral)", radio)
        self.assertNotIn("id(quietcool_fan).publish_state();", radio)
        # No TX path may carry 0x80: the only send_packet lives in tx_burst,
        # whose payload comes from the wrapper scripts' cmd bytes - none of
        # which may be 0x80.
        scripts = top_level_block(self.text, "script")
        self.assertNotIn("0x80", scripts)

    def test_learn_accept_seeds_burst_dedup_tracker(self) -> None:
        # Seen on a second-unit onboarding: learn accepted on a
        # frame of the second OEM burst, and that burst's remaining 45 ms
        # repeats - now matching the freshly learned sender - fell through
        # to the observed-state path and published the LEARNING press as
        # live fan state ("stuck Low"). The accept block must seed the
        # sliding dedup tracker with the accepting frame so those tail
        # repeats are suppressed like any other intra-burst duplicate.
        radio = top_level_block(self.text, "sx127x")
        accept = radio.index("Accepted and persisted sender ID")
        seed_cmd = radio.rindex("id(last_valid_rx_command) = cmd;", 0, accept)
        seed_time = radio.rindex("id(last_valid_rx_time) = now;", 0, accept)
        persist = radio.index("global_preferences->sync();")
        # The seed writes sit inside the accept block: after the NVS
        # persist call, before the accept log/return.
        self.assertGreater(seed_cmd, persist)
        self.assertGreater(seed_time, persist)
        self.assertLess(seed_cmd, accept)
        self.assertLess(seed_time, accept)

    def test_rx_invalidates_but_never_publishes_passive_state_while_tx_in_flight(self) -> None:
        # FIX 4: our own TX cannot self-receive (half-duplex), so this only
        # ever matters for genuine concurrent OEM traffic. The frame is
        # still counted in diagnostics; only the entity publish is skipped.
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("id(tx_burst).is_running()", radio)

        diag_index = radio.index(
            "id(rx_valid_count_sensor).publish_state(id(rx_valid_count_sensor).state + 1);"
        )
        guard_index = radio.index("id(tx_burst).is_running()")
        invalidate_index = radio.index("id(fan_state_known) = false;", diag_index)
        self.assertLess(diag_index, guard_index)
        self.assertLess(invalidate_index, guard_index)
        self.assertNotIn("id(quietcool_fan).publish_state();", radio)

    def test_authoritative_oem_command_invalidates_known_state_before_rx_early_return(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        remote_start = radio.index("if (remote_command_ok) {")
        running_return = radio.index("if (id(tx_burst).is_running())", remote_start)
        fan_unknown = radio.index("id(fan_state_known) = false;", remote_start)
        timer_unknown = radio.index("id(timer_state_known) = false;", remote_start)
        fan_sensor_unknown = radio.index(
            "id(fan_state_known_sensor).publish_state(false);", remote_start
        )
        timer_sensor_unknown = radio.index(
            "id(timer_state_known_sensor).publish_state(false);", remote_start
        )
        self.assertTrue(remote_start < fan_unknown < running_return)
        self.assertTrue(remote_start < timer_unknown < running_return)
        self.assertTrue(fan_unknown < fan_sensor_unknown < running_return)
        self.assertTrue(timer_unknown < timer_sensor_unknown < running_return)
        stop_index = radio.index("id(tx_burst).stop();", remote_start)
        remote_return = radio.index("return;", stop_index)
        self.assertTrue(timer_sensor_unknown < stop_index < remote_return < running_return)

    def test_only_coordinator_publishes_confirmed_state_without_control_path(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertNotIn("id(quietcool_fan).publish_state();", radio)
        self.assertIn("retained as diagnostics", radio)
        coordinator = interval_item_containing(self.text, "id(cl_report_ready)")
        self.assertIn("if (state_authoritative)", coordinator)
        self.assertIn("id(quietcool_fan).publish_state();", coordinator)
        self.assertNotIn("quietcool_fan).turn_on(", radio)
        self.assertNotIn("quietcool_fan).turn_off(", radio)
        self.assertNotIn("quietcool_fan).make_call(", radio)
        self.assertNotIn("fan.turn_on", radio)
        self.assertNotIn("fan.turn_off", radio)

    def test_confirmed_publish_is_raw_mutation_inside_authoritative_branch(self) -> None:
        # The old rf_echo_guard flag was removed as dead code: the
        # confirmation-driven fan platform has no publish callback, so the
        # echo protection is architectural. Pin the structure instead: the
        # ONLY fan publish_state() sits inside the coordinator's
        # state_authoritative branch, immediately after raw field mutation,
        # and control() (pinned elsewhere) contains no publish at all.
        coordinator = interval_item_containing(self.text, "id(cl_report_ready)")
        authoritative = coordinator.index("if (state_authoritative) {")
        publish = coordinator.index("id(quietcool_fan).publish_state();")
        self.assertGreater(publish, authoritative)
        mutate = coordinator.index("id(quietcool_fan).state = actual_duration != 0;")
        self.assertTrue(authoritative < mutate < publish)
        self.assertEqual(self.text.count("id(quietcool_fan).publish_state();"), 1)
        self.assertNotIn("rf_echo_guard", self.text)

    def test_diagnostics_entities_present(self) -> None:
        for name in ("TX Count", "RX Valid Count", "RX Rejected Count"):
            with self.subTest(name=name):
                self.assertIn(f'name: "{name}"', self.text)
        for name in ("Last TX Command", "Last Valid RX Frame"):
            with self.subTest(name=name):
                self.assertIn(f'name: "{name}"', self.text)
        self.assertIn("id: tx_count_sensor", self.text)
        self.assertIn("id: rx_valid_count_sensor", self.text)
        self.assertIn("id: rx_rejected_count_sensor", self.text)
        self.assertIn("id: last_tx_command_sensor", self.text)
        self.assertIn("id: last_rx_frame_sensor", self.text)

    def test_closed_loop_globals_are_volatile_and_boot_safe(self) -> None:
        globals_block = top_level_block(self.text, "globals")
        required = (
            "cl_active", "cl_desired_cmd", "cl_command_attempts",
            "cl_attempt_limit", "cl_query_due", "cl_query_due_ms",
            "cl_query_window", "cl_command_response_epoch", "cl_query_epoch",
            "cl_query_epoch_confirmation",
            "cl_query_started_ms", "cl_query_count",
            "cl_candidate_state", "cl_candidate_total_count",
            "cl_candidate_exact_count", "cl_report_ready",
            "cl_report_state", "cl_report_raw", "cl_report_capability",
            "oem_query_seen_ms", "oem_query_seen",
        )
        for global_id in required:
            with self.subTest(global_id=global_id):
                start = globals_block.index(f"- id: {global_id}")
                next_id = globals_block.find("\n  - id:", start + 1)
                item = globals_block[start:] if next_id == -1 else globals_block[start:next_id]
                self.assertIn("restore_value: false", item)

        boot = top_level_block(self.text, "esphome")
        self.assertIn('id(confirmed_fan_state_sensor).publish_state("unknown");', boot)
        self.assertIn('id(command_confirmation_status_sensor).publish_state("idle");', boot)
        # Volatile transaction state must stay inert across boot/OTA. Status
        # refresh remains explicit; boot executes no RF script at all.
        self.assertEqual(boot.count("script.execute"), 0)

    def test_new_command_transactions_arm_closed_loop_and_keep_refire(self) -> None:
        # Wrappers declare WHAT (command byte + refire budget);
        # begin_transaction arms the complete transaction and only then
        # enqueues, so a second HA call in the window joins instead of
        # resetting the safety budget.
        routes = {
            "send_off": ("!lambda 'return id(off_tx_command);'", "${off_refire_count}"),
            "send_low": ("0x9F", "${command_refire_count}"),
            "send_medium": ("0xAF", "${command_refire_count}"),
            "send_high": ("0xBF", "${command_refire_count}"),
            "send_timer": ("!lambda 'return id(timer_tx_command);'", "${command_refire_count}"),
        }
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            scripts = script_blocks(text)
            for script_id, (command, refires) in routes.items():
                with self.subTest(config=config_name, script_id=script_id):
                    wrapper = scripts[script_id]
                    self.assertIn("id: begin_transaction", wrapper)
                    self.assertIn(f"cmd: {command}", wrapper)
                    self.assertIn(f"refires: {refires}", wrapper)
            begin = scripts["begin_transaction"]
            tx_index = begin.index("id: tx_burst")
            for required_before_tx in (
                "id(refire_cmd) = cmd;",
                "id(refire_left) = refires;",
                "id(cl_active) = true;",
                "id(cl_desired_cmd) = cmd;",
                "id(cl_command_attempts) = 0;",
                "id(cl_attempt_limit) = refires + 1;",
                "id(cl_query_count) = 0;",
                "id(cl_candidate_total_count) = 0;",
                "id(cl_candidate_exact_count) = 0;",
                'publish_state(status)',
            ):
                with self.subTest(config=config_name, token=required_before_tx):
                    self.assertLess(begin.index(required_before_tx), tx_index)

    def test_duplicate_active_off_joins_without_tx_or_budget_reset(self) -> None:
        # All x0 variants mean OFF. A re-aimed B0 transaction must absorb an
        # HA/interlock repeat even when a fresh entity-derived request would
        # choose 90 or A0: send_off computes its variant, then routes into
        # begin_transaction whose both_off equivalence joins BEFORE any
        # transaction reset, enqueue, or airtime.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                scripts = script_blocks(text)
                off = scripts["send_off"]
                self.assertIn("id(off_tx_command) = off_speed_nibble;", off)
                self.assertIn("id: begin_transaction", off)
                self.assertIn("cmd: !lambda 'return id(off_tx_command);'", off)
                self.assertIn("refires: ${off_refire_count}", off)
                # No transaction state is touched in the wrapper itself.
                for forbidden in (
                    "id(refire_cmd) =",
                    "id(refire_left) =",
                    "id(cl_active) =",
                    "id(cl_command_attempts) =",
                    "id(fan_state_known) =",
                ):
                    self.assertNotIn(forbidden, off)
                begin = scripts["begin_transaction"]
                arm = begin.index("id(refire_left) = refires;")
                enqueue = begin.index("id: tx_burst")
                self.assertLess(begin.index("bool both_off"), arm)
                self.assertLess(arm, enqueue)
                self.assertLess(
                    begin.index('publish_state(status)'), enqueue
                )

    def test_timer_commands_wait_for_confirmation_on_both_targets(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                timer = script_blocks(text)["send_timer"]
                self.assertNotIn("id(quietcool_fan).publish_state();", timer)
                self.assertNotIn("id(quietcool_fan).state = true;", timer)

    def test_closed_loop_is_bounded_and_layered_on_spaced_refire(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            substitutions = top_level_block(text, "substitutions")
            tx_burst = script_blocks(text)["tx_burst"]
            query_interval = interval_item_containing(text, "id(cl_report_ready)")
            refire_interval = interval_item_containing(text, 'ESP_LOGD("REFIRE"')
            refire_condition = refire_interval[
                refire_interval.index("return id(refire_left)") :
                refire_interval.index(
                    "then:", refire_interval.index("return id(refire_left)")
                )
            ]
            with self.subTest(config=config_name):
                self.assertIn('command_refire_count: "3"', substitutions)
                self.assertIn('off_refire_count: "5"', substitutions)
                self.assertIn('command_refire_interval_ms: "1000"', substitutions)
                self.assertIn('closed_loop_query_delay_ms: "0"', substitutions)
                self.assertIn('post_command_response_min_ms: "400"', substitutions)
                self.assertIn('post_command_response_window_ms: "1600"', substitutions)
                self.assertIn("closed_loop_response_window_ms", substitutions)
                self.assertIn("closed_loop_response_min_ms", substitutions)
                self.assertIn("cmd != 0x66", tx_burst)
                self.assertIn("cmd == id(cl_desired_cmd)", tx_burst)
                self.assertIn(
                    "id(cl_command_attempts) = id(cl_command_attempts) + 1;",
                    tx_burst,
                )
                self.assertIn("id(cl_query_due) = false;", tx_burst)
                self.assertNotIn("id(cl_query_due) = true;", tx_burst)
                self.assertIn("id(cl_command_attempts) >= id(cl_attempt_limit)", tx_burst)
                self.assertIn("id(refire_left) = 0;", tx_burst)
                self.assertIn(
                    "id(refire_next_ms) = millis() + ${command_refire_interval_ms};",
                    tx_burst,
                )
                self.assertIn("interval: 100ms", query_interval)
                self.assertIn("id(cl_query_window) = true;", query_interval)
                self.assertIn("id: tx_burst", query_interval)
                self.assertRegex(query_interval, r"(?m)^\s+cmd: 0x66\s*$")
                self.assertIn(
                    "id(cl_query_count) = id(cl_query_count) + 1;", query_interval
                )
                self.assertIn("id(cl_active) = false;", query_interval)
                self.assertIn("id(refire_left) = 0;", query_interval)
                self.assertIn("interval: 250ms", refire_interval)
                self.assertIn("id: tx_burst", refire_interval)
                self.assertIn(
                    "id(refire_left) = id(refire_left) - 1;", refire_interval
                )
                self.assertIn(
                    "id(cl_command_attempts) < id(cl_attempt_limit)", refire_interval
                )
                self.assertIn("!id(cl_query_window)", refire_interval)
                # Both the free-report window and a due fallback query block
                # the one-second re-fire; consensus/mismatch/timeout releases it.
                self.assertIn("!id(cl_query_due)", refire_condition)

    def test_command_report_window_precedes_fallback_query_on_both_radios(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=config_name):
                substitutions = top_level_block(text, "substitutions")
                self.assertIn('post_command_response_min_ms: "400"', substitutions)
                self.assertIn('post_command_response_window_ms: "1600"', substitutions)
                self.assertIn('closed_loop_query_delay_ms: "0"', substitutions)

                tx = script_blocks(text)["tx_burst"]
                completion = tx[tx.index("id(cl_command_attempts) =") :]
                self.assertIn("id(cl_last_command_completed_ms) = millis();", completion)
                self.assertIn("id(cl_command_response_epoch) = true;", completion)
                self.assertIn("id(cl_query_epoch) = true;", completion)
                self.assertIn("id(cl_query_epoch_confirmation) = true;", completion)
                self.assertIn("id(cl_query_response_complete) = false;", completion)
                self.assertIn(
                    "id(cl_query_started_ms) = id(cl_last_command_completed_ms);",
                    completion,
                )
                self.assertIn("id(cl_query_window) = true;", completion)
                self.assertIn("id(cl_query_due) = false;", completion)
                self.assertNotIn("id(cl_query_due) = true;", completion)

                radio = top_level_block(text, radio_key)
                epoch = radio.index("bool command_response_epoch")
                correlated = radio.index("bool correlated_response", epoch)
                consensus = radio.index("if (exact_candidate || recovered_candidate)", correlated)
                remote = radio.index("bool remote_command_ok", consensus)
                self.assertTrue(epoch < correlated < consensus < remote)
                response_bounds = radio[epoch:correlated]
                self.assertIn("${post_command_response_min_ms}", response_bounds)
                self.assertIn("${post_command_response_window_ms}", response_bounds)
                self.assertIn("${closed_loop_response_min_ms}", response_bounds)
                self.assertIn("${closed_loop_response_window_ms}", response_bounds)
                consensus_branch = radio[consensus:remote]
                self.assertIn("candidate_total_count >= 2", consensus_branch)
                self.assertIn("candidate_exact_count >= 1", consensus_branch)
                self.assertIn(
                    "id(cl_report_confirmation) = id(cl_query_epoch_confirmation);",
                    consensus_branch,
                )
                self.assertIn("!local_response_epoch || recent_oem_query", radio[remote:])

                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                timeout = coordinator.index(
                    "if (id(cl_active) && id(cl_command_response_epoch)"
                )
                fallback = coordinator[timeout:]
                self.assertIn("${post_command_response_window_ms}", fallback)
                self.assertIn("!id(cl_query_response_complete)", fallback)
                self.assertIn("id(cl_query_window) = false;", fallback)
                self.assertIn("id(cl_query_due) = true;", fallback)
                self.assertIn(
                    "id(cl_query_due_ms) = millis() + ${closed_loop_query_delay_ms};",
                    fallback,
                )
                self.assertLess(
                    fallback.index("id(cl_query_due) = true;"),
                    fallback.index("cmd: 0x66"),
                )

                # The existing ambiguity and promotion policy remains the
                # sole arbiter after either free-report or query consensus.
                report = coordinator[coordinator.index("if (id(cl_report_ready))") :]
                for token in (
                    "bool state_matches",
                    "bool should_yield",
                    "actual != id(cl_prior_confirmed_state)",
                    "bool state_authoritative",
                    "bool timer_authoritative",
                ):
                    self.assertIn(token, report)

    def test_all_active_off_variants_join_but_other_or_terminal_requests_do_not(self) -> None:
        # Structural, not model-based: the YAML lambda itself is the source
        # of truth (a Python re-implementation was removed - it modelled
        # neither the incoming cmd nor the non-Off same_state branch and
        # could drift silently). both_off masks ONLY the duration nibble,
        # so 80/90/A0/B0 all join an active duration-zero transaction and
        # any running/timed command (nonzero duration) cannot.
        begin = script_blocks(self.text)["begin_transaction"]
        self.assertIn("bool both_off = ((cmd & 0x0F) == 0) &&", begin)
        self.assertIn("((id(cl_desired_cmd) & 0x0F) == 0);", begin)
        self.assertIn("(id(cl_desired_cmd) & 0x3F) == (cmd & 0x3F);", begin)
        self.assertIn("return both_off || same_state;", begin)

    def test_oem_query_supersedes_closed_loop_without_rx_transmit(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        query_start = radio.index("if (exact_frame && cmd == 0x66 &&")
        query_end = radio.index("return;", query_start) + len("return;")
        branch = radio[query_start:query_end]
        self.assertIn("id(oem_query_seen) = true;", branch)
        self.assertIn("id(refire_left) = 0;", branch)
        self.assertIn("id(cl_active) = false;", branch)
        self.assertIn("id(cl_query_due) = false;", branch)
        self.assertIn("id(cl_query_window) = false;", branch)
        self.assertIn("id(cl_query_epoch) = false;", branch)
        self.assertIn("id(cl_query_epoch_confirmation) = false;", branch)
        self.assertIn("id(tx_burst).stop();", branch)
        self.assertNotIn("script.execute", branch)

    def test_response_epoch_consumes_post_consensus_repeats(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=config_name):
                radio = top_level_block(text, radio_key)
                epoch = radio.index("bool local_response_epoch")
                passive = radio.index(
                    "if (!correlated_response || id(cl_report_ready))", epoch
                )
                normal = radio.index("// Normal six-byte traffic", passive)
                cancel = radio.index("bool remote_command_ok", normal)
                self.assertTrue(epoch < passive < normal < cancel)
                self.assertIn("id(cl_query_epoch)", radio[epoch:passive])
                self.assertIn("return;", radio[passive:normal])

                tx_burst = script_blocks(text)["tx_burst"]
                self.assertIn("if (cmd == 0x66)", tx_burst)
                self.assertIn("id(cl_query_epoch) = true;", tx_burst)
                self.assertIn("id(cl_query_epoch_confirmation)", tx_burst)
                self.assertIn("id(cl_query_response_complete) = false;", tx_burst)
                self.assertIn("id(cl_report_confirmation)", radio[passive:normal])
                self.assertIn("id(cl_query_started_ms) = millis();", tx_burst)

                remote_cancel = radio[normal:]
                self.assertIn(
                    "!local_response_epoch || recent_oem_query", remote_cancel
                )
                self.assertIn("id(tx_burst).stop();", remote_cancel)
                self.assertIn("Physical controls always win", remote_cancel)

                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                self.assertIn(
                    "(millis() - id(cl_query_started_ms)) > ${closed_loop_response_window_ms}",
                    coordinator,
                )

    def test_command_epoch_reanchors_after_any_prior_query_tail(self) -> None:
        # Each completed command replaces the prior response epoch at the
        # command-burst-end anchor and applies the command-specific minimum;
        # old query timing/candidates therefore cannot be inherited.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            tx = script_blocks(text)["tx_burst"]
            completion = tx[tx.index("id(cl_command_attempts) =") :]
            with self.subTest(config=config_name):
                anchor = completion.index("id(cl_last_command_completed_ms) = millis();")
                epoch = completion.index("id(cl_command_response_epoch) = true;")
                started = completion.index(
                    "id(cl_query_started_ms) = id(cl_last_command_completed_ms);"
                )
                candidates = completion.index("id(cl_candidate_total_count) = 0;")
                self.assertTrue(anchor < epoch < started < candidates)
                self.assertIn("id(cl_query_due) = false;", completion)

                refire = interval_item_containing(text, 'ESP_LOGD("REFIRE"')
                condition = refire[
                    refire.index("return id(refire_left)") :
                    refire.index("then:", refire.index("return id(refire_left)"))
                ]
                self.assertIn("!id(cl_query_due)", condition)
                self.assertIn("!id(cl_query_window)", condition)

    def test_manual_refresh_uses_consensus_and_tail_is_classification_only(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=config_name):
                radio = top_level_block(text, radio_key)
                observed = radio.index("bool observed_from_local_response")
                correlated = radio.index("bool correlated_response", observed)
                window = radio[observed:correlated]
                self.assertIn("query_age >= response_min_ms", window)
                self.assertIn("query_age <= response_window_ms", window)
                bounds = radio[radio.index("uint32_t response_min_ms") : observed]
                self.assertIn("${closed_loop_response_min_ms}", bounds)
                self.assertIn("${closed_loop_response_window_ms}", bounds)
                consensus_branch = radio[
                    radio.index("if (exact_candidate || recovered_candidate)") :
                    radio.index("// Normal six-byte traffic")
                ]
                self.assertIn("!id(cl_query_epoch_confirmation)", radio[correlated:])
                self.assertIn("candidate_total_count >= 2", consensus_branch)
                self.assertIn(
                    "id(cl_report_confirmation) = id(cl_query_epoch_confirmation);",
                    consensus_branch,
                )
                self.assertIn("id(cl_query_response_complete) = true;", consensus_branch)
                self.assertIn("Contradictory tail frame", consensus_branch)
                self.assertIn(
                    "id(fan_confirmed_off_sensor).publish_state(false);",
                    consensus_branch,
                )
                # A manual epoch is authoritative only while no command is
                # active. A new state request cannot inherit a late Refresh
                # reply and republish stale OFF during an ON transaction.
                correlated_expr = radio[
                    correlated : radio.index(";", correlated)
                ]
                self.assertIn(
                    "!id(cl_query_epoch_confirmation) && !id(cl_active)",
                    correlated_expr,
                )

                self.assertIn("id(cl_report_ready) = false;", consensus_branch)
                self.assertIn("id(cl_report_confirmation) = false;", consensus_branch)
                self.assertIn("id(refire_left) > 0", consensus_branch)
                self.assertIn("id(cl_active) = false;", consensus_branch)

    def test_fresh_command_poison_old_manual_query_epoch_before_enqueue_and_airtime(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                scripts = script_blocks(text)
                tx = scripts["tx_burst"]
                actual_invalidate = tx.index("if (cmd != 0x66)")
                query_branch = tx.index("if (cmd == 0x66)", actual_invalidate)
                self.assertIn(
                    "id(cl_query_response_complete) = true;",
                    tx[actual_invalidate:query_branch],
                )
                begin = scripts["begin_transaction"]
                poison = begin.index("id(cl_query_response_complete) = true;")
                enqueue = begin.index("id: tx_burst")
                self.assertLess(poison, enqueue)

    def test_manual_query_cannot_erase_unconsumed_command_consensus(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                tx = script_blocks(text)["tx_burst"]
                guard = tx.index("bool unsafe_manual_query")
                reset = tx.index("id(cl_report_ready) = false;", guard)
                self.assertIn(
                    "id(cl_active) || id(cl_report_ready) || id(cl_query_epoch)",
                    tx[guard:reset],
                )
                self.assertIn("prior response tail", tx)
                self.assertLess(guard, reset)

                refresh = list_item_containing(
                    text, "button", 'name: "Refresh Fan State"'
                )
                # Queue rejection must still fail stale authority closed.
                self.assertLess(
                    refresh.index("id(fan_confirmed_off_sensor).publish_state(false);"),
                    refresh.index("id: tx_burst"),
                )

    def test_passive_rx_never_mutates_safety_fan_and_fails_authority_closed(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=config_name):
                radio = top_level_block(text, radio_key)
                passive = radio[radio.index("// Any other strictly validated passive") :]
                self.assertIn("id(fan_state_known) = false;", passive)
                self.assertIn("id(timer_state_known) = false;", passive)
                self.assertIn("id(fan_confirmed_off_sensor).publish_state(false);", passive)
                self.assertNotIn("id(quietcool_fan).publish_state();", radio)

    def test_atomic_confirmed_off_diagnostic_has_fail_closed_semantics(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                binary = list_item_containing(
                    text, "binary_sensor", 'name: "Fan Confirmed Off"'
                )
                self.assertIn("id: fan_confirmed_off_sensor", binary)
                self.assertIn(
                    "return id(fan_state_known) && !id(quietcool_fan).state;",
                    binary,
                )
                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                self.assertIn(
                    "id(fan_confirmed_off_sensor).publish_state(actual_duration == 0);",
                    coordinator,
                )
                self.assertIn(
                    "id(fan_confirmed_off_sensor).publish_state(false);",
                    coordinator,
                )

    def test_local_response_classification_is_bounded_and_outlives_teardown(self) -> None:
        # Classification must survive the healthy coordinator's worst-case
        # teardown latency, but a sticky raw epoch must eventually stop masking
        # OEM commands. Pin both sides of that safety valve on both radios.
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=config_name):
                substitutions = top_level_block(text, "substitutions")

                def milliseconds(name: str) -> int:
                    match = re.search(rf'(?m)^  {name}: "(\d+)"$', substitutions)
                    self.assertIsNotNone(match, name)
                    return int(match.group(1))

                command_min = milliseconds("post_command_response_min_ms")
                command_window = milliseconds("post_command_response_window_ms")
                query_min = milliseconds("closed_loop_response_min_ms")
                query_window = milliseconds("closed_loop_response_window_ms")
                classification_tail = milliseconds("closed_loop_response_tail_ms")
                classification_ceiling = milliseconds("classification_ceiling_ms")
                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                coordinator_tick_match = re.search(
                    r"(?m)^  - interval: (\d+)ms$", coordinator
                )
                self.assertIsNotNone(coordinator_tick_match)
                coordinator_tick = int(coordinator_tick_match.group(1))

                self.assertEqual(classification_ceiling, 3000)
                self.assertGreaterEqual(classification_tail, command_window)
                self.assertGreaterEqual(classification_tail, query_window)
                self.assertGreater(
                    classification_ceiling,
                    classification_tail + coordinator_tick,
                )

                radio = top_level_block(text, radio_key)
                epoch_start = radio.index("bool command_response_epoch")
                response_end = radio.index("auto bit_count", epoch_start)
                response_preamble = radio[epoch_start:response_end]
                self.assertRegex(
                    response_preamble,
                    r"bool local_response_epoch\s*=\s*"
                    r"id\(cl_query_epoch\)\s*&&\s*"
                    r"query_age <= \$\{classification_ceiling_ms\};",
                )
                self.assertIn("query_age <= response_window_ms", response_preamble)
                self.assertIn(
                    "classification must outlive teardown in the healthy case",
                    response_preamble,
                )
                self.assertIn(
                    "must expire in the stuck case",
                    response_preamble,
                )

                teardown_start = coordinator.index(
                    "if (id(cl_query_epoch) && !id(cl_query_due)"
                )
                teardown_end = coordinator.index("\n          }", teardown_start)
                teardown = coordinator[teardown_start:teardown_end]
                self.assertIn("!id(cl_query_window)", teardown)
                self.assertIn(
                    "> ${closed_loop_response_tail_ms}", teardown
                )
                self.assertIn("id(cl_query_epoch) = false;", teardown)

                tx_burst = script_blocks(text)["tx_burst"]
                query_anchor = tx_burst[
                    tx_burst.index("if (cmd == 0x66)") :
                ]
                self.assertIn("id(cl_query_epoch) = true;", query_anchor)
                self.assertIn("id(cl_query_started_ms) = millis();", query_anchor)

                # Enumerate every millisecond for command/query epochs in both
                # healthy and sticky-TX paths. A matching frame has exactly one
                # disposition: accepted, passive-local, or outside the
                # effective epoch and therefore eligible for OEM handling.
                healthy_teardown_age = classification_tail + coordinator_tick
                enumeration_end = classification_ceiling + coordinator_tick + 1
                for epoch_kind, minimum, window in (
                    ("command", command_min, command_window),
                    ("query", query_min, query_window),
                ):
                    for path in ("healthy", "stuck-tx"):
                        for query_age in range(enumeration_end + 1):
                            raw_epoch_live = (
                                query_age < healthy_teardown_age
                                if path == "healthy"
                                else True
                            )
                            effective_epoch_live = (
                                raw_epoch_live
                                and query_age <= classification_ceiling
                            )
                            accepted = (
                                effective_epoch_live
                                and minimum <= query_age <= window
                            )
                            passive = effective_epoch_live and not accepted
                            outside = not effective_epoch_live
                            dispositions = (accepted, passive, outside)
                            self.assertEqual(
                                sum(dispositions),
                                1,
                                (config_name, epoch_kind, path, query_age),
                            )
                            if raw_epoch_live and query_age <= classification_ceiling:
                                self.assertTrue(
                                    accepted or passive,
                                    (config_name, epoch_kind, path, query_age),
                                )
                            if (
                                path == "stuck-tx"
                                and query_age == classification_ceiling + 1
                            ):
                                self.assertTrue(
                                    outside,
                                    (config_name, epoch_kind, path, query_age),
                                )

                        if path == "healthy":
                            self.assertLess(
                                healthy_teardown_age,
                                classification_ceiling,
                            )

    def test_stuck_command_fallback_watchdog_terminates_without_transmitting(self) -> None:
        required_clears = (
            "id(cl_query_epoch) = false;",
            "id(cl_query_epoch_confirmation) = false;",
            "id(cl_command_response_epoch) = false;",
            "id(cl_query_window) = false;",
            "id(cl_query_due) = false;",
            "id(cl_active) = false;",
        )
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                watchdog_start = coordinator.index("// Classification watchdog:")
                watchdog_end = coordinator.index(
                    "// End classification watchdog.", watchdog_start
                )
                watchdog = coordinator[watchdog_start:watchdog_end]

                self.assertIn("id(cl_query_epoch)", watchdog)
                self.assertIn("id(cl_query_due)", watchdog)
                self.assertRegex(
                    watchdog,
                    r"id\(cl_command_response_epoch\)\s*&&\s*"
                    r"id\(cl_query_window\)",
                )
                self.assertRegex(
                    watchdog,
                    r">\s*\$\{classification_ceiling_ms\}",
                )
                for clear in required_clears:
                    self.assertIn(clear, watchdog)
                self.assertIn("id(cl_prior_confirmed_state) = 0xFF;", watchdog)
                self.assertIn("id(refire_left) = 0;", watchdog)
                self.assertIn("id(tx_burst).stop();", watchdog)
                self.assertIn(
                    "id(command_confirmation_status_sensor).publish_state",
                    watchdog,
                )
                self.assertRegex(watchdog, r'"FAILED:[^"]*watchdog[^"]*"')
                self.assertIn('ESP_LOGE("CLOSED_LOOP"', watchdog)

                # Cancellation is allowed; the watchdog must never enqueue or
                # perform RF work of its own.
                self.assertNotIn("send_packet", watchdog)
                self.assertNotIn("script.execute", watchdog)
                self.assertNotIn("id: tx_burst", watchdog)

    def test_confirmation_diagnostics_and_capability_are_published(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                for name, sensor_id in (
                    ("Last Confirmed Fan State", "confirmed_fan_state_sensor"),
                    ("Command Confirmation Status", "command_confirmation_status_sensor"),
                    ("Fan Speed Capability", "fan_capability_sensor"),
                ):
                    item = list_item_containing(
                        text, "text_sensor", f'name: "{name}"'
                    )
                    self.assertIn(f"id: {sensor_id}", item)
                    self.assertIn("update_interval: never", item)

                query_interval = interval_item_containing(text, "id(cl_report_ready)")
                self.assertIn(
                    "id(confirmed_fan_state_sensor).publish_state", query_interval
                )
                self.assertIn(
                    "id(command_confirmation_status_sensor).publish_state",
                    query_interval,
                )
                self.assertIn(
                    "id(fan_capability_sensor).publish_state", query_interval
                )
                # "Last Confirmed" uses physical-state authority; countdown
                # authority is independently stricter for active timers.
                confirmed_publish = query_interval.index(
                    "id(confirmed_fan_state_sensor).publish_state"
                )
                authority_guard = query_interval.rindex(
                    "if (state_authoritative) {", 0, confirmed_publish
                )
                self.assertLess(authority_guard, confirmed_publish)

    def test_explicit_state_known_diagnostics_bound_native_fan_boot_guess(self) -> None:
        # ESPHome's native Fan API has no missing-state bit and exposes its raw
        # default on initial subscription. These diagnostics are the explicit
        # authority boundary for HA safety logic until validated RF arrives.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                globals_block = top_level_block(text, "globals")
                self.assertIn("- id: fan_state_known", globals_block)
                self.assertIn("- id: timer_state_known", globals_block)

                binary = top_level_block(text, "binary_sensor")
                self.assertIn('name: "Fan State Known"', binary)
                self.assertIn("return id(fan_state_known);", binary)
                self.assertIn('name: "Timer State Known"', binary)
                self.assertIn("return id(timer_state_known);", binary)

                boot = top_level_block(text, "esphome")
                self.assertNotIn("id(fan_timer_select).publish_state", boot)

                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                unknown_sensor = coordinator.index(
                    "id(fan_state_known_sensor).publish_state(false);"
                )
                publish_index = coordinator.index("id(quietcool_fan).publish_state();")
                timer_known = coordinator.index(
                    "id(timer_state_known) = timer_authoritative;", publish_index
                )
                known_sensor = coordinator.index(
                    "id(fan_state_known_sensor).publish_state(true);", publish_index
                )
                self.assertTrue(unknown_sensor < publish_index < timer_known)
                self.assertTrue(publish_index < known_sensor)

    def test_homeassistant_display_temperature_aliases_present(self) -> None:
        substitutions = top_level_block(self.text, "substitutions")
        sensors = top_level_block(self.text, "sensor")
        homeassistant_items = re.findall(
            r"(?ms)^  - platform: homeassistant\n.*?(?=^  - platform:|\Z)", sensors
        )
        self.assertEqual(len(homeassistant_items), 3)

        for substitution, entity_id, sensor_id in DISPLAY_TEMPERATURE_SENSORS:
            with self.subTest(entity_id=entity_id):
                self.assertIn(f'  {substitution}: "{entity_id}"', substitutions)
                self.assertEqual(self.text.count(entity_id), 1)
                item = list_item_containing(
                    self.text, "sensor", f"entity_id: ${{{substitution}}}"
                )
                self.assertIn("platform: homeassistant", item)
                self.assertIn(f"id: {sensor_id}", item)
                self.assertIn("internal: true", item)

    def test_legacy_temperature_sources_and_device_mean_are_absent(self) -> None:
        sensors = top_level_block(self.text, "sensor")
        for entity_id in (
            "sensor.st_00192556_temperature",
            "sensor.downstairs_attic_fan_attic_temperature",
            *(entity_id for entity_id, _ in LEGACY_AIOSENSE_TEMPERATURE_SENSORS),
        ):
            with self.subTest(entity_id=entity_id):
                self.assertNotIn(entity_id, self.text)

        for _, sensor_id in LEGACY_AIOSENSE_TEMPERATURE_SENSORS:
            with self.subTest(sensor_id=sensor_id):
                self.assertNotIn(f"id: {sensor_id}", sensors)
                self.assertNotIn(f"id({sensor_id}).state", sensors)

        self.assertNotIn('name: "Indoor Downstairs Temperature"', sensors)
        self.assertNotIn("const float readings[]", sensors)
        self.assertNotIn("return total / count;", sensors)

    def test_removed_downstairs_climate_import_is_absent(self) -> None:
        sensors = top_level_block(self.text, "sensor")
        self.assertNotIn("climate.downstairs", sensors)
        self.assertNotIn("attribute: current_temperature", sensors)

    def test_oled_indoor_temperature_renders_placeholder_for_nan(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertIn(
            "id(temp_indoor).has_state() && std::isfinite(id(temp_indoor).state)",
            display,
        )
        self.assertRegex(
            display,
            r'(?ms)if \(id\(temp_indoor\).*?\}\s+else \{\s+strcpy\(in_buf, "--"\);',
        )

    def test_oled_outdoor_temperature_renders_placeholder_for_nan(self) -> None:
        # FIX 5: an `unavailable` HA sensor publishes NAN, not just "no
        # state yet" - has_state() alone isn't enough, isfinite() is
        # required too (mirrors the indoor check above).
        display = top_level_block(self.text, "display")
        self.assertIn(
            "id(temp_outdoor).has_state() && std::isfinite(id(temp_outdoor).state)",
            display,
        )
        self.assertRegex(
            display,
            r'(?ms)if \(id\(temp_outdoor\).*?\}\s+else \{\s+strcpy\(out_buf, "--"\);',
        )

    def test_oled_attic_temperature_renders_placeholder_for_nan(self) -> None:
        # FIX 5: same NAN-safety gap as outdoor, on the attic reading.
        display = top_level_block(self.text, "display")
        self.assertIn(
            "id(temp_attic).has_state() && std::isfinite(id(temp_attic).state)",
            display,
        )
        self.assertRegex(
            display,
            r'(?ms)if \(id\(temp_attic\).*?\}\s+else \{\s+strcpy\(attic_buf, "--"\);',
        )

    def test_no_automation_attached_to_temperature_sensors(self) -> None:
        # Telemetry only: none of the homeassistant sensor entries may carry
        # an on_value (or any other) automation that could reach a script.
        for substitution, _, _ in DISPLAY_TEMPERATURE_SENSORS:
            item = list_item_containing(
                self.text, "sensor", f"entity_id: ${{{substitution}}}"
            )
            with self.subTest(substitution=substitution):
                self.assertNotIn("on_value", item)
                self.assertNotIn("script.execute", item)
                self.assertNotIn("sx127x.send_packet", item)
                self.assertNotIn("quietcool_fan", item)

    def test_web_server_removed(self) -> None:
        self.assertNotIn("web_server:", self.text)
        self.assertNotIn("quietcool123", self.text)

    def test_secrets_use_fleet_naming_convention(self) -> None:
        # Fleet convention for ESPHome Builder sync: per-device secret names
        # (<device>_api_key, <device>_ota_password, <device>_fallback_ap_
        # password) alongside the shared wifi_ssid/wifi_password. Only the
        # key NAMES are asserted here - never a value - so this can't leak
        # a secret into test output.
        api_block = top_level_block(self.text, "api")
        self.assertIn("key: !secret quietcool_lora32_api_key", api_block)

        ota_block = top_level_block(self.text, "ota")
        self.assertIn("password: !secret quietcool_lora32_ota_password", ota_block)

        wifi_block = top_level_block(self.text, "wifi")
        self.assertIn("ssid: !secret wifi_ssid", wifi_block)
        self.assertIn("password: !secret wifi_password", wifi_block)
        self.assertIn(
            "password: !secret quietcool_lora32_fallback_ap_password", wifi_block
        )

        # The legacy shared/generic names must no longer appear anywhere in
        # the device config now that every per-device secret is renamed.
        self.assertNotIn("!secret api_encryption_key", self.text)
        self.assertNotIn("!secret ota_password", self.text)
        self.assertNotIn("!secret fallback_ap_password", self.text)

        if self.secrets_text:
            for key in (
                "quietcool_lora32_api_key:",
                "quietcool_lora32_ota_password:",
                "quietcool_lora32_fallback_ap_password:",
                "wifi_ssid:",
                "wifi_password:",
            ):
                with self.subTest(key=key):
                    self.assertIn(key, self.secrets_text)

    def test_user_visible_sender_id_is_dynamic_not_hardcoded(self) -> None:
        self.assertIn('name: "Remote Sender ID"', self.text)
        self.assertIn('"%02X %02X %02X %02X"', self.text)
        self.assertIn('"unset"', self.text)
        self.assertNotIn("CB000152", self.text)
        self.assertNotIn("CB 00 01 52", self.text)

    # -------------------------------------------------------------------
    # OLED redesign (v3 - animated fan icon, HH:MM:SS, icon-only status)
    # -------------------------------------------------------------------

    def test_oled_has_no_title_line(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertNotIn("QuietCool 2.4k", display)
        self.assertNotIn("QuietCool", display)

    def test_i2c_runs_at_400khz(self) -> None:
        # At the previous 50kHz default a full 128x64 frame push took
        # ~190ms, capping the fan-icon animation at ~2fps and risking loop
        # stalls. 400kHz (SSD1306-supported fast mode) is required to give
        # the 250ms display update_interval enough headroom.
        i2c_block = top_level_block(self.text, "i2c")
        self.assertIn("frequency: 400kHz", i2c_block)

    def test_display_update_interval_supports_animation(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertRegex(display, r"(?m)^    update_interval: (?:2[5-9][0-9]|[3-4][0-9][0-9]|500)ms$")

    def test_fan_animation_globals_present_and_display_only(self) -> None:
        # Purely cosmetic rotation state: must exist as a global, must be
        # driven only from the display lambda, and must never be read or
        # written by any TX/RX/actuation path.
        globals_block = top_level_block(self.text, "globals")
        self.assertIn("id: fan_anim_frame", globals_block)

        display = top_level_block(self.text, "display")
        self.assertIn("id(fan_anim_frame)", display)

        for section_name in ("script", "sx127x", "interval", "fan", "button", "binary_sensor"):
            with self.subTest(section=section_name):
                section = top_level_block(self.text, section_name)
                self.assertNotIn("fan_anim_frame", section)

    def test_oled_fan_icon_uses_real_mdi_glyph_not_hand_drawn_shapes(self) -> None:
        # v3.2: operator review of v3.1 rejected the hand-drawn triangle
        # "hub + blades" icon outright ("wtf is that fan icon.. it looks
        # like blades"). No shape-drawing call from that old icon may
        # remain anywhere in the lambda - the fan is now a real pre-
        # rendered `mdi:fan` glyph image, drawn with `it.image(...)`.
        display = top_level_block(self.text, "display")
        for banned in ("filled_triangle", "it.triangle(", "it.circle(", "it.filled_circle("):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, display)
        self.assertNotIn("filled_rectangle", display)
        self.assertNotIn("COLOR_OFF", display)
        self.assertNotIn("it.rectangle", display)

        self.assertIn("it.image(fan_icon_x, fan_icon_y, current_fan_frame);", display)
        self.assertIn(
            "image::Image *current_fan_frame = running ? fan_frames[id(fan_anim_frame)] : id(fan_off_frame);",
            display,
        )

        # Rotation speed proportional to fan speed (frame-index step per
        # display refresh, not a hand-computed angle); frozen (not reset)
        # when off - the display instead switches to the static
        # `fan_off_frame` image, giving a static, distinct "off" look.
        self.assertIn("frame_step = 1;", display)
        self.assertIn("frame_step = 2;", display)
        self.assertIn("frame_step = 3;", display)
        self.assertIn("id(fan_anim_frame) = (id(fan_anim_frame) + frame_step) % 12;", display)

    def test_fan_icon_image_blocks_present_for_every_frame(self) -> None:
        # Every frame the display lambda's fan_frames[] array and
        # fan_off_frame reference must be declared as a file-backed
        # `image:` platform entry (type: BINARY, matching the 1-bit
        # SSD1306), so the referenced ids actually resolve at compile time.
        image_block = top_level_block(self.text, "image")
        for i in range(12):
            with self.subTest(frame=i):
                self.assertIn(f"id: fan_frame_{i}", image_block)
                self.assertIn(f'file: "images/fan_frame_{i}.png"', image_block)
        self.assertIn("id: fan_off_frame", image_block)
        self.assertIn('file: "images/fan_off.png"', image_block)
        # Every entry uses the new `platform: file` form, not the
        # deprecated bare-list image: syntax ESPHome 2026.7 warns about.
        self.assertEqual(image_block.count("platform: file"), 13)
        self.assertEqual(image_block.count("type: BINARY"), 13)

    def test_fan_icon_frame_files_exist_on_disk(self) -> None:
        # tools/generate_fan_frames.py must have actually been run and its
        # output committed - a config: that references a missing image
        # file fails at ESPHome compile time, not at YAML-parse time, so
        # this test catches it earlier and more cheaply.
        images_dir = ROOT / "images"
        for i in range(12):
            with self.subTest(frame=i):
                path = images_dir / f"fan_frame_{i}.png"
                self.assertTrue(path.is_file(), f"missing {path}")
        off_path = images_dir / "fan_off.png"
        self.assertTrue(off_path.is_file(), f"missing {off_path}")

    def test_generate_fan_frames_tool_exists_and_documents_codepoint_verification(self) -> None:
        tool = ROOT / "tools" / "generate_fan_frames.py"
        self.assertTrue(tool.is_file(), f"missing {tool}")
        text = tool.read_text()
        # Codepoints must be verified against the shipped font's own cmap
        # at generation time, not hardcoded from memory or an external
        # stylesheet reference.
        self.assertIn("find_glyph_codepoint", text)
        self.assertIn("0xF0210", text)  # mdi:fan
        self.assertIn("0xF081D", text)  # mdi:fan-off

    def test_status_row_icons_are_bottom_left_horizontal(self) -> None:
        # Operator requirement for v3.2: "the status icons should be a
        # HORIZONTAL row in the BOTTOM-LEFT corner, not a center vertical
        # stack." All three status icons must share one y (a row, not a
        # column) and have strictly increasing x (left-to-right, evenly
        # spaced) - the opposite of v3.1's shared-x/increasing-y column.
        display = top_level_block(self.text, "display")
        x_match = re.search(
            r"status_wifi_x = (\d+), status_api_x = (\d+), status_battery_x = (\d+);", display
        )
        self.assertIsNotNone(x_match, "status row x-position declaration not found")
        wifi_x, api_x, battery_x = (int(v) for v in x_match.groups())
        self.assertLess(wifi_x, api_x)
        self.assertLess(api_x, battery_x)

        self.assertIn(
            "it.print(status_wifi_x, status_row_y, id(font_icons), TextAlign::TOP_LEFT, wifi_glyph);",
            display,
        )
        self.assertIn(
            "it.print(status_api_x, status_row_y, id(font_icons), TextAlign::TOP_LEFT, api_glyph);",
            display,
        )
        self.assertIn(
            "it.print(status_battery_x, status_row_y, id(font_icons), TextAlign::TOP_LEFT, batt_glyph);",
            display,
        )
        # All three draw calls share the SAME y variable (a row) - the
        # v3.1 column instead gave each icon its own distinct y.
        self.assertIn("const int status_row_y = 52;", display)

    def test_oled_state_word_present(self) -> None:
        display = top_level_block(self.text, "display")
        for marker in ("LOW", "MED", "HIGH", "OFF"):
            self.assertIn(f'"{marker}"', display)
        self.assertIn("TextAlign::TOP_CENTER", display)

    def test_oled_learn_and_confirmation_states_replace_left_zone_text(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertIn("KEEP IN SYNC: LEARN_STATE", display)
        self.assertIn("id(learn_active)", display)
        self.assertIn("id(learn_confirm_until)", display)
        self.assertIn('"LEARN"', display)
        self.assertIn('"REMOTE X2"', display)
        self.assertIn('"LEARNED"', display)
        self.assertIn('"ID SAVED"', display)
        self.assertIn("id(font_learn)", display)
        self.assertIn("id(font_learn_prompt)", display)

        learn_start = display.index("KEEP IN SYNC: LEARN_STATE")
        status_start = display.index("KEEP IN SYNC: STATUS_ICONS", learn_start)
        learn_block = display[learn_start:status_start]
        self.assertLess(learn_block.index("if (id(learn_active))"), learn_block.index('"LEARN"'))
        self.assertLess(learn_block.index("else if (learn_confirm)"), learn_block.index('"LEARNED"'))
        self.assertNotIn("sx127x.send_packet", learn_block)
        self.assertNotIn("script.execute", learn_block)

    def test_oled_countdown_is_zero_padded_hhmmss(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertIn("timer_active", display)
        self.assertIn('"%02d:%02d:%02d"', display)
        self.assertIn("int hh = remaining_ms / 3600000;", display)
        self.assertIn("int mm = (remaining_ms % 3600000) / 60000;", display)
        self.assertIn("int ss = (remaining_ms % 60000) / 1000;", display)

    def test_oled_timer_countdown_uses_rollover_safe_arithmetic(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertIn(
            "(int32_t) (id(timer_expiry_millis) - millis())", display
        )

    def test_oled_continuous_on_renders_no_timer_text(self) -> None:
        # Continuous-on (running, no active timer) shows NOTHING on the
        # countdown line - no "CONT" text, no infinity glyph (removed: it
        # read as illegible "OO" at small size), and no it.print/it.printf
        # call at all in that branch. The spinning fan icon + state word
        # already say "running"; a countdown line only appears while one
        # is actually counting down.
        display = top_level_block(self.text, "display")
        self.assertNotIn('"CONT"', display)
        self.assertNotIn("\\U000F06E4", display)
        self.assertNotIn("infinity", display.lower())
        self.assertNotIn("font_icons_lg", display)

        # Normal rendering keeps the same single timer predicate and no
        # branch that draws continuous-on text; learn/confirmation own the
        # two left-zone text rows before normal state rendering.
        learn_start = display.index("KEEP IN SYNC: LEARN_STATE")
        status_start = display.index("KEEP IN SYNC: STATUS_ICONS", learn_start)
        left_text_block = display[learn_start:status_start]
        self.assertIn(
            "if (running && id(timer_state_known) && id(timer_active)) {",
            left_text_block,
        )
        self.assertEqual(left_text_block.count("it.printf"), 1)
        self.assertNotIn('"CONT"', left_text_block)

    def test_oled_wifi_and_api_status_are_icons_not_words(self) -> None:
        # WiFi and API status must be icons (MDI glyphs), never words like
        # the old "NoWiFi"/"No API"/"API OK" text labels.
        display = top_level_block(self.text, "display")
        for banned in ("NoWiFi", "No API", "API OK"):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, display)

        self.assertIn("wifi::global_wifi_component->is_connected()", display)
        self.assertIn("api::global_api_server->is_connected()", display)
        self.assertIn("id(wifi_signal_sensor).state", display)

        # WiFi: four signal-strength tiers plus a distinct disconnected
        # glyph, selected by RSSI.
        for glyph in ("\\U000F0928", "\\U000F0925", "\\U000F0922", "\\U000F091F", "\\U000F092D"):
            with self.subTest(glyph=glyph):
                self.assertIn(glyph, display)

        # API: the Home Assistant logo glyph / network-off-outline pair
        # (v3.3) - not the old illegible lan-connect/lan-disconnect pair,
        # and not v3.1/v3.2's filled home/home-off pair either (retired in
        # v3.3 because it now collides semantically with the new indoor
        # home-thermometer-outline glyph - see test_temp_icons_* below).
        self.assertIn("\\U000F07D0", display)  # home-assistant (connected)
        self.assertIn("\\U000F0C9C", display)  # network-off-outline (disconnected)
        self.assertNotIn("\\U000F0318", display)  # old lan-connect, removed
        self.assertNotIn("\\U000F0319", display)  # old lan-disconnect, removed
        self.assertNotIn("\\U000F02DC", display)  # v3.1/v3.2 home, retired
        self.assertNotIn("\\U000F1A46", display)  # v3.1/v3.2 home-off, retired
        self.assertIn("id(font_icons)", display)

    def test_wifi_signal_sensor_has_id_for_display_lambda(self) -> None:
        item = list_item_containing(self.text, "sensor", 'name: "WiFi Signal"')
        self.assertIn("id: wifi_signal_sensor", item)

    def test_oled_right_side_temperatures_are_right_aligned(self) -> None:
        display = top_level_block(self.text, "display")
        self.assertIn("TextAlign::TOP_RIGHT", display)
        self.assertIn("id(font_temp_large)", display)
        self.assertIn("id(font_temp_small)", display)
        self.assertIn('"%s°F"', display)
        # v3.3: the "Out"/"At" text labels are gone entirely, replaced by
        # icons (see test_temp_icons_replace_out_at_labels below) - the
        # printf format strings must no longer carry them. (A substring
        # check for bare "Out "/"At " would be too broad here - it'd also
        # flag this file's own prose comments about the change.)
        self.assertNotIn('"Out %s°F"', display)
        self.assertNotIn('"At %s°F"', display)
        self.assertIn('it.printf(127, 27, id(font_temp_small), TextAlign::TOP_RIGHT, "%s°F", out_buf);', display)
        self.assertIn('it.printf(127, 43, id(font_temp_small), TextAlign::TOP_RIGHT, "%s°F", attic_buf);', display)

    def test_temp_icons_replace_out_at_labels(self) -> None:
        # v3.3: operator feedback - "Out/At doesn't meaningfully tell me
        # what's going on - icons for that would be nice too, for primary
        # interior temp, outside, and attic." One icon per temperature
        # line (indoor/outdoor/attic), drawn with font_icons at a shared
        # left-edge x so they read as one column (KEEP IN SYNC:
        # TEMP_ICONS in tools/render_display.py carries the same layout).
        display = top_level_block(self.text, "display")
        self.assertIn("const int temp_icon_x = 67;", display)

        icon_calls = re.findall(
            r'it\.print\(temp_icon_x, (\d+), id\(font_icons\), TextAlign::TOP_LEFT, "(\\U000F[0-9A-Fa-f]+)"\);',
            display,
        )
        self.assertEqual(len(icon_calls), 3, f"expected 3 temp-icon draw calls, found {icon_calls}")
        ys = [int(y) for y, _glyph in icon_calls]
        self.assertEqual(ys, sorted(ys), "temp icons must be declared top-to-bottom (indoor, outdoor, attic)")

        glyphs = [glyph for _y, glyph in icon_calls]
        self.assertEqual(
            glyphs,
            ["\\U000F0F55", "\\U000F0599", "\\U000F112B"],
            "indoor=home-thermometer-outline, outdoor=weather-sunny, attic=home-roof",
        )

    def test_mdi_icon_font_declared_from_local_file(self) -> None:
        font_block = top_level_block(self.text, "font")
        self.assertIn('file: "fonts/materialdesignicons-webfont.ttf"', font_block)
        self.assertIn("id: font_icons", font_block)
        # font_icons_lg (18pt, infinity-only) was removed along with the
        # infinity glyph (FIX 2) - every icon, including battery, now
        # shares the single 13pt font_icons declaration.
        self.assertNotIn("font_icons_lg", font_block)
        mdi_font_path = ROOT / "fonts" / "materialdesignicons-webfont.ttf"
        self.assertTrue(mdi_font_path.is_file(), f"missing {mdi_font_path}")

        # Every icon glyph actually drawn in the display lambda must be
        # declared in font_icons's glyphs list (verified codepoints - see
        # the display lambda tests above/below for where each is drawn).
        for glyph in (
            "\\U000F07D0",  # home-assistant (API connected, v3.3)
            "\\U000F0C9C",  # network-off-outline (API disconnected, v3.3)
            "\\U000F0083",  # battery-alert
            "\\U000F008E",  # battery-outline
            "\\U000F0079",  # battery (full)
            "\\U000F0084",  # battery-charging
            "\\U000F06A5",  # power-plug
            "\\U000F0F55",  # home-thermometer-outline (indoor, v3.3)
            "\\U000F0599",  # weather-sunny (outdoor, v3.3)
            "\\U000F112B",  # home-roof (attic, v3.3)
        ):
            with self.subTest(glyph=glyph):
                self.assertIn(glyph, font_block)

        # v3.1/v3.2's home/home-off pair is fully retired - it must not
        # linger in the glyphs list even though it's no longer drawn.
        self.assertNotIn("\\U000F02DC", font_block)
        self.assertNotIn("\\U000F1A46", font_block)

    def test_display_lambda_never_actuates(self) -> None:
        # Display path is pure rendering: reads globals/sensors and draws,
        # never reaches a script, a transmit, or a fan control call - on
        # EITHER target (banning only sx127x.send_packet here would be
        # blind to an SX1262-only regression).
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                display = top_level_block(text, "display")
                for banned in (
                    "script.execute",
                    "sx127x.send_packet",
                    "sx126x.send_packet",
                    "turn_on(",
                    "turn_off(",
                    "make_call(",
                ):
                    with self.subTest(banned=banned):
                        self.assertNotIn(banned, display)

    def test_preview_renderer_mirrors_firmware_draw_positions(self) -> None:
        # The Python preview renderer went stale once (32/41 vs the
        # firmware's 29/40, and the wrong "ID SAVED" font). Pin its shared
        # constants to the actual YAML draw calls so the mirror can't
        # silently drift again.
        renderer = (ROOT / "tools" / "render_display.py").read_text()

        def renderer_y(name: str) -> int:
            match = re.search(rf"{name} = \(LEFT_ZONE_CENTER_X, (\d+)\)", renderer)
            self.assertIsNotNone(match, name)
            return int(match.group(1))

        display = top_level_block(self.text, "display")
        state_draw = re.search(
            r"it\.print\(left_zone_center_x, (\d+), id\(font_state\)", display
        )
        self.assertEqual(renderer_y("STATE_WORD_POS"), int(state_draw.group(1)))
        countdown_draw = re.search(
            r"it\.printf\(left_zone_center_x, (\d+), id\(font_timer\)", display
        )
        self.assertEqual(renderer_y("COUNTDOWN_POS"), int(countdown_draw.group(1)))
        self.assertIn(
            'id(font_learn_prompt), TextAlign::TOP_CENTER, "ID SAVED"', display
        )
        self.assertIn('LEARN_PROMPT_POS, font_learn_prompt, "ID SAVED"', renderer)

    def test_render_display_tool_exists(self) -> None:
        renderer = ROOT / "tools" / "render_display.py"
        self.assertTrue(renderer.is_file(), f"missing {renderer}")
        text = renderer.read_text()
        self.assertIn("KEEP IN SYNC", text)
        self.assertIn("learn_active", text)
        self.assertIn("learn_confirm", text)
        self.assertIn('"learn-active"', text)
        self.assertIn('"learn-confirmed"', text)
        self.assertIn('c.register("learn_title", "LEFT"', text)
        self.assertIn('c.register("learn_prompt", "LEFT"', text)

    # -------------------------------------------------------------------
    # Timer tracking
    # -------------------------------------------------------------------

    def test_timer_globals_present_with_rollover_safe_type(self) -> None:
        globals_block = top_level_block(self.text, "globals")
        self.assertIn("id: timer_active", globals_block)
        self.assertIn("type: bool", globals_block)
        self.assertIn("id: timer_expiry_millis", globals_block)
        self.assertIn("type: uint32_t", globals_block)

    def test_timer_request_never_starts_countdown_before_confirmation(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                timer = script_blocks(text)["send_timer"]
                self.assertIn("speed_nibble | duration_nibble", timer)
                for optimistic_mutation in (
                    "id(timer_active) =",
                    "id(timer_armed_hours) =",
                    "id(timer_expiry_millis) =",
                    "id(timer_remaining_sensor).publish_state",
                ):
                    self.assertNotIn(optimistic_mutation, timer)

    def test_every_new_command_invalidates_expiry_inference_without_mutating_confirmed_timer(self) -> None:
        # A request makes a previously predicted expiry uncertain, but must
        # not rewrite confirmed timer metadata until a state response
        # arrives. The invalidation lives once, in begin_transaction, before
        # the enqueue; neither it nor any wrapper mutates timer metadata.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            scripts = script_blocks(text)
            begin = scripts["begin_transaction"]
            tx_index = begin.index("id: tx_burst")
            for invalidation in (
                "id(fan_state_known) = false;",
                "id(timer_state_known) = false;",
                "id(fan_state_known_sensor).publish_state(false);",
                "id(timer_state_known_sensor).publish_state(false);",
                "id(fan_confirmed_off_sensor).publish_state(false);",
            ):
                with self.subTest(config=config_name, token=invalidation):
                    self.assertIn(invalidation, begin)
                    self.assertLess(begin.index(invalidation), tx_index)
            for script_id in ("begin_transaction", "send_off", "send_low",
                              "send_medium", "send_high", "send_timer"):
                with self.subTest(config=config_name, script_id=script_id):
                    block = scripts[script_id]
                    for optimistic_mutation in (
                        "id(timer_active) =",
                        "id(timer_armed_hours) =",
                        "id(timer_expiry_millis) =",
                        "id(timer_remaining_sensor).publish_state",
                    ):
                        self.assertNotIn(optimistic_mutation, block)

    def test_timer_countdown_requires_matching_local_command_confirmation(self) -> None:
        for config_name, text, radio_key in (
            ("SX1278", self.text, "sx127x"),
            ("SX1262", self.v3_text, "sx126x"),
        ):
            radio = top_level_block(text, radio_key)
            coordinator = interval_item_containing(text, "id(cl_report_ready)")
            anchor_start = coordinator.index("bool locally_anchored_timer")
            anchor = coordinator[anchor_start : coordinator.index(";", anchor_start)]
            timer_authority_start = coordinator.index("bool timer_authoritative")
            timer_authority = coordinator[
                timer_authority_start : coordinator.index(";", timer_authority_start)
            ]
            with self.subTest(config=config_name):
                self.assertNotIn("id(timer_expiry_millis) =", radio)
                for nibble, hours in (
                    ("0x1", "1"),
                    ("0x2", "2"),
                    ("0x4", "4"),
                    ("0x8", "8"),
                    ("0xC", "12"),
                ):
                    self.assertIn(
                        f"if (actual_duration == {nibble}) actual_timer_hours = {hours};",
                        coordinator.replace("else if", "if"),
                    )
                self.assertIn("report_for_transaction", anchor)
                self.assertIn("state_matches", anchor)
                self.assertIn("id(cl_last_command_completed_ms) != 0", anchor)
                self.assertIn("state_authoritative", timer_authority)
                self.assertIn(
                    "actual_timer_hours == 0 || locally_anchored_timer",
                    timer_authority,
                )
                self.assertIn(
                    "id(cl_last_command_completed_ms) + actual_timer_hours * 3600000UL",
                    coordinator,
                )

    def test_unanchored_active_timer_promotes_fan_but_not_timer_state(self) -> None:
        # A correlated active-timer report fully determines running/speed even
        # when it cannot supply a remaining-time anchor. Fan authority must be
        # independent; timer authority stays false and clears stale countdown.
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            coordinator = interval_item_containing(text, "id(cl_report_ready)")
            state_start = coordinator.index("bool state_authoritative")
            state_authority = coordinator[
                state_start : coordinator.index(";", state_start)
            ]
            timer_start = coordinator.index("bool timer_authoritative", state_start)
            timer_authority = coordinator[
                timer_start : coordinator.index(";", timer_start)
            ]
            fan_assignment = coordinator.index(
                "id(fan_state_known) = state_authoritative;", timer_start
            )
            fan_publish = coordinator.index(
                "id(quietcool_fan).publish_state();", fan_assignment
            )
            timer_assignment = coordinator.index(
                "id(timer_state_known) = timer_authoritative;", fan_publish
            )
            fan_branch = coordinator[fan_assignment:timer_assignment]
            timer_guard = coordinator.index(
                "if (timer_authoritative) {", timer_assignment
            )
            timer_reconcile = coordinator[
                timer_guard : coordinator.index("id(cl_candidate_state)", timer_guard)
            ]
            with self.subTest(config=config_name):
                self.assertIn("report_authoritative", state_authority)
                self.assertIn("!future_energizing_work", state_authority)
                self.assertNotIn("locally_anchored_timer", state_authority)
                self.assertNotIn("actual_timer_hours", state_authority)
                self.assertIn("state_authoritative", timer_authority)
                self.assertIn(
                    "actual_timer_hours == 0 || locally_anchored_timer",
                    timer_authority,
                )
                self.assertTrue(fan_assignment < fan_publish < timer_assignment)
                # Every active timer has nonzero duration, so this atomic
                # interlock remains false even when fan state is promoted.
                self.assertIn(
                    "id(fan_confirmed_off_sensor).publish_state(actual_duration == 0);",
                    fan_branch,
                )
                self.assertIn("id(timer_active) = false;", timer_reconcile)
                self.assertIn(
                    "id(timer_remaining_sensor).publish_state(NAN);", timer_reconcile
                )
                self.assertIn(
                    "id(timer_state_known_sensor).publish_state(false);",
                    timer_reconcile,
                )

    def test_timer_expiry_interval_never_transmits(self) -> None:
        interval_block = interval_item_containing(self.text, "Estimated timer deadline reached")
        self.assertIn("id(timer_active)", interval_block)
        self.assertIn(
            "(int32_t) (id(timer_expiry_millis) - millis())", interval_block
        )

        known_guard = interval_block.index("if (!id(timer_state_known))")
        self.assertIn("id(timer_active) = false;", interval_block)
        self.assertIn("id(fan_state_known) = false;", interval_block)
        self.assertIn("id(timer_state_known) = false;", interval_block)
        self.assertIn("id(fan_confirmed_off_sensor).publish_state(false);", interval_block)
        self.assertNotIn("id(quietcool_fan).publish_state();", interval_block)

        timer_sync = interval_item_containing(
            self.text, "id(fan_timer_select).publish_state(desired)"
        )
        self.assertIn("if (!id(timer_state_known))", timer_sync)

        timer_sensor = list_item_containing(
            self.text, "sensor", 'name: "Timer Remaining"'
        )
        self.assertIn("if (!id(timer_state_known))", timer_sensor)
        self.assertIn("return {};", timer_sensor)

        # Safety invariant: the expiry path must never transmit.
        self.assertNotIn("script.execute", interval_block)
        self.assertNotIn("sx127x.send_packet", interval_block)
        self.assertNotIn("turn_off(", interval_block)
        self.assertNotIn("turn_on(", interval_block)
        self.assertNotIn("make_call(", interval_block)

    def test_timer_remaining_sensor_present(self) -> None:
        aggregate = list_item_containing(self.text, "sensor", 'name: "Timer Remaining"')
        self.assertIn("platform: template", aggregate)
        self.assertIn("id: timer_remaining_sensor", aggregate)
        self.assertIn('unit_of_measurement: "min"', aggregate)
        self.assertRegex(aggregate, r"(?m)^    update_interval: (?:[3-5][0-9]|60)s$")
        self.assertIn("if (!id(timer_active))", aggregate)
        self.assertIn("return 0;", aggregate)
        self.assertNotIn("on_value", aggregate)
        self.assertNotIn("script.execute", aggregate)
        self.assertNotIn("sx127x.send_packet", aggregate)

    # -------------------------------------------------------------------
    # Battery telemetry (TTGO LoRa32 V2.1 onboard LiPo circuit, GPIO35)
    # -------------------------------------------------------------------

    def test_battery_adc_sensor_configured(self) -> None:
        item = list_item_containing(self.text, "sensor", 'name: "Battery Voltage"')
        self.assertIn("platform: adc", item)
        self.assertIn("pin: GPIO35", item)
        self.assertIn("id: battery_voltage_sensor", item)
        self.assertIn("entity_category: diagnostic", item)
        self.assertIn("attenuation: 12db", item)
        self.assertRegex(item, r"(?m)^    update_interval: (?:[1-5][0-9]|60)s$")
        # 2:1 onboard divider must be undone so the published value is real
        # battery voltage, not half of it, and the reading must be
        # smoothed against ADC/noise jitter.
        self.assertIn("multiply: 2.0", item)
        self.assertIn("sliding_window_moving_average", item)

    def test_gpio35_was_previously_unused(self) -> None:
        # The battery ADC is only safe to add because GPIO35 wasn't wired
        # to anything else in this config; guard against a future edit
        # accidentally double-assigning it as an actual pin: elsewhere
        # (comments mentioning GPIO35 don't count).
        pin_assignments = re.findall(r"(?m)^\s*pin:\s*GPIO35\s*$", self.text)
        self.assertEqual(
            pin_assignments, ["    pin: GPIO35"], "GPIO35 should be assigned as a pin: exactly once (the battery ADC)"
        )

    def test_battery_level_percent_sensor_present(self) -> None:
        item = list_item_containing(self.text, "sensor", 'name: "Battery Level"')
        self.assertIn("platform: template", item)
        self.assertIn("id: battery_level_sensor", item)
        self.assertIn('unit_of_measurement: "%"', item)
        self.assertIn("device_class: battery", item)
        self.assertIn("internal: false", item)
        self.assertIn("id(battery_voltage_sensor).state", item)
        self.assertIn("id(battery_voltage_sensor).has_state()", item)
        self.assertIn("std::isfinite(id(battery_voltage_sensor).state)", item)
        # No battery attached (very low/implausible voltage) must publish
        # NAN, not a false 0%, mirroring the has_state()+isfinite() NAN
        # guard used for the temperature sensors above.
        self.assertIn("if (v < 2.5f)", item)
        self.assertIn("return NAN;", item)

    def test_battery_percent_curve_is_documented_with_breakpoints(self) -> None:
        # "Document the curve" requirement: the piecewise-linear LiPo
        # voltage->percent table must actually be present and span the
        # full 0-100% range, not just a naive two-point linear map.
        item = list_item_containing(self.text, "sensor", 'name: "Battery Level"')
        self.assertIn("CURVE_V[]", item)
        self.assertIn("CURVE_PCT[]", item)
        self.assertIn("4.20f", item)  # 100%
        self.assertIn("3.27f", item)  # 0% floor
        self.assertIn("100, 95, 90, 85", item)
        self.assertIn("50,  45, 40, 35", item)
        self.assertIn("10, 5, 0", item)
        # Linear interpolation between adjacent breakpoints, not a lookup
        # snapped to the nearest one.
        self.assertIn("float frac = (v - CURVE_V[i + 1]) / (CURVE_V[i] - CURVE_V[i + 1]);", item)

    def test_battery_sensors_have_no_automation_attached(self) -> None:
        # Telemetry only, exactly like the temperature sensors: nothing in
        # the battery path may reach a script, a transmit, or the fan
        # entity - operator requirement ("no-actuation from battery path").
        for marker in ('name: "Battery Voltage"', 'name: "Battery Level"'):
            with self.subTest(marker=marker):
                item = list_item_containing(self.text, "sensor", marker)
                for banned in ("on_value", "script.execute", "sx127x.send_packet", "quietcool_fan"):
                    with self.subTest(banned=banned):
                        self.assertNotIn(banned, item)

    def test_battery_icon_state_global_is_display_only(self) -> None:
        # Purely cosmetic hysteresis state, exactly like fan_anim_frame:
        # must exist as a global, must only be touched inside the display
        # lambda, and must never reach any TX/RX/timer/actuation path.
        globals_block = top_level_block(self.text, "globals")
        self.assertIn("id: battery_icon_state", globals_block)
        start = globals_block.index("- id: battery_icon_state")
        next_id = globals_block.find("\n  - id:", start + 1)
        item = globals_block[start:] if next_id == -1 else globals_block[start:next_id]
        self.assertIn("type: int", item)
        self.assertIn("restore_value: false", item)
        self.assertIn('initial_value: "-1"', item)

        display = top_level_block(self.text, "display")
        self.assertIn("id(battery_icon_state)", display)

        for section_name in ("script", "sx127x", "interval", "fan", "button", "binary_sensor", "sensor"):
            with self.subTest(section=section_name):
                section = top_level_block(self.text, section_name)
                self.assertNotIn("battery_icon_state", section)

    def test_battery_icon_thresholds_have_hysteresis(self) -> None:
        # Operator requirement: "Thresholds must have hysteresis so the
        # icon doesn't flicker at boundaries." Extract the RISE/FALL
        # arrays and prove every boundary actually has a gap (FALL[i] <
        # RISE[i]), not just duplicated numbers.
        display = top_level_block(self.text, "display")
        rise_match = re.search(r"BATT_RISE\[(\d+)\]\s*=\s*\{([^}]+)\}", display)
        fall_match = re.search(r"BATT_FALL\[(\d+)\]\s*=\s*\{([^}]+)\}", display)
        self.assertIsNotNone(rise_match, "BATT_RISE array not found in display lambda")
        self.assertIsNotNone(fall_match, "BATT_FALL array not found in display lambda")
        declared_size = int(rise_match.group(1))
        self.assertEqual(declared_size, int(fall_match.group(1)))
        rise = [float(v.strip().rstrip("f")) for v in rise_match.group(2).split(",")]
        fall = [float(v.strip().rstrip("f")) for v in fall_match.group(2).split(",")]
        self.assertEqual(len(rise), declared_size)
        self.assertEqual(len(fall), declared_size)
        # 5 boundaries -> 6 tiers (hidden/alert/outline/full/charging/plugged).
        self.assertEqual(declared_size, 5)
        for i, (r, f) in enumerate(zip(rise, fall)):
            with self.subTest(boundary=i):
                self.assertLess(f, r, f"boundary {i}: FALL {f} must be strictly below RISE {r}")
        # Thresholds ascend monotonically (each boundary is above the last).
        self.assertEqual(rise, sorted(rise))
        self.assertEqual(fall, sorted(fall))

        # The hysteresis walk must be driven by the PREVIOUS tier (read
        # before being overwritten), not recomputed from scratch every
        # frame - otherwise the persisted state couldn't do anything.
        prev_read = display.index("int prev_batt_tier = id(battery_icon_state);")
        state_write = display.index("id(battery_icon_state) = batt_tier;")
        self.assertLess(prev_read, state_write)

    def test_battery_icon_hidden_when_no_battery_attached(self) -> None:
        # Operator requirement: "icon HIDDEN entirely when the reading says
        # no battery is attached (< ~2.5 V)." Tier 0 must gate the only
        # battery glyph draw call - no glyph is ever emitted for tier 0.
        display = top_level_block(self.text, "display")
        self.assertIn("if (batt_tier > 0) {", display)
        battery_start = display.index("KEEP IN SYNC: BATTERY_ICON")
        next_section = display.index("KEEP IN SYNC:", battery_start + 1)
        battery_block = display[battery_start:next_section]
        # Exactly one draw call for the battery icon, and it must be
        # inside the tier>0 guard (i.e. after the guard's opening brace).
        self.assertEqual(battery_block.count("it.print("), 1)
        guard_index = battery_block.index("if (batt_tier > 0) {")
        draw_index = battery_block.index("it.print(")
        self.assertLess(guard_index, draw_index)
        # The 2.5V no-battery floor feeds tier 0 via the lowest RISE entry.
        self.assertIn("BATT_RISE[5] = {2.55f,", battery_block)
        self.assertIn("BATT_FALL[5] = {2.45f,", battery_block)

    def test_battery_heuristic_is_documented_as_voltage_only(self) -> None:
        # Operator requirement: the TP4054's charge-status pin is not
        # wired to the ESP32, so true charge state is not readable - the
        # battery icon and Battery Level sensor are both a VOLTAGE
        # HEURISTIC, and that limitation must be stated plainly in a YAML
        # comment, not just implemented silently.
        self.assertIn("CHRG status pin is NOT", self.text)
        self.assertIn("heuristic", self.text.lower())

        # The HONEST LIMITATION comment sits directly above the ADC sensor
        # item (as section-level documentation, like the other big comment
        # blocks in this file), not inside the YAML item's own captured
        # text, so check the containing sensor: block rather than the
        # narrower per-item extraction used elsewhere in this file.
        sensors_block = top_level_block(self.text, "sensor")
        self.assertIn("HONEST LIMITATION", sensors_block)
        self.assertIn("battery_voltage_sensor", sensors_block)

        display = top_level_block(self.text, "display")
        self.assertIn("HONEST LIMITATION", display)
        battery_start = display.index("KEEP IN SYNC: BATTERY_ICON")
        next_section = display.index("KEEP IN SYNC:", battery_start + 1)
        battery_block = display[battery_start:next_section]
        self.assertIn("HONEST LIMITATION", battery_block)

    def test_battery_glyphs_are_strong_silhouettes_not_the_old_icons(self) -> None:
        # Regression guard: the numbered battery-20/40/70 glyphs were
        # tried first and rejected (verified by rendering them at 13px
        # 1-bit - they only differ by a faint fill band near the icon's
        # top, imperceptible at this size) in favor of three maximally-
        # distinct silhouettes (alert/outline/full) plus charging/plugged.
        display = top_level_block(self.text, "display")
        battery_start = display.index("KEEP IN SYNC: BATTERY_ICON")
        next_section = display.index("KEEP IN SYNC:", battery_start + 1)
        battery_block = display[battery_start:next_section]
        for glyph in (
            "\\U000F0083",  # battery-alert
            "\\U000F008E",  # battery-outline
            "\\U000F0079",  # battery (full)
            "\\U000F0084",  # battery-charging
            "\\U000F06A5",  # power-plug
        ):
            with self.subTest(glyph=glyph):
                self.assertIn(glyph, battery_block)
        for rejected_glyph in (
            "\\U000F007B",  # battery-20 - rejected, illegible at 13px
            "\\U000F007D",  # battery-40 - rejected, illegible at 13px
            "\\U000F0080",  # battery-70 - rejected, illegible at 13px
        ):
            with self.subTest(rejected_glyph=rejected_glyph):
                self.assertNotIn(rejected_glyph, battery_block)

    def test_oem_activity_arms_bounded_requery_from_rx_flags_only(self) -> None:
        # All three OEM-evidence RX sites (heard 66 query, accepted OEM
        # command, other validated passive frame) arm the initial query plus
        # one retry by setting flags only; RX still never executes a script.
        for name, text, radio_key in (
            ("lora32", self.text, "sx127x"),
            ("v3", self.v3_text, "sx126x"),
        ):
            with self.subTest(config=name):
                radio = top_level_block(text, radio_key)
                self.assertEqual(
                    radio.count("id(oem_requery_pending) = true;"), 3
                )
                self.assertEqual(radio.count("id(oem_activity_ms) = now;"), 3)
                self.assertEqual(
                    radio.count("id(oem_requery_retry_left) = 1;"), 3
                )
                self.assertEqual(
                    radio.count("id(oem_requery_due_ms) = now + 3000UL;"), 3
                )
                self.assertNotRegex(radio, r"(?m)^\s+- script\.execute:")

    def test_auto_requery_fires_one_bounded_query_after_quiet_window(self) -> None:
        for name, text in (("lora32", self.text), ("v3", self.v3_text)):
            with self.subTest(config=name):
                item = interval_item_containing(
                    text, "auto refresh after OEM remote activity"
                )
                condition = item[: item.index("id(oem_requery_pending) = false;")]
                self.assertIn("if (!id(oem_requery_pending)) return false;", condition)
                self.assertIn("since <= 30000UL", condition)
                self.assertIn(
                    "(int32_t)(millis() - id(oem_requery_due_ms)) >= 0",
                    condition,
                )
                self.assertIn("!id(fan_state_known)", condition)
                self.assertIn("!id(cl_active) && id(refire_left) == 0", condition)
                self.assertIn(
                    "!id(cl_query_epoch) && !id(cl_report_ready)", condition
                )
                self.assertIn("!id(tx_burst).is_running()", condition)
                self.assertIn(
                    "!id(learn_active) && id(learned_sender_id) != 0", condition
                )
                body = item[item.index("id(oem_requery_pending) = false;") :]
                self.assertIn("bool is_retry", item)
                self.assertIn("cmd: 0x66", body)
                self.assertEqual(body.count("script.execute"), 1)

    def test_auto_requery_request_expires_and_yields_to_restored_authority(self) -> None:
        for name, text in (("lora32", self.text), ("v3", self.v3_text)):
            with self.subTest(config=name):
                housekeeping = interval_item_containing(
                    text, "id(oem_query_seen) = false;"
                )
                drop = housekeeping[
                    housekeeping.index("id(oem_requery_pending) ||") :
                ]
                self.assertIn("id(fan_state_known) ||", drop)
                self.assertIn("(millis() - id(oem_activity_ms)) > 30000UL", drop)
                self.assertIn("id(oem_requery_pending) = false;", drop)
                self.assertIn("id(oem_requery_retry_left) = 0;", drop)
                self.assertIn("id(oem_requery_due_ms) = 0;", drop)

    def test_failed_oem_auto_refresh_gets_exactly_one_jittered_retry(self) -> None:
        for config_name, text in (("SX1278", self.text), ("SX1262", self.v3_text)):
            with self.subTest(config=config_name):
                globals_block = top_level_block(text, "globals")
                self.assertIn("- id: oem_requery_retry_left", globals_block)
                self.assertIn("- id: oem_requery_due_ms", globals_block)
                self.assertIn("- id: cl_query_auto_recovery", globals_block)

                # Every genuine OEM-evidence site renews one retry and a
                # three-second quiet deadline. It still only raises flags in
                # RX; no receive callback is allowed to transmit.
                self.assertEqual(
                    text.count("id(oem_requery_retry_left) = 1;"), 3
                )
                self.assertEqual(
                    text.count("id(oem_requery_due_ms) = now + 3000UL;"), 3
                )

                coordinator = interval_item_containing(text, "id(cl_report_ready)")
                manual_timeout = coordinator[
                    coordinator.index(
                        "if (id(cl_query_epoch) && !id(cl_query_epoch_confirmation)"
                    ) : coordinator.index(
                        "if (id(cl_query_epoch) &&", coordinator.index(
                            "if (id(cl_query_epoch) && !id(cl_query_epoch_confirmation)"
                        ) + 1
                    )
                ]
                self.assertIn("id(oem_requery_retry_left) > 0", manual_timeout)
                self.assertIn("<= 30000UL", manual_timeout)
                self.assertIn("id(oem_requery_retry_left)--;", manual_timeout)
                self.assertIn("id(oem_requery_pending) = true;", manual_timeout)
                self.assertIn("${closed_loop_response_tail_ms}", manual_timeout)
                self.assertIn("500UL + (id(oem_activity_ms) % 501UL)", manual_timeout)
                self.assertIn("id(cl_query_auto_recovery)", manual_timeout)
                self.assertIn(
                    "manual refresh missed; bounded OEM-recovery retry pending",
                    manual_timeout,
                )
                self.assertIn(
                    "auto refresh missed; one bounded retry pending",
                    manual_timeout,
                )

                tx_burst = script_blocks(text)["tx_burst"]
                self.assertIn("auto_recovery: bool", tx_burst)
                self.assertIn(
                    "id(cl_query_auto_recovery) = auto_recovery;", tx_burst
                )

                auto_requery = interval_item_containing(
                    text, "auto refresh after OEM remote activity"
                )
                action_start = auto_requery.index("          then:")
                condition = auto_requery[:action_start]
                self.assertIn(
                    "(int32_t)(millis() - id(oem_requery_due_ms)) >= 0",
                    condition,
                )
                body = auto_requery[action_start:]
                self.assertEqual(body.count("script.execute"), 1)
                self.assertEqual(body.count("cmd: 0x66"), 1)
                self.assertEqual(body.count("auto_recovery: true"), 1)

                refresh = list_item_containing(
                    text, "button", 'name: "Refresh Fan State"'
                )
                self.assertIn("auto_recovery: false", refresh)

    def test_display_state_word_marks_lost_authority(self) -> None:
        for name, text in (("lora32", self.text), ("v3", self.v3_text)):
            with self.subTest(config=name):
                display = top_level_block(text, "display")
                self.assertIn("KEEP IN SYNC: STATE_UNKNOWN", display)
                self.assertIn('id(fan_state_known) ? "" : "?"', display)
                # The embedded state font must actually contain the '?'
                # glyph, or the panel draws a missing-codepoint box.
                self.assertIn('glyphs: "OFLWMEDHIG?"', text)
        renderer = (ROOT / "tools" / "render_display.py").read_text()
        self.assertIn("KEEP IN SYNC: STATE_UNKNOWN", renderer)
        self.assertIn('("" if state.state_known else "?")', renderer)


if __name__ == "__main__":
    unittest.main()
