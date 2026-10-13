"""Reject private development files, unreviewed media and broken document links."""

import argparse
from pathlib import Path, PurePosixPath
import posixpath
import re
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


MARKDOWN_LINK = re.compile(r'\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)')
HTML_REFERENCE = re.compile(r'\b(?:href|src)="([^"]+)"')
HEADING = re.compile(r'^#{1,6}\s+(.*?)\s*#*\s*$', re.MULTILINE)
FENCED_BLOCK = re.compile(r'^(```|~~~).*?^\1[^\n]*$', re.MULTILINE | re.DOTALL)
INLINE_CODE = re.compile(r'`[^`\n]*`')


def heading_anchors(text):
    """The anchors GitHub generates for the headings of a Markdown text."""
    anchors, seen = set(), {}
    for title in HEADING.findall(FENCED_BLOCK.sub('', text)):
        title = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', title)  # a link keeps its text
        slug = re.sub(r'[^\w\- ]', '', title.replace('`', '').replace('*', '').lower())
        slug = slug.replace(' ', '-')
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        anchors.add(slug if count == 0 else f'{slug}-{count}')
    return anchors


def broken_links(root, paths):
    """Relative links in tracked Markdown files that lead nowhere.

    A target must be a file or directory of the checked inventory, so a link to
    something that exists only on the author's disk is reported too. An anchor
    must be a heading of its target.
    """
    tracked = {path.replace('\\', '/') for path in paths if path}
    directories = {str(parent) for path in tracked for parent in PurePosixPath(path).parents}
    texts = {}

    def read(path):
        if path not in texts:
            file = root / path
            texts[path] = file.read_text(encoding='utf-8-sig') if file.is_file() else None
        return texts[path]

    broken = []
    for path in sorted(tracked):
        if not path.lower().endswith('.md'):
            continue
        text = read(path)
        if text is None:
            continue
        prose = INLINE_CODE.sub('', FENCED_BLOCK.sub('', text))
        for target in MARKDOWN_LINK.findall(prose) + HTML_REFERENCE.findall(prose):
            if re.match(r'[a-zA-Z][a-zA-Z0-9+.\-]*:', target) or target.startswith('//'):
                continue  # Another site, a mail address.
            file_part, _, anchor = target.partition('#')
            if file_part:
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(path), file_part))
                if resolved.startswith('..') or (resolved not in tracked and resolved not in directories):
                    broken.append(f'{path}: {target}')
                    continue
            else:
                resolved = path
            if anchor and resolved.lower().endswith('.md'):
                destination = read(resolved)
                if destination is not None and anchor.lower() not in heading_anchors(destination):
                    broken.append(f'{path}: {target} (no such heading)')
    return broken


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
    links = broken_links(root, paths)
    if private:
        print('Private files must remain local:', *private, sep='\n')
    if offenders:
        print('Unexpected media files:', *offenders, sep='\n')
    if links:
        print('Broken relative links:', *links, sep='\n')
    if private or offenders or links:
        return 1
    print('No private files, stray media or broken links in the checked inventory.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
