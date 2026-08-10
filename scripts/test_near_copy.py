import re

def norm(s):
    return re.sub(r'\s+', ' ', re.sub(r'[^A-Za-z0-9]+', ' ', s.lower())).strip()

def strip_opener(s):
    for op in ['user is ', 'they are ', 'the user is ', 'currently ', 'right now ']:
        if s.startswith(op):
            return s[len(op):]
    return s

def is_near_copy(line, existing):
    nl = norm(line)
    if not nl:
        return False
    nlo = strip_opener(nl)
    for e in existing:
        ne = norm(e)
        if not ne:
            continue
        if nl == ne:
            return True
        if nlo == ne:
            return True
    return False

cases = [
    ('User is reading about Indexer Reliability in Outlook and M365 Copilot.',
     ['Reading about Indexer Reliability (in Outlook & M365 Copilot)']),
    ('User is exploring the indexer rollout across 5 browser tabs.',
     ['Exploring Indexer Rollout (across 5 browser tabs)']),
    ('User is working on the auth refactor in their browser and editing the related source file in Visual Studio.',
     ['Working on auth refactor (across Visual Studio & Edge)']),
    ('User is checking email in Outlook.',
     ['Checking email in Outlook']),
    ('User is working on Indexer Reliability across Word, Excel, and PowerPoint.',
     ['Working on Indexer Reliability (across Word, Excel, PowerPoint)']),
    # Regression: pure copy should still be rejected
    ('Reading about Indexer Reliability (in Outlook & M365 Copilot)',
     ['Reading about Indexer Reliability (in Outlook & M365 Copilot)']),
    # Regression: opener-only filler should be rejected (no other change)
    ('User is reading about Indexer Reliability in Outlook M365 Copilot',
     ['Reading about Indexer Reliability in Outlook M365 Copilot']),
]
for line, ex in cases:
    res = is_near_copy(line, ex)
    tag = 'REJECT' if res else 'KEEP'
    print(f'{tag:7}  {line[:80]!r}')
