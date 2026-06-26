#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CJK = re.compile(r'[\u3400-\u9fff]')
LINK = re.compile(r'\[[^\]]+\]\(([^)]+)\)')
TASK = re.compile(r'\*\*([A-Z]+-M\d+-\d{3})\*\*')

PUBLIC_REQUIRED = [
    'README.md', 'ARCHITECTURE.md', 'ROADMAP.md', 'TODO.md',
    'CONTRIBUTING.md', 'SECURITY.md', 'CHANGELOG.md', '.gitignore',
    'xmake.lua', 'voris-package.toml',
    'docs/API.md', 'docs/BUILDING.md', 'docs/DEPENDENCIES.md',
    'docs/TESTING.md', 'docs/REPOSITORY_ISOLATION.md',
    'docs/RELEASES.md', 'docs/LICENSING.md', 'docs/REFERENCES.md'
]

AGENT_MARKDOWN = [
    'AGENTS.md',
    '.agent/README.md',
    '.agent/UPSTREAM_SYNC.md',
    '.agent/UPSTREAM_REQUEST_TEMPLATE.md',
    '.agent/TASK_REPORT_TEMPLATE.md',
]

errors: list[str] = []
for rel in PUBLIC_REQUIRED:
    if not (ROOT / rel).exists():
        errors.append(f'missing required file: {rel}')

ignore_text = (ROOT / '.gitignore').read_text(encoding='utf-8')
for entry in ('AGENTS.md', '.agent/'):
    if entry not in ignore_text:
        errors.append(f'.gitignore does not ignore {entry}')

public_markdown = [p for p in ROOT.rglob('*.md') if p.name != 'AGENTS.md' and '.agent' not in p.parts]
for path in public_markdown:
    text = path.read_text(encoding='utf-8')
    if CJK.search(text):
        errors.append(f'public Markdown contains CJK text: {path.relative_to(ROOT)}')

agent_markdown = [ROOT / rel for rel in AGENT_MARKDOWN if (ROOT / rel).exists()]
agent_dir = ROOT / '.agent'
if agent_dir.exists():
    known_agent_markdown = {path.resolve() for path in agent_markdown}
    for path in agent_dir.rglob('*.md'):
        if path.resolve() not in known_agent_markdown:
            agent_markdown.append(path)

for path in agent_markdown:
    if not CJK.search(path.read_text(encoding='utf-8')):
        errors.append(f'Agent Markdown is expected to contain Chinese text: {path.relative_to(ROOT)}')

ids = TASK.findall((ROOT / 'TODO.md').read_text(encoding='utf-8'))
if not ids:
    errors.append('TODO.md contains no task identifiers')
if len(ids) != len(set(ids)):
    errors.append('TODO.md contains duplicate task identifiers')

for path in public_markdown:
    text = path.read_text(encoding='utf-8')
    for target in LINK.findall(text):
        if '://' in target or target.startswith('#') or target.startswith('mailto:'):
            continue
        clean = target.split('#', 1)[0]
        if not clean:
            continue
        resolved = (path.parent / clean).resolve()
        try:
            resolved.relative_to(ROOT.resolve())
        except ValueError:
            errors.append(f'cross-repository relative link in {path.relative_to(ROOT)}: {target}')
            continue
        if not resolved.exists():
            errors.append(f'broken link in {path.relative_to(ROOT)}: {target}')

for path in ROOT.rglob('*'):
    if path.is_file() and path.suffix in {'.md', '.lua', '.toml', '.hpp', '.cpp'}:
        text = path.read_text(encoding='utf-8', errors='replace')
        if re.search(r'\.\./(?:VMem|VIO|VNet|VQuic|VHTTP|VCache|VDB)(?:/|\\)', text):
            errors.append(f'cross-repository source path found: {path.relative_to(ROOT)}')

if errors:
    print('Repository validation failed:')
    for error in errors:
        print(f'  - {error}')
    sys.exit(1)

print('Repository validation passed.')
print(f'TODO task identifiers: {len(ids)}')
print(f'Public Markdown files: {len(public_markdown)}')
print(f'Agent Markdown files: {len([p for p in agent_markdown if p.exists()])}')
