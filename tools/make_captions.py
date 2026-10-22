"""Makes a caption file for Half-Life's voice recordings with whisper.cpp (speech recognition).

    python tools/make_captions.py [--hl <Half-Life folder>] [--model <ggml model>] [--whisper <whisper-cli.exe>]

Reads the scientists', guards', G-Man's, the tram's, the training hologram's, the Nihilanth's and the
soldiers' longer recordings from the installed game (never writes there), and writes
bin/userdata/valve/captions/english_whisper.txt: one "Tokens" line per recording, keyed by its path,
in the speaker's colour. newen captions a sentence from its recordings' lines, so this covers the
dialogue that can't be captioned from words. The result is Valve's dialogue: it stays on your own
machine and doesn't go into the project.

Needs ffmpeg on PATH (to give whisper 16 kHz audio) and whisper.cpp with a model, by default
bin/whisper/bin/Release/whisper-cli.exe and bin/whisper/ggml-medium.en.bin. Finished recordings are
remembered in whisper_cache.json next to the output, so a second run only does new or failed ones
(--fresh forgets them). Every line is worth a read: names get misheard and screams turn into words.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave
from fnmatch import fnmatch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# sound folder -> (colour, which files are speech)
VOICES = {
	'scientist': ('150,200,255', ['*.wav']),
	'barney': ('130,170,255', ['*.wav']),
	'gman': ('200,170,255', ['*.wav']),
	'tride': ('230,230,170', ['*.wav']),
	'holo': ('120,220,220', ['*.wav']),
	'nihilanth': ('255,130,130', ['*.wav']),
	# the soldiers' radio words are captioned from the words already; only their whole recordings here
	'hgrunt': ('200,220,140', ['c?a*_*.wav', 'hg_*.wav']),
}

# recordings that aren't meant to be understood get a fixed caption instead
FIXED = {
	'gman/gman_mumble*': '[Mumbling]',
}

# names whisper has to guess at; a prompt makes them come out spelled right more often
PROMPT = 'Dr. Freeman, Gordon, Black Mesa, the Lambda Complex, Xen, the HEV suit, the test chamber.'
BATCH = 40   # files per whisper run: the model loads once per run


def find_half_life():
	cached = ROOT / 'bin' / 'basedir.txt'
	if cached.exists():
		path = Path(cached.read_text(encoding='utf-8', errors='replace').strip())
		if (path / 'valve').is_dir():
			return path
	default = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Half-Life')
	return default if (default / 'valve').is_dir() else None


def clean(text):
	text = ' '.join(text.split())
	for a, b in (('\u2019', "'"), ('\u2018', "'"), ('\u201c', "'"), ('\u201d', "'"), ('"', "'"),
			('\u2014', ' - '), ('\u2013', '-'), ('\u2026', '...')):
		text = text.replace(a, b)
	text = text.encode('ascii', 'ignore').decode()   # newen's caption font is ASCII only
	# whisper marks what isn't speech in brackets: [BLANK_AUDIO], (screaming), *gasps*
	text = re.sub(r'\[[^\]]*\]|\([^)]*\)|\*[^*]*\*', '', text)
	text = re.sub(r'(?P<c>\w)(?P=c){4,}', lambda m: m.group('c') * 4, text)   # a scream is AHHHH, not 60 H's
	text = re.sub(r'(?P<c>[!?])(?P=c)+', lambda m: m.group('c'), text)
	return ' '.join(text.split()).lstrip(' -.,/;:').rstrip(' -')


def duration(wav):
	try:
		with wave.open(str(wav), 'rb') as w:
			return w.getnframes() / w.getframerate()
	except (wave.Error, EOFError, OSError):
		return 0


LONG = 110   # characters: a part longer than this is split into sentences


def pace(parts, total):
	"""whisper sometimes hands back a whole speech as one part: split those into sentences, each
	starting in proportion to where its words fall in the part"""
	out = []
	for i, (start, text) in enumerate(parts):
		end = parts[i + 1][0] if i + 1 < len(parts) else max(total, start)
		sentences = re.split(r'(?<=[.!?])\s+', text) if len(text) > LONG else [text]
		length = sum(len(x) for x in sentences) or 1
		at = 0
		for sentence in sentences:
			out.append((round(start + (end - start) * at / length, 1), sentence))
			at += len(sentence)
	return out


def read_srt(path):
	"""whisper's subtitle output -> [[start seconds, text]]"""
	segments = []
	for block in re.split(r'\n\s*\n', path.read_text(encoding='utf-8', errors='replace')):
		rows = block.strip().splitlines()
		times = re.match(r'(\d+):(\d+):(\d+)[,.](\d+)', rows[1]) if len(rows) >= 3 else None
		if times:
			h, m, sec, ms = (int(x) for x in times.groups())
			segments.append([round(h * 3600 + m * 60 + sec + ms / 1000, 1), ' '.join(rows[2:])])
	return segments


