import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "quietcool-lora32.yaml"
SECRETS = ROOT / "secrets.yaml"
README = ROOT / "README.md"

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


def list_item_containing(text: str, section_name: str, marker: str) -> str:
    section = top_level_block(text, section_name)
    for match in re.finditer(r"(?ms)^  - platform:.*?(?=^  - platform:|\Z)", section):
        item = match.group(0)
        if marker in item:
            return item
    raise ValueError(f"No {section_name} item contains {marker!r}")


class QuietCoolESPHomeConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CONFIG.read_text()
        cls.secrets_text = SECRETS.read_text() if SECRETS.exists() else ""
        cls.readme_text = README.read_text()

    def test_radio_profile_matches_recovered_firmware(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        expected_lines = (
            "  frequency: 433920000",
            "  modulation: FSK",
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
        # Post-refactor invariant (FIX 1): sx127x.send_packet appears
        # exactly once in the whole config, inside the single queued core
        # script tx_burst. The four fixed state scripts and send_timer are
        # thin wrappers that route through it and contain neither
        # send_packet nor delay:, so they always complete synchronously and
        # can never drop a rapid re-press.
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

        for wrapper_id in ("send_off", "send_low", "send_medium", "send_high", "send_timer"):
            with self.subTest(script_id=wrapper_id):
                wrapper = scripts[wrapper_id]
                self.assertNotIn("sx127x.send_packet", wrapper)
                self.assertNotIn("delay:", wrapper)
                self.assertIn("id: tx_burst", wrapper)

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
        self.assertIn('quietcool_sender_id: "0xCB004739"', substitutions)

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
        self.assertIn("return id(learned_sender_id) == 0;", tx_burst)
        self.assertIn('ESP_LOGE("TX"', tx_burst)

        zero_guard = tx_burst.index("return id(learned_sender_id) == 0;")
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

    def test_default_seed_preserves_our_four_end_to_end_payloads(self) -> None:
        seed = 0xCB004739
        sender_bytes = [(seed >> shift) & 0xFF for shift in (24, 16, 8, 0)]
        self.assertEqual(sender_bytes, [0xCB, 0x00, 0x47, 0x39])
        expected = {
            "send_off": [0xCB, 0x00, 0x47, 0x39, 0x90, 0x90],
            "send_low": [0xCB, 0x00, 0x47, 0x39, 0x9F, 0x9F],
            "send_medium": [0xCB, 0x00, 0x47, 0x39, 0xAF, 0xAF],
            "send_high": [0xCB, 0x00, 0x47, 0x39, 0xBF, 0xBF],
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
        # These four command bytes are byte-identical to the previously
        # verified firmware and must never change. The actual six-byte
        # payload (sender ID + repeated command byte) is constructed in
        # exactly one place: tx_burst (see test_only_tx_burst_transmits).
        expected = {
            "send_off": 0x90,
            "send_low": 0x9F,
            "send_medium": 0xAF,
            "send_high": 0xBF,
        }
        scripts = script_blocks(self.text)
        for script_id, command_byte in expected.items():
            with self.subTest(script_id=script_id):
                self.assertIn(script_id, scripts)
                wrapper = scripts[script_id]
                self.assertIn("id: tx_burst", wrapper)
                self.assertRegex(wrapper, rf"(?m)^\s+cmd:\s*0x{command_byte:02X}\s*$")
                self.assertNotIn("sx127x.send_packet", wrapper)
                self.assertNotIn("delay:", wrapper)

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
            {"tx_burst", "send_off", "send_low", "send_medium", "send_high", "send_timer"},
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

        # Routes the actual transmission through the single core TX script
        # instead of transmitting directly (see test_only_tx_burst_transmits).
        self.assertIn("id: tx_burst", timer)
        self.assertNotIn("sx127x.send_packet", timer)
        self.assertNotIn("delay:", timer)

        # After queuing the TX, the fan entity is published as on at the
        # speed that was actually sent, without going through the control
        # path (see test_rx_and_timer_updates_never_use_control_path).
        self.assertIn("id(quietcool_fan).state = true;", timer)
        self.assertIn("id(quietcool_fan).speed = observed_speed;", timer)
        self.assertIn("id(quietcool_fan).publish_state();", timer)

    def test_timer_publish_is_not_gated_by_a_delay(self) -> None:
        # FIX 2: tx_burst's mode: queued handles RF timing asynchronously,
        # so send_timer's own entity publish must never be sequenced after
        # a delay: - that would let a later OFF press race ahead on air
        # while the entity still visually showed the timer's on-at-speed
        # state. Since send_timer contains no delay: at all (enforced by
        # test_timer_script_is_speed_aware), this is a redundant but
        # explicit regression guard for the ordering specifically.
        scripts = script_blocks(self.text)
        timer = scripts["send_timer"]
        publish_index = timer.index("id(quietcool_fan).publish_state();")
        self.assertNotIn("delay:", timer[:publish_index])

    def test_timer_buttons_use_speed_aware_script_with_correct_durations(self) -> None:
        expected = {
            "Timer 1H": "0x01",
            "Timer 2H": "0x02",
            "Timer 4H": "0x04",
        }
        for name, duration in expected.items():
            with self.subTest(button=name):
                marker = f'name: "{name}"'
                index = self.text.index(marker)
                # The button's on_press block is a small, bounded window
                # after its name.
                window = self.text[index : index + 400]
                self.assertIn("id: send_timer", window)
                self.assertIn(f"duration_nibble: {duration}", window)

    def test_boot_never_transmits(self) -> None:
        boot = top_level_block(self.text, "esphome")
        self.assertNotIn("sx127x.send_packet", boot)
        self.assertNotIn("script.execute", boot)
        self.assertNotIn("fan.turn_on", boot)
        self.assertNotIn("fan.turn_off", boot)
        # The boot lambda performs a raw (unpublished) speed-field
        # initialization on the fan entity (FIX 3; see
        # test_bare_turn_on_after_boot_defaults_to_low_not_high) but must
        # never publish_state()/turn_on()/turn_off()/make_call() it - any
        # of those would fire on_state and therefore transmit at boot.
        self.assertNotIn("quietcool_fan).publish_state()", boot)
        self.assertNotIn("quietcool_fan).turn_on(", boot)
        self.assertNotIn("quietcool_fan).turn_off(", boot)
        self.assertNotIn("quietcool_fan).make_call(", boot)
        self.assertIn("id(quietcool_fan).speed = 1;", boot)

    def test_bare_turn_on_after_boot_defaults_to_low_not_high(self) -> None:
        # ESPHome's FanCall::validate_() maps "turn on with no explicit
        # speed while Fan::speed == 0" to full speed (High). With
        # restore_mode: NO_RESTORE, speed starts at its class default of 0
        # unless something else sets it first. The boot lambda pre-seeds
        # speed to 1 (Low) via a raw field write (no publish, no TX)
        # specifically to avoid a bare post-boot/OTA fan.turn_on defaulting
        # to High.
        boot = top_level_block(self.text, "esphome")
        fan_block = top_level_block(self.text, "fan")
        self.assertIn("restore_mode: NO_RESTORE", fan_block)
        self.assertIn("id(quietcool_fan).speed = 1;", boot)
        self.assertIn("FanCall::validate_", boot)

    def test_fan_restore_mode_prevents_boot_time_publish(self) -> None:
        # ESPHome's Fan::restore_state_() returns an empty optional only for
        # NO_RESTORE; every other restore mode (including the platform
        # default ALWAYS_OFF) causes FanRestoreState::apply() to call
        # publish_state() during setup(), which would fire on_state (and
        # therefore transmit) at boot. NO_RESTORE is the only setting that
        # guarantees setup() never calls publish_state().
        fan_block = top_level_block(self.text, "fan")
        self.assertIn("restore_mode: NO_RESTORE", fan_block)

    def test_fan_entity_uses_unconditional_on_state_not_edge_triggers(self) -> None:
        fan_block = top_level_block(self.text, "fan")
        # on_turn_on/on_turn_off/on_speed_set are edge-triggered (they only
        # fire when the value actually changes), which is why the old
        # "Fan OFF" button was a no-op when the entity already read as off,
        # and could double-fire when state and speed changed together. A
        # single on_state trigger (unconditional on every publish_state())
        # replaces all three.
        self.assertIn("on_state:", fan_block)
        self.assertNotIn("on_turn_on:", fan_block)
        self.assertNotIn("on_turn_off:", fan_block)
        self.assertNotIn("on_speed_set:", fan_block)
        self.assertIn("rf_echo_guard", fan_block)

    def test_fan_off_button_is_unconditional(self) -> None:
        index = self.text.index('name: "Fan OFF"')
        window = self.text[index : index + 400]
        self.assertIn("fan.turn_off: quietcool_fan", window)
        # Must not be gated by any condition - "Fan OFF" always transmits,
        # even when the entity already reads as off.
        on_press_index = window.index("on_press:")
        action_window = window[on_press_index:]
        self.assertNotIn("if:", action_window)

    def test_rx_validation_rejects_malformed_frames(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("x.size() != 6", radio)
        self.assertIn("x[4] != x[5]", radio)
        self.assertIn("sender_id != id(learned_sender_id)", radio)
        for nibble in ("0x90", "0xA0", "0xB0"):
            self.assertIn(nibble, radio)
        for nibble in ("0x0", "0x1", "0x2", "0x4", "0x8", "0xC", "0xF"):
            self.assertIn(f"duration_nibble == {nibble}", radio)

    def test_rx_handles_wake_query_and_special_frame_without_publishing(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("cmd == 0x66", radio)
        self.assertRegex(
            radio,
            r"x\[0\] == 0xCE && \(sender_id & 0x00FFFFFFUL\) ==\s*"
            r"\(id\(learned_sender_id\) & 0x00FFFFFFUL\)",
        )

        # Neither branch may publish the fan entity or execute a script
        # before its own return - both are log-and-ignore only.
        wake_start = radio.index("if (cmd == 0x66)")
        wake_end = radio.index("return;", wake_start) + len("return;")
        wake_branch = radio[wake_start:wake_end]
        self.assertNotIn("publish_state", wake_branch)
        self.assertNotIn("script.execute", wake_branch)
        self.assertNotIn("quietcool_fan", wake_branch)

        ce_start = radio.index("if (x[0] == 0xCE")
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
        interval_block = top_level_block(self.text, "interval")
        self.assertIn("id(learn_active)", interval_block)
        self.assertIn("120000UL", interval_block)
        self.assertIn("id(learn_auto_mode)", interval_block)
        self.assertIn("id(learn_candidate_id) = 0;", interval_block)
        self.assertNotIn("script.execute", interval_block)
        self.assertNotIn("sx127x.send_packet", interval_block)

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
        # FIX 2: this unit compiles with a nonzero seed
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
        learn_button = list_item_containing(
            self.text, "button", 'name: "Learn Remote ID"'
        )
        self.assertIn("id(learn_active) = true;", learn_button)
        self.assertIn("id(learn_auto_mode) = false;", learn_button)
        self.assertIn("id(learn_window_started) = millis();", learn_button)

        forget_button = list_item_containing(
            self.text, "button", 'name: "Forget Remote ID"'
        )
        self.assertIn("id(learned_sender_id) = 0;", forget_button)
        self.assertIn("learned_sender_id->update();", forget_button)
        self.assertIn("global_preferences->sync();", forget_button)
        self.assertIn("id(learn_active) = true;", forget_button)
        self.assertIn("id(learn_auto_mode) = true;", forget_button)
        self.assertIn('publish_state("unset")', forget_button)

        for item in (learn_button, forget_button):
            for banned in ("sx127x.send_packet", "script.execute", "fan.turn_on", "fan.turn_off"):
                with self.subTest(banned=banned):
                    self.assertNotIn(banned, item)

    def test_prg_very_long_press_enters_manual_learn_without_collision(self) -> None:
        prg = list_item_containing(self.text, "binary_sensor", 'name: "PRG Button"')
        self.assertIn("min_length: 1000ms", prg)
        self.assertIn("max_length: 4999ms", prg)
        self.assertIn("min_length: 5000ms", prg)
        self.assertIn("max_length: 10000ms", prg)
        very_long = prg[prg.index("min_length: 5000ms") :]
        self.assertIn("id(learn_active) = true;", very_long)
        self.assertIn("id(learn_auto_mode) = false;", very_long)
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

        # FIX 6: sliding window, comfortably above the 45 ms intra-burst
        # gap and the burst length, asserted at its actual value.
        self.assertIn("(now - id(last_valid_rx_time)) < 450", radio)

        # The timestamp must be refreshed unconditionally (between the
        # duplicate computation and the return-on-duplicate branch), not
        # only inside the "accepted" path - otherwise a genuine second
        # press of the same command could still be swallowed by a stale
        # reference point from the first burst.
        is_duplicate_decl = radio.index("bool is_duplicate")
        # Anchor after the declaration: the learn-accept block seeds the
        # same tracker earlier in the lambda, and .index() would find that
        # write first.
        command_write = radio.index("id(last_valid_rx_command) = cmd;", is_duplicate_decl)
        if_duplicate = radio.index("if (is_duplicate)", is_duplicate_decl)
        self.assertLess(is_duplicate_decl, command_write)
        self.assertLess(command_write, if_duplicate)

    def test_rx_accepts_neutral_off_80_for_observed_state_only(self) -> None:
        # Observed live from the upstairs installation's OEM remote
        # (2026-07-18): its Off button transmits 80 80 - speed nibble 8
        # ("no remembered speed"), duration 0 - which the original
        # downstairs-derived 9/A/B whitelist rejected, leaving the entity
        # stuck on the previous state. 0x80 must be decodable as OFF, but
        # only exactly 0x80: nibble-8 frames with a nonzero duration stay
        # rejected until physically observed, and 0x80 must never appear in
        # any TX payload.
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("bool off_neutral = (cmd == 0x80);", radio)
        self.assertIn("if (!(speed_ok && duration_ok) && !off_neutral)", radio)
        # Decode branch: off_neutral turns the entity off WITHOUT touching
        # its remembered speed.
        off_branch = radio.index("if (off_neutral)")
        state_false = radio.index("id(quietcool_fan).state = false;", off_branch)
        self.assertGreater(state_false, off_branch)
        # No TX path may carry 0x80: the only send_packet lives in tx_burst,
        # whose payload comes from the wrapper scripts' cmd bytes - none of
        # which may be 0x80.
        scripts = top_level_block(self.text, "script")
        self.assertNotIn("0x80", scripts)

    def test_learn_accept_seeds_burst_dedup_tracker(self) -> None:
        # Seen on the upstairs onboarding (2026-07-18): learn accepted on a
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

    def test_rx_ignores_observed_state_while_tx_burst_in_flight(self) -> None:
        # FIX 4: our own TX cannot self-receive (half-duplex), so this only
        # ever matters for genuine concurrent OEM traffic. The frame is
        # still counted in diagnostics; only the entity publish is skipped.
        radio = top_level_block(self.text, "sx127x")
        self.assertIn("id(tx_burst).is_running()", radio)

        diag_index = radio.index(
            "id(rx_valid_count_sensor).publish_state(id(rx_valid_count_sensor).state + 1);"
        )
        guard_index = radio.index("id(tx_burst).is_running()")
        publish_index = radio.index("id(quietcool_fan).publish_state();")
        self.assertLess(diag_index, guard_index)
        self.assertLess(guard_index, publish_index)

    def test_rx_publishes_observed_state_without_control_path(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        # Must mutate the fan entity's fields directly and call
        # publish_state() so nothing is echoed back over RF - never through
        # turn_on()/turn_off()/make_call(), which would run control() and
        # (via on_state) transmit.
        self.assertIn("id(quietcool_fan).speed = observed_speed;", radio)
        self.assertIn("id(quietcool_fan).state = (duration_nibble != 0x00);", radio)
        self.assertIn("id(quietcool_fan).publish_state();", radio)
        self.assertNotIn("quietcool_fan).turn_on(", radio)
        self.assertNotIn("quietcool_fan).turn_off(", radio)
        self.assertNotIn("quietcool_fan).make_call(", radio)
        self.assertNotIn("fan.turn_on", radio)
        self.assertNotIn("fan.turn_off", radio)

    def test_rx_uses_echo_guard_around_publish(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        guard_true = radio.index("id(rf_echo_guard) = true;")
        publish = radio.index("id(quietcool_fan).publish_state();", guard_true)
        guard_false = radio.index("id(rf_echo_guard) = false;", publish)
        self.assertTrue(guard_true < publish < guard_false)

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
        self.assertIn("if (running && id(timer_active)) {", left_text_block)
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
        # never reaches a script, a transmit, or a fan control call. This
        # is the same invariant test_only_tx_burst_transmits enforces for
        # the RF path, applied to the display block specifically.
        display = top_level_block(self.text, "display")
        for banned in (
            "script.execute",
            "sx127x.send_packet",
            "turn_on(",
            "turn_off(",
            "make_call(",
        ):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, display)

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

    def test_send_timer_starts_countdown_with_full_duration_map(self) -> None:
        scripts = script_blocks(self.text)
        timer = scripts["send_timer"]
        self.assertIn("id(timer_active) = true;", timer)
        self.assertIn(
            "id(timer_expiry_millis) = millis() + timer_hours * 3600000UL;",
            timer,
        )
        for nibble, hours in (
            ("0x1", "1"),
            ("0x2", "2"),
            ("0x4", "4"),
            ("0x8", "8"),
            ("0xC", "12"),
        ):
            with self.subTest(nibble=nibble):
                self.assertIn(f"case {nibble}: timer_hours = {hours}; break;", timer)

    def test_non_timer_tx_scripts_clear_timer(self) -> None:
        # send_off/send_low/send_medium/send_high all transmit a duration
        # nibble of 0x0 (off) or 0xF (continuous) - a non-timer command
        # that replaces any running timer, exactly like the physical fan.
        scripts = script_blocks(self.text)
        for script_id in ("send_off", "send_low", "send_medium", "send_high"):
            with self.subTest(script_id=script_id):
                wrapper = scripts[script_id]
                self.assertIn("id(timer_active) = false;", wrapper)
                self.assertIn("id(timer_remaining_sensor).publish_state(0);", wrapper)

    def test_rx_sets_and_clears_timer_after_validation(self) -> None:
        radio = top_level_block(self.text, "sx127x")
        for nibble, hours in (
            ("0x1", "1"),
            ("0x2", "2"),
            ("0x4", "4"),
            ("0x8", "8"),
            ("0xC", "12"),
        ):
            with self.subTest(nibble=nibble):
                self.assertIn(
                    f"case {nibble}: observed_timer_hours = {hours}; break;", radio
                )
        self.assertIn("id(timer_active) = true;", radio)
        self.assertIn("id(timer_active) = false;", radio)

        # The timer bookkeeping must come after every existing validation
        # step (length/sender/duplicate/is_running) and after the observed
        # fan-entity publish, never weakening or bypassing them.
        guard_index = radio.index("id(tx_burst).is_running()")
        publish_index = radio.index("id(quietcool_fan).publish_state();")
        timer_index = radio.index("uint32_t observed_timer_hours")
        self.assertLess(guard_index, publish_index)
        self.assertLess(publish_index, timer_index)

    def test_timer_expiry_interval_never_transmits(self) -> None:
        interval_block = top_level_block(self.text, "interval")
        self.assertIn("id(timer_active)", interval_block)
        self.assertIn(
            "(int32_t) (id(timer_expiry_millis) - millis())", interval_block
        )

        guard_true = interval_block.index("id(rf_echo_guard) = true;")
        publish = interval_block.index(
            "id(quietcool_fan).publish_state();", guard_true
        )
        guard_false = interval_block.index("id(rf_echo_guard) = false;", publish)
        self.assertTrue(guard_true < publish < guard_false)
        self.assertIn("id(quietcool_fan).state = false;", interval_block)
        self.assertIn("id(timer_active) = false;", interval_block)

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
        # Purely cosmetic hysteresis state, exactly like fan_anim_angle:
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

    def test_battery_path_never_actuates(self) -> None:
        # Same invariant test_display_lambda_never_actuates already proves
        # for the whole display block, checked explicitly against just the
        # battery-icon section for a direct, self-documenting regression
        # guard on this specific addition.
        display = top_level_block(self.text, "display")
        battery_start = display.index("KEEP IN SYNC: BATTERY_ICON")
        next_section = display.index("KEEP IN SYNC:", battery_start + 1)
        battery_block = display[battery_start:next_section]
        for banned in ("script.execute", "sx127x.send_packet", "turn_on(", "turn_off(", "make_call("):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, battery_block)

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


if __name__ == "__main__":
    unittest.main()
