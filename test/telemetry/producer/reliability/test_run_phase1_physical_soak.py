#!/usr/bin/env python3
"""Regression contracts for the opt-in physical soak orchestrator."""
from __future__ import annotations
import json
import hashlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNNER = ROOT / "run_phase1_physical_soak.py"
SOAK_FIXTURE = ROOT / "fixtures" / "telemetry_phase1_soak_solo.fs2"


class PhysicalSoakContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(); self.root = Path(self.temp.name)
        self.game = self.root / "game.py"; self.console = self.root / "console.py"; self.preflight = self.root / "preflight.py"; self.restart = self.root / "restart.py"; self.state = self.root / "state.json"; self.pid_file = self.root / "restart.pid"
        self.mission = self.root / "soak.fs2"; self.config = self.root / "telemetry.json"
        self.game_root = self.root / "game-root"; self.game_root.mkdir()
        self.binary = self.game_root / Path(sys.executable).name; shutil.copy2(sys.executable, self.binary)
        self.mission.write_text("#Mission Info\n$Name: Soak\n#End\n", encoding="utf-8")
        self.config.write_text('{"enabled": true, "bindPort": 42042, "maxClients": 1}\n', encoding="utf-8")
        self.state.write_text('{"session":"100", "generation":1}', encoding="utf-8")
        self.game.write_text("import time\ntime.sleep(5)\n", encoding="utf-8")
        self.preflight.write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
        self.console.write_text(textwrap.dedent("""\
            import argparse, json, os, time
            p=argparse.ArgumentParser(); p.add_argument('--host'); p.add_argument('--port'); p.add_argument('--seconds',type=float); p.add_argument('--stale-ms'); a=p.parse_args()
            time.sleep(a.seconds)
            state=json.load(open(os.environ['SOAK_STATE'], encoding='utf-8'))
            if os.environ.get('FSO_TELEMETRY_PHYSICAL_SOAK_PROCESS_EPOCH') == '2': state={'session':'300','generation':1}
            print(json.dumps({'status':'Live','session':state['session'],'mission_generation':state['generation']}))
        """), encoding="utf-8")
        self.restart.write_text("import json, os\njson.dump({'session':'200','generation':2}, open(os.environ['SOAK_STATE'],'w',encoding='utf-8'))\n", encoding="utf-8")

    def tearDown(self) -> None: self.temp.cleanup()

    def invoke(self, *extra: str, environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        env = {**__import__('os').environ, "SOAK_STATE": str(self.state)}
        if environment:
            env.update(environment)
        native = "--preferences-mode" in extra and extra[extra.index("--preferences-mode") + 1] == "native"
        preferences = [] if native else ["--profile-dir", str(self.root / "profile")]
        return subprocess.run([sys.executable, str(RUNNER), "--binary", str(self.binary), "--game-args-json", json.dumps([str(self.game)]), "--mission", str(self.mission), "--config", str(self.config), "--console-client", str(self.console), "--mission-preflight-command-json", json.dumps([sys.executable, str(self.preflight)]), *preferences, "--output-dir", str(self.root / "out"), "--workdir", str(self.game_root), "--duration-seconds", "0.08", *extra], text=True, capture_output=True, env=env, check=False)

    def report(self) -> dict: return json.loads((self.root / "out" / "physical-soak-report.json").read_text(encoding="utf-8"))

    def test_normal_requires_final_live_handshake_and_wall_clock(self) -> None:
        result = self.invoke(); self.assertEqual(0, result.returncode, result.stderr)
        report = self.report(); self.assertEqual("passed", report["status"]); self.assertTrue(report["bindHandshake"]["observed"]); self.assertGreaterEqual(report["wallDurationSeconds"], .08); self.assertEqual("Live", report["phases"]["pre"]["finalStatus"])
        self.assertEqual("mainhall", report["launchMode"])
        self.assertEqual(str(self.game_root.resolve()), report["inputs"]["workdir"]["path"])
        self.assertNotIn("-start_mission", report["producer"]["command"])
        self.assertEqual("diagnostic-only", report["attestation"]["classification"])
        self.assertFalse(report["attestation"]["ac16Eligible"])
        self.assertEqual("launchMode=mainhall does not attest a mission run", report["attestation"]["reason"])
        self.assertFalse(report["deployment"]["mission"]["deployed"])
        self.assertFalse(report["deployment"]["restoration"]["mission"]["attempted"])

    def test_game_arguments_are_exact_without_runner_display_or_focus_flags(self) -> None:
        supplied = [str(self.game), "-mod", "Warzone,mediavps", "-nospec", "-noglow"]
        result = self.invoke("--game-args-json", json.dumps(supplied))
        self.assertEqual(0, result.returncode, result.stderr)
        command = self.report()["producer"]["command"]
        self.assertEqual([str(self.binary.resolve()), *supplied], command)
        self.assertNotIn("-window", command)
        self.assertNotIn("-window_res", command)
        self.assertNotIn("-no_unfocused_pause", command)

    def test_start_mission_is_explicit_opt_in(self) -> None:
        result = self.invoke("--launch-mode", "start-mission")
        self.assertEqual(0, result.returncode, result.stderr)
        report = self.report()
        self.assertEqual("start-mission", report["launchMode"])
        self.assertEqual([str(self.binary.resolve()), str(self.game), "-start_mission", self.mission.name], report["producer"]["command"])
        self.assertEqual("ac16-eligible-candidate", report["attestation"]["classification"])
        self.assertTrue(report["attestation"]["ac16Eligible"])
        self.assertTrue(report["deployment"]["mission"]["deployedSha256"])
        self.assertTrue(report["deployment"]["restoration"]["mission"]["restored"])

    def test_runner_rejects_case_insensitive_start_mission_game_argument_before_deployment(self) -> None:
        result = self.invoke("--game-args-json", json.dumps([str(self.game), "-START_MISSION", self.mission.name]))
        self.assertEqual(2, result.returncode)
        self.assertIn("must not contain runner-owned -start_mission", result.stderr)
        self.assertFalse((self.root / "profile").exists())
        self.assertFalse((self.root / "out").exists())

    def test_binary_outside_workdir_is_rejected_before_deployment(self) -> None:
        outside_binary = self.root / Path(sys.executable).name; shutil.copy2(sys.executable, outside_binary)
        command = [sys.executable, str(RUNNER), "--binary", str(outside_binary), "--game-args-json", json.dumps([str(self.game)]), "--mission", str(self.mission), "--config", str(self.config), "--console-client", str(self.console), "--mission-preflight-command-json", json.dumps([sys.executable, str(self.preflight)]), "--profile-dir", str(self.root / "outside-profile"), "--output-dir", str(self.root / "outside-out"), "--workdir", str(self.game_root), "--duration-seconds", "0.08"]
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(2, result.returncode)
        self.assertIn("binary must reside directly in --workdir", result.stderr)
        self.assertFalse((self.root / "outside-profile").exists())

    def test_producer_exit_during_client_fails_with_reported_exit_code(self) -> None:
        self.game.write_text("", encoding="utf-8")
        result = self.invoke(); self.assertEqual(1, result.returncode)
        report = self.report(); self.assertIn("producer exited", report["error"]); self.assertEqual(0, report["producer"]["initialExitCode"])

    def test_stale_final_chronology_fails_even_after_live(self) -> None:
        self.console.write_text("import argparse,json,time\np=argparse.ArgumentParser();p.add_argument('--host');p.add_argument('--port');p.add_argument('--seconds',type=float);p.add_argument('--stale-ms');a=p.parse_args();time.sleep(a.seconds);print(json.dumps({'status':'Live','session':'100','mission_generation':1}));print(json.dumps({'status':'Stale','session':'100','mission_generation':1}))\n", encoding="utf-8")
        result = self.invoke(); self.assertEqual(1, result.returncode); self.assertIn("did not end in a Live", self.report()["error"])

    def test_mission_restart_requires_same_pid_new_session_and_generation(self) -> None:
        missing_checkpoint = self.invoke("--mode", "restart-mission")
        self.assertEqual(2, missing_checkpoint.returncode)
        self.assertIn("Restart Mission, then Fly Mission", missing_checkpoint.stderr)
        rejected = self.invoke("--mode", "restart-mission", "--restart-command-json", json.dumps([sys.executable, str(self.restart)])); self.assertEqual(2, rejected.returncode); self.assertIn("--launch-mode start-mission", rejected.stderr)
        result = self.invoke("--launch-mode", "start-mission", "--mode", "restart-mission", "--restart-command-json", json.dumps([sys.executable, str(self.restart)])); self.assertEqual(0, result.returncode, result.stderr)
        report = self.report(); self.assertEqual("100", report["phases"]["pre"]["session"]); self.assertEqual("200", report["phases"]["post"]["session"]); self.assertEqual(2, report["phases"]["post"]["missionGeneration"])
        self.assertEqual("external-manual-checkpoint", report["restart"]["control"])
        checkpoint = report["restart"]["checkpoint"]
        self.assertEqual("Restart Mission, wait for the briefing, then select Fly Mission", checkpoint["operatorAction"])
        self.assertIn("new session", checkpoint["postObservation"])

    def test_mission_restart_rejects_briefing_only_post_observation(self) -> None:
        self.restart.write_text("import json, os\njson.dump({'session':'200','generation':0}, open(os.environ['SOAK_STATE'],'w',encoding='utf-8'))\n", encoding="utf-8")
        self.console.write_text(textwrap.dedent("""\
            import argparse, json, os, time
            p=argparse.ArgumentParser(); p.add_argument('--host'); p.add_argument('--port'); p.add_argument('--seconds',type=float); p.add_argument('--stale-ms'); a=p.parse_args()
            time.sleep(a.seconds)
            state=json.load(open(os.environ['SOAK_STATE'], encoding='utf-8'))
            status='Live' if state['generation'] == 1 else 'Synchronizing'
            print(json.dumps({'status':status,'session':state['session'],'mission_generation':state['generation']}))
        """), encoding="utf-8")
        result = self.invoke("--launch-mode", "start-mission", "--mode", "restart-mission", "--restart-command-json", json.dumps([sys.executable, str(self.restart)]))
        self.assertEqual(1, result.returncode)
        self.assertIn("post after Restart Mission then Fly Mission console chronology did not end in a Live", self.report()["error"])

    def test_restart_mission_help_explains_the_manual_checkpoint(self) -> None:
        result = subprocess.run([sys.executable, str(RUNNER), "--help"], text=True, capture_output=True, check=False)
        self.assertEqual(0, result.returncode)
        self.assertIn("Restart Mission,\n                        then Fly Mission", result.stdout)
        self.assertIn("never automates game UI", result.stdout)

    def test_restart_process_is_runner_controlled_with_new_pid_and_session(self) -> None:
        rejected = self.invoke("--mode", "restart-process", "--restart-command-json", json.dumps([sys.executable, str(self.restart)])); self.assertEqual(2, rejected.returncode); self.assertIn("runner-controlled", rejected.stderr)
        result = self.invoke("--mode", "restart-process"); self.assertEqual(0, result.returncode, result.stderr)
        report = self.report(); self.assertEqual("runner-controlled", report["restart"]["control"]); self.assertNotEqual(report["producer"]["initialPid"], report["producer"]["restartPid"]); self.assertNotEqual(report["phases"]["pre"]["session"], report["phases"]["post"]["session"]); self.assertIsNotNone(report["producer"]["initialExitCode"])

    def test_restart_process_fails_if_relaunched_producer_exits_during_post_client(self) -> None:
        self.game.write_text("import os, time\nif os.environ.get('FSO_TELEMETRY_PHYSICAL_SOAK_PROCESS_EPOCH') == '2': raise SystemExit(9)\ntime.sleep(5)\n", encoding="utf-8")
        result = self.invoke("--mode", "restart-process")
        self.assertEqual(1, result.returncode)
        self.assertIn("restarted producer exited", self.report()["error"])

    def test_bind_port_and_preflight_are_mandatory(self) -> None:
        self.config.write_text('{"enabled": true, "bindPort": 7}\n', encoding="utf-8")
        result = self.invoke(); self.assertEqual(2, result.returncode); self.assertIn("bindPort", result.stderr)
        self.preflight.write_text("import sys\nsys.exit(4)\n", encoding="utf-8"); self.config.write_text('{"enabled": true, "bindPort": 42042}\n', encoding="utf-8")
        result = self.invoke(); self.assertEqual(1, result.returncode); self.assertIn("preflight failed", self.report()["error"])

    def test_game_root_files_are_restored_after_campaign(self) -> None:
        target_config = self.game_root / "data" / "config" / "telemetry.json"; target_mission = self.game_root / "data" / "missions" / self.mission.name
        target_config.parent.mkdir(parents=True); target_mission.parent.mkdir(parents=True); target_config.write_text('{"old":true}', encoding="utf-8"); target_mission.write_text("old mission", encoding="utf-8")
        result = self.invoke(); self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual('{"old":true}', target_config.read_text(encoding="utf-8")); self.assertEqual("old mission", target_mission.read_text(encoding="utf-8")); self.assertFalse((self.root / "profile" / "data" / "config" / "telemetry.json").exists())
        restoration = self.report()["deployment"]["restoration"]
        self.assertTrue(restoration["config"]["restored"]); self.assertFalse(restoration["mission"]["attempted"])

    def test_config_restores_even_if_mission_restoration_refuses_tampered_target(self) -> None:
        target_mission = self.game_root / "data" / "missions" / self.mission.name
        target_mission.parent.mkdir(parents=True); target_mission.write_text("old mission", encoding="utf-8")
        self.preflight.write_text("import os\nfrom pathlib import Path\nPath(os.environ['FSO_TELEMETRY_PHYSICAL_SOAK_MISSION']).write_text('tampered', encoding='utf-8')\n", encoding="utf-8")
        result = self.invoke("--launch-mode", "start-mission")
        self.assertEqual(1, result.returncode)
        report = self.report(); restoration = report["deployment"]["restoration"]
        self.assertFalse(restoration["mission"]["restored"])
        self.assertTrue(restoration["config"]["restored"])
        self.assertFalse((self.root / "profile" / "data" / "config" / "telemetry.json").exists())

    def test_profile_with_user_mod_file_is_rejected_before_launch(self) -> None:
        mod_file = self.root / "profile" / "data" / "mod.ini"; mod_file.parent.mkdir(parents=True); mod_file.write_text("forbidden", encoding="utf-8")
        result = self.invoke()
        self.assertEqual(2, result.returncode)
        self.assertIn("profile directory must be empty", result.stderr)

    def test_native_preferences_guard_restores_explicit_config_without_fso_override(self) -> None:
        native = self.root / "native-preferences"; target = native / "data" / "config" / "telemetry.json"
        target.parent.mkdir(parents=True); target.write_text('{"old":true}', encoding="utf-8")
        self.preflight.write_text("import os, sys\nkeys=('USERPROFILE','APPDATA','LOCALAPPDATA')\nok='FSO_PREFERENCES_PATH' not in os.environ and all(os.environ.get(key, '')==os.environ.get('EXPECTED_'+key, '') for key in keys)\nsys.exit(0 if ok else 6)\n", encoding="utf-8")
        environment = {"FSO_PREFERENCES_PATH": "must-be-removed"}
        environment.update({"EXPECTED_" + key: __import__('os').environ.get(key, '') for key in ('USERPROFILE', 'APPDATA', 'LOCALAPPDATA')})
        result = self.invoke("--preferences-mode", "native", "--preferences-dir", str(native), environment=environment)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual('{"old":true}', target.read_text(encoding="utf-8"))
        report = self.report(); profile = report["profile"]
        self.assertEqual("native", profile["mode"])
        self.assertEqual("native-guarded-telemetry-config", profile["configurationPolicy"])
        self.assertEqual(str(target.resolve()), report["deployment"]["config"]["target"])
        self.assertTrue(report["deployment"]["restoration"]["config"]["restored"])

    def test_native_preferences_directory_is_explicit_and_must_exist(self) -> None:
        result = self.invoke("--preferences-mode", "native", "--preferences-dir", str(self.root / "does-not-exist"))
        self.assertEqual(2, result.returncode)
        self.assertIn("must already exist", result.stderr)
        result = self.invoke("--preferences-mode", "native")
        self.assertEqual(2, result.returncode)
        self.assertIn("requires --preferences-dir", result.stderr)

    def test_dedicated_soak_fixture_is_a_quiet_single_player_fred_mission(self) -> None:
        """Keep the physical soak source auditable without treating it as executed proof."""
        contents = SOAK_FIXTURE.read_text(encoding="utf-8")
        self.assertTrue(hashlib.sha256(contents.encode("utf-8")).hexdigest())
        for header in ("#Mission Info", "#Players", "#Objects", "#Wings", "#Events", "#Goals", "#End"):
            self.assertIn(header, contents)
        self.assertIn('$Name: Telemetry Soak 1', contents)
        self.assertIn('$Class: GTF Ulysses', contents)
        self.assertIn('"player-start"', contents)
        self.assertIn('"invulnerable"', contents)
        self.assertEqual(1, contents.count('$Name: Telemetry Soak 1'))
        self.assertIn('#Wings\t\t;! 1 total', contents)
        wing_block = contents.split('#Wings', 1)[1].split('#Events', 1)[0]
        self.assertIn('$Name: Alpha', wing_block)
        self.assertIn('$Special Ship: 0\t\t;! Telemetry Soak 1', wing_block)
        self.assertIn('$Ships: (\t\t;! 1 total\n\t"Telemetry Soak 1"\n)', wing_block)
        self.assertNotIn('"Telemetry Soak 2"', wing_block)
        self.assertIn('#Events\t\t;! 0 total', contents)
        self.assertIn('#Goals\t\t;! 0 total', contents)
        self.assertNotIn('end-mission', contents.lower())
        object_block = contents.split('#Objects', 1)[1].split('#Wings', 1)[0].lower()
        self.assertNotIn('$team: hostile', object_block)

    def test_dedicated_soak_fixture_has_required_empty_background_fields_before_asteroids(self) -> None:
        """Mission parser requires these fields immediately after #Background bitmaps."""
        contents = SOAK_FIXTURE.read_text(encoding="utf-8")
        background = contents.split('#Background bitmaps', 1)[1].split('\n', 1)[1].split('#Asteroid Fields', 1)[0]
        self.assertEqual(['$Num stars: 0', '$Ambient light level: 7895160'], [
            line for line in background.splitlines() if line.strip()
        ])

    @unittest.skipUnless(__import__('os').name == "nt", "Windows environment contract")
    def test_windows_preserves_user_profile_environment_and_reports_policy(self) -> None:
        self.preflight.write_text("import os, sys\nkeys=('USERPROFILE','APPDATA','LOCALAPPDATA')\nok=all(os.environ.get(key)==os.environ.get('EXPECTED_'+key) for key in keys) and os.environ.get('FSO_PREFERENCES_PATH')==os.environ.get('EXPECTED_FSO_PREFERENCES_PATH')\nsys.exit(0 if ok else 6)\n", encoding="utf-8")
        env = __import__('os').environ
        previous = {key: env.get('EXPECTED_' + key) for key in ('USERPROFILE','APPDATA','LOCALAPPDATA')}
        try:
            for key in previous: env['EXPECTED_' + key] = env.get(key, '')
            env['EXPECTED_FSO_PREFERENCES_PATH'] = str(self.root / 'profile')
            result = self.invoke()
        finally:
            for key, value in previous.items():
                if value is None: env.pop('EXPECTED_' + key, None)
                else: env['EXPECTED_' + key] = value
            env.pop('EXPECTED_FSO_PREFERENCES_PATH', None)
        self.assertEqual(0, result.returncode, result.stderr)
        profile = self.report()["profile"]
        self.assertEqual("preserve-windows-user-profile", profile["environmentPolicy"])
        self.assertEqual("isolated-fso-preferences-path", profile["configurationPolicy"])
        self.assertEqual("FSO_PREFERENCES_PATH", profile["preferencesEnvironmentVariable"])


if __name__ == "__main__": unittest.main()