def transcribe(files, whisper, model, threads, work):
	"""files: [(key, wav path)] -> {key: [[start seconds, text]]}"""
	staged = []
	for i, (key, src) in enumerate(files):
		dst = work / f'{i:04d}.wav'
		subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', str(src), '-ar', '16000', '-ac', '1',
			'-c:a', 'pcm_s16le', str(dst)], check=True)
		staged.append((key, dst))
	cmd = [str(whisper), '-m', str(model), '-l', 'en', '-t', str(threads), '-np', '-osrt',
		'--prompt', PROMPT] + [str(p) for _, p in staged]
	subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	out = {}
	for key, wav in staged:
		srt = Path(str(wav) + '.srt')
		if srt.exists():
			out[key] = read_srt(srt)
			srt.unlink()
		wav.unlink()
	return out


def main():
	ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	ap.add_argument('--hl', type=Path, help='Half-Life folder (default: the one newen found)')
	ap.add_argument('--whisper', type=Path, default=ROOT / 'bin/whisper/bin/Release/whisper-cli.exe')
	ap.add_argument('--model', type=Path, default=ROOT / 'bin/whisper/ggml-medium.en.bin')
	ap.add_argument('--out', type=Path, default=ROOT / 'bin/userdata/valve/captions/english_whisper.txt')
	ap.add_argument('--threads', type=int, default=max(1, (os.cpu_count() or 4) - 1))
	ap.add_argument('--only', help='only recordings whose path matches this pattern, e.g. "scientist/*"')
	ap.add_argument('--fresh', action='store_true', help='ignore the cache and transcribe everything again')
	args = ap.parse_args()

	hl = args.hl or find_half_life()
	if not hl or not (hl / 'valve' / 'sound').is_dir():
		sys.exit('Half-Life not found; pass --hl <folder>')
	for tool, what in ((args.whisper, 'whisper-cli'), (args.model, 'the whisper model')):
		if not tool.exists():
			sys.exit(f'{what} not found at {tool}')
	if not shutil.which('ffmpeg'):
		sys.exit('ffmpeg not found on PATH')

	sound = hl / 'valve' / 'sound'
	files = []
	for folder, (_, patterns) in VOICES.items():
		for wav in sorted((sound / folder).glob('*.wav')):
			key = f'{folder}/{wav.name.lower()}'
			if any(fnmatch(wav.name.lower(), p) for p in patterns) and (not args.only or fnmatch(key, args.only)):
				files.append((key, wav))

	args.out.parent.mkdir(parents=True, exist_ok=True)
	cache_path = args.out.parent / 'whisper_cache.json'
	cache = {} if args.fresh or not cache_path.exists() else json.loads(cache_path.read_text(encoding='utf-8'))
	# an older cache held plain text; those are done again with timing
	todo = [(k, p) for k, p in files if not isinstance(cache.get(k), list)
		and not any(fnmatch(k, pat) for pat in FIXED)]
	print(f'{len(files)} recordings, {len(todo)} to transcribe with {args.model.name}')
	with tempfile.TemporaryDirectory(prefix='newen_whisper_') as tmp:
		for start in range(0, len(todo), BATCH):
			cache.update(transcribe(todo[start:start + BATCH], args.whisper, args.model, args.threads, Path(tmp)))
			cache_path.write_text(json.dumps(cache, indent=1, sort_keys=True), encoding='utf-8')
			print(f'  {min(start + BATCH, len(todo))}/{len(todo)}', flush=True)

	lines, empty = [], 0
	for key, wav in files:
		fixed = next((t for pat, t in FIXED.items() if fnmatch(key, pat)), None)
		# a long recording keeps whisper's timing: each part shows when it's said (<at:seconds>)
		parts = [] if fixed else [(start, clean(t)) for start, t in cache.get(key, [])]
		parts = pace([(start, t) for start, t in parts if t], duration(wav))
		text = fixed or ''.join((f'<at:{start:g}>' if i else '') + t + ' ' for i, (start, t) in enumerate(parts)).strip()
		if not text:
			empty += 1   # a scream, a cough, or nothing whisper could make out
			continue
		colour = VOICES[key.split('/')[0]][0]
		lines.append(f'\t\t"{key}"\t"<clr:{colour}>{text}"')
	args.out.write_text('// Made by tools/make_captions.py with whisper.cpp from your installed Half-Life.\n'
		'// Machine transcription: check the lines and fix them here (or in another captions/english_*.txt).\n'
		'// This is Valve\'s dialogue; keep it to yourself.\n'
		'"lang"\n{\n\t"Tokens"\n\t{\n' + '\n'.join(lines) + '\n\t}\n}\n', encoding='utf-8')
	print(f'Wrote {args.out}: {len(lines)} lines ({empty} recordings without speech)')


if __name__ == '__main__':
	main()
