# Copyright 2026 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Hermetic unit tests for fetch_netsim main module."""

import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

# find_spec("pkg.sub") imports "pkg" and raises ModuleNotFoundError if absent.
if importlib.util.find_spec("oauth2client") is None:
    oauth2client_mod = types.ModuleType("oauth2client")
    oauth2client_mod.__path__ = []
    client_mod = types.ModuleType("oauth2client.client")
    client_mod.AccessTokenCredentials = mock.MagicMock
    client_mod.AccessTokenCredentialsError = type(
        "AccessTokenCredentialsError", (Exception,), {}
    )
    setattr(oauth2client_mod, "client", client_mod)
    sys.modules.setdefault("oauth2client", oauth2client_mod)
    sys.modules.setdefault("oauth2client.client", client_mod)

if importlib.util.find_spec("googleapiclient") is None:
    gapiclient_mod = types.ModuleType("googleapiclient")
    gapiclient_mod.__path__ = []
    errors_mod = types.ModuleType("googleapiclient.errors")
    errors_mod.HttpError = type("HttpError", (Exception,), {})
    discovery_mod = types.ModuleType("googleapiclient.discovery")
    http_mod = types.ModuleType("googleapiclient.http")
    setattr(gapiclient_mod, "errors", errors_mod)
    setattr(gapiclient_mod, "discovery", discovery_mod)
    setattr(gapiclient_mod, "http", http_mod)
    sys.modules.setdefault("googleapiclient", gapiclient_mod)
    sys.modules.setdefault("googleapiclient.errors", errors_mod)
    sys.modules.setdefault("googleapiclient.discovery", discovery_mod)
    sys.modules.setdefault("googleapiclient.http", http_mod)

if importlib.util.find_spec("tqdm") is None:
    tqdm_mod = types.ModuleType("tqdm")
    tqdm_mod.tqdm = mock.MagicMock
    sys.modules.setdefault("tqdm", tqdm_mod)

from aemu import main


def make_fake_unzip(
    include_proto: bool = True, proto_files: dict[str, str] | None = None
):
    """Creates a fake unzip_artifact side_effect."""

    def _unzip(ab_client, dest, name, artifact, bid):
        dest_path = Path(dest)
        dest_path.mkdir(parents=True, exist_ok=True)
        (dest_path / "netsim").write_text("netsim binary")
        (dest_path / "netsimd").write_text("netsimd binary")
        if include_proto:
            files = (
                proto_files
                if proto_files is not None
                else {
                    "netsim/common.proto": (
                        f'syntax = "proto3";\npackage netsim;\n// {name}\n'
                    ),
                    "rootcanal/configuration.proto": (
                        f'syntax = "proto3";\npackage rootcanal;\n// {name}\n'
                    ),
                }
            )
            for rel_path, content in files.items():
                p = dest_path / "proto" / rel_path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content)

    return _unzip


