"""Exercise the production hygiene predicate and real Git ignore semantics."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from check_repository_hygiene import (APP_VALUE_HEADERS, broken_links, heading_anchors,
                                      include_direction_violations, private_files,
                                      read_allowlist, unexpected_media)


class RepositoryHygieneTests(unittest.TestCase):
    def test_allowlist_and_capture_rejection(self):
        root = Path(__file__).resolve().parent.parent
        allowed = read_allowlist(root)
        rejected = {'capture.png', 'capture.JPG', 'clip.mp4', 'clip.GIF',
                    'crash.dmp', 'debug.log', 'docs/ui-references/new-capture.png',
                    'docs/evidence/private.log', 'docs/assets/recording.png',
                    'apps/easnap-desktop/resources/private.jpeg'}
        self.assertEqual(unexpected_media(allowed | rejected, allowed), sorted(rejected))

        # This repository may not have Git metadata yet. Do not initialize or stage it.
        local = root / '.local'
        local.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hygiene-', dir=local) as directory:
            fixture = Path(directory)
            subprocess.run(['git', 'init', '--quiet', str(fixture)], check=True)
            shutil.copyfile(root / '.gitignore', fixture / '.gitignore')
            ignored = rejected | {'build/CMakeCache.txt', 'build-old/output.obj',
                                  '.local/backup.zip', '.serena/cache/symbols.pkl',
                                  '.serena/project.local.yml', '.agents/mcp_config.json',
                                  'AGENTS.md', 'Easnap-UI-Modernization-Agent-Prompt.en.md',
                                  '.agents/rules/autonomous-development.md',
                                  '.serena/project.yml', '.serena/.gitignore',
                                  'docs/evidence/run.json', 'docs/ui-references/reference.txt',
                                  '.env', '.env.production', 'config/.env.local',
                                  'credentials.json', 'config/credentials.local.json',
                                  'config/secrets.json', 'config/secrets.local.yml',
                                  'config/settings.local.json', 'settings.local.toml',
                                  'signing/release.pfx', 'signing/release.key',
                                  'signing/release.pem', 'signing/release.p12',
                                  'signing/release.cer', 'signing/release.crt',
                                  'signing/release.p7b', 'signing/release.p7c',
                                  'config/settings.local.yml', 'config/settings.local.yaml',
                                  'id_rsa', 'id_dsa', 'id_ecdsa', 'id_ed25519',
                                  '.aws/credentials', '.azure/token.json', '.ssh/config',
                                  '.npmrc', '.netrc', '_netrc', '.git-credentials'}
            visible = allowed | {'.github/workflows/build.yml', 'apps/easnap-desktop/src/main.cpp',
                                 'docs/decisions/ADR-024-native-trim-frame.md'}
            paths = sorted(ignored | visible)
            result = subprocess.run(['git', '-C', str(fixture), 'check-ignore',
                                     '--no-index', '-z', '--stdin'],
                                    input=('\0'.join(paths) + '\0').encode(),
                                    stdout=subprocess.PIPE, check=False)
            self.assertIn(result.returncode, (0, 1))
            actual = set(result.stdout.decode().rstrip('\0').split('\0'))
            self.assertEqual(actual, ignored)


    def test_allowlisted_assets_exist(self):
        root = Path(__file__).resolve().parent.parent
        missing = sorted(path for path in read_allowlist(root) if not (root / path).is_file())
        self.assertEqual(missing, [], 'Every publication image must exist and be reviewed.')

    def test_tracked_private_files_cannot_bypass_ignore(self):
        private = {'.env', '.env.production', 'nested/.env.local', 'credentials.json',
                   'config/secrets.json', 'config/secrets.local.toml',
                   'config/settings.local.yaml', 'signing/key.pem', 'signing/key.pfx',
                   '.ssh/id_ed25519', '.aws/credentials', '.azure/token.json',
                   '.agents/hooks.json', '.serena/project.yml', 'AGENTS.md',
                   'docs/evidence/run.json', 'docs/ui-references/notes.txt',
                   'Easnap-UI-Modernization-Agent-Prompt.en.md'}
        public = {'apps/easnap-desktop/src/main.cpp', '.github/workflows/build.yml',
                  'docs/architecture.md', 'scripts/check_repository_hygiene.py',
                  'config/settings.json', 'docs/images/editor.png'}
        self.assertEqual(private_files(private | public), sorted(private))
        self.assertEqual(private_files({'DOCS/EVIDENCE/run.json', r'.agents\hooks.json'}),
                         sorted({'DOCS/EVIDENCE/run.json', r'.agents\hooks.json'}))

    def test_broken_links_and_anchors_are_reported(self):
        root = Path(__file__).resolve().parent.parent
        local = root / '.local'
        local.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hygiene-links-', dir=local) as directory:
            fixture = Path(directory)
            (fixture / 'docs/sub').mkdir(parents=True)
            (fixture / 'docs/guide.md').write_text(
                '# Guide\n\n## Known limits (beta)\n\n## `Code` and *style*\n\n## Twice\n\n## Twice\n',
                encoding='utf-8')
            (fixture / 'docs/sub/page.md').write_text(
                '[ok](../guide.md) [anchor](../guide.md#known-limits-beta) [code](../guide.md#code-and-style)\n'
                '[second](../guide.md#twice-1) [self](#here) [dir](../sub) [web](https://example.org/x.md)\n'
                '[mail](mailto:someone@example.org) <img src="../picture.png"> <a href="#here">x</a>\n'
                '[gone](../missing.md) [private](../../notes/private.md) [bad](../guide.md#no-such-part)\n'
                '[outside](../../../elsewhere.md) [nowhere](#absent)\n'
                '```\n[in a code block](never.md)\n```\n`[inline](never.md)`\n\n## Here\n',
                encoding='utf-8')
            (fixture / 'notes').mkdir()
            (fixture / 'notes/private.md').write_text('# Exists on disk, not in the inventory\n',
                                                       encoding='utf-8')
            tracked = ['docs/guide.md', 'docs/sub/page.md', 'docs/picture.png']
            self.assertEqual(broken_links(fixture, tracked), [
                'docs/sub/page.md: ../missing.md',
                'docs/sub/page.md: ../../notes/private.md',
                'docs/sub/page.md: ../guide.md#no-such-part (no such heading)',
                'docs/sub/page.md: ../../../elsewhere.md',
                'docs/sub/page.md: #absent (no such heading)'])
            self.assertEqual(broken_links(fixture, ['docs/guide.md']), [])
        self.assertEqual(heading_anchors('# v1.0.2\n## A [link](x.md) here\n'),
                         {'v102', 'a-link-here'})

    def test_documents_of_this_repository_have_no_broken_links(self):
        root = Path(__file__).resolve().parent.parent
        result = subprocess.run(['git', 'ls-files', '-z'], cwd=root, check=False,
                                stdout=subprocess.PIPE)
        if result.returncode != 0:
            self.skipTest('not a Git checkout')
        paths = result.stdout.decode('utf-8').rstrip('\0').split('\0')
        self.assertEqual(broken_links(root, paths), [])

    def test_includes_against_the_layer_order_are_reported(self):
        root = Path(__file__).resolve().parent.parent
        local = root / '.local'
        local.mkdir(exist_ok=True)
        sources = {
            'platform/paint.h': '#pragma once\n#include "media/frame.h"\n',
            'platform/paint.cpp': '#include "paint.h"\n#include "platform/paint.h"\n#include "resource.h"\n',
            'media/frame.h': '#pragma once\n\n#include "platform/paint.h"\n#include "capture/grab.h"\n',
            'media/text.cpp': '#include "strings/strings.h"\n',
            'capture/grab.h': '#include "media/frame.h"\n#include "recording/session.h"\n',
            'recording/session.h': '#include "capture/grab.h"\n#include "media/frame.h"\n',
            'scrollshot/stitch.h': '#include "recording/session.h"\n',
            'editor/editor.h': ('#include "ui/controls.h"\n#include "app/app_settings.h"\n'
                                '#include "app/session_state.h"\n'),
            'ui/controls.h': ('#include "editor/editor.h"\n#include "app/trim_workflow.h"\n'
                              '#include "app/trim_flow.h"\n#include "strings/strings.h"\n'),
            'app/app_settings.h': '#include "app/appearance_theme.h"\n#include "media/frame.h"\n',
            'app/appearance_theme.h': '#pragma once\n',
            'app/trim_workflow.h': '#include "app/session_state.h"\n#include "ui/controls.h"\n',
            'app/session_state.h': '#include "ui/controls.h"\n#include "editor/editor.h"\n',
            'app/recording_completion.cpp': '#include "strings/strings.h"\n',
            'app/trim_flow.h': '#include "strings/strings.h"\n',
            'strings/strings.h': '#include "string_ids.h"\n#include "platform/paint.h"\n',
            'tools/helper.h': '#include "platform/paint.h"\n',
            'main.cpp': '#include "app/session_state.h"\n#include "strings/strings.h"\n',
        }
        prefix = 'apps/easnap-desktop/src/'
        with tempfile.TemporaryDirectory(prefix='hygiene-includes-', dir=local) as directory:
            fixture = Path(directory)
            for name, text in sources.items():
                file = fixture / prefix / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(text, encoding='utf-8')
            tracked = [prefix + name for name in sources] + ['docs/readme.md']
            self.assertEqual(include_direction_violations(fixture, tracked), [prefix + line for line in [
                'app/recording_completion.cpp:1: strings/strings.h (takes its captions from the caller)',
                'capture/grab.h:2: recording/session.h (capture must not include recording)',
                'editor/editor.h:3: app/session_state.h (editor must not include app)',
                'media/frame.h:4: capture/grab.h (media must not include capture)',
                'media/text.cpp:1: strings/strings.h (media must not show interface text)',
                'platform/paint.h:2: media/frame.h (platform must not include media)',
                'scrollshot/stitch.h:1: recording/session.h (scrollshot must not include recording)',
                'strings/strings.h:2: platform/paint.h (strings includes nothing of the other layers)',
                'tools/helper.h:1: platform/paint.h (tools is not a known layer)',
                'ui/controls.h:1: editor/editor.h (ui must not include editor)',
                'ui/controls.h:3: app/trim_flow.h (ui must not include app)',
                'app/trim_workflow.h:1: app/session_state.h '
                '(an app value header includes only lower layers or another value header)',
                'app/trim_workflow.h:2: ui/controls.h (an app value header must not include ui or editor)',
            ]])
            # A file that is not in the inventory is neither checked nor a target.
            self.assertEqual(include_direction_violations(fixture, [prefix + 'media/frame.h']), [])

    def test_app_value_headers_are_app_headers(self):
        self.assertTrue(APP_VALUE_HEADERS)
        self.assertTrue(all(header.startswith('app/') and header.endswith('.h')
                            for header in APP_VALUE_HEADERS))

    def test_sources_of_this_repository_follow_the_layer_order(self):
        root = Path(__file__).resolve().parent.parent
        result = subprocess.run(['git', 'ls-files', '-z'], cwd=root, check=False,
                                stdout=subprocess.PIPE)
        if result.returncode != 0:
            self.skipTest('not a Git checkout')
        paths = result.stdout.decode('utf-8').rstrip('\0').split('\0')
        self.assertTrue(any(path.startswith('apps/easnap-desktop/src/ui/') for path in paths))
        self.assertEqual(include_direction_violations(root, paths), [])

    def test_inventory_command_rejects_private_and_unreviewed_files(self):
        root = Path(__file__).resolve().parent.parent
        local = root / '.local'
        local.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hygiene-inventory-', dir=local) as directory:
            inventory = Path(directory) / 'files.txt'
            inventory.write_text('.env\ndocs/images/private.png\n', encoding='utf-8')
            result = subprocess.run([sys.executable, str(root / 'scripts/check_repository_hygiene.py'),
                                     '--files-from', str(inventory)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Private files must remain local:', result.stdout)
            self.assertIn('Unexpected media files:', result.stdout)
            inventory.write_text('apps/easnap-desktop/src/main.cpp\ndocs/images/editor.png\n',
                                 encoding='utf-8')
            result = subprocess.run([sys.executable, str(root / 'scripts/check_repository_hygiene.py'),
                                     '--files-from', str(inventory)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
