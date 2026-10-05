"""Portable readable session filenames and safe download headers."""
import re
import unicodedata
from urllib.parse import quote

MAX_STEM_BYTES = 160
DEVICE_NAME = re.compile(r'(?i)^(?:con|prn|aux|nul|com[1-9\u00b9\u00b2\u00b3]|lpt[1-9\u00b9\u00b2\u00b3])(?:\.|$)')
INVALID = re.compile(r'[<>:"/\\|?*]+')


def title_stem(title):
    value = unicodedata.normalize('NFC', str(title))
    value = ''.join('-' if unicodedata.category(character) in ('Cc', 'Cf', 'Cs') else character for character in value)
    value = INVALID.sub('-', value).strip(' .')
    while value.casefold().endswith('.lattice'):
        value = value[:-8].rstrip(' .')
    value = value or 'Session'
    if DEVICE_NAME.match(value):
        value = '_' + value
    raw = value.encode('utf-8')[:MAX_STEM_BYTES]
    value = raw.decode('utf-8', errors='ignore').rstrip(' .') or 'Session'
    return value


def session_filename(title, index=1):
    if type(index) is not int or not 1 <= index <= 10000:
        raise ValueError('Session filename collision limit exceeded')
    suffix = '' if index == 1 else f' ({index})'
    return title_stem(title) + suffix + '.lattice'


def filename_key(name):
    return unicodedata.normalize('NFC', name).casefold()


def download_disposition(title):
    filename = session_filename(title)
    ascii_title = unicodedata.normalize('NFKD', title_stem(title)).encode('ascii', errors='ignore').decode('ascii')
    fallback = session_filename(ascii_title or 'Session').replace('"', '_').replace('\\', '_')
    return 'attachment; filename="' + fallback + '"; filename*=UTF-8\'\'' + quote(filename, safe='')