class FetchNetsimMainTest(unittest.TestCase):
    """Hermetic unit tests for obtain() and fetch_and_commit()."""

    def setUp(self):
        super().setUp()
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.base_dir = Path(self.temp_dir.name)

        subprocess.run(
            ["git", "init", str(self.base_dir)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(self.base_dir),
                "config",
                "user.name",
                "Test User",
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(self.base_dir),
                "config",
                "user.email",
                "test@google.com",
            ],
            check=True,
            capture_output=True,
        )

        self.repo_commands = []
        real_subprocess_run = subprocess.run

        def fake_subprocess_run(cmd, *popenargs, **kwargs):
            cmd_list = [str(c) for c in cmd]
            if cmd_list and cmd_list[0] == "git":
                kwargs["check"] = True
                return real_subprocess_run(cmd_list, *popenargs, **kwargs)
            if cmd_list and cmd_list[0] == "repo":
                self.repo_commands.append(cmd_list)
                return subprocess.CompletedProcess(
                    args=cmd_list, returncode=0
                )
            return real_subprocess_run(cmd, *popenargs, **kwargs)

        self.run_patcher = mock.patch(
            "aemu.main.subprocess.run", side_effect=fake_subprocess_run
        )
        self.mock_run = self.run_patcher.start()
        self.addCleanup(self.run_patcher.stop)

        self.build_client_patcher = mock.patch("aemu.main.AndroidBuildClient")
        self.mock_build_client = self.build_client_patcher.start()
        self.addCleanup(self.build_client_patcher.stop)

        self.args = argparse.Namespace(
            token="fake-token",
            artifact="netsim-{target}-{bid}.zip",
            prefix="netsim",
            dry_run=False,
            dest=str(self.base_dir),
            branch="git_main-netsim-dev",
            targets="linux_x64,mac_x64,mac_aarch64,windows_x64",
            reviewers="rev@google.com",
            cc="cc@google.com",
            netsim_version=None,
            canary_version=None,
            buganizer_id=None,
        )

    def get_git_files(self) -> set[str]:
        """Returns the set of files tracked or staged in git index."""
        res = subprocess.run(
            ["git", "-C", str(self.base_dir), "ls-files"],
            capture_output=True,
            text=True,
            check=True,
        )
        return set(res.stdout.splitlines())

    def get_head_files(self) -> set[str]:
        """Returns the set of files committed in HEAD."""
        res = subprocess.run(
            [
                "git",
                "-C",
                str(self.base_dir),
                "ls-tree",
                "-r",
                "--name-only",
                "HEAD",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        return set(res.stdout.splitlines())

    @mock.patch("aemu.main.unzip_artifact")
    def test_obtain_moves_proto_to_base_dir_and_strips_from_target_dir(
        self, mock_unzip
    ):
        """obtain() with default update_proto=True stages proto at base_dir/proto."""
        mock_unzip.side_effect = make_fake_unzip(include_proto=True)
        main.obtain(
            self.args,
            "16756866",
            "linux_x64",
            self.base_dir,
            "submitted",
        )

        proto_common = self.base_dir / "proto" / "netsim" / "common.proto"
        proto_config = (
            self.base_dir / "proto" / "rootcanal" / "configuration.proto"
        )
        self.assertTrue(proto_common.exists())
        self.assertTrue(proto_config.exists())
        self.assertFalse((self.base_dir / "linux-x86_64" / "proto").exists())

        common_text = proto_common.read_text(encoding="utf-8")
        self.assertIn("netsim_linux_x64", common_text)

        staged_files = self.get_git_files()
        self.assertIn("proto/netsim/common.proto", staged_files)
        self.assertIn("proto/rootcanal/configuration.proto", staged_files)
        self.assertIn("linux-x86_64/netsimd", staged_files)
        self.assertIn("linux-x86_64/netsim", staged_files)

    @mock.patch("aemu.main.unzip_artifact")
    def test_obtain_replaces_existing_base_dir_proto(self, mock_unzip):
        """obtain() replaces existing tracked proto and cleans up obsolete."""
        obsolete_file = self.base_dir / "proto" / "netsim" / "obsolete.proto"
        obsolete_file.parent.mkdir(parents=True, exist_ok=True)
        obsolete_file.write_text("syntax = \"proto3\"; // obsolete")
        subprocess.run(
            ["git", "-C", str(self.base_dir), "add", "proto"], check=True
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(self.base_dir),
                "commit",
                "-m",
                "add obsolete proto",
            ],
            check=True,
        )

        mock_unzip.side_effect = make_fake_unzip(include_proto=True)
        main.obtain(
            self.args,
            bid="12345",
            target="linux_x64",
            base_dir=self.base_dir,
            build_type="submitted",
            update_proto=True,
        )

        self.assertFalse(obsolete_file.exists())
        staged_files = self.get_git_files()
        self.assertNotIn("proto/netsim/obsolete.proto", staged_files)
        self.assertIn("proto/netsim/common.proto", staged_files)

    @mock.patch("aemu.main.unzip_artifact")
    def test_obtain_replaces_untracked_existing_base_dir_proto(
        self, mock_unzip
    ):
        """obtain() replaces untracked proto dir without git rm exit 128."""
        untracked_file = self.base_dir / "proto" / "netsim" / "untracked.proto"
        untracked_file.parent.mkdir(parents=True, exist_ok=True)
        untracked_file.write_text("syntax = \"proto3\"; // untracked")

        mock_unzip.side_effect = make_fake_unzip(include_proto=True)
        main.obtain(
            self.args,
            bid="12345",
            target="linux_x64",
            base_dir=self.base_dir,
            build_type="submitted",
            update_proto=True,
        )

        self.assertFalse(untracked_file.exists())
        staged_files = self.get_git_files()
        self.assertIn("proto/netsim/common.proto", staged_files)
        self.assertNotIn("proto/netsim/untracked.proto", staged_files)

    @mock.patch("aemu.main.unzip_artifact")
    def test_obtain_strips_target_proto_preserving_base_when_update_proto_false(
        self, mock_unzip
    ):
        """obtain() with update_proto=False strips dest/proto preserving base."""
        base_proto = self.base_dir / "proto" / "netsim" / "common.proto"
        base_proto.parent.mkdir(parents=True, exist_ok=True)
        sentinel_content = "syntax = \"proto3\"; // SENTINEL CONTENT"
        base_proto.write_text(sentinel_content)
        subprocess.run(
            ["git", "-C", str(self.base_dir), "add", "proto"], check=True
        )

        mock_unzip.side_effect = make_fake_unzip(
            include_proto=True,
            proto_files={
                "netsim/common.proto": (
                    "syntax = \"proto3\"; // ARCHIVE OVERRIDE"
                )
            },
        )

        main.obtain(
            self.args,
            bid="12345",
            target="mac_x64",
            base_dir=self.base_dir,
            build_type="submitted",
            update_proto=False,
        )

        dest = self.base_dir / "darwin-x86_64"
        self.assertFalse((dest / "proto").exists())
        self.assertEqual(base_proto.read_text(), sentinel_content)
        staged_files = self.get_git_files()
        self.assertIn("darwin-x86_64/netsimd", staged_files)

    @mock.patch("aemu.main.unzip_artifact")
    def test_obtain_without_proto_in_archive_preserves_legacy_behavior(
        self, mock_unzip
    ):
        """obtain() handles archives lacking proto without creating base proto."""
        mock_unzip.side_effect = make_fake_unzip(include_proto=False)
        main.obtain(
            self.args,
            bid="12345",
            target="linux_x64",
            base_dir=self.base_dir,
            build_type="submitted",
            update_proto=True,
        )

        self.assertFalse((self.base_dir / "proto").exists())
        staged_files = self.get_git_files()
        self.assertIn("linux-x86_64/netsimd", staged_files)
        self.assertIn("linux-x86_64/netsim", staged_files)

    @mock.patch("aemu.main.unzip_artifact")
    def test_fetch_and_commit_updates_proto_once_and_strips_all_target_subdirs(
        self, mock_unzip
    ):
        """fetch_and_commit() updates proto once and commits across targets."""
        mock_unzip.side_effect = make_fake_unzip(include_proto=True)
        main.fetch_and_commit(
            self.args,
            bid="12345",
            build_type="submitted",
            destination_dir=self.base_dir,
        )

        head_files = self.get_head_files()
        self.assertIn("proto/netsim/common.proto", head_files)
        self.assertIn("proto/rootcanal/configuration.proto", head_files)

        common_proto = (
            self.base_dir / "proto" / "netsim" / "common.proto"
        ).read_text(encoding="utf-8")
        self.assertIn("netsim_linux_x64", common_proto)
        self.assertNotIn("netsim_mac_x64", common_proto)

        expected_targets = [
            "linux-x86_64",
            "darwin-x86_64",
            "darwin-aarch64",
            "windows_msvc-x86_64",
        ]
        for t in expected_targets:
            self.assertIn(f"{t}/netsimd", head_files)
            self.assertIn(f"{t}/netsim", head_files)
            self.assertFalse((self.base_dir / t / "proto").exists())
            for hf in head_files:
                self.assertFalse(hf.startswith(f"{t}/proto/"))

        self.assertEqual(len(self.repo_commands), 1)
        self.assertEqual(self.repo_commands[0][0], "repo")
        self.assertEqual(self.repo_commands[0][1], "upload")

    @mock.patch("aemu.main.unzip_artifact")
    def test_fetch_and_commit_pending_build_updates_proto_on_first_successful_target(
        self, mock_unzip
    ):
        """Pending build updates proto on first successful target if earlier fail."""
        def _unzip_with_failure(ab_client, dest, name, artifact, bid):
            if "linux_x64" in name:
                raise RuntimeError("Simulated failure for linux_x64")
            make_fake_unzip(include_proto=True)(
                ab_client, dest, name, artifact, bid
            )

        mock_unzip.side_effect = _unzip_with_failure
        self.args.targets = "linux_x64, mac_x64,"

        main.fetch_and_commit(
            self.args,
            bid="P12345",
            build_type="pending",
            destination_dir=self.base_dir,
        )

        head_files = self.get_head_files()
        self.assertIn("proto/netsim/common.proto", head_files)
        self.assertIn("proto/rootcanal/configuration.proto", head_files)
        self.assertIn("darwin-x86_64/netsimd", head_files)
        self.assertNotIn("linux-x86_64/netsimd", head_files)
        self.assertFalse((self.base_dir / "darwin-x86_64" / "proto").exists())

        common_proto = (
            self.base_dir / "proto" / "netsim" / "common.proto"
        ).read_text(encoding="utf-8")
        self.assertIn("netsim_mac_x64", common_proto)
        self.assertNotIn("netsim_linux_x64", common_proto)

        self.assertEqual(len(self.repo_commands), 1)


if __name__ == "__main__":
    unittest.main()
