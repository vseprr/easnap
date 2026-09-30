"""Reject private development files and unreviewed media from publication."""

import argparse
from pathlib import Path, PurePosixPath
import subprocess
import sys


BLOCKED_SUFFIXES = {'.mp4', '.gif', '.webm', '.jpg', '.jpeg', '.png', '.dmp', '.log'}
PRIVATE_SUFFIXES = {'.pem', '.key', '.pfx', '.p12', '.p7b', '.p7c', '.cer', '.crt'}
PRIVATE_ROOTS = {'.agents', '.serena', 'docs/ui-references', 'docs/evidence'}
PRIVATE_ROOT_FILES = {'agents.md', 'easnap-ui-modernization-agent-prompt.en.md'}
PRIVATE_NAMES = {'.npmrc', '.netrc', '_netrc', '.git-credentials', 'credentials.json',
                 'secrets.json', 'id_rsa', 'id_dsa', 'id_ecdsa', 'id_ed25519'}


def private_files(paths):
    rejected = []
    for path in paths:
        normalized = PurePosixPath(path.replace('\\', '/'))
        parts = tuple(part.lower() for part in normalized.parts)
        if not parts:
            continue
        name = parts[-1]
        relative = '/'.join(parts)
        local_config = any(name.endswith('.local' + suffix)
                           for suffix in ('.json', '.yml', '.yaml', '.toml'))
        if (relative in PRIVATE_ROOT_FILES
                or any(relative == root or relative.startswith(root + '/')
                       for root in PRIVATE_ROOTS)
                or any(part in {'.aws', '.azure', '.ssh'} for part in parts)
                or name in PRIVATE_NAMES or name == '.env' or name.startswith('.env.')
                or name.startswith(('credentials.local.', 'secrets.local.'))
                or local_config or normalized.suffix.lower() in PRIVATE_SUFFIXES):
            rejected.append(path)
    return sorted(rejected)



def unexpected_media(paths, allowed):
    return sorted(path for path in paths
                  if PurePosixPath(path).suffix.lower() in BLOCKED_SUFFIXES
                  and path not in allowed)


def read_allowlist(root):
    return {line for line in (root / 'scripts/repository-media-allowlist.txt')
            .read_text(encoding='utf-8').splitlines()
            if line and not line.startswith('#')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files-from', type=Path,
                        help='check a UTF-8, newline-separated inventory instead of Git')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.files_from:
        paths = args.files_from.read_text(encoding='utf-8').splitlines()
    else:
        result = subprocess.run(['git', 'ls-files', '-z'], cwd=root,
                                check=True, stdout=subprocess.PIPE)
        paths = result.stdout.decode('utf-8').rstrip('\0').split('\0')
    offenders = unexpected_media(paths, read_allowlist(root))
    private = private_files(paths)
    if private:
        print('Private files must remain local:', *private, sep='\n')
    if offenders:
        print('Unexpected media files:', *offenders, sep='\n')
    if private or offenders:
        return 1
    print('No private files or stray media in the checked inventory.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
