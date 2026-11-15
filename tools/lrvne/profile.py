# SPDX-License-Identifier: GPL-3.0-or-later
"""The game profile text (include/lrvne/profile.h): a pack's "profile" entry.

A profile is UTF-8 text, one `key=value` per line; blank lines and lines starting with '#'
are ignored. `dumps` writes one from a dict and refuses whatever the C parser would not read
back as the same value:

    text = profile.dumps({'id': 'mygame', 'title': 'My Game', 'lang': 'ja',
                          'width': 640, 'mouse': True, 'palette': [0, 7, 15]})
    writer.add('profile', xpak.T_META, text.encode('utf-8'))

Values: bool -> yes/no, int -> decimal, a list or tuple of ints -> comma separated, str as
it is. `loads` returns the raw strings; `parse_int`, `parse_bool` and `parse_ints` read them
as the C parser does.
"""
from typing import Dict, List, Mapping, Sequence, Union

Value = Union[str, bool, int, Sequence[int]]

KEY_MAX = 47                    # bytes; the C parser reports longer keys truncated
INT_MAX = 0x7FFFFFFF            # what an `int` field takes
_DEC_MAX = 0x7FFFFFF * 10 + 9   # the largest decimal number the C parser accepts


class ProfileError(Exception):
    """A key or a value cannot be written to, or read from, a profile."""


def _check_key(key: str) -> str:
    if not isinstance(key, str) or not key:
        raise ProfileError('profile key %r: not a non-empty string' % (key,))
    if key != key.strip(' \t\r') or key.startswith('#'):
        raise ProfileError('profile key %r: must not start with # or have spaces around it' % key)
    if any(c in key for c in '=\n\r\0'):
        raise ProfileError('profile key %r: contains =, a line break or NUL' % key)
    if len(key.encode('utf-8')) > KEY_MAX:
        raise ProfileError('profile key %r: longer than %d bytes' % (key, KEY_MAX))
    return key


def _int(key: str, value: int) -> str:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProfileError('profile key %s: %r is not an integer' % (key, value))
    if not -INT_MAX <= value <= INT_MAX:
        raise ProfileError('profile key %s: %d does not fit an int' % (key, value))
    if abs(value) > _DEC_MAX:   # only hexadecimal reaches the top of the range
        return '%s0x%X' % ('-' if value < 0 else '', abs(value))
    return str(value)


def format_value(key: str, value: Value) -> str:
    """The text of one value; ProfileError when the C parser could not read it back."""
    if isinstance(value, bool):
        return 'yes' if value else 'no'
    if isinstance(value, int):
        return _int(key, value)
    if isinstance(value, str):
        if any(c in value for c in '\n\r\0'):
            raise ProfileError('profile key %s: the value contains a line break or NUL' % key)
        if value != value.strip(' \t'):
            raise ProfileError('profile key %s: spaces around the value %r would be lost'
                               % (key, value))
        return value
    if isinstance(value, (list, tuple)):
        return ','.join(_int(key, v) for v in value)
    raise ProfileError('profile key %s: values of type %s cannot be written'
                       % (key, type(value).__name__))


def dumps(values: Mapping[str, Value]) -> str:
    """The profile text of a dict, the keys in the dict's order."""
    lines = []
    for key, value in values.items():
        lines.append('%s=%s\n' % (_check_key(key), format_value(key, value)))
    return ''.join(lines)


def loads(text: Union[str, bytes]) -> Dict[str, str]:
    """{key: value text} of a profile, read as the C parser reads it: keys and values trimmed
    of spaces and tabs, a later line replacing an earlier one with the same key. ProfileError
    for a line without '='."""
    if isinstance(text, bytes):
        text = text.decode('utf-8')
    out: Dict[str, str] = {}
    for no, line in enumerate(text.split('\n'), 1):
        key, eq, value = line.partition('=')
        key = key.strip(' \t\r')
        if (not key and not value) or key.startswith('#'):
            continue
        if not eq:
            raise ProfileError('profile line %d (%s): no \'=\'' % (no, key))
        out[key] = value.strip(' \t\r')
    return out


def parse_int(text: str) -> int:
    """A number as the C parser reads it: decimal, or hexadecimal with 0x; an optional sign."""
    s = text.strip(' \t')
    sign = -1 if s[:1] == '-' else 1
    digits = s[1:] if s[:1] in ('-', '+') else s
    base = 16 if digits[:2].lower() == '0x' and len(digits) > 2 else 10
    body = digits[2:] if base == 16 else digits
    allowed = '0123456789abcdef'[:base]
    if not body or any(c not in allowed for c in body.lower()):
        raise ProfileError('%r is not a number' % text)
    value = int(body, base)
    if value // base > 0x7FFFFFF:
        raise ProfileError('%r is too large' % text)
    return sign * value


def parse_bool(text: str) -> bool:
    """A truth value: 1/yes/true/on or 0/no/false/off, any case."""
    word = text.strip(' \t').lower()
    if word in ('1', 'yes', 'true', 'on'):
        return True
    if word in ('0', 'no', 'false', 'off'):
        return False
    raise ProfileError('%r is not yes or no' % text)


def parse_ints(text: str) -> List[int]:
    """A comma-separated list of numbers; an empty value is an empty list."""
    if not text.strip(' \t'):
        return []
    return [parse_int(part) for part in text.split(',')]
